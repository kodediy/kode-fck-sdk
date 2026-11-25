/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file wardriving_wigle.h
 * @brief What the example heard, as the lines of a WiGLE CSV file (WigleWifi-1.6).
 *
 * WiGLE's format, as its page gives it (https://api.wigle.net/csvFormat.html): a file is UTF-8,
 * starts with one line that says the version and what wrote it and one with the fourteen column
 * names, and then holds one row per network or device heard, with the time in UTC and the position
 * it was heard at. This file is plain C - no module, no file, no log - so the host suite of the
 * rules runs it (tests/host/cases/test_wardriving_wigle.c).
 */
#ifndef WARDRIVING_WIGLE_H_
#define WARDRIVING_WIGLE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "fck.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Bytes that hold the two lines a file starts with, NUL included. */
#define WARDRIVING_WIGLE_HEADER_MAX 320U
/** Bytes that hold the longest row, NUL included. */
#define WARDRIVING_WIGLE_ROW_MAX    256U

/**
 * @brief Whether @p fix can place what was just heard: a position the receiver vouches for, with
 *        its date and time, read no longer than @p age_max_ms ago (fck_gnss_fix_t.age_ms).
 * @param fix        The latest fix; NULL is none.
 * @param age_max_ms The oldest that still counts as "where this was heard".
 * @return true when a row may be written with it.
 */
bool wardriving_wigle_fix_places(const fck_gnss_fix_t *fix, uint32_t age_max_ms);

/**
 * @brief The two lines a WiGLE file starts with.
 * @param out Out: the text, NUL-terminated.
 * @param cap How many bytes @p out holds; WARDRIVING_WIGLE_HEADER_MAX is enough.
 * @return Its length; 0, and nothing written, when it does not fit.
 */
size_t wardriving_wigle_header(char *out, size_t cap);

/**
 * @brief One Wi-Fi network heard at @p fix, as a row.
 * @param out     Out: the row, ending in a line feed, NUL-terminated.
 * @param cap     How many bytes @p out holds; WARDRIVING_WIGLE_ROW_MAX is enough.
 * @param network One of fck_wifi_scan()'s.
 * @param fix     Where and when: its position, date and time. Its age is the caller's to judge
 *                (wardriving_wigle_fix_places()).
 * @return Its length; 0, and nothing written, for a fix with no position, date or time, a NULL
 *         argument, or a row that does not fit.
 */
size_t wardriving_wigle_wifi_row(char *out, size_t cap, const fck_wifi_network_t *network, const fck_gnss_fix_t *fix);

/**
 * @brief One Bluetooth LE device heard at @p fix, as a row.
 * @param out     Out: the row, ending in a line feed, NUL-terminated.
 * @param cap     How many bytes @p out holds; WARDRIVING_WIGLE_ROW_MAX is enough.
 * @param device  One of fck_ble_scan()'s.
 * @param company The number of the company whose data it advertised
 *                (fck_ble_manufacturer_data()), or NULL when it advertised none.
 * @param fix     As wardriving_wigle_wifi_row().
 * @return As wardriving_wigle_wifi_row().
 */
size_t wardriving_wigle_ble_row(char *out, size_t cap, const fck_ble_device_t *device, const uint16_t *company,
                                const fck_gnss_fix_t *fix);

#ifdef __cplusplus
}
#endif

#endif /* WARDRIVING_WIGLE_H_ */
