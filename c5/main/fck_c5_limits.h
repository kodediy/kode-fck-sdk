/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file fck_c5_limits.h
 * @brief The module's radio limits on the Wi-Fi driver. They are always on:
 *        no option of the build turns them off.
 *
 * The same values as the official firmware's: firmware/fck-c5 keeps them in
 * its own table (firmware/fck-c5/main/fck_c5_limits.h), and this is a
 * copy, not a shared component.
 *
 * What they do:
 * - fck_c5_limits_wifi_start() sets country ES with 802.11d off, 20 MHz and no
 *   802.11b on the station, starts the driver, and leaves it on 2.4 GHz only
 *   at 15 dBm. If the driver refuses any of that, the radio stays off until
 *   the C5 restarts.
 * - fck_c5_limits_scan_pass() scans 2.4 GHz actively on channels 1-13 at
 *   15 dBm, or listens on 5 GHz channels 100-140 at 13 dBm without
 *   transmitting, and goes back to 2.4 GHz only.
 * - The build fails when menuconfig enables the SoftAP, a Bluetooth role
 *   that transmits, a Bluetooth controller without a host, or 802.15.4
 *   (fck_c5_limits.c).
 *
 * Left to your code, because no call here sees it: scanning Bluetooth LE
 * passively, and using neither ESP-NOW nor raw 802.11 frames
 * (esp_wifi_80211_tx()), which pick their own rate (ESP-NOW's default is
 * 1 Mbps, 802.11b).
 */
#ifndef FCK_C5_LIMITS_H_
#define FCK_C5_LIMITS_H_

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_wifi.h"

/**
 * @brief Start the Wi-Fi driver inside the limits.
 *
 * After esp_wifi_init() and esp_wifi_set_mode(WIFI_MODE_STA). A limit the
 * driver refuses, on this call or an earlier one, keeps the radio off: the
 * driver is not started, or is stopped again, and fck_c5_limits_state() names
 * the limit, which the greeting reports as lim=.
 *
 * @return ESP_OK, ESP_ERR_NOT_ALLOWED after a refused limit, or what
 *         esp_wifi_start() returned.
 */
esp_err_t fck_c5_limits_wifi_start(void);

/**
 * @brief One scan of one band, blocking, its records appended to @p rec.
 *
 * @param five  false: 2.4 GHz. true: 5 GHz.
 * @param rec   Where the records go, from @p rec[*n] on.
 * @param n     In: how many @p rec already holds. Out: plus what this pass
 *              added, never past @p cap.
 * @param cap   How many @p rec holds.
 * @param total Out: plus how many APs the pass heard, kept or not.
 * @return ESP_OK, ESP_ERR_NOT_ALLOWED after a refused limit (nothing
 *         scanned), or the first ESP-IDF error on the way.
 */
esp_err_t fck_c5_limits_scan_pass(bool five, wifi_ap_record_t *rec, uint16_t *n,
                                  uint16_t cap, uint16_t *total);

/** @brief "EU": the region whose limits this firmware keeps. */
const char *fck_c5_limits_region(void);

/** @brief "ok" when the driver took every limit, or the first one it
 *         refused: "country", "bw20",
 *         "no11b", "band", "power" or "power-readback". */
const char *fck_c5_limits_state(void);

#endif /* FCK_C5_LIMITS_H_ */
