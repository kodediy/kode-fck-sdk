/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file fck_radio.c
 * @brief The nRF's radio for an app: fck_radio_*() in fck.h.
 *
 * Each call is one or two of the firmware's commands (fck_drv_nrf.h). The nRF is
 * held for the length of the call (fck_aos_hold()), so no other task's
 * command comes between two of them, and each command is sent only while
 * the nRF still runs the firmware the call was judged by
 * (fck_aos_command_if_same_firmware()). What transmits sets its power first and reads the
 * step the firmware took back before it goes.
 */
#include "fck.h"

#include <stddef.h>
#include <string.h>

#include "fck_drv_nrf.h"
#include "fck_aos.h"
#include "fck_private.h"

/* The limits fck.h promises an app are the ones the drivers hold the chip to: one number, two
 * places, and a compiler that says when they part. */
_Static_assert(FCK_RADIO_RECEIVE_MHZ_MIN == FCK_DRV_NRF_RECEIVE_MHZ_MIN, "fck.h and fck_drv_nrf.h part");
_Static_assert(FCK_RADIO_RECEIVE_MHZ_MAX == FCK_DRV_NRF_RECEIVE_MHZ_MAX, "fck.h and fck_drv_nrf.h part");
_Static_assert(FCK_RADIO_TRANSMIT_MHZ_MIN == FCK_DRV_NRF_TRANSMIT_MHZ_MIN, "fck.h and fck_drv_nrf.h part");
_Static_assert(FCK_RADIO_TRANSMIT_MHZ_MAX == FCK_DRV_NRF_TRANSMIT_MHZ_MAX, "fck.h and fck_drv_nrf.h part");
_Static_assert(FCK_RADIO_TRANSMIT_DBM_MAX == FCK_DRV_NRF_TRANSMIT_DBM_MAX, "fck.h and fck_drv_nrf.h part");
_Static_assert(FCK_RADIO_FRAMES_MAX == FCK_DRV_NRF_FRAMES_MAX, "fck.h and fck_drv_nrf.h part");
_Static_assert(FCK_RADIO_FRAME_MAX == FCK_DRV_NRF_FRAME_MAX, "fck.h and fck_drv_nrf.h part");
_Static_assert(FCK_RADIO_QUIET_DBM == FCK_DRV_NRF_RSSI_QUIET, "fck.h and fck_drv_nrf.h part");

/* What a call waits beyond its own time for a task that holds the nRF. */
#define HOLD_MARGIN_MS 1000U

/* The longest one command of a long call runs. fck_core_radio_spectrum(),
 * fck_core_radio_sniff() and fck_core_radio_ping() are made of commands of about this long,
 * one after another, and fck_core_cancel() is heard between two: the chip is not
 * asked to stop a command it has begun. */
#define SLICE_MS 1000U

/* The different frames of a sniff, kept while the nRF is held: one call at a
 * time fills and reads them, and 2 KB is too much for a caller's stack. */
static fck_drv_nrf_frames_t s_frames;

/* ---- Every call's start and end -------------------------------------------------- */

/* The nRF held, and the firmware on it: the typed calls speak only its words,
 * and only those its #NRFINFO says it has (fck_drv_nrf_has()). */
static kode_status_t take(uint32_t ms, fck_drv_nrf_feature_t need, uint32_t *seq)
{
    kode_status_t st = fck_aos_hold(FCK_AOS_NRF, ms + FCK_FIRMWARE_WAIT_MS + HOLD_MARGIN_MS);
    if (st != KODE_OK) {
        return st;
    }
    fck_aos_firmware_t a;
    st = fck_firmware_wait(FCK_AOS_NRF, &a);
    if (st == KODE_OK) {
        fck_drv_link_message_t const m = { .tag = "NRFINFO", .body = a.body };
        fck_drv_nrf_info_t  info;
        if (!fck_drv_nrf_parse_info(&m, &info) || !fck_drv_nrf_is_compatible(&info)) {
            st = fck_fail(KODE_DENIED, "the nRF's firmware does not speak the SDK's words (no caps= in its #NRFINFO): the typed calls are not sent to it");
        } else if (!fck_drv_nrf_has(&info, need)) {
            st = fck_fail(KODE_DENIED, "the nRF's firmware lacks the command: its caps= does not list it");
        }
        *seq = a.seq;
    }
    if (st != KODE_OK) {
        (void)fck_aos_release(FCK_AOS_NRF);
    }
    return st;
}

