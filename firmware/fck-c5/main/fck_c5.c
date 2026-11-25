/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file fck_c5.c
 * @brief fck-c5: the ESP32-C5 (U501) firmware of the F*CK module, the one the SDK talks to.
 *
 * KDFCK001 rev 1.0 wiring:
 *   C5 U0TXD IO11 -> J401.7 -> P4 GPIO12        C5 U0RXD IO12 <- J401.5 <- P4 GPIO10
 *   GNSS TXD (U801.2) -> C5 IO4 (UART1 RX)       GNSS RXD (U801.3) <- C5 IO5 (UART1 TX)
 *   GNSS 1PPS -> C5 IO6                          C5 EN / IO28 (BOOT) <- P4 GPIO8 / GPIO9
 *
 * The module's radio limits are one table, FCK_C5_LIMITS in fck_c5_limits.h. This file
 * reads every radio number from it and holds none of its own.
 *
 * Line protocol on UART0 at 115200, read by the SDK's fck_drv_c5 (fck_drv_c5.h). One command per line, no
 * prompt, no echo; every command ends with "#DONE <cmd> rc=<n>" (0 ok, 1 failed, 2 bad usage):
 *   info               #C5INFO fw=fck-c5 ver= chip=esp32c5 rev= mac= reg= lim= caps= uptime_ms= rst=
 *                      (also at boot). Asked twice without a restart in between it says the same but for
 *                      uptime_ms=, which is how the SDK tells a restart: it compares the two lines and
 *                      skips the words that start with "uptime", as it does for the nRF's #NRFINFO
 *                        ver=   the project's version, as major.minor
 *                        reg=   the region the limits are for
 *                        lim=   "ok" once Wi-Fi is up with every limit of the table in force. Otherwise
 *                               what stopped it: the step of the bring-up that failed (wifi-loop,
 *                               wifi-init, wifi-mode, wifi-start) or the first limit the driver refused
 *                               (country, bw20, no11b, power, power-readback)
 *                        caps=  what this firmware does, a word per command (FW_CAPS)
 *                        uptime_ms= milliseconds since it started
 *                        rst=   why it started: poweron, ext, sw, panic, task_wdt, brownout, ... (rst_name())
 *   health             #C5HEALTH uptime_ms= rst= heap= heap_min=   what changes while it runs, and so is
 *                      kept out of #C5INFO: the free heap now, in bytes, and the least it has been
 *   scan [2g|5g|all]   #AP ssid="<escaped>" bssid= ch= band=<2g/5g> rssi= auth=   per AP,
 *                      then #SCANDONE n= ms=. Each band on the channels, and actively or passively, as the
 *                      table says. The only command that transmits: refused with rc=1, before anything
 *                      goes on the air, while lim= is not "ok". `chan` and `ble scan` only listen and
 *                      do not look at lim=
 *   gnss on|off        from then on, asynchronously: #GNSSU <sentence> per line from the GNSS
 *                      UART and #PPS n= t_ms= per 1PPS rising edge
 *   chan [2g|5g|all] [dwell_ms=250]
 *                      channel activity survey in promiscuous mode, one line per channel:
 *                      #CHAN ch= band= frames= mgmt= data= ctrl= rssi_max= rssi_avg= dwell_ms=
 *                      then #CHANDONE n= ms=. Counts and RSSI only: no address, no payload.
 *   frames ch=<n> [ms=250]
 *                      listens on one channel (2.4 GHz 1-13, or a 5 GHz one chan knows) in promiscuous
 *                      mode for ms (20..5000) and tells the frames it hears, one line each, as they
 *                      come: #F <base64>, a record of 24..70 bytes (frame control, channel, RSSI, length
 *                      on the air, the three addresses, and a head of at most 46 bytes: the fixed
 *                      fields and the name tag of a beacon / probe response, the name tag of a probe
 *                      request, the reason of a deauthentication / disassociation; a data frame's
 *                      payload is never read). To fit the UART a run tells each sender once (beacon,
 *                      probe response: per address 2; probe request: per address 2 and name; data: per
 *                      address 1 and 2) and every deauthentication / disassociation; the rest is
 *                      counted. At the end: #FRAMES ch= ms= heard= mgmt= data= ctrl= beacon=
 *                      probe_req= probe_resp= deauth= disassoc= sent= dropped=. Receives only.
 *   ble scan [secs=5]  passive BLE scan (NimBLE, observer role), one line per advertiser:
 *                      #BLE addr= type=<pub|rnd> rssi= name="<escaped>" adv=<reports>
 *                           evt=<0..4> first=<ms> last=<ms> data=<hex>
 *                      then #BLEDONE n= ms=. rc=1 when the scan could not start or was cut short;
 *                      what it heard until then is still listed. evt is the advertising PDU of its
 *                      latest report, as the controller numbers them (0 ADV_IND, 1 ADV_DIRECT_IND,
 *                      2 ADV_SCAN_IND, 3 ADV_NONCONN_IND; 4, a scan response, never comes: the scan
 *                      only listens); first and last, when its first and its latest report were
 *                      heard, from the start of the scan; data, what its latest report that carried
 *                      anything carried, as sent (AD structures, at most 31 bytes, two digits each),
 *                      and no data= at all when none did.
 *   wp [0|1|in]        wake lines: #WP host_wp_c5= c5_wp_host= driven=
 *   gtx <body>         one NMEA sentence to the GNSS UART (IO5): the firmware adds '$', the
 *                      checksum and CR LF, writes "$<body>*CS\r\n" and prints
 *                      #GTX sent="$<body>*CS". The body has no spaces, no '$' inside and no '*'
 *                      (a leading '$' is accepted and dropped). Works with the relay off; the
 *                      GNSS's answer, if any, only shows with `gnss on`.
 *
 * SSID / BLE name escaping: '"' and '\' get a backslash, bytes outside 0x20..0x7E become \xHH,
 * so a line never carries a raw control character or a bare quote.
 *
 * A line is whole or it is nothing. One that stops half way for LINE_IDLE_MS is dropped without a word, so it
 * is never glued to the next command; this holds while the loop is reading, not for bytes that arrive while
 * a command runs. One longer than CMD_MAX - 1 characters is not cut and run: it is refused with rc=2, under
 * its first word (or "?").
 *
 * The command loop is watched by the task watchdog (sdkconfig.defaults). A command that waits feeds it in
 * slices of WDT_SLICE_MS, however long it runs; a loop that hangs is restarted with the C5, by a panic that
 * prints on this UART, and the firmware greets again.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "driver/uart_vfs.h"
#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_event.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"
#include "esp_wifi.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "fck_c5_limits.h"

/* The version is the project's, set once in the top CMakeLists.txt: #C5INFO reads it from the app descriptor.
 * Before the public line, which starts again at 0.x, it was counted 1.1 (chan, ble scan), 1.2 (wp), 1.3 (gtx),
 * 1.4 (gtx -n/loop), 1.5 (relay lock-up fix), 2.0 (the radio limits, reg= and lim= in #C5INFO), 2.1
 * (named fck-c5, c5_fck_agent before), 2.2 (ble scan lists what each advertiser sent) and 2.4 (frames); no
 * 2.3 came from that line. On the public line: 0.1.0 (caps=, health), 0.2.0 (2.2's and 2.4's commands, with
 * their words in caps=). */
#define FW_NAME       "fck-c5"
/* caps= of #C5INFO: what this firmware does, one word per command, for the SDK to ask by instead of by
 * version. A word is a promise about its command as the file header describes it:
 *   scan   `scan [2g|5g|all]`, inside the limits of fck_c5_limits.h
 *   chan   `chan [2g|5g|all] [dwell_ms]`
 *   ble    `ble scan [secs]`
 *   bleadv `ble scan` says what each advertiser sent: evt=, first=, last= and data= in every #BLE
 *   frames `frames ch=<n> [ms]`, with #F and #FRAMES; it only listens
 *   gnss   `gnss on|off`, with #GNSSU and #PPS; a `gnss on` while it is on is harmless
 *   wp     `wp [0|1|in]`
 *   gtx    `gtx <body>`, `gtx -n <body>` and `gtx loop`
 *   health `health`, with #C5HEALTH
 * A command added later adds its word; a word is never taken back while its command is there. */
#define FW_CAPS       "scan,chan,ble,bleadv,frames,gnss,wp,gtx,health"

#define HOST_UART     UART_NUM_0
#define HOST_TX       11
#define HOST_RX       12
#define GNSS_UART     UART_NUM_1
#define GNSS_RX       4         /* from GNSS TXD */
#define GNSS_TX       5         /* to GNSS RXD */
#define PIN_PPS       6
#define BAUD          115200

#define CMD_MAX       256       /* commands from the P4 */
#define OUT_MAX       320       /* longest line out: a #BLE with a name escaped byte by byte and a whole
                                 * advertisement in data=, under 300 */
#define NMEA_MAX      200       /* NMEA 0183 caps sentences at 82; proprietary ones run longer */
#define GTX_BODY_MAX  190       /* gtx: "$" + body + "*CS\r\n" stays within NMEA_MAX */
#define AP_MAX        64
#define VERB_MAX      15        /* the longest verb the SDK's link matches in a #DONE */
/* What the SDK keeps of a #C5INFO after its tag (its FCK_AOS_FIRMWARE_MAX, less the terminator). A longer one
 * it forgets whole. */
#define INFO_BODY_MAX 191
/* A line stops when nothing has come for this long: what there was of it is dropped. The loop reads in
 * windows of half of it (see app_main()), which is also how often an idle command loop turns. */
