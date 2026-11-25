/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file fck.c
 * @brief The facade. See fck.h. Each call is the owner's, in an app's words.
 */
#include "fck.h"

#include <stddef.h>
#include <string.h>

#include "fck_drv_c5.h"
#include "fck_drv_nrf.h"
#include "fck_aos.h"
#include "fck_private.h"

/* A command's reader, on the caller's stack for as long as the call blocks. */
typedef struct {
    fck_line_fn fn;
    void       *ctx;
} relay_t;

static void relay(void *ctx, const fck_drv_link_message_t *msg)
{
    relay_t const *const r    = (relay_t const *)ctx;
    fck_line_t const     line = { .tag = msg->tag, .body = msg->body };
    r->fn(r->ctx, &line);
}

static fck_aos_chip_t own_chip(fck_chip_t chip)
{
    return (chip == FCK_NRF) ? FCK_AOS_NRF : FCK_AOS_C5;
}

static bool chip_ok(fck_chip_t chip)
{
    return chip == FCK_C5 || chip == FCK_NRF;
}

kode_status_t fck_firmware_wait(fck_aos_chip_t chip, fck_aos_firmware_t *out)
{
    return fck_aos_firmware(chip, FCK_FIRMWARE_WAIT_MS, out);
}

/* ---- What a chip runs --------------------------------------------------------------- */

static bool given(const uint8_t *bytes, size_t n)
{
    for (size_t i = 0U; i < n; i++) {
        if (bytes[i] != 0U) {
            return true;
        }
    }
    return false;
}

void fck_firmware_fill(fck_aos_chip_t chip, const fck_aos_firmware_t *in, fck_firmware_t *out)
{
    *out = (fck_firmware_t){ 0 };
    if (!in->known) {
        return;
    }
    out->known = true;
    if (chip == FCK_AOS_C5) {
        fck_drv_link_message_t const m = { .tag = "C5INFO", .body = in->body };
        fck_drv_c5_info_t   i;
        if (!fck_drv_c5_parse_info(&m, &i)) {
            return;
        }
        _Static_assert(sizeof(out->fw) == sizeof(i.fw), "fck_firmware_t.fw holds a #C5INFO's fw");
        memcpy(out->fw, i.fw, sizeof(out->fw));
        out->compatible  = fck_drv_c5_is_compatible(&i);
        out->official    = fck_drv_c5_is_official(i.fw);
        out->has_version = i.has_version;
        out->ver_major   = i.ver_major;
        out->ver_minor   = i.ver_minor;
        out->ver_patch   = i.ver_patch;
        (void)fck_drv_link_message_text(&m, "caps", out->caps, sizeof(out->caps), NULL);
        if (given(i.mac, sizeof(i.mac))) {
            memcpy(out->id, i.mac, sizeof(i.mac));
            out->id_len = (uint8_t)sizeof(i.mac);
        }
    } else {
        fck_drv_link_message_t const m = { .tag = "NRFINFO", .body = in->body };
        fck_drv_nrf_info_t  i;
        if (!fck_drv_nrf_parse_info(&m, &i)) {
            return;
        }
        _Static_assert(sizeof(out->fw) == sizeof(i.fw), "fck_firmware_t.fw holds a #NRFINFO's fw");
        memcpy(out->fw, i.fw, sizeof(out->fw));
        out->compatible  = fck_drv_nrf_is_compatible(&i);
        out->official    = fck_drv_nrf_is_official(i.fw);
        out->has_version = i.has_version;
        out->ver_major   = i.ver_major;
        out->ver_minor   = i.ver_minor;
        out->ver_patch   = i.ver_patch;
        (void)fck_drv_link_message_text(&m, "caps", out->caps, sizeof(out->caps), NULL);
        if (i.devid != 0U) {
            for (size_t k = 0U; k < sizeof(out->id); k++) {
                out->id[k] = (uint8_t)(i.devid >> (8U * (sizeof(out->id) - 1U - k)));
            }
            out->id_len = (uint8_t)sizeof(out->id);
        }
    }
}

kode_status_t fck_core_firmware(fck_chip_t chip, fck_firmware_t *out)
{
    if (!chip_ok(chip) || out == NULL) {
        return fck_fail(KODE_DENIED, "chip is not FCK_C5 or FCK_NRF, or out is NULL");
    }
    fck_aos_firmware_t     a;
    kode_status_t const st = fck_firmware_wait(own_chip(chip), &a);
    if (st != KODE_OK) {
        return st;
    }
    fck_firmware_fill(own_chip(chip), &a, out);
    return KODE_OK;
}

kode_status_t fck_firmware_c5(fck_drv_c5_info_t *info, uint32_t *seq)
{
    fck_aos_firmware_t     a;
    kode_status_t const st = fck_firmware_wait(FCK_AOS_C5, &a);
    if (st != KODE_OK) {
        return st;
    }
    fck_drv_link_message_t const m = { .tag = "C5INFO", .body = a.body };
    if (!fck_drv_c5_parse_info(&m, info) || !fck_drv_c5_is_compatible(info)) {
        return fck_fail(KODE_DENIED, "the C5's firmware does not speak the SDK's words (no caps= in its #C5INFO): the typed calls are not sent to it");
    }
    *seq = a.seq;
    return KODE_OK;
}

/* ---- The module ------------------------------------------------------------- */

