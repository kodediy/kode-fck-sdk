/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file fck_core.h
 * @brief The facade's core: every call of the module, in kode-sdk's own words (kode_status_t).
 *
 * Both layers an app can use are built on this and nothing else:
 *
 *   fck.h      the standard layer, for tasks: fck_standard.c asks the core, turns its
 *              kode_status_t into a bool and keeps fck_error()
 *   fck_sys.h  the extended layer, for Active Objects: fck_sys.c carries each request out
 *              on a task of its own through the core, and posts what it answered to the
 *              asking Active Object
 *
 * It is not an app's header. fck_core_<x>() is fck_<x>() as the v27 API had it, and each
 * documents itself in fck.h under that name: what it validates, what it waits for and what
 * it answers. Every one BLOCKS (but fck_core_cancel(), _status(), the pure helpers and
 * fck_core_subghz_listen_start() / _stop() / _air_time_left()), writes none of its outputs when it does not
 * answer KODE_OK, and records why it failed for fck_error() with fck_aos_error_set_for().
 *
 * The tests of the owner (tests/host_owner) make these calls over the real owner and a
 * model of the module.
 *
 * It sits among the public headers because fck_sys is another component, and compiles only
 * under FCK_CORE_ALLOWED, which `fck` and `fck_sys` define for their own sources and
 * tools/check_layers.sh (rule 13) keeps to those two - as kode-sdk does with kode_sys_internal.h.
 */
#ifndef FCK_CORE_H_
#define FCK_CORE_H_

#ifndef FCK_CORE_ALLOWED
#error "fck_core.h is the SDK's own: an app includes fck.h (tasks) or fck_sys.h (Active Objects)"
#endif

#include "kode_defs.h"

#include "fck.h"