#define LINE_IDLE_MS  100

/* The task watchdog (sdkconfig.defaults: CONFIG_ESP_TASK_WDT_TIMEOUT_S, with panic) restarts the C5 when the
 * command loop stops feeding it. A command that waits does it in slices of this, feeding after each, so the
 * longest command is as safe as the shortest and only a loop that is stuck goes hungry. */
#define WDT_SLICE_MS  500
/* A scan the driver does not end is stopped: after its channels' times, this per channel for the hop, and
 * this once. The SDK gives a `scan` 20 s. */
#define SCAN_HOP_MS   100
#define SCAN_SLACK_MS 5000

/* The module's radio limits (fck_c5_limits.h). One image, no region logic: the user may
 * replace it from the Kode, this one is the firmware the module leaves the factory with. */
static const fck_c5_limits_t *const k_lim = &FCK_C5_LIMITS;

/* 1PPS is a ~100 ms pulse once per second: an edge closer than this to the previous one is
 * ringing, not a second pulse. */
#define PPS_MIN_GAP_US 100000

#define CHAN_DWELL_DEF 250      /* ms per channel */
#define CHAN_DWELL_MIN 20
#define CHAN_DWELL_MAX 5000
#define CHAN_SETTLE_MS 20       /* after a hop, before the counters open */
#define FR_FIXED       24       /* frames: fixed part of a record (see the file header) */
#define FR_HEAD_MAX    46       /* beacon: 12 fixed bytes + a name tag of 2 + 32 */
#define FR_REC_MAX     (FR_FIXED + FR_HEAD_MAX)
#define FR_RING        48       /* records waiting for the command loop (the UART is the slow part) */
#define FR_SEEN        128      /* senders remembered per run (open addressing, 32-bit hashes) */
#define RSSI_NONE      (-127)   /* rssi_max / rssi_avg of a channel where no frame was heard */
#define BLE_SECS_DEF   5
#define BLE_SECS_MAX   60
#define BLE_MAX        128      /* advertisers kept per scan (random addresses rotate) */
#define BLE_NAME_MAX   31       /* a legacy advertisement carries at most 31 bytes of AD */
#define BLE_DATA_MAX   31       /* and those 31 are what data= lists: 62 digits on the line */

static esp_err_t s_wifi_err = ESP_ERR_INVALID_STATE;
/* lim= of #C5INFO. "ok" only once wifi_up() has brought Wi-Fi up with every limit taken. Until then, and for
 * good if it fails, the step of the bring-up it is at, or the first limit the driver refused. */
static const char *s_lim = "wifi-loop";
static const char *s_lim_refused;       /* the first limit the driver refused; NULL while all of them hold */
static bool s_wdt;                      /* the command loop is subscribed to the task watchdog */
static SemaphoreHandle_t s_scan_done;   /* given by WIFI_EVENT_SCAN_DONE */
static volatile uint32_t s_scan_status; /* that event's status: 0 when the scan succeeded */
static volatile bool s_gnss_on;
static bool s_gnss_uart;
static TaskHandle_t s_gnss_task, s_pps_task;
static volatile uint32_t s_pps_n;
static volatile int64_t s_pps_us;
static uint8_t s_fr_last_ch;            /* channel the last `frames` run left; 0 after `scan` / `chan` / a failure */

/* ----- output ----- */

/**
 * One protocol line on UART0. Built whole and handed to the driver in one uart_write_bytes()
 * call: the driver holds its TX mutex for the whole call, so the command loop, the GNSS relay
 * and the PPS task never interleave inside a line.
 */
static void out(const char *fmt, ...)
{
    char b[OUT_MAX];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(b, sizeof b - 2, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof b - 3) n = sizeof b - 3;
    b[n++] = '\r';
    b[n++] = '\n';
    uart_write_bytes(HOST_UART, b, n);
}

/* ----- watchdog ----- */

/* The command loop is alive. Only that loop calls this, and the waits below, which are its own. */
static void wdt_feed(void)
{
    if (s_wdt) esp_task_wdt_reset();
}

/* vTaskDelay(ms), fed. */
static void wait_fed(uint32_t ms)
{
    while (ms) {
        uint32_t slice = ms < WDT_SLICE_MS ? ms : WDT_SLICE_MS;
        vTaskDelay(pdMS_TO_TICKS(slice));
        wdt_feed();
        ms -= slice;
    }
}

/* xSemaphoreTake(sem, ms), fed: true when the semaphore was given within ms. */
static bool take_fed(SemaphoreHandle_t sem, uint32_t ms)
{
    for (;;) {
        uint32_t slice = ms < WDT_SLICE_MS ? ms : WDT_SLICE_MS;
        bool got = xSemaphoreTake(sem, pdMS_TO_TICKS(slice)) == pdTRUE;
        wdt_feed();
        if (got) return true;
        ms -= slice;
        if (!ms) return false;
    }
}

/* ----- info ----- */

/* How much of a version is its major.minor: "0.1" of "0.1.0", the form every SDK reads in ver=. */
static int ver_short(const char *v)
{
    const char *dot = strchr(v, '.');
    if (dot) dot = strchr(dot + 1, '.');
    return dot ? (int)(dot - v) : (int)strlen(v);
}

/* rst= of #C5INFO and #C5HEALTH: why the C5 started this time, in a word of at most eight characters. */
static const char *rst_name(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_POWERON:    return "poweron";
    case ESP_RST_EXT:        return "ext";
    case ESP_RST_SW:         return "sw";
    case ESP_RST_PANIC:      return "panic";
    case ESP_RST_INT_WDT:    return "int_wdt";
    case ESP_RST_TASK_WDT:   return "task_wdt";     /* the command loop hung, and the watchdog restarted it */
    case ESP_RST_WDT:        return "wdt";
    case ESP_RST_DEEPSLEEP:  return "sleep";
    case ESP_RST_BROWNOUT:   return "brownout";
    case ESP_RST_SDIO:       return "sdio";
    case ESP_RST_USB:        return "usb";
    case ESP_RST_JTAG:       return "jtag";
    case ESP_RST_EFUSE:      return "efuse";
    case ESP_RST_PWR_GLITCH: return "glitch";
    case ESP_RST_CPU_LOCKUP: return "lockup";
    default:                 return "unknown";
    }
}

static int cmd_info(void)
{
    esp_chip_info_t ci;
    esp_chip_info(&ci);
    uint8_t m[6] = { 0 };
    esp_read_mac(m, ESP_MAC_BASE);
    const char *ver = esp_app_get_description()->version;    /* PROJECT_VER of the top CMakeLists.txt */
    /* The keys every version has printed come first and in their order; the newer ones follow. Nothing here
     * changes between two `info` of one boot but uptime_ms=, which the SDK skips when it compares them: a
     * value that moves (the heap) belongs in `health`. The line is built here to be measured, and out() copies
     * it into a buffer of its own: static, so the two do not stack up on the command loop's task, the only
     * one that calls this. */
    static char line[OUT_MAX];
    int id = snprintf(line, sizeof line,
                      "#C5INFO fw=%s ver=%.*s chip=esp32c5 rev=%d.%d mac=%02x:%02x:%02x:%02x:%02x:%02x reg=%s lim=%s",
                      FW_NAME, ver_short(ver), ver, ci.revision / 100, ci.revision % 100, m[0], m[1], m[2], m[3],
                      m[4], m[5], k_lim->region, s_lim);
    if (id < 0 || id >= (int)sizeof line) return 1;
    int caps = id + snprintf(line + id, sizeof line - id, " caps=%s", FW_CAPS);
    int all = caps;
    if (caps < (int)sizeof line) {
        all = caps + snprintf(line + caps, sizeof line - caps, " uptime_ms=%lld rst=%s",
                              (long long)(esp_timer_get_time() / 1000), rst_name(esp_reset_reason()));
    }
    /* A greeting the SDK cannot keep is worse than one without its newer keys: it would not know what the
     * C5 runs. As the keys are today a usual line is 152 characters after the tag, and 179 with every
     * field at its longest. What goes first when it does not fit is what only informs, uptime_ms= and
     * rst= (`health` has both): without caps= the SDK would ask this firmware by a version that says
     * nothing, so caps= goes last, and only when the line does not fit even without the other two. */
    const int tag = (int)strlen("#C5INFO ");
    if (all - tag > INFO_BODY_MAX) {
        line[(caps < (int)sizeof line && caps - tag <= INFO_BODY_MAX) ? caps : id] = 0;
    }
    out("%s", line);
    return 0;
}

/* health: how it is doing, for whoever watches a module over time. These numbers move, so they have a line
 * of their own and stay out of #C5INFO, which has to read the same every time within a boot. */
static int cmd_health(void)
{
    out("#C5HEALTH uptime_ms=%lld rst=%s heap=%lu heap_min=%lu", (long long)(esp_timer_get_time() / 1000),
        rst_name(esp_reset_reason()), (unsigned long)esp_get_free_heap_size(),
        (unsigned long)esp_get_minimum_free_heap_size());
    return 0;
}

/* ----- Wi-Fi scan ----- */

/* Records the first limit the driver refuses; #C5INFO reports it as lim=<what>. */
static void limit(esp_err_t e, const char *what)
{
    if (e != ESP_OK && !s_lim_refused) s_lim_refused = what;
}

/* The cap the driver rests at between scans, the lower of the two bands'; and the higher, which no reading
 * of it may pass. */
static int8_t tx_power_rest(void)
{
    return k_lim->band_2g.tx_power_qdbm < k_lim->band_5g.tx_power_qdbm ? k_lim->band_2g.tx_power_qdbm
                                                                       : k_lim->band_5g.tx_power_qdbm;
}

