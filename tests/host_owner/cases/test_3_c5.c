/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_3_c5.c
 * @brief The C5 through the facade: what a chip runs (fck_core_firmware()), which lines
 *        reach it (fck_core_command()'s gate) and the typed calls fck_core_wifi_scan(),
 *        fck_core_wifi_channels(), fck_core_wifi_frames() and fck_core_ble_scan().
 *
 * The C5's firmware is scripted: a command is answered with the lines a case gives.
 * Every case looks at what was SENT to the chip as well as at what came back:
 * the radio rule is that a line the gate refuses never reaches the C5.
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "unity.h"

#include "fck.h"
#include "fck_core.h"
#include "fck_aos.h"

#include "fake_world.h"

static char s_text[24576];

/* Appends to s_text, which a case starts with text_begin(). */
static void text_begin(void)
{
    s_text[0] = '\0';
}

static void text_add(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static void text_add(const char *fmt, ...)
{
    size_t const n = strlen(s_text);
    va_list      ap;
    va_start(ap, fmt);
    (void)vsnprintf(s_text + n, sizeof(s_text) - n, fmt, ap);
    va_end(ap);
}

static void script_text(const char *prefix)
{
    sim_script(SIM_C5, prefix, s_text, 5U);
}

/* Nothing but `info` may have been sent to a chip that was refused. */
static size_t sent_other_than_info(int chip)
{
    size_t n = 0U;
    for (size_t i = 0U; strcmp(sim_sent_line(chip, i), "") != 0; i++) {
        n += (strncmp(sim_sent_line(chip, i), "info", 4U) != 0) ? 1U : 0U;
    }
    return n;
}

/* ---- What a chip runs ------------------------------------------------------------------ */

TEST_CASE("a chip's banner, in an app's terms: the 2.0 firmwares", "[c5][firmware]")
{
    sim_begin_fck(NULL, NULL);
    fck_firmware_t a;
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_firmware(FCK_C5, &a));
    TEST_ASSERT_TRUE(a.known && a.official && a.compatible);
    TEST_ASSERT_EQUAL_STRING("c5_fck_agent", a.fw);
    TEST_ASSERT_EQUAL_UINT8(2, a.ver_major);
    TEST_ASSERT_EQUAL_UINT8(0, a.ver_minor);
    TEST_ASSERT_EQUAL_UINT8(6, a.id_len);
    const uint8_t mac[6] = { 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0x01 };
    TEST_ASSERT_EQUAL_HEX8_ARRAY(mac, a.id, 6);

    TEST_ASSERT_EQUAL(KODE_OK, fck_core_firmware(FCK_NRF, &a));
    TEST_ASSERT_TRUE(a.known && a.official && a.compatible);
    TEST_ASSERT_EQUAL_STRING("nrf_fck_agent", a.fw);
    TEST_ASSERT_EQUAL_UINT8(2, a.ver_major);
    TEST_ASSERT_EQUAL_UINT8(8, a.id_len);
    const uint8_t devid[8] = { 0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF };
    TEST_ASSERT_EQUAL_HEX8_ARRAY(devid, a.id, 8);
}

TEST_CASE("older firmwares and other firmware are told apart, and no limits are claimed for them", "[c5][firmware]")
{
    sim_begin_fck(SIM_C5_BANNER_15, SIM_NRF_BANNER_10);
    fck_firmware_t a;
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_firmware(FCK_C5, &a));
    TEST_ASSERT_TRUE(a.known && a.official);
    TEST_ASSERT_TRUE_MESSAGE(a.compatible, "the first modules' firmware is compatible by its name");
    TEST_ASSERT_EQUAL_UINT8(1, a.ver_major);
    TEST_ASSERT_EQUAL_UINT8(5, a.ver_minor);
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_firmware(FCK_NRF, &a));
    TEST_ASSERT_TRUE(a.known && a.official && a.compatible);
    TEST_ASSERT_EQUAL_UINT8(1, a.ver_major);

    sim_begin_fck(SIM_C5_BANNER_OWN, NULL);
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_firmware(FCK_C5, &a));
    TEST_ASSERT_TRUE(a.known);
    TEST_ASSERT_FALSE_MESSAGE(a.official, "another firmware is not the official firmware");
    TEST_ASSERT_FALSE_MESSAGE(a.compatible, "and with no caps= it does not speak the SDK's words");
    TEST_ASSERT_EQUAL_STRING("my_firmware", a.fw);
    TEST_ASSERT_EQUAL_UINT8(0, a.ver_major);
    TEST_ASSERT_EQUAL_UINT8(3, a.ver_minor);
    TEST_ASSERT_EQUAL_UINT8(0, a.id_len);
}

TEST_CASE("a 2.0 banner that does not say reg=EU lim=ok keeps no limits either", "[c5][firmware]")
{
    fck_firmware_t a;
    sim_begin_fck("#C5INFO fw=c5_fck_agent ver=2.0 chip=esp32c5 rev=1.2 mac=aa:bb:cc:dd:ee:03 reg=US lim=ok",
                  NULL);
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_firmware(FCK_C5, &a));
    TEST_ASSERT_TRUE(a.official && a.compatible);
    fck_wifi_network_t ap[2];
    size_t             n = 77;
    TEST_ASSERT_EQUAL_MESSAGE(KODE_DENIED, fck_core_wifi_scan(FCK_BAND_ALL, ap, 2U, &n), "reg=US");
    sim_begin_fck("#C5INFO fw=c5_fck_agent ver=2.0 chip=esp32c5 rev=1.2 mac=aa:bb:cc:dd:ee:03 reg=EU lim=off",
                  NULL);
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_firmware(FCK_C5, &a));
    TEST_ASSERT_TRUE(a.official && a.compatible);
    TEST_ASSERT_EQUAL_MESSAGE(KODE_DENIED, fck_core_wifi_scan(FCK_BAND_ALL, ap, 2U, &n), "lim=off");
    TEST_ASSERT_EQUAL_UINT(0, sent_other_than_info(SIM_C5));
}

TEST_CASE("fck_firmware: a bad argument, and no module", "[c5][firmware]")
{
    sim_begin_fck(NULL, NULL);
    fck_firmware_t a;
    memset(&a, 0x5A, sizeof(a));
    fck_firmware_t const before = a;
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_firmware(FCK_C5, NULL));
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_firmware((fck_chip_t)2, &a));
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_firmware((fck_chip_t)-1, &a));
    TEST_ASSERT_EQUAL_MEMORY(&before, &a, sizeof(a));

    sim_begin_empty();
    uint32_t const t0 = sim_now_ms();
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_firmware(FCK_C5, &a));
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_firmware(FCK_NRF, &a));
    TEST_ASSERT_TRUE_MESSAGE(sim_now_ms() - t0 < 500U, "no module, no waiting");
    TEST_ASSERT_EQUAL_MEMORY(&before, &a, sizeof(a));
}

TEST_CASE("a chip held in reset is not waited for, and says what it runs once it runs", "[c5][firmware]")
{
    sim_begin_fck(NULL, NULL);
    fck_firmware_t a;
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_power(FCK_C5, false));
    uint32_t const t0 = sim_now_ms();
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_firmware(FCK_C5, &a));
    TEST_ASSERT_TRUE_MESSAGE(sim_now_ms() - t0 < 500U, "a chip in reset is not asked");
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_power(FCK_C5, true));
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_firmware(FCK_C5, &a));
    TEST_ASSERT_TRUE(a.known && a.official);
}

TEST_CASE("a chip that printed no banner is asked with `info`, once", "[c5][firmware]")
{
    sim_begin_fck(NULL, NULL);
    sim_firmware_quiet(SIM_C5, SIM_C5_BANNER_20);
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_reset(FCK_C5));
    sim_sleep(200U);        /* it runs by now, and said nothing */
    sim_sent_clear(SIM_C5);
    fck_firmware_t a;
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_firmware(FCK_C5, &a));
    TEST_ASSERT_TRUE(a.known && a.official);
    TEST_ASSERT_EQUAL_UINT(1, sim_sent(SIM_C5, "info"));
    /* Said now: not asked again. */
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_firmware(FCK_C5, &a));
    TEST_ASSERT_EQUAL_UINT(1, sim_sent(SIM_C5, "info"));
}

TEST_CASE("a chip that never says what it runs: 7.5 s, then TIMEOUT, and nothing is sent to it", "[c5][firmware][slow]")
{
    sim_begin_fck(NULL, NULL);
    sim_firmware(SIM_C5, NULL);
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_reset(FCK_C5));
    sim_sent_clear(SIM_C5);
    /* fck.h: out is filled on KODE_OK only. Started as "known", so that a
     * refusal that wrote into it would show. */
    fck_firmware_t a = { .known = true };
    uint32_t const t0 = sim_now_ms();
    TEST_ASSERT_EQUAL(KODE_TIMEOUT, fck_core_firmware(FCK_C5, &a));
    uint32_t const took = sim_now_ms() - t0;
    TEST_ASSERT_TRUE_MESSAGE(took >= 7000U && took < 9000U, "the wait is 7.5 s");
    TEST_ASSERT_TRUE_MESSAGE(a.known, "a refusal leaves out alone");

    /* The facade's commands judge a chip by what it said: nothing, nothing sent. */
    int32_t rc = 99;
    TEST_ASSERT_EQUAL(KODE_TIMEOUT, fck_core_command(FCK_C5, "scan 2g", "scan", NULL, NULL, 0U, &rc));
    TEST_ASSERT_EQUAL_UINT(0, sent_other_than_info(SIM_C5));
}

/* ---- The gate ---------------------------------------------------------------------------- */

typedef struct {
    unsigned n;
    char     tag[24];
    char     body[80];
} seen_t;

static void on_seen(void *ctx, const fck_line_t *l)
{
    seen_t *const s = ctx;
    if (s->n++ == 0U) {
        (void)snprintf(s->tag, sizeof(s->tag), "%s", l->tag);
        (void)snprintf(s->body, sizeof(s->body), "%s", l->body);
    }
}

TEST_CASE("fck_command: `scan` reaches a 2.0 firmware that keeps the radio limits, and only as the gate builds it", "[c5][gate]")
{
    sim_begin_fck(NULL, NULL);
    sim_script(SIM_C5, "scan",
               "#AP ssid=\"x\" bssid=aa:bb:cc:dd:ee:11 ch=6 band=2g rssi=-50 auth=open\r\n"
               "#SCANDONE n=1 ms=10\r\n#DONE scan rc=0\r\n", 5U);
    seen_t  seen = { 0 };
    int32_t rc   = -9;
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_command(FCK_C5, "scan 2g", NULL, on_seen, &seen, 0U, &rc));
    TEST_ASSERT_EQUAL_INT32(0, rc);
    TEST_ASSERT_EQUAL_UINT(2, seen.n);       /* the #AP and the #SCANDONE */
    TEST_ASSERT_EQUAL_STRING("AP", seen.tag);
    TEST_ASSERT_EQUAL_UINT(1, sim_sent(SIM_C5, "scan"));
    TEST_ASSERT_EQUAL_STRING("scan 2g", sim_sent_line(SIM_C5, 0));
    /* The verb, if given, must be the one the gate works out. */
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_command(FCK_C5, "scan 2g", "chan", NULL, NULL, 0U, &rc));
    TEST_ASSERT_EQUAL_UINT(1, sim_sent(SIM_C5, "scan"));
}

