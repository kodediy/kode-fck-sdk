/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_standard.c
 * @brief fck.h's calls over a core that writes down what it is asked: that each is one core call with
 *        its arguments, that its answer is the core's as true or false with the reason in fck_error(),
 *        that a call that blocks is refused from an Active Object before the core is asked and one that
 *        does not is not, and what fck_begin() and fck_end() do besides.
 *
 * The table below is generated from fck_core.h: one row per call, with arguments that differ from
 * one another so that a call that swaps two is seen. A row says whether the call blocks (the ones
 * that do are refused from an Active Object's task).
 */
#include <stdio.h>
#include <string.h>

#include "unity.h"

#include "fck.h"
#include "fck_aos.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "fake_aos.h"
#include "fake_core.h"

/* Room for every pointer a call is given: the core does not touch it. */
static uint8_t k_buf[1024];
#define P(n) (&k_buf[(n) * 16U])

static void dummy_line(void *ctx, const fck_line_t *line)
{
    (void)ctx;
    (void)line;
}

#define A(x) ((uint64_t)(uintptr_t)(x))

typedef struct {
    const char *name;                 /**< The core call, without its prefix.            */
    bool        blocks;               /**< Refused from an Active Object's task.         */
    bool      (*call)(uint64_t *args);/**< Makes the call; fills the arguments it passed. */
    unsigned    nargs;
} row_t;

static bool call_cancel(uint64_t *args)
{
    (void)args;
    return fck_cancel();
}

static bool call_wait_present(uint64_t *args)
{
    args[0] = A((uint32_t)7);
    return fck_wait_present((uint32_t)7);
}

static bool call_status(uint64_t *args)
{
    args[0] = A((fck_status_t *)P(1));
    return fck_status((fck_status_t *)P(1));
}

static bool call_command(uint64_t *args)
{
    args[0] = A((fck_chip_t)7);
    args[1] = A((const char *)P(2));
    args[2] = A((const char *)P(3));
    args[3] = A(dummy_line);
    args[4] = A((void *)P(5));
    args[5] = A((uint32_t)42);
    args[6] = A((int32_t *)P(7));
    return fck_command((fck_chip_t)7, (const char *)P(2), (const char *)P(3), dummy_line, (void *)P(5), (uint32_t)42, (int32_t *)P(7));
}

static bool call_reset(uint64_t *args)
{
    args[0] = A((fck_chip_t)7);
    return fck_reset((fck_chip_t)7);
}

static bool call_power(uint64_t *args)
{
    args[0] = A((fck_chip_t)7);
    args[1] = A(true);
    return fck_power((fck_chip_t)7, true);
}

static bool call_firmware(uint64_t *args)
{
    args[0] = A((fck_chip_t)7);
    args[1] = A((fck_firmware_t *)P(2));
    return fck_firmware((fck_chip_t)7, (fck_firmware_t *)P(2));
}

static bool call_flash(uint64_t *args)
{
    args[0] = A((fck_chip_t)7);
    args[1] = A((const fck_image_t *)P(2));
    args[2] = A((size_t)21);
    args[3] = A((fck_flash_report_t *)P(4));
    return fck_flash((fck_chip_t)7, (const fck_image_t *)P(2), (size_t)21, (fck_flash_report_t *)P(4));
}

static bool call_rescue_probe(uint64_t *args)
{
    args[0] = A((fck_chip_t)7);
    args[1] = A((fck_rescue_report_t *)P(2));
    return fck_rescue_probe((fck_chip_t)7, (fck_rescue_report_t *)P(2));
}

static bool call_wifi_scan(uint64_t *args)
{
    args[0] = A((fck_band_t)7);
    args[1] = A((fck_wifi_network_t *)P(2));
    args[2] = A((size_t)21);
    args[3] = A((size_t *)P(4));
    return fck_wifi_scan((fck_band_t)7, (fck_wifi_network_t *)P(2), (size_t)21, (size_t *)P(4));
}

static bool call_wifi_channels(uint64_t *args)
{
    args[0] = A((fck_band_t)7);
    args[1] = A((uint16_t)14);
    args[2] = A((fck_wifi_channel_t *)P(3));
    args[3] = A((size_t)28);
    args[4] = A((size_t *)P(5));
    return fck_wifi_channels((fck_band_t)7, (uint16_t)14, (fck_wifi_channel_t *)P(3), (size_t)28, (size_t *)P(5));
}

static bool call_wifi_frames(uint64_t *args)
{
    args[0] = A((uint8_t)6);
    args[1] = A((uint16_t)250);
    args[2] = A((fck_wifi_frame_t *)P(3));
    args[3] = A((size_t)28);
    args[4] = A((size_t *)P(5));
    args[5] = A((fck_wifi_frames_statistics_t *)P(6));
    return fck_wifi_frames((uint8_t)6, (uint16_t)250, (fck_wifi_frame_t *)P(3), (size_t)28, (size_t *)P(5),
                           (fck_wifi_frames_statistics_t *)P(6));
}

static bool call_ble_scan(uint64_t *args)
{
    args[0] = A((uint8_t)7);
    args[1] = A((fck_ble_device_t *)P(2));
    args[2] = A((size_t)21);
    args[3] = A((size_t *)P(4));
    return fck_ble_scan((uint8_t)7, (fck_ble_device_t *)P(2), (size_t)21, (size_t *)P(4));
}

static bool call_gnss_begin(uint64_t *args)
{
    (void)args;
    return fck_gnss_begin();
}

static bool call_gnss_read(uint64_t *args)
{
    args[0] = A((fck_gnss_fix_t *)P(1));
    args[1] = A((uint32_t)14);
    return fck_gnss_read((fck_gnss_fix_t *)P(1), (uint32_t)14);
}

static bool call_gnss_end(uint64_t *args)
{
    (void)args;
    return fck_gnss_end();
}

static bool call_radio_spectrum(uint64_t *args)
{
    args[0] = A((uint16_t)7);
    args[1] = A((uint16_t)14);
    args[2] = A((uint32_t)21);
    args[3] = A((uint16_t)28);
    args[4] = A((int8_t *)P(5));
    args[5] = A((size_t)42);
    args[6] = A((size_t *)P(7));
    return fck_radio_spectrum((uint16_t)7, (uint16_t)14, (uint32_t)21, (uint16_t)28, (int8_t *)P(5), (size_t)42, (size_t *)P(7));
}

static bool call_radio_sniff(uint64_t *args)
{
    args[0] = A((uint16_t)7);
    args[1] = A((uint32_t)14);
    args[2] = A((int8_t)-3);
    args[3] = A((fck_radio_frame_t *)P(4));
    args[4] = A((size_t)35);
    args[5] = A((size_t *)P(6));
    args[6] = A((fck_radio_sniff_statistics_t *)P(7));
    return fck_radio_sniff((uint16_t)7, (uint32_t)14, (int8_t)-3, (fck_radio_frame_t *)P(4), (size_t)35, (size_t *)P(6), (fck_radio_sniff_statistics_t *)P(7));
}

static bool call_radio_sniff_tune(uint64_t *args)
{
    args[0] = A((const fck_radio_sniff_config_t *)P(1));
    return fck_radio_sniff_tune((const fck_radio_sniff_config_t *)P(1));
}

static bool call_radio_ping(uint64_t *args)
{
    args[0] = A((uint16_t)7);
    args[1] = A((uint16_t)14);
    args[2] = A((uint16_t)21);
    args[3] = A((int8_t)-3);
    args[4] = A((fck_radio_ping_statistics_t *)P(5));
    return fck_radio_ping((uint16_t)7, (uint16_t)14, (uint16_t)21, (int8_t)-3, (fck_radio_ping_statistics_t *)P(5));
}

static bool call_radio_echo_start(uint64_t *args)
{
    args[0] = A((uint16_t)7);
    args[1] = A((int8_t)-3);
    return fck_radio_echo_start((uint16_t)7, (int8_t)-3);
}

static bool call_radio_echo_stop(uint64_t *args)
{
    args[0] = A((fck_radio_echo_statistics_t *)P(1));
    return fck_radio_echo_stop((fck_radio_echo_statistics_t *)P(1));
}

static bool call_subghz_level(uint64_t *args)
{
    args[0] = A((uint32_t)7);
    args[1] = A((fck_subghz_bandwidth_t)14);
    args[2] = A((int16_t *)P(3));
    return fck_subghz_level((uint32_t)7, (fck_subghz_bandwidth_t)14, (int16_t *)P(3));
}

static bool call_subghz_spectrum(uint64_t *args)
{
    args[0] = A((uint32_t)7);
    args[1] = A((uint32_t)14);
    args[2] = A((uint16_t)21);
    args[3] = A((fck_subghz_bandwidth_t)28);
    args[4] = A((int16_t *)P(5));
    args[5] = A((size_t)42);
    args[6] = A((size_t *)P(7));
    return fck_subghz_spectrum((uint32_t)7, (uint32_t)14, (uint16_t)21, (fck_subghz_bandwidth_t)28, (int16_t *)P(5), (size_t)42, (size_t *)P(7));
}

static bool call_subghz_listen_start(uint64_t *args)
{
    args[0] = A((uint32_t)7);
    return fck_subghz_listen_start((uint32_t)7);
}

static bool call_subghz_listen_read(uint64_t *args)
{
    args[0] = A((uint32_t)7);
    args[1] = A((uint32_t)14);
    args[2] = A((int32_t *)P(3));
    args[3] = A((size_t)28);
    args[4] = A((size_t *)P(5));
    args[5] = A((fck_subghz_heard_t *)P(6));
    return fck_subghz_listen_read((uint32_t)7, (uint32_t)14, (int32_t *)P(3), (size_t)28, (size_t *)P(5), (fck_subghz_heard_t *)P(6));
}

static bool call_subghz_listen_stop(uint64_t *args)
{
    (void)args;
    return fck_subghz_listen_stop();
}

static bool call_subghz_frequencies(uint64_t *args)
{
    args[0] = A((uint32_t *)P(1));
    args[1] = A((size_t)14);
    args[2] = A((size_t *)P(3));
    return fck_subghz_frequencies((uint32_t *)P(1), (size_t)14, (size_t *)P(3));
}

static bool call_subghz_find(uint64_t *args)
{
    args[0] = A((uint32_t)7);
    args[1] = A((fck_subghz_hit_t *)P(2));
    return fck_subghz_find((uint32_t)7, (fck_subghz_hit_t *)P(2));
}

static bool call_subghz_capture(uint64_t *args)
{
    args[0] = A((uint32_t)7);
    args[1] = A((uint32_t)14);
    args[2] = A((int32_t *)P(3));
    args[3] = A((size_t)28);
    args[4] = A((size_t *)P(5));
    return fck_subghz_capture((uint32_t)7, (uint32_t)14, (int32_t *)P(3), (size_t)28, (size_t *)P(5));
}

static bool call_subghz_decode(uint64_t *args)
{
    args[0] = A((const int32_t *)P(1));
    args[1] = A((size_t)14);
    args[2] = A((fck_subghz_code_t *)P(3));
    args[3] = A((size_t)28);
    args[4] = A((size_t *)P(5));
    return fck_subghz_decode((const int32_t *)P(1), (size_t)14, (fck_subghz_code_t *)P(3), (size_t)28, (size_t *)P(5));
}

static bool call_subghz_encode(uint64_t *args)
{
    args[0] = A((const fck_subghz_code_t *)P(1));
    args[1] = A((uint8_t)14);
    args[2] = A((int32_t *)P(3));
    args[3] = A((size_t)28);
    args[4] = A((size_t *)P(5));
    return fck_subghz_encode((const fck_subghz_code_t *)P(1), (uint8_t)14, (int32_t *)P(3), (size_t)28, (size_t *)P(5));
}

static bool call_subghz_protocol_info(uint64_t *args)
{
    args[0] = A((fck_subghz_protocol_t)7);
    args[1] = A((fck_subghz_protocol_info_t *)P(2));
    return fck_subghz_protocol_info((fck_subghz_protocol_t)7, (fck_subghz_protocol_info_t *)P(2));
}

static bool call_subghz_decode_rolling(uint64_t *args)
{
    args[0] = A((const int32_t *)P(1));
    args[1] = A((size_t)14);
    args[2] = A((fck_subghz_rolling_t *)P(3));
    args[3] = A((size_t)28);
    args[4] = A((size_t *)P(5));
    return fck_subghz_decode_rolling((const int32_t *)P(1), (size_t)14, (fck_subghz_rolling_t *)P(3), (size_t)28, (size_t *)P(5));
}

static bool call_subghz_air_time_left(uint64_t *args)
{
    args[0] = A((uint32_t)7);
    args[1] = A((uint32_t *)P(2));
    return fck_subghz_air_time_left((uint32_t)7, (uint32_t *)P(2));
}

static bool call_subghz_send_raw(uint64_t *args)
{
    args[0] = A((uint32_t)7);
    args[1] = A((const int32_t *)P(2));
    args[2] = A((size_t)21);
    args[3] = A((uint8_t)28);
    return fck_subghz_send_raw((uint32_t)7, (const int32_t *)P(2), (size_t)21, (uint8_t)28);
}

static bool call_subghz_send_code(uint64_t *args)
{
    args[0] = A((uint32_t)7);
    args[1] = A((const fck_subghz_code_t *)P(2));
    args[2] = A((uint8_t)21);
    return fck_subghz_send_code((uint32_t)7, (const fck_subghz_code_t *)P(2), (uint8_t)21);
}

static bool call_subghz_packet_receive(uint64_t *args)
{
    args[0] = A((uint32_t)7);
    args[1] = A((const fck_subghz_packet_settings_t *)P(2));
    args[2] = A((uint32_t)21);
    args[3] = A((fck_subghz_packet_t *)P(4));
    args[4] = A((size_t)35);
    args[5] = A((size_t *)P(6));
    return fck_subghz_packet_receive((uint32_t)7, (const fck_subghz_packet_settings_t *)P(2), (uint32_t)21, (fck_subghz_packet_t *)P(4), (size_t)35, (size_t *)P(6));
}

static bool call_subghz_packet_send(uint64_t *args)
{
    args[0] = A((uint32_t)7);
    args[1] = A((const fck_subghz_packet_settings_t *)P(2));
    args[2] = A((const uint8_t *)P(3));
    args[3] = A((uint8_t)28);
    args[4] = A((uint8_t)35);
    return fck_subghz_packet_send((uint32_t)7, (const fck_subghz_packet_settings_t *)P(2), (const uint8_t *)P(3), (uint8_t)28, (uint8_t)35);
}

static bool call_subghz_to_file(uint64_t *args)
{
    args[0] = A((uint32_t)7);
    args[1] = A((const int32_t *)P(2));
    args[2] = A((size_t)21);
    args[3] = A((char *)P(4));
    args[4] = A((size_t)35);
    args[5] = A((size_t *)P(6));
    return fck_subghz_to_file((uint32_t)7, (const int32_t *)P(2), (size_t)21, (char *)P(4), (size_t)35, (size_t *)P(6));
}

static bool call_subghz_code_to_file(uint64_t *args)
{
    args[0] = A((uint32_t)7);
    args[1] = A((const fck_subghz_code_t *)P(2));
    args[2] = A((char *)P(3));
    args[3] = A((size_t)28);
    args[4] = A((size_t *)P(5));
    return fck_subghz_code_to_file((uint32_t)7, (const fck_subghz_code_t *)P(2), (char *)P(3), (size_t)28, (size_t *)P(5));
}

static bool call_subghz_from_file(uint64_t *args)
{
    args[0] = A((const char *)P(1));
    args[1] = A((size_t)14);
    args[2] = A((fck_subghz_file_t *)P(3));
    args[3] = A((int32_t *)P(4));
    args[4] = A((size_t)35);
    args[5] = A((size_t *)P(6));
    return fck_subghz_from_file((const char *)P(1), (size_t)14, (fck_subghz_file_t *)P(3), (int32_t *)P(4), (size_t)35, (size_t *)P(6));
}

static const row_t k_rows[] = {
    { "cancel", false, call_cancel, 0 },
    { "wait_present", false, call_wait_present, 1 },
    { "status", false, call_status, 1 },
    { "command", true, call_command, 7 },
    { "reset", true, call_reset, 1 },
    { "power", true, call_power, 2 },
    { "firmware", true, call_firmware, 2 },
    { "flash", true, call_flash, 4 },
    { "rescue_probe", true, call_rescue_probe, 2 },
    { "wifi_scan", true, call_wifi_scan, 4 },
    { "wifi_channels", true, call_wifi_channels, 5 },
    { "wifi_frames", true, call_wifi_frames, 6 },
    { "ble_scan", true, call_ble_scan, 4 },
    { "gnss_begin", true, call_gnss_begin, 0 },
    { "gnss_read", false, call_gnss_read, 2 },
    { "gnss_end", true, call_gnss_end, 0 },
    { "radio_spectrum", true, call_radio_spectrum, 7 },
    { "radio_sniff", true, call_radio_sniff, 7 },
    { "radio_sniff_tune", true, call_radio_sniff_tune, 1 },
    { "radio_ping", true, call_radio_ping, 5 },
    { "radio_echo_start", true, call_radio_echo_start, 2 },
    { "radio_echo_stop", true, call_radio_echo_stop, 1 },
    { "subghz_level", true, call_subghz_level, 3 },
    { "subghz_spectrum", true, call_subghz_spectrum, 7 },
    { "subghz_listen_start", false, call_subghz_listen_start, 1 },
    { "subghz_listen_read", false, call_subghz_listen_read, 6 },
    { "subghz_listen_stop", false, call_subghz_listen_stop, 0 },
    { "subghz_frequencies", false, call_subghz_frequencies, 3 },
    { "subghz_find", true, call_subghz_find, 2 },
    { "subghz_capture", true, call_subghz_capture, 5 },
    { "subghz_decode", false, call_subghz_decode, 5 },
    { "subghz_encode", false, call_subghz_encode, 5 },
    { "subghz_protocol_info", false, call_subghz_protocol_info, 2 },
    { "subghz_decode_rolling", false, call_subghz_decode_rolling, 5 },
    { "subghz_air_time_left", false, call_subghz_air_time_left, 2 },
    { "subghz_send_raw", true, call_subghz_send_raw, 4 },
    { "subghz_send_code", true, call_subghz_send_code, 3 },
    { "subghz_packet_receive", true, call_subghz_packet_receive, 6 },
    { "subghz_packet_send", true, call_subghz_packet_send, 5 },
    { "subghz_to_file", false, call_subghz_to_file, 6 },
    { "subghz_code_to_file", false, call_subghz_code_to_file, 5 },
    { "subghz_from_file", false, call_subghz_from_file, 6 },
};
#define ROWS (sizeof(k_rows) / sizeof(k_rows[0]))

/* ---- The table's checks --------------------------------------------------------------------- */

static void start(void)
{
    fake_core_reset();
    fake_aos_reset();
}

static const struct {
    kode_status_t st;
    const char   *name;
} k_statuses[] = {
    { KODE_DENIED, "KODE_DENIED" },
    { KODE_TIMEOUT, "KODE_TIMEOUT" },
    { KODE_IO_ERROR, "KODE_IO_ERROR" },
    { KODE_NO_RESOURCES, "KODE_NO_RESOURCES" },
    { KODE_ERROR, "KODE_ERROR" },
};

static void label(char *to, size_t cap, const row_t *r, const char *what)
{
    (void)snprintf(to, cap, "fck_%s(): %s", r->name, what);
}

TEST_CASE("every call is one core call with its arguments, and answers true when the core says OK", "[standard]")
{
    for (size_t i = 0U; i < ROWS; i++) {
        char msg[96];
        start();
        uint64_t args[FAKE_CORE_ARGS_MAX] = { 0 };
        label(msg, sizeof(msg), &k_rows[i], "true");
        TEST_ASSERT_TRUE_MESSAGE(k_rows[i].call(args), msg);
        label(msg, sizeof(msg), &k_rows[i], "one core call");
        TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, g_core.calls, msg);
        label(msg, sizeof(msg), &k_rows[i], "the core call is the call's own");
        TEST_ASSERT_EQUAL_STRING_MESSAGE(k_rows[i].name, g_core.last, msg);
        label(msg, sizeof(msg), &k_rows[i], "the number of arguments");
        TEST_ASSERT_EQUAL_UINT_MESSAGE(k_rows[i].nargs, g_core.nargs, msg);
        for (unsigned a = 0U; a < k_rows[i].nargs; a++) {
            label(msg, sizeof(msg), &k_rows[i], "an argument reached the core changed or out of place");
            TEST_ASSERT_EQUAL_UINT64_MESSAGE(args[a], g_core.arg[a], msg);
        }
    }
}

