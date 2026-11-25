/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file fck_drv_nrf_dfu.c
 * @brief A new image into the nRF, over SMP. See fck_drv_nrf_dfu.h.
 *
 * The packets, the framing and the answers are fck_drv_smp's; this file only
 * sends, waits and decides what comes next. MCUboot answers some 200 ms
 * after the reset. The module's one-slot MCUboot answers the echo at once
 * whatever the slot holds, and the image list in about 0.5 ms a KB of image,
 * because MCUboot validates the whole image on each list (650 ms for
 * 1.3 MB). That is why MCUboot is looked for with the echo, and its list
 * asked once, with time to answer.
 */
#include "fck_drv_nrf_dfu.h"

#include <string.h>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "fck_bsp.h"
#include "fck_drv_uart.h"

#define ECHO_TEXT        "kode"
#define ECHO_MS          1500U
#define STATE_MS         2000U
/* With BOOT_ERASE_PROGRESSIVELY=y the first piece erases only what it writes,
 * not the slot; the first piece still gets 15 s. */
#define FIRST_UPLOAD_MS  15000U
#define UPLOAD_MS        3000U
#define RESET_MS         1500U
#define UPLOAD_TRIES     5U       /* answers in a row with no progress       */
/* MCUboot's window: knocked on with an echo every KNOCK_MS for KNOCK_FOR_MS;
 * an answer later than WINDOW_MS after the reset is the image's, not
 * MCUboot's. */
#define KNOCK_MS         150U
#define KNOCK_FOR_MS     4000U
#define WINDOW_MS        2900U
/* MCUboot is asked which it is this many times: an echo lost on the way is
 * not a loader that has none. */
#define LOADER_TRIES     2U
#define RESET_PULSE_MS   20U
#define READ_SLICE_MS    20U
/* Once MCUboot lets the nRF go, nothing is sent for LISTEN_MS while the
 * image prints its first line (the firmware 2.x's came 6.3 s after the reset):
 * an SMP frame inside MCUboot's 3 s keeps it in recovery. With no line, the
 * last knocks cover one MCUboot window and a bit. */
#define LISTEN_MS        FCK_DRV_NRF_DFU_LISTEN_MS
#define LAST_KNOCK_FOR_MS 3500U

#define P6_MASK          ((uint8_t)(1U << FCK_BSP_EXPANDER_NRF_RST))

/* The probe's reading of the nRF's text, on its caller's stack. */
typedef struct {
    fck_drv_nrf_dfu_probe_t *rep;
    fck_drv_smp_text_t       reader;
} text_t;

/* One job at a time has the module, so one set of buffers, off the owner's
 * stack. */
static struct {
    fck_drv_smp_request_t req;
    fck_drv_smp_response_t rsp;
    fck_drv_smp_receive_t  rx;
    uint8_t       wire[FCK_DRV_SMP_WIRE_MAX];
    uint8_t       pkt[FCK_DRV_SMP_PACKET_MAX];
    uint8_t       in[64];
    uint8_t       seq;
    int32_t       refused;   /* the rc of the last answer that refused */
    text_t       *text;      /* the probe's, while it reads; NULL else */
} s;

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void sleep_ms(uint32_t ms)
{
    vTaskDelay(pdMS_TO_TICKS(ms));
}

/* s.req out, and its answer into s.rsp: KODE_TIMEOUT when none came within
 * @p ms. Packets that answer something else - a request that ran out - are
 * read past. */
static void text_feed(const uint8_t *b, size_t n);

