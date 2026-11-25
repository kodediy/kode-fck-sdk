/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file main.c
 * @brief A firmware of your own for the F*CK's ESP32-C5, to start from.
 *
 * It speaks the SDK's line protocol on UART0 at 115200, the way the factory
 * firmware does (firmware/fck-c5): one command per line, no prompt and
 * no echo, and every command ends with "#DONE <command> rc=<n>", rc 0 done,
 * 1 failed, 2 not understood. Lines the Kode's SDK reads start with '#'.
 *
 *   info               the greeting, fck_c5_hello.h (also sent at boot)
 *   scan [2g|5g|all]   #AP ssid="<escaped>" bssid= ch= band= rssi= auth= per
 *                      AP, then #SCANDONE n= ms=; the firmware's own lines
 *
 * A command that fails for a reason a program should know says it in a
 * "#ERR cmd=<command> why=<reason>" line before its #DONE, as the nRF firmware
 * does: scan says why=lim_<limit> when the driver refused a radio limit.
 *
 * `scan` is the example to copy: the Wi-Fi driver starts for it, inside the
 * radio limits (fck_c5_ce.h), and stops when it is done, so between commands the
 * radio is off. Add your commands to run_line().
 *
 * From the Kode, an app talks to it through the SDK's fck_command().
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/uart.h"
#include "driver/uart_vfs.h"
#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_event.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#include "fck_c5_board.h"
#include "fck_c5_ce.h"
#include "fck_c5_hello.h"

#ifdef FCK_C5_FW_NAME_BAD   /* set by main/CMakeLists.txt */
#error "FCK_C5_FW_NAME: 1 to 23 letters, digits, '_', '.' or '-', and neither fck-c5 nor c5_fck_agent. Change it in idf.py menuconfig, under F*CK C5 firmware."
#endif

/* What would close the module to the Kode for good stops the build. The Kode
 * writes the C5 through its ROM loader (fck_flash()) and reads what it carries
 * there (fck_rescue_probe()), and each of these burns eFuses the first time
 * the firmware starts: Secure Boot and flash encryption (the Kode does not
 * write a C5 with either, and by default they put the ROM loader in secure
 * download mode, where it does not even give the MAC), the bootloader's
 * anti-rollback (the ROM then refuses the official firmware's bootloader), and the
 * ROM's log off or tied to a pin (fck_rescue_probe() counts the C5's starts in
 * it). None is set in sdkconfig.defaults; this keeps a menuconfig from setting
 * one by accident. */
#if CONFIG_SECURE_BOOT || CONFIG_SECURE_FLASH_ENC_ENABLED
#error "Secure Boot and flash encryption burn eFuses on the first start: the Kode can no longer write this C5, and by default not even read it. Turn both off in idf.py menuconfig."
#endif
#if CONFIG_BOOTLOADER_ANTI_ROLLBACK_ENABLE
#error "The bootloader's anti-rollback burns an eFuse, and the ROM then refuses the official firmware's bootloader. Turn it off in idf.py menuconfig."
#endif
#if CONFIG_BOOT_ROM_LOG_ALWAYS_OFF || CONFIG_BOOT_ROM_LOG_ON_GPIO_HIGH || CONFIG_BOOT_ROM_LOG_ON_GPIO_LOW
#error "The ROM's log off, or tied to a pin, burns an eFuse, and fck_rescue_probe() counts the C5's starts in that log. Leave it always on in idf.py menuconfig."
#endif

#define CMD_MAX    256   /* a command from the Kode; the rest of a longer line is dropped */
#define OUT_MAX    320   /* the longest line out: an #AP with a fully escaped SSID */
#define AP_MAX     64    /* APs listed per scan */

static esp_err_t s_wifi = ESP_ERR_INVALID_STATE;   /* esp_wifi_init()'s result */

/* ---- Output ---------------------------------------------------------------- */

/* One line, built whole and written in one call: the UART driver holds its
 * lock for the call, so another task's line never lands inside this one. */
static void out(const char *fmt, ...)
{
    char    b[OUT_MAX];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(b, sizeof(b) - 2, fmt, ap);
    va_end(ap);
    if (n < 0) {
        return;
    }
    if (n > (int)sizeof(b) - 3) {
        n = (int)sizeof(b) - 3;
    }
    b[n++] = '\r';
    b[n++] = '\n';
    (void)uart_write_bytes(FCK_C5_HOST_UART, b, (size_t)n);
}

/* '"' and '\' get a backslash, bytes outside 0x20..0x7E become \xHH: a line
 * never carries a control character or a bare quote. */