TEST_CASE("a call that worked clears fck_error(), whatever an earlier call left in it", "[standard]")
{
    for (size_t i = 0U; i < ROWS; i++) {
        char msg[96];
        start();
        fck_aos_error_set_for(KODE_DENIED, "an earlier call's reason");
        uint64_t args[FAKE_CORE_ARGS_MAX] = { 0 };
        TEST_ASSERT_TRUE(k_rows[i].call(args));
        label(msg, sizeof(msg), &k_rows[i], "fck_error() was not cleared");
        TEST_ASSERT_EQUAL_STRING_MESSAGE("", fck_error(), msg);
    }
}

static void every_call_is_false_for(kode_status_t st)
{
    for (size_t i = 0U; i < ROWS; i++) {
        char msg[96];
        start();
        g_core.next = st;
        uint64_t args[FAKE_CORE_ARGS_MAX] = { 0 };
        label(msg, sizeof(msg), &k_rows[i], "must answer false");
        TEST_ASSERT_FALSE_MESSAGE(k_rows[i].call(args), msg);
        label(msg, sizeof(msg), &k_rows[i], "the core was asked once");
        TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, g_core.calls, msg);
        label(msg, sizeof(msg), &k_rows[i], "fck_error() must say why");
        TEST_ASSERT_TRUE_MESSAGE(strlen(fck_error()) > 0U, msg);
    }
}