static kode_status_t call(const fck_drv_nrf_dfu_io_t *io, uint32_t ms)
{
    size_t const n = fck_drv_smp_frame(&s.req, s.wire, sizeof(s.wire));
    if (n == 0U) {
        return KODE_DENIED;
    }
    fck_drv_uart_flush(io->uart);
    fck_drv_smp_receive_init(&s.rx);
    if (fck_drv_uart_write(io->uart, s.wire, n) != KODE_OK) {
        return KODE_IO_ERROR;
    }
    uint32_t const t0 = now_ms();
    for (;;) {
        uint32_t const gone = now_ms() - t0;
        if (gone >= ms) {
            return KODE_TIMEOUT;
        }
        uint32_t const wait = (ms - gone < READ_SLICE_MS) ? ms - gone : READ_SLICE_MS;
        size_t const   got  = fck_drv_uart_read_wait(io->uart, s.in, sizeof(s.in), wait);
        text_feed(s.in, got);
        for (size_t k = 0U; k < got; k++) {
            size_t const pn = fck_drv_smp_receive_byte(&s.rx, s.in[k], s.pkt, sizeof(s.pkt));
            if (pn != 0U && fck_drv_smp_parse(s.pkt, pn, &s.rsp)
                && fck_drv_smp_answers(&s.rsp, &s.req)) {
                return KODE_OK;
            }
        }
    }
}

static bool rc_ok(void)
{
    return !s.rsp.has_rc || s.rsp.rc == 0;
}

/* KODE_OK when the answer came and says done; KODE_ERROR when it refused. */
static kode_status_t call_ok(const fck_drv_nrf_dfu_io_t *io, uint32_t ms)
{
    kode_status_t const st = call(io, ms);
    if (st == KODE_OK && !rc_ok()) {
        s.refused = s.rsp.rc;
        return KODE_ERROR;
    }
    return st;
}

static kode_status_t echo(const fck_drv_nrf_dfu_io_t *io)
{
    (void)fck_drv_smp_request_echo(&s.req, s.seq++, ECHO_TEXT);
    kode_status_t const st = call_ok(io, ECHO_MS);
    return (st == KODE_OK && strcmp(s.rsp.echo, ECHO_TEXT) != 0) ? KODE_ERROR : st;
}

static kode_status_t state(const fck_drv_nrf_dfu_io_t *io, uint32_t ms)
{
    (void)fck_drv_smp_request_state(&s.req, s.seq++);
    return call_ok(io, ms);
}

/* The image list once, given STATE_MS: true with what it says in s.rsp;
 * false, and nothing there, when it did not come or was refused. MCUboot
 * always gives its list; an image that runs and answers the echo need not
 * (fck-nrf carries only SMP's OS group), so an echo answered and no list is
 * an image, not MCUboot. */
static bool list(const fck_drv_nrf_dfu_io_t *io)
{
    if (state(io, STATE_MS) != KODE_OK) {
        (void)memset(&s.rsp, 0, sizeof(s.rsp));
        return false;
    }
    return true;
}

/* What the image says of itself, before the nRF is touched: false for one
 * that cannot start in the slot, has no hash, or was linked for another
 * address (rep->image says which). Which MCUboot would boot it is asked
 * later, of the MCUboot. */
static bool image_ok(const uint8_t *img, size_t len, const uint8_t **hash, size_t *hash_len,
                     fck_drv_nrf_dfu_report_t *rep)
{
    _Static_assert(FCK_BSP_NRF_FIRST_IMAGE_MAX <= FCK_BSP_NRF_IMAGE_MAX,
                   "the first modules' slot is the shorter one");
    rep->image = fck_drv_smp_image_check(img, len, FCK_BSP_NRF_SLOT0, FCK_BSP_NRF_IMAGE_MAX,
                                         FCK_BSP_NRF_RAM, FCK_BSP_NRF_RAM_SIZE);
    if (rep->image != FCK_DRV_SMP_IMAGE_FITS) {
        return false;
    }
    rep->has_map = fck_drv_smp_image_map(img, len, &rep->map);
    if (rep->has_map && rep->map != FCK_BSP_NRF_IMAGE_START) {
        rep->image = FCK_DRV_SMP_IMAGE_MAP;
        return false;
    }
    rep->holds_start = fck_drv_smp_image_holds(img, len, FCK_BSP_NRF_IMAGE_START);
    if (!fck_drv_smp_image_hash(img, len, hash, hash_len)) {
        rep->image = FCK_DRV_SMP_IMAGE_LOADER;
        return false;
    }
    return true;
}

/* Slot 0, as a list says it: NULL when it says nothing of it, or it holds
 * another image than the one whose hash is @p hash. A running image's list
 * says whether it runs (active) and stays (confirmed); MCUboot's own list,
 * in recovery, says neither. */