static int8_t tx_power_most(void)
{
    return k_lim->band_2g.tx_power_qdbm > k_lim->band_5g.tx_power_qdbm ? k_lim->band_2g.tx_power_qdbm
                                                                       : k_lim->band_5g.tx_power_qdbm;
}

/* The default event loop's task, when a scan ends, by itself or stopped. */
static void on_scan_done(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)id;
    s_scan_status = data ? ((const wifi_event_sta_scan_done_t *)data)->status : 1U;
    xSemaphoreGive(s_scan_done);
}

static esp_err_t wifi_up(void)
{
    /* No NVS (see sdkconfig.defaults) and no esp_netif / lwIP: a scan never passes a data
     * frame up, and leaving the IP stack out keeps the image small enough for the P4 to
     * embed. The driver still posts its events, so the default loop must exist. */
    /* s_lim names the step being taken, so a return from any of them leaves lim= saying which one failed:
     * the limits are confirmed only by getting to the end. Each word fits the 15 characters the SDK keeps. */
    s_lim = "wifi-loop";
    esp_err_t e = esp_event_loop_create_default();
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) return e;
    /* `scan` waits for the end of a scan here instead of inside the driver, so that it can feed the watchdog
     * while it waits and give up on a scan that never ends. */
    if (!s_scan_done && !(s_scan_done = xSemaphoreCreateBinary())) return ESP_ERR_NO_MEM;
    if ((e = esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_SCAN_DONE, on_scan_done, NULL)) != ESP_OK) return e;
    s_lim = "wifi-init";
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    if ((e = esp_wifi_init(&cfg)) != ESP_OK) return e;
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    s_lim = "wifi-mode";
    if ((e = esp_wifi_set_mode(WIFI_MODE_STA)) != ESP_OK) return e;
    limit(esp_wifi_set_country_code(k_lim->country, k_lim->country_from_ap), "country");
    wifi_bandwidths_t bw = { .ghz_2g = k_lim->band_2g.bandwidth, .ghz_5g = k_lim->band_5g.bandwidth };
    limit(esp_wifi_set_bandwidths(WIFI_IF_STA, &bw), "bw20");
    wifi_protocols_t pr = { .ghz_2g = k_lim->band_2g.protocols, .ghz_5g = k_lim->band_5g.protocols };
    limit(esp_wifi_set_protocols(WIFI_IF_STA, &pr), "no11b");
    s_lim = "wifi-start";
    if ((e = esp_wifi_start()) != ESP_OK) return e;
    limit(esp_wifi_set_max_tx_power(tx_power_rest()), "power");    /* only valid once started */
    int8_t q = 0;
    if (esp_wifi_get_max_tx_power(&q) == ESP_OK && q > tx_power_most()) limit(ESP_FAIL, "power-readback");
    s_lim = s_lim_refused ? s_lim_refused : "ok";
    return ESP_OK;
}

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

/* Escapes up to `max` bytes of `s` (stopping at a NUL) into `o`; see the file header. */
static void escape_str(const uint8_t *s, size_t max, char *o, size_t on)
{
    size_t k = 0;
    for (size_t i = 0; i < max && s[i] && k + 5 < on; i++) {
        uint8_t c = s[i];
        if (c == '"' || c == '\\') { o[k++] = '\\'; o[k++] = (char)c; }
        else if (c < 0x20 || c > 0x7E) k += snprintf(o + k, on - k, "\\x%02X", c);
        else o[k++] = (char)c;
    }
    o[k] = 0;
}

/* One pass per band, on the channels, at the power, of the type (active sends probe requests, passive
 * transmits nothing) and for the times the table gives that band. The records of the pass are appended to
 * rec[]. */
static esp_err_t scan_pass(bool five, wifi_ap_record_t *rec, uint16_t *n, uint16_t cap, uint16_t *total)
{
    const fck_c5_band_limits_t *band = five ? &k_lim->band_5g : &k_lim->band_2g;
    wifi_scan_config_t sc = { .show_hidden = true, .scan_type = band->scan_type };
    sc.scan_time.active.min = band->active_min_ms;
    sc.scan_time.active.max = band->active_max_ms;
    sc.scan_time.passive = band->passive_ms;
    /* The band's channels as the driver's bitmap: a 2.4 GHz channel's bit is its number, a 5 GHz one's is
     * in the table. */
    uint32_t want = 0;
    if (five) {
        for (size_t i = 0; i < FCK_C5_CHANNELS_5G_MAX && k_lim->scan_5g[i].number; i++) want |= k_lim->scan_5g[i].bit;
    } else {
        for (unsigned ch = k_lim->channel_2g_first; ch <= k_lim->channel_2g_last; ch++) want |= 1UL << ch;
    }
    /* an empty bitmap would mean every channel of the band: with none in the table, none is scanned */
    if (!want) return ESP_OK;
    sc.channel_bitmap.ghz_2_channels = five ? 1U : (uint16_t)want;    /* bit 0: leave that band out */
    sc.channel_bitmap.ghz_5_channels = five ? want : 1U;
    esp_err_t e = esp_wifi_set_band_mode(five ? WIFI_BAND_MODE_5G_ONLY : WIFI_BAND_MODE_2G_ONLY);
    if (e != ESP_OK) return e;
    if ((e = esp_wifi_set_max_tx_power(band->tx_power_qdbm)) != ESP_OK) return e;

    /* Not a blocking scan: the driver says when it ends (on_scan_done()), and meanwhile the loop feeds the
     * watchdog. A scan that has not ended after its channels' times and some slack is stopped, and fails. */
    uint32_t per_channel = band->scan_type == WIFI_SCAN_TYPE_ACTIVE ? band->active_max_ms : band->passive_ms;
    uint32_t cap_ms = (uint32_t)__builtin_popcountl(want) * (per_channel + SCAN_HOP_MS) + SCAN_SLACK_MS;
    xSemaphoreTake(s_scan_done, 0);                     /* an end that nobody waited for */
    if ((e = esp_wifi_scan_start(&sc, false)) != ESP_OK) return e;
    if (!take_fed(s_scan_done, cap_ms)) {
        esp_wifi_scan_stop();
        xSemaphoreTake(s_scan_done, pdMS_TO_TICKS(WDT_SLICE_MS));    /* the end that the stop brings */
        esp_wifi_clear_ap_list();
        return ESP_ERR_TIMEOUT;
    }
    if (s_scan_status) { esp_wifi_clear_ap_list(); return ESP_FAIL; }
    uint16_t t = 0;
    esp_wifi_scan_get_ap_num(&t);
    *total += t;
    uint16_t got = t < (uint16_t)(cap - *n) ? t : (uint16_t)(cap - *n);
    if (!got) { esp_wifi_clear_ap_list(); return ESP_OK; }
    if ((e = esp_wifi_scan_get_ap_records(&got, rec + *n)) != ESP_OK) { esp_wifi_clear_ap_list(); return e; }
    *n += got;
    return ESP_OK;
}

static int cmd_scan(int argc, char **argv)
{
    const char *which = argc >= 2 ? argv[1] : "all";
    bool do2 = true, do5 = true;
    if (!strcmp(which, "all")) { }
    else if (!strcmp(which, "2g")) do5 = false;
    else if (!strcmp(which, "5g")) do2 = false;
    else { out("usage: scan [2g|5g|all]"); return 2; }
    if (s_wifi_err != ESP_OK) { out("wifi not available: %s", esp_err_to_name(s_wifi_err)); return 1; }
    /* `scan` is the one command that transmits (the probe requests of an active scan), so it is the one that
     * needs the limits in force, and it does not leave that to whoever sends it: with a limit the driver
     * did not take, nothing goes on the air. */
    if (strcmp(s_lim, "ok")) {
        out("scan refused: lim=%s, a limit the driver did not take, so nothing is transmitted", s_lim);
        return 1;
    }

    s_fr_last_ch = 0;                    /* the scan retunes the radio: the next `frames` settles */
    wifi_ap_record_t *rec = calloc(AP_MAX, sizeof *rec);
    if (!rec) { out("scan: out of memory for %u records", AP_MAX); return 1; }
    uint16_t n = 0, total = 0;
    int64_t t0 = esp_timer_get_time();
    esp_err_t e = ESP_OK;
    if (do2) e = scan_pass(false, rec, &n, AP_MAX, &total);
    uint16_t n2 = n;                                     /* the 2.4 GHz pass's records come first */
    if (e == ESP_OK && do5) e = scan_pass(true, rec, &n, AP_MAX, &total);
    int64_t ms = (esp_timer_get_time() - t0) / 1000;
    esp_wifi_set_band_mode(WIFI_BAND_MODE_AUTO);
    esp_wifi_set_max_tx_power(tx_power_rest());          /* resting cap: the lower of the two */
    if (e != ESP_OK) { free(rec); out("scan failed: %s", esp_err_to_name(e)); return 1; }
    for (uint16_t i = 0; i < n; i++) {
        char ssid[32 * 4 + 1];
        escape_str(rec[i].ssid, 32, ssid, sizeof ssid);
        const uint8_t *b = rec[i].bssid;
        out("#AP ssid=\"%s\" bssid=%02x:%02x:%02x:%02x:%02x:%02x ch=%u band=%s rssi=%d auth=%s", ssid,
            b[0], b[1], b[2], b[3], b[4], b[5], rec[i].primary, i < n2 ? "2g" : "5g",
            rec[i].rssi, auth_name(rec[i].authmode));
    }
    free(rec);
    if (total > n) out("(%u more APs not listed)", total - n);
    out("#SCANDONE n=%u ms=%lld", n, ms);
    return 0;
}

