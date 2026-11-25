/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file fake_core.c
 * @brief fake_core.h: every call of the core (fck_core.h), written down and answered.
 *        Generated from fck_core.h's prototypes, one stub each: the standard layer
 *        (fck_standard.c) is the only code that calls them here.
 */
#include "fake_core.h"

#include <string.h>

#include "fck_aos.h"
#include "fck_core.h"

fake_core_t g_core;

void fake_core_reset(void)
{
    (void)memset(&g_core, 0, sizeof(g_core));
    g_core.next    = KODE_OK;
    g_core.present = true;
}

static kode_status_t answer(const char *name, unsigned n, const uint64_t *args)
{
    g_core.calls++;
    g_core.last  = name;
    g_core.nargs = n;
    for (unsigned i = 0U; i < n && i < FAKE_CORE_ARGS_MAX; i++) {
        g_core.arg[i] = args[i];
    }
    if (g_core.next != KODE_OK) {
        /* What the real core does on a failure: the reason is the task's, for fck_error(). */
        fck_aos_error_set_for(g_core.next, "the core refused, as the test scripted");
    }
    return g_core.next;
}

#define A(x) ((uint64_t)(uintptr_t)(x))

kode_status_t fck_core_begin(void)
{
    return answer("begin", 0, NULL);
}

kode_status_t fck_core_end(void)
{
    return answer("end", 0, NULL);
}

kode_status_t fck_core_cancel(void)
{
    return answer("cancel", 0, NULL);
}

kode_status_t fck_core_wait_present(uint32_t ms)
{
    const uint64_t a[] = { A(ms) };
    return answer("wait_present", 1, a);
}

kode_status_t fck_core_status(fck_status_t *out)
{
    const uint64_t a[] = { A(out) };
    return answer("status", 1, a);
}

kode_status_t fck_core_command(fck_chip_t chip, const char *line, const char *verb, fck_line_fn on_line, void *ctx, uint32_t ms, int32_t *rc)
{
    const uint64_t a[] = { A(chip), A(line), A(verb), A(on_line), A(ctx), A(ms), A(rc) };
    return answer("command", 7, a);
}

kode_status_t fck_core_reset(fck_chip_t chip)
{
    const uint64_t a[] = { A(chip) };
    return answer("reset", 1, a);
}

kode_status_t fck_core_power(fck_chip_t chip, bool on)
{
    const uint64_t a[] = { A(chip), A(on) };
    return answer("power", 2, a);
}

kode_status_t fck_core_firmware(fck_chip_t chip, fck_firmware_t *out)
{
    const uint64_t a[] = { A(chip), A(out) };
    return answer("firmware", 2, a);
}

kode_status_t fck_core_flash(fck_chip_t chip, const fck_image_t *img, size_t n, fck_flash_report_t *rep)
{
    const uint64_t a[] = { A(chip), A(img), A(n), A(rep) };
    return answer("flash", 4, a);
}

kode_status_t fck_core_rescue_probe(fck_chip_t chip, fck_rescue_report_t *out)
{
    const uint64_t a[] = { A(chip), A(out) };
    return answer("rescue_probe", 2, a);
}

kode_status_t fck_core_wifi_scan(fck_band_t band, fck_wifi_network_t *out, size_t cap, size_t *n)
{
    const uint64_t a[] = { A(band), A(out), A(cap), A(n) };
    return answer("wifi_scan", 4, a);
}

kode_status_t fck_core_wifi_channels(fck_band_t band, uint16_t dwell_ms, fck_wifi_channel_t *out, size_t cap, size_t *n)
{
    const uint64_t a[] = { A(band), A(dwell_ms), A(out), A(cap), A(n) };
    return answer("wifi_channels", 5, a);
}

kode_status_t fck_core_wifi_frames(uint8_t channel, uint16_t ms, fck_wifi_frame_t *out, size_t cap, size_t *n, fck_wifi_frames_statistics_t *stats)
{
    const uint64_t a[] = { A(channel), A(ms), A(out), A(cap), A(n), A(stats) };
    return answer("wifi_frames", 6, a);
}

kode_status_t fck_core_ble_scan(uint8_t duration_s, fck_ble_device_t *out, size_t cap, size_t *n)
{
    const uint64_t a[] = { A(duration_s), A(out), A(cap), A(n) };
    return answer("ble_scan", 4, a);
}