TEST_CASE("fck_command: `scan` never reaches a firmware that does not keep the limits", "[c5][gate]")
{
    static const char *const banners[] = {
        SIM_C5_BANNER_15,
        SIM_C5_BANNER_10,
        "#C5INFO fw=c5_fck_agent ver=2.0 chip=esp32c5 rev=1.2 mac=aa:bb:cc:dd:ee:03 reg=US lim=ok",
        "#C5INFO fw=c5_fck_agent ver=2.0 chip=esp32c5 rev=1.2 mac=aa:bb:cc:dd:ee:03 reg=EU lim=off",
        "#C5INFO fw=c5_fck_agent ver=2.0 chip=esp32c5 rev=1.2 mac=aa:bb:cc:dd:ee:03",
    };
    for (size_t i = 0U; i < sizeof(banners) / sizeof(banners[0]); i++) {
        sim_begin_fck(banners[i], NULL);
        sim_script(SIM_C5, "scan", "#SCANDONE n=0 ms=1\r\n#DONE scan rc=0\r\n", 5U);
        int32_t rc = -9;
        TEST_ASSERT_EQUAL_MESSAGE(KODE_DENIED,
                                  fck_core_command(FCK_C5, "scan", NULL, NULL, NULL, 0U, &rc), banners[i]);
        TEST_ASSERT_EQUAL_MESSAGE(KODE_DENIED,
                                  fck_core_command(FCK_C5, "scan 5g", "scan", NULL, NULL, 0U, &rc), banners[i]);
        TEST_ASSERT_EQUAL_UINT_MESSAGE(0, sent_other_than_info(SIM_C5), banners[i]);
    }
}

TEST_CASE("fck_command: only the official firmware's words reach it", "[c5][gate]")
{
    sim_begin_fck(NULL, NULL);
    int32_t rc = 0;
    static const char *const refused[] = {
        "ble raw 00112233", "ble", "sniff", "pcap", "deauth aa:bb:cc:dd:ee:ff", "probe x",
        "gtx POLCFGPRT,0,115200", "gtx -n $POLCFGPRT,0,9600", "reboot", "", "scan 2g\nscan 5g",
    };
    for (size_t i = 0U; i < sizeof(refused) / sizeof(refused[0]); i++) {
        TEST_ASSERT_EQUAL_MESSAGE(KODE_DENIED,
                                  fck_core_command(FCK_C5, refused[i], NULL, NULL, NULL, 200U, &rc),
                                  refused[i]);
    }
    TEST_ASSERT_EQUAL_UINT(0, sent_other_than_info(SIM_C5));

    /* The ones that are the firmware's go out, as written. */
    sim_script(SIM_C5, "gtx", "#DONE gtx rc=0\r\n", 5U);
    sim_script(SIM_C5, "wp", "#DONE wp rc=0\r\n", 5U);
    sim_script(SIM_C5, "ble scan", "#BLEDONE n=0 ms=1\r\n#DONE ble rc=0\r\n", 5U);
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_command(FCK_C5, "gtx POLCFGMSG,0,2,1", NULL, NULL, NULL, 0U, &rc));
    TEST_ASSERT_EQUAL_INT32(0, rc);
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_command(FCK_C5, "wp", NULL, NULL, NULL, 0U, &rc));
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_command(FCK_C5, "ble scan 1", NULL, NULL, NULL, 0U, &rc));
    TEST_ASSERT_EQUAL_UINT(1, sim_sent(SIM_C5, "gtx POLCFGMSG,0,2,1"));
    TEST_ASSERT_EQUAL_UINT(1, sim_sent(SIM_C5, "ble scan 1"));
}

TEST_CASE("fck_command: another firmware gets the line as written, and must name its verb", "[c5][gate]")
{
    sim_begin_fck(SIM_C5_BANNER_OWN, NULL);
    sim_script(SIM_C5, "hello", "hi there\r\n#DONE hello rc=0\r\n", 5U);
    sim_script(SIM_C5, "scan", "#DONE scan rc=0\r\n", 5U);
    seen_t  seen = { 0 };
    int32_t rc   = -9;
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_command(FCK_C5, "hello world", NULL, on_seen, &seen, 500U, &rc));
    TEST_ASSERT_EQUAL_UINT(0, sent_other_than_info(SIM_C5));
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_command(FCK_C5, "hello world", "hello", on_seen, &seen, 500U, &rc));
    TEST_ASSERT_EQUAL_INT32(0, rc);
    TEST_ASSERT_EQUAL_STRING("hello world", sim_sent_line(SIM_C5, 0));
    /* Not the official firmware: `scan` is its own firmware's word, whatever it does. */
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_command(FCK_C5, "scan", "scan", NULL, NULL, 500U, &rc));
    TEST_ASSERT_EQUAL_UINT(1, sim_sent(SIM_C5, "scan"));
}

TEST_CASE("fck_command: no answer in time is TIMEOUT; a chip that restarts in the middle is IO_ERROR", "[c5][gate]")
{
    sim_begin_fck(NULL, NULL);
    int32_t rc = 99;
    sim_script(SIM_C5, "chan", "", 5U);           /* the firmware says nothing */
    uint32_t const t0 = sim_now_ms();
    TEST_ASSERT_EQUAL(KODE_TIMEOUT, fck_core_command(FCK_C5, "chan 2g 20", "chan", NULL, NULL, 300U, &rc));
    uint32_t const took = sim_now_ms() - t0;
    TEST_ASSERT_TRUE_MESSAGE(took >= 280U && took < 1500U, "the time the caller gave it");
    TEST_ASSERT_EQUAL_INT32(-1, rc);
    /* The firmware that never closed that command is left for the next test to
     * find a fresh module. */
    sim_unplug();
    TEST_ASSERT_TRUE(sim_wait_absent(5000U));

    sim_begin_fck(NULL, NULL);
    sim_script(SIM_C5, "chan", SIM_C5_BANNER_20 "\r\n#DONE chan rc=0\r\n", 5U);
    TEST_ASSERT_EQUAL(KODE_IO_ERROR, fck_core_command(FCK_C5, "chan 2g 20", "chan", NULL, NULL, 0U, &rc));
    TEST_ASSERT_TRUE(sim_log_has("restarted in the middle of `chan`"));
}

TEST_CASE("fck_command: the firmware's own refusal is rc 2, and the call still returns OK", "[c5][gate]")
{
    sim_begin_fck(NULL, NULL);
    int32_t rc = 99;
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_command(FCK_C5, "wp", NULL, NULL, NULL, 0U, &rc));
    TEST_ASSERT_EQUAL_INT32_MESSAGE(2, rc, "the model's firmware does not know `wp` here");
}

/* ---- Wi-Fi scan ----------------------------------------------------------------------------- */

static void ap_line(const char *ssid, unsigned last, unsigned ch, const char *band, int rssi,
                    const char *auth)
{
    text_add("#AP ssid=\"%s\" bssid=aa:bb:cc:dd:ee:%02x ch=%u band=%s rssi=%d auth=%s\r\n", ssid, last,
             ch, band, rssi, auth);
}

static kode_status_t quiet_job(void *arg, const fck_aos_job_context_t *job)
{
    (void)arg;
    (void)job;
    sim_sleep(50U);
    return KODE_OK;
}

TEST_CASE("a line judged before a job without the chips is still sent; after a write it is not", "[c5][gate]")
{
    sim_begin_fck(NULL, NULL);
    sim_script(SIM_C5, "chan", "#CHANDONE n=0 ms=10\r\n#DONE chan rc=0\r\n", 5U);
    fck_aos_firmware_t a;
    TEST_ASSERT_EQUAL(KODE_OK, fck_aos_firmware(FCK_AOS_C5, 1000U, &a));

    int32_t rc = -1;
    TEST_ASSERT_EQUAL(KODE_OK, fck_aos_run_job_without_chips(quiet_job, NULL, 1000U));
    TEST_ASSERT_EQUAL(KODE_OK, fck_aos_command_if_same_firmware(FCK_AOS_C5, a.seq, "chan 2g 100", "chan",
                                                             NULL, NULL, 2000U, &rc));
    TEST_ASSERT_EQUAL_INT32(0, rc);
    TEST_ASSERT_EQUAL_UINT(1, sim_sent(SIM_C5, "chan"));

    /* A write may have put another image there: judged again, or refused. */
    TEST_ASSERT_EQUAL(KODE_OK, fck_aos_run_job(quiet_job, NULL, 1000U));
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_aos_command_if_same_firmware(FCK_AOS_C5, a.seq, "chan 2g 100",
                                                                 "chan", NULL, NULL, 2000U, &rc));
    TEST_ASSERT_EQUAL_UINT(1, sim_sent(SIM_C5, "chan"));
}

TEST_CASE("fck_wifi_scan: strongest first, equal strengths in the firmware's order", "[c5][wifi]")
{
    sim_begin_fck(NULL, NULL);
    text_begin();
    ap_line("alpha", 0x11, 1, "2g", -70, "wpa2_psk");
    ap_line("bravo", 0x12, 6, "2g", -50, "open");
    ap_line("charlie", 0x13, 11, "2g", -70, "wpa3_psk");
    ap_line("", 0x14, 100, "5g", -60, "wpa2_psk");
    ap_line("delta", 0x15, 36, "5g", -70, "wpa_wpa2_psk");
    text_add("#SCANDONE n=5 ms=4000\r\n#DONE scan rc=0\r\n");
    script_text("scan");

    fck_wifi_network_t ap[8];
    size_t        n = 99;
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_wifi_scan(FCK_BAND_ALL, ap, 8U, &n));
    TEST_ASSERT_EQUAL_UINT(5, n);
    TEST_ASSERT_EQUAL_STRING("bravo", ap[0].ssid);
    TEST_ASSERT_EQUAL_STRING("", ap[1].ssid);
    TEST_ASSERT_EQUAL_STRING("alpha", ap[2].ssid);
    TEST_ASSERT_EQUAL_STRING("charlie", ap[3].ssid);
    TEST_ASSERT_EQUAL_STRING("delta", ap[4].ssid);
    TEST_ASSERT_EQUAL_UINT8(5, ap[0].ssid_len);
    TEST_ASSERT_EQUAL_UINT8(0, ap[1].ssid_len);
    TEST_ASSERT_EQUAL_UINT8(6, ap[0].channel);
    TEST_ASSERT_EQUAL(FCK_BAND_2G, ap[0].band);
    TEST_ASSERT_EQUAL(FCK_BAND_5G, ap[1].band);
    TEST_ASSERT_EQUAL_UINT8(100, ap[1].channel);
    TEST_ASSERT_EQUAL_INT8(-50, ap[0].rssi);
    TEST_ASSERT_EQUAL(FCK_WIFI_SECURITY_OPEN, ap[0].auth);
    TEST_ASSERT_EQUAL(FCK_WIFI_SECURITY_WPA3_PSK, ap[3].auth);
    TEST_ASSERT_EQUAL(FCK_WIFI_SECURITY_WPA_WPA2_PSK, ap[4].auth);
    const uint8_t b0[6] = { 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0x12 };
    TEST_ASSERT_EQUAL_HEX8_ARRAY(b0, ap[0].bssid, 6);
    TEST_ASSERT_EQUAL_STRING("scan all", sim_sent_line(SIM_C5, 0));
}