static kode_status_t give(kode_status_t st)
{
    (void)fck_aos_release(FCK_AOS_NRF);
    return st;
}

/* One command; its rc as the caller's status. */
static kode_status_t run(uint32_t seq, const fck_drv_nrf_command_t *c, fck_drv_link_message_fn fn, void *ctx,
                         int32_t *rc_out)
{
    int32_t             rc = -1;
    kode_status_t const st = fck_aos_command_if_same_firmware(FCK_AOS_NRF, seq, c->line, c->verb, fn, ctx,
                                                c->ms, &rc);
    if (rc_out != NULL) {
        *rc_out = rc;
    }
    return (st == KODE_OK) ? fck_drv_nrf_status(rc) : st;
}

/* ---- The power, before anything transmits ------------------------------------------ */

typedef struct {
    bool            got;
    fck_drv_nrf_power_t p;
} power_t;

static void on_power(void *ctx, const fck_drv_link_message_t *m)
{
    power_t *const x = ctx;
    if (fck_drv_nrf_parse_power(m, &x->p)) {
        x->got = true;
    }
}

/* The firmware snaps a level down to a step of the nRF54L15's. A step above
 * what was asked, or none said, and nothing is transmitted. */
static kode_status_t power(uint32_t seq, const fck_drv_nrf_command_t *c, int8_t asked, int8_t *step)
{
    power_t       x  = { 0 };
    kode_status_t st = run(seq, c, on_power, &x, NULL);
    if (st == KODE_OK && (!x.got || x.p.dbm > asked || x.p.dbm > FCK_DRV_NRF_TRANSMIT_DBM_MAX)) {
        st = fck_fail(KODE_IO_ERROR, "the firmware took a power step above the level asked, or did not say which");
    }
    if (st == KODE_OK && step != NULL) {
        *step = x.p.dbm;
    }
    return st;
}

/* ---- Listening ---------------------------------------------------------------------- */

typedef struct {
    uint16_t first;
    size_t   n;
    int8_t   dbm[FCK_DRV_NRF_SPECTRUM_MAX];
} spec_t;

static void on_spec(void *ctx, const fck_drv_link_message_t *m)
{
    spec_t *const  x = ctx;
    fck_drv_nrf_spectrum_t sweep;
    if (fck_drv_nrf_parse_spectrum(m, &sweep)) {
        fck_drv_nrf_spectrum_peak(&sweep, x->first, x->dbm, x->n);
    }
}