static const fck_drv_smp_image_t *slot0_is(const fck_drv_smp_response_t *st, const uint8_t *hash,
                                       size_t hash_len)
{
    const fck_drv_smp_image_t *const i = fck_drv_smp_slot(st, 0);
    return (i != NULL && i->hash_len == hash_len && memcmp(i->hash, hash, hash_len) == 0) ? i
                                                                                       : NULL;
}

static void keep_version(char *dst, size_t cap, const fck_drv_smp_response_t *st)
{
    const fck_drv_smp_image_t *const i = fck_drv_smp_slot(st, 0);
    (void)strncpy(dst, (i != NULL) ? i->version : "", cap - 1U);
    dst[cap - 1U] = '\0';
}

static kode_status_t upload(const fck_drv_nrf_dfu_io_t *io, const uint8_t *img, size_t len,
                            fck_drv_nrf_dfu_report_t *rep)
{
    size_t   off   = 0U;
    unsigned tries = 0U;
    while (off < len) {
        size_t took;
        if (fck_drv_smp_request_upload(&s.req, s.seq++, img, len, off, &took) != KODE_OK) {
            return KODE_DENIED;
        }
        kode_status_t const st = call(io, (off == 0U) ? FIRST_UPLOAD_MS : UPLOAD_MS);
        if (st == KODE_IO_ERROR) {
            return st;
        }
        bool const moved = st == KODE_OK && rc_ok() && s.rsp.has_off && s.rsp.off > off
                           && s.rsp.off <= len;
        if (!moved) {
            /* A piece lost on the way, or an answer to it: the next one
             * goes again from where the server last said. */
            if (++tries >= UPLOAD_TRIES) {
                s.refused = (st == KODE_OK && s.rsp.has_rc) ? s.rsp.rc : -1;
                return (st == KODE_OK) ? KODE_ERROR : st;
            }
            if (st == KODE_OK && rc_ok() && s.rsp.has_off && s.rsp.off < len) {
                off = s.rsp.off;
            }
            continue;
        }
        tries        = 0U;
        off          = s.rsp.off;
        rep->written = (uint32_t)off;
    }
    return KODE_OK;
}

static kode_status_t pulse(const fck_drv_nrf_dfu_io_t *io)
{
    kode_status_t const st = fck_drv_tca6408a_mask(io->tca, P6_MASK, true, false);
    sleep_ms(RESET_PULSE_MS);
    kode_status_t const up = fck_drv_tca6408a_mask(io->tca, P6_MASK, false, true);
    return (st == KODE_OK) ? up : st;
}

static bool line_taken(void)
{
    return (s.text != NULL) && (s.text->rep->line[0] != '\0');
}

/* Right after a reset: an echo every KNOCK_MS for up to @p for_ms, until
 * something answers it - or, while the probe reads, prints a line once
 * @p stop_on_line. Any answer will do, a refusal too: a first module's loader
 * refuses the echo. Not the image list: MCUboot validates the whole image
 * before it answers one, longer than KNOCK_MS for an image over ~250 KB, and
 * an answer to a knock already given up is read past. KODE_OK with the answer
 * in s.rsp; *at_ms is when. */
static kode_status_t knock(const fck_drv_nrf_dfu_io_t *io, uint32_t for_ms, bool stop_on_line,
                           uint32_t *at_ms)
{
    uint32_t const t0 = now_ms();
    kode_status_t  st = KODE_TIMEOUT;
    while ((st != KODE_OK) && ((now_ms() - t0) < for_ms) && !(stop_on_line && line_taken())) {
        (void)fck_drv_smp_request_echo(&s.req, s.seq++, ECHO_TEXT);
        st = call(io, KNOCK_MS);
        if (st == KODE_IO_ERROR) {
            break;
        }
    }
    *at_ms = now_ms() - t0;
    return st;
}

/* MCUboot out of its recovery: its own reset, the expander's if it does not
 * answer. */
static kode_status_t mcuboot_reset(const fck_drv_nrf_dfu_io_t *io)
{
    (void)fck_drv_smp_request_reset(&s.req, s.seq++);
    return ((call(io, RESET_MS) == KODE_OK) || (pulse(io) == KODE_OK)) ? KODE_OK : KODE_IO_ERROR;
}

