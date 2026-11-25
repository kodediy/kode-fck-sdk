/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file fck_gnss.c
 * @brief The GNSS receiver for an app: fck_gnss_*() in fck.h.
 *
 * The receiver is the owner's to start and stop (fck_aos_gnss_power()): its reset is
 * on the expander, and its standard firmware may take the bus. The C5's firmware
 * relays what it says (`gnss on`), and the owner reads each sentence as it
 * arrives and keeps the fix (fck_aos_gnss_fix()). This file asks for the two,
 * in the right order, and hands the fix over in an app's terms.
 *
 * A POSITION IS PRIVATE. Nothing here is logged, at any level, and no sentence
 * or fix is kept past the call that carries it.
 */
#include "fck.h"

#include <stddef.h>
#include <string.h>

#include "fck_drv_c5.h"
#include "fck_drv_nmea.h"
#include "fck_aos.h"
#include "fck_private.h"

/* What a call waits beyond its own commands for a task that holds the C5. */
#define HOLD_MARGIN_MS 1000U

/* One command of the firmware's; its rc as the caller's status. */
static kode_status_t run(uint32_t seq, const fck_drv_c5_command_t *c)
{
    int32_t             rc = -1;
    kode_status_t const st = fck_aos_command_if_same_firmware(FCK_AOS_C5, seq, c->line, c->verb, NULL, NULL,
                                                c->ms, &rc);
    return (st == KODE_OK) ? fck_drv_c5_status(rc) : st;
}

/* The relay on, with the receiver already out of reset. */
static kode_status_t relay_on(const fck_drv_c5_info_t *info, uint32_t seq)
{
    fck_drv_c5_command_t on;
    fck_drv_c5_command_t off;
    if (fck_drv_c5_command_gnss(&on, true) != KODE_OK || fck_drv_c5_command_gnss(&off, false) != KODE_OK) {
        return fck_fail(KODE_ERROR, "the gnss command could not be built");
    }
    kode_status_t st = KODE_OK;
    if (!fck_drv_c5_has(info, FCK_DRV_C5_GNSS_REPEAT)) {
        /* TEMPORARY: pre-production firmware (2.x). Remove when the production modules ship.
         * Before 1.5 a `gnss on` while it is on froze the firmware. A firmware
         * whose caps= has "gnss" promises it does not, and never comes here. */
        st = run(seq, &off);
    }
    return (st == KODE_OK) ? run(seq, &on) : st;
}

kode_status_t fck_core_gnss_begin(void)
{
    /* The C5 is kept for the length of the call, so that no other task's
     * command comes between the two the relay takes. */
    kode_status_t st = fck_aos_hold(FCK_AOS_C5, 2U * FCK_DRV_C5_COMMAND_MS + FCK_FIRMWARE_WAIT_MS
                                                    + HOLD_MARGIN_MS);
    if (st != KODE_OK) {
        return st;
    }
    fck_drv_c5_info_t info;
    uint32_t      seq = 0U;
    st                = fck_firmware_c5(&info, &seq);
    if (st == KODE_OK && !fck_drv_c5_has(&info, FCK_DRV_C5_GNSS_RELAY)) {
        /* Before the receiver is let out of reset: nobody would hear it. */
        st = fck_fail(KODE_DENIED, "the C5's firmware lacks the command: no GNSS relay in what it says it does");
    }
    if (st == KODE_OK) {
        st = fck_aos_gnss_power(true);
        if (st == KODE_OK) {
            st = relay_on(&info, seq);
            if (st != KODE_OK) {
                /* A receiver nobody hears costs energy for nothing. */
                (void)fck_aos_gnss_power(false);
            }
        }
    }
    (void)fck_aos_release(FCK_AOS_C5);
    return st;
}

kode_status_t fck_core_gnss_read(fck_gnss_fix_t *out, uint32_t ms)
{
    if (out == NULL) {
        return fck_fail(KODE_DENIED, "out is NULL");
    }
    fck_drv_nmea_fix_t f;
    uint32_t           age_ms = 0U;
    kode_status_t      st     = fck_aos_gnss_fix(ms, &f, &age_ms);
    if (st == KODE_TIMEOUT && ms == 0U) {
        st = fck_fail(KODE_DENIED, "the receiver has no fix yet, and ms is 0: nothing is waited for");       /* nothing has come yet: nothing to wait for */
    }
    if (st != KODE_OK) {
        return st;
    }
    *out = (fck_gnss_fix_t){
        .quality     = f.quality,
        .sats        = f.sats,
        .valid       = f.valid,
        .lat         = f.lat,
        .lon         = f.lon,
        .alt_m       = f.alt_m,
        .hdop        = f.hdop,
        .speed_kn    = f.speed_kn,
        .course_deg  = f.course_deg,
        .has_time    = f.has_time,
        .hour        = f.hour,
        .min         = f.min,
        .sec         = f.sec,
        .ms          = f.ms,
        .has_date    = f.has_date,
        .day         = f.day,
        .month       = f.month,
        .year        = f.year,
        .age_ms      = age_ms,
    };
    return KODE_OK;
}

kode_status_t fck_core_gnss_end(void)
{
    /* The receiver first: it is what costs energy, and what must not be left
     * running whatever the C5 says. */
    kode_status_t const st = fck_aos_gnss_power(false);
    if (st == KODE_DENIED) {
        return st;     /* no module: no receiver, and no relay to turn off */
    }
    /* The relay, if the C5 is known to speak the SDK's words: asked nothing and
     * waited for nothing, since a C5 that has not said gets nothing sent. */
    fck_aos_firmware_t a;
    if (fck_aos_firmware(FCK_AOS_C5, 0U, &a) == KODE_OK) {
        fck_drv_link_message_t const m = { .tag = "C5INFO", .body = a.body };
        fck_drv_c5_info_t   info;
        fck_drv_c5_command_t    off;
        if (fck_drv_c5_parse_info(&m, &info) && fck_drv_c5_is_compatible(&info)
            && fck_drv_c5_command_gnss(&off, false) == KODE_OK) {
            (void)run(a.seq, &off);
        }
    }
    return st;
}