/* "250", or "dwell_ms=250" when key is "dwell_ms": the value, or -1 if `t` is neither form. */
static long num_arg(const char *t, const char *key)
{
    size_t kl = strlen(key);
    if (!strncmp(t, key, kl) && t[kl] == '=') t += kl + 1;
    if (*t < '0' || *t > '9') return -1;
    char *end;
    long v = strtol(t, &end, 10);
    return *end ? -1 : v;
}

/* a / b rounded to the nearest integer, half away from zero (b > 0) */
static int div_round(int32_t a, uint32_t b)
{
    int32_t h = (int32_t)(b / 2);
    return (int)(a >= 0 ? (a + h) / (int32_t)b : (a - h) / (int32_t)b);
}

/* ----- channel activity survey (promiscuous mode) ----- */

/* The candidate channels are the table's: the 2.4 GHz range, and listen_5g. The Wi-Fi driver refuses the
 * ones the table's country does not allow, and those are left out of the survey. */

typedef struct {
    uint32_t mgmt, data, ctrl;
    int32_t rssi_sum;
    int rssi_max;
} chan_cnt_t;

static chan_cnt_t s_cc;
static volatile bool s_cc_open;
static portMUX_TYPE s_cc_mux = portMUX_INITIALIZER_UNLOCKED;

/* Wi-Fi task, once per received frame: count its type and keep its RSSI. The frame itself
 * (addresses, payload) is never looked at. */
static void chan_rx(void *buf, wifi_promiscuous_pkt_type_t type)
{
    if (!s_cc_open) return;
    int rssi = ((const wifi_promiscuous_pkt_t *)buf)->rx_ctrl.rssi;
    portENTER_CRITICAL(&s_cc_mux);
    switch (type) {
    case WIFI_PKT_MGMT: s_cc.mgmt++; break;
    case WIFI_PKT_DATA: s_cc.data++; break;
    case WIFI_PKT_CTRL: s_cc.ctrl++; break;
    default: portEXIT_CRITICAL(&s_cc_mux); return;
    }
    s_cc.rssi_sum += rssi;
    if (rssi > s_cc.rssi_max) s_cc.rssi_max = rssi;
    portEXIT_CRITICAL(&s_cc_mux);
}

/* Tunes one channel, counts for `dwell` ms, prints its #CHAN line. false: the driver refused it. */
static bool chan_one(uint8_t ch, bool five, long dwell)
{
    if (esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE) != ESP_OK) return false;
    vTaskDelay(pdMS_TO_TICKS(CHAN_SETTLE_MS));
    portENTER_CRITICAL(&s_cc_mux);
    s_cc = (chan_cnt_t){ .rssi_max = -128 };
    portEXIT_CRITICAL(&s_cc_mux);
    s_cc_open = true;
    int64_t t0 = esp_timer_get_time();
    wait_fed((uint32_t)dwell);
    s_cc_open = false;
    int64_t dms = (esp_timer_get_time() - t0 + 500) / 1000;
    portENTER_CRITICAL(&s_cc_mux);
    chan_cnt_t c = s_cc;
    portEXIT_CRITICAL(&s_cc_mux);
    uint32_t fr = c.mgmt + c.data + c.ctrl;
    out("#CHAN ch=%u band=%s frames=%lu mgmt=%lu data=%lu ctrl=%lu rssi_max=%d rssi_avg=%d dwell_ms=%lld", ch,
        five ? "5g" : "2g", (unsigned long)fr, (unsigned long)c.mgmt, (unsigned long)c.data, (unsigned long)c.ctrl,
        fr ? c.rssi_max : RSSI_NONE, fr ? div_round(c.rssi_sum, fr) : RSSI_NONE, dms);
    return true;
}

static int cmd_chan(int argc, char **argv)
{
    bool g2 = true, g5 = true;
    long dwell = CHAN_DWELL_DEF, v;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "all")) g2 = g5 = true;
        else if (!strcmp(argv[i], "2g")) { g2 = true; g5 = false; }
        else if (!strcmp(argv[i], "5g")) { g2 = false; g5 = true; }
        else if ((v = num_arg(argv[i], "dwell_ms")) >= 0) dwell = v;
        else { out("usage: chan [2g|5g|all] [dwell_ms=%d]", CHAN_DWELL_DEF); return 2; }
    }
    if (dwell < CHAN_DWELL_MIN || dwell > CHAN_DWELL_MAX) {
        out("chan: dwell_ms must be %d..%d", CHAN_DWELL_MIN, CHAN_DWELL_MAX);
        return 2;
    }
    if (s_wifi_err != ESP_OK) { out("wifi not available: %s", esp_err_to_name(s_wifi_err)); return 1; }
    /* No look at lim= here, unlike `scan`: the survey only receives, so a limit the driver refused takes
     * nothing from it. With "country" refused the driver's own table decides which channels it tunes. */

    s_fr_last_ch = 0;                    /* the survey retunes the radio: the next `frames` settles */
    /* `scan 2g` / `scan 5g` leave the band pinned; hopping across both needs auto. */
    esp_err_t e = esp_wifi_set_band_mode(WIFI_BAND_MODE_AUTO);
    if (e != ESP_OK) { out("chan: band mode auto refused: %s", esp_err_to_name(e)); return 1; }
    const wifi_promiscuous_filter_t f = { .filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT | WIFI_PROMIS_FILTER_MASK_DATA |
                                                         WIFI_PROMIS_FILTER_MASK_CTRL };
    const wifi_promiscuous_filter_t cf = { .filter_mask = WIFI_PROMIS_CTRL_FILTER_MASK_ALL };
    s_cc_open = false;
    if ((e = esp_wifi_set_promiscuous_rx_cb(chan_rx)) != ESP_OK || (e = esp_wifi_set_promiscuous_filter(&f)) != ESP_OK ||
        (e = esp_wifi_set_promiscuous_ctrl_filter(&cf)) != ESP_OK || (e = esp_wifi_set_promiscuous(true)) != ESP_OK) {
        esp_wifi_set_promiscuous(false);
        out("chan: promiscuous mode refused: %s", esp_err_to_name(e));
        return 1;
    }

    int64_t t0 = esp_timer_get_time();
    unsigned n = 0;
    char skipped[128];
    size_t sk = 0;
    skipped[0] = 0;
    for (int b = 0; b < 2; b++) {
        bool five = b == 1;
        if (five ? !g5 : !g2) continue;
        for (unsigned i = 0;; i++) {
            unsigned ch;
            if (five) {
                if (i >= FCK_C5_CHANNELS_5G_MAX || !(ch = k_lim->listen_5g[i].number)) break;
            } else if ((ch = k_lim->channel_2g_first + i) > k_lim->channel_2g_last) {
                break;
            }
            if (chan_one((uint8_t)ch, five, dwell)) n++;
            else if (sk < sizeof skipped - 5) sk += snprintf(skipped + sk, sizeof skipped - sk, " %u", ch);
        }
    }
    /* back to a plain station, the way `scan` expects to find it */
    esp_wifi_set_promiscuous(false);
    if (sk) out("(chan: channels refused under country %s, not surveyed:%s)", k_lim->country, skipped);
    out("#CHANDONE n=%u ms=%lld", n, (esp_timer_get_time() - t0) / 1000);
    return n ? 0 : 1;
}

/* ----- frames: Wi-Fi frames one by one (promiscuous mode, receive only) ----- */

typedef struct {
    uint32_t heard, mgmt, data, ctrl;
    uint32_t beacon, probe_req, probe_resp, deauth, disassoc;
    uint32_t dropped;
} fr_cnt_t;

typedef struct {
    uint8_t n;                          /* record length, 24..70 */
    uint8_t b[FR_REC_MAX];
} fr_rec_t;

static fr_cnt_t s_fr;
static bool s_fr_open;                  /* read and written only inside s_fr_mux */
static uint32_t s_fr_seen[FR_SEEN];     /* hashes of the senders told; 0 = free slot */
static unsigned s_fr_seen_n;
static fr_rec_t s_fr_ring[FR_RING];
static unsigned s_fr_head, s_fr_tail, s_fr_cnt;
static portMUX_TYPE s_fr_mux = portMUX_INITIALIZER_UNLOCKED;

/* Standard base64 (RFC 4648, '=' padding) of n bytes into o, which holds 4 * ((n + 2) / 3) + 1 characters.
 * Returns the length written, without the NUL. */
static size_t b64_encode(const uint8_t *in, size_t n, char *o)
{
    static const char a[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t k = 0;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16;
        if (i + 1 < n) v |= (uint32_t)in[i + 1] << 8;
        if (i + 2 < n) v |= in[i + 2];
        o[k++] = a[(v >> 18) & 63];
        o[k++] = a[(v >> 12) & 63];
        o[k++] = i + 1 < n ? a[(v >> 6) & 63] : '=';
        o[k++] = i + 2 < n ? a[v & 63] : '=';
    }
    o[k] = 0;
    return k;
}

/* FNV-1a, 32 bits. Never 0: 0 marks a free slot of the table of senders. */
static uint32_t fnv1a(const uint8_t *d, size_t n)
{
    uint32_t h = 2166136261UL;
    for (size_t i = 0; i < n; i++) { h ^= d[i]; h *= 16777619UL; }
    return h ? h : 1U;
}

/* Wi-Fi task, once per received frame. Never blocks, prints or allocates: it classifies the frame, builds its
 * record and key on the stack, then in ONE critical section counts it, asks the table of senders and, if the
 * frame gets a line, leaves the record in the ring. */