kode_status_t fck_core_radio_spectrum(uint16_t first_mhz, uint16_t last_mhz, uint32_t dwell_us,
                              uint16_t sweeps, int8_t *dbm, size_t cap, size_t *n)
{
    fck_drv_nrf_command_t c;
    if (dbm == NULL || last_mhz < first_mhz || (size_t)(last_mhz - first_mhz) + 1U > cap
        || fck_drv_nrf_command_spectrum(&c, first_mhz, last_mhz, dwell_us, sweeps) != KODE_OK) {
        return fck_fail(KODE_DENIED, "dbm is NULL, the range is reversed, outside 2400 to 2500 MHz or larger than cap, or dwell_us or sweeps is out of range");
    }
    spec_t x = { .first = first_mhz, .n = (size_t)(last_mhz - first_mhz) + 1U };
    for (size_t i = 0U; i < x.n; i++) {
        x.dbm[i] = FCK_DRV_NRF_RSSI_QUIET;
    }
    uint32_t      seq  = 0U;
    uint32_t const mark = fck_aos_cancel_mark();
    kode_status_t st   = take(c.ms, FCK_DRV_NRF_SPECTRUM, &seq);
    if (st != KODE_OK) {
        return st;
    }
    /* Sweeps to a slice: the builder's time for the whole, less its slack, over the sweeps. */
    uint32_t const per    = (c.ms > FCK_DRV_NRF_SLACK_MS) ? ((c.ms - FCK_DRV_NRF_SLACK_MS) / sweeps) : 1U;
    uint32_t       slice  = SLICE_MS / ((per > 0U) ? per : 1U);
    slice                 = (slice < 1U) ? 1U : slice;
    uint16_t       left   = sweeps;
    while (st == KODE_OK && left > 0U) {
        if (fck_aos_cancelled(mark)) {
            st = fck_fail(KODE_DENIED, FCK_WHY_CANCELLED);
            break;
        }
        uint16_t const        k = (left < slice) ? left : (uint16_t)slice;
        fck_drv_nrf_command_t part;
        st = (fck_drv_nrf_command_spectrum(&part, first_mhz, last_mhz, dwell_us, k) == KODE_OK)
                 ? run(seq, &part, on_spec, &x, NULL)
                 : fck_fail(KODE_DENIED, "the spectrum command could not be built");
        left = (uint16_t)(left - k);
    }
    st = give(st);
    if (st == KODE_OK) {
        memcpy(dbm, x.dbm, x.n);
        if (n != NULL) {
            *n = x.n;
        }
    }
    return st;
}

typedef struct {
    bool                 done;
    fck_drv_nrf_sniff_done_t sum;
} sniff_t;

static void on_sniff(void *ctx, const fck_drv_link_message_t *m)
{
    sniff_t *const  x = ctx;
    fck_drv_nrf_frame_t f;
    if (fck_drv_nrf_parse_frame(m, &f)) {
        (void)fck_drv_nrf_frames_add(&s_frames, &f);
    } else if (fck_drv_nrf_parse_sniff_done(m, &x->sum)) {
        x->done = true;
    }
}

/* The frames one slice heard, into the caller's table: a frame it has is counted again and
 * keeps the strongest RSSI; a new one takes a free place, or the place of the least repeated if
 * it was heard more. */
static size_t fold(fck_radio_frame_t *out, size_t cap, size_t k)
{
    for (uint8_t i = 0U; i < s_frames.n; i++) {
        fck_drv_nrf_frame_t const *const f = &s_frames.f[i];
        size_t                           j = 0U;
        while (j < k && !(out[j].len == f->len && memcmp(out[j].data, f->data, f->len) == 0)) {
            j++;
        }
        if (j < k) {
            uint32_t const seen = (uint32_t)out[j].seen + f->cnt;
            out[j].seen = (seen > UINT16_MAX) ? UINT16_MAX : (uint16_t)seen;
            out[j].rssi = (f->rssi > out[j].rssi) ? f->rssi : out[j].rssi;
            continue;
        }
        if (k < cap) {
            j = k++;
        } else {
            size_t weakest = 0U;
            for (size_t w = 1U; w < k; w++) {
                weakest = (out[w].seen < out[weakest].seen) ? w : weakest;
            }
            if (f->cnt <= out[weakest].seen) {
                continue;
            }
            j = weakest;
        }
        out[j].len = f->len;
        memcpy(out[j].data, f->data, f->len);
        out[j].rssi = f->rssi;
        out[j].seen = f->cnt;
    }
    return k;
}

/* Most repeated first; equal ones stay in the order they came. */
static void by_seen(fck_radio_frame_t *out, size_t k)
{
    for (size_t i = 1U; i < k; i++) {
        fck_radio_frame_t const x = out[i];
        size_t                  j = i;
        while (j > 0U && out[j - 1U].seen < x.seen) {
            out[j] = out[j - 1U];
            j--;
        }
        out[j] = x;
    }
}

