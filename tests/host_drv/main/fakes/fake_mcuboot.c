/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file fake_mcuboot.c
 * @brief fake_mcuboot.h: MCUboot's serial recovery and the image behind it,
 *        on the fake ESP-IDF's UART, expander and clock.
 */
#include "fake_mcuboot.h"

#include <stdio.h>
#include <string.h>

#include "esp_timer.h"

#include "fck_bsp.h"
#include "fck_drv_smp.h"

#include "fake_idf.h"

fake_mcuboot_t g_mcuboot;

#define PENDING_MAX   8U
#define BANNER_MS     30U
#define REBOOT_MS     10U      /* from SMP's reset answer to the nRF restarting */
#define P6            ((uint8_t)(1U << FCK_BSP_EXPANDER_NRF_RST))

#define SMP_OP_READ   0U
#define SMP_OP_WRITE  2U
#define SMP_GROUP_OS  0U
#define SMP_GROUP_IMG 1U
#define SMP_ID_ECHO   0U
#define SMP_ID_RESET  5U
#define SMP_ID_STATE  0U
#define SMP_ID_UPLOAD 1U
#define SMP_RC_ENOTSUP 8

static const char k_banner[] = "*** Booting MCUboot v2.1.0 ***\r\n";

typedef enum {
    MODE_IMAGE,       /* what boots runs (or the nRF ran it before the first reset) */
    MODE_RESET,       /* the pin holds the nRF in reset                              */
    MODE_RECOVERY,    /* MCUboot, listening or held                                  */
} fake_mode_t;

static struct {
    int      port;
    fake_mode_t  mode;
    int64_t  boot_at_us;
    bool     held;           /* a frame came: it stays until the next reset          */
    bool     banner_sent;
    bool     line_sent;
    bool     pin_low;
    unsigned echoes_seen;
    int64_t  busy_until_us;  /* one request at a time: the next answer waits for this one */

    uint8_t  slot[FAKE_MCUBOOT_SLOT_BYTES];
    uint32_t slot_len;       /* the length the first piece said                      */
    uint32_t written;
    bool     present;        /* a list names an image in it                          */
    bool     bad;            /* what was stored is not what was sent                 */
    bool     fresh;          /* written whole by an upload, since the last boot      */

    struct {
        bool    used;
        int64_t due_us;
        size_t  n;
        uint8_t wire[FCK_DRV_SMP_WIRE_MAX];
    } pending[PENDING_MAX];
} nrf;

static int64_t now_us(void)
{
    return esp_timer_get_time();
}

/* ---- What boots ---------------------------------------------------------------------- */

static void boot(int64_t at_us)
{
    nrf.mode        = MODE_RECOVERY;
    nrf.boot_at_us  = at_us;
    nrf.held        = false;
    nrf.banner_sent = false;
    nrf.line_sent   = false;
    nrf.fresh       = false;
    nrf.busy_until_us = 0;
}

bool fake_mcuboot_slot_boots(void)
{
    return nrf.present && !nrf.bad && nrf.written == nrf.slot_len;
}

bool fake_mcuboot_in_recovery(void)
{
    return nrf.mode == MODE_RECOVERY;
}

const uint8_t *fake_mcuboot_slot(size_t *written)
{
    *written = nrf.written;
    return nrf.slot;
}

/* MCUboot's window over: it boots what the slot holds, or stays. */
static void advance(int64_t now)
{
    if (nrf.mode == MODE_RECOVERY && !nrf.held
        && now >= nrf.boot_at_us + ((int64_t)g_mcuboot.window_ms * 1000)) {
        if (fake_mcuboot_slot_boots()) {
            nrf.mode = MODE_IMAGE;
        } else {
            nrf.held = true;   /* nothing to boot: it waits for an upload, for good */
        }
    }
}

static bool listening(int64_t now)
{
    return nrf.mode == MODE_RECOVERY && !g_mcuboot.silent
           && now >= nrf.boot_at_us + ((int64_t)g_mcuboot.ready_ms * 1000);
}

/* ---- What it says -------------------------------------------------------------------- */