TEST_CASE("every call answers false when the core says KODE_DENIED, and fck_error() says why", "[standard]")
{
    every_call_is_false_for(KODE_DENIED);
}

TEST_CASE("every call answers false when the core says KODE_TIMEOUT, and fck_error() says why", "[standard]")
{
    every_call_is_false_for(KODE_TIMEOUT);
}

TEST_CASE("every call answers false when the core says KODE_IO_ERROR, and fck_error() says why", "[standard]")
{
    every_call_is_false_for(KODE_IO_ERROR);
}

TEST_CASE("every call answers false when the core says KODE_NO_RESOURCES, and fck_error() says why", "[standard]")
{
    every_call_is_false_for(KODE_NO_RESOURCES);
}

TEST_CASE("every call answers false when the core says KODE_ERROR, and fck_error() says why", "[standard]")
{
    every_call_is_false_for(KODE_ERROR);
}

TEST_CASE("a call is true for exactly the status OK: no other value passes", "[standard]")
{
    start();
    uint64_t args[FAKE_CORE_ARGS_MAX] = { 0 };
    for (size_t s = 0U; s < sizeof(k_statuses) / sizeof(k_statuses[0]); s++) {
        g_core.next = k_statuses[s].st;
        TEST_ASSERT_FALSE_MESSAGE(fck_reset(FCK_C5), k_statuses[s].name);
    }
    g_core.next = KODE_OK;
    TEST_ASSERT_TRUE(k_rows[0].call(args));
}

