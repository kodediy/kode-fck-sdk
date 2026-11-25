/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file fck_c5_limits.c
 * @brief The module's radio limits on the Wi-Fi driver. See fck_c5_limits.h.
 *
 * The numbers and the calls are the official firmware's (its wifi_up() and
 * scan_pass()). Two changes:
 * - The firmware rests on both bands at the lower power, 13 dBm, and this rests
 *   on 2.4 GHz only at 15 dBm, because a firmware of your own may associate,
 *   and on 5 GHz the module may only listen.
 * - The firmware reports a refused limit, goes on and refuses `scan` itself.
 *   A firmware of your own may transmit in other ways, so here a refused
 *   limit keeps the radio off.
 */
#include "fck_c5_limits.h"

#include "sdkconfig.h"

/* What the limits cannot check while running is checked here: an access
 * point, a Bluetooth role or controller that may transmit, or 802.15.4.
 * Not whether an observer scans passively: an active scan
 * sends SCAN_REQs, and that is chosen at run time. */
#if CONFIG_ESP_WIFI_SOFTAP_SUPPORT
#error "Radio limits: Wi-Fi as a station only, no access point. Turn CONFIG_ESP_WIFI_SOFTAP_SUPPORT off."
#endif
#if CONFIG_BT_BLUEDROID_ENABLED
#error "Radio limits: Bluedroid can advertise. Use NimBLE in the observer role only."
#endif
#if CONFIG_BT_CONTROLLER_ONLY
#error "Radio limits: a Bluetooth controller without a host sends whatever its host asks for. Use NimBLE in the observer role only."
#endif
#if CONFIG_BT_NIMBLE_ROLE_BROADCASTER || CONFIG_BT_NIMBLE_ROLE_PERIPHERAL || CONFIG_BT_NIMBLE_ROLE_CENTRAL
#error "Radio limits: Bluetooth LE may only observe. Leave CONFIG_BT_NIMBLE_ROLE_OBSERVER alone."
#endif
#if CONFIG_IEEE802154_ENABLED
#error "Radio limits: no 802.15.4 on this module. Turn it off."
#endif

/* Country ES with 802.11d off: the ES table (2.4 GHz 1-13, 5 GHz 36-64 and
 * 100-140) and never an AP's country. */
#define LIMITS_COUNTRY        "ES"
/* The module's maximum powers. esp_wifi_set_max_tx_power() takes quarters
 * of a dBm. */
#define LIMITS_QDBM_2G        60          /* 15.0 dBm */
#define LIMITS_QDBM_5G        52          /* 13.0 dBm */
/* channel_bitmap: bits 1-13 are 2.4 GHz channels 1-13; bits 9-19 of the
 * 5 GHz map are channels 100-140, the only 5 GHz channels the module may
 * use. */
#define LIMITS_SCAN_2G_MASK   0x3FFEU
#define LIMITS_SCAN_5G_MASK   0x000FFE00UL
/* Three beacons at the usual 102.4 ms interval. */
#define LIMITS_5G_LISTEN_MS   300

static esp_err_t   s_first = ESP_OK;
static const char *s_what  = "ok";

/* Keeps the first limit the driver refuses: the greeting reports it, and from
 * then on the radio stays off. */
static void limit(esp_err_t e, const char *what)
{
    if (e != ESP_OK && s_first == ESP_OK) {
        s_first = e;
        s_what  = what;
    }
}