TEST_CASE("fck_wifi_scan: a smaller array holds the strongest, and the band goes to the firmware", "[c5][wifi]")
{
    sim_begin_fck(NULL, NULL);
    text_begin();
    ap_line("alpha", 0x11, 1, "2g", -70, "wpa2_psk");
    ap_line("bravo", 0x12, 6, "2g", -50, "open");
    ap_line("charlie", 0x13, 11, "2g", -70, "wpa3_psk");
    ap_line("", 0x14, 13, "2g", -60, "wpa2_psk");
    text_add("#SCANDONE n=4 ms=4000\r\n#DONE scan rc=0\r\n");
    script_text("scan");

    fck_wifi_network_t ap[2];
    size_t        n = 99;
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_wifi_scan(FCK_BAND_2G, ap, 2U, &n));
    TEST_ASSERT_EQUAL_UINT(2, n);
    TEST_ASSERT_EQUAL_STRING("bravo", ap[0].ssid);
    TEST_ASSERT_EQUAL_STRING("", ap[1].ssid);
    TEST_ASSERT_EQUAL_STRING("scan 2g", sim_sent_line(SIM_C5, 0));
    /* `n` may be left out. */
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_wifi_scan(FCK_BAND_5G, ap, 2U, NULL));
    TEST_ASSERT_EQUAL_STRING("scan 5g", sim_sent_line(SIM_C5, 1));
}

TEST_CASE("fck_wifi_scan: no more than the firmware's 64 are kept, the weakest going", "[c5][wifi]")
{
    sim_begin_fck(NULL, NULL);
    text_begin();
    for (unsigned i = 0U; i < 70U; i++) {
        char name[16];
        (void)snprintf(name, sizeof(name), "n%u", i);
        ap_line(name, i, 1U + (i % 13U), "2g", -99 + (int)i, "open");    /* -99 ... -30, weakest first */
    }
    text_add("#SCANDONE n=70 ms=4000\r\n#DONE scan rc=0\r\n");
    script_text("scan");

    static fck_wifi_network_t ap[100];
    size_t               n = 0;
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_wifi_scan(FCK_BAND_ALL, ap, 100U, &n));
    TEST_ASSERT_EQUAL_UINT(FCK_WIFI_NETWORK_MAX, n);
    TEST_ASSERT_EQUAL_STRING("n69", ap[0].ssid);
    TEST_ASSERT_EQUAL_STRING("n6", ap[63].ssid);
    for (size_t i = 1U; i < n; i++) {
        TEST_ASSERT_TRUE(ap[i - 1U].rssi >= ap[i].rssi);
    }
}

TEST_CASE("fck_wifi_scan: nothing in range is OK with none", "[c5][wifi]")
{
    sim_begin_fck(NULL, NULL);
    sim_script(SIM_C5, "scan", "#SCANDONE n=0 ms=3000\r\n#DONE scan rc=0\r\n", 5U);
    fck_wifi_network_t ap[4];
    size_t        n = 99;
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_wifi_scan(FCK_BAND_ALL, ap, 4U, &n));
    TEST_ASSERT_EQUAL_UINT(0, n);
}

TEST_CASE("fck_wifi_scan: it transmits, so a firmware without the radio limits is never asked", "[c5][wifi]")
{
    static const char *const banners[] = {
        SIM_C5_BANNER_15,
        "#C5INFO fw=c5_fck_agent ver=2.0 chip=esp32c5 rev=1.2 mac=aa:bb:cc:dd:ee:03 reg=US lim=ok",
        SIM_C5_BANNER_OWN,
    };
    for (size_t i = 0U; i < sizeof(banners) / sizeof(banners[0]); i++) {
        sim_begin_fck(banners[i], NULL);
        text_begin();
        ap_line("alpha", 0x11, 1, "2g", -70, "open");
        text_add("#SCANDONE n=1 ms=1\r\n#DONE scan rc=0\r\n");
        script_text("scan");
        fck_wifi_network_t ap[2];
        size_t        n = 77;
        memset(ap, 0x5A, sizeof(ap));
        TEST_ASSERT_EQUAL_MESSAGE(KODE_DENIED, fck_core_wifi_scan(FCK_BAND_ALL, ap, 2U, &n), banners[i]);
        TEST_ASSERT_EQUAL_UINT_MESSAGE(77, n, "untouched");
        TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x5A, ((uint8_t *)ap)[0], "untouched");
        TEST_ASSERT_EQUAL_UINT_MESSAGE(0, sent_other_than_info(SIM_C5), banners[i]);
    }
}

TEST_CASE("fck_wifi_scan: a bad argument sends nothing", "[c5][wifi]")
{
    sim_begin_fck(NULL, NULL);
    fck_wifi_network_t ap[2];
    size_t        n = 0;
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_wifi_scan(FCK_BAND_ALL, NULL, 2U, &n));
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_wifi_scan(FCK_BAND_ALL, ap, 0U, &n));
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_wifi_scan((fck_band_t)3, ap, 2U, &n));
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_wifi_scan((fck_band_t)-1, ap, 2U, &n));
    TEST_ASSERT_EQUAL_UINT(0, sent_other_than_info(SIM_C5));
}

TEST_CASE("fck_wifi_scan: the firmware's rc is the call's status, and the array is left alone", "[c5][wifi]")
{
    sim_begin_fck(NULL, NULL);
    fck_wifi_network_t ap[2];
    size_t        n = 77;
    memset(ap, 0x5A, sizeof(ap));
    text_begin();
    ap_line("alpha", 0x11, 1, "2g", -70, "open");
    text_add("#DONE scan rc=1\r\n");
    script_text("scan");
    TEST_ASSERT_EQUAL_MESSAGE(KODE_IO_ERROR, fck_core_wifi_scan(FCK_BAND_ALL, ap, 2U, &n), "Wi-Fi not up");
    TEST_ASSERT_EQUAL_UINT(77, n);
    TEST_ASSERT_EQUAL_HEX8(0x5A, ((uint8_t *)ap)[0]);

    sim_begin_fck(NULL, NULL);
    memset(ap, 0x5A, sizeof(ap));
    sim_script(SIM_C5, "scan", "#DONE scan rc=2\r\n", 5U);
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_wifi_scan(FCK_BAND_ALL, ap, 2U, &n));
    TEST_ASSERT_EQUAL_HEX8(0x5A, ((uint8_t *)ap)[0]);

    sim_begin_fck(NULL, NULL);
    sim_script(SIM_C5, "scan", "#DONE scan\r\n", 5U);
    TEST_ASSERT_EQUAL_MESSAGE(KODE_IO_ERROR, fck_core_wifi_scan(FCK_BAND_ALL, ap, 2U, &n), "a #DONE with no rc");
}

TEST_CASE("fck_wifi_scan: no module", "[c5][wifi]")
{
    sim_begin_empty();
    fck_wifi_network_t ap[2];
    size_t        n = 77;
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_wifi_scan(FCK_BAND_ALL, ap, 2U, &n));
    TEST_ASSERT_EQUAL_UINT(77, n);
}

/* ---- Channels --------------------------------------------------------------------------------- */

static void chan_line(unsigned ch, const char *band, unsigned frames)
{
    text_add("#CHAN ch=%u band=%s frames=%u mgmt=%u data=0 ctrl=0 rssi_max=%d rssi_avg=%d dwell_ms=250\r\n",
             ch, band, frames, frames, frames != 0U ? -55 : -127, frames != 0U ? -78 : -127);
}

TEST_CASE("fck_wifi_channels: the firmware's order and counts, and the line it was asked with", "[c5][wifi]")
{
    sim_begin_fck(NULL, NULL);
    text_begin();
    for (unsigned c = 1U; c <= 13U; c++) {
        chan_line(c, "2g", c * 3U);
    }
    text_add("#CHANDONE n=13 ms=3500\r\n#DONE chan rc=0\r\n");
    script_text("chan");

    fck_wifi_channel_t ch[FCK_WIFI_CHANNEL_MAX];
    size_t          n = 99;
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_wifi_channels(FCK_BAND_2G, 250U, ch, FCK_WIFI_CHANNEL_MAX, &n));
    TEST_ASSERT_EQUAL_UINT(13, n);
    TEST_ASSERT_EQUAL_STRING("chan 2g 250", sim_sent_line(SIM_C5, 0));
    TEST_ASSERT_EQUAL_UINT8(1, ch[0].channel);
    TEST_ASSERT_EQUAL_UINT8(13, ch[12].channel);
    TEST_ASSERT_EQUAL(FCK_BAND_2G, ch[0].band);
    TEST_ASSERT_EQUAL_UINT32(3, ch[0].frames);
    TEST_ASSERT_EQUAL_UINT32(3, ch[0].mgmt);
    TEST_ASSERT_EQUAL_UINT32(0, ch[0].data);
    TEST_ASSERT_EQUAL_INT8(-55, ch[0].rssi_max);
    TEST_ASSERT_EQUAL_INT8(-78, ch[0].rssi_avg);
    TEST_ASSERT_EQUAL_UINT16(250, ch[0].dwell_ms);

    /* Quiet channels say so. */
    sim_begin_fck(NULL, NULL);
    text_begin();
    chan_line(36, "5g", 0U);
    text_add("#CHANDONE n=1 ms=300\r\n#DONE chan rc=0\r\n");
    script_text("chan 5g");
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_wifi_channels(FCK_BAND_5G, 100U, ch, FCK_WIFI_CHANNEL_MAX, &n));
    TEST_ASSERT_EQUAL_STRING("chan 5g 100", sim_sent_line(SIM_C5, 0));
    TEST_ASSERT_EQUAL_UINT(1, n);
    TEST_ASSERT_EQUAL(FCK_BAND_5G, ch[0].band);
    TEST_ASSERT_EQUAL_INT8(FCK_WIFI_RSSI_NONE, ch[0].rssi_max);
}

TEST_CASE("fck_wifi_channels: room for every channel of the band, or nothing is sent", "[c5][wifi]")
{
    sim_begin_fck(NULL, NULL);
    sim_script(SIM_C5, "chan", "#CHANDONE n=0 ms=1\r\n#DONE chan rc=0\r\n", 5U);
    fck_wifi_channel_t ch[FCK_WIFI_CHANNEL_MAX];
    size_t          n = 99;
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_wifi_channels(FCK_BAND_2G, 250U, ch, FCK_WIFI_CHANNEL_2G_MAX - 1U, &n));
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_wifi_channels(FCK_BAND_5G, 250U, ch, FCK_WIFI_CHANNEL_5G_MAX - 1U, &n));
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_wifi_channels(FCK_BAND_ALL, 250U, ch, FCK_WIFI_CHANNEL_MAX - 1U, &n));
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_wifi_channels(FCK_BAND_ALL, 250U, NULL, FCK_WIFI_CHANNEL_MAX, &n));
    TEST_ASSERT_EQUAL_UINT(0, sent_other_than_info(SIM_C5));
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_wifi_channels(FCK_BAND_2G, 250U, ch, FCK_WIFI_CHANNEL_2G_MAX, &n));
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_wifi_channels(FCK_BAND_5G, 250U, ch, FCK_WIFI_CHANNEL_5G_MAX, &n));
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_wifi_channels(FCK_BAND_ALL, 250U, ch, FCK_WIFI_CHANNEL_MAX, &n));
    TEST_ASSERT_EQUAL_UINT(3, sent_other_than_info(SIM_C5));
}