kode_status_t fck_core_radio_sniff(uint16_t mhz, uint32_t ms, int8_t rssi_min, fck_radio_frame_t *out,
                           size_t cap, size_t *n, fck_radio_sniff_statistics_t *stats)
{
    fck_drv_nrf_command_t c;
    if (out == NULL || cap == 0U || fck_drv_nrf_command_sniff(&c, mhz, ms, rssi_min) != KODE_OK) {
        return fck_fail(KODE_DENIED, "out is NULL, cap is 0, or mhz, ms or rssi_min is out of range");
    }
    uint32_t      seq  = 0U;
    uint32_t const mark = fck_aos_cancel_mark();
    kode_status_t st   = take(c.ms, FCK_DRV_NRF_SNIFF, &seq);
    if (st != KODE_OK) {
        return st;
    }
    /* In slices of SLICE_MS, the last one taking what is left if it would be under the
     * firmware's shortest sniff. What each heard is put together here. */
    size_t   k        = 0U;
    uint32_t left     = ms;
    uint32_t heard    = 0U;
    uint32_t weak     = 0U;
    uint32_t took     = 0U;
    uint8_t  distinct = 0U;
    bool     sliced   = false;
    while (st == KODE_OK && left > 0U) {
        if (fck_aos_cancelled(mark)) {
            st = fck_fail(KODE_DENIED, FCK_WHY_CANCELLED);
            break;
        }
        uint32_t const        part_ms = (left >= (SLICE_MS + FCK_DRV_NRF_SNIFF_MIN_MS)) ? SLICE_MS : left;
        fck_drv_nrf_command_t part;
        if (fck_drv_nrf_command_sniff(&part, mhz, part_ms, rssi_min) != KODE_OK) {
            st = fck_fail(KODE_DENIED, "the sniff command could not be built");
            break;
        }
        memset(&s_frames, 0, sizeof(s_frames));
        sniff_t x = { 0 };
        st = run(seq, &part, on_sniff, &x, NULL);
        if (st == KODE_OK) {
            k      = fold(out, cap, k);
            heard += x.sum.rx;
            weak  += x.sum.gated;
            took  += x.sum.ms;
            uint8_t const u = x.done ? x.sum.uniq : s_frames.n;
            distinct        = (u > distinct) ? u : distinct;
            sliced          = sliced || (part_ms != ms);
        }
        left -= part_ms;
    }
    if (st == KODE_OK) {
        by_seen(out, k);
        if (n != NULL) {
            *n = k;
        }
        if (stats != NULL) {
            *stats = (fck_radio_sniff_statistics_t){
                .heard    = heard,
                .weak     = weak,
                .distinct = (sliced && k > distinct) ? (uint8_t)k : distinct,
                .ms       = took,
            };
        }
    }
    return give(st);
}

kode_status_t fck_core_radio_sniff_tune(const fck_radio_sniff_config_t *cfg)
{
    if (cfg == NULL) {
        return fck_fail(KODE_DENIED, "cfg is NULL");
    }
    fck_drv_nrf_sniff_config_t const  phy  = { .rate_mbps = cfg->rate_mbps, .crc_bytes = cfg->crc_bytes,
                                   .len = cfg->len };
    fck_drv_nrf_sniff_address_t const addr = { .balen = cfg->base_len, .base = cfg->base,
                                   .prefix = cfg->prefix };
    fck_drv_nrf_command_t         a;
    fck_drv_nrf_command_t         b;
    if (fck_drv_nrf_command_sniff_config(&a, &phy) != KODE_OK || fck_drv_nrf_command_sniff_address(&b, &addr) != KODE_OK) {
        return fck_fail(KODE_DENIED, "the sniff configuration is out of range");
    }
    uint32_t      seq = 0U;
    kode_status_t st  = take(a.ms + b.ms, FCK_DRV_NRF_SNIFF, &seq);
    if (st != KODE_OK) {
        return st;
    }
    st = run(seq, &a, NULL, NULL, NULL);
    if (st == KODE_OK) {
        st = run(seq, &b, NULL, NULL, NULL);
    }
    return give(st);
}