kode_status_t fck_core_gnss_begin(void)
{
    return answer("gnss_begin", 0, NULL);
}

kode_status_t fck_core_gnss_read(fck_gnss_fix_t *out, uint32_t ms)
{
    const uint64_t a[] = { A(out), A(ms) };
    return answer("gnss_read", 2, a);
}

kode_status_t fck_core_gnss_end(void)
{
    return answer("gnss_end", 0, NULL);
}

kode_status_t fck_core_radio_spectrum(uint16_t first_mhz, uint16_t last_mhz, uint32_t dwell_us, uint16_t sweeps, int8_t *dbm, size_t cap, size_t *n)
{
    const uint64_t a[] = { A(first_mhz), A(last_mhz), A(dwell_us), A(sweeps), A(dbm), A(cap), A(n) };
    return answer("radio_spectrum", 7, a);
}

kode_status_t fck_core_radio_sniff(uint16_t mhz, uint32_t ms, int8_t rssi_min, fck_radio_frame_t *out, size_t cap, size_t *n, fck_radio_sniff_statistics_t *stats)
{
    const uint64_t a[] = { A(mhz), A(ms), A(rssi_min), A(out), A(cap), A(n), A(stats) };
    return answer("radio_sniff", 7, a);
}

kode_status_t fck_core_radio_sniff_tune(const fck_radio_sniff_config_t *cfg)
{
    const uint64_t a[] = { A(cfg) };
    return answer("radio_sniff_tune", 1, a);
}

kode_status_t fck_core_radio_sniff_tune_post(const fck_radio_sniff_config_t *cfg)
{
    const uint64_t a[] = { A(cfg) };
    return answer("radio_sniff_tune_post", 1, a);
}

kode_status_t fck_core_radio_ping(uint16_t count, uint16_t mhz, uint16_t gap_ms, int8_t dbm, fck_radio_ping_statistics_t *stats)
{
    const uint64_t a[] = { A(count), A(mhz), A(gap_ms), A(dbm), A(stats) };
    return answer("radio_ping", 5, a);
}

kode_status_t fck_core_radio_echo_start(uint16_t mhz, int8_t dbm)
{
    const uint64_t a[] = { A(mhz), A(dbm) };
    return answer("radio_echo_start", 2, a);
}

kode_status_t fck_core_radio_echo_stop(fck_radio_echo_statistics_t *stats)
{
    const uint64_t a[] = { A(stats) };
    return answer("radio_echo_stop", 1, a);
}

kode_status_t fck_core_subghz_level(uint32_t hz, fck_subghz_bandwidth_t bw, int16_t *dbm)
{
    const uint64_t a[] = { A(hz), A(bw), A(dbm) };
    return answer("subghz_level", 3, a);
}

kode_status_t fck_core_subghz_spectrum(uint32_t first_hz, uint32_t step_hz, uint16_t count, fck_subghz_bandwidth_t bw, int16_t *dbm, size_t cap, size_t *n)
{
    const uint64_t a[] = { A(first_hz), A(step_hz), A(count), A(bw), A(dbm), A(cap), A(n) };
    return answer("subghz_spectrum", 7, a);
}

kode_status_t fck_core_subghz_listen_start(uint32_t hz)
{
    const uint64_t a[] = { A(hz) };
    return answer("subghz_listen_start", 1, a);
}

kode_status_t fck_core_subghz_listen_read(uint32_t after, uint32_t ms, int32_t *us, size_t cap, size_t *n, fck_subghz_heard_t *heard)
{
    const uint64_t a[] = { A(after), A(ms), A(us), A(cap), A(n), A(heard) };
    return answer("subghz_listen_read", 6, a);
}

kode_status_t fck_core_subghz_listen_stop(void)
{
    return answer("subghz_listen_stop", 0, NULL);
}

kode_status_t fck_core_subghz_frequencies(uint32_t *hz, size_t cap, size_t *n)
{
    const uint64_t a[] = { A(hz), A(cap), A(n) };
    return answer("subghz_frequencies", 3, a);
}

kode_status_t fck_core_subghz_find(uint32_t ms, fck_subghz_hit_t *hit)
{
    const uint64_t a[] = { A(ms), A(hit) };
    return answer("subghz_find", 2, a);
}