TEST_CASE("fck_wifi_channels: a dwell outside 20 to 5000, a band outside the three", "[c5][wifi]")
{
    sim_begin_fck(NULL, NULL);
    fck_wifi_channel_t ch[FCK_WIFI_CHANNEL_MAX];
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_wifi_channels(FCK_BAND_ALL, 19U, ch, FCK_WIFI_CHANNEL_MAX, NULL));
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_wifi_channels(FCK_BAND_ALL, 5001U, ch, FCK_WIFI_CHANNEL_MAX, NULL));
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_wifi_channels((fck_band_t)3, 250U, ch, FCK_WIFI_CHANNEL_MAX, NULL));
    TEST_ASSERT_EQUAL_UINT(0, sent_other_than_info(SIM_C5));
}

TEST_CASE("fck_wifi_channels: a firmware before 1.1 has no `chan`, and is not asked", "[c5][wifi]")
{
    sim_begin_fck(SIM_C5_BANNER_10, NULL);
    fck_wifi_channel_t ch[FCK_WIFI_CHANNEL_MAX];
    size_t          n = 99;
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_wifi_channels(FCK_BAND_ALL, 250U, ch, FCK_WIFI_CHANNEL_MAX, &n));
    TEST_ASSERT_EQUAL_UINT(99, n);
    TEST_ASSERT_EQUAL_UINT(0, sent_other_than_info(SIM_C5));
}

TEST_CASE("fck_wifi_channels: a firmware that lists more than the band has is NO_RESOURCES", "[c5][wifi]")
{
    sim_begin_fck(NULL, NULL);
    text_begin();
    for (unsigned c = 1U; c <= 14U; c++) {
        chan_line(c, "2g", 1U);
    }
    text_add("#CHANDONE n=14 ms=3500\r\n#DONE chan rc=0\r\n");
    script_text("chan");
    fck_wifi_channel_t ch[FCK_WIFI_CHANNEL_2G_MAX];
    size_t          n = 99;
    memset(ch, 0x5A, sizeof(ch));
    TEST_ASSERT_EQUAL(KODE_NO_RESOURCES, fck_core_wifi_channels(FCK_BAND_2G, 250U, ch, FCK_WIFI_CHANNEL_2G_MAX, &n));
    TEST_ASSERT_EQUAL_UINT(99, n);
    TEST_ASSERT_EQUAL_HEX8(0x5A, ((uint8_t *)ch)[0]);
}

/* ---- Frames ------------------------------------------------------------------------------------ */

/* The records of the contract (ESPEC 1) as the firmware prints them. */
#define F_BEACON "gAAGzLQA////////qrvM3e4BqrvM3e4BAQIDBAUGBwhkABEEAARLb2Rl"
#define F_DEAUTH "wAALwx4AAhEiM0RVqrvM3e4BqrvM3e4BBwA="
#define F_DATA   "iAEk0AQGqrvM3e4BAhEiM0RVCgsMDQ4P"
#define F_PROBE  "QAAGujwA////////AhEiM0RV////////AAA="
#define FRAMES_SUMMARY(ch, sent) \
    "#FRAMES ch=" #ch " ms=251 heard=412 mgmt=300 data=100 ctrl=12 beacon=40 probe_req=22 probe_resp=18 deauth=3 " \
    "disassoc=1 sent=" #sent " dropped=2\r\n"

/* A data frame of 24 bytes of record whose length on the air is @p serial: the order they came in. */
static void frame_numbered(unsigned serial)
{
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    uint8_t const rec[24] = { 0x88, 0x01, 36U, 0xD0, (uint8_t)(serial & 0xFFU), (uint8_t)(serial >> 8), 0xaa, 0xbb, 0xcc, 0xdd,
                              0xee, 0x01, 0x02, 0x11, 0x22, 0x33, 0x44, 0x55, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f };
    char     body[33];
    for (size_t i = 0U; i < 8U; i++) {
        uint32_t const b = ((uint32_t)rec[3U * i] << 16) | ((uint32_t)rec[3U * i + 1U] << 8) | rec[3U * i + 2U];
        body[4U * i]      = alphabet[(b >> 18) & 63U];
        body[4U * i + 1U] = alphabet[(b >> 12) & 63U];
        body[4U * i + 2U] = alphabet[(b >> 6) & 63U];
        body[4U * i + 3U] = alphabet[b & 63U];
    }
    body[32] = '\0';
    text_add("#F %s\r\n", body);
}

TEST_CASE("fck_wifi_frames: the firmware's frames in the order heard, its counts, and the line it was asked with", "[c5][frames]")
{
    sim_begin_fck(SIM_C5_BANNER_24, NULL);
    text_begin();
    text_add("tuning channel 6\r\n");                 /* a human line: nobody reads it */
    text_add("#F " F_BEACON "\r\n#F " F_PROBE "\r\n#F " F_DEAUTH "\r\n#F " F_DATA "\r\n");
    text_add(FRAMES_SUMMARY(6, 4));
    text_add("#DONE frames rc=0\r\n");
    script_text("frames");

    fck_wifi_frame_t             f[8];
    fck_wifi_frames_statistics_t st;
    size_t                       n = 99;
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_wifi_frames(6U, 250U, f, 8U, &n, &st));
    TEST_ASSERT_EQUAL_STRING("frames ch=6 ms=250", sim_sent_line(SIM_C5, 0));
    TEST_ASSERT_EQUAL_UINT(4, n);
    TEST_ASSERT_EQUAL_HEX8(0x80, f[0].fc[0]);
    TEST_ASSERT_EQUAL_INT8(-52, f[0].rssi);
    TEST_ASSERT_EQUAL_UINT16(180, f[0].len);
    TEST_ASSERT_EQUAL_UINT8(18, f[0].head_len);
    TEST_ASSERT_EQUAL_HEX8('e', f[0].head[17]);
    TEST_ASSERT_EQUAL_HEX8(0x40, f[1].fc[0]);
    TEST_ASSERT_EQUAL_UINT8(2, f[1].head_len);
    TEST_ASSERT_EQUAL_HEX8(0xc0, f[2].fc[0]);
    TEST_ASSERT_EQUAL_UINT8(11, f[2].channel);
    TEST_ASSERT_EQUAL_HEX8(0x07, f[2].head[0]);
    TEST_ASSERT_EQUAL_HEX8(0x88, f[3].fc[0]);
    TEST_ASSERT_EQUAL_UINT16(1540, f[3].len);
    TEST_ASSERT_EQUAL_UINT8(0, f[3].head_len);
    TEST_ASSERT_EQUAL_UINT8(6, st.channel);
    TEST_ASSERT_EQUAL_UINT32(251, st.ms);
    TEST_ASSERT_EQUAL_UINT32(412, st.heard);
    TEST_ASSERT_EQUAL_UINT32(40, st.beacon);
    TEST_ASSERT_EQUAL_UINT32(22, st.probe_request);
    TEST_ASSERT_EQUAL_UINT32(18, st.probe_response);
    TEST_ASSERT_EQUAL_UINT32(3, st.deauth);
    TEST_ASSERT_EQUAL_UINT32(1, st.disassoc);
    TEST_ASSERT_EQUAL_UINT32(4, st.sent);
    TEST_ASSERT_EQUAL_UINT32(2, st.dropped);

    /* A channel on 5 GHz, and a short listen. */
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_wifi_frames(36U, 20U, f, 8U, NULL, NULL));
    TEST_ASSERT_EQUAL_STRING("frames ch=36 ms=20", sim_sent_line(SIM_C5, 1));
}

TEST_CASE("fck_wifi_frames: no more than FCK_WIFI_FRAME_MAX, the first ones, and sent says how many there were", "[c5][frames]")
{
    sim_begin_fck(SIM_C5_BANNER_24, NULL);
    text_begin();
    for (unsigned i = 0U; i < 140U; i++) {
        frame_numbered(i);
    }
    text_add(FRAMES_SUMMARY(11, 140));
    text_add("#DONE frames rc=0\r\n");
    script_text("frames");

    static fck_wifi_frame_t f[200];
    fck_wifi_frames_statistics_t st;
    size_t                       n = 0;
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_wifi_frames(11U, 5000U, f, 200U, &n, &st));
    TEST_ASSERT_EQUAL_UINT(FCK_WIFI_FRAME_MAX, n);
    for (size_t i = 0U; i < n; i++) {
        TEST_ASSERT_EQUAL_UINT16(i, f[i].len);
    }
    TEST_ASSERT_EQUAL_UINT32(140, st.sent);

    TEST_ASSERT_EQUAL(KODE_OK, fck_core_wifi_frames(11U, 250U, f, 3U, &n, &st));
    TEST_ASSERT_EQUAL_UINT(3, n);
    TEST_ASSERT_EQUAL_UINT16(2, f[2].len);
    TEST_ASSERT_EQUAL_UINT32(140, st.sent);
}

TEST_CASE("fck_wifi_frames: a line that is not a record is skipped, one that is goes on", "[c5][frames]")
{
    sim_begin_fck(SIM_C5_BANNER_24, NULL);
    text_begin();
    text_add("#F not-base64!\r\n#F AAAA\r\n#F " F_DATA "\r\n#F " F_DATA "A\r\n");
    text_add(FRAMES_SUMMARY(36, 4));
    text_add("#DONE frames rc=0\r\n");
    script_text("frames");
    fck_wifi_frame_t f[4];
    size_t           n = 0;
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_wifi_frames(36U, 250U, f, 4U, &n, NULL));
    TEST_ASSERT_EQUAL_UINT(1, n);
    TEST_ASSERT_EQUAL_UINT16(1540, f[0].len);
}

TEST_CASE("fck_wifi_frames: bad arguments, or a firmware before 2.4, and nothing is sent", "[c5][frames]")
{
    sim_begin_fck(SIM_C5_BANNER_24, NULL);
    fck_wifi_frame_t f[4];
    size_t           n = 77;
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_wifi_frames(6U, 250U, NULL, 4U, &n, NULL));
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_wifi_frames(6U, 250U, f, 0U, &n, NULL));
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_wifi_frames(0U, 250U, f, 4U, &n, NULL));
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_wifi_frames(178U, 250U, f, 4U, &n, NULL));
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_wifi_frames(6U, 19U, f, 4U, &n, NULL));
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_wifi_frames(6U, 5001U, f, 4U, &n, NULL));
    TEST_ASSERT_EQUAL_UINT(0, sent_other_than_info(SIM_C5));

    static const char *const old[] = { SIM_C5_BANNER_23, SIM_C5_BANNER_22, SIM_C5_BANNER_20, SIM_C5_BANNER_15, SIM_C5_BANNER_10, SIM_C5_BANNER_OWN };
    for (size_t i = 0U; i < sizeof(old) / sizeof(old[0]); i++) {
        sim_begin_fck(old[i], NULL);
        TEST_ASSERT_EQUAL_MESSAGE(KODE_DENIED, fck_core_wifi_frames(6U, 250U, f, 4U, &n, NULL), old[i]);
        TEST_ASSERT_EQUAL_UINT(77, n);
        TEST_ASSERT_EQUAL_UINT(0, sent_other_than_info(SIM_C5));
    }
}