static void frames_rx(void *buf, wifi_promiscuous_pkt_type_t type)
{
    if (type != WIFI_PKT_MGMT && type != WIFI_PKT_DATA && type != WIFI_PKT_CTRL) return;
    const wifi_promiscuous_pkt_t *pk = buf;
    const uint8_t *p = pk->payload;
    unsigned sig = pk->rx_ctrl.sig_len;
    unsigned wlen = sig >= 4 ? sig - 4 : 0;         /* the frame without its FCS: never read past it */

    unsigned kind = 0;                              /* 1 beacon, 2 probe response, 3 probe request, 4 deauth, 5 disassoc, 6 data */
    uint8_t key[1 + 12 + 2 + 32];
    size_t klen = 0;
    bool always = false;                            /* every frame of this kind gets a line */
    fr_rec_t r;
    r.n = 0;
    if (type != WIFI_PKT_CTRL && wlen >= FR_FIXED) {
        uint8_t ftype = (p[0] >> 2) & 3, sub = p[0] >> 4;
        /* A management frame with Order set carries 4 bytes of HT Control after the header: the head starts after
         * them. One with Protected set has its body encrypted (802.11w): it gets no head, never bytes read as a
         * reason or a name. */
        unsigned off = (ftype == 0 && (p[1] & 0x80)) ? FR_FIXED + 4U : FR_FIXED;
        unsigned avail = wlen > off ? wlen - off : 0;
        if (ftype == 0 && (p[1] & 0x40)) avail = 0;
        const uint8_t *h = p + off;
        unsigned hl = 0;
        if (type == WIFI_PKT_MGMT && ftype == 0) {
            switch (sub) {
            case 8: case 5:                         /* beacon, probe response: 12 fixed bytes, then the name tag */
                kind = sub == 8 ? 1 : 2;
                if (avail >= 12) {
                    hl = 12;
                    if (avail >= 14 && h[12] == 0 && h[13] <= 32 && avail >= 14U + h[13]) hl = 14U + h[13];
                }
                key[klen++] = (uint8_t)kind;
                memcpy(key + klen, p + 10, 6); klen += 6;           /* address 2 */
                break;
            case 4:                                 /* probe request: the name tag, or no head */
                kind = 3;
                key[klen++] = 3;
                memcpy(key + klen, p + 10, 6); klen += 6;
                if (avail >= 2 && h[0] == 0 && h[1] <= 32 && avail >= 2U + h[1]) {
                    hl = 2U + h[1];
                    key[klen++] = 1;                /* a name tag, wildcard included */
                    memcpy(key + klen, h + 1, 1U + h[1]); klen += 1U + h[1];
                } else {
                    key[klen++] = 0;                /* none */
                }
                break;
            case 12: case 10:                       /* deauthentication, disassociation: the reason */
                kind = sub == 12 ? 4 : 5;
                always = true;
                if (avail >= 2) hl = 2;
                break;
            default: break;
            }
        } else if (type == WIFI_PKT_DATA && ftype == 2) {
            kind = 6;                               /* any subtype; its payload is never read */
            key[klen++] = 6;
            memcpy(key + klen, p + 4, 12); klen += 12;              /* addresses 1 and 2 */
        }
        if (kind) {
            r.b[0] = p[0];
            r.b[1] = p[1];
            r.b[2] = pk->rx_ctrl.channel;
            r.b[3] = (uint8_t)(int8_t)pk->rx_ctrl.rssi;
            r.b[4] = (uint8_t)(sig & 0xFF);
            r.b[5] = (uint8_t)(sig >> 8);
            memcpy(r.b + 6, p + 4, 18);                             /* addresses 1, 2 and 3 */
            if (hl) memcpy(r.b + FR_FIXED, h, hl);                  /* hl stays 0 for data */
            r.n = (uint8_t)(FR_FIXED + hl);
        }
    }
    uint32_t hash = (!always && r.n) ? fnv1a(key, klen) : 0;

    portENTER_CRITICAL(&s_fr_mux);
    if (!s_fr_open) { portEXIT_CRITICAL(&s_fr_mux); return; }
    s_fr.heard++;
    if (type == WIFI_PKT_MGMT) s_fr.mgmt++;
    else if (type == WIFI_PKT_DATA) s_fr.data++;
    else s_fr.ctrl++;
    switch (kind) {
    case 1: s_fr.beacon++; break;
    case 2: s_fr.probe_resp++; break;
    case 3: s_fr.probe_req++; break;
    case 4: s_fr.deauth++; break;
    case 5: s_fr.disassoc++; break;
    default: break;
    }
    if (r.n) {
        bool chosen = true;
        unsigned slot = 0;
        if (!always) {
            unsigned i = hash % FR_SEEN, probes = 0;
            while (probes < FR_SEEN && s_fr_seen[i] && s_fr_seen[i] != hash) { i = (i + 1) % FR_SEEN; probes++; }
            if (probes < FR_SEEN && s_fr_seen[i] == hash) chosen = false;   /* told already */
            else if (probes >= FR_SEEN || s_fr_seen_n >= FR_SEEN) { chosen = false; s_fr.dropped++; }   /* table full */
            slot = i;
        }
        if (chosen) {
            if (s_fr_cnt >= FR_RING) s_fr.dropped++;        /* the UART cannot keep up: not entered in the table, so a later frame may still tell it */
            else {
                if (!always) { s_fr_seen[slot] = hash; s_fr_seen_n++; }
                s_fr_ring[s_fr_head] = r;
                s_fr_head = (s_fr_head + 1) % FR_RING;
                s_fr_cnt++;
            }
        }
    }
    portEXIT_CRITICAL(&s_fr_mux);
}

/* Takes the oldest record out of the ring; false when it is empty. */
static bool fr_pop(fr_rec_t *r)
{
    bool got = false;
    portENTER_CRITICAL(&s_fr_mux);
    if (s_fr_cnt) {
        *r = s_fr_ring[s_fr_tail];
        s_fr_tail = (s_fr_tail + 1) % FR_RING;
        s_fr_cnt--;
        got = true;
    }
    portEXIT_CRITICAL(&s_fr_mux);
    return got;
}

/* "key=123": the value; -2 when `t` is another key's, -1 when it is this key's with no number of its own. */
static long kv_arg(const char *t, const char *key)
{
    size_t kl = strlen(key);
    if (strncmp(t, key, kl) || t[kl] != '=') return -2;
    t += kl + 1;
    if (*t < '0' || *t > '9') return -1;
    char *end;
    long v = strtol(t, &end, 10);
    return *end || v > 100000 ? -1 : v;
}

/* A channel `chan` tries: the table's 2.4 GHz range, or one of its listen_5g. */
static bool listen_channel(long ch)
{
    if (ch >= k_lim->channel_2g_first && ch <= k_lim->channel_2g_last) return true;
    for (size_t i = 0; i < FCK_C5_CHANNELS_5G_MAX && k_lim->listen_5g[i].number; i++) {
        if (k_lim->listen_5g[i].number == ch) return true;
    }
    return false;
}

static void fr_tell(const fr_rec_t *r, unsigned *sent)
{
    char b64[4 * ((FR_REC_MAX + 2) / 3) + 1];
    b64_encode(r->b, r->n, b64);
    out("#F %s", b64);
    (*sent)++;
}