kode_status_t fck_core_begin(void)
{
    return fck_aos_start();
}

kode_status_t fck_core_end(void)
{
    return fck_aos_stop();
}

const char *fck_error(void)
{
    return fck_aos_error();
}

kode_status_t fck_core_cancel(void)
{
    return fck_aos_cancel();
}

bool fck_is_present(void)
{
    return fck_aos_present();
}

kode_status_t fck_core_wait_present(uint32_t ms)
{
    return fck_aos_wait_present(ms);
}

kode_status_t fck_core_status(fck_status_t *out)
{
    if (out == NULL) {
        return fck_fail(KODE_DENIED, "out is NULL");
    }
    fck_aos_status_t st;
    fck_aos_status(&st);
    *out = (fck_status_t){
        .present        = st.present,
        .generation     = st.generation,
        .c5_on          = st.chip_on[FCK_AOS_C5],
        .nrf_on         = st.chip_on[FCK_AOS_NRF],
        .cc1101_ok      = st.cc1101_ok,
        .cc1101_part    = st.cc1101_part,
        .cc1101_version = st.cc1101_version,
        .gnss_running   = st.gnss_running,
        .listening      = st.listening,
        .j3_supply      = st.j3_supply,
        .events_dropped = st.events_dropped,
        .news_dropped   = st.news_dropped,
        .answers_dropped = st.answers_dropped,
    };
    return KODE_OK;
}

/* cppcheck-suppress misra-c2012-8.7 ; an app's call, declared in fck.h: the SDK itself never makes it */
uint8_t fck_header_position(uint8_t pos)
{
    return fck_aos_header_position(pos);
}

/* What a chip runs decides how a line reaches it: the
 * firmware's own words only, checked - for the nRF by fck_drv_nrf_gate(), for the C5
 * by fck_drv_c5_gate(), each keeping the radio inside the radio limits whatever the
 * firmware says of itself; another firmware's as written; nothing to a chip that
 * has not said, since one that does not keep the limits itself and whose
 * banner was lost would otherwise be sent a line unchecked. The owner sends
 * it only while the chip still runs what was judged
 * (fck_aos_command_if_same_firmware()). */
kode_status_t fck_core_command(fck_chip_t chip, const char *line, const char *verb,
                          fck_line_fn on_line, void *ctx, uint32_t ms, int32_t *rc)
{
    if (!chip_ok(chip) || line == NULL) {
        return fck_fail(KODE_DENIED, "chip is not FCK_C5 or FCK_NRF, or line is NULL");
    }
    fck_aos_firmware_t     a;
    kode_status_t const st = fck_firmware_wait(own_chip(chip), &a);
    if (st != KODE_OK) {
        return st;
    }
    /* What a gate builds is what goes out, so it outlives the branch. */
    fck_drv_nrf_command_t nc;
    fck_drv_c5_command_t  cc;
    if (chip == FCK_NRF) {
        fck_drv_link_message_t const m = { .tag = "NRFINFO", .body = a.body };
        fck_drv_nrf_info_t  info;
        switch (fck_drv_nrf_gate(fck_drv_nrf_parse_info(&m, &info) ? &info : NULL, line, verb, &nc)) {
        case FCK_DRV_NRF_GATE_OFFICIAL:
            /* The vocabulary fck_drv_nrf_command() takes keeps the radio inside the
             * module's limits whatever the firmware says of itself. */
            line = nc.line;
            verb = nc.verb;
            ms   = (ms != 0U) ? ms : nc.ms;
            break;
        case FCK_DRV_NRF_GATE_AS_IS:
            break;
        default:
            return fck_fail(KODE_DENIED, "the line is refused: the chip has not said what it runs, or the nRF firmware does not take it within the radio limits");
        }
    } else {
        fck_drv_link_message_t const m = { .tag = "C5INFO", .body = a.body };
        fck_drv_c5_info_t   info;
        switch (fck_drv_c5_gate(fck_drv_c5_parse_info(&m, &info) ? &info : NULL, line, verb, &cc)) {
        case FCK_DRV_C5_GATE_OFFICIAL:
            /* The firmware's words, and `scan` only with its radio limits
             * in force, as its #C5INFO says them (reg=EU and lim=ok). */
            line = cc.line;
            verb = cc.verb;
            ms   = (ms != 0U) ? ms : cc.ms;
            break;
        case FCK_DRV_C5_GATE_AS_IS:
            break;
        default:
            return fck_fail(KODE_DENIED, "the line is refused: the chip has not said what it runs, or the C5 firmware does not take it within the radio limits");
        }
    }
    relay_t r = { .fn = on_line, .ctx = ctx };
    return fck_aos_command_if_same_firmware(own_chip(chip), a.seq, line, verb,
                              (on_line != NULL) ? relay : NULL, &r, ms, rc);
}

kode_status_t fck_core_reset(fck_chip_t chip)
{
    return chip_ok(chip) ? fck_aos_reset(own_chip(chip))
                         : fck_fail(KODE_DENIED, "chip is not FCK_C5 or FCK_NRF");
}

kode_status_t fck_core_power(fck_chip_t chip, bool on)
{
    return chip_ok(chip) ? fck_aos_power(own_chip(chip), on)
                         : fck_fail(KODE_DENIED, "chip is not FCK_C5 or FCK_NRF");
}