TEST_CASE("fck_wifi_frames: it listens, so a firmware without the radio limits is still asked", "[c5][frames]")
{
    sim_begin_fck("#C5INFO fw=fck-c5 ver=2.4 chip=esp32c5 rev=1.2 mac=aa:bb:cc:dd:ee:03", NULL);
    text_begin();
    text_add(FRAMES_SUMMARY(6, 0));
    text_add("#DONE frames rc=0\r\n");
    script_text("frames");
    fck_wifi_frame_t f[1];
    size_t           n = 77;
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_wifi_frames(6U, 250U, f, 1U, &n, NULL));
    TEST_ASSERT_EQUAL_UINT(0, n);
}

TEST_CASE("fck_wifi_frames: a fck-c5 0.2.0 is asked by the word in its caps=, and a 0.1.0, which lacks it, is not", "[c5][frames]")
{
    fck_wifi_frame_t f[1];
    size_t           n = 77;

    sim_begin_fck(SIM_C5_BANNER_020, NULL);
    text_begin();
    text_add(FRAMES_SUMMARY(6, 0));
    text_add("#DONE frames rc=0\r\n");
    script_text("frames");
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_wifi_frames(6U, 250U, f, 1U, &n, NULL));
    TEST_ASSERT_EQUAL_UINT(0, n);
    TEST_ASSERT_EQUAL_UINT(1, sent_other_than_info(SIM_C5));

    /* 0.1.0 says what it does too, and `frames` is not in it: its version is not looked at. */
    n = 77;
    sim_begin_fck(SIM_C5_BANNER_010, NULL);
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_wifi_frames(6U, 250U, f, 1U, &n, NULL));
    TEST_ASSERT_EQUAL_UINT(77, n);
    TEST_ASSERT_EQUAL_UINT(0, sent_other_than_info(SIM_C5));
}

TEST_CASE("fck_wifi_frames: the firmware's rc, and a listen that ended with no summary, write nothing", "[c5][frames]")
{
    fck_wifi_frame_t             f[4];
    fck_wifi_frames_statistics_t st;
    size_t                       n = 77;
    memset(f, 0x5A, sizeof(f));
    memset(&st, 0x5A, sizeof(st));

    sim_begin_fck(SIM_C5_BANNER_24, NULL);
    sim_script(SIM_C5, "frames", "frames: the driver refused channel 149: ESP_ERR_INVALID_ARG\r\n#DONE frames rc=1\r\n", 5U);
    TEST_ASSERT_EQUAL(KODE_IO_ERROR, fck_core_wifi_frames(149U, 250U, f, 4U, &n, &st));
    TEST_ASSERT_TRUE_MESSAGE(strstr(fck_error(), "rc 1") != NULL, fck_error());

    sim_begin_fck(SIM_C5_BANNER_24, NULL);
    sim_script(SIM_C5, "frames", "usage: frames ch=<n> [ms=<dwell>]\r\n#DONE frames rc=2\r\n", 5U);
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_wifi_frames(6U, 250U, f, 4U, &n, &st));
    TEST_ASSERT_TRUE_MESSAGE(strstr(fck_error(), "rc 2") != NULL, fck_error());

    sim_begin_fck(SIM_C5_BANNER_24, NULL);
    text_begin();
    text_add("#F " F_DATA "\r\n#DONE frames rc=0\r\n");
    script_text("frames");
    TEST_ASSERT_EQUAL(KODE_IO_ERROR, fck_core_wifi_frames(6U, 250U, f, 4U, &n, &st));

    TEST_ASSERT_EQUAL_UINT(77, n);
    TEST_ASSERT_EQUAL_HEX8(0x5A, f[0].fc[0]);
    TEST_ASSERT_EQUAL_UINT8(0x5A, st.channel);
}

/* ---- Bluetooth LE -------------------------------------------------------------------------------- */

static void ble_line(unsigned last, const char *type, int rssi, const char *name, unsigned adv)
{
    text_add("#BLE addr=aa:bb:cc:dd:%02x:%02x type=%s rssi=%d name=\"%s\" adv=%u\r\n", last >> 8, last & 0xFFU,
             type, rssi, name, adv);
}

TEST_CASE("fck_ble_scan: strongest first whatever order the firmware sent, with names and kinds", "[c5][ble]")
{
    sim_begin_fck(NULL, NULL);
    text_begin();
    ble_line(0x0001, "pub", -80, "Tag One", 4);
    ble_line(0x0002, "rnd", -45, "", 12);
    ble_line(0x0003, "pub", -80, "Tag Three", 2);
    ble_line(0x0004, "rnd", -60, "Kode Dot", 9);
    text_add("#BLEDONE n=4 ms=5003\r\n#DONE ble rc=0\r\n");
    script_text("ble scan");

    fck_ble_device_t d[8];
    size_t        n = 99;
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_ble_scan(5U, d, 8U, &n));
    TEST_ASSERT_EQUAL_UINT(4, n);
    TEST_ASSERT_EQUAL_STRING("ble scan 5", sim_sent_line(SIM_C5, 0));
    TEST_ASSERT_EQUAL_INT8(-45, d[0].rssi);
    TEST_ASSERT_TRUE(d[0].random);
    TEST_ASSERT_EQUAL_STRING("", d[0].name);
    TEST_ASSERT_EQUAL_UINT8(0, d[0].name_len);
    TEST_ASSERT_EQUAL_UINT16(12, d[0].adv);
    TEST_ASSERT_EQUAL_STRING("Kode Dot", d[1].name);
    TEST_ASSERT_EQUAL_UINT8(8, d[1].name_len);
    TEST_ASSERT_TRUE(d[1].random);
    TEST_ASSERT_EQUAL_STRING("Tag One", d[2].name);      /* equal: the firmware's order */
    TEST_ASSERT_EQUAL_STRING("Tag Three", d[3].name);
    TEST_ASSERT_FALSE(d[2].random);
    const uint8_t a0[6] = { 0xaa, 0xbb, 0xcc, 0xdd, 0x00, 0x02 };
    TEST_ASSERT_EQUAL_HEX8_ARRAY(a0, d[0].addr, 6);
}

TEST_CASE("fck_ble_scan: no more than 128, the weakest going; a smaller array holds the strongest", "[c5][ble]")
{
    sim_begin_fck(NULL, NULL);
    text_begin();
    for (unsigned i = 0U; i < 130U; i++) {
        ble_line(i, "pub", -127 + (int)i, "", 1);     /* -127 ... 2, weakest first */
    }
    text_add("#BLEDONE n=130 ms=5003\r\n#DONE ble rc=0\r\n");
    script_text("ble scan");
    static fck_ble_device_t d[200];
    size_t               n = 0;
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_ble_scan(1U, d, 200U, &n));
    TEST_ASSERT_EQUAL_UINT(FCK_BLE_DEVICE_MAX, n);
    TEST_ASSERT_EQUAL_INT8(2, d[0].rssi);
    TEST_ASSERT_EQUAL_INT8(-125, d[127].rssi);       /* the two weakest, -127 and -126, went */
    for (size_t i = 1U; i < n; i++) {
        TEST_ASSERT_TRUE(d[i - 1U].rssi >= d[i].rssi);
    }
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_ble_scan(1U, d, 3U, &n));
    TEST_ASSERT_EQUAL_UINT(3, n);
    TEST_ASSERT_EQUAL_INT8(2, d[0].rssi);
    TEST_ASSERT_EQUAL_INT8(0, d[2].rssi);
}

TEST_CASE("fck_ble_scan: seconds 1 to 60, an array, and a firmware that has `ble scan`", "[c5][ble]")
{
    sim_begin_fck(NULL, NULL);
    fck_ble_device_t d[2];
    size_t        n = 77;
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_ble_scan(0U, d, 2U, &n));
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_ble_scan(61U, d, 2U, &n));
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_ble_scan(5U, NULL, 2U, &n));
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_ble_scan(5U, d, 0U, &n));
    TEST_ASSERT_EQUAL_UINT(0, sent_other_than_info(SIM_C5));

    sim_begin_fck(SIM_C5_BANNER_10, NULL);
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_ble_scan(5U, d, 2U, &n));
    TEST_ASSERT_EQUAL_UINT(77, n);
    TEST_ASSERT_EQUAL_UINT(0, sent_other_than_info(SIM_C5));

    sim_begin_fck(NULL, NULL);
    sim_script(SIM_C5, "ble scan", "#DONE ble rc=1\r\n", 5U);
    TEST_ASSERT_EQUAL_MESSAGE(KODE_IO_ERROR, fck_core_ble_scan(60U, d, 2U, &n), "Bluetooth not up");
    TEST_ASSERT_EQUAL_STRING("ble scan 60", sim_sent_line(SIM_C5, 0));
}

TEST_CASE("fck_ble_scan: it listens, so a firmware without the radio limits is still asked", "[c5][ble]")
{
    sim_begin_fck(SIM_C5_BANNER_15, NULL);
    sim_script(SIM_C5, "ble scan", "#BLEDONE n=0 ms=1\r\n#DONE ble rc=0\r\n", 5U);
    fck_ble_device_t d[2];
    size_t        n = 77;
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_ble_scan(1U, d, 2U, &n));
    TEST_ASSERT_EQUAL_UINT(0, n);
    sim_script(SIM_C5, "chan", "#CHANDONE n=0 ms=1\r\n#DONE chan rc=0\r\n", 5U);
    fck_wifi_channel_t ch[FCK_WIFI_CHANNEL_MAX];
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_wifi_channels(FCK_BAND_ALL, 250U, ch, FCK_WIFI_CHANNEL_MAX, &n));
}

/* A #BLE as the firmware prints it from 2.2: what was there, then how its latest report was sent
 * (evt=), when it was first and last heard, and what it carried - no data= when it carried none. */
static void ble_line_22(unsigned last, int rssi, unsigned evt, unsigned first_ms, unsigned last_ms, const char *data)
{
    text_add("#BLE addr=aa:bb:cc:dd:%02x:%02x type=rnd rssi=%d name=\"\" adv=3 evt=%u first=%u last=%u%s%s\r\n",
             last >> 8, last & 0xFFU, rssi, evt, first_ms, last_ms, (data[0] != '\0') ? " data=" : "", data);
}

/* In shape, what a tracker away from its owner sends: 30 bytes of manufacturer data, company
 * 0x004C. The bytes after the company are made up. */
#define ADV_TRACKER "1eff4c001219" "00a1a2a3a4a5a6a7a8a9aaabacadaeafb0b1b2b3b4b5b6" "0102"
/* Flags, the 16-bit service 0xFD5A listed, and 10 bytes of that service's data. */
#define ADV_SERVICE "020106" "03035afd" "0d165afd" "13112233445566778899"