static void escape(const uint8_t *s, size_t max, char *o, size_t on)
{
    size_t k = 0;
    for (size_t i = 0; i < max && s[i] != 0U && k + 5U < on; i++) {
        uint8_t c = s[i];
        if (c == '"' || c == '\\') {
            o[k++] = '\\';
            o[k++] = (char)c;
        } else if (c < 0x20U || c > 0x7EU) {
            k += (size_t)snprintf(o + k, on - k, "\\x%02X", c);
        } else {
            o[k++] = (char)c;
        }
    }
    o[k] = '\0';
}

/* ---- info ------------------------------------------------------------------ */

static int cmd_info(void)
{
    esp_chip_info_t ci;
    esp_chip_info(&ci);
    uint8_t m[6] = { 0 };
    (void)esp_read_mac(m, ESP_MAC_BASE);
    out(FCK_C5_HELLO_FMT, CONFIG_FCK_C5_FW_NAME, esp_app_get_description()->version,
        ci.revision / 100, ci.revision % 100, m[0], m[1], m[2], m[3], m[4], m[5],
        fck_c5_ce_region(), fck_c5_ce_state());
    return 0;
}

/* ---- scan, the example ----------------------------------------------------- */

static const char *auth_name(wifi_auth_mode_t a)
{
    switch (a) {
    case WIFI_AUTH_OPEN:                 return "open";
    case WIFI_AUTH_WEP:                  return "wep";
    case WIFI_AUTH_WPA_PSK:              return "wpa_psk";
    case WIFI_AUTH_WPA2_PSK:             return "wpa2_psk";
    case WIFI_AUTH_WPA_WPA2_PSK:         return "wpa_wpa2_psk";
    case WIFI_AUTH_WPA2_ENTERPRISE:      return "wpa2_ent";
    case WIFI_AUTH_WPA3_PSK:             return "wpa3_psk";
    case WIFI_AUTH_WPA2_WPA3_PSK:        return "wpa2_wpa3_psk";
    case WIFI_AUTH_WAPI_PSK:             return "wapi_psk";
    case WIFI_AUTH_OWE:                  return "owe";
    case WIFI_AUTH_WPA3_ENT_192:         return "wpa3_ent_192";
    case WIFI_AUTH_DPP:                  return "dpp";
    case WIFI_AUTH_WPA3_ENTERPRISE:      return "wpa3_ent";
    case WIFI_AUTH_WPA2_WPA3_ENTERPRISE: return "wpa2_wpa3_ent";
    case WIFI_AUTH_WPA_ENTERPRISE:       return "wpa_ent";
    default:                             return "unknown";
    }
}

static int cmd_scan(int argc, char **argv)
{
    const char *which = argc >= 2 ? argv[1] : "all";
    bool        two   = true;
    bool        five  = true;
    if (strcmp(which, "2g") == 0) {
        five = false;
    } else if (strcmp(which, "5g") == 0) {
        two = false;
    } else if (strcmp(which, "all") != 0) {
        out("usage: scan [2g|5g|all]");
        return 2;
    }
    if (s_wifi != ESP_OK) {
        out("wifi not available: %s", esp_err_to_name(s_wifi));
        return 1;
    }
    wifi_ap_record_t *rec = calloc(AP_MAX, sizeof(*rec));
    if (rec == NULL) {
        out("scan: no memory for %u records", (unsigned)AP_MAX);
        return 1;
    }
    uint16_t  n     = 0;
    uint16_t  total = 0;
    int64_t   t0    = esp_timer_get_time();
    esp_err_t e     = fck_c5_ce_wifi_start();
    if (e == ESP_ERR_NOT_ALLOWED) {
        /* A radio limit the driver refused: the radio stays off (fck_c5_ce.h). */
        free(rec);
        out("#ERR cmd=scan why=lim_%s", fck_c5_ce_state());
        return 1;
    }
    if (e == ESP_OK && two) {
        e = fck_c5_ce_scan_pass(false, rec, &n, AP_MAX, &total);
    }
    if (e == ESP_OK && five) {
        e = fck_c5_ce_scan_pass(true, rec, &n, AP_MAX, &total);
    }
    (void)esp_wifi_stop();   /* the radio rests off between commands */
    long ms = (long)((esp_timer_get_time() - t0) / 1000);
    if (e != ESP_OK) {
        free(rec);
        out("scan failed: %s", esp_err_to_name(e));
        return 1;
    }
    for (uint16_t i = 0; i < n; i++) {
        char           ssid[32 * 4 + 1];
        const uint8_t *b = rec[i].bssid;
        escape(rec[i].ssid, sizeof(rec[i].ssid), ssid, sizeof(ssid));
        out("#AP ssid=\"%s\" bssid=%02x:%02x:%02x:%02x:%02x:%02x ch=%u band=%s rssi=%d auth=%s",
            ssid, b[0], b[1], b[2], b[3], b[4], b[5], rec[i].primary,
            rec[i].primary > 14 ? "5g" : "2g", rec[i].rssi, auth_name(rec[i].authmode));
    }
    free(rec);
    if (total > n) {
        out("(%u more APs not listed)", (unsigned)(total - n));
    }
    out("#SCANDONE n=%u ms=%ld", (unsigned)n, ms);
    return 0;
}

