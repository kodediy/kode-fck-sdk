/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file fake_cc1101.c
 * @brief The CC1101 the sub-GHz facade (fck_subghz.c) calls, as the model has
 *        it: the two sends are recorded and answer what a case says, and
 *        nothing else reaches a radio - the rest refuses.
 *
 * Also the clock the facade asks kode-sdk for, kode_sys_time_ms(): the model's own
 * plus a skip a case can add, so that a case can let an hour go by without
 * waiting for it. The sends are jobs of the owner's: they run in its worker,
 * and what is recorded here is what that job saw.
 */
#include "fake_world.h"

#include <string.h>

#include "kode_sys.h"
#include "kode.h"

#include "fck_drv_cc1101.h"

sim_cc1101_t sim_cc1101;

static uint32_t s_skip_ms;

static volatile bool s_abort;

void fck_drv_cc1101_abort(void)
{
    s_abort = true;
}

void fck_drv_cc1101_abort_clear(void)
{
    s_abort = false;
}

bool fck_drv_cc1101_aborted(void)
{
    return s_abort;
}

static int32_t       s_hear_us[SIM_CC1101_HEAR_RUNS_MAX];
static size_t        s_hear_n;
static uint32_t      s_hear_hz;

void sim_cc1101_reset(void)
{
    s_hear_n = 0U;
    (void)memset(&sim_cc1101, 0, sizeof(sim_cc1101));
    sim_cc1101.st = KODE_OK;
    s_abort       = false;
}

/* The carrier of a send: on for play_ms, in 5 ms steps, or until the driver is
 * told to leave. Returns whether it was cut. */
static bool play(void)
{
    sim_cc1101.on_air      = true;
    sim_cc1101.began_at_ms = sim_now_ms();
    bool cut               = false;
    for (uint32_t t = 0U; t < sim_cc1101.play_ms; t += 5U) {
        if (s_abort) {
            cut = true;
            break;
        }
        sim_sleep(5U);
    }
    sim_cc1101.on_air     = false;
    sim_cc1101.idle_at_ms = sim_now_ms();
    if (cut) {
        sim_cc1101.cut++;
    }
    return cut;
}

void sim_clock_skip_ms(uint32_t ms)
{
    s_skip_ms += ms;
}

uint32_t kode_sys_time_ms(void)
{
    return sim_now_ms() + s_skip_ms;
}

uint32_t kode_uptime_ms(void)
{
    return kode_sys_time_ms();
}

void kode_init(void)
{
}

/* ---- What a send reaches ------------------------------------------------------------ */

kode_status_t fck_drv_cc1101_send(const fck_drv_pins_t *hw, fck_drv_tca6408a_t *tca, uint32_t hz,
                                  const int32_t *runs, size_t n, uint8_t repeats)
{
    (void)hw;
    (void)tca;
    if (s_abort) {
        return KODE_DENIED;     /* leaving: nothing is keyed, as the driver's */
    }
    sim_cc1101.sends++;
    sim_cc1101.hz      = hz;
    sim_cc1101.runs    = n;
    sim_cc1101.repeats = repeats;
    (void)memcpy(sim_cc1101.run, runs, ((n < SIM_CC1101_RUNS_MAX) ? n : SIM_CC1101_RUNS_MAX) * sizeof(runs[0]));
    return play() ? KODE_IO_ERROR : sim_cc1101.st;
}

kode_status_t fck_drv_cc1101_packet_send(const fck_drv_pins_t *hw, fck_drv_tca6408a_t *tca, uint32_t hz,
                                         const fck_drv_subghz_modem_t *modem, const uint8_t *bytes,
                                         uint8_t length, uint8_t repeats)
{
    (void)hw;
    (void)tca;
    if (s_abort) {
        return KODE_DENIED;
    }
    sim_cc1101.packet_sends++;
    sim_cc1101.hz      = hz;
    sim_cc1101.repeats = repeats;
    sim_cc1101.length  = length;
    sim_cc1101.baud    = modem->baud;
    (void)memcpy(sim_cc1101.bytes, bytes, length);
    return play() ? KODE_IO_ERROR : sim_cc1101.st;
}

/* ---- What nothing here listens with ------------------------------------------------- */

/* The CC1101's three bands, roughly: enough for a case to start a listening on 433.92 MHz. */
bool fck_drv_cc1101_tunes(uint32_t hz)
{
    return hz >= 300000000U && hz <= 928000000U;
}