TEST_CASE("fck_ble_scan: what each device advertised comes with it, whatever place it is sorted to", "[c5][ble]")
{
    sim_begin_fck(NULL, NULL);
    text_begin();
    ble_line_22(0x0001, -80, 3U, 140U, 4130U, ADV_TRACKER);
    ble_line(0x0002, "pub", -70, "Old style", 2);            /* as a firmware before 2.2 prints it */
    ble_line_22(0x0003, -40, 0U, 12U, 4990U, ADV_SERVICE);
    ble_line_22(0x0004, -90, 1U, 2500U, 2500U, "");            /* directed: it carries nothing */
    text_add("#BLEDONE n=4 ms=5003\r\n#DONE ble rc=0\r\n");
    script_text("ble scan");

    fck_ble_device_t d[8];
    size_t           n = 0;
    memset(d, 0x5A, sizeof(d));
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_ble_scan(5U, d, 8U, &n));
    TEST_ASSERT_EQUAL_UINT(4, n);

    /* The strongest: connectable, with its service's data. */
    TEST_ASSERT_EQUAL_INT8(-40, d[0].rssi);
    TEST_ASSERT_EQUAL(FCK_BLE_ADVERTISING_CONNECTABLE, d[0].advertising);
    TEST_ASSERT_EQUAL_UINT32(12, d[0].first_ms);
    TEST_ASSERT_EQUAL_UINT32(4990, d[0].last_ms);
    TEST_ASSERT_EQUAL_UINT8(21, d[0].data_len);
    const uint8_t service[7] = { 0x02, 0x01, 0x06, 0x03, 0x03, 0x5a, 0xfd };
    TEST_ASSERT_EQUAL_HEX8_ARRAY(service, d[0].data, 7);

    /* A firmware before 2.2 says none of it. */
    TEST_ASSERT_EQUAL_STRING("Old style", d[1].name);
    TEST_ASSERT_EQUAL(FCK_BLE_ADVERTISING_UNKNOWN, d[1].advertising);
    TEST_ASSERT_EQUAL_UINT32(0, d[1].first_ms);
    TEST_ASSERT_EQUAL_UINT32(0, d[1].last_ms);
    TEST_ASSERT_EQUAL_UINT8(0, d[1].data_len);

    /* The tracker: all 31 bytes, first to last. */
    TEST_ASSERT_EQUAL_INT8(-80, d[2].rssi);
    TEST_ASSERT_EQUAL(FCK_BLE_ADVERTISING_NOT_CONNECTABLE, d[2].advertising);
    TEST_ASSERT_EQUAL_UINT32(140, d[2].first_ms);
    TEST_ASSERT_EQUAL_UINT32(4130, d[2].last_ms);
    TEST_ASSERT_EQUAL_UINT8(FCK_BLE_DATA_MAX, d[2].data_len);
    TEST_ASSERT_EQUAL_HEX8(0x1e, d[2].data[0]);
    TEST_ASSERT_EQUAL_HEX8(0x02, d[2].data[30]);

    /* Directed: known how it was sent, and nothing in it. */
    TEST_ASSERT_EQUAL(FCK_BLE_ADVERTISING_DIRECTED, d[3].advertising);
    TEST_ASSERT_EQUAL_UINT32(2500, d[3].first_ms);
    TEST_ASSERT_EQUAL_UINT8(0, d[3].data_len);
}

/* ---- What a device advertised, read: plain C, no module ------------------------------------------- */

/* A device as a scan would give it, with @p hex as what it advertised. */
static fck_ble_device_t device_with(const char *hex)
{
    fck_ble_device_t d;
    memset(&d, 0, sizeof(d));
    size_t const len = strlen(hex) / 2U;
    TEST_ASSERT_TRUE(len <= sizeof(d.data));
    for (size_t i = 0U; i < len; i++) {
        unsigned v = 0U;
        TEST_ASSERT_EQUAL(1, sscanf(&hex[2U * i], "%2x", &v));
        d.data[i] = (uint8_t)v;
    }
    d.data_len = (uint8_t)len;
    return d;
}

TEST_CASE("fck_ble_manufacturer_data: the company and the bytes after it", "[ble][advertisement]")
{
    /* Flags, then 7 bytes from company 0x004C. */
    fck_ble_device_t d = device_with("02011a" "0aff4c0010050b1caabbcc");
    uint16_t company = 0U;
    uint8_t  out[FCK_BLE_DATA_MAX];
    size_t   n = 99;
    memset(out, 0x5A, sizeof(out));
    TEST_ASSERT_TRUE(fck_ble_manufacturer_data(&d, &company, out, sizeof(out), &n));
    TEST_ASSERT_EQUAL_HEX16(0x004C, company);
    TEST_ASSERT_EQUAL_UINT(7, n);
    const uint8_t want[7] = { 0x10, 0x05, 0x0b, 0x1c, 0xaa, 0xbb, 0xcc };
    TEST_ASSERT_EQUAL_HEX8_ARRAY(want, out, 7);
    TEST_ASSERT_EQUAL_HEX8(0x5A, out[7]);

    /* A smaller array holds its first bytes; only the company, or only whether, can be asked. */
    memset(out, 0x5A, sizeof(out));
    TEST_ASSERT_TRUE(fck_ble_manufacturer_data(&d, &company, out, 2U, &n));
    TEST_ASSERT_EQUAL_UINT(2, n);
    TEST_ASSERT_EQUAL_HEX8(0x05, out[1]);
    TEST_ASSERT_EQUAL_HEX8(0x5A, out[2]);
    company = 0U;
    n = 99;
    TEST_ASSERT_TRUE(fck_ble_manufacturer_data(&d, &company, NULL, 0U, &n));
    TEST_ASSERT_EQUAL_HEX16(0x004C, company);
    TEST_ASSERT_EQUAL_UINT(0, n);
    TEST_ASSERT_TRUE(fck_ble_manufacturer_data(&d, NULL, NULL, 0U, NULL));

    /* A whole advertisement of it: 27 bytes after the company. */
    d = device_with(ADV_TRACKER);
    TEST_ASSERT_TRUE(fck_ble_manufacturer_data(&d, &company, out, sizeof(out), &n));
    TEST_ASSERT_EQUAL_HEX16(0x004C, company);
    TEST_ASSERT_EQUAL_UINT(27, n);
    TEST_ASSERT_EQUAL_HEX8(0x12, out[0]);
    TEST_ASSERT_EQUAL_HEX8(0x02, out[26]);
}

TEST_CASE("fck_ble_manufacturer_data: none, and nothing is written", "[ble][advertisement]")
{
    uint16_t company = 0x7777U;
    uint8_t  out[FCK_BLE_DATA_MAX];
    size_t   n = 99;
    memset(out, 0x5A, sizeof(out));
    fck_ble_device_t d = device_with(ADV_SERVICE);                 /* it advertises something else  */
    TEST_ASSERT_FALSE(fck_ble_manufacturer_data(&d, &company, out, sizeof(out), &n));
    d = device_with("");                                           /* it advertises nothing        */
    TEST_ASSERT_FALSE(fck_ble_manufacturer_data(&d, &company, out, sizeof(out), &n));
    d = device_with("020106" "02ff4c");                            /* too short to name a company  */
    TEST_ASSERT_FALSE(fck_ble_manufacturer_data(&d, &company, out, sizeof(out), &n));
    TEST_ASSERT_FALSE(fck_ble_manufacturer_data(NULL, &company, out, sizeof(out), &n));
    d = device_with("02011a" "0aff4c0010050b1caabbcc");
    TEST_ASSERT_FALSE(fck_ble_manufacturer_data(&d, &company, NULL, 4U, &n));   /* room said, array not given */
    TEST_ASSERT_EQUAL_HEX16(0x7777, company);
    TEST_ASSERT_EQUAL_UINT(99, n);
    TEST_ASSERT_EQUAL_HEX8(0x5A, out[0]);
}

TEST_CASE("fck_ble_service_data: a service's bytes, by its 16-bit UUID", "[ble][advertisement]")
{
    fck_ble_device_t d = device_with(ADV_SERVICE);
    uint8_t out[FCK_BLE_DATA_MAX];
    size_t  n = 99;
    memset(out, 0x5A, sizeof(out));
    TEST_ASSERT_TRUE(fck_ble_service_data(&d, 0xFD5AU, out, sizeof(out), &n));
    TEST_ASSERT_EQUAL_UINT(10, n);
    const uint8_t want[10] = { 0x13, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99 };
    TEST_ASSERT_EQUAL_HEX8_ARRAY(want, out, 10);
    TEST_ASSERT_EQUAL_HEX8(0x5A, out[10]);
    TEST_ASSERT_TRUE(fck_ble_service_data(&d, 0xFD5AU, out, 3U, &n));
    TEST_ASSERT_EQUAL_UINT(3, n);
    TEST_ASSERT_TRUE(fck_ble_service_data(&d, 0xFD5AU, NULL, 0U, NULL));

    /* Two services' data in one advertisement: each by its own UUID, and a UUID with no bytes
     * after it is there too, with none. */
    d = device_with("0516aafe4001" "04165afd77" "0316edfe");
    TEST_ASSERT_TRUE(fck_ble_service_data(&d, 0xFEAAU, out, sizeof(out), &n));
    TEST_ASSERT_EQUAL_UINT(2, n);
    TEST_ASSERT_EQUAL_HEX8(0x40, out[0]);
    TEST_ASSERT_TRUE(fck_ble_service_data(&d, 0xFD5AU, out, sizeof(out), &n));
    TEST_ASSERT_EQUAL_UINT(1, n);
    TEST_ASSERT_EQUAL_HEX8(0x77, out[0]);
    TEST_ASSERT_TRUE(fck_ble_service_data(&d, 0xFEEDU, out, sizeof(out), &n));
    TEST_ASSERT_EQUAL_UINT(0, n);
}

TEST_CASE("fck_ble_service_data: another service's, none, and nothing is written", "[ble][advertisement]")
{
    uint8_t out[FCK_BLE_DATA_MAX];
    size_t  n = 99;
    memset(out, 0x5A, sizeof(out));
    fck_ble_device_t d = device_with(ADV_SERVICE);
    TEST_ASSERT_FALSE(fck_ble_service_data(&d, 0xFEEDU, out, sizeof(out), &n));
    TEST_ASSERT_FALSE(fck_ble_service_data(&d, 0x5AFDU, out, sizeof(out), &n));    /* its bytes the other way round */
    d = device_with(ADV_TRACKER);
    TEST_ASSERT_FALSE(fck_ble_service_data(&d, 0x004CU, out, sizeof(out), &n));    /* a company is not a service */
    d = device_with("020106" "0216fd");                                            /* half a UUID */
    TEST_ASSERT_FALSE(fck_ble_service_data(&d, 0x00FDU, out, sizeof(out), &n));
    TEST_ASSERT_FALSE(fck_ble_service_data(NULL, 0xFD5AU, out, sizeof(out), &n));
    d = device_with(ADV_SERVICE);
    TEST_ASSERT_FALSE(fck_ble_service_data(&d, 0xFD5AU, NULL, 4U, &n));
    TEST_ASSERT_EQUAL_UINT(99, n);
    TEST_ASSERT_EQUAL_HEX8(0x5A, out[0]);
}

TEST_CASE("fck_ble_services: the 16-bit services it lists, whether it says the list is whole or not", "[ble][advertisement]")
{
    /* Two in a list it calls incomplete (0x02), one in a list it calls complete (0x03). */
    fck_ble_device_t d = device_with("05020f180a18" "0303edfe");
    uint16_t out[16];
    size_t   n = 99;
    memset(out, 0x5A, sizeof(out));
    TEST_ASSERT_TRUE(fck_ble_services(&d, out, 16U, &n));
    TEST_ASSERT_EQUAL_UINT(3, n);
    TEST_ASSERT_EQUAL_HEX16(0x180F, out[0]);
    TEST_ASSERT_EQUAL_HEX16(0x180A, out[1]);
    TEST_ASSERT_EQUAL_HEX16(0xFEED, out[2]);
    TEST_ASSERT_EQUAL_HEX16(0x5A5A, out[3]);

    /* A smaller array holds the first; with none it only says whether. */
    memset(out, 0x5A, sizeof(out));
    TEST_ASSERT_TRUE(fck_ble_services(&d, out, 2U, &n));
    TEST_ASSERT_EQUAL_UINT(2, n);
    TEST_ASSERT_EQUAL_HEX16(0x180A, out[1]);
    TEST_ASSERT_EQUAL_HEX16(0x5A5A, out[2]);
    TEST_ASSERT_TRUE(fck_ble_services(&d, NULL, 0U, &n));
    TEST_ASSERT_EQUAL_UINT(0, n);
    TEST_ASSERT_TRUE(fck_ble_services(&d, NULL, 0U, NULL));

    /* Half a UUID at the end of a list is not one. */
    d = device_with("0403edfe12");
    TEST_ASSERT_TRUE(fck_ble_services(&d, out, 16U, &n));
    TEST_ASSERT_EQUAL_UINT(1, n);
    TEST_ASSERT_EQUAL_HEX16(0xFEED, out[0]);

    /* As many as one advertisement can list: 14. */
    d = device_with("1d03" "0118021803180418051806180718081809180a180b180c180d180e18");
    TEST_ASSERT_TRUE(fck_ble_services(&d, out, 16U, &n));
    TEST_ASSERT_EQUAL_UINT(14, n);
    TEST_ASSERT_EQUAL_HEX16(0x180E, out[13]);
}

