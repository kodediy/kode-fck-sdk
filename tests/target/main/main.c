/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file main.c
 * @brief The target build's app: it links every call of fck.h and fck_sys.h, and starts the
 *        owner.
 *
 * Compiling and linking for the esp32p4 is the whole check. Every call of both layers is named in
 * ALL so that each one, and everything below it - the core, the owner, the drivers, the C5's ROM
 * loader and the nRF's DFU - is linked here rather than first in somebody's app. Flashed on a Dot,
 * it also starts looking for the module (through the standard layer, on this task) and says what it
 * found. Nothing transmits: the one call it makes on a module listens.
 */
#include "kode.h"
#include "fck.h"
#include "fck_sys.h"

#define TAG "fck_target"

__attribute__((used)) static const void *const ALL[] = {
    /* fck.h, the standard layer */
    (const void *)fck_firmware,
    (const void *)fck_begin,
    (const void *)fck_ble_appearance,
    (const void *)fck_ble_manufacturer_data,
    (const void *)fck_ble_scan,
    (const void *)fck_ble_service_data,
    (const void *)fck_ble_services,
    (const void *)fck_cancel,
    (const void *)fck_command,
    (const void *)fck_end,
    (const void *)fck_error,
    (const void *)fck_event_add_listener,
    (const void *)fck_event_remove_listener,
    (const void *)fck_event_wait,
    (const void *)fck_flash,
    (const void *)fck_gnss_begin,
    (const void *)fck_gnss_end,
    (const void *)fck_gnss_read,
    (const void *)fck_header_position,
    (const void *)fck_is_present,
    (const void *)fck_power,
    (const void *)fck_radio_echo_start,
    (const void *)fck_radio_echo_stop,
    (const void *)fck_radio_ping,
    (const void *)fck_radio_sniff,
    (const void *)fck_radio_sniff_tune,
    (const void *)fck_radio_spectrum,
    (const void *)fck_rescue_probe,
    (const void *)fck_reset,
    (const void *)fck_status,
    (const void *)fck_subghz_air_time_left,
    (const void *)fck_subghz_capture,
    (const void *)fck_subghz_code_to_file,
    (const void *)fck_subghz_decode,
    (const void *)fck_subghz_decode_rolling,
    (const void *)fck_subghz_encode,
    (const void *)fck_subghz_find,
    (const void *)fck_subghz_frequencies,
    (const void *)fck_subghz_from_file,
    (const void *)fck_subghz_is_rolling,
    (const void *)fck_subghz_level,
    (const void *)fck_subghz_listen_hz,
    (const void *)fck_subghz_listen_read,
    (const void *)fck_subghz_listen_start,
    (const void *)fck_subghz_listen_stop,
    (const void *)fck_subghz_may_transmit,
    (const void *)fck_subghz_packet_receive,
    (const void *)fck_subghz_packet_send,
    (const void *)fck_subghz_protocol_info,
    (const void *)fck_subghz_send_code,
    (const void *)fck_subghz_send_raw,
    (const void *)fck_subghz_spectrum,
    (const void *)fck_subghz_to_file,
    (const void *)fck_wait_present,
    (const void *)fck_wifi_channels,
    (const void *)fck_wifi_frames,
    (const void *)fck_wifi_scan,
    /* fck_sys.h, the extended layer */
    (const void *)fck_sys_firmware,
    (const void *)fck_sys_begin,
    (const void *)fck_sys_ble_scan,
    (const void *)fck_sys_cancel,
    (const void *)fck_sys_command,
    (const void *)fck_sys_end,
    (const void *)fck_sys_flash,
    (const void *)fck_sys_gnss_begin,
    (const void *)fck_sys_gnss_end,
    (const void *)fck_sys_gnss_read,
    (const void *)fck_sys_header_position,
    (const void *)fck_sys_is_present,
    (const void *)fck_sys_power,
    (const void *)fck_sys_radio_echo_start,
    (const void *)fck_sys_radio_echo_stop,
    (const void *)fck_sys_radio_ping,
    (const void *)fck_sys_radio_sniff,
    (const void *)fck_sys_radio_sniff_tune,
    (const void *)fck_sys_radio_spectrum,
    (const void *)fck_sys_rescue_probe,
    (const void *)fck_sys_reset,
    (const void *)fck_sys_status_read,
    (const void *)fck_sys_subghz_air_time_left,
    (const void *)fck_sys_subghz_capture,
    (const void *)fck_sys_subghz_find,
    (const void *)fck_sys_subghz_frequencies,
    (const void *)fck_sys_subghz_is_rolling,
    (const void *)fck_sys_subghz_level,
    (const void *)fck_sys_subghz_listen_hz,
    (const void *)fck_sys_subghz_listen_read,
    (const void *)fck_sys_subghz_listen_start,
    (const void *)fck_sys_subghz_listen_stop,
    (const void *)fck_sys_subghz_may_transmit,
    (const void *)fck_sys_subghz_packet_receive,
    (const void *)fck_sys_subghz_packet_send,
    (const void *)fck_sys_subghz_protocol_info,
    (const void *)fck_sys_subghz_send_code,
    (const void *)fck_sys_subghz_send_raw,
    (const void *)fck_sys_subghz_spectrum,
    (const void *)fck_sys_subscribe,
    (const void *)fck_sys_unsubscribe,
    (const void *)fck_sys_wifi_channels,
    (const void *)fck_sys_wifi_scan,
    (const void *)fck_why_text,
};

void app_main(void)
{
    kode_init();
    if (!fck_begin()) {
        KLOG_E(TAG, "the owner did not start: %s", fck_error());
        return;
    }
    KLOG_I(TAG, "%u calls linked; looking for an F*CK on J3", (unsigned)(sizeof(ALL) / sizeof(ALL[0])));
    if (!fck_wait_present(5000U)) {
        KLOG_W(TAG, "no F*CK on J3");
        return;
    }
    fck_status_t info;
    if (!fck_status(&info)) {
        KLOG_W(TAG, "the status could not be read: %s", fck_error());
        return;
    }
    KLOG_I(TAG, "found, generation %lu: C5 %s, nRF %s, CC1101 %s (0x%02X/0x%02X)",
           (unsigned long)info.generation, info.c5_on ? "on" : "off",
           info.nrf_on ? "on" : "off", info.cc1101_ok ? "asleep" : "missing",
           (unsigned)info.cc1101_part, (unsigned)info.cc1101_version);

    /* Listening only; the level itself is not logged. */
    int16_t dbm = 0;
    KLOG_I(TAG, "fck_subghz_level 433.92 MHz: %s", fck_subghz_level(433920000U, FCK_SUBGHZ_NARROW, &dbm) ? "ok" : fck_error());
}