/* Delivered to the UART once their time has come. */
static void deliver(int port)
{
    int64_t const now = now_us();
    if (port != nrf.port) {
        return;
    }
    advance(now);
    if (nrf.mode == MODE_RESET) {
        return;
    }
    for (unsigned k = 0U; k < PENDING_MAX; k++) {
        if (nrf.pending[k].used && nrf.pending[k].due_us <= now) {
            idf_fake_uart_feed(port, nrf.pending[k].wire, nrf.pending[k].n);
            nrf.pending[k].used = false;
        }
    }
    if (nrf.mode == MODE_RECOVERY && !nrf.banner_sent
        && now >= nrf.boot_at_us + ((int64_t)BANNER_MS * 1000)) {
        nrf.banner_sent = true;
        idf_fake_uart_feed(port, k_banner, sizeof(k_banner) - 1U);
    }
    if (nrf.mode == MODE_IMAGE && !nrf.line_sent
        && now >= nrf.boot_at_us + ((int64_t)g_mcuboot.boot_to_line_ms * 1000)) {
        nrf.line_sent = true;
        if (g_mcuboot.image_prints && g_mcuboot.image_line != NULL) {
            idf_fake_uart_feed(port, g_mcuboot.image_line, strlen(g_mcuboot.image_line));
            idf_fake_uart_feed(port, "\r\n", 2U);
        }
    }
}

/* A minimal CBOR writer: only what MCUboot's answers use. */
typedef struct {
    uint8_t b[FCK_DRV_SMP_PACKET_MAX];
    size_t  n;
} cbor_t;

static void put_head(cbor_t *c, uint8_t major, uint32_t arg)
{
    if (arg < 24U) {
        c->b[c->n++] = (uint8_t)((major << 5) | arg);
    } else if (arg < 256U) {
        c->b[c->n++] = (uint8_t)((major << 5) | 24U);
        c->b[c->n++] = (uint8_t)arg;
    } else {
        c->b[c->n++] = (uint8_t)((major << 5) | 25U);
        c->b[c->n++] = (uint8_t)(arg >> 8);
        c->b[c->n++] = (uint8_t)arg;
    }
}

static void put_text(cbor_t *c, const char *s)
{
    put_head(c, 3U, (uint32_t)strlen(s));
    (void)memcpy(&c->b[c->n], s, strlen(s));
    c->n += strlen(s);
}

static void put_bytes(cbor_t *c, const uint8_t *p, size_t n)
{
    put_head(c, 2U, (uint32_t)n);
    (void)memcpy(&c->b[c->n], p, n);
    c->n += n;
}

static void put_int(cbor_t *c, int32_t v)
{
    if (v >= 0) {
        put_head(c, 0U, (uint32_t)v);
    } else {
        put_head(c, 1U, (uint32_t)(-1 - v));
    }
}

/* The answer to a request with the header @p req (its op + 1, same group,
 * sequence number and id), on the wire @p ms after the request before it was
 * answered, or from now: boot_serial takes one request at a time, so what
 * comes while it validates an image for a list waits behind that list. */
static void answer(const uint8_t *req, const cbor_t *c, uint32_t ms)
{
    fck_drv_smp_request_t r;
    r.pkt[0] = (uint8_t)(req[0] + 1U);
    r.pkt[1] = 0U;
    r.pkt[2] = (uint8_t)(c->n >> 8);
    r.pkt[3] = (uint8_t)c->n;
    r.pkt[4] = req[4];
    r.pkt[5] = req[5];
    r.pkt[6] = req[6];
    r.pkt[7] = req[7];
    (void)memcpy(&r.pkt[8], c->b, c->n);
    r.len = 8U + c->n;
    for (unsigned k = 0U; k < PENDING_MAX; k++) {
        if (!nrf.pending[k].used) {
            nrf.pending[k].n      = fck_drv_smp_frame(&r, nrf.pending[k].wire, sizeof(nrf.pending[k].wire));
            int64_t const from    = (nrf.busy_until_us > now_us()) ? nrf.busy_until_us : now_us();
            nrf.pending[k].due_us = from + ((int64_t)ms * 1000);
            nrf.busy_until_us     = nrf.pending[k].due_us;
            nrf.pending[k].used   = (nrf.pending[k].n != 0U);
            return;
        }
    }
}

static void answer_rc(const uint8_t *req, int32_t rc, uint32_t ms)
{
    cbor_t c = { .n = 0U };
    c.b[c.n++] = 0xBF;
    put_text(&c, "rc");
    put_int(&c, rc);
    c.b[c.n++] = 0xFF;
    answer(req, &c, ms);
}