/* ---- From an Active Object's task ----------------------------------------------------------- */

static void refused_from_an_active_object(const row_t *r, bool *refused)
{
    char msg[96];
    start();
    fake_aos_mark_ao(xTaskGetCurrentTaskHandle());
    uint64_t args[FAKE_CORE_ARGS_MAX] = { 0 };
    bool const rc = r->call(args);
    fake_aos_mark_ao(NULL);
    if (r->blocks) {
        label(msg, sizeof(msg), r, "a call that blocks must be refused from an Active Object");
        TEST_ASSERT_FALSE_MESSAGE(rc, msg);
        label(msg, sizeof(msg), r, "the core must not be asked");
        TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, g_core.calls, msg);
        char want[48];
        (void)snprintf(want, sizeof(want), "fck_%s()", r->name);
        label(msg, sizeof(msg), r, "the reason names the call");
        TEST_ASSERT_NOT_NULL_MESSAGE(strstr(fck_error(), want), msg);
        label(msg, sizeof(msg), r, "the reason says what it was");
        TEST_ASSERT_NOT_NULL_MESSAGE(strstr(fck_error(), "was called from an Active Object"), msg);
        label(msg, sizeof(msg), r, "the reason names the way out");
        TEST_ASSERT_NOT_NULL_MESSAGE(strstr(fck_error(), "fck_sys_*()"), msg);
        *refused = true;
    } else {
        label(msg, sizeof(msg), r, "a call that never waits is allowed from an Active Object");
        TEST_ASSERT_TRUE_MESSAGE(rc, msg);
        TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, g_core.calls, msg);
        *refused = false;
    }
}

