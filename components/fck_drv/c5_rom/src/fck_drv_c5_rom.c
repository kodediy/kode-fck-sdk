/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file fck_drv_c5_rom.c
 * @brief The C5's flash written, read or asked about through its ROM loader.
 *        See fck_drv_c5_rom.h.
 *
 * esp-serial-flasher talks to the loader through a port it is handed
 * (esp_loader_port_ops_t). This one is the owner's UART and lines: nothing
 * here installs a driver or configures a pin of its own, and EN and BOOT go
 * through fck_drv_pins, which never drives them high.
 *
 * Four traps, the first three met before by somebody else:
 * - A deflated write: the ROM wants the size it erases in whole blocks
 *   (esptool's flash_defl_begin rounds it up); the MD5 still covers the
 *   region and nothing more.
 * - A raw write: esptool sends the last block padded to a whole one with
 *   0xFF, and so does this. Blocks never cross a sector, so the padding
 *   lands in the region's own erased sector and changes nothing there.
 * - esp-serial-flasher 2.1.0 says flash encryption is on when the key
 *   purposes have an odd number of bits set, not when SPI_BOOT_CRYPT_CNT has
 *   (esptool's test, and the eFuse's own meaning). So its security info is
 *   not used: the two eFuses are read here.
 * - A deflated block is answered once the stub has inflated and written all
 *   of it, and the library waits 1 s for that whatever the block holds:
 *   esptool gives each block the time its inflated size needs. 833 bytes of
 *   0xFF that inflate to 816 KB took longer, and the library sent the block
 *   again (a KDFCK001 rev 1.0). So while a deflated region is
 *   written no timer runs shorter than esptool's for the most one of its
 *   blocks could inflate to (defl_block_ms()). Only a block whose answer
 *   never comes waits that long: 42 s at most, three times.
 */
#include "fck_drv_c5_rom.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_rom_md5.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "efuse/esp32c5/esp_loader_efuse_table.h"
#include "esp_loader.h"
#include "esp_loader_efuse.h"
#include "esp_loader_io.h"
#include "esp_loader_stubs.h"

#include "fck_bsp.h"
#include "fck_drv_uart.h"

/* BOOT stays low this long after EN is let go. EN rises through R502 and
 * C503, about 10 ms to the C5's threshold, and the ROM reads BOOT as it
 * does. esp-serial-flasher's own port gives 50 ms after driving EN high. */
#define BOOT_HOLD_MS     100U
/* One esp_loader_connect() resets once and tries to sync ten times
 * (ESP_LOADER_CONNECT_DEFAULT), about 2 s; if none answers, one more reset. */
#define CONNECT_TRIES    2U
/* After a speed change, what the ROM sent while it moved is dropped (esptool
 * waits 50 ms and flushes too). */
#define BAUD_SETTLE_MS   50U
/* The flash esp-serial-flasher takes the C5 to have when its ID names no size
 * it knows (DEFAULT_FLASH_SIZE in its esp_loader.c). */
#define FLASH_GUESS      (2U * 1024U * 1024U)
/* fck_drv_c5_rom_listen() reads the UART this long at a time, at most. */
#define LISTEN_POLL_MS   50U
/* A deflated block's time, esptool's: 40 s for every MB it inflates to, 3 s
 * at least. Not knowing what it inflates to, the most it could: the region,
 * or deflate's limit of 1032 times its own length. */
#define DEFL_MS_PER_MB   40000U
#define DEFL_MIN_MS      3000U
#define DEFL_MAX_RATIO   1032U

/* ---- The port: the owner's UART and lines, lent to esp-serial-flasher ---- */

typedef struct {
    esp_loader_port_t    base;
    const fck_drv_pins_t      *hw;
    int                  uart;
    int64_t              end_us;      /* start_timer()'s deadline */
    fck_drv_c5_rom_report_t *rep;         /* where the library's words go */
    int                  note_level;  /* of the words kept there, 0 for none */
    bool                 behind;      /* the loader changed speed, the UART did not */
    uint32_t             floor_ms;    /* no timer shorter, while a deflated region is written */
} rom_port_t;

static rom_port_t *port_of(esp_loader_port_t *port)
{
    return container_of(port, rom_port_t, base);
}

/* At least @p ms: pdMS_TO_TICKS() rounds down, to nothing below a tick. */
static TickType_t ticks_for(uint32_t ms)
{
    return (TickType_t)((ms + portTICK_PERIOD_MS - 1U) / portTICK_PERIOD_MS);
}

static void port_enter_bootloader(esp_loader_port_t *port)
{
    rom_port_t *p = port_of(port);
    fck_drv_pins_c5_reset(p->hw, true, false);
    vTaskDelay(ticks_for(BOOT_HOLD_MS));
    fck_drv_pins_c5_release_boot(p->hw);
    /* The ROM's banner, and whatever the C5 said before its reset. */
    fck_drv_uart_flush(p->uart);
}

static void port_reset_target(esp_loader_port_t *port)
{
    /* BOOT let go through the reset: the C5 boots its flash. */
    fck_drv_pins_c5_reset(port_of(port)->hw, false, false);
}

static void port_start_timer(esp_loader_port_t *port, uint32_t ms)
{
    rom_port_t *const p    = port_of(port);
    uint32_t const    wait = (ms > p->floor_ms) ? ms : p->floor_ms;
    p->end_us              = esp_timer_get_time() + (int64_t)wait * 1000;
}

static uint32_t port_remaining_time(esp_loader_port_t *port)
{
    int64_t const left = port_of(port)->end_us - esp_timer_get_time();
    return (left > 0) ? (uint32_t)((left + 999) / 1000) : 0U;
}

static void port_delay_ms(esp_loader_port_t *port, uint32_t ms)
{
    (void)port;
    vTaskDelay(ticks_for(ms));
}

/* The library's own words (its build keeps WARN and up,
 * CONFIG_SERIAL_FLASHER_LOG_LEVEL), into the report for the caller to log:
 * the worst of them, and of those the last. A warning never covers an error. */
static void port_log(esp_loader_port_t *port, esp_loader_log_level_t level,
                     const char *fmt, va_list args)
{
    rom_port_t *p = port_of(port);
    if (p->note_level != 0 && (int)level > p->note_level) {
        return;
    }
    p->note_level = (int)level;
    (void)vsnprintf(p->rep->note, sizeof(p->rep->note), fmt, args);
}

/* Called once the loader has taken @p rate. If the UART cannot follow, the
 * two no longer hear each other: said in the report, as an error. */
static esp_loader_error_t port_change_rate(esp_loader_port_t *port, uint32_t rate)
{
    rom_port_t *p = port_of(port);
    if (fck_drv_uart_baud(p->uart, rate) == KODE_OK) {
        return ESP_LOADER_SUCCESS;
    }
    p->behind     = true;
    p->note_level = (int)ESP_LOADER_LOG_LEVEL_ERROR;
    (void)snprintf(p->rep->note, sizeof(p->rep->note),
                   "The C5 went to %" PRIu32 " baud and the UART could not", rate);
    return ESP_LOADER_ERROR_FAIL;
}

/* fck_drv_uart_write() returns once the bytes are in the UART's FIFO: the
 * loader's answer, read next, has a timeout of its own. */
static esp_loader_error_t port_write(esp_loader_port_t *port, const uint8_t *data,
                                     uint16_t size, uint32_t timeout)
{
    (void)timeout;
    return (fck_drv_uart_write(port_of(port)->uart, data, size) == KODE_OK)
               ? ESP_LOADER_SUCCESS : ESP_LOADER_ERROR_FAIL;
}

/* All @p size bytes within @p timeout ms, or ESP_LOADER_ERROR_TIMEOUT. The
 * library reads its answers a byte at a time (slip.c), so one wait is the
 * whole read; the wait is rounded up to whole ticks, where the UART's own
 * rounding would cut it short. */
static esp_loader_error_t port_read(esp_loader_port_t *port, uint8_t *data,
                                    uint16_t size, uint32_t timeout)
{
    uint32_t const ms  = (uint32_t)ticks_for(timeout) * portTICK_PERIOD_MS;
    size_t const   got = fck_drv_uart_read_wait(port_of(port)->uart, data, size, ms);
    return (got == size) ? ESP_LOADER_SUCCESS : ESP_LOADER_ERROR_TIMEOUT;
}

static const esp_loader_port_ops_t s_ops = {
    .enter_bootloader         = port_enter_bootloader,
    .reset_target             = port_reset_target,
    .start_timer              = port_start_timer,
    .remaining_time           = port_remaining_time,
    .delay_ms                 = port_delay_ms,
    .log                      = port_log,
    .change_transmission_rate = port_change_rate,
    .write                    = port_write,
    .read                     = port_read,
};

/* ---- The regions --------------------------------------------------------- */

static bool hex_digit(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

/* @p md5 as the ROM writes it, in lower case: the library compares bytes. */
static bool md5_lower(const char *md5, char out[FCK_DRV_C5_ROM_MD5_LENGTH + 1U])
{
    for (size_t i = 0U; i < FCK_DRV_C5_ROM_MD5_LENGTH; i++) {
        char const c = md5[i];
        if (!hex_digit(c)) {
            return false;   /* a '\0' too: the digest is short */
        }
        out[i] = (c >= 'A' && c <= 'F') ? (char)(c - 'A' + 'a') : c;
    }
    out[FCK_DRV_C5_ROM_MD5_LENGTH] = '\0';
    return md5[FCK_DRV_C5_ROM_MD5_LENGTH] == '\0';
}

static bool part_ok(const fck_drv_c5_rom_part_t *p)
{
    char md5[FCK_DRV_C5_ROM_MD5_LENGTH + 1U];

    if (p->data == NULL || p->size == 0U || (p->size % 4U) != 0U
        || (p->addr % FCK_DRV_C5_ROM_SECTOR) != 0U || p->addr >= FCK_DRV_C5_ROM_REACH
        || p->size > FCK_DRV_C5_ROM_REACH - p->addr || p->zsize > FCK_DRV_C5_ROM_REACH) {
        return false;
    }
    if (p->md5 != NULL && !md5_lower(p->md5, md5)) {
        return false;
    }
    return p->zsize == 0U || p->md5 != NULL;
}

/* With every start on a sector, two regions share a sector only if they
 * share a byte: one's erase would take the other's data. */
static bool overlap(const fck_drv_c5_rom_part_t *a, const fck_drv_c5_rom_part_t *b)
{
    return a->addr < b->addr + b->size && b->addr < a->addr + a->size;
}

bool fck_drv_c5_rom_parts_ok(const fck_drv_c5_rom_part_t *parts, size_t n)
{
    if (parts == NULL || n == 0U || n > FCK_DRV_C5_ROM_PARTS_MAX) {
        return false;
    }
    for (size_t i = 0U; i < n; i++) {
        if (!part_ok(&parts[i])) {
            return false;
        }
        for (size_t j = 0U; j < i; j++) {
            if (overlap(&parts[i], &parts[j])) {
                return false;
            }
        }
    }
    return true;
}

/* @p c's digest as the ROM writes one: 32 hex digits, lower case. */
static void md5_hex(md5_context_t *c, char out[FCK_DRV_C5_ROM_MD5_LENGTH + 1U])
{
    static const char hex[] = "0123456789abcdef";
    uint8_t           d[ESP_ROM_MD5_DIGEST_LEN];

    esp_rom_md5_final(d, c);
    for (size_t i = 0U; i < ESP_ROM_MD5_DIGEST_LEN; i++) {
        out[2U * i]      = hex[d[i] >> 4];
        out[2U * i + 1U] = hex[d[i] & 0x0FU];
    }
    out[FCK_DRV_C5_ROM_MD5_LENGTH] = '\0';
}

static void md5_of(const uint8_t *data, uint32_t size, char out[FCK_DRV_C5_ROM_MD5_LENGTH + 1U])
{
    md5_context_t c;

    esp_rom_md5_init(&c);
    esp_rom_md5_update(&c, data, size);
    md5_hex(&c, out);
}

/* What each region's flash must hash to, before the C5 is touched: a raw
 * region whose bytes are not the MD5 it came with is refused here. */
static bool digests(const fck_drv_c5_rom_part_t *parts, size_t n,
                    char md5[][FCK_DRV_C5_ROM_MD5_LENGTH + 1U])
{
    for (size_t i = 0U; i < n; i++) {
        const fck_drv_c5_rom_part_t *p = &parts[i];
        if (p->zsize != 0U) {
            (void)md5_lower(p->md5, md5[i]);
            continue;
        }
        md5_of(p->data, p->size, md5[i]);
        char given[FCK_DRV_C5_ROM_MD5_LENGTH + 1U];
        if (p->md5 != NULL && (!md5_lower(p->md5, given) || strcmp(given, md5[i]) != 0)) {
            return false;
        }
    }
    return true;
}

/* ---- Each region, written ------------------------------------------------ */

typedef struct {
    fck_drv_c5_rom_progress_fn cb;
    void                  *ctx;
    uint32_t               done;
    uint32_t               total;
    uint32_t               pct;     /* the last percent reported */
} progress_t;

static void progress(progress_t *pr, uint32_t n)
{
    pr->done += n;
    if (pr->cb == NULL) {
        return;
    }
    uint32_t const pct = (uint32_t)(((uint64_t)pr->done * 100U) / pr->total);
    if (pct != pr->pct) {
        pr->pct = pct;
        pr->cb(pr->ctx, pr->done, pr->total);
    }
}

static esp_loader_error_t write_raw(esp_loader_t *l, const fck_drv_c5_rom_part_t *p,
                                    const char *md5, uint8_t *block, progress_t *pr)
{
    /* The library's own MD5 would take in the padding; the one below covers
     * the region alone. */
    esp_loader_flash_cfg_t cfg = {
        .offset      = p->addr,
        .image_size  = p->size,
        .block_size  = FCK_DRV_C5_ROM_BLOCK,
        .skip_verify = true,
    };
    esp_loader_error_t err = esp_loader_flash_start(l, &cfg);

    for (uint32_t off = 0U; err == ESP_LOADER_SUCCESS && off < p->size;) {
        uint32_t const left = p->size - off;
        uint32_t const n    = (left < FCK_DRV_C5_ROM_BLOCK) ? left : FCK_DRV_C5_ROM_BLOCK;
        (void)memcpy(block, &p->data[off], n);
        (void)memset(&block[n], 0xFF, FCK_DRV_C5_ROM_BLOCK - n);
        err = esp_loader_flash_write(l, &cfg, block, FCK_DRV_C5_ROM_BLOCK);
        off += n;
        progress(pr, n);
    }
    if (err == ESP_LOADER_SUCCESS) {
        err = esp_loader_flash_finish(l, &cfg);
    }
    if (err == ESP_LOADER_SUCCESS) {
        err = esp_loader_flash_verify_known_md5(l, p->addr, p->size, (const uint8_t *)md5);
    }
    return err;
}

/* What the ROM erases before a deflated region's first block: its size in
 * whole blocks. */
static uint32_t defl_erase(uint32_t size)
{
    return (size + FCK_DRV_C5_ROM_BLOCK - 1U) / FCK_DRV_C5_ROM_BLOCK * FCK_DRV_C5_ROM_BLOCK;
}

/* How long one deflated block of a region of @p size may take. */
static uint32_t defl_block_ms(uint32_t size)
{
    uint32_t const most = DEFL_MAX_RATIO * FCK_DRV_C5_ROM_BLOCK;
    uint32_t const out  = (size < most) ? size : most;
    uint32_t const ms   = (uint32_t)(((uint64_t)DEFL_MS_PER_MB * out) / 1000000U);
    return (ms > DEFL_MIN_MS) ? ms : DEFL_MIN_MS;
}

static esp_loader_error_t write_deflated(esp_loader_t *l, rom_port_t *port,
                                         const fck_drv_c5_rom_part_t *p, const char *md5,
                                         uint8_t *block, progress_t *pr)
{
    esp_loader_flash_deflate_cfg_t cfg = {
        .offset          = p->addr,
        .image_size      = defl_erase(p->size),
        .compressed_size = p->zsize,
        .block_size      = FCK_DRV_C5_ROM_BLOCK,
    };
    esp_loader_error_t err = esp_loader_flash_deflate_start(l, &cfg);

    port->floor_ms = defl_block_ms(p->size);
    for (uint32_t off = 0U; err == ESP_LOADER_SUCCESS && off < p->zsize;) {
        uint32_t const left = p->zsize - off;
        uint32_t const n    = (left < FCK_DRV_C5_ROM_BLOCK) ? left : FCK_DRV_C5_ROM_BLOCK;
        /* The call takes a buffer it may write to; the stream is read-only. */
        (void)memcpy(block, &p->data[off], n);
        err = esp_loader_flash_deflate_write(l, &cfg, block, n);
        off += n;
        progress(pr, n);
    }
    if (err == ESP_LOADER_SUCCESS) {
        err = esp_loader_flash_deflate_finish(l, &cfg);
    }
    port->floor_ms = 0U;
    if (err == ESP_LOADER_SUCCESS) {
        err = esp_loader_flash_verify_known_md5(l, p->addr, p->size, (const uint8_t *)md5);
    }
    return err;
}

/* ---- The session: into the loader, the questions, back to the flash ------ */

/* The stub esp-serial-flasher carries for the C5, and no other chip's: its own
 * provider, esp_loader_connect_with_stub(), would link every chip's. */
static const esp_stub_t *c5_stub(esp_loader_t *l, target_chip_t chip, void *ctx)
{
    (void)l;
    (void)ctx;
    return (chip == ESP32C5_CHIP) ? &esp_stub_esp32c5 : NULL;
}

static esp_loader_error_t connect(esp_loader_t *l, bool stub)
{
    esp_loader_connect_args_t args = ESP_LOADER_CONNECT_DEFAULT();
    esp_loader_error_t        err  = ESP_LOADER_ERROR_TIMEOUT;

    for (unsigned i = 0U; i < CONNECT_TRIES && err == ESP_LOADER_ERROR_TIMEOUT; i++) {
        err = stub ? esp_loader_connect_with_stub_provider(l, &args, c5_stub, NULL)
                   : esp_loader_connect(l, &args);
    }
    return err;
}

/* What the loader says about the chip: which one, its MAC, the two eFuses
 * that would keep a plain image from booting and, if neither is on, its
 * flash. */
static kode_status_t chip_info(esp_loader_t *l, fck_drv_c5_rom_report_t *rep, esp_loader_error_t *err)
{
    if (esp_loader_get_target(l) != ESP32C5_CHIP) {
        *err = ESP_LOADER_ERROR_INVALID_TARGET;
        return KODE_DENIED;
    }
    uint8_t sb  = 0U;
    uint8_t cnt = 0U;
    *err = esp_loader_read_mac(l, rep->mac);
    if (*err == ESP_LOADER_SUCCESS) {
        *err = esp_loader_efuse_read_field(l, ESP32C5_EFUSE_SECURE_BOOT_EN, &sb, 1U);
    }
    if (*err == ESP_LOADER_SUCCESS) {
        *err = esp_loader_efuse_read_field(l, ESP32C5_EFUSE_SPI_BOOT_CRYPT_CNT, &cnt, 3U);
    }
    if (*err != ESP_LOADER_SUCCESS) {
        return KODE_IO_ERROR;
    }
    rep->secure_boot = (sb & 1U) != 0U;
    /* On with 1 or 3 of its 3 bits burned, off with 0 or 2. */
    rep->flash_encryption = (__builtin_popcount(cnt & 7U) & 1) != 0;
    if (rep->secure_boot || rep->flash_encryption) {
        return KODE_DENIED;
    }

    *err = esp_loader_flash_detect_size(l, &rep->flash_size);
    if (*err == ESP_LOADER_ERROR_UNSUPPORTED_CHIP) {
        /* An ID with no size it knows: the library's own guess. */
        rep->flash_size = FLASH_GUESS;
        *err            = ESP_LOADER_SUCCESS;
    }
    if (*err != ESP_LOADER_SUCCESS) {
        rep->flash_size = 0U;
        return KODE_IO_ERROR;
    }
    return KODE_OK;
}

typedef struct {
    rom_port_t   port;
    esp_loader_t loader;
    int64_t      t0;
} session_t;

/* Into the ROM loader, and the chip asked about. Whatever comes of it,
 * session_close() follows. */
static kode_status_t session_open(session_t *s, const fck_drv_pins_t *hw, int uart,
                                  fck_drv_c5_rom_report_t *rep, esp_loader_error_t *err)
{
    s->port = (rom_port_t){ .base.ops = &s_ops, .hw = hw, .uart = uart, .rep = rep };
    s->t0   = esp_timer_get_time();
    *err    = esp_loader_init_serial(&s->loader, &s->port.base);

    (void)fck_drv_uart_baud(uart, FCK_BSP_C5_BAUD);
    if (*err == ESP_LOADER_SUCCESS) {
        *err = connect(&s->loader, false);
    }
    if (*err != ESP_LOADER_SUCCESS) {
        return (*err == ESP_LOADER_ERROR_TIMEOUT)        ? KODE_TIMEOUT
               : (*err == ESP_LOADER_ERROR_INVALID_TARGET) ? KODE_DENIED
                                                           : KODE_IO_ERROR;
    }
    rep->connected = true;
    return chip_info(&s->loader, rep, err);
}

/* The flasher stub into the C5's RAM, and run, once the ROM has said the C5
 * is open: a locked one never runs it. Connecting again resets the C5 into
 * its loader first. */
static kode_status_t session_stub(session_t *s, esp_loader_error_t *err)
{
    *err = connect(&s->loader, true);
    return (*err == ESP_LOADER_SUCCESS)         ? KODE_OK
           : (*err == ESP_LOADER_ERROR_TIMEOUT) ? KODE_TIMEOUT
                                                : KODE_IO_ERROR;
}

/* Back where the C5 runs: the UART first, since it boots at
 * FCK_BSP_C5_BAUD, then a reset with BOOT let go, which also takes the stub
 * away. */
static void session_close(session_t *s, fck_drv_c5_rom_report_t *rep, esp_loader_error_t err)
{
    (void)fck_drv_uart_baud(s->port.uart, FCK_BSP_C5_BAUD);
    esp_loader_reset_target(&s->loader);
    rep->reset = true;
    esp_loader_deinit(&s->loader);
    fck_drv_uart_flush(s->port.uart);

    rep->loader_error = (int)err;
    rep->ms           = (uint32_t)((esp_timer_get_time() - s->t0) / 1000);
}

/* Up to @p baud, or left at FCK_BSP_C5_BAUD if the loader will not move; @p at
 * says which. A loader that moved without the UART is a failure, before
 * anything is erased or read. */
static esp_loader_error_t speed_up(esp_loader_t *l, rom_port_t *port, uint32_t baud,
                                   uint32_t *at)
{
    *at = FCK_BSP_C5_BAUD;
    if (baud == FCK_BSP_C5_BAUD
        || esp_loader_change_transmission_rate(l, baud) != ESP_LOADER_SUCCESS) {
        return port->behind ? ESP_LOADER_ERROR_FAIL : ESP_LOADER_SUCCESS;
    }
    vTaskDelay(ticks_for(BAUD_SETTLE_MS));
    fck_drv_uart_flush(port->uart);
    *at = baud;
    return ESP_LOADER_SUCCESS;
}

/* ---- The write ----------------------------------------------------------- */

/* Every region must fit the flash before the first is erased
 * (esp-serial-flasher checks each one only as it starts it). A deflated
 * region's erase, in whole blocks, ends where its size does or before the
 * next sector: never past a flash of whole kilobytes. */
static kode_status_t fits(const fck_drv_c5_rom_part_t *parts, size_t n, uint32_t flash,
                          esp_loader_error_t *err)
{
    for (size_t i = 0U; i < n; i++) {
        if (parts[i].addr >= flash || parts[i].size > flash - parts[i].addr) {
            *err = ESP_LOADER_ERROR_IMAGE_SIZE;
            return KODE_DENIED;
        }
    }
    return KODE_OK;
}

static kode_status_t write_all(const fck_drv_pins_t *hw, int uart, const fck_drv_c5_rom_part_t *parts,
                               size_t n, char md5[][FCK_DRV_C5_ROM_MD5_LENGTH + 1U], uint32_t baud,
                               progress_t *pr, fck_drv_c5_rom_report_t *rep)
{
    uint8_t *block = malloc(FCK_DRV_C5_ROM_BLOCK);
    if (block == NULL) {
        return KODE_NO_RESOURCES;
    }
    session_t          s;
    esp_loader_error_t err;
    kode_status_t      st = session_open(&s, hw, uart, rep, &err);

    if (st == KODE_OK) {
        st = fits(parts, n, rep->flash_size, &err);
    }
    if (st == KODE_OK) {
        err = speed_up(&s.loader, &s.port, baud, &rep->baud);
        if (err != ESP_LOADER_SUCCESS) {
            st = KODE_IO_ERROR;
        }
        for (size_t i = 0U; i < n && err == ESP_LOADER_SUCCESS; i++) {
            err = (parts[i].zsize != 0U)
                      ? write_deflated(&s.loader, &s.port, &parts[i], md5[i], block, pr)
                      : write_raw(&s.loader, &parts[i], md5[i], block, pr);
            if (err != ESP_LOADER_SUCCESS) {
                rep->failed = (int)i;
                st          = KODE_IO_ERROR;
            }
        }
    }
    session_close(&s, rep, err);
    free(block);
    return st;
}

kode_status_t fck_drv_c5_rom_write(const fck_drv_pins_t *hw, int uart,
                               const fck_drv_c5_rom_part_t *parts, size_t n,
                               uint32_t baud, fck_drv_c5_rom_progress_fn cb,
                               void *ctx, fck_drv_c5_rom_report_t *report)
{
    fck_drv_c5_rom_report_t rep = { .baud = FCK_BSP_C5_BAUD, .failed = -1 };
    char                md5[FCK_DRV_C5_ROM_PARTS_MAX][FCK_DRV_C5_ROM_MD5_LENGTH + 1U];
    kode_status_t       st  = KODE_DENIED;

    if (hw != NULL && uart >= 0 && baud >= FCK_BSP_C5_BAUD && baud <= FCK_DRV_C5_ROM_BAUD_MAX
        && fck_drv_c5_rom_parts_ok(parts, n) && digests(parts, n, md5)) {
        progress_t pr = { .cb = cb, .ctx = ctx };
        for (size_t i = 0U; i < n; i++) {
            pr.total += (parts[i].zsize != 0U) ? parts[i].zsize : parts[i].size;
        }
        st = write_all(hw, uart, parts, n, md5, baud, &pr, &rep);
    }
    if (report != NULL) {
        *report = rep;
    }
    return st;
}

/* ---- The probe ----------------------------------------------------------- */

kode_status_t fck_drv_c5_rom_probe(const fck_drv_pins_t *hw, int uart, fck_drv_c5_rom_report_t *report)
{
    fck_drv_c5_rom_report_t rep = { .baud = FCK_BSP_C5_BAUD, .failed = -1 };
    kode_status_t       st  = KODE_DENIED;

    if (hw != NULL && uart >= 0) {
        session_t          s;
        esp_loader_error_t err;
        st = session_open(&s, hw, uart, &rep, &err);
        session_close(&s, &rep, err);
    }
    if (report != NULL) {
        *report = rep;
    }
    return st;
}

/* ---- What the C5 prints once it boots ------------------------------------ */

kode_status_t fck_drv_c5_rom_listen(const fck_drv_pins_t *hw, int uart, uint32_t ms,
                                    fck_drv_c5_boot_t *out, uint32_t *listened_ms)
{
    if (listened_ms != NULL) {
        *listened_ms = 0U;
    }
    if (out == NULL) {
        return KODE_DENIED;
    }
    fck_drv_c5_boot_init(out);
    if ((hw == NULL) || (uart < 0)) {
        return KODE_DENIED;
    }

    /* Held in reset while the UART empties: what was on its way before the
     * reset is not the ROM's first line. */
    fck_drv_pins_c5_reset(hw, false, true);
    fck_drv_uart_flush(uart);
    fck_drv_pins_c5_run(hw);
    int64_t const t0    = esp_timer_get_time();
    uint32_t      spent = 0U;
    uint8_t       buf[64];
    while (!fck_drv_c5_boot_done(out) && (spent < ms)) {
        uint32_t const left = ms - spent;
        size_t const   got  = fck_drv_uart_read_wait(uart, buf, sizeof(buf),
                                                     (left < LISTEN_POLL_MS) ? left : LISTEN_POLL_MS);
        fck_drv_c5_boot_feed(out, buf, got);
        int64_t const elapsed_ms = (esp_timer_get_time() - t0) / 1000;
        if ((got == 0U) && ((uint32_t)elapsed_ms == spent)) {
            /* A read that gave nothing and waited for nothing (a port that is not open) does not spin. */
            vTaskDelay(1);
        }
        spent = (uint32_t)elapsed_ms;
    }
    if (listened_ms != NULL) {
        *listened_ms = spent;
    }
    return KODE_OK;
}

/* ---- The read ------------------------------------------------------------ */

/* Block by block to @p sink. esp_loader_flash_read() checks each block's MD5
 * against the one the stub sends after it; the one for all of it is worked
 * out here, for the caller to keep. */
static kode_status_t read_all(session_t *s, uint32_t addr, uint32_t size, uint8_t *buf,
                              fck_drv_c5_rom_sink_fn sink, void *ctx, fck_drv_c5_rom_report_t *rep,
                              esp_loader_error_t *err)
{
    md5_context_t c;

    esp_rom_md5_init(&c);
    for (uint32_t off = 0U; off < size;) {
        uint32_t const left = size - off;
        uint32_t const n    = (left < FCK_DRV_C5_ROM_READ_CHUNK) ? left : FCK_DRV_C5_ROM_READ_CHUNK;
        *err = esp_loader_flash_read(&s->loader, buf, addr + off, n);
        if (*err != ESP_LOADER_SUCCESS) {
            rep->failed = 0;
            return KODE_IO_ERROR;
        }
        esp_rom_md5_update(&c, buf, n);
        if (!sink(ctx, addr + off, buf, n)) {
            rep->failed = 0;
            return KODE_ERROR;
        }
        off += n;
    }
    md5_hex(&c, rep->md5);
    return KODE_OK;
}

kode_status_t fck_drv_c5_rom_read(const fck_drv_pins_t *hw, int uart, uint32_t addr, uint32_t size,
                              uint32_t baud, fck_drv_c5_rom_sink_fn sink, void *ctx,
                              fck_drv_c5_rom_report_t *report)
{
    fck_drv_c5_rom_report_t rep = { .baud = FCK_BSP_C5_BAUD, .failed = -1 };
    kode_status_t       st  = KODE_DENIED;

    if (hw != NULL && uart >= 0 && sink != NULL && size != 0U && (addr % 4U) == 0U
        && (size % 4U) == 0U && addr < FCK_DRV_C5_ROM_REACH && size <= FCK_DRV_C5_ROM_REACH - addr
        && baud >= FCK_BSP_C5_BAUD && baud <= FCK_DRV_C5_ROM_BAUD_MAX) {
        uint8_t *buf = malloc(FCK_DRV_C5_ROM_READ_CHUNK);
        if (buf == NULL) {
            st = KODE_NO_RESOURCES;
        } else {
            session_t          s;
            esp_loader_error_t err;
            st = session_open(&s, hw, uart, &rep, &err);
            if (st == KODE_OK && (addr >= rep.flash_size || size > rep.flash_size - addr)) {
                err = ESP_LOADER_ERROR_IMAGE_SIZE;
                st  = KODE_DENIED;
            }
            if (st == KODE_OK) {
                st = session_stub(&s, &err);
            }
            if (st == KODE_OK) {
                err = speed_up(&s.loader, &s.port, baud, &rep.baud);
                st  = (err == ESP_LOADER_SUCCESS)
                          ? read_all(&s, addr, size, buf, sink, ctx, &rep, &err)
                          : KODE_IO_ERROR;
            }
            session_close(&s, &rep, err);
            free(buf);
        }
    }
    if (report != NULL) {
        *report = rep;
    }
    return st;
}