TEST_CASE("fck_ble_services: none listed, and nothing is written", "[ble][advertisement]")
{
    uint16_t out[16];
    size_t   n = 99;
    memset(out, 0x5A, sizeof(out));
    fck_ble_device_t d = device_with(ADV_TRACKER);
    TEST_ASSERT_FALSE(fck_ble_services(&d, out, 16U, &n));
    /* A service it carries data of and does not list is not listed: fck_ble_service_data() finds it. */
    d = device_with("0516aafe4001");
    TEST_ASSERT_FALSE(fck_ble_services(&d, out, 16U, &n));
    d = device_with("0203ed");                                     /* a list with half a UUID */
    TEST_ASSERT_FALSE(fck_ble_services(&d, out, 16U, &n));
    d = device_with("");
    TEST_ASSERT_FALSE(fck_ble_services(&d, out, 16U, &n));
    TEST_ASSERT_FALSE(fck_ble_services(NULL, out, 16U, &n));
    d = device_with("0303edfe");
    TEST_ASSERT_FALSE(fck_ble_services(&d, NULL, 4U, &n));
    TEST_ASSERT_EQUAL_UINT(99, n);
    TEST_ASSERT_EQUAL_HEX16(0x5A5A, out[0]);
}

TEST_CASE("fck_ble_appearance: what the device says it is", "[ble][advertisement]")
{
    /* Flags, a keyboard's appearance (0x03C1, the low byte first), then a service it lists. */
    fck_ble_device_t d = device_with("020106" "0319c103" "03031218");
    uint16_t appearance = 0x7777U;
    TEST_ASSERT_TRUE(fck_ble_appearance(&d, &appearance));
    TEST_ASSERT_EQUAL_HEX16(0x03C1, appearance);
    TEST_ASSERT_TRUE(fck_ble_appearance(&d, NULL));                /* only whether */

    /* Of two, the first. */
    d = device_with("03194000" "0319c103");
    TEST_ASSERT_TRUE(fck_ble_appearance(&d, &appearance));
    TEST_ASSERT_EQUAL_HEX16(0x0040, appearance);

    /* One that is not two bytes is not one, and the next is read. */
    d = device_with("0219c1" "0419c10300" "0319c000");
    TEST_ASSERT_TRUE(fck_ble_appearance(&d, &appearance));
    TEST_ASSERT_EQUAL_HEX16(0x00C0, appearance);
}

TEST_CASE("fck_ble_appearance: none, and nothing is written", "[ble][advertisement]")
{
    uint16_t appearance = 0x7777U;
    fck_ble_device_t d = device_with(ADV_SERVICE);                 /* it advertises something else  */
    TEST_ASSERT_FALSE(fck_ble_appearance(&d, &appearance));
    d = device_with("");                                           /* it advertises nothing         */
    TEST_ASSERT_FALSE(fck_ble_appearance(&d, &appearance));
    d = device_with("020106" "0219c1");                            /* half of one                   */
    TEST_ASSERT_FALSE(fck_ble_appearance(&d, &appearance));
    d = device_with("0419c10300");                                 /* a byte too many               */
    TEST_ASSERT_FALSE(fck_ble_appearance(&d, &appearance));
    d = device_with("020106" "0519c103");                          /* longer than what is left      */
    TEST_ASSERT_FALSE(fck_ble_appearance(&d, &appearance));
    TEST_ASSERT_FALSE(fck_ble_appearance(NULL, &appearance));
    TEST_ASSERT_EQUAL_HEX16(0x7777, appearance);
}

TEST_CASE("an advertisement that is not what it says ends where it stops making sense", "[ble][advertisement]")
{
    uint16_t company = 0U;
    uint16_t uuids[16];
    uint8_t  out[FCK_BLE_DATA_MAX];
    size_t   n = 0;

    /* A field that says it is longer than what is left: it and what follows are not read. */
    fck_ble_device_t d = device_with("020106" "09ff4c0010");
    TEST_ASSERT_FALSE(fck_ble_manufacturer_data(&d, &company, out, sizeof(out), &n));
    /* A field of no length ends the advertisement: what is after it is padding. */
    d = device_with("00" "05ff4c001005" "0303edfe");
    TEST_ASSERT_FALSE(fck_ble_manufacturer_data(&d, &company, out, sizeof(out), &n));
    TEST_ASSERT_FALSE(fck_ble_services(&d, uuids, 16U, &n));
    /* What comes before the broken field was read. */
    d = device_with("0303edfe" "0416aafe40" "1fff4c00");
    TEST_ASSERT_TRUE(fck_ble_services(&d, uuids, 16U, &n));
    TEST_ASSERT_TRUE(fck_ble_service_data(&d, 0xFEAAU, out, sizeof(out), &n));
    TEST_ASSERT_FALSE(fck_ble_manufacturer_data(&d, &company, out, sizeof(out), &n));
    /* A length an app wrote that no scan gives: nothing is read past the 31 bytes. */
    d = device_with(ADV_TRACKER);
    d.data_len = 200U;
    TEST_ASSERT_FALSE(fck_ble_manufacturer_data(&d, &company, out, sizeof(out), &n));
    d = device_with(ADV_SERVICE);
    d.data_len = 200U;
    TEST_ASSERT_FALSE(fck_ble_service_data(&d, 0xFD5AU, out, sizeof(out), &n));
    TEST_ASSERT_FALSE(fck_ble_services(&d, uuids, 16U, &n));
}

TEST_CASE("of two fields of a kind the first is read, and one too short to be of the kind is passed over",
          "[ble][advertisement]")
{
    uint16_t company = 0U;
    uint8_t  out[FCK_BLE_DATA_MAX];
    size_t   n = 0;

    fck_ble_device_t d = device_with("05ff4c00aabb" "05ffe000ccdd");
    TEST_ASSERT_TRUE(fck_ble_manufacturer_data(&d, &company, out, sizeof(out), &n));
    TEST_ASSERT_EQUAL_HEX16(0x004C, company);
    TEST_ASSERT_EQUAL_UINT(2, n);
    TEST_ASSERT_EQUAL_HEX8(0xAA, out[0]);

    /* One byte cannot name a company: the manufacturer's data is the next. */
    d = device_with("02ff4c" "05ffe000ccdd");
    TEST_ASSERT_TRUE(fck_ble_manufacturer_data(&d, &company, out, sizeof(out), &n));
    TEST_ASSERT_EQUAL_HEX16(0x00E0, company);
    TEST_ASSERT_EQUAL_UINT(2, n);
    TEST_ASSERT_EQUAL_HEX8(0xCC, out[0]);

    d = device_with("0416aafe01" "0416aafe02");
    TEST_ASSERT_TRUE(fck_ble_service_data(&d, 0xFEAAU, out, sizeof(out), &n));
    TEST_ASSERT_EQUAL_UINT(1, n);
    TEST_ASSERT_EQUAL_HEX8(0x01, out[0]);

    /* One byte cannot name a service either. */
    d = device_with("0216aa" "0416aafe02");
    TEST_ASSERT_TRUE(fck_ble_service_data(&d, 0xFEAAU, out, sizeof(out), &n));
    TEST_ASSERT_EQUAL_UINT(1, n);
    TEST_ASSERT_EQUAL_HEX8(0x02, out[0]);
}

/* ---- What is nobody's business --------------------------------------------------------------------- */

TEST_CASE("nothing of a network, a channel or a device reaches the log", "[c5][privacy]")
{
    sim_begin_fck(NULL, NULL);
    text_begin();
    ap_line("Cafe Central", 0x31, 6, "2g", -50, "wpa2_psk");
    ap_line("Casa Perez 5G", 0x32, 100, "5g", -60, "wpa3_psk");
    text_add("#SCANDONE n=2 ms=4000\r\n#DONE scan rc=0\r\n");
    script_text("scan");
    fck_wifi_network_t ap[4];
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_wifi_scan(FCK_BAND_ALL, ap, 4U, NULL));

    text_begin();
    ble_line(0x7A01, "rnd", -50, "Pulsera de Ana", 3);
    ble_line(0x7A02, "pub", -70, "Audifonos", 3);
    ble_line_22(0x7A03, -80, 3U, 120U, 4900U, ADV_TRACKER);
    text_add("#BLEDONE n=3 ms=5003\r\n#DONE ble rc=0\r\n");
    script_text("ble scan");
    fck_ble_device_t d[4];
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_ble_scan(5U, d, 4U, NULL));

    static const char *const needles[] = {
        "Cafe", "Central", "Casa", "Perez", "Pulsera", "Ana", "Audifonos", "ee:31", "ee:32", "dd:7a",
        "bssid", "ssid", "#AP", "#BLE", "addr=", "dd:7a", "data=", "4c00", "a1a2a3",
    };
    for (size_t i = 0U; i < sizeof(needles) / sizeof(needles[0]); i++) {
        TEST_ASSERT_FALSE_MESSAGE(sim_log_has(needles[i]), needles[i]);
    }
}

/* ---- The 0.1 firmwares: asked by what they say they do ----------------------------------------------- */

TEST_CASE("a chip's banner, in an app's terms: the 0.1 firmwares, a 0 that is a version, and their caps", "[c5][firmware]")
{
    sim_begin_fck(SIM_C5_BANNER_010, SIM_NRF_BANNER_010);
    fck_firmware_t a;
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_firmware(FCK_C5, &a));
    TEST_ASSERT_TRUE(a.known && a.official && a.compatible);
    TEST_ASSERT_EQUAL_STRING("fck-c5", a.fw);
    TEST_ASSERT_TRUE_MESSAGE(a.has_version, "0.1 is a version");
    TEST_ASSERT_EQUAL_UINT8(0, a.ver_major);
    TEST_ASSERT_EQUAL_UINT8(1, a.ver_minor);
    TEST_ASSERT_EQUAL_STRING("scan,chan,ble,gnss,wp,gtx,health", a.caps);
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_firmware(FCK_NRF, &a));
    TEST_ASSERT_TRUE(a.known && a.official && a.compatible);
    TEST_ASSERT_EQUAL_STRING("fck-nrf", a.fw);
    TEST_ASSERT_TRUE(a.has_version);
    TEST_ASSERT_EQUAL_UINT8(0, a.ver_major);
    TEST_ASSERT_EQUAL_UINT8(1, a.ver_minor);
    TEST_ASSERT_EQUAL_UINT8(0, a.ver_patch);
    TEST_ASSERT_EQUAL_STRING("spec,off,power,echo,ping,sniff,scfg,saddr,eu,done,wdt,echoidle,smp", a.caps);

    /* The first modules' say no caps, and one that printed no version has none. */
    sim_begin_fck(NULL, "#NRFINFO fw=nrf_fck_agent devid=0123456789ABCD15");
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_firmware(FCK_C5, &a));
    TEST_ASSERT_TRUE(a.has_version);
    TEST_ASSERT_EQUAL_STRING("", a.caps);
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_firmware(FCK_NRF, &a));
    TEST_ASSERT_FALSE(a.has_version);
    TEST_ASSERT_EQUAL_UINT8(0, a.ver_major);
}