TEST_CASE("a call that blocks is refused from an Active Object's task before the core is asked; one that never waits is not", "[standard][ao]")
{
    unsigned refusals = 0U;
    unsigned allowed  = 0U;
    for (size_t i = 0U; i < ROWS; i++) {
        bool refused = false;
        refused_from_an_active_object(&k_rows[i], &refused);
        if (refused) {
            refusals++;
        } else {
            allowed++;
        }
    }
    /* The table is the contract: 26 calls block, 16 do not (the listening's, the status, the pure helpers). */
    TEST_ASSERT_EQUAL_UINT(26U, refusals);
    TEST_ASSERT_EQUAL_UINT(16U, allowed);
}

TEST_CASE("the same calls on an ordinary task are not refused", "[standard][ao]")
{
    start();
    fake_aos_mark_ao((TaskHandle_t)0x1);     /* some other task is an Active Object's */
    for (size_t i = 0U; i < ROWS; i++) {
        uint64_t args[FAKE_CORE_ARGS_MAX] = { 0 };
        TEST_ASSERT_TRUE(k_rows[i].call(args));
    }
    fake_aos_mark_ao(NULL);
}

/* ---- fck_begin and fck_end ----------------------------------------------------------------- */

TEST_CASE("fck_begin runs kode_init, starts the events task, asks the core to begin and answers true", "[standard][begin]")
{
    start();
    TEST_ASSERT_TRUE(fck_begin());
    TEST_ASSERT_EQUAL_UINT(1U, g_kode_inits);
    TEST_ASSERT_TRUE_MESSAGE(fake_aos_sink_is_set(), "the events task registered its sink");
    TEST_ASSERT_EQUAL_UINT(1U, g_core.calls);
    TEST_ASSERT_EQUAL_STRING("begin", g_core.last);
    TEST_ASSERT_EQUAL_STRING("", fck_error());
    /* Idempotent: the app may call it again. */
    TEST_ASSERT_TRUE(fck_begin());
    TEST_ASSERT_EQUAL_UINT(2U, g_kode_inits);
}