/* The two lines of fck_core_radio_sniff_tune(), posted to the owner and not waited for: the extended
 * layer's command. The owner sends the second when the first is answered. Both set the sniffer and
 * nothing transmits; each is idempotent, so a repeat after a full queue is harmless. */
kode_status_t fck_core_radio_sniff_tune_post(const fck_radio_sniff_config_t *cfg)
{
    if (cfg == NULL) {
        return fck_fail(KODE_DENIED, "cfg is NULL");
    }
    fck_drv_nrf_sniff_config_t const  phy  = { .rate_mbps = cfg->rate_mbps, .crc_bytes = cfg->crc_bytes,
                                   .len = cfg->len };
    fck_drv_nrf_sniff_address_t const addr = { .balen = cfg->base_len, .base = cfg->base,
                                   .prefix = cfg->prefix };
    fck_drv_nrf_command_t         a;
    fck_drv_nrf_command_t         b;
    if (fck_drv_nrf_command_sniff_config(&a, &phy) != KODE_OK || fck_drv_nrf_command_sniff_address(&b, &addr) != KODE_OK) {
        return fck_fail(KODE_DENIED, "the sniff configuration is out of range");
    }
    /* The typed calls go only to a firmware that speaks their words: the nRF must have said what it runs. */
    fck_aos_firmware_t ag;
    if (fck_aos_firmware(FCK_AOS_NRF, 0U, &ag) != KODE_OK || !ag.known) {
        return fck_fail(KODE_DENIED, "the nRF has not said what it runs yet: ask for it with fck_sys_firmware() first");
    }
    fck_drv_link_message_t const m = { .tag = "NRFINFO", .body = ag.body };
    fck_drv_nrf_info_t  info;
    if (!fck_drv_nrf_parse_info(&m, &info) || !fck_drv_nrf_is_compatible(&info)) {
        return fck_fail(KODE_DENIED, "the nRF's firmware does not speak the SDK's words (no caps= in its #NRFINFO): the typed calls are not sent to it");
    }
    if (!fck_drv_nrf_has(&info, FCK_DRV_NRF_SNIFF)) {
        return fck_fail(KODE_DENIED, "the nRF's firmware lacks the command: its caps= does not list it");
    }
    kode_status_t st = fck_aos_command_post(FCK_AOS_NRF, a.line, a.verb, a.ms);
    if (st == KODE_OK) {
        st = fck_aos_command_post(FCK_AOS_NRF, b.line, b.verb, b.ms);
    }
    return st;
}

/* ---- Transmitting ------------------------------------------------------------------- */

typedef struct {
    bool                done;
    fck_drv_nrf_ping_done_t sum;
} ping_t;

static void on_ping(void *ctx, const fck_drv_link_message_t *m)
{
    ping_t *const x = ctx;
    if (fck_drv_nrf_parse_ping_done(m, &x->sum)) {
        x->done = true;
    }
}

/* The stats of one more slice of a ping, put with what the earlier ones gave. */
static void ping_add(fck_drv_nrf_ping_done_t *all, const fck_drv_nrf_ping_done_t *d)
{
    if (d->ok > 0U) {
        uint32_t const had = all->ok;
        uint32_t const now = had + d->ok;
        all->fwd_mean_x10 = (int16_t)(((int32_t)all->fwd_mean_x10 * (int32_t)had
                                       + (int32_t)d->fwd_mean_x10 * (int32_t)d->ok) / (int32_t)now);
        all->rev_mean_x10 = (int16_t)(((int32_t)all->rev_mean_x10 * (int32_t)had
                                       + (int32_t)d->rev_mean_x10 * (int32_t)d->ok) / (int32_t)now);
        if (had == 0U) {
            all->fwd_min = d->fwd_min;
            all->fwd_max = d->fwd_max;
            all->rev_min = d->rev_min;
            all->rev_max = d->rev_max;
        } else {
            all->fwd_min = (d->fwd_min < all->fwd_min) ? d->fwd_min : all->fwd_min;
            all->fwd_max = (d->fwd_max > all->fwd_max) ? d->fwd_max : all->fwd_max;
            all->rev_min = (d->rev_min < all->rev_min) ? d->rev_min : all->rev_min;
            all->rev_max = (d->rev_max > all->rev_max) ? d->rev_max : all->rev_max;
        }
        all->peer     = d->peer;
        all->peer_dbm = d->peer_dbm;
    }
    all->sent     = (uint16_t)(all->sent + d->sent);
    all->ok       = (uint16_t)(all->ok + d->ok);
    all->timeout  = (uint16_t)(all->timeout + d->timeout);
    all->crcbad   = (uint16_t)(all->crcbad + d->crcbad);
    all->mismatch = (uint16_t)(all->mismatch + d->mismatch);
    all->txfail   = (uint16_t)(all->txfail + d->txfail);
    all->dbm      = d->dbm;
}

