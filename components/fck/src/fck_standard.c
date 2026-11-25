/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file fck_standard.c
 * @brief The standard layer: fck.h's calls, for tasks. Each asks the core (fck_core.h), which
 *        waits for the owner, and answers a bool: true when the core answered KODE_OK.
 *
 * Why a call failed is in fck_error() (the owner records it per task, with the class it fell
 * in), and a call that goes well clears it. A call that blocks and is made from an Active
 * Object's task is refused here, before anything is asked, with the text kode.h gives its own:
 * "<call> was called from an Active Object (<name>); fck.h calls block - call fck_sys_*() instead".
 * The ones that never wait (fck_cancel(), fck_status(), the listening's, the pure helpers) are
 * allowed anywhere; fck_wait_present(), fck_gnss_read() and fck_subghz_listen_read() are, with a
 * wait of 0 ms.
 *
 * Nothing here is new behaviour: the core is the v27 facade, call for call.
 */
#include "fck.h"

#include "kode.h"

#include "fck_aos.h"
#include "fck_core.h"
#include "fck_events.h"
#include "fck_private.h"

/* A call's end: true on KODE_OK, which clears fck_error(); the core recorded why when it was not. */
static bool done(kode_status_t st)
{
    if (st == KODE_OK) {
        fck_aos_error_ok();
        return true;
    }
    return false;
}

bool fck_begin(void)
{
    if (fck_aos_refuse_ao("fck_begin()")) {
        return false;
    }
    /* Idempotent, and waits for the first call to finish: the app need not call it. */
    kode_init();
    if (!fck_events_start()) {
        (void)fck_fail(KODE_NO_RESOURCES, "no memory for the listeners' task and queue");
        return false;
    }
    return done(fck_core_begin());
}

bool fck_end(void)
{
    if (fck_aos_refuse_ao("fck_end()")) {
        return false;
    }
    if (fck_events_in_listener()) {
        (void)fck_fail(KODE_DENIED, "fck_end() was called from a listener: it must not run on the events task");
        return false;
    }
    return done(fck_core_end());
}

bool fck_cancel(void)
{
    return done(fck_core_cancel());
}

bool fck_wait_present(uint32_t ms)
{
    return done(fck_core_wait_present(ms));
}

bool fck_status(fck_status_t *out)
{
    return done(fck_core_status(out));
}

bool fck_command(fck_chip_t chip, const char *line, const char *verb, fck_line_fn on_line, void *ctx, uint32_t ms, int32_t *rc)
{
    if (fck_aos_refuse_ao("fck_command()")) {
        return false;
    }
    return done(fck_core_command(chip, line, verb, on_line, ctx, ms, rc));
}

bool fck_reset(fck_chip_t chip)
{
    if (fck_aos_refuse_ao("fck_reset()")) {
        return false;
    }
    return done(fck_core_reset(chip));
}

bool fck_power(fck_chip_t chip, bool on)
{
    if (fck_aos_refuse_ao("fck_power()")) {
        return false;
    }
    return done(fck_core_power(chip, on));
}

bool fck_firmware(fck_chip_t chip, fck_firmware_t *out)
{
    if (fck_aos_refuse_ao("fck_firmware()")) {
        return false;
    }
    return done(fck_core_firmware(chip, out));
}

bool fck_flash(fck_chip_t chip, const fck_image_t *img, size_t n, fck_flash_report_t *rep)
{
    if (fck_aos_refuse_ao("fck_flash()")) {
        return false;
    }
    return done(fck_core_flash(chip, img, n, rep));
}

bool fck_rescue_probe(fck_chip_t chip, fck_rescue_report_t *out)
{
    if (fck_aos_refuse_ao("fck_rescue_probe()")) {
        return false;
    }
    return done(fck_core_rescue_probe(chip, out));
}

bool fck_wifi_scan(fck_band_t band, fck_wifi_network_t *out, size_t cap, size_t *n)
{
    if (fck_aos_refuse_ao("fck_wifi_scan()")) {
        return false;
    }
    return done(fck_core_wifi_scan(band, out, cap, n));
}

bool fck_wifi_channels(fck_band_t band, uint16_t dwell_ms, fck_wifi_channel_t *out, size_t cap, size_t *n)
{
    if (fck_aos_refuse_ao("fck_wifi_channels()")) {
        return false;
    }
    return done(fck_core_wifi_channels(band, dwell_ms, out, cap, n));
}

bool fck_wifi_frames(uint8_t channel, uint16_t ms, fck_wifi_frame_t *out, size_t cap, size_t *n,
                     fck_wifi_frames_statistics_t *stats)
{
    if (fck_aos_refuse_ao("fck_wifi_frames()")) {
        return false;
    }
    return done(fck_core_wifi_frames(channel, ms, out, cap, n, stats));
}

bool fck_ble_scan(uint8_t duration_s, fck_ble_device_t *out, size_t cap, size_t *n)
{
    if (fck_aos_refuse_ao("fck_ble_scan()")) {
        return false;
    }
    return done(fck_core_ble_scan(duration_s, out, cap, n));
}

bool fck_gnss_begin(void)
{
    if (fck_aos_refuse_ao("fck_gnss_begin()")) {
        return false;
    }
    return done(fck_core_gnss_begin());
}

bool fck_gnss_read(fck_gnss_fix_t *out, uint32_t ms)
{
    return done(fck_core_gnss_read(out, ms));
}

bool fck_gnss_end(void)
{
    if (fck_aos_refuse_ao("fck_gnss_end()")) {
        return false;
    }
    return done(fck_core_gnss_end());
}

bool fck_radio_spectrum(uint16_t first_mhz, uint16_t last_mhz, uint32_t dwell_us, uint16_t sweeps, int8_t *dbm, size_t cap, size_t *n)
{
    if (fck_aos_refuse_ao("fck_radio_spectrum()")) {
        return false;
    }
    return done(fck_core_radio_spectrum(first_mhz, last_mhz, dwell_us, sweeps, dbm, cap, n));
}

bool fck_radio_sniff(uint16_t mhz, uint32_t ms, int8_t rssi_min, fck_radio_frame_t *out, size_t cap, size_t *n, fck_radio_sniff_statistics_t *stats)
{
    if (fck_aos_refuse_ao("fck_radio_sniff()")) {
        return false;
    }
    return done(fck_core_radio_sniff(mhz, ms, rssi_min, out, cap, n, stats));
}

bool fck_radio_sniff_tune(const fck_radio_sniff_config_t *cfg)
{
    if (fck_aos_refuse_ao("fck_radio_sniff_tune()")) {
        return false;
    }
    return done(fck_core_radio_sniff_tune(cfg));
}

bool fck_radio_ping(uint16_t count, uint16_t mhz, uint16_t gap_ms, int8_t dbm, fck_radio_ping_statistics_t *stats)
{
    if (fck_aos_refuse_ao("fck_radio_ping()")) {
        return false;
    }
    return done(fck_core_radio_ping(count, mhz, gap_ms, dbm, stats));
}

bool fck_radio_echo_start(uint16_t mhz, int8_t dbm)
{
    if (fck_aos_refuse_ao("fck_radio_echo_start()")) {
        return false;
    }
    return done(fck_core_radio_echo_start(mhz, dbm));
}

bool fck_radio_echo_stop(fck_radio_echo_statistics_t *stats)
{
    if (fck_aos_refuse_ao("fck_radio_echo_stop()")) {
        return false;
    }
    return done(fck_core_radio_echo_stop(stats));
}

bool fck_subghz_level(uint32_t hz, fck_subghz_bandwidth_t bw, int16_t *dbm)
{
    if (fck_aos_refuse_ao("fck_subghz_level()")) {
        return false;
    }
    return done(fck_core_subghz_level(hz, bw, dbm));
}

bool fck_subghz_spectrum(uint32_t first_hz, uint32_t step_hz, uint16_t count, fck_subghz_bandwidth_t bw, int16_t *dbm, size_t cap, size_t *n)
{
    if (fck_aos_refuse_ao("fck_subghz_spectrum()")) {
        return false;
    }
    return done(fck_core_subghz_spectrum(first_hz, step_hz, count, bw, dbm, cap, n));
}

bool fck_subghz_listen_start(uint32_t hz)
{
    return done(fck_core_subghz_listen_start(hz));
}

bool fck_subghz_listen_read(uint32_t after, uint32_t ms, int32_t *us, size_t cap, size_t *n, fck_subghz_heard_t *heard)
{
    return done(fck_core_subghz_listen_read(after, ms, us, cap, n, heard));
}

bool fck_subghz_listen_stop(void)
{
    return done(fck_core_subghz_listen_stop());
}

bool fck_subghz_frequencies(uint32_t *hz, size_t cap, size_t *n)
{
    return done(fck_core_subghz_frequencies(hz, cap, n));
}

bool fck_subghz_find(uint32_t ms, fck_subghz_hit_t *hit)
{
    if (fck_aos_refuse_ao("fck_subghz_find()")) {
        return false;
    }
    return done(fck_core_subghz_find(ms, hit));
}

bool fck_subghz_capture(uint32_t hz, uint32_t ms, int32_t *us, size_t cap, size_t *n)
{
    if (fck_aos_refuse_ao("fck_subghz_capture()")) {
        return false;
    }
    return done(fck_core_subghz_capture(hz, ms, us, cap, n));
}

bool fck_subghz_decode(const int32_t *us, size_t n, fck_subghz_code_t *out, size_t cap, size_t *got)
{
    return done(fck_core_subghz_decode(us, n, out, cap, got));
}

bool fck_subghz_encode(const fck_subghz_code_t *code, uint8_t frames, int32_t *us, size_t cap, size_t *n)
{
    return done(fck_core_subghz_encode(code, frames, us, cap, n));
}

bool fck_subghz_protocol_info(fck_subghz_protocol_t proto, fck_subghz_protocol_info_t *info)
{
    return done(fck_core_subghz_protocol_info(proto, info));
}

bool fck_subghz_decode_rolling(const int32_t *us, size_t n, fck_subghz_rolling_t *out, size_t cap, size_t *got)
{
    return done(fck_core_subghz_decode_rolling(us, n, out, cap, got));
}

bool fck_subghz_air_time_left(uint32_t hz, uint32_t *ms)
{
    return done(fck_core_subghz_air_time_left(hz, ms));
}

bool fck_subghz_send_raw(uint32_t hz, const int32_t *us, size_t n, uint8_t repeats)
{
    if (fck_aos_refuse_ao("fck_subghz_send_raw()")) {
        return false;
    }
    return done(fck_core_subghz_send_raw(hz, us, n, repeats));
}

bool fck_subghz_send_code(uint32_t hz, const fck_subghz_code_t *code, uint8_t frames)
{
    if (fck_aos_refuse_ao("fck_subghz_send_code()")) {
        return false;
    }
    return done(fck_core_subghz_send_code(hz, code, frames));
}

bool fck_subghz_packet_receive(uint32_t hz, const fck_subghz_packet_settings_t *settings, uint32_t ms, fck_subghz_packet_t *packets, size_t cap, size_t *got)
{
    if (fck_aos_refuse_ao("fck_subghz_packet_receive()")) {
        return false;
    }
    return done(fck_core_subghz_packet_receive(hz, settings, ms, packets, cap, got));
}

bool fck_subghz_packet_send(uint32_t hz, const fck_subghz_packet_settings_t *settings, const uint8_t *bytes, uint8_t length, uint8_t repeats)
{
    if (fck_aos_refuse_ao("fck_subghz_packet_send()")) {
        return false;
    }
    return done(fck_core_subghz_packet_send(hz, settings, bytes, length, repeats));
}

bool fck_subghz_to_file(uint32_t hz, const int32_t *us, size_t n, char *text, size_t cap, size_t *length)
{
    return done(fck_core_subghz_to_file(hz, us, n, text, cap, length));
}

bool fck_subghz_code_to_file(uint32_t hz, const fck_subghz_code_t *code, char *text, size_t cap, size_t *length)
{
    return done(fck_core_subghz_code_to_file(hz, code, text, cap, length));
}

bool fck_subghz_from_file(const char *text, size_t length, fck_subghz_file_t *file, int32_t *us, size_t cap, size_t *n)
{
    return done(fck_core_subghz_from_file(text, length, file, us, cap, n));
}