kode_status_t fck_core_subghz_capture(uint32_t hz, uint32_t ms, int32_t *us, size_t cap, size_t *n)
{
    const uint64_t a[] = { A(hz), A(ms), A(us), A(cap), A(n) };
    return answer("subghz_capture", 5, a);
}

kode_status_t fck_core_subghz_decode(const int32_t *us, size_t n, fck_subghz_code_t *out, size_t cap, size_t *got)
{
    const uint64_t a[] = { A(us), A(n), A(out), A(cap), A(got) };
    return answer("subghz_decode", 5, a);
}

kode_status_t fck_core_subghz_encode(const fck_subghz_code_t *code, uint8_t frames, int32_t *us, size_t cap, size_t *n)
{
    const uint64_t a[] = { A(code), A(frames), A(us), A(cap), A(n) };
    return answer("subghz_encode", 5, a);
}

kode_status_t fck_core_subghz_protocol_info(fck_subghz_protocol_t proto, fck_subghz_protocol_info_t *info)
{
    const uint64_t a[] = { A(proto), A(info) };
    return answer("subghz_protocol_info", 2, a);
}

kode_status_t fck_core_subghz_decode_rolling(const int32_t *us, size_t n, fck_subghz_rolling_t *out, size_t cap, size_t *got)
{
    const uint64_t a[] = { A(us), A(n), A(out), A(cap), A(got) };
    return answer("subghz_decode_rolling", 5, a);
}

kode_status_t fck_core_subghz_air_time_left(uint32_t hz, uint32_t *ms)
{
    const uint64_t a[] = { A(hz), A(ms) };
    return answer("subghz_air_time_left", 2, a);
}

kode_status_t fck_core_subghz_send_raw(uint32_t hz, const int32_t *us, size_t n, uint8_t repeats)
{
    const uint64_t a[] = { A(hz), A(us), A(n), A(repeats) };
    return answer("subghz_send_raw", 4, a);
}

kode_status_t fck_core_subghz_send_code(uint32_t hz, const fck_subghz_code_t *code, uint8_t frames)
{
    const uint64_t a[] = { A(hz), A(code), A(frames) };
    return answer("subghz_send_code", 3, a);
}

kode_status_t fck_core_subghz_packet_receive(uint32_t hz, const fck_subghz_packet_settings_t *settings, uint32_t ms, fck_subghz_packet_t *packets, size_t cap, size_t *got)
{
    const uint64_t a[] = { A(hz), A(settings), A(ms), A(packets), A(cap), A(got) };
    return answer("subghz_packet_receive", 6, a);
}

kode_status_t fck_core_subghz_packet_send(uint32_t hz, const fck_subghz_packet_settings_t *settings, const uint8_t *bytes, uint8_t length, uint8_t repeats)
{
    const uint64_t a[] = { A(hz), A(settings), A(bytes), A(length), A(repeats) };
    return answer("subghz_packet_send", 5, a);
}

kode_status_t fck_core_subghz_to_file(uint32_t hz, const int32_t *us, size_t n, char *text, size_t cap, size_t *length)
{
    const uint64_t a[] = { A(hz), A(us), A(n), A(text), A(cap), A(length) };
    return answer("subghz_to_file", 6, a);
}

kode_status_t fck_core_subghz_code_to_file(uint32_t hz, const fck_subghz_code_t *code, char *text, size_t cap, size_t *length)
{
    const uint64_t a[] = { A(hz), A(code), A(text), A(cap), A(length) };
    return answer("subghz_code_to_file", 5, a);
}

kode_status_t fck_core_subghz_from_file(const char *text, size_t length, fck_subghz_file_t *file, int32_t *us, size_t cap, size_t *n)
{
    const uint64_t a[] = { A(text), A(length), A(file), A(us), A(cap), A(n) };
    return answer("subghz_from_file", 6, a);
}

/* ---- What the core's files also define: the calls that never fail ------------------ */

const char *fck_error(void)
{
    return fck_aos_error();
}

bool fck_is_present(void)
{
    return g_core.present;
}

uint8_t fck_header_position(uint8_t pos)
{
    return g_core.present ? pos : 0U;
}

uint32_t fck_subghz_listen_hz(void)
{
    return g_core.hz;
}

bool fck_subghz_may_transmit(uint32_t hz)
{
    return g_core.may && hz != 0U;
}

bool fck_subghz_is_rolling(const int32_t *us, size_t n)
{
    return g_core.rolling && us != NULL && n > 0U;
}