kode_status_t fck_core_radio_ping(uint16_t count, uint16_t mhz, uint16_t gap_ms, int8_t dbm,
                          fck_radio_ping_statistics_t *stats)
{
    fck_drv_nrf_command_t pw;
    fck_drv_nrf_command_t pg;
    if (stats == NULL || fck_drv_nrf_command_power(&pw, dbm) != KODE_OK
        || fck_drv_nrf_command_ping(&pg, count, mhz, gap_ms) != KODE_OK) {
        return fck_fail(KODE_DENIED, "stats is NULL, or dbm, count, mhz or gap_ms is out of range or outside the EU transmit band");
    }
    uint32_t       seq  = 0U;
    uint32_t const mark = fck_aos_cancel_mark();
    kode_status_t  st   = take(pw.ms + pg.ms, FCK_DRV_NRF_PING, &seq);
    if (st != KODE_OK) {
        return st;
    }
    fck_drv_nrf_ping_done_t all = { 0 };
    st = power(seq, &pw, dbm, NULL);
    /* Packets to a slice: the builder's time for all of them, less its slack, over the count. */
    uint32_t const per   = (pg.ms > FCK_DRV_NRF_SLACK_MS) ? ((pg.ms - FCK_DRV_NRF_SLACK_MS) / count) : 1U;
    uint32_t       slice = SLICE_MS / ((per > 0U) ? per : 1U);
    slice                = (slice < 1U) ? 1U : slice;
    uint16_t       left  = count;
    while (st == KODE_OK && left > 0U) {
        if (fck_aos_cancelled(mark)) {
            st = fck_fail(KODE_DENIED, FCK_WHY_CANCELLED);
            break;
        }
        uint16_t const        k = (left < slice) ? left : (uint16_t)slice;
        fck_drv_nrf_command_t part;
        if (fck_drv_nrf_command_ping(&part, k, mhz, gap_ms) != KODE_OK) {
            st = fck_fail(KODE_DENIED, "the ping command could not be built");
            break;
        }
        /* No echo at all is the firmware's rc 1, and still an answer: its
         * summary says so. A #DONE without one is not. */
        ping_t  x  = { 0 };
        int32_t rc = -1;
        st = run(seq, &part, on_ping, &x, &rc);
        if (x.done && (st == KODE_OK || (st == KODE_IO_ERROR && rc == 1))) {
            st = KODE_OK;
            ping_add(&all, &x.sum);
        } else if (st == KODE_OK) {
            st = fck_fail(KODE_IO_ERROR, "the firmware ended the ping without its summary");
        }
        left = (uint16_t)(left - k);
    }
    if (st == KODE_OK) {
        fck_drv_nrf_ping_done_t const *const d = &all;
        *stats = (fck_radio_ping_statistics_t){
            .sent         = d->sent,
            .ok           = d->ok,
            .fwd_mean_x10 = d->fwd_mean_x10,
            .rev_mean_x10 = d->rev_mean_x10,
            .fwd_min      = d->fwd_min,
            .fwd_max      = d->fwd_max,
            .rev_min      = d->rev_min,
            .rev_max      = d->rev_max,
            .dbm          = d->dbm,
            .timeout      = d->timeout,
            .crc_bad      = d->crcbad,
            .mismatch     = d->mismatch,
            .tx_fail      = d->txfail,
            .peer         = d->peer,
            .peer_dbm     = d->peer_dbm,
        };
    }
    return give(st);
}