static void report_start(fck_drv_nrf_dfu_report_t *rep)
{
    memset(rep, 0, sizeof(*rep));
    rep->rc   = -1;
    rep->ms   = now_ms();
    s.refused = -1;
}

static kode_status_t report_end(fck_drv_nrf_dfu_report_t *rep, fck_drv_nrf_dfu_step_t step,
                                kode_status_t st)
{
    if (st != KODE_OK) {
        rep->failed = step;
        rep->rc     = s.refused;
    }
    rep->ms = now_ms() - rep->ms;
    return st;
}

/* Which MCUboot is in its recovery: the module's answers an echo, a first
 * module's answers that it has none. KODE_TIMEOUT when it said neither. */
static kode_status_t loader_of(const fck_drv_nrf_dfu_io_t *io, fck_drv_smp_loader_t *out)
{
    kode_status_t st = KODE_TIMEOUT;
    for (unsigned k = 0U; k < LOADER_TRIES; k++) {
        st = echo(io);
        if (st == KODE_OK) {
            *out = FCK_DRV_SMP_LOADER_HASH;
            return KODE_OK;
        }
        if (st == KODE_ERROR) {
            /* TEMPORARY: pre-production modules. Remove when the production modules ship.
             * It answered, and not with the echo: a loader without one. */
            *out      = FCK_DRV_SMP_LOADER_SIGNED;
            s.refused = -1;
            return KODE_OK;
        }
        if (st == KODE_IO_ERROR) {
            break;
        }
    }
    return st;
}

static void text_start(text_t *t);
static kode_status_t listen(const fck_drv_nrf_dfu_io_t *io, uint32_t ms);

/* MCUboot was let go: whether what it started says something. Nothing is
 * sent while it may still be in its window. */
static bool started(const fck_drv_nrf_dfu_io_t *io, fck_drv_nrf_dfu_report_t *rep)
{
    fck_drv_nrf_dfu_probe_t heard = { 0 };
    text_t                  t     = { .rep = &heard };
    text_start(&t);
    s.text = &t;
    (void)listen(io, LISTEN_MS);
    s.text = NULL;
    _Static_assert(sizeof(rep->line) == sizeof(heard.line), "the report keeps the image's first line");
    (void)memcpy(rep->line, heard.line, sizeof(rep->line));
    return (heard.line[0] != '\0') || heard.banner;
}