TEST_CASE("fck_begin answers false with the core's reason for every status but OK", "[standard][begin]")
{
    for (size_t s = 0U; s < sizeof(k_statuses) / sizeof(k_statuses[0]); s++) {
        start();
        g_core.next = k_statuses[s].st;
        TEST_ASSERT_FALSE_MESSAGE(fck_begin(), k_statuses[s].name);
        TEST_ASSERT_TRUE_MESSAGE(strlen(fck_error()) > 0U, k_statuses[s].name);
    }
}

TEST_CASE("fck_begin from an Active Object is refused and nothing starts", "[standard][begin][ao]")
{
    start();
    fake_aos_mark_ao(xTaskGetCurrentTaskHandle());
    unsigned const inits = g_kode_inits;
    TEST_ASSERT_FALSE(fck_begin());
    fake_aos_mark_ao(NULL);
    TEST_ASSERT_EQUAL_UINT(inits, g_kode_inits);
    TEST_ASSERT_EQUAL_UINT(0U, g_core.calls);
    TEST_ASSERT_NOT_NULL(strstr(fck_error(), "fck_begin()"));
    TEST_ASSERT_NOT_NULL(strstr(fck_error(), "fck_sys_*()"));
}

TEST_CASE("fck_end asks the core to end and answers true; false with the core's reason otherwise", "[standard][end]")
{
    start();
    TEST_ASSERT_TRUE(fck_end());
    TEST_ASSERT_EQUAL_STRING("end", g_core.last);
    for (size_t s = 0U; s < sizeof(k_statuses) / sizeof(k_statuses[0]); s++) {
        start();
        g_core.next = k_statuses[s].st;
        TEST_ASSERT_FALSE_MESSAGE(fck_end(), k_statuses[s].name);
        TEST_ASSERT_TRUE_MESSAGE(strlen(fck_error()) > 0U, k_statuses[s].name);
    }
}