static int cmd_frames(int argc, char **argv)
{
    long ch = -1, ms = 250, v;
    bool have_ch = false, bad = false;
    for (int i = 1; i < argc; i++) {
        if ((v = kv_arg(argv[i], "ch")) != -2) { if (v < 0) bad = true; else { ch = v; have_ch = true; } }
        else if ((v = kv_arg(argv[i], "ms")) != -2) { if (v < 0) bad = true; else ms = v; }
        else bad = true;
    }
    if (bad || !have_ch) { out("usage: frames ch=<n> [ms=250]"); return 2; }
    if (!listen_channel(ch)) {
        out("frames: ch must be 1-13 or one of the 5 GHz channels (36-64, 100-140, 144, 149-165)");
        return 2;
    }
    if (ms < CHAN_DWELL_MIN || ms > CHAN_DWELL_MAX) {
        out("frames: ms must be %d..%d", CHAN_DWELL_MIN, CHAN_DWELL_MAX);
        return 2;
    }
    if (s_wifi_err != ESP_OK) { out("wifi not available: %s", esp_err_to_name(s_wifi_err)); return 1; }

    esp_err_t e = esp_wifi_set_band_mode(WIFI_BAND_MODE_AUTO);
    if (e != ESP_OK) { out("frames: band mode auto refused: %s", esp_err_to_name(e)); return 1; }
    const wifi_promiscuous_filter_t f = { .filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT | WIFI_PROMIS_FILTER_MASK_DATA |
                                                         WIFI_PROMIS_FILTER_MASK_CTRL };
    const wifi_promiscuous_filter_t cf = { .filter_mask = WIFI_PROMIS_CTRL_FILTER_MASK_ALL };
    portENTER_CRITICAL(&s_fr_mux);
    s_fr_open = false;
    portEXIT_CRITICAL(&s_fr_mux);
    if ((e = esp_wifi_set_promiscuous_rx_cb(frames_rx)) != ESP_OK || (e = esp_wifi_set_promiscuous_filter(&f)) != ESP_OK ||
        (e = esp_wifi_set_promiscuous_ctrl_filter(&cf)) != ESP_OK || (e = esp_wifi_set_promiscuous(true)) != ESP_OK) {
        esp_wifi_set_promiscuous(false);
        s_fr_last_ch = 0;
        out("frames: promiscuous mode refused: %s", esp_err_to_name(e));
        return 1;
    }
    if ((e = esp_wifi_set_channel((uint8_t)ch, WIFI_SECOND_CHAN_NONE)) != ESP_OK) {
        esp_wifi_set_promiscuous(false);
        s_fr_last_ch = 0;
        out("frames: the driver refused channel %ld: %s", ch, esp_err_to_name(e));
        return 1;
    }
    if (s_fr_last_ch != (uint8_t)ch) wait_fed(CHAN_SETTLE_MS);
    s_fr_last_ch = (uint8_t)ch;

    portENTER_CRITICAL(&s_fr_mux);
    memset(&s_fr, 0, sizeof s_fr);
    memset(s_fr_seen, 0, sizeof s_fr_seen);
    s_fr_seen_n = s_fr_head = s_fr_tail = s_fr_cnt = 0;
    s_fr_open = true;
    portEXIT_CRITICAL(&s_fr_mux);
    int64_t t0 = esp_timer_get_time();
    int64_t t_end = t0 + (int64_t)ms * 1000;
    int64_t fed = t0;
    unsigned sent = 0;
    fr_rec_t r;
    while (esp_timer_get_time() < t_end) {
        if (fr_pop(&r)) fr_tell(&r, &sent);
        else vTaskDelay(1);                 /* at least one tick, whatever CONFIG_FREERTOS_HZ is */
        /* fed in slices, as every command that waits: this loop never blocks for one whole */
        if (esp_timer_get_time() - fed >= (int64_t)WDT_SLICE_MS * 1000) {
            wdt_feed();
            fed = esp_timer_get_time();
        }
    }
    portENTER_CRITICAL(&s_fr_mux);
    s_fr_open = false;
    fr_cnt_t c = s_fr;
    portEXIT_CRITICAL(&s_fr_mux);
    int64_t dms = (esp_timer_get_time() - t0 + 500) / 1000;
    /* back to a plain station, the way `scan` and `chan` expect it; then what is still queued */
    esp_wifi_set_promiscuous(false);
    while (fr_pop(&r)) fr_tell(&r, &sent);
    out("#FRAMES ch=%ld ms=%lld heard=%lu mgmt=%lu data=%lu ctrl=%lu beacon=%lu probe_req=%lu probe_resp=%lu deauth=%lu "
        "disassoc=%lu sent=%u dropped=%lu", ch, dms, (unsigned long)c.heard, (unsigned long)c.mgmt,
        (unsigned long)c.data, (unsigned long)c.ctrl, (unsigned long)c.beacon, (unsigned long)c.probe_req,
        (unsigned long)c.probe_resp, (unsigned long)c.deauth, (unsigned long)c.disassoc, sent, (unsigned long)c.dropped);
    return 0;
}

/* ----- BLE scan (NimBLE host, observer role only) ----- */

typedef struct {
    uint8_t addr[6];            /* as NimBLE keeps it: least significant byte first */
    uint8_t type;               /* BLE_ADDR_PUBLIC / _RANDOM / _PUBLIC_ID / _RANDOM_ID */
    int8_t rssi;                /* of the latest report that carried one */
    uint16_t adv;               /* reports received */
    uint8_t name_kind;          /* 0 none, 1 shortened (AD 0x08), 2 complete (AD 0x09) */
    uint8_t name_len;
    uint8_t name[BLE_NAME_MAX];
    uint8_t evt;                /* the advertising PDU of the latest report (BLE_HCI_ADV_RPT_EVTYPE_*) */
    uint8_t data_len;
    uint8_t data[BLE_DATA_MAX]; /* the AD of the latest report that carried any, as sent */
    uint32_t first_ms;          /* its first report and its latest, from the start of the scan */
    uint32_t last_ms;
} ble_dev_t;

static ble_dev_t s_ble[BLE_MAX];
static int s_ble_n;
static bool s_ble_full;
static volatile bool s_ble_open;
static volatile bool s_ble_ended;       /* the discovery ended by itself, which a scan with no duration never should */
static volatile int s_ble_ended_why;    /* the host's reason for it */
static volatile int64_t s_ble_t0;       /* the scan's start, by esp_timer: what first= and last= count from.
                                         * volatile, so it is written before s_ble_open says to read it */
static SemaphoreHandle_t s_ble_sync;
static int s_ble_state;         /* 0 not started, 1 host started (sync pending), 2 synced, -1 init failed */
static esp_err_t s_ble_err;

static void ble_on_sync(void) { xSemaphoreGive(s_ble_sync); }
static void ble_on_reset(int reason) { (void)reason; }

static void ble_host_task(void *arg)
{
    (void)arg;
    nimble_port_run();          /* returns only after nimble_port_stop(), which the firmware never calls */
    nimble_port_freertos_deinit();
}

/* Keeps the best name an advertisement carries: complete beats shortened, a newer one of the
 * same kind replaces the older. Passive scan: no scan responses, so often there is none. */
static void ble_name(const uint8_t *d, uint8_t len, ble_dev_t *dev)
{
    for (unsigned i = 0; i + 1 < len;) {
        uint8_t l = d[i];
        if (!l || i + 1 + l > len) break;
        uint8_t t = d[i + 1];
        uint8_t kind = t == 0x09 ? 2 : t == 0x08 ? 1 : 0;
        if (kind && kind >= dev->name_kind) {
            uint8_t nl = l - 1 < BLE_NAME_MAX ? l - 1 : BLE_NAME_MAX;
            memcpy(dev->name, d + i + 2, nl);
            dev->name_len = nl;
            dev->name_kind = kind;
        }
        i += 1 + l;
    }
}

/* NimBLE host task, once per advertising report. */
static void ble_seen(const struct ble_gap_disc_desc *d)
{
    ble_dev_t *dev = NULL;
    for (int i = 0; i < s_ble_n && !dev; i++)
        if (s_ble[i].type == d->addr.type && !memcmp(s_ble[i].addr, d->addr.val, 6)) dev = &s_ble[i];
    int64_t us = esp_timer_get_time() - s_ble_t0;
    uint32_t now = us > 0 ? (uint32_t)(us / 1000) : 0;
    if (!dev) {
        if (s_ble_n >= BLE_MAX) { s_ble_full = true; return; }
        dev = &s_ble[s_ble_n++];
        memset(dev, 0, sizeof *dev);
        dev->type = d->addr.type;
        memcpy(dev->addr, d->addr.val, 6);
        dev->rssi = RSSI_NONE;
        dev->first_ms = now;
    }
    dev->last_ms = now;
    dev->evt = d->event_type;
    if (dev->adv < UINT16_MAX) dev->adv++;
    if (d->rssi != 127) dev->rssi = d->rssi;     /* 127 = not available */
    if (d->data && d->length_data) {
        ble_name(d->data, d->length_data, dev);
        /* Whole or not at all: a legacy report is never longer, and a cut one would read as
         * another advertisement. */
        if (d->length_data <= BLE_DATA_MAX) {
            memcpy(dev->data, d->data, d->length_data);
            dev->data_len = d->length_data;
        }
    }
}

static int ble_gap_cb(struct ble_gap_event *ev, void *arg)
{
    (void)arg;
    if (ev->type == BLE_GAP_EVENT_DISC) { if (s_ble_open) ble_seen(&ev->disc); }
    else if (ev->type == BLE_GAP_EVENT_DISC_COMPLETE) {
        /* `ble scan` does not wait for this to finish: it only learns here that the scan was cut short */
        s_ble_ended_why = ev->disc_complete.reason;
        s_ble_ended = true;
    }
    return 0;
}

/* Brought up on the first `ble scan`, not at boot: if the controller does not fit next to
 * Wi-Fi, info / scan / gnss are untouched and only `ble` reports the error. */
static esp_err_t ble_up(void)
{
    if (s_ble_state == 2) return ESP_OK;
    if (s_ble_state < 0) return s_ble_err;
    if (s_ble_state == 0) {
        if (!s_ble_sync) s_ble_sync = xSemaphoreCreateBinary();
        if (!s_ble_sync) return ESP_ERR_NO_MEM;
        esp_err_t e = nimble_port_init();
        if (e != ESP_OK) { s_ble_state = -1; s_ble_err = e; return e; }
        ble_hs_cfg.sync_cb = ble_on_sync;
        ble_hs_cfg.reset_cb = ble_on_reset;
        nimble_port_freertos_init(ble_host_task);
        s_ble_state = 1;
    }
    if (!ble_hs_synced() && !take_fed(s_ble_sync, 3000) && !ble_hs_synced())
        return ESP_ERR_TIMEOUT;
    s_ble_state = 2;
    return ESP_OK;
}