typedef struct {
    bool           got;
    fck_drv_nrf_echo_t e;
} echo_t;

static void on_echo(void *ctx, const fck_drv_link_message_t *m)
{
    echo_t *const x = ctx;
    if (fck_drv_nrf_parse_echo(m, &x->e)) {
        x->got = true;
    }
}

kode_status_t fck_core_radio_echo_start(uint16_t mhz, int8_t dbm)
{
    fck_drv_nrf_command_t pw;
    fck_drv_nrf_command_t on;
    fck_drv_nrf_command_t off;
    if (fck_drv_nrf_command_power(&pw, dbm) != KODE_OK || fck_drv_nrf_command_echo(&on, true, mhz) != KODE_OK
        || fck_drv_nrf_command_off(&off) != KODE_OK) {
        return fck_fail(KODE_DENIED, "dbm or mhz is out of range or outside the EU transmit band");
    }
    uint32_t      seq = 0U;
    kode_status_t st  = take(pw.ms + on.ms + off.ms, FCK_DRV_NRF_ECHO, &seq);
    if (st != KODE_OK) {
        return st;
    }
    st = power(seq, &pw, dbm, NULL);
    if (st == KODE_OK) {
        echo_t x = { 0 };
        st = run(seq, &on, on_echo, &x, NULL);
        if (st == KODE_TIMEOUT) {
            /* The firmware may have started the responder and not said so.
             * The `rf off` waits for the link to give up on the #DONE that
             * did not come - as long again as `rf echo on` had - and goes;
             * a responder left on keeps the power and channel checked
             * above. */
            off.ms += on.ms;
            (void)run(seq, &off, NULL, NULL, NULL);
        } else if (st == KODE_OK && (!x.got || !x.e.on || x.e.dbm > dbm)) {
            /* Not the responder that was asked for: none at all, then. */
            (void)run(seq, &off, NULL, NULL, NULL);
            st = fck_fail(KODE_IO_ERROR, "the firmware did not confirm the responder that was asked for");
        }
    }
    return give(st);
}

typedef struct {
    bool                got;
    fck_drv_nrf_echo_statistics_t s;
} echo_stat_t;

static void on_echo_stat(void *ctx, const fck_drv_link_message_t *m)
{
    echo_stat_t *const x = ctx;
    if (fck_drv_nrf_parse_echo_statistics(m, &x->s)) {
        x->got = true;
    }
}

kode_status_t fck_core_radio_echo_stop(fck_radio_echo_statistics_t *stats)
{
    fck_drv_nrf_command_t stop;
    fck_drv_nrf_command_t off;
    if (fck_drv_nrf_command_echo(&stop, false, 0U) != KODE_OK || fck_drv_nrf_command_off(&off) != KODE_OK) {
        return fck_fail(KODE_DENIED, "the echo commands could not be built");
    }
    uint32_t      seq = 0U;
    kode_status_t st  = take(stop.ms + off.ms, FCK_DRV_NRF_ECHO, &seq);
    if (st != KODE_OK) {
        return st;
    }
    echo_stat_t x = { 0 };
    st = run(seq, &stop, on_echo_stat, &x, NULL);
    /* `rf off` whatever the first said: the radio idle is the point. */
    kode_status_t const idle = run(seq, &off, NULL, NULL, NULL);
    if (st == KODE_OK) {
        st = idle;
    }
    if (st == KODE_OK && stats != NULL) {
        *stats = (fck_radio_echo_statistics_t){
            .heard   = x.got ? x.s.rx : 0U,
            .crc_bad = x.got ? x.s.crcbad : 0U,
            .replied = x.got ? x.s.replied : 0U,
            .foreign = x.got ? x.s.foreign : 0U,
            .tx_slow = x.got ? x.s.txslow : 0U,
            .idle    = x.got && x.s.idle,
        };
    }
    return give(st);
}