TEST_CASE("fck_end from an Active Object is refused before the core is asked", "[standard][end][ao]")
{
    start();
    fake_aos_mark_ao(xTaskGetCurrentTaskHandle());
    TEST_ASSERT_FALSE(fck_end());
    fake_aos_mark_ao(NULL);
    TEST_ASSERT_EQUAL_UINT(0U, g_core.calls);
    TEST_ASSERT_NOT_NULL(strstr(fck_error(), "fck_end()"));
}

/* ---- The calls that never fail ---------------------------------------------------------------- */

TEST_CASE("fck_error is empty before any call and never NULL", "[standard][immediate]")
{
    start();
    TEST_ASSERT_NOT_NULL(fck_error());
    TEST_ASSERT_EQUAL_STRING("", fck_error());
}

TEST_CASE("fck_is_present, fck_header_position and the listening's frequency answer from the owner at once", "[standard][immediate]")
{
    start();
    g_core.present = false;
    TEST_ASSERT_FALSE(fck_is_present());
    TEST_ASSERT_EQUAL_UINT8(0U, fck_header_position(9U));
    g_core.present = true;
    TEST_ASSERT_TRUE(fck_is_present());
    TEST_ASSERT_EQUAL_UINT8(9U, fck_header_position(9U));
    g_core.hz = 433920000U;
    TEST_ASSERT_EQUAL_UINT32(433920000U, fck_subghz_listen_hz());
    TEST_ASSERT_EQUAL_UINT(0U, g_core.calls);
}