esp_err_t fck_c5_limits_wifi_start(void)
{
    limit(esp_wifi_set_country_code(LIMITS_COUNTRY, false), "country");
    wifi_bandwidths_t bw = { .ghz_2g = WIFI_BW20, .ghz_5g = WIFI_BW20 };
    limit(esp_wifi_set_bandwidths(WIFI_IF_STA, &bw), "bw20");
    /* No 802.11b: a probe request leaves as OFDM. */
    wifi_protocols_t pr = {
        .ghz_2g = WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N | WIFI_PROTOCOL_11AX,
        .ghz_5g = WIFI_PROTOCOL_11A | WIFI_PROTOCOL_11N | WIFI_PROTOCOL_11AC
                  | WIFI_PROTOCOL_11AX,
    };
    limit(esp_wifi_set_protocols(WIFI_IF_STA, &pr), "no11b");
    if (s_first != ESP_OK) {
        return ESP_ERR_NOT_ALLOWED;   /* refused now or before: not started */
    }
    esp_err_t e = esp_wifi_start();
    if (e != ESP_OK) {
        return e;
    }
    /* Both only take once the driver has started. */
    limit(esp_wifi_set_band_mode(WIFI_BAND_MODE_2G_ONLY), "band");
    limit(esp_wifi_set_max_tx_power(LIMITS_QDBM_2G), "power");
    int8_t q = 0;
    if (esp_wifi_get_max_tx_power(&q) == ESP_OK && q > LIMITS_QDBM_2G) {
        limit(ESP_FAIL, "power-readback");
    }
    if (s_first != ESP_OK) {
        (void)esp_wifi_stop();
        return ESP_ERR_NOT_ALLOWED;
    }
    return ESP_OK;
}

esp_err_t fck_c5_limits_scan_pass(bool five, wifi_ap_record_t *rec, uint16_t *n,
                                  uint16_t cap, uint16_t *total)
{
    if (s_first != ESP_OK) {
        return ESP_ERR_NOT_ALLOWED;   /* for a driver started some other way */
    }
    wifi_scan_config_t sc = { .show_hidden = true };
    esp_err_t          e  = esp_wifi_set_band_mode(five ? WIFI_BAND_MODE_5G_ONLY
                                                        : WIFI_BAND_MODE_2G_ONLY);
    if (e == ESP_OK) {
        e = esp_wifi_set_max_tx_power(five ? LIMITS_QDBM_5G : LIMITS_QDBM_2G);
    }
    if (five) {
        /* A DFS band: nothing may be sent before the AP is heard, and a probe
         * request is sent. So 5 GHz is only listened to. */
        sc.scan_type                     = WIFI_SCAN_TYPE_PASSIVE;
        sc.scan_time.passive             = LIMITS_5G_LISTEN_MS;
        sc.channel_bitmap.ghz_2_channels = 1U;   /* bit 0: no 2.4 GHz */
        sc.channel_bitmap.ghz_5_channels = LIMITS_SCAN_5G_MASK;
    } else {
        sc.scan_type                     = WIFI_SCAN_TYPE_ACTIVE;
        sc.channel_bitmap.ghz_2_channels = LIMITS_SCAN_2G_MASK;
        sc.channel_bitmap.ghz_5_channels = 1U;   /* bit 0: no 5 GHz */
    }
    if (e == ESP_OK) {
        e = esp_wifi_scan_start(&sc, true);
    }
    if (e == ESP_OK) {
        uint16_t heard = 0;
        (void)esp_wifi_scan_get_ap_num(&heard);
        *total += heard;
        uint16_t room = (uint16_t)(cap - *n);
        uint16_t got  = heard < room ? heard : room;
        if (got > 0U) {
            e = esp_wifi_scan_get_ap_records(&got, rec + *n);
            if (e == ESP_OK) {
                *n = (uint16_t)(*n + got);
            }
        }
    }
    /* The records are out, or never will be: free the driver's list, then go
     * back to resting. */
    (void)esp_wifi_clear_ap_list();
    (void)esp_wifi_set_band_mode(WIFI_BAND_MODE_2G_ONLY);
    (void)esp_wifi_set_max_tx_power(LIMITS_QDBM_2G);
    return e;
}

const char *fck_c5_limits_region(void)
{
    return "EU";
}

const char *fck_c5_limits_state(void)
{
    return s_first == ESP_OK ? "ok" : s_what;
}