kode_status_t fck_drv_nrf_dfu_recover(const fck_drv_nrf_dfu_io_t *io, const uint8_t *img, size_t len,
                                  fck_drv_nrf_dfu_report_t *rep)
{
    fck_drv_nrf_dfu_report_t dummy;
    rep = (rep != NULL) ? rep : &dummy;
    report_start(rep);
    const uint8_t *hash;
    size_t         hash_len;
    if (io == NULL || !image_ok(img, len, &hash, &hash_len, rep)) {
        return report_end(rep, FCK_DRV_NRF_DFU_STEP_IMAGE, KODE_DENIED);
    }
    if (io->tca == NULL) {
        return report_end(rep, FCK_DRV_NRF_DFU_STEP_RESET, KODE_DENIED);
    }
    kode_status_t st = pulse(io);
    if (st != KODE_OK) {
        return report_end(rep, FCK_DRV_NRF_DFU_STEP_RESET, KODE_IO_ERROR);
    }
    rep->reset = true;
    uint32_t at_ms;
    st = knock(io, KNOCK_FOR_MS, false, &at_ms);
    if (st == KODE_IO_ERROR) {
        return report_end(rep, FCK_DRV_NRF_DFU_STEP_SERVER, st);
    }
    if (st != KODE_OK) {
        return report_end(rep, FCK_DRV_NRF_DFU_STEP_SERVER, KODE_TIMEOUT);
    }
    if (at_ms > WINDOW_MS) {
        /* Past MCUboot's window: the image answered, and MCUboot has gone. */
        return report_end(rep, FCK_DRV_NRF_DFU_STEP_SERVER, KODE_ERROR);
    }
    if (!list(io)) {
        /* What answered gave no list: not MCUboot (an image that kept
         * running, the reset not taken), or one that cannot read its slot.
         * Nothing is written to it, and it is let go as it was. */
        (void)mcuboot_reset(io);
        return report_end(rep, FCK_DRV_NRF_DFU_STEP_SERVER, KODE_ERROR);
    }
    keep_version(rep->before, sizeof(rep->before), &s.rsp);
    bool const same = slot0_is(&s.rsp, hash, hash_len) != NULL;

    /* Which MCUboot it is, before a byte is uploaded: an image it would not
     * boot would leave the nRF in recovery, with nothing to go back to. */
    st = loader_of(io, &rep->loader);
    if (st != KODE_OK) {
        (void)mcuboot_reset(io);
        return report_end(rep, FCK_DRV_NRF_DFU_STEP_SERVER, st);
    }
    rep->loader_known = true;
    bool const first  = (rep->loader == FCK_DRV_SMP_LOADER_SIGNED);
    if (!fck_drv_smp_image_suits(img, len, rep->loader) || (first && len > FCK_BSP_NRF_FIRST_IMAGE_MAX)) {
        rep->image = fck_drv_smp_image_suits(img, len, rep->loader) ? FCK_DRV_SMP_IMAGE_LARGER
                                                                    : FCK_DRV_SMP_IMAGE_LOADER;
        (void)mcuboot_reset(io);     /* what it had boots again */
        return report_end(rep, FCK_DRV_NRF_DFU_STEP_IMAGE, KODE_DENIED);
    }

    if (same) {
        rep->same = true;
    } else {
        st = upload(io, img, len, rep);
        if (st != KODE_OK) {
            if (rep->written == 0U) {
                /* Not a byte taken: the slot was not touched, and MCUboot
                 * would stay in recovery for good. Let it go, to boot the
                 * image it had. Once something was written the slot holds
                 * nothing to boot, and recovery is where it stays. */
                (void)mcuboot_reset(io);
            }
            return report_end(rep, FCK_DRV_NRF_DFU_STEP_UPLOAD, st);
        }
        /* What the slot holds now, before MCUboot is let go: an image stored
         * other than it was sent would boot nothing, and MCUboot is still in
         * recovery for the next try (the old image is gone anyway). A list
         * that does not come within STATE_MS is not a failure - MCUboot
         * validates the whole image on each list, and a large one takes
         * long - so the write goes on as it did before. */
        if (state(io, STATE_MS) == KODE_OK && slot0_is(&s.rsp, hash, hash_len) == NULL) {
            return report_end(rep, FCK_DRV_NRF_DFU_STEP_UPLOAD, KODE_ERROR);
        }
    }
    if (mcuboot_reset(io) != KODE_OK) {
        return report_end(rep, FCK_DRV_NRF_DFU_STEP_RESET, KODE_IO_ERROR);
    }
    if (!started(io, rep)) {
        /* Written, and silent: MCUboot found it wanting and stays in
         * recovery, or it hangs, or it never prints. */
        return report_end(rep, FCK_DRV_NRF_DFU_STEP_BOOT, KODE_ERROR);
    }
    rep->answered = true;
    return report_end(rep, FCK_DRV_NRF_DFU_STEP_NONE, KODE_OK);
}

/* ---- What the nRF carries ----------------------------------------------------------- */

/* The first line the image prints is kept, and nothing after it. */
static void text_feed(const uint8_t *b, size_t n)
{
    text_t *const t = s.text;
    for (size_t k = 0U; (t != NULL) && (t->rep->line[0] == '\0') && (k < n); k++) {
        if (fck_drv_smp_text_byte(&t->reader, b[k], t->rep->line, sizeof(t->rep->line))
            == FCK_DRV_SMP_LINE_BANNER) {
            t->rep->banner = true;
        }
    }
}

/* Whatever came so far is forgotten. */
static void text_start(text_t *t)
{
    fck_drv_smp_text_init(&t->reader);
    t->rep->line[0] = '\0';
    t->rep->banner  = false;
}

/* Nothing sent for up to @p ms while the nRF prints: KODE_OK once it
 * printed its first line. */