static int cmd_ble(int argc, char **argv)
{
    long secs = BLE_SECS_DEF;
    if (argc < 2 || strcmp(argv[1], "scan") || argc > 3 || (argc == 3 && (secs = num_arg(argv[2], "secs")) < 0)) {
        out("usage: ble scan [secs=%d]", BLE_SECS_DEF);
        return 2;
    }
    if (secs < 1 || secs > BLE_SECS_MAX) { out("ble scan: secs must be 1..%d", BLE_SECS_MAX); return 2; }
    esp_err_t e = ble_up();
    if (e != ESP_OK) {
        out("ble not available: %s%s", esp_err_to_name(e), s_ble_state < 0 ? " (reset the C5 to retry)" : "");
        return 1;
    }
    uint8_t own;
    int rc = ble_hs_id_infer_auto(0, &own);
    if (rc) { out("ble: no usable own address (host rc=%d)", rc); return 1; }

    /* Passive (no scan requests), as the table says, and no duplicate filter: every report reaches
     * ble_seen(), so adv= counts them and rssi= is the latest. Interval/window 0 = host defaults. */
    struct ble_gap_disc_params p = { .passive = k_lim->ble_scan_passive ? 1 : 0, .filter_duplicates = 0 };
    /* The scan is started with no duration and ended here, by the clock of this loop: nothing depends on
     * the host reporting that a timed discovery completed. */
    if (ble_gap_disc_active()) ble_gap_disc_cancel();   /* one that an earlier `ble scan` could not end */
    s_ble_n = 0;
    s_ble_full = false;
    s_ble_ended = false;
    int64_t t0 = esp_timer_get_time();
    s_ble_t0 = t0;              /* before the gate opens: ble_seen() reads it on the host task */
    s_ble_open = true;
    rc = ble_gap_disc(own, BLE_HS_FOREVER, &p, ble_gap_cb, NULL);
    if (rc) { s_ble_open = false; out("ble: discovery refused (host rc=%d)", rc); return 1; }
    for (uint32_t left = (uint32_t)secs * 1000; left && !s_ble_ended;) {
        uint32_t slice = left < WDT_SLICE_MS ? left : WDT_SLICE_MS;
        wait_fed(slice);
        left -= slice;
    }
    s_ble_open = false;
    /* Cut short: it ended before its time, or it would not stop when told to. Either way what was heard
     * is printed, and the command fails. */
    bool ended = s_ble_ended;
    int stop = ended ? 0 : ble_gap_disc_cancel();
    vTaskDelay(pdMS_TO_TICKS(20));          /* let a report already in the host task finish */
    int64_t ms = (esp_timer_get_time() - t0) / 1000;

    /* strongest first */
    int n = s_ble_n;
    for (int i = 1; i < n; i++) {
        ble_dev_t d = s_ble[i];
        int j = i;
        for (; j > 0 && s_ble[j - 1].rssi < d.rssi; j--) s_ble[j] = s_ble[j - 1];
        s_ble[j] = d;
    }
    for (int i = 0; i < n; i++) {
        const ble_dev_t *d = &s_ble[i];
        char name[BLE_NAME_MAX * 4 + 1];
        escape_str(d->name, d->name_len, name, sizeof name);
        const uint8_t *a = d->addr;
        /* A full list of the longest lines is some 3 s of UART: fed on the way, as every wait is. */
        if ((i & 15) == 15) wdt_feed();
        /* " data=" and two digits a byte, or nothing: the key is left out when it sent none. */
        char data[sizeof " data=" + BLE_DATA_MAX * 2] = "";
        if (d->data_len) {
            int k = snprintf(data, sizeof data, " data=");
            for (unsigned b = 0; b < d->data_len; b++) k += snprintf(data + k, sizeof data - k, "%02x", d->data[b]);
        }
        out("#BLE addr=%02x:%02x:%02x:%02x:%02x:%02x type=%s rssi=%d name=\"%s\" adv=%u evt=%u first=%u last=%u%s",
            a[5], a[4], a[3], a[2], a[1], a[0], (d->type & 1) ? "rnd" : "pub", d->rssi, name, d->adv, d->evt,
            (unsigned)d->first_ms, (unsigned)d->last_ms, data);
    }
    if (s_ble_full) out("(more than %d advertisers: the rest are not listed)", BLE_MAX);
    if (ended) out("(ble: the scan ended before its %ld s, host reason=%d)", secs, s_ble_ended_why);
    else if (stop) out("(ble: the scan did not stop when told to, host rc=%d)", stop);
    out("#BLEDONE n=%d ms=%lld", n, ms);
    return ended || stop ? 1 : 0;
}

/* ----- GNSS relay + 1PPS ----- */

static void IRAM_ATTR pps_isr(void *arg)
{
    (void)arg;
    int64_t now = esp_timer_get_time();
    if (s_pps_us && now - s_pps_us < PPS_MIN_GAP_US) return;
    s_pps_us = now;
    s_pps_n++;
    BaseType_t woke = pdFALSE;
    vTaskNotifyGiveFromISR(s_pps_task, &woke);
    portYIELD_FROM_ISR(woke);
}

static void pps_task(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (s_gnss_on) out("#PPS n=%u t_ms=%lld", (unsigned)s_pps_n, s_pps_us / 1000);
    }
}

static void gnss_task(void *arg)
{
    (void)arg;
    char line[NMEA_MAX];
    size_t n = 0;
    uint8_t b[128];
    for (;;) {
        if (!s_gnss_on) { ulTaskNotifyTake(pdTRUE, portMAX_DELAY); n = 0; continue; }
        int got = uart_read_bytes(GNSS_UART, b, sizeof b, pdMS_TO_TICKS(50));
        for (int i = 0; i < got; i++) {
            uint8_t c = b[i];
            if (c == '\n' || c == '\r') {
                line[n] = 0;
                /* a sentence right after the GNSS reset can carry junk before its '$' */
                char *s = strchr(line, '$');
                if (n && s_gnss_on) out("#GNSSU %s", s ? s : line);
                n = 0;
                continue;
            }
            if (c < 0x20 || c > 0x7E) continue;
            if (n < sizeof line - 1) line[n++] = (char)c;
        }
    }
}

static esp_err_t gnss_uart_up(void)
{
    if (s_gnss_uart) return ESP_OK;
    uart_config_t c = { .baud_rate = BAUD, .data_bits = UART_DATA_8_BITS, .parity = UART_PARITY_DISABLE,
                        .stop_bits = UART_STOP_BITS_1, .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
                        .source_clk = UART_SCLK_DEFAULT };
    esp_err_t e = uart_driver_install(GNSS_UART, 2048, 0, 0, NULL, 0);
    if (e == ESP_OK) e = uart_param_config(GNSS_UART, &c);
    if (e == ESP_OK) e = uart_set_pin(GNSS_UART, GNSS_TX, GNSS_RX, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (e != ESP_OK) return e;

    /* Pull-down: while the P4 holds the GNSS in reset (its boot default) 1PPS floats. */
    gpio_config_t io = { .pin_bit_mask = 1ULL << PIN_PPS, .mode = GPIO_MODE_INPUT,
                         .pull_down_en = GPIO_PULLDOWN_ENABLE, .intr_type = GPIO_INTR_POSEDGE };
    if ((e = gpio_config(&io)) != ESP_OK) return e;
    e = gpio_install_isr_service(0);
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) return e;
    if ((e = gpio_isr_handler_add(PIN_PPS, pps_isr, NULL)) != ESP_OK) return e;
    gpio_intr_disable(PIN_PPS);
    s_gnss_uart = true;
    return ESP_OK;
}

static int cmd_gnss(int argc, char **argv)
{
    const char *a = argc >= 2 ? argv[1] : "";
    if (!strcmp(a, "on")) {
        /* Already relaying: nothing to do. Never flush while gnss_task is reading: it keeps
         * re-taking the RX mutex at a higher priority and uart_flush_input() would wait for
         * ever, freezing this command loop (seen after a long relay run). */
        if (s_gnss_on) return 0;
        esp_err_t e = gnss_uart_up();
        if (e != ESP_OK) { out("gnss uart/pps setup failed: %s", esp_err_to_name(e)); return 1; }
        vTaskDelay(pdMS_TO_TICKS(60));              /* let gnss_task park after a recent `off` */
        uart_flush_input(GNSS_UART);
        s_pps_n = 0;
        s_pps_us = 0;
        s_gnss_on = true;
        gpio_intr_enable(PIN_PPS);
        xTaskNotifyGive(s_gnss_task);
        return 0;
    }
    if (!strcmp(a, "off")) {
        s_gnss_on = false;
        if (s_gnss_uart) gpio_intr_disable(PIN_PPS);
        return 0;
    }
    out("usage: gnss on|off");
    return 2;
}

/**
 * gtx <body>    "$<body>*CS\r\n" to the GNSS RXD (UART1 TX, IO5), CS = XOR of the body bytes
 * as NMEA 0183 defines it. The ME31GR02 datasheet (section 6.4) prints its input commands
 * ($POLCFGRESET, $POLCFGMSG, ...) without a checksum; the firmware always adds one, the way
 * 05-GNSS_ME31GR02/p4_fck_c5_bridge sent $POLCFGRESET. The UART is brought up here when the
 * relay never was, so a sentence can go out with `gnss off` (its answer is then not shown).
 */
/** gtx loop: route UART1 RX to its own TX pin (IO5) and check that a test string comes back,
 *  i.e. the C5 really drives IO5 (the GNSS RXD line); then restore RX to IO4. */