static void answer_offset(const uint8_t *req, uint32_t ms)
{
    cbor_t c = { .n = 0U };
    c.b[c.n++] = 0xBF;
    put_text(&c, "rc");
    put_int(&c, 0);
    put_text(&c, "off");
    put_head(&c, 0U, nrf.written);
    c.b[c.n++] = 0xFF;
    answer(req, &c, ms);
}

/* ---- What it is asked ---------------------------------------------------------------- */

static void echo_back(const uint8_t *req, const uint8_t *body, size_t n);

static void do_echo(const uint8_t *req, const uint8_t *body, size_t n)
{
    g_mcuboot.echoes++;
    nrf.echoes_seen++;
    bool const dropped = (nrf.echoes_seen > g_mcuboot.drop_echoes_after)
                         && (nrf.echoes_seen <= g_mcuboot.drop_echoes_after + g_mcuboot.drop_echoes);
    if (g_mcuboot.echo == FAKE_MCUBOOT_ECHO_SILENT || dropped) {
        return;
    }
    if (g_mcuboot.echo == FAKE_MCUBOOT_ECHO_REFUSED) {
        answer_rc(req, SMP_RC_ENOTSUP, g_mcuboot.answer_ms);
        return;
    }
    echo_back(req, body, n);
}

/* The request's map is {"d": text}: the text comes back as {"r": text}. */
static void echo_back(const uint8_t *req, const uint8_t *body, size_t n)
{
    cbor_t c = { .n = 0U };
    c.b[c.n++] = 0xBF;
    put_text(&c, "r");
    if (n > 3U) {
        size_t const len = body[3] & 0x1FU;
        put_head(&c, 3U, (uint32_t)len);
        (void)memcpy(&c.b[c.n], &body[4], len);
        c.n += len;
    }
    c.b[c.n++] = 0xFF;
    answer(req, &c, g_mcuboot.answer_ms);
}

static void do_list(const uint8_t *req)
{
    g_mcuboot.lists++;
    if (g_mcuboot.list_mute_when_complete && nrf.fresh) {
        return;
    }
    if (g_mcuboot.list_rc != 0) {
        answer_rc(req, g_mcuboot.list_rc, g_mcuboot.list_ms);
        return;
    }
    cbor_t c = { .n = 0U };
    c.b[c.n++] = 0xBF;            /* the outermost map is left open, as MCUboot's is */
    put_text(&c, "images");
    c.b[c.n++] = 0x9F;
    const uint8_t *hash;
    size_t         hash_len;
    if (nrf.present && fck_drv_smp_image_hash(nrf.slot, nrf.written, &hash, &hash_len)) {
        uint8_t h[FCK_DRV_SMP_HASH_MAX];
        (void)memcpy(h, hash, hash_len);
        if (nrf.bad) {
            h[0] = (uint8_t)~h[0];
        }
        char version[24];
        (void)snprintf(version, sizeof(version), "%u.%u.%u", (unsigned)nrf.slot[20], (unsigned)nrf.slot[21],
                       (unsigned)(nrf.slot[22] | (nrf.slot[23] << 8)));
        c.b[c.n++] = 0xBF;
        put_text(&c, "image");
        put_int(&c, 0);
        put_text(&c, "slot");
        put_int(&c, 0);
        put_text(&c, "version");
        put_text(&c, version);
        put_text(&c, "hash");
        put_bytes(&c, h, hash_len);
        c.b[c.n++] = 0xFF;
    }
    c.b[c.n++] = 0xFF;
    answer(req, &c, g_mcuboot.list_ms);
}

/* The head of one CBOR item at @p *p: its major type and argument. */
static bool item(const uint8_t **p, const uint8_t *end, uint8_t *major, uint32_t *arg)
{
    if (*p >= end) {
        return false;
    }
    uint8_t const ib = **p;
    (*p)++;
    *major = (uint8_t)(ib >> 5);
    uint8_t const ai = ib & 0x1FU;
    if (ai < 24U) {
        *arg = ai;
        return true;
    }
    size_t const w = (ai == 24U) ? 1U : (ai == 25U) ? 2U : (ai == 26U) ? 4U : 0U;
    if (w == 0U || *p + w > end) {
        return false;
    }
    *arg = 0U;
    for (size_t k = 0U; k < w; k++) {
        *arg = (*arg << 8) | **p;
        (*p)++;
    }
    return true;
}

