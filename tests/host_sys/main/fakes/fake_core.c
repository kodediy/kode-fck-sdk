/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file fake_core.c
 * @brief fake_core.h: the core (fck_core.h) as far as fck_sys.c reaches it. One stub for each call,
 *        generated from fck_core.h's prototypes: it writes down its name and arguments, holds while a
 *        case says so (cut by the cancel marks, as the real core's waits are), answers what the case
 *        scripted and, on KODE_OK, writes what the call is lent a buffer for.
 */
#include "fake_core.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "fake_aos.h"
#include "fck_core.h"

fake_core_t g_core;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

void fake_core_reset(void)
{
    taskENTER_CRITICAL(&s_mux);
    (void)memset(&g_core, 0, sizeof(g_core));
    g_core.next    = KODE_OK;
    g_core.why     = FCK_AOS_WHY_REFUSED;
    g_core.present = true;
    g_core.may     = true;
    taskEXIT_CRITICAL(&s_mux);
}

unsigned fake_core_count(const char *name)
{
    unsigned n = 0U;
    taskENTER_CRITICAL(&s_mux);
    for (unsigned i = 0U; i < g_core.calls && i < FAKE_CORE_LOG; i++) {
        if (strcmp(g_core.log[i].name, name) == 0) {
            n++;
        }
    }
    taskEXIT_CRITICAL(&s_mux);
    return n;
}

const fake_call_t *fake_core_call(const char *name, unsigned nth)
{
    const fake_call_t *found = NULL;
    taskENTER_CRITICAL(&s_mux);
    for (unsigned i = 0U; i < g_core.calls && i < FAKE_CORE_LOG; i++) {
        if (strcmp(g_core.log[i].name, name) == 0) {
            if (nth == 0U) {
                found = &g_core.log[i];
                break;
            }
            nth--;
        }
    }
    taskEXIT_CRITICAL(&s_mux);
    return found;
}

/* Written down, held while the case says so, answered. */
static kode_status_t enter(const char *name, bool holds, unsigned n, const uint64_t *args)
{
    taskENTER_CRITICAL(&s_mux);
    unsigned const slot = g_core.calls++;
    if (slot < FAKE_CORE_LOG) {
        g_core.log[slot].name  = name;
        g_core.log[slot].nargs = n;
        for (unsigned i = 0U; i < n && i < FAKE_CORE_ARGS; i++) {
            g_core.log[slot].arg[i] = args[i];
        }
    }
    taskEXIT_CRITICAL(&s_mux);
    if (holds) {
        uint32_t const mark = fck_aos_cancel_mark();
        taskENTER_CRITICAL(&s_mux);
        g_core.inside++;
        taskEXIT_CRITICAL(&s_mux);
        kode_status_t cut = KODE_OK;
        while (g_core.hold && (g_core.hold_name == NULL || strcmp(g_core.hold_name, name) == 0)) {
            if (fck_aos_cancelled(mark)) {
                cut = KODE_DENIED;
                break;
            }
            vTaskDelay(1);
        }
        taskENTER_CRITICAL(&s_mux);
        g_core.inside--;
        taskEXIT_CRITICAL(&s_mux);
        if (cut != KODE_OK) {
            fake_aos_error(cut, FCK_AOS_WHY_CANCELLED, "cancelled by fck_cancel()");
            return cut;
        }
    }
    kode_status_t const st = g_core.next;
    if (st != KODE_OK) {
        fake_aos_error(st, g_core.why, "the core refused, as the test scripted");
    } else {
        fck_aos_error_ok();
    }
    return st;
}

#define A(x) ((uint64_t)(uintptr_t)(x))
#define FILL(p) do { if ((p) != NULL) { (void)memset((p), 0x5A, sizeof(*(p))); } } while (0)

kode_status_t fck_core_begin(void)
{
    kode_status_t const st = enter("begin", false, 0, NULL);
    return st;
}

kode_status_t fck_core_end(void)
{
    kode_status_t const st = enter("end", true, 0, NULL);
    return st;
}

kode_status_t fck_core_cancel(void)
{
    kode_status_t const st = enter("cancel", false, 0, NULL);
    if (st == KODE_OK) {
        fake_aos_cancel();
    }
    return st;
}

kode_status_t fck_core_status(fck_status_t *out)
{
    const uint64_t a[] = { A(out) };
    kode_status_t const st = enter("status", false, 1, a);
    if (st == KODE_OK) {
        FILL(out);
    }
    return st;
}

kode_status_t fck_core_command(fck_chip_t chip, const char *line, const char *verb, fck_line_fn on_line, void *ctx, uint32_t ms, int32_t *rc)
{
    const uint64_t a[] = { A(chip), A(line), A(verb), A(on_line), A(ctx), A(ms), A(rc) };
    (void)snprintf(g_core.cmd_line, sizeof(g_core.cmd_line), "%s", (line != NULL) ? line : "");
    g_core.cmd_has_verb = (verb != NULL);
    (void)snprintf(g_core.cmd_verb, sizeof(g_core.cmd_verb), "%s", (verb != NULL) ? verb : "");
    g_core.cmd_ms = ms;
    kode_status_t const st = enter("command", true, 7, a);
    if (st == KODE_OK) {
        if (rc != NULL) { *rc = g_core.cmd_rc; }
        if (on_line != NULL) {
            for (unsigned i = 0U; i < g_core.n_lines; i++) {
                fck_line_t const line = { .tag = g_core.line[i].tag, .body = g_core.line[i].body };
                on_line(ctx, &line);
            }
        }
    }
    return st;
}

kode_status_t fck_core_reset(fck_chip_t chip)
{
    const uint64_t a[] = { A(chip) };
    kode_status_t const st = enter("reset", true, 1, a);
    return st;
}

kode_status_t fck_core_power(fck_chip_t chip, bool on)
{
    const uint64_t a[] = { A(chip), A(on) };
    kode_status_t const st = enter("power", true, 2, a);
    return st;
}

kode_status_t fck_core_firmware(fck_chip_t chip, fck_firmware_t *out)
{
    const uint64_t a[] = { A(chip), A(out) };
    kode_status_t const st = enter("firmware", true, 2, a);
    if (st == KODE_OK) {
        FILL(out);
        if (out != NULL) { out->known = true; }
    }
    return st;
}

kode_status_t fck_core_flash(fck_chip_t chip, const fck_image_t *img, size_t n, fck_flash_report_t *rep)
{
    const uint64_t a[] = { A(chip), A(img), A(n), A(rep) };
    kode_status_t const st = enter("flash", true, 4, a);
    if (st == KODE_OK) {
        FILL(rep);
    }
    return st;
}

kode_status_t fck_core_rescue_probe(fck_chip_t chip, fck_rescue_report_t *out)
{
    const uint64_t a[] = { A(chip), A(out) };
    kode_status_t const st = enter("rescue_probe", true, 2, a);
    if (st == KODE_OK) {
        FILL(out);
    }
    return st;
}

kode_status_t fck_core_wifi_scan(fck_band_t band, fck_wifi_network_t *out, size_t cap, size_t *n)
{
    const uint64_t a[] = { A(band), A(out), A(cap), A(n) };
    kode_status_t const st = enter("wifi_scan", true, 4, a);
    if (st == KODE_OK) {
        if (n != NULL) { *n = g_core.out_n; }
        size_t k = (g_core.wifi_n < cap) ? g_core.wifi_n : cap;
        for (size_t i = 0U; i < k; i++) {
            (void)memset(&out[i], 0, sizeof(out[i]));
            (void)snprintf(out[i].ssid, sizeof(out[i].ssid), "net%u", (unsigned)i);
            out[i].channel = (uint8_t)(1U + (i % 13U));
        }
        *n = k;
    }
    return st;
}

kode_status_t fck_core_wifi_channels(fck_band_t band, uint16_t dwell_ms, fck_wifi_channel_t *out, size_t cap, size_t *n)
{
    const uint64_t a[] = { A(band), A(dwell_ms), A(out), A(cap), A(n) };
    kode_status_t const st = enter("wifi_channels", true, 5, a);
    if (st == KODE_OK) {
        FILL(out);
        if (n != NULL) { *n = g_core.out_n; }
    }
    return st;
}

kode_status_t fck_core_ble_scan(uint8_t duration_s, fck_ble_device_t *out, size_t cap, size_t *n)
{
    const uint64_t a[] = { A(duration_s), A(out), A(cap), A(n) };
    kode_status_t const st = enter("ble_scan", true, 4, a);
    if (st == KODE_OK) {
        if (n != NULL) { *n = g_core.out_n; }
        size_t k = (g_core.ble_n < cap) ? g_core.ble_n : cap;
        for (size_t i = 0U; i < k; i++) {
            (void)memset(&out[i], 0, sizeof(out[i]));
            (void)snprintf(out[i].name, sizeof(out[i].name), "dev%u", (unsigned)i);
            out[i].addr[5] = (uint8_t)i;
            /* What a 2.2 firmware says of each: a whole advertisement, 31 bytes that tell the
             * devices apart, so an entry that lost a byte of it shows. */
            out[i].advertising = FCK_BLE_ADVERTISING_NOT_CONNECTABLE;
            out[i].first_ms    = 100U * (uint32_t)i;
            out[i].last_ms     = (100U * (uint32_t)i) + 50U;
            out[i].data[0]     = (uint8_t)(FCK_BLE_DATA_MAX - 1U);
            out[i].data[1]     = 0xFFU;
            for (size_t b = 2U; b < FCK_BLE_DATA_MAX; b++) {
                out[i].data[b] = (uint8_t)((i * 16U) + b);
            }
            out[i].data_len = (uint8_t)FCK_BLE_DATA_MAX;
        }
        *n = k;
    }
    return st;
}

kode_status_t fck_core_gnss_begin(void)
{
    kode_status_t const st = enter("gnss_begin", true, 0, NULL);
    return st;
}

kode_status_t fck_core_gnss_read(fck_gnss_fix_t *out, uint32_t ms)
{
    const uint64_t a[] = { A(out), A(ms) };
    kode_status_t const st = enter("gnss_read", false, 2, a);
    if (st == KODE_OK) {
        FILL(out);
    }
    return st;
}

kode_status_t fck_core_gnss_end(void)
{
    kode_status_t const st = enter("gnss_end", true, 0, NULL);
    return st;
}

kode_status_t fck_core_radio_spectrum(uint16_t first_mhz, uint16_t last_mhz, uint32_t dwell_us, uint16_t sweeps, int8_t *dbm, size_t cap, size_t *n)
{
    const uint64_t a[] = { A(first_mhz), A(last_mhz), A(dwell_us), A(sweeps), A(dbm), A(cap), A(n) };
    kode_status_t const st = enter("radio_spectrum", true, 7, a);
    if (st == KODE_OK) {
        FILL(dbm);
        if (n != NULL) { *n = g_core.out_n; }
    }
    return st;
}

kode_status_t fck_core_radio_sniff(uint16_t mhz, uint32_t ms, int8_t rssi_min, fck_radio_frame_t *out, size_t cap, size_t *n, fck_radio_sniff_statistics_t *stats)
{
    const uint64_t a[] = { A(mhz), A(ms), A(rssi_min), A(out), A(cap), A(n), A(stats) };
    kode_status_t const st = enter("radio_sniff", true, 7, a);
    if (st == KODE_OK) {
        FILL(out);
        if (n != NULL) { *n = g_core.out_n; }
        FILL(stats);
    }
    return st;
}

kode_status_t fck_core_radio_sniff_tune_post(const fck_radio_sniff_config_t *cfg)
{
    const uint64_t a[] = { A(cfg) };
    kode_status_t const st = enter("radio_sniff_tune_post", false, 1, a);
    return st;
}

kode_status_t fck_core_radio_ping(uint16_t count, uint16_t mhz, uint16_t gap_ms, int8_t dbm, fck_radio_ping_statistics_t *stats)
{
    const uint64_t a[] = { A(count), A(mhz), A(gap_ms), A(dbm), A(stats) };
    kode_status_t const st = enter("radio_ping", true, 5, a);
    if (st == KODE_OK) {
        FILL(stats);
    }
    return st;
}

kode_status_t fck_core_radio_echo_start(uint16_t mhz, int8_t dbm)
{
    const uint64_t a[] = { A(mhz), A(dbm) };
    kode_status_t const st = enter("radio_echo_start", true, 2, a);
    return st;
}

kode_status_t fck_core_radio_echo_stop(fck_radio_echo_statistics_t *stats)
{
    const uint64_t a[] = { A(stats) };
    kode_status_t const st = enter("radio_echo_stop", true, 1, a);
    if (st == KODE_OK) {
        FILL(stats);
    }
    return st;
}

kode_status_t fck_core_subghz_level(uint32_t hz, fck_subghz_bandwidth_t bw, int16_t *dbm)
{
    const uint64_t a[] = { A(hz), A(bw), A(dbm) };
    kode_status_t const st = enter("subghz_level", true, 3, a);
    if (st == KODE_OK) {
        FILL(dbm);
    }
    return st;
}

kode_status_t fck_core_subghz_spectrum(uint32_t first_hz, uint32_t step_hz, uint16_t count, fck_subghz_bandwidth_t bw, int16_t *dbm, size_t cap, size_t *n)
{
    const uint64_t a[] = { A(first_hz), A(step_hz), A(count), A(bw), A(dbm), A(cap), A(n) };
    kode_status_t const st = enter("subghz_spectrum", true, 7, a);
    if (st == KODE_OK) {
        FILL(dbm);
        if (n != NULL) { *n = g_core.out_n; }
    }
    return st;
}

kode_status_t fck_core_subghz_listen_start(uint32_t hz)
{
    const uint64_t a[] = { A(hz) };
    kode_status_t const st = enter("subghz_listen_start", false, 1, a);
    return st;
}

kode_status_t fck_core_subghz_listen_read(uint32_t after, uint32_t ms, int32_t *us, size_t cap, size_t *n, fck_subghz_heard_t *heard)
{
    const uint64_t a[] = { A(after), A(ms), A(us), A(cap), A(n), A(heard) };
    kode_status_t const st = enter("subghz_listen_read", false, 6, a);
    if (st == KODE_OK) {
        FILL(us);
        if (n != NULL) { *n = g_core.out_n; }
        FILL(heard);
    }
    return st;
}

kode_status_t fck_core_subghz_listen_stop(void)
{
    kode_status_t const st = enter("subghz_listen_stop", false, 0, NULL);
    return st;
}

kode_status_t fck_core_subghz_frequencies(uint32_t *hz, size_t cap, size_t *n)
{
    const uint64_t a[] = { A(hz), A(cap), A(n) };
    kode_status_t const st = enter("subghz_frequencies", false, 3, a);
    if (st == KODE_OK) {
        FILL(hz);
        if (n != NULL) { *n = g_core.out_n; }
    }
    return st;
}

kode_status_t fck_core_subghz_find(uint32_t ms, fck_subghz_hit_t *hit)
{
    const uint64_t a[] = { A(ms), A(hit) };
    kode_status_t const st = enter("subghz_find", true, 2, a);
    if (st == KODE_OK) {
        FILL(hit);
    }
    return st;
}

kode_status_t fck_core_subghz_capture(uint32_t hz, uint32_t ms, int32_t *us, size_t cap, size_t *n)
{
    const uint64_t a[] = { A(hz), A(ms), A(us), A(cap), A(n) };
    kode_status_t const st = enter("subghz_capture", true, 5, a);
    if (st == KODE_OK) {
        FILL(us);
        if (n != NULL) { *n = g_core.out_n; }
    }
    return st;
}

kode_status_t fck_core_subghz_protocol_info(fck_subghz_protocol_t proto, fck_subghz_protocol_info_t *info)
{
    const uint64_t a[] = { A(proto), A(info) };
    kode_status_t const st = enter("subghz_protocol_info", false, 2, a);
    if (st == KODE_OK) {
        FILL(info);
    }
    return st;
}

kode_status_t fck_core_subghz_air_time_left(uint32_t hz, uint32_t *ms)
{
    const uint64_t a[] = { A(hz), A(ms) };
    kode_status_t const st = enter("subghz_air_time_left", false, 2, a);
    if (st == KODE_OK) {
        FILL(ms);
    }
    return st;
}

kode_status_t fck_core_subghz_send_raw(uint32_t hz, const int32_t *us, size_t n, uint8_t repeats)
{
    const uint64_t a[] = { A(hz), A(us), A(n), A(repeats) };
    kode_status_t const st = enter("subghz_send_raw", true, 4, a);
    return st;
}

kode_status_t fck_core_subghz_send_code(uint32_t hz, const fck_subghz_code_t *code, uint8_t frames)
{
    const uint64_t a[] = { A(hz), A(code), A(frames) };
    if (code != NULL) { g_core.code = *code; }
    kode_status_t const st = enter("subghz_send_code", true, 3, a);
    return st;
}

kode_status_t fck_core_subghz_packet_receive(uint32_t hz, const fck_subghz_packet_settings_t *settings, uint32_t ms, fck_subghz_packet_t *packets, size_t cap, size_t *got)
{
    const uint64_t a[] = { A(hz), A(settings), A(ms), A(packets), A(cap), A(got) };
    if (settings != NULL) { g_core.settings = *settings; }
    kode_status_t const st = enter("subghz_packet_receive", true, 6, a);
    if (st == KODE_OK) {
        FILL(packets);
        if (got != NULL) { *got = g_core.out_n; }
    }
    return st;
}

kode_status_t fck_core_subghz_packet_send(uint32_t hz, const fck_subghz_packet_settings_t *settings, const uint8_t *bytes, uint8_t length, uint8_t repeats)
{
    const uint64_t a[] = { A(hz), A(settings), A(bytes), A(length), A(repeats) };
    if (settings != NULL) { g_core.settings = *settings; }
    if (bytes != NULL && length <= sizeof(g_core.bytes)) { (void)memcpy(g_core.bytes, bytes, length); }
    g_core.length = length;
    kode_status_t const st = enter("subghz_packet_send", true, 5, a);
    return st;
}

/* fck.h's own, which fck_sys.c reads without waiting. */
bool fck_is_present(void)
{
    return g_core.present;
}

uint8_t fck_header_position(uint8_t pos)
{
    return (uint8_t)(pos + 1U);
}

uint32_t fck_subghz_listen_hz(void)
{
    return g_core.hz;
}

bool fck_subghz_may_transmit(uint32_t hz)
{
    (void)hz;
    return g_core.may;
}

bool fck_subghz_is_rolling(const int32_t *us, size_t n)
{
    (void)us;
    (void)n;
    return g_core.rolling;
}