TEST_CASE("a 0.1 firmware is asked what its caps= lists: the typed calls reach it, `scan` included", "[c5][wifi]")
{
    sim_begin_fck(SIM_C5_BANNER_010, SIM_NRF_BANNER_010);
    sim_script(SIM_C5, "scan", "#SCANDONE n=0 ms=1\r\n#DONE scan rc=0\r\n", 5U);
    sim_script(SIM_C5, "chan", "#CHANDONE n=0 ms=1\r\n#DONE chan rc=0\r\n", 5U);
    sim_script(SIM_C5, "ble scan", "#BLEDONE n=0 ms=1\r\n#DONE ble rc=0\r\n", 5U);
    sim_script(SIM_C5, "health", "#C5HEALTH uptime_ms=5 rst=poweron heap=9 heap_min=8\r\n#DONE health rc=0\r\n", 5U);
    fck_wifi_network_t ap[2];
    fck_wifi_channel_t ch[FCK_WIFI_CHANNEL_MAX];
    fck_ble_device_t   d[2];
    size_t             n  = 77;
    int32_t            rc = -1;
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_wifi_scan(FCK_BAND_2G, ap, 2U, &n));
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_wifi_channels(FCK_BAND_ALL, 250U, ch, FCK_WIFI_CHANNEL_MAX, &n));
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_ble_scan(1U, d, 2U, &n));
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_command(FCK_C5, "health", NULL, NULL, NULL, 0U, &rc));
    TEST_ASSERT_EQUAL_INT32(0, rc);
    TEST_ASSERT_EQUAL_UINT(1, sim_sent(SIM_C5, "scan 2g"));
    TEST_ASSERT_EQUAL_UINT(1, sim_sent(SIM_C5, "health"));

    /* One whose caps= lacks the word is not sent it, whatever its number. */
    sim_begin_fck("#C5INFO fw=fck-c5 ver=9.9 reg=EU lim=ok caps=scan,gnss,wp,gtx", NULL);
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_wifi_channels(FCK_BAND_ALL, 250U, ch, FCK_WIFI_CHANNEL_MAX, &n));
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_ble_scan(1U, d, 2U, &n));
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_command(FCK_C5, "health", NULL, NULL, NULL, 0U, &rc));
    TEST_ASSERT_EQUAL_UINT(0, sent_other_than_info(SIM_C5));
}

TEST_CASE("a 0.1 firmware whose Wi-Fi is not up inside the limits refuses the scan itself, and the call fails", "[c5][wifi]")
{
    sim_begin_fck("#C5INFO fw=fck-c5 ver=0.1 chip=esp32c5 rev=1.2 mac=aa:bb:cc:dd:ee:01 reg=EU lim=wifi-start "
                  "caps=scan,chan,ble,gnss,wp,gtx,health uptime_ms=40 rst=poweron",
                  NULL);
    fck_firmware_t a;
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_firmware(FCK_C5, &a));
    TEST_ASSERT_TRUE(a.official && a.compatible);
    /* The limits are the firmware's: the SDK asks, and the firmware says no. */
    sim_script(SIM_C5, "scan",
               "scan refused: lim=wifi-start, a limit the driver did not take, so nothing is transmitted\r\n"
               "#DONE scan rc=1\r\n",
               5U);
    fck_wifi_network_t ap[2];
    size_t             n  = 77;
    int32_t            rc = -1;
    TEST_ASSERT_EQUAL(KODE_IO_ERROR, fck_core_wifi_scan(FCK_BAND_ALL, ap, 2U, &n));
    TEST_ASSERT_NOT_NULL(strstr(fck_aos_error(), "said the scan failed"));
    TEST_ASSERT_EQUAL_UINT_MESSAGE(77, n, "nothing is handed over");
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_command(FCK_C5, "scan all", NULL, NULL, NULL, 0U, &rc));
    TEST_ASSERT_EQUAL_INT32(1, rc);
    TEST_ASSERT_EQUAL_UINT(2, sim_sent(SIM_C5, "scan all"));
    /* What only listens goes all the same. */
    sim_script(SIM_C5, "ble scan", "#BLEDONE n=0 ms=1\r\n#DONE ble rc=0\r\n", 5U);
    fck_ble_device_t d[2];
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_ble_scan(1U, d, 2U, &n));
}

TEST_CASE("fck_ble_scan: a scan the firmware cut short fails, and hands over what it heard until then", "[c5][ble]")
{
    sim_begin_fck(SIM_C5_BANNER_010, NULL);
    text_begin();
    ble_line(0x0001, "pub", -80, "Tag One", 4);
    ble_line(0x0002, "rnd", -45, "", 12);
    text_add("#BLEDONE n=2 ms=2100\r\n#DONE ble rc=1\r\n");
    script_text("ble scan");
    fck_ble_device_t d[4];
    size_t           n = 77;
    memset(d, 0, sizeof(d));
    TEST_ASSERT_EQUAL(KODE_IO_ERROR, fck_core_ble_scan(5U, d, 4U, &n));
    TEST_ASSERT_NOT_NULL(strstr(fck_aos_error(), "cut short"));
    TEST_ASSERT_EQUAL_UINT_MESSAGE(2, n, "what it heard is not thrown away");
    TEST_ASSERT_EQUAL_INT8(-45, d[0].rssi);
    TEST_ASSERT_EQUAL_INT8(-80, d[1].rssi);

    /* A scan that could not start heard nothing: failed, with none. */
    sim_begin_fck(SIM_C5_BANNER_010, NULL);
    sim_script(SIM_C5, "ble scan", "#DONE ble rc=1\r\n", 5U);
    n = 77;
    TEST_ASSERT_EQUAL(KODE_IO_ERROR, fck_core_ble_scan(5U, d, 4U, &n));
    TEST_ASSERT_EQUAL_UINT(0, n);
}

TEST_CASE("an empty line goes before every command, so half a line left in the chip is not glued to it", "[c5][gate]")
{
    sim_begin_fck(SIM_C5_BANNER_010, SIM_NRF_BANNER_010);
    sim_script(SIM_C5, "wp", "#WP host_wp_c5=0 c5_wp_host=0 driven=0\r\n#DONE wp rc=0\r\n", 5U);
    sim_script(SIM_NRF, "rf off", "#RFOFF ok=1\r\n#DONE rf_off rc=0\r\n", 5U);
    sim_sent_clear(SIM_C5);
    sim_sent_clear(SIM_NRF);
    int32_t rc = -1;
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_command(FCK_C5, "wp", NULL, NULL, NULL, 0U, &rc));
    TEST_ASSERT_EQUAL_INT32(0, rc);
    TEST_ASSERT_EQUAL_UINT(1, sim_blank_lines(SIM_C5));
    TEST_ASSERT_EQUAL_STRING("wp", sim_sent_line(SIM_C5, 0));
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_command(FCK_NRF, "rf off", NULL, NULL, NULL, 0U, &rc));
    TEST_ASSERT_EQUAL_INT32(0, rc);
    TEST_ASSERT_EQUAL_UINT(1, sim_blank_lines(SIM_NRF));
    TEST_ASSERT_EQUAL_STRING("rf off", sim_sent_line(SIM_NRF, 0));

    /* The Dot reset half way through `scan all`: the C5 still holds "sca". The next command is
     * heard as itself; the leftover ends alone, and its #DONE is nobody's. */
    sim_sent_clear(SIM_C5);
    sim_half_line(SIM_C5, "sca");
    rc = -1;
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_command(FCK_C5, "wp", NULL, NULL, NULL, 0U, &rc));
    TEST_ASSERT_EQUAL_INT32(0, rc);
    TEST_ASSERT_EQUAL_STRING("sca", sim_sent_line(SIM_C5, 0));
    TEST_ASSERT_EQUAL_STRING("wp", sim_sent_line(SIM_C5, 1));
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0, sim_sent(SIM_C5, "scawp"), "the two were glued");
}

TEST_CASE("a typed call to a chip whose firmware does not speak the SDK's words is refused, and not as a chip that is not ready", "[c5][agent]")
{
    sim_begin_fck(SIM_C5_BANNER_OWN, NULL);
    fck_ble_device_t d[2];
    size_t           n = 77;
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_ble_scan(1U, d, 2U, &n));
    TEST_ASSERT_NOT_NULL(strstr(fck_aos_error(), "does not speak the SDK's words"));
    /* The chip said what it runs: nothing to wait for, so FCK_WHY_CHIP_NOT_READY would send an app
     * waiting. It is the class none of the others names. */
    TEST_ASSERT_EQUAL(FCK_AOS_WHY_REFUSED, fck_aos_error_why());
    TEST_ASSERT_EQUAL_UINT(0, sent_other_than_info(SIM_C5));
}

TEST_CASE("a firmware of someone's own that prints its caps= is compatible: the typed calls reach it by its words", "[c5][firmware]")
{
    sim_begin_fck("#C5INFO fw=my_firmware ver=0.3 caps=scan,ble", NULL);
    fck_firmware_t a;
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_firmware(FCK_C5, &a));
    TEST_ASSERT_TRUE(a.known && a.compatible);
    TEST_ASSERT_FALSE_MESSAGE(a.official, "its name is its own");
    TEST_ASSERT_EQUAL_STRING("scan,ble", a.caps);

    sim_script(SIM_C5, "scan", "#SCANDONE n=0 ms=1\r\n#DONE scan rc=0\r\n", 5U);
    sim_script(SIM_C5, "ble scan", "#BLEDONE n=0 ms=1\r\n#DONE ble rc=0\r\n", 5U);
    fck_wifi_network_t ap[2];
    fck_wifi_channel_t ch[FCK_WIFI_CHANNEL_MAX];
    fck_ble_device_t   d[2];
    size_t             n  = 77;
    int32_t            rc = -1;
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_wifi_scan(FCK_BAND_2G, ap, 2U, &n));
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_ble_scan(1U, d, 2U, &n));
    TEST_ASSERT_EQUAL_UINT(1, sim_sent(SIM_C5, "scan 2g"));
    /* A word its caps= lacks is not sent. */
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_wifi_channels(FCK_BAND_ALL, 250U, ch, FCK_WIFI_CHANNEL_MAX, &n));
    /* fck_command() still hands it the line as written: its words are its programmer's. */
    sim_script(SIM_C5, "blink", "#DONE blink rc=0\r\n", 5U);
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_command(FCK_C5, "blink 3", "blink", NULL, NULL, 500U, &rc));
    TEST_ASSERT_EQUAL_INT32(0, rc);
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_command(FCK_C5, "blink 3", NULL, NULL, NULL, 500U, &rc));
}