static void do_upload(const uint8_t *req, const uint8_t *body, size_t n)
{
    g_mcuboot.pieces++;
    uint32_t       off = 0U, len = 0U, size = 0U;
    bool           has_len = false;
    const uint8_t *data = NULL;
    const uint8_t *p = body;
    const uint8_t *end = body + n;
    uint8_t        major;
    uint32_t       pairs;
    if (!item(&p, end, &major, &pairs) || major != 5U) {
        return;
    }
    for (uint32_t k = 0U; k < pairs; k++) {
        uint32_t klen, v;
        char     key[8] = { 0 };
        if (!item(&p, end, &major, &klen) || major != 3U || klen >= sizeof(key) || p + klen > end) {
            return;
        }
        (void)memcpy(key, p, klen);
        p += klen;
        if (!item(&p, end, &major, &v)) {
            return;
        }
        if (strcmp(key, "off") == 0) {
            off = v;
        } else if (strcmp(key, "len") == 0) {
            len     = v;
            has_len = true;
        } else if (strcmp(key, "data") == 0 && major == 2U && p + v <= end) {
            data = p;
            size = v;
            p += v;
        }
    }
    if (g_mcuboot.refuse_rc != 0) {
        answer_rc(req, g_mcuboot.refuse_rc, g_mcuboot.answer_ms);
        return;
    }
    uint32_t ms = g_mcuboot.answer_ms;
    if (off == 0U && has_len) {
        if (len > g_mcuboot.slot_max || len > sizeof(nrf.slot)) {
            answer_rc(req, 2, g_mcuboot.answer_ms);   /* no memory */
            return;
        }
        /* The slot is erased where it will be written, and the old image is gone. */
        nrf.slot_len = len;
        nrf.written  = 0U;
        nrf.present  = false;
        nrf.bad      = false;
        nrf.fresh    = false;
        ms         = g_mcuboot.first_piece_ms;
    }
    if (g_mcuboot.mute_at != 0U && nrf.written >= g_mcuboot.mute_at) {
        return;
    }
    if (off != nrf.written || data == NULL || nrf.written + size > nrf.slot_len
        || (g_mcuboot.stall_at != 0U && nrf.written >= g_mcuboot.stall_at)) {
        answer_offset(req, ms);
        return;
    }
    (void)memcpy(&nrf.slot[nrf.written], data, size);
    if (g_mcuboot.corrupt_store) {
        nrf.slot[nrf.written] = (uint8_t)~nrf.slot[nrf.written];
        nrf.bad             = true;
    }
    nrf.written += size;
    if (nrf.written == nrf.slot_len) {
        nrf.present = true;
        nrf.fresh   = true;
    }
    answer_offset(req, ms);
}

static void handle(const uint8_t *pkt, size_t n)
{
    uint8_t const  op    = pkt[0];
    uint16_t const group = (uint16_t)((pkt[4] << 8) | pkt[5]);
    uint8_t const  id    = pkt[7];
    const uint8_t *body  = pkt + 8;
    size_t const   bn    = n - 8U;
    g_mcuboot.frames++;
    nrf.held = true;   /* a frame inside the window: it stays in recovery */
    if (op == SMP_OP_WRITE && group == SMP_GROUP_OS && id == SMP_ID_ECHO) {
        do_echo(pkt, body, bn);
    } else if (op == SMP_OP_READ && group == SMP_GROUP_IMG && id == SMP_ID_STATE) {
        do_list(pkt);
    } else if (op == SMP_OP_WRITE && group == SMP_GROUP_IMG && id == SMP_ID_UPLOAD) {
        do_upload(pkt, body, bn);
    } else if (op == SMP_OP_WRITE && group == SMP_GROUP_OS && id == SMP_ID_RESET) {
        g_mcuboot.smp_resets++;
        cbor_t c = { .n = 0U };
        c.b[c.n++] = 0xA0;
        answer(pkt, &c, g_mcuboot.answer_ms);
        boot(now_us() + ((int64_t)REBOOT_MS * 1000));
    } else {
        answer_rc(pkt, SMP_RC_ENOTSUP, g_mcuboot.answer_ms);
    }
}

/* An image that runs and carries SMP's OS group only, as fck-nrf does: the
 * echo and the reset are answered, anything else refused. */