static kode_status_t listen(const fck_drv_nrf_dfu_io_t *io, uint32_t ms)
{
    uint32_t const t0 = now_ms();
    while (!line_taken()) {
        uint32_t const gone = now_ms() - t0;
        if (gone >= ms) {
            return KODE_TIMEOUT;
        }
        uint32_t const wait = ((ms - gone) < READ_SLICE_MS) ? (ms - gone) : READ_SLICE_MS;
        size_t const   got  = fck_drv_uart_read_wait(io->uart, s.in, sizeof(s.in), wait);
        text_feed(s.in, got);
    }
    return KODE_OK;
}

/* Slot 0 as the list in s.rsp names it. */
static void probe_slot0(fck_drv_nrf_dfu_probe_t *rep)
{
    const fck_drv_smp_image_t *const i = fck_drv_smp_slot(&s.rsp, 0);
    if (i == NULL) {
        return;
    }
    rep->image   = true;
    rep->running = i->active;
    (void)strncpy(rep->version, i->version, sizeof(rep->version) - 1U);
    rep->version[sizeof(rep->version) - 1U] = '\0';
    size_t const n = (i->hash_len < sizeof(rep->hash)) ? i->hash_len : sizeof(rep->hash);
    (void)memcpy(rep->hash, i->hash, n);
}

static kode_status_t probe(const fck_drv_nrf_dfu_io_t *io, text_t *t)
{
    fck_drv_nrf_dfu_probe_t *const rep = t->rep;
    if (pulse(io) != KODE_OK) {
        return KODE_IO_ERROR;
    }
    rep->reset = true;
    s.text     = t;
    uint32_t      at_ms;
    kode_status_t st = knock(io, KNOCK_FOR_MS, false, &at_ms);
    if (st == KODE_IO_ERROR) {
        return st;
    }
    if (st != KODE_OK) {
        /* No MCUboot and no image that speaks SMP; one that prints may have
         * said something meanwhile, and rep->line has it. */
        return KODE_OK;
    }
    (void)list(io);
    probe_slot0(rep);
    if (at_ms > WINDOW_MS) {
        /* An image answered, and MCUboot's window was missed: it runs. */
        rep->late = true;
        return KODE_OK;
    }
    rep->mcuboot    = true;
    rep->mcuboot_ms = at_ms;
    rep->running    = false;   /* MCUboot's own list says nothing runs */
    fck_drv_smp_loader_t loader;
    rep->echo = (loader_of(io, &loader) == KODE_OK) && (loader == FCK_DRV_SMP_LOADER_HASH);
    if (mcuboot_reset(io) != KODE_OK) {
        return KODE_IO_ERROR;
    }
    text_start(t);
    if (listen(io, LISTEN_MS) == KODE_OK) {
        return KODE_OK;
    }
    st = knock(io, LAST_KNOCK_FOR_MS, true, &at_ms);
    if (st == KODE_IO_ERROR) {
        return st;
    }
    if (st == KODE_OK) {
        bool const                       listed = list(io);
        const fck_drv_smp_image_t *const i      = fck_drv_smp_slot(&s.rsp, 0);
        rep->answered = true;
        /* No list: an image that answers the echo and not the list runs (an
         * MCUboot in recovery always gives its list). */
        rep->running  = listed ? ((i != NULL) && i->active) : true;
        if (!rep->running) {
            /* MCUboot, kept in recovery by that knock: let it go on as it
             * was. */
            return mcuboot_reset(io);
        }
    }
    return KODE_OK;
}

kode_status_t fck_drv_nrf_dfu_probe(const fck_drv_nrf_dfu_io_t *io, fck_drv_nrf_dfu_probe_t *rep)
{
    fck_drv_nrf_dfu_probe_t        dummy;
    fck_drv_nrf_dfu_probe_t *const r = (rep != NULL) ? rep : &dummy;
    (void)memset(r, 0, sizeof(*r));
    uint32_t const t0 = now_ms();
    s.refused         = -1;
    if ((io == NULL) || (io->tca == NULL)) {
        return KODE_DENIED;
    }
    text_t              t  = { .rep = r };
    kode_status_t const st = probe(io, &t);
    s.text                 = NULL;
    r->ms                  = now_ms() - t0;
    return st;
}
