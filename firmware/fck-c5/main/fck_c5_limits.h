/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file fck_c5_limits.h
 * @brief The radio limits of fck-c5: every one of them, in this one table.
 *
 * These are the limits of the F*CK module (KDFCK001) for the EU. fck_c5.c takes every radio number from
 * FCK_C5_LIMITS and holds none of its own, and nothing lifts them: no command, no build option, no second
 * table.
 *
 * A fork that wants other values changes them here, and only here.
 *
 * Three limits decide what is compiled, so they are in sdkconfig.defaults and not below: Wi-Fi as a station
 * only (no SoftAP), Bluetooth LE with the observer role alone, and no IEEE 802.15.4.
 */
#ifndef FCK_C5_LIMITS_H_
#define FCK_C5_LIMITS_H_

#include <stdbool.h>
#include <stdint.h>
#include "esp_wifi_types.h"

/** A 5 GHz channel: its number, and its bit in the Wi-Fi driver's scan bitmap (wifi_5g_channel_bit_t). */
typedef struct {
    uint8_t  number;
    uint32_t bit;
} fck_c5_channel_5g_t;

/** Written this way a number the driver has no channel for does not compile. */
#define FCK_C5_CHANNEL_5G(n) { (n), WIFI_CHANNEL_##n }

/** Every 5 GHz channel the driver knows, 36 to 177. A list ends at the first number 0. */
#define FCK_C5_CHANNELS_5G_MAX 28

/** What one band may do. */
typedef struct {
    int8_t           tx_power_qdbm;  /**< The most it transmits, in quarters of a dBm, the unit of
                                      *   esp_wifi_set_max_tx_power(): 60 is 15.0 dBm.                    */
    wifi_bandwidth_t bandwidth;      /**< The widest channel it uses.                                    */
    uint16_t         protocols;      /**< The 802.11 modes that are on, WIFI_PROTOCOL_*.                 */
    wifi_scan_type_t scan_type;      /**< How `scan` looks for APs: WIFI_SCAN_TYPE_ACTIVE sends probe
                                      *   requests, WIFI_SCAN_TYPE_PASSIVE only listens.                 */
    uint16_t         active_min_ms;  /**< An active scan, per channel: how long it waits for a first
                                      *   answer (0: it stays active_max_ms whatever it hears) ...       */
    uint16_t         active_max_ms;  /**< ... and how long it stays at most.                             */
    uint16_t         passive_ms;     /**< A passive scan, per channel: how long it listens.              */
} fck_c5_band_limits_t;

/** The radio limits of the firmware. */
typedef struct {
    const char *region;              /**< The region the limits are for: reg= of #C5INFO.                */
    const char *country;             /**< The Wi-Fi driver's country table.                              */
    bool        country_from_ap;     /**< 802.11d: let an AP's country replace it.                       */
    uint8_t     channel_2g_first;    /**< 2.4 GHz: the channels `scan` and `chan` use, first to last.    */
    uint8_t     channel_2g_last;
    fck_c5_band_limits_t band_2g;
    fck_c5_band_limits_t band_5g;
    /** 5 GHz: the channels `scan` covers. */
    fck_c5_channel_5g_t scan_5g[FCK_C5_CHANNELS_5G_MAX + 1];
    /** 5 GHz: the channels `chan` tries. It receives only, and the driver refuses the ones the country
     *  does not allow, which are then left out of the survey. */
    fck_c5_channel_5g_t listen_5g[FCK_C5_CHANNELS_5G_MAX + 1];
    bool        ble_scan_passive;    /**< `ble scan` never sends a scan request.                         */
} fck_c5_limits_t;

static const fck_c5_limits_t FCK_C5_LIMITS = {
    .region = "EU",

    /* The ES table (2.4 GHz 1-13, 5 GHz 36-64 and 100-140), and never the country of an AP. */
    .country         = "ES",
    .country_from_ap = false,

    .channel_2g_first = 1,
    .channel_2g_last  = 13,

    /* 2.4 GHz: 15.0 dBm, 20 MHz, no 802.11b, so a probe request leaves as OFDM. An active
     * scan: 120 ms on each channel. */
    .band_2g = {
        .tx_power_qdbm = 60,
        .bandwidth     = WIFI_BW20,
        .protocols     = WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N | WIFI_PROTOCOL_11AX,
        .scan_type     = WIFI_SCAN_TYPE_ACTIVE,
        .active_min_ms = 0,
        .active_max_ms = 120,
        .passive_ms    = 0,          /* not used: this band scans actively */
    },

    /* 5 GHz: 13.0 dBm, 20 MHz, and only LISTENED to: a DFS slave must not transmit before it
     * has heard the AP, and a probe request is a transmission. A passive scan: 300 ms on each
     * channel, three beacons at the usual 102.4 ms interval. */
    .band_5g = {
        .tx_power_qdbm = 52,
        .bandwidth     = WIFI_BW20,
        .protocols     = WIFI_PROTOCOL_11A | WIFI_PROTOCOL_11N | WIFI_PROTOCOL_11AC | WIFI_PROTOCOL_11AX,
        .scan_type     = WIFI_SCAN_TYPE_PASSIVE,
        .active_min_ms = 0,          /* not used: this band scans passively */
        .active_max_ms = 0,
        .passive_ms    = 300,
    },

    /* Only the declared 5470-5725 MHz band, channels 100 to 140. 36-64 and 144-177 are never
     * scanned. */
    .scan_5g = {
        FCK_C5_CHANNEL_5G(100), FCK_C5_CHANNEL_5G(104), FCK_C5_CHANNEL_5G(108), FCK_C5_CHANNEL_5G(112),
        FCK_C5_CHANNEL_5G(116), FCK_C5_CHANNEL_5G(120), FCK_C5_CHANNEL_5G(124), FCK_C5_CHANNEL_5G(128),
        FCK_C5_CHANNEL_5G(132), FCK_C5_CHANNEL_5G(136), FCK_C5_CHANNEL_5G(140),
    },

    /* Under the ES table the driver takes 36-64 and 100-140 and refuses 144-165. */
    .listen_5g = {
        FCK_C5_CHANNEL_5G(36),  FCK_C5_CHANNEL_5G(40),  FCK_C5_CHANNEL_5G(44),  FCK_C5_CHANNEL_5G(48),
        FCK_C5_CHANNEL_5G(52),  FCK_C5_CHANNEL_5G(56),  FCK_C5_CHANNEL_5G(60),  FCK_C5_CHANNEL_5G(64),
        FCK_C5_CHANNEL_5G(100), FCK_C5_CHANNEL_5G(104), FCK_C5_CHANNEL_5G(108), FCK_C5_CHANNEL_5G(112),
        FCK_C5_CHANNEL_5G(116), FCK_C5_CHANNEL_5G(120), FCK_C5_CHANNEL_5G(124), FCK_C5_CHANNEL_5G(128),
        FCK_C5_CHANNEL_5G(132), FCK_C5_CHANNEL_5G(136), FCK_C5_CHANNEL_5G(140), FCK_C5_CHANNEL_5G(144),
        FCK_C5_CHANNEL_5G(149), FCK_C5_CHANNEL_5G(153), FCK_C5_CHANNEL_5G(157), FCK_C5_CHANNEL_5G(161),
        FCK_C5_CHANNEL_5G(165),
    },

    /* Bluetooth LE: a passive scan, which never transmits. */
    .ble_scan_passive = true,
};

#endif /* FCK_C5_LIMITS_H_ */