static int gtx_loop(void)
{
    esp_err_t e = gnss_uart_up();
    if (e != ESP_OK) { out("gnss uart setup failed: %s", esp_err_to_name(e)); return 1; }
    bool was = s_gnss_on;
    s_gnss_on = false;                               /* keep the relay task off the RX FIFO */
    vTaskDelay(pdMS_TO_TICKS(60));
    uart_set_pin(GNSS_UART, GNSS_TX, GNSS_TX, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    uart_flush_input(GNSS_UART);
    static const char t[] = "LOOP-io5-fck";
    uart_write_bytes(GNSS_UART, t, sizeof t - 1);
    uart_wait_tx_done(GNSS_UART, pdMS_TO_TICKS(50));
    char r[32] = {0};
    int got = uart_read_bytes(GNSS_UART, r, sizeof t - 1, pdMS_TO_TICKS(100));
    uart_set_pin(GNSS_UART, GNSS_TX, GNSS_RX, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    uart_flush_input(GNSS_UART);
    s_gnss_on = was;
    if (was) xTaskNotifyGive(s_gnss_task);           /* gnss_task parked while we borrowed RX */
    bool ok = got == (int)sizeof t - 1 && !memcmp(r, t, sizeof t - 1);
    out("#GTXLOOP ok=%d got=%d", ok, got);
    return ok ? 0 : 1;
}

static int cmd_gtx(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "loop")) return gtx_loop();
    /* `gtx -n <body>` sends the sentence without "*CS", exactly as the ME31GR02 datasheet
     * prints its commands (section 6.4). */
    bool nocs = argc == 3 && !strcmp(argv[1], "-n");
    if (nocs) { argv++; argc--; }
    const char *body = argc == 2 ? argv[1] : "";
    if (*body == '$') body++;
    size_t n = strlen(body);
    /* '"' and '\' are refused too: #GTX quotes the sentence and no GNSS command uses them */
    if (argc != 2 || !n || n > GTX_BODY_MAX || strpbrk(body, "$*\"\\")) {
        out("usage: gtx <sentence body: no '$', no '*CS', no spaces, 1..%d chars>", GTX_BODY_MAX);
        return 2;
    }
    bool was_up = s_gnss_uart;
    esp_err_t e = gnss_uart_up();
    if (e != ESP_OK) { out("gnss uart/pps setup failed: %s", esp_err_to_name(e)); return 1; }
    if (!was_up) vTaskDelay(pdMS_TO_TICKS(5));      /* IO5 has just become an idle-high TX */

    uint8_t cs = 0;
    for (size_t i = 0; i < n; i++) cs ^= (uint8_t)body[i];
    char s[GTX_BODY_MAX + 8];
    int k = nocs ? snprintf(s, sizeof s, "$%s\r\n", body) : snprintf(s, sizeof s, "$%s*%02X\r\n", body, cs);
    if (uart_write_bytes(GNSS_UART, s, k) != k || uart_wait_tx_done(GNSS_UART, pdMS_TO_TICKS(100)) != ESP_OK) {
        out("gtx: the GNSS UART did not send the sentence");
        return 1;
    }
    out("#GTX sent=\"%.*s\"", k - 2, s);             /* without the CR LF */
    if (!s_gnss_on) out("(gtx: relay off, so no answer from the GNSS is shown; `gnss on` first to see one)");
    return 0;
}

/* ----- wake lines to the host (TCA6408A) ----- */

/* HOST_WP_C5: host TCA P1 -> C5 IO0 (input here). C5_WP_HOST: C5 IO23 -> host TCA P0 (output
 * here, also raises the TCA's ~INT to the P4's GPIO18 on every change). */
#define PIN_HOST_WP_C5 0
#define PIN_C5_WP_HOST 23

/**
 * wp            read IO0 and the level IO23 is driving
 * wp 0|1        drive IO23 to that level, then read
 * wp in         release IO23 (input, pull-down)
 */
static int cmd_wp(int argc, char **argv)
{
    static bool driven;
    gpio_set_direction(PIN_HOST_WP_C5, GPIO_MODE_INPUT);
    gpio_set_pull_mode(PIN_HOST_WP_C5, GPIO_FLOATING);
    if (argc >= 2) {
        if (!strcmp(argv[1], "in")) {
            gpio_set_direction(PIN_C5_WP_HOST, GPIO_MODE_INPUT);
            gpio_set_pull_mode(PIN_C5_WP_HOST, GPIO_PULLDOWN_ONLY);
            driven = false;
        } else if (!strcmp(argv[1], "0") || !strcmp(argv[1], "1")) {
            gpio_set_level(PIN_C5_WP_HOST, argv[1][0] == '1');
            gpio_set_direction(PIN_C5_WP_HOST, GPIO_MODE_INPUT_OUTPUT);
            driven = true;
        } else {
            out("usage: wp [0|1|in]");
            return 2;
        }
    }
    esp_rom_delay_us(50);
    out("#WP host_wp_c5=%d c5_wp_host=%d driven=%d", gpio_get_level(PIN_HOST_WP_C5),
        gpio_get_level(PIN_C5_WP_HOST), driven);
    return 0;
}

/* ----- command loop ----- */

static void run_line(char *line)
{
    char *argv[4];
    int argc = 0;
    char *save = NULL;
    for (char *t = strtok_r(line, " \t", &save); t && argc < 4; t = strtok_r(NULL, " \t", &save)) argv[argc++] = t;
    if (!argc) return;
    int rc;
    if (!strcmp(argv[0], "info")) rc = cmd_info();
    else if (!strcmp(argv[0], "health")) rc = cmd_health();
    else if (!strcmp(argv[0], "scan")) rc = cmd_scan(argc, argv);
    else if (!strcmp(argv[0], "gnss")) rc = cmd_gnss(argc, argv);
    else if (!strcmp(argv[0], "chan")) rc = cmd_chan(argc, argv);
    else if (!strcmp(argv[0], "frames")) rc = cmd_frames(argc, argv);
    else if (!strcmp(argv[0], "ble")) rc = cmd_ble(argc, argv);
    else if (!strcmp(argv[0], "wp")) rc = cmd_wp(argc, argv);
    else if (!strcmp(argv[0], "gtx")) rc = cmd_gtx(argc, argv);
    else {
        out("unknown command: %s (info | health | scan [2g|5g|all] | gnss on|off | chan [2g|5g|all] [dwell_ms] | "
            "frames ch=<n> [ms] | ble scan [secs] | wp [0|1|in] | gtx <nmea body>)",
            argv[0]);
        rc = 2;
    }
    out("#DONE %s rc=%d", argv[0], rc);
}

/* A line that outgrew the buffer. Cut and run, it would be another command than the one sent, so it is
 * refused whole, the way an unknown command is: a word for people, and rc=2 under the line's first word, so
 * that whoever sent it is not left waiting. A first word too long for the SDK's link to match goes as "?". */
static void refuse_long(char *kept)
{
    char *save = NULL;
    const char *verb = strtok_r(kept, " \t", &save);
    if (!verb || strlen(verb) > VERB_MAX) verb = "?";
    out("line too long: more than %d characters, not run", CMD_MAX - 1);
    out("#DONE %s rc=2", verb);
}

void app_main(void)
{
    uart_config_t c = { .baud_rate = BAUD, .data_bits = UART_DATA_8_BITS, .parity = UART_PARITY_DISABLE,
                        .stop_bits = UART_STOP_BITS_1, .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
                        .source_clk = UART_SCLK_DEFAULT };
    uart_driver_install(HOST_UART, 1024, 2048, 0, NULL, 0);
    uart_param_config(HOST_UART, &c);
    uart_set_pin(HOST_UART, HOST_TX, HOST_RX, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    /* ESP_LOGE and panics go through the driver too, so they cannot split one of our lines */
    uart_vfs_dev_use_driver(HOST_UART);

    s_wifi_err = wifi_up();
    xTaskCreate(gnss_task, "gnss", 4096, NULL, 5, &s_gnss_task);
    xTaskCreate(pps_task, "pps", 3072, NULL, 6, &s_pps_task);

    /* From here this task is the command loop, and the task watchdog watches it: it is fed at every turn of
     * the loop and inside every command that waits. The GNSS relay and the 1PPS counter are tasks of their
     * own and block for as long as they like. */
    esp_err_t wdt_err = esp_task_wdt_add(NULL);
    s_wdt = wdt_err == ESP_OK;

    /* the ROM banner may end mid-line: start ours on a fresh one */
    uart_write_bytes(HOST_UART, "\r\n", 2);
    cmd_info();
    if (s_wifi_err != ESP_OK) out("wifi init failed: %s (scan will refuse)", esp_err_to_name(s_wifi_err));
    else if (strcmp(s_lim, "ok")) out("lim=%s: the driver did not take that limit (scan will refuse)", s_lim);
    if (!s_wdt) out("watchdog not armed: %s (a command loop that hangs will stay hung)", esp_err_to_name(wdt_err));

    char line[CMD_MAX];
    size_t n = 0;
    bool over = false;          /* the line outgrew the buffer: nothing more of it is kept */
    uint8_t b[64];
    for (;;) {
        /* uart_read_bytes() gives what it has once no byte has come for its timeout, and nothing after one
         * more: with half of LINE_IDLE_MS as the timeout, an empty read is LINE_IDLE_MS of silence. Bytes that
         * trickle in keep one call waiting, a timeout for each of the sizeof b at the very most, which is
         * still well inside the watchdog's time. */
        int got = uart_read_bytes(HOST_UART, b, sizeof b, pdMS_TO_TICKS(LINE_IDLE_MS / 2));
        wdt_feed();
        if (got <= 0) {
            /* Nothing for LINE_IDLE_MS. Whatever of a line was waiting for its end is dropped: the sender
             * gave up on it (the P4 reset, or stopped half way), and its next command must not be glued
             * to it. */
            n = 0;
            over = false;
            continue;
        }
        for (int i = 0; i < got; i++) {
            uint8_t ch = b[i];
            if (ch == '\n' || ch == '\r') {
                line[n] = 0;
                if (over) refuse_long(line);
                else if (n) run_line(line);
                n = 0;
                over = false;
                continue;
            }
            if (ch < 0x20 || ch > 0x7E) continue;   /* noise while the P4 resets or reflashes */
            if (n < sizeof line - 1) line[n++] = (char)ch;
            else over = true;
        }
    }
}