/* A read that works hears nothing and takes up to 50 ms, in steps; the way home cuts it. */
static kode_status_t quiet_read(uint32_t ms)
{
    if (!sim_cc1101.reads_ok) {
        return KODE_DENIED;
    }
    uint32_t const left = (ms < 50U) ? ms : 50U;
    for (uint32_t t = 0U; t < left; t += 5U) {
        if (s_abort) {
            return KODE_IO_ERROR;
        }
        sim_sleep(5U);
    }
    return s_abort ? KODE_IO_ERROR : KODE_OK;
}

kode_status_t fck_drv_cc1101_rssi(const fck_drv_pins_t *hw, fck_drv_tca6408a_t *tca, uint32_t hz,
                                  fck_drv_cc1101_filter_t f, int16_t *half)
{
    (void)hw; (void)tca; (void)hz; (void)f;
    kode_status_t const st = quiet_read(5U);
    if (st == KODE_OK) {
        *half = -200;
    }
    return st;
}

kode_status_t fck_drv_cc1101_spectrum(const fck_drv_pins_t *hw, fck_drv_tca6408a_t *tca, uint32_t first_hz,
                                      uint32_t step_hz, size_t count, fck_drv_cc1101_filter_t f, int16_t *half)
{
    (void)hw; (void)tca; (void)first_hz; (void)step_hz; (void)f;
    kode_status_t const st = quiet_read(5U);
    if (st == KODE_OK) {
        for (size_t i = 0U; i < count; i++) {
            half[i] = -200;
        }
    }
    return st;
}

kode_status_t fck_drv_cc1101_find(const fck_drv_pins_t *hw, fck_drv_tca6408a_t *tca, uint32_t ms,
                                  fck_drv_cc1101_hit_t *hit)
{
    (void)hw; (void)tca;
    kode_status_t const st = quiet_read(ms);
    if (st == KODE_OK) {
        *hit = (fck_drv_cc1101_hit_t){ .found = false };
    }
    return st;
}

kode_status_t fck_drv_cc1101_record(const fck_drv_pins_t *hw, fck_drv_tca6408a_t *tca, uint32_t hz, uint32_t ms,
                                    int32_t *runs, size_t cap, size_t *n)
{
    (void)hw; (void)tca; (void)hz; (void)runs; (void)cap;
    kode_status_t const st = quiet_read(ms);
    if (st == KODE_OK) {
        *n = 0U;
    }
    return st;
}

void fck_drv_cc1101_listen_start(fck_drv_cc1101_listen_t *l, uint32_t hz)
{
    (void)memset(l, 0, sizeof(*l));
    l->hold = fck_drv_subghz_scan_hold_start(hz);
    sim_cc1101.listen_starts++;
    sim_cc1101.listen_hz = hz;
}

uint32_t fck_drv_cc1101_listen_now(void)
{
    return 0U;
}

/* A capture the next slice will hear, once (sim_cc1101_hear()). */

void sim_cc1101_hear(const int32_t *us, size_t n, uint32_t hz)
{
    if (n > SIM_CC1101_HEAR_RUNS_MAX) {
        n = SIM_CC1101_HEAR_RUNS_MAX;
    }
    (void)memcpy(s_hear_us, us, n * sizeof(us[0]));
    s_hear_hz = hz;
    s_hear_n  = n;
}

kode_status_t fck_drv_cc1101_listen(const fck_drv_pins_t *hw, fck_drv_tca6408a_t *tca,
                                    fck_drv_cc1101_listen_t *l, uint32_t ms, int32_t *runs, size_t cap,
                                    size_t *n, fck_drv_cc1101_heard_t *heard)
{
    (void)hw; (void)tca; (void)l; (void)ms;
    sim_cc1101.listens++;
    /* A slice takes its time (the driver's takes up to ms): 20 ms of the model's clock. */
    sim_sleep(20U);
    *n     = 0U;
    *heard = (fck_drv_cc1101_heard_t){ .heard = false };
    if (s_hear_n > 0U) {
        size_t const k = (s_hear_n < cap) ? s_hear_n : cap;
        (void)memcpy(runs, s_hear_us, k * sizeof(runs[0]));
        *n     = k;
        *heard = (fck_drv_cc1101_heard_t){ .heard = true, .hz = s_hear_hz, .half = -80, .noise_half = -120 };
        s_hear_n = 0U;
    }
    return KODE_OK;
}

kode_status_t fck_drv_cc1101_packets(const fck_drv_pins_t *hw, fck_drv_tca6408a_t *tca, uint32_t hz,
                                     const fck_drv_subghz_modem_t *modem, uint32_t ms,
                                     fck_drv_cc1101_packet_t *out, size_t cap, size_t *got)
{
    (void)hw; (void)tca; (void)hz; (void)modem; (void)out; (void)cap;
    kode_status_t const st = quiet_read(ms);
    if (st == KODE_OK) {
        *got = 0U;
    }
    return st;
}