#ifdef __cplusplus
extern "C" {
#endif

kode_status_t fck_core_begin(void);

kode_status_t fck_core_end(void);

kode_status_t fck_core_cancel(void);

kode_status_t fck_core_wait_present(uint32_t ms);

kode_status_t fck_core_status(fck_status_t *out);

kode_status_t fck_core_command(fck_chip_t chip, const char *line, const char *verb, fck_line_fn on_line, void *ctx, uint32_t ms, int32_t *rc);

kode_status_t fck_core_reset(fck_chip_t chip);

kode_status_t fck_core_power(fck_chip_t chip, bool on);

kode_status_t fck_core_firmware(fck_chip_t chip, fck_firmware_t *out);

kode_status_t fck_core_flash(fck_chip_t chip, const fck_image_t *img, size_t n, fck_flash_report_t *rep);

kode_status_t fck_core_rescue_probe(fck_chip_t chip, fck_rescue_report_t *out);

kode_status_t fck_core_wifi_scan(fck_band_t band, fck_wifi_network_t *out, size_t cap, size_t *n);

kode_status_t fck_core_wifi_channels(fck_band_t band, uint16_t dwell_ms, fck_wifi_channel_t *out, size_t cap, size_t *n);

kode_status_t fck_core_wifi_frames(uint8_t channel, uint16_t ms, fck_wifi_frame_t *out, size_t cap, size_t *n, fck_wifi_frames_statistics_t *stats);

kode_status_t fck_core_ble_scan(uint8_t duration_s, fck_ble_device_t *out, size_t cap, size_t *n);

kode_status_t fck_core_gnss_begin(void);

kode_status_t fck_core_gnss_read(fck_gnss_fix_t *out, uint32_t ms);

kode_status_t fck_core_gnss_end(void);

kode_status_t fck_core_radio_spectrum(uint16_t first_mhz, uint16_t last_mhz, uint32_t dwell_us, uint16_t sweeps, int8_t *dbm, size_t cap, size_t *n);

kode_status_t fck_core_radio_sniff(uint16_t mhz, uint32_t ms, int8_t rssi_min, fck_radio_frame_t *out, size_t cap, size_t *n, fck_radio_sniff_statistics_t *stats);

kode_status_t fck_core_radio_sniff_tune(const fck_radio_sniff_config_t *cfg);

/**
 * @brief fck_core_radio_sniff_tune() without the wait: its two lines are posted to the owner and the
 *        call returns at once (the extended layer's command). KODE_OK both were taken; KODE_DENIED
 *        for a value out of range, with no module, or an nRF that has not said it runs the official firmware;
 *        KODE_NO_RESOURCES when the owner's queue had no room (the first line may have gone).
 */
kode_status_t fck_core_radio_sniff_tune_post(const fck_radio_sniff_config_t *cfg);

kode_status_t fck_core_radio_ping(uint16_t count, uint16_t mhz, uint16_t gap_ms, int8_t dbm, fck_radio_ping_statistics_t *stats);

kode_status_t fck_core_radio_echo_start(uint16_t mhz, int8_t dbm);

kode_status_t fck_core_radio_echo_stop(fck_radio_echo_statistics_t *stats);

kode_status_t fck_core_subghz_level(uint32_t hz, fck_subghz_bandwidth_t bw, int16_t *dbm);

kode_status_t fck_core_subghz_spectrum(uint32_t first_hz, uint32_t step_hz, uint16_t count, fck_subghz_bandwidth_t bw, int16_t *dbm, size_t cap, size_t *n);

kode_status_t fck_core_subghz_listen_start(uint32_t hz);

kode_status_t fck_core_subghz_listen_read(uint32_t after, uint32_t ms, int32_t *us, size_t cap, size_t *n, fck_subghz_heard_t *heard);

kode_status_t fck_core_subghz_listen_stop(void);

kode_status_t fck_core_subghz_frequencies(uint32_t *hz, size_t cap, size_t *n);

kode_status_t fck_core_subghz_find(uint32_t ms, fck_subghz_hit_t *hit);

kode_status_t fck_core_subghz_capture(uint32_t hz, uint32_t ms, int32_t *us, size_t cap, size_t *n);

kode_status_t fck_core_subghz_decode(const int32_t *us, size_t n, fck_subghz_code_t *out, size_t cap, size_t *got);

kode_status_t fck_core_subghz_encode(const fck_subghz_code_t *code, uint8_t frames, int32_t *us, size_t cap, size_t *n);

kode_status_t fck_core_subghz_protocol_info(fck_subghz_protocol_t proto, fck_subghz_protocol_info_t *info);

kode_status_t fck_core_subghz_decode_rolling(const int32_t *us, size_t n, fck_subghz_rolling_t *out, size_t cap, size_t *got);

kode_status_t fck_core_subghz_air_time_left(uint32_t hz, uint32_t *ms);

kode_status_t fck_core_subghz_send_raw(uint32_t hz, const int32_t *us, size_t n, uint8_t repeats);

kode_status_t fck_core_subghz_send_code(uint32_t hz, const fck_subghz_code_t *code, uint8_t frames);

kode_status_t fck_core_subghz_packet_receive(uint32_t hz, const fck_subghz_packet_settings_t *settings, uint32_t ms, fck_subghz_packet_t *packets, size_t cap, size_t *got);

kode_status_t fck_core_subghz_packet_send(uint32_t hz, const fck_subghz_packet_settings_t *settings, const uint8_t *bytes, uint8_t length, uint8_t repeats);

kode_status_t fck_core_subghz_to_file(uint32_t hz, const int32_t *us, size_t n, char *text, size_t cap, size_t *length);

kode_status_t fck_core_subghz_code_to_file(uint32_t hz, const fck_subghz_code_t *code, char *text, size_t cap, size_t *length);

kode_status_t fck_core_subghz_from_file(const char *text, size_t length, fck_subghz_file_t *file, int32_t *us, size_t cap, size_t *n);

#ifdef __cplusplus
}
#endif

#endif /* FCK_CORE_H_ */
