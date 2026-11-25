/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file fck_subghz.c
 * @brief The CC1101, listening and sending, for an app: fck_subghz_*() in fck.h.
 *
 * Each call but decoding is one job of the owner's that leaves the chips
 * alone (fck_aos_run_job_without_chips()): the module to itself for its
 * length, while the owner goes on reading what the chips say, and the CC1101
 * opened, used and put back to sleep by fck_drv_cc1101.h. The job's own
 * status travels in its arguments, so that what the owner says - no module,
 * or the chips busy - stays apart from what the radio said.
 *
 * The air time (the band's limit, fck_drv_subghz_duty.h) is kept here, in
 * the jobs that send: they run one at a time on the owner's worker, which is the
 * account's only writer, and nothing reaches the radio but through them. A reader
 * from another task (fck_core_subghz_air_time_left(), which never waits) copies it
 * under a sequence count, as a seqlock does.
 *
 * The sub-GHz listening is the owner's (fck_aos_setting_listen(), fck_aos_listen_last()):
 * it listens in slices in the background while it is in force, and the calls here set it
 * and read what it kept.
 */
#include "fck.h"

#include <stdatomic.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "kode.h"

#include "fck_drv_cc1101.h"
#include "fck_drv_ook.h"
#include "fck_aos.h"
#include "fck_core.h"
#include "fck_private.h"
#include "fck_drv_subghz_duty.h"
#include "fck_drv_subghz_file.h"
#include "fck_drv_subghz_scan.h"

_Static_assert(FCK_SUBGHZ_FRAME_RUNS == FCK_DRV_OOK_FRAME_RUNS, "a frame's runs, in both layers");
_Static_assert(FCK_SUBGHZ_TE_MIN_US == FCK_DRV_OOK_TE_MIN_US, "the shortest te, in both layers");
_Static_assert(FCK_SUBGHZ_TE_MAX_US == FCK_DRV_OOK_TE_MAX_US, "the longest te, in both layers");
_Static_assert(FCK_SUBGHZ_FILE_RAW_MAX(0U) == FCK_DRV_SUBGHZ_FILE_RAW_MAX(0U)
                   && FCK_SUBGHZ_FILE_RAW_MAX(4097U) == FCK_DRV_SUBGHZ_FILE_RAW_MAX(4097U),
               "a RAW file's size, in both layers");
_Static_assert(FCK_SUBGHZ_FILE_CODE_MAX == FCK_DRV_SUBGHZ_FILE_CODE_MAX, "a code file's size, in both layers");
_Static_assert(FCK_SUBGHZ_PACKET_MAX == FCK_DRV_SUBGHZ_MODEM_LENGTH_MAX, "a packet's bytes, in both layers");
_Static_assert(FCK_SUBGHZ_FREQUENCIES == FCK_DRV_SUBGHZ_SCAN_REMOTES, "the remotes' frequencies, in both layers");

/* How long a call waits for the commands in flight on both chips to end. */
#define FREE_MS 3000U

typedef struct {
    uint32_t            hz;
    uint32_t            step_hz;
    size_t              count;
    uint32_t            ms;
    fck_drv_cc1101_filter_t filter;
    int16_t            *dbm;
    int32_t            *us;
    size_t              cap;
    size_t             *n;
    fck_subghz_hit_t   *hit;
    fck_subghz_heard_t *heard;
    const fck_drv_subghz_modem_t *modem;
    fck_drv_cc1101_packet_t      *packets;
    uint32_t            mark;      /* the sends: fck_aos_cancel_mark() when the call began */
    bool                cut;       /* the sends: the air was cut short (cancel, leaving)   */
    const uint8_t      *tx_bytes;  /* packet_send: the packet, kept by the caller */
    uint8_t             tx_length;
    uint32_t            air_ms;    /* the sends: what the call puts on air, all repeats */
    const int32_t      *tx_us;    /* send_raw: the runs, kept by the caller */
    size_t              tx_n;     /* send_raw: how many */
    uint8_t             repeats;
    kode_status_t       st;
    const char         *why;      /* a literal: what fck_error() says when st is not KODE_OK */
} job_t;

/* A spectrum's half-dB readings, filled and read inside its job alone: jobs
 * run one at a time, in the owner's task. */
static int16_t s_half[FCK_SUBGHZ_POINTS_MAX];

/* The last hour's air time. Written by the sending jobs alone, which run one at a time on the
 * owner's worker, between duty_write_begin() and _end(); read as a copy by
 * fck_core_subghz_air_time_left(). About 500 bytes. */
static fck_drv_subghz_duty_t s_duty;
static atomic_uint           s_duty_seq;   /* odd while the worker writes */

static void duty_write_begin(void)
{
    (void)atomic_fetch_add_explicit(&s_duty_seq, 1U, memory_order_seq_cst);
}

static void duty_write_end(void)
{
    (void)atomic_fetch_add_explicit(&s_duty_seq, 1U, memory_order_seq_cst);
}

/* A copy of the account that no write was in the middle of; false after DUTY_TRIES tries (the
 * worker is writing that often: it cannot be, for a write is a few instructions). */
#define DUTY_TRIES 64U
static bool duty_copy(fck_drv_subghz_duty_t *out)
{
    for (unsigned i = 0U; i < DUTY_TRIES; i++) {
        unsigned const before = atomic_load_explicit(&s_duty_seq, memory_order_seq_cst);
        if ((before & 1U) == 0U) {
            (void)memcpy(out, &s_duty, sizeof(*out));
            if (atomic_load_explicit(&s_duty_seq, memory_order_seq_cst) == before) {
                return true;
            }
        }
    }
    return false;
}

_Static_assert(FCK_SUBGHZ_SEND_MS_MAX == FCK_DRV_SUBGHZ_SCAN_SEND_MS_MAX, "a call's carrier cap, in both layers");
_Static_assert(FCK_SUBGHZ_SEND_REPEATS_MAX == FCK_DRV_CC1101_SEND_REPEATS_MAX,
               "the send repeats cap, in both layers");

static int16_t dbm_of(int16_t half)
{
    return (half == FCK_DRV_SUBGHZ_SCAN_NO_RECEIVE) ? FCK_SUBGHZ_NO_DBM : (int16_t)(half / 2);
}

/* The packet modem @p s describes; its modulation is checked by the caller. */
static fck_drv_subghz_modem_t modem_of(const fck_subghz_packet_settings_t *s)
{
    return (fck_drv_subghz_modem_t){
        .gfsk         = s->modulation == FCK_SUBGHZ_GFSK,
        .baud         = s->baud,
        .deviation_hz = s->deviation_hz,
        .filter_hz    = s->filter_hz,
        .sync_word    = s->sync_word,
        .length       = s->length,
        .crc          = s->crc,
        .whitening    = s->whitening,
    };
}

static bool filter_of(fck_subghz_bandwidth_t bw, fck_drv_cc1101_filter_t *f)
{
    switch (bw) {
    case FCK_SUBGHZ_WIDE:
        *f = FCK_DRV_CC1101_WIDE;
        return true;
    case FCK_SUBGHZ_NARROW:
        *f = FCK_DRV_CC1101_NARROW;
        return true;
    default:
        return false;
    }
}

/* The public protocols and the driver's, by a switch: a value that is none of
 * the enum's - and KeeLoq and Somfy are not in it - is refused. */
static bool protocol_to_driver(fck_subghz_protocol_t p, fck_drv_ook_protocol_t *d)
{
    switch (p) {
    case FCK_SUBGHZ_EV1527:
        *d = FCK_DRV_OOK_EV1527;
        return true;
    case FCK_SUBGHZ_PT2262:
        *d = FCK_DRV_OOK_PT2262;
        return true;
    case FCK_SUBGHZ_HT12E:
        *d = FCK_DRV_OOK_HT12E;
        return true;
    default:
        return false;
    }
}

static fck_subghz_protocol_t protocol_of_driver(fck_drv_ook_protocol_t d)
{
    switch (d) {
    case FCK_DRV_OOK_PT2262:
        return FCK_SUBGHZ_PT2262;
    case FCK_DRV_OOK_HT12E:
        return FCK_SUBGHZ_HT12E;
    case FCK_DRV_OOK_EV1527:
    default:
        return FCK_SUBGHZ_EV1527;
    }
}

/* A code's driver protocol, when its protocol and its bits are ones the
 * SDK sends. */
static bool code_to_driver(const fck_subghz_code_t *code, fck_drv_ook_protocol_t *d)
{
    return protocol_to_driver(code->proto, d) && fck_drv_ook_bits_ok(*d, code->bits);
}

/* The same, for a code that is to be drawn as runs: its te as well, so that a
 * bad one is refused before send_code allocates anything. */
static bool code_to_runs_driver(const fck_subghz_code_t *code, fck_drv_ook_protocol_t *d)
{
    return code_to_driver(code, d) && fck_drv_ook_te_ok(*d, code->te_us);
}

/* The module the CC1101's calls need: started, there, and its CC1101 answered. */
static kode_status_t module_ready(void)
{
    fck_aos_status_t s;
    fck_aos_status(&s);
    if (!s.started) {
        return fck_fail(KODE_DENIED, "fck_begin() has not run, or fck_end() was called");
    }
    if (!s.present || !s.cc1101_ok) {
        return fck_fail(KODE_DENIED, "no F*CK on J3, or its CC1101 did not answer");
    }
    return KODE_OK;
}

static kode_status_t run(fck_aos_job_fn fn, job_t *j)
{
    kode_status_t const ready = module_ready();
    if (ready != KODE_OK) {
        return ready;
    }
    j->st                  = KODE_ERROR;
    j->cut                 = false;
    j->mark                = fck_aos_cancel_mark();
    kode_status_t const st = fck_aos_run_job_without_chips(fn, j, FREE_MS);
    if (st == KODE_OK && fck_aos_cancelled(j->mark)) {
        return fck_fail(KODE_DENIED, FCK_WHY_CANCELLED);     /* fck_core_cancel() ended it: what it gathered is not given */
    }
    if (st != KODE_OK) {
        /* The owner said why (the chips are busy, the module is gone). */
        return st;
    }
    if (j->cut) {
        /* A send the way home, a rest or fck_end() cut: answered as cancelled, not as a failing chip. */
        return fck_fail(KODE_IO_ERROR, FCK_WHY_CUT);
    }
    if (j->st != KODE_OK) {
        fck_aos_error_set_for(j->st, (j->why != NULL) ? j->why : "the CC1101 refused or failed the request");
    }
    return j->st;
}

/* ---- The jobs: in the owner's task, the caller blocked on its outputs ------------- */

static kode_status_t rssi_job(void *arg, const fck_aos_job_context_t *job)
{
    job_t *const j    = (job_t *)arg;
    int16_t      half = FCK_DRV_SUBGHZ_SCAN_NO_RECEIVE;
    j->st             = fck_drv_cc1101_rssi(job->hw, job->tca, j->hz, j->filter, &half);
    if (j->st == KODE_OK) {
        *j->dbm = dbm_of(half);
    }
    return KODE_OK;
}

static kode_status_t spectrum_job(void *arg, const fck_aos_job_context_t *job)
{
    job_t *const j = (job_t *)arg;
    j->st = fck_drv_cc1101_spectrum(job->hw, job->tca, j->hz, j->step_hz, j->count, j->filter, s_half);
    if (j->st == KODE_OK) {
        for (size_t i = 0U; i < j->count; i++) {
            j->dbm[i] = dbm_of(s_half[i]);
        }
        *j->n = j->count;
    }
    return KODE_OK;
}

static kode_status_t find_job(void *arg, const fck_aos_job_context_t *job)
{
    job_t *const     j = (job_t *)arg;
    fck_drv_cc1101_hit_t h = { .found = false };
    j->st              = fck_drv_cc1101_find(job->hw, job->tca, j->ms, &h);
    if (j->st == KODE_OK) {
        *j->hit = (fck_subghz_hit_t){ .found = false };
        if (h.found) {
            *j->hit = (fck_subghz_hit_t){
                .found     = true,
                .hz        = h.hz,
                .dbm       = dbm_of(h.half),
                .noise_dbm = dbm_of(h.noise_half),
            };
        }
    }
    return KODE_OK;
}

static kode_status_t capture_job(void *arg, const fck_aos_job_context_t *job)
{
    job_t *const j = (job_t *)arg;
    j->st          = fck_drv_cc1101_record(job->hw, job->tca, j->hz, j->ms, j->us, j->cap, j->n);
    return KODE_OK;
}

static kode_status_t packets_job(void *arg, const fck_aos_job_context_t *job)
{
    job_t *const j = (job_t *)arg;
    j->st = fck_drv_cc1101_packets(job->hw, job->tca, j->hz, j->modem, j->ms, j->packets, j->cap, j->n);
    return KODE_OK;
}

/* Charge the air time to the hour unless the driver refused before it
 * touched the chip: an error after that may have put the carrier on, and
 * counts. */
static void duty_charge(kode_status_t st, uint32_t start_ms, const job_t *j)
{
    if ((st != KODE_DENIED) && (st != KODE_NO_RESOURCES)) {
        duty_write_begin();
        (void)fck_drv_subghz_duty_record(&s_duty, start_ms, j->hz, j->air_ms);
        duty_write_end();
    }
}

/* The sends: one job, because the two share the hour's account and its clock, and the
 * account is written here and nowhere else. A packet when the job has a modem, the app's
 * OOK runs when it has not. */
static kode_status_t send_job(void *arg, const fck_aos_job_context_t *job)
{
    job_t *const   j     = (job_t *)arg;
    uint32_t const start = kode_uptime_ms();
    duty_write_begin();
    bool const may = fck_drv_subghz_duty_may(&s_duty, start, j->hz, j->air_ms);
    duty_write_end();
    if (!may) {
        j->st = KODE_DENIED;
        j->why = "the hour's air time for this band is used up";
    } else if ((j->modem == NULL) && fck_drv_ook_rolling(j->tx_us, j->tx_n)) {
        /* The last line: whoever got here, a rolling code's shape is not keyed.
         * The driver cannot say it (a driver does not include another). */
        j->st = KODE_DENIED;
        j->why = "a rolling code's shape is never keyed";
    } else {
        j->st = (j->modem != NULL)
                    ? fck_drv_cc1101_packet_send(job->hw, job->tca, j->hz, j->modem, j->tx_bytes,
                                                 j->tx_length, j->repeats)
                    : fck_drv_cc1101_send(job->hw, job->tca, j->hz, j->tx_us, j->tx_n, j->repeats);
        duty_charge(j->st, start, j);
        if ((j->st != KODE_OK) && fck_drv_cc1101_aborted()) {
            /* The air was cut: by fck_cancel(), or by the way home, a rest or fck_end(). */
            j->cut = !fck_aos_cancelled(j->mark);
            fck_aos_news(FCK_AOS_EVENT_SEND_CANCELLED,
                         fck_aos_cancelled(j->mark) ? FCK_AOS_CANCEL_CALL : FCK_AOS_CANCEL_LEAVING);
        }
    }
    return KODE_OK;
}

/* ---- The calls --------------------------------------------------------------------- */

kode_status_t fck_core_subghz_level(uint32_t hz, fck_subghz_bandwidth_t bw, int16_t *dbm)
{
    job_t j = { .hz = hz, .dbm = dbm };
    if (dbm == NULL || !filter_of(bw, &j.filter)) {
        return fck_fail(KODE_DENIED, "dbm is NULL, or bw is not an fck_subghz_bandwidth_t");
    }
    return run(rssi_job, &j);
}

kode_status_t fck_core_subghz_spectrum(uint32_t first_hz, uint32_t step_hz, uint16_t count,
                                  fck_subghz_bandwidth_t bw, int16_t *dbm, size_t cap, size_t *n)
{
    job_t j = { .hz = first_hz, .step_hz = step_hz, .count = count, .dbm = dbm, .n = n };
    if (dbm == NULL || n == NULL || count == 0U || count > FCK_SUBGHZ_POINTS_MAX || cap < count
        || !filter_of(bw, &j.filter)) {
        return fck_fail(KODE_DENIED, "dbm or n is NULL, count is 0 or above FCK_SUBGHZ_POINTS_MAX, cap is below count, or bw is not an fck_subghz_bandwidth_t");
    }
    return run(spectrum_job, &j);
}

kode_status_t fck_core_subghz_find(uint32_t ms, fck_subghz_hit_t *hit)
{
    job_t j = { .ms = ms, .hit = hit };
    if (hit == NULL || ms > FCK_SUBGHZ_MS_MAX) {
        return fck_fail(KODE_DENIED, "hit is NULL, or ms is above FCK_SUBGHZ_MS_MAX");
    }
    return run(find_job, &j);
}

kode_status_t fck_core_subghz_capture(uint32_t hz, uint32_t ms, int32_t *us, size_t cap, size_t *n)
{
    job_t j = { .hz = hz, .ms = ms, .us = us, .cap = cap, .n = n };
    if (us == NULL || cap == 0U || n == NULL || ms > FCK_SUBGHZ_MS_MAX) {
        return fck_fail(KODE_DENIED, "us or n is NULL, cap is 0, or ms is above FCK_SUBGHZ_MS_MAX");
    }
    return run(capture_job, &j);
}

kode_status_t fck_core_subghz_listen_start(uint32_t hz)
{
    if (hz != FCK_SUBGHZ_HOP && !fck_drv_cc1101_tunes(hz)) {
        return fck_fail(KODE_DENIED, "hz is not a frequency the CC1101 tunes, nor FCK_SUBGHZ_HOP");
    }
    kode_status_t const ready = module_ready();
    if (ready != KODE_OK) {
        return ready;
    }
    /* The owner keeps that it is in force: it listens in the background, puts it out when the
     * module leaves and back when the module is back (FCK_EVENT_SETTING_LOST / _RESTORED). */
    fck_aos_setting_listen(true, hz);
    return KODE_OK;
}

kode_status_t fck_core_subghz_listen_read(uint32_t after, uint32_t ms, int32_t *us, size_t cap, size_t *n,
                                          fck_subghz_heard_t *heard)
{
    if (us == NULL || cap == 0U || n == NULL || heard == NULL || ms > FCK_SUBGHZ_MS_MAX) {
        return fck_fail(KODE_DENIED, "us, n or heard is NULL, cap is 0, or ms is above FCK_SUBGHZ_MS_MAX");
    }
    fck_drv_cc1101_heard_t h   = { .heard = false };
    uint32_t               seq = 0U;
    size_t                 got = 0U;
    kode_status_t const    st  = fck_aos_listen_last(after, ms, us, cap, &got, &h, &seq);
    if (st == KODE_TIMEOUT) {
        /* Nothing newer: a listening that heard nothing is not a failure. */
        *heard = (fck_subghz_heard_t){ .heard = false };
        *n     = 0U;
        return KODE_OK;
    }
    if (st != KODE_OK) {
        return st;
    }
    *n     = got;
    *heard = (fck_subghz_heard_t){
        .heard     = true,
        .hz        = h.hz,
        .dbm       = dbm_of(h.half),
        .noise_dbm = dbm_of(h.noise_half),
        .sequence  = seq,
    };
    return KODE_OK;
}

uint32_t fck_subghz_listen_hz(void)
{
    uint32_t hz = 0U;
    return fck_aos_setting_listening(&hz, NULL) ? hz : 0U;
}

kode_status_t fck_core_subghz_listen_stop(void)
{
    fck_aos_status_t s;
    fck_aos_status(&s);
    if (!s.started) {
        return fck_fail(KODE_DENIED, "fck_begin() has not run, or fck_end() was called");
    }
    /* Forgotten whether or not the module is there: a listening that came back with it
     * is not one the app still wants. */
    fck_aos_setting_listen(false, 0U);
    return KODE_OK;
}

kode_status_t fck_core_subghz_frequencies(uint32_t *hz, size_t cap, size_t *n)
{
    if (hz == NULL || n == NULL || cap < FCK_SUBGHZ_FREQUENCIES) {
        return fck_fail(KODE_DENIED, "hz or n is NULL, or cap is below FCK_SUBGHZ_FREQUENCIES");
    }
    const uint32_t *const list = fck_drv_subghz_scan_remotes(NULL);
    for (size_t i = 0U; i < FCK_SUBGHZ_FREQUENCIES; i++) {
        hz[i] = list[i];
    }
    *n = FCK_SUBGHZ_FREQUENCIES;
    return KODE_OK;
}

kode_status_t fck_core_subghz_decode(const int32_t *us, size_t n, fck_subghz_code_t *out,
                                size_t cap, size_t *got)
{
    if (out == NULL || cap == 0U || got == NULL || (us == NULL && n > 0U)) {
        return fck_fail(KODE_DENIED, "out or got is NULL, cap is 0, or us is NULL with n above 0");
    }
    fck_drv_ook_code_t codes[FCK_DRV_OOK_FRAMES_MAX];
    size_t const   k = fck_drv_ook_decode(us, n, codes,
                                      (cap < FCK_DRV_OOK_FRAMES_MAX) ? cap : FCK_DRV_OOK_FRAMES_MAX);
    for (size_t i = 0U; i < k; i++) {
        out[i] = (fck_subghz_code_t){
            .proto   = protocol_of_driver(codes[i].proto),
            .bits    = codes[i].bits,
            .key     = codes[i].key,
            .te_us   = codes[i].te_us,
            .repeats = codes[i].frames,
        };
    }
    *got = k;
    return KODE_OK;
}

kode_status_t fck_core_subghz_decode_rolling(const int32_t *us, size_t n, fck_subghz_rolling_t *out, size_t cap,
                                        size_t *got)
{
    if (out == NULL || cap == 0U || got == NULL || (us == NULL && n > 0U)) {
        return fck_fail(KODE_DENIED, "out or got is NULL, cap is 0, or us is NULL with n above 0");
    }
    fck_drv_ook_rolling_t codes[FCK_DRV_OOK_FRAMES_MAX];
    size_t const          k = fck_drv_ook_decode_rolling(us, n, codes,
                                                (cap < FCK_DRV_OOK_FRAMES_MAX) ? cap : FCK_DRV_OOK_FRAMES_MAX);
    for (size_t i = 0U; i < k; i++) {
        out[i] = (fck_subghz_rolling_t){
            .proto        = (codes[i].proto == FCK_DRV_OOK_SOMFY_RTS) ? FCK_SUBGHZ_SOMFY_RTS : FCK_SUBGHZ_KEELOQ,
            .bits         = codes[i].bits,
            .data         = codes[i].data,
            .serial       = codes[i].serial,
            .button       = codes[i].button,
            .hop          = codes[i].hop,
            .counter      = codes[i].counter,
            .counter_read = codes[i].counter_read,
            .battery_low  = codes[i].battery_low,
            .te_us        = codes[i].te_us,
            .repeats      = codes[i].frames,
        };
    }
    *got = k;
    return KODE_OK;
}

kode_status_t fck_core_subghz_encode(const fck_subghz_code_t *code, uint8_t frames, int32_t *us,
                                size_t cap, size_t *n)
{
    fck_drv_ook_protocol_t proto;
    if (code == NULL || us == NULL || n == NULL || frames == 0U || frames > FCK_SUBGHZ_FRAMES_MAX
        || !code_to_runs_driver(code, &proto)) {
        return fck_fail(KODE_DENIED, "code, us or n is NULL, frames is 0 or above FCK_SUBGHZ_FRAMES_MAX, or the code is not one the SDK encodes");
    }
    size_t const w = fck_drv_ook_encode(proto, code->bits, code->key, code->te_us, frames, us, cap);
    if (w == 0U) {
        return fck_fail(KODE_DENIED, "the code does not fit in cap runs");
    }
    *n = w;
    return KODE_OK;
}

kode_status_t fck_core_subghz_protocol_info(fck_subghz_protocol_t proto, fck_subghz_protocol_info_t *info)
{
    /* In fck_subghz_protocol_t's order. */
    static char const *const NAMES[] = { "EV1527", "PT2262", "HT12E" };
    static uint8_t const COUNTS[] = { 12U, 24U };
    fck_drv_ook_protocol_t     d;
    fck_subghz_protocol_info_t out = { 0 };
    if ((info == NULL) || !protocol_to_driver(proto, &d)
        || !fck_drv_ook_te_range(d, &out.te_min_us, &out.te_max_us, &out.te_us)) {
        return fck_fail(KODE_DENIED, "info is NULL, or proto is not an fck_subghz_protocol_t");
    }
    out.name         = NAMES[proto];
    out.file_name    = fck_drv_ook_name(d);
    size_t k         = 0U;
    for (size_t i = 0U; i < (sizeof(COUNTS) / sizeof(COUNTS[0])); i++) {
        if (fck_drv_ook_bits_ok(d, COUNTS[i]) && (k < (sizeof(out.bits) / sizeof(out.bits[0])))) {
            out.bits[k] = COUNTS[i];
            k++;
        }
    }
    *info = out;
    return KODE_OK;
}

bool fck_subghz_may_transmit(uint32_t hz)
{
    return fck_drv_subghz_scan_may_transmit(hz);
}

bool fck_subghz_is_rolling(const int32_t *us, size_t n)
{
    return (us != NULL) && (n > 0U) && fck_drv_ook_rolling(us, n);
}

kode_status_t fck_core_subghz_send_raw(uint32_t hz, const int32_t *us, size_t n, uint8_t repeats)
{
    job_t j = { .hz = hz, .tx_us = us, .tx_n = n, .repeats = repeats };
    if (us == NULL || n == 0U || repeats == 0U || repeats > FCK_SUBGHZ_SEND_REPEATS_MAX
        || !fck_drv_subghz_scan_may_transmit(hz)) {
        return fck_fail(KODE_DENIED, "us is NULL, n or repeats is 0, repeats is above FCK_SUBGHZ_SEND_REPEATS_MAX, or hz is not a frequency the SDK transmits on");
    }
    /* A rolling code is never replayed: a capture that reads as one is refused. */
    if (fck_subghz_is_rolling(us, n)) {
        return fck_fail(KODE_DENIED, "the signal reads as a rolling code, which is never replayed");
    }
    /* One call keeps the carrier on for so long (its positive runs, all the
     * repeats) and no longer, checked before the job and again by the driver. */
    if (!fck_drv_subghz_scan_send_fits(fck_drv_subghz_scan_on_air_us(us, n, repeats))) {
        return fck_fail(KODE_DENIED, "the carrier would be on for longer than FCK_SUBGHZ_SEND_MS_MAX in one call");
    }
    /* The hour's account is kept in the whole signal, its off runs too, times
     * the repeats: the chip is in TX for all of it, and an over-count is the
     * safe side. */
    j.air_ms = fck_drv_subghz_duty_signal_ms(us, n, repeats);
    return run(send_job, &j);
}

kode_status_t fck_core_subghz_send_code(uint32_t hz, const fck_subghz_code_t *code, uint8_t frames)
{
    fck_drv_ook_protocol_t proto;
    if (code == NULL || frames == 0U || frames > FCK_SUBGHZ_FRAMES_MAX || !code_to_runs_driver(code, &proto)
        || !fck_drv_subghz_scan_may_transmit(hz)) {
        return fck_fail(KODE_DENIED, "code is NULL, frames is 0 or above FCK_SUBGHZ_FRAMES_MAX, the code is not one the SDK sends, or hz is not a frequency the SDK transmits on");
    }
    size_t const cap = (size_t)frames * FCK_SUBGHZ_FRAME_RUNS;
    /* The code's runs, freed before it returns: a static would keep that much
     * internal RAM from every app, asking to send or not. */
    /* cppcheck-suppress misra-c2012-21.3 ; one allocation for the call, freed before it returns */
    int32_t *const us = malloc(cap * sizeof(int32_t));
    if (us == NULL) {
        return fck_fail(KODE_NO_RESOURCES, "no memory for the code's runs");
    }
    size_t const  n = fck_drv_ook_encode(proto, code->bits, code->key, code->te_us, frames, us, cap);
    kode_status_t st;
    if ((n == 0U) || !fck_drv_subghz_scan_send_fits(fck_drv_subghz_scan_on_air_us(us, n, 1U))) {
        st = fck_fail(KODE_DENIED, "the code does not encode, or its carrier would be on for longer than FCK_SUBGHZ_SEND_MS_MAX");
    } else {
        job_t j = { .hz = hz, .tx_us = us, .tx_n = n, .repeats = 1U };
        j.air_ms = fck_drv_subghz_duty_signal_ms(us, n, 1U);
        st       = run(send_job, &j);
    }
    /* cppcheck-suppress misra-c2012-21.3 ; the allocation above, freed in the same call */
    free(us);
    return st;
}

kode_status_t fck_core_subghz_to_file(uint32_t hz, const int32_t *us, size_t n, char *text, size_t cap,
                                         size_t *length)
{
    if (length == NULL) {
        return fck_fail(KODE_DENIED, "length is NULL");
    }
    size_t const w = fck_drv_subghz_file_write_raw(hz, us, n, text, cap);
    if (w == 0U) {
        return fck_fail(KODE_DENIED, "the signal does not fit in text, or hz or the runs are out of range");
    }
    *length = w;
    return KODE_OK;
}

kode_status_t fck_core_subghz_code_to_file(uint32_t hz, const fck_subghz_code_t *code, char *text, size_t cap,
                                              size_t *length)
{
    fck_drv_ook_protocol_t proto;
    fck_drv_ook_protocol_t again;
    /* What the reader accepts back: the name, the bits and a key the protocol can send (no PT2262 pair 10). */
    if (code == NULL || length == NULL || !code_to_runs_driver(code, &proto)
        || !fck_drv_ook_from_name(fck_drv_ook_name(proto), code->bits, code->key, &again) || again != proto) {
        return fck_fail(KODE_DENIED, "code or length is NULL, or the code is not one the SDK writes, its te included");
    }
    size_t const w = fck_drv_subghz_file_write_code(hz, fck_drv_ook_name(proto), code->bits, code->key,
                                                    code->te_us, text, cap);
    if (w == 0U) {
        return fck_fail(KODE_DENIED, "the code does not fit in text, or hz is 0");
    }
    *length = w;
    return KODE_OK;
}

kode_status_t fck_core_subghz_from_file(const char *text, size_t length, fck_subghz_file_t *file, int32_t *us,
                                           size_t cap, size_t *n)
{
    fck_drv_subghz_file_t f;
    if (file == NULL || n == NULL || !fck_drv_subghz_file_read(text, length, &f, us, cap)) {
        return fck_fail(KODE_DENIED, "file or n is NULL, or the text is not a .ksub file the SDK reads");
    }
    fck_subghz_file_t read = { .hz = f.hz, .cut = f.cut };
    if (f.kind == FCK_DRV_SUBGHZ_FILE_CODE) {
        /* A protocol of the SDK's, with a count of bits it sends and a pulse in its band. */
        fck_drv_ook_protocol_t proto;
        if (!fck_drv_ook_from_name(f.protocol, f.bits, f.code, &proto)) {
            return fck_fail(KODE_DENIED, "the file's protocol, bit count or code is not one the SDK knows");
        }
        if (!fck_drv_ook_te_ok(proto, f.pulse_us)) {
            return fck_fail(KODE_DENIED, "the file has no pulse, or its pulse is outside its protocol's band");
        }
        read.is_code = true;
        read.code    = (fck_subghz_code_t){
               .proto = protocol_of_driver(proto),
               .bits  = f.bits,
               .key   = f.code,
               .te_us = f.pulse_us,
        };
    }
    *file = read;
    *n    = f.runs;
    return KODE_OK;
}

kode_status_t fck_core_subghz_packet_receive(uint32_t hz, const fck_subghz_packet_settings_t *settings,
                                        uint32_t ms, fck_subghz_packet_t *packets, size_t cap,
                                        size_t *got)
{
    if (settings == NULL || packets == NULL || got == NULL || cap == 0U || cap > FCK_SUBGHZ_PACKETS_MAX
        || ms > FCK_SUBGHZ_MS_MAX
        || (settings->modulation != FCK_SUBGHZ_2FSK && settings->modulation != FCK_SUBGHZ_GFSK)) {
        return fck_fail(KODE_DENIED, "settings, packets or got is NULL, cap is 0 or above FCK_SUBGHZ_PACKETS_MAX, ms is above FCK_SUBGHZ_MS_MAX, or the modulation is not 2-FSK or GFSK");
    }
    fck_drv_subghz_modem_t const modem = modem_of(settings);
    fck_drv_subghz_modem_registers_t r;
    if (!fck_drv_subghz_modem_registers(&modem, &r)) {
        return fck_fail(KODE_DENIED, "the settings are not a modem the CC1101 can be set to");
    }
    /* The call's own packets, up to 2.3 KB, freed before it returns: a static
     * buffer would keep that much internal RAM from every app, asking or not. */
    /* cppcheck-suppress misra-c2012-21.3 ; one allocation for the call, freed before it returns */
    fck_drv_cc1101_packet_t *const raw = malloc(cap * sizeof(fck_drv_cc1101_packet_t));
    if (raw == NULL) {
        return fck_fail(KODE_NO_RESOURCES, "no memory for the packets");
    }
    job_t         j  = { .hz = hz, .ms = ms, .cap = cap, .n = got, .modem = &modem, .packets = raw };
    kode_status_t st = run(packets_job, &j);
    if (st == KODE_OK) {
        for (size_t i = 0U; i < *got; i++) {
            packets[i] = (fck_subghz_packet_t){
                .length             = raw[i].length,
                .dbm                = dbm_of(raw[i].status.half),
                .link_quality       = raw[i].status.link_quality,
                .crc_ok             = raw[i].status.crc_ok,
                .frequency_error_hz = raw[i].status.frequency_error_hz,
            };
            (void)memcpy(packets[i].bytes, raw[i].bytes, raw[i].length);
        }
    }
    /* cppcheck-suppress misra-c2012-21.3 ; the allocation above, freed in the same call */
    free(raw);
    return st;
}

kode_status_t fck_core_subghz_packet_send(uint32_t hz, const fck_subghz_packet_settings_t *settings,
                                     const uint8_t *bytes, uint8_t length, uint8_t repeats)
{
    if ((settings == NULL) || (bytes == NULL) || (length == 0U) || (length > FCK_SUBGHZ_PACKET_MAX)
        || (length != settings->length) || (repeats == 0U) || (repeats > FCK_SUBGHZ_SEND_REPEATS_MAX)
        || ((settings->modulation != FCK_SUBGHZ_2FSK) && (settings->modulation != FCK_SUBGHZ_GFSK))
        || !fck_drv_subghz_scan_may_transmit(hz)) {
        return fck_fail(KODE_DENIED, "settings or bytes is NULL, length is 0, above FCK_SUBGHZ_PACKET_MAX or not the settings', repeats is 0 or above FCK_SUBGHZ_SEND_REPEATS_MAX, the modulation is not 2-FSK or GFSK, or hz is not a frequency the SDK transmits on");
    }
    fck_drv_subghz_modem_t const     modem = modem_of(settings);
    fck_drv_subghz_modem_registers_t r;
    uint32_t                         air_us = 0U;
    if (!fck_drv_subghz_modem_registers(&modem, &r) || !fck_drv_subghz_modem_may_transmit(&modem, hz)
        || !fck_drv_subghz_modem_air_time_us(&modem, &air_us)
        || !fck_drv_subghz_scan_send_fits((uint64_t)air_us * repeats)) {
        return fck_fail(KODE_DENIED, "the settings are not a modem the CC1101 can be set to, the signal would not stay in the band, or its carrier would be on for longer than FCK_SUBGHZ_SEND_MS_MAX");
    }
    job_t j = { .hz = hz, .modem = &modem, .tx_bytes = bytes, .tx_length = length, .repeats = repeats };
    /* A packet's air time, rounded up to the millisecond, times the repeats
     * (the pause between them is not on air): at most 32 of 1 s. */
    j.air_ms = ((air_us + 999U) / 1000U) * repeats;
    return run(send_job, &j);
}

kode_status_t fck_core_subghz_air_time_left(uint32_t hz, uint32_t *ms)
{
    if ((ms == NULL) || !fck_drv_subghz_scan_may_transmit(hz)) {
        return fck_fail(KODE_DENIED, "ms is NULL, or hz is not a frequency the SDK transmits on");
    }
    kode_status_t const ready = module_ready();
    if (ready != KODE_OK) {
        return ready;
    }
    fck_drv_subghz_duty_t copy;
    if (!duty_copy(&copy)) {
        return fck_fail(KODE_NO_RESOURCES, "the air time's account was being written: ask again");
    }
    uint32_t left = 0U;
    if (!fck_drv_subghz_duty_left(&copy, kode_uptime_ms(), hz, &left)) {
        return fck_fail(KODE_DENIED, "hz is not a frequency the SDK transmits on");
    }
    *ms = left;
    return KODE_OK;
}