static void image_handle(const uint8_t *pkt, size_t n)
{
    uint16_t const group = (uint16_t)((pkt[4] << 8) | pkt[5]);
    uint8_t const  id    = pkt[7];
    g_mcuboot.image_frames++;
    if (pkt[0] == SMP_OP_WRITE && group == SMP_GROUP_OS && id == SMP_ID_ECHO) {
        echo_back(pkt, pkt + 8, n - 8U);
    } else if (pkt[0] == SMP_OP_WRITE && group == SMP_GROUP_OS && id == SMP_ID_RESET) {
        cbor_t c = { .n = 0U };
        c.b[c.n++] = 0xA0;
        answer(pkt, &c, g_mcuboot.answer_ms);
        boot(now_us() + ((int64_t)REBOOT_MS * 1000));
    } else {
        answer_rc(pkt, SMP_RC_ENOTSUP, g_mcuboot.answer_ms);
    }
}

static void on_write(int port, const uint8_t *data, size_t n)
{
    if (port != nrf.port) {
        return;
    }
    int64_t const now = now_us();
    advance(now);
    if (nrf.mode == MODE_IMAGE && g_mcuboot.image_smp_os) {
        fck_drv_smp_receive_t irx;
        fck_drv_smp_receive_init(&irx);
        uint8_t ipkt[FCK_DRV_SMP_PACKET_MAX];
        for (size_t k = 0U; k < n; k++) {
            size_t const got = fck_drv_smp_receive_byte(&irx, data[k], ipkt, sizeof(ipkt));
            if (got >= 8U) {
                image_handle(ipkt, got);
            }
        }
        return;
    }
    if (!listening(now)) {
        return;   /* nobody is there: not MCUboot yet, or no longer, or mute */
    }
    fck_drv_smp_receive_t rx;
    fck_drv_smp_receive_init(&rx);
    uint8_t pkt[FCK_DRV_SMP_PACKET_MAX];
    for (size_t k = 0U; k < n; k++) {
        size_t const got = fck_drv_smp_receive_byte(&rx, data[k], pkt, sizeof(pkt));
        if (got >= 8U) {
            handle(pkt, got);
        }
    }
}

/* P6 of the expander is the nRF's reset: low while it is driven low, and the
 * nRF restarts when it is let go. */
static void on_expander(uint8_t reg, uint8_t val)
{
    (void)reg;
    (void)val;
    if (g_mcuboot.pin_dead) {
        return;   /* the reset line does nothing: the nRF goes on as it was */
    }
    bool const low = ((g_idf.exp_reg[3] & P6) == 0U) && ((g_idf.exp_reg[1] & P6) == 0U);
    if (low && !nrf.pin_low) {
        nrf.mode = MODE_RESET;
        (void)memset(nrf.pending, 0, sizeof(nrf.pending));
        nrf.busy_until_us = 0;
    } else if (!low && nrf.pin_low) {
        g_mcuboot.pin_resets++;
        boot(now_us());
    }
    nrf.pin_low = low;
}

void fake_mcuboot_start(int port, const uint8_t *image, size_t len)
{
    (void)memset(&nrf, 0, sizeof(nrf));
    (void)memset(&g_mcuboot, 0, sizeof(g_mcuboot));
    g_mcuboot.ready_ms        = 180U;
    g_mcuboot.window_ms       = 3000U;
    g_mcuboot.answer_ms       = 5U;
    g_mcuboot.list_ms         = 20U;
    g_mcuboot.first_piece_ms  = 800U;
    g_mcuboot.boot_to_line_ms = 6300U;
    g_mcuboot.slot_max        = FAKE_MCUBOOT_SLOT_BYTES;
    g_mcuboot.image_prints    = true;
    g_mcuboot.image_line      = "#NRFINFO fw=fck-nrf ver=2.0.1";
    nrf.port                    = port;
    nrf.mode                    = MODE_IMAGE;   /* it runs what it had until the first reset */
    nrf.line_sent               = true;
    if (image != NULL && len <= sizeof(nrf.slot)) {
        (void)memcpy(nrf.slot, image, len);
        nrf.slot_len = (uint32_t)len;
        nrf.written  = (uint32_t)len;
        nrf.present  = true;
    }
    g_idf.uart_on_write      = on_write;
    g_idf.uart_on_read       = deliver;
    g_idf.uart_virtual_wait  = true;
    g_idf.i2c_on_write       = on_expander;
}