/* ---- The command loop ------------------------------------------------------ */

static void run_line(char *line)
{
    char *argv[4];
    int   argc = 0;
    char *save = NULL;
    for (char *t = strtok_r(line, " \t", &save); t != NULL && argc < 4;
         t = strtok_r(NULL, " \t", &save)) {
        argv[argc++] = t;
    }
    if (argc == 0) {
        return;
    }
    int rc;
    if (strcmp(argv[0], "info") == 0) {
        rc = cmd_info();
    } else if (strcmp(argv[0], "scan") == 0) {
        rc = cmd_scan(argc, argv);
    } else {
        out("unknown command: %s (info | scan [2g|5g|all])", argv[0]);
        rc = 2;
    }
    out("#DONE %s rc=%d", argv[0], rc);
}

/* The driver ready and the radio off. It is started once here, so the
 * greeting's lim= says whether the driver takes the limits; if it refused
 * one, the radio stays off. Then fck_c5_ce_wifi_start() turns it on for each
 * command that needs it. No esp_netif: add it when your firmware needs IP. */
static esp_err_t wifi_init(void)
{
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        /* Not this firmware's NVS, or an older layout of it: start it afresh. */
        (void)nvs_flash_erase();
        e = nvs_flash_init();
    }
    if (e != ESP_OK) {
        return e;
    }
    e = esp_event_loop_create_default();
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) {
        return e;
    }
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    if ((e = esp_wifi_init(&cfg)) != ESP_OK) {
        return e;
    }
    if ((e = esp_wifi_set_mode(WIFI_MODE_STA)) != ESP_OK) {
        return e;
    }
    e = fck_c5_ce_wifi_start();
    if (e == ESP_ERR_NOT_ALLOWED) {
        return ESP_OK;   /* the greeting names the limit, and scan refuses */
    }
    if (e != ESP_OK) {
        return e;
    }
    return esp_wifi_stop();
}

void app_main(void)
{
    const uart_config_t c = {
        .baud_rate  = FCK_C5_HOST_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    (void)uart_driver_install(FCK_C5_HOST_UART, 1024, 2048, 0, NULL, 0);
    (void)uart_param_config(FCK_C5_HOST_UART, &c);
    (void)uart_set_pin(FCK_C5_HOST_UART, FCK_C5_HOST_TX, FCK_C5_HOST_RX,
                       UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    /* printf and ESP_LOGE go through the driver too, so they cannot split one
     * of the protocol's lines. A panic does not: its handler writes to the
     * UART directly and may land inside a line, which the Kode reads as
     * noise. */
    uart_vfs_dev_use_driver(FCK_C5_HOST_UART);

    s_wifi = wifi_init();

    /* The ROM's banner may end mid-line: the greeting starts on a fresh one. */
    (void)uart_write_bytes(FCK_C5_HOST_UART, "\r\n", 2);
    (void)cmd_info();
    if (s_wifi != ESP_OK) {
        out("wifi init failed: %s (scan will refuse)", esp_err_to_name(s_wifi));
    }

    char    line[CMD_MAX];
    size_t  len = 0;
    uint8_t b[64];
    for (;;) {
        int got = uart_read_bytes(FCK_C5_HOST_UART, b, sizeof(b), pdMS_TO_TICKS(100));
        for (int i = 0; i < got; i++) {
            uint8_t ch = b[i];
            if (ch == '\n' || ch == '\r') {
                line[len] = '\0';
                if (len > 0U) {
                    run_line(line);
                }
                len = 0;
                continue;
            }
            if (ch < 0x20U || ch > 0x7EU) {
                continue;   /* noise while the Kode resets or writes this chip */
            }
            if (len < sizeof(line) - 1U) {
                line[len++] = (char)ch;
            }
        }
    }
}