TEST_CASE("fck_subghz_may_transmit and fck_subghz_is_rolling are plain answers, from any task", "[standard][immediate][ao]")
{
    start();
    static const int32_t us[2] = { 350, -1050 };
    g_core.may     = true;
    g_core.rolling = true;
    fake_aos_mark_ao(xTaskGetCurrentTaskHandle());
    TEST_ASSERT_TRUE(fck_subghz_may_transmit(433920000U));
    TEST_ASSERT_TRUE(fck_subghz_is_rolling(us, 2U));
    TEST_ASSERT_EQUAL_UINT32(0U, fck_subghz_listen_hz());
    fake_aos_mark_ao(NULL);
    g_core.may     = false;
    g_core.rolling = false;
    TEST_ASSERT_FALSE(fck_subghz_may_transmit(433920000U));
    TEST_ASSERT_FALSE(fck_subghz_is_rolling(us, 2U));
}

/* ---- Any call, in any state ------------------------------------------------------------------ */

TEST_CASE("any call in any state: it returns, it answers true or false, and fck_error() agrees", "[standard][any]")
{
    static const kode_status_t k_all[] = { KODE_OK, KODE_DENIED, KODE_TIMEOUT, KODE_IO_ERROR, KODE_NO_RESOURCES, KODE_ERROR };
    unsigned runs = 0U;
    for (unsigned started = 0U; started < 2U; started++) {
        for (unsigned ao = 0U; ao < 2U; ao++) {
            for (size_t s = 0U; s < sizeof(k_all) / sizeof(k_all[0]); s++) {
                for (size_t i = 0U; i < ROWS; i++) {
                    char msg[96];
                    start();
                    g_aos_started = (started != 0U);
                    g_core.next   = k_all[s];
                    fake_aos_mark_ao(ao != 0U ? xTaskGetCurrentTaskHandle() : NULL);
                    for (unsigned again = 0U; again < 2U; again++) {      /* twice: the second sees the first's state */
                        uint64_t args[FAKE_CORE_ARGS_MAX] = { 0 };
                        bool const rc = k_rows[i].call(args);
                        runs++;
                        TEST_ASSERT_NOT_NULL(fck_error());
                        if (rc) {
                            label(msg, sizeof(msg), &k_rows[i], "true with a reason left in fck_error()");
                            TEST_ASSERT_EQUAL_STRING_MESSAGE("", fck_error(), msg);
                        } else {
                            label(msg, sizeof(msg), &k_rows[i], "false without a reason");
                            TEST_ASSERT_TRUE_MESSAGE(strlen(fck_error()) > 0U, msg);
                        }
                        if (k_all[s] != KODE_OK && !(ao != 0U && k_rows[i].blocks)) {
                            label(msg, sizeof(msg), &k_rows[i], "must be false for a core that failed");
                            TEST_ASSERT_FALSE_MESSAGE(rc, msg);
                        }
                    }
                    fake_aos_mark_ao(NULL);
                }
            }
        }
    }
    TEST_ASSERT_EQUAL_UINT(2U * 2U * 6U * ROWS * 2U, runs);
    g_aos_started = true;
}
