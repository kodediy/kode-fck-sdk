/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_facade.c
 * @brief The facade (fck.c, fck_wifi.c, fck_radio.c, fck_gnss.c) over an owner
 *        that writes down what it is asked: that each call maps to the right
 *        request, that what must be refused is refused before the owner is
 *        bothered, and that what the owner answers comes back in an app's
 *        terms. The real owner is tests/host_owner's.
 */
#include <string.h>

#include "unity.h"

#include "fck.h"
#include "fck_core.h"
#include "fck_aos.h"
#include "fck_drv_c5.h"

#include "fake_fck_aos.h"

#define C5_20  "fw=c5_fck_agent ver=2.0 chip=esp32c5 rev=1.2 mac=aa:bb:cc:dd:ee:01 reg=EU lim=ok"
#define C5_24  "fw=fck-c5 ver=2.4 chip=esp32c5 rev=1.2 mac=aa:bb:cc:dd:ee:01 reg=EU lim=ok"
/* The public line: what a firmware does is in its caps=, and its version says nothing of it. */
#define C5_020 "fw=fck-c5 ver=0.2 chip=esp32c5 rev=1.2 mac=aa:bb:cc:dd:ee:01 reg=EU lim=ok caps=scan,chan,ble,bleadv,frames,gnss,wp,gtx,health"
#define C5_010 "fw=fck-c5 ver=0.1 chip=esp32c5 rev=1.2 mac=aa:bb:cc:dd:ee:01 reg=EU lim=ok caps=scan,chan,ble,gnss,wp,gtx,health"
#define C5_23  "fw=fck-c5 ver=2.3 chip=esp32c5 rev=1.2 mac=aa:bb:cc:dd:ee:01 reg=EU lim=ok"
#define C5_22  "fw=fck-c5 ver=2.2 chip=esp32c5 rev=1.2 mac=aa:bb:cc:dd:ee:01 reg=EU lim=ok"
#define C5_15  "fw=c5_fck_agent ver=1.5 chip=esp32c5 rev=1.2 mac=aa:bb:cc:dd:ee:02"
#define NRF_20 "fw=nrf_fck_agent ver=2.0 devid=0123456789ABCDEF reset=0x0 uptime_ms=1"

/* ---- The module ------------------------------------------------------------------ */

TEST_CASE("init, present and wait go to the owner and its answers come back", "[facade]")
{
    fake_fck_aos_reset();
    g_aos.start_rc = KODE_NO_RESOURCES;
    TEST_ASSERT_EQUAL(KODE_NO_RESOURCES, fck_core_begin());
    TEST_ASSERT_TRUE(fake_fck_aos_called(CALL_START));

    g_aos.present = false;
    TEST_ASSERT_FALSE(fck_is_present());
    g_aos.present = true;
    TEST_ASSERT_TRUE(fck_is_present());

    g_aos.wait_rc = KODE_TIMEOUT;
    TEST_ASSERT_EQUAL(KODE_TIMEOUT, fck_core_wait_present(1234U));
    TEST_ASSERT_EQUAL_UINT32(1234U, g_aos.last_ms);
}

TEST_CASE("end goes to the owner and its answer comes back", "[facade]")
{
    fake_fck_aos_reset();
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_end());
    TEST_ASSERT_TRUE(fake_fck_aos_called(CALL_STOP));
    g_aos.stop_rc = KODE_DENIED;
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_end());
    g_aos.stop_rc = KODE_NO_RESOURCES;
    TEST_ASSERT_EQUAL(KODE_NO_RESOURCES, fck_core_end());
}

TEST_CASE("cancel goes to the owner and its answer comes back", "[facade]")
{
    fake_fck_aos_reset();
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_cancel());
    TEST_ASSERT_TRUE(fake_fck_aos_called(CALL_CANCEL));
    g_aos.cancel_rc = KODE_DENIED;
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_cancel());
}

TEST_CASE("a spectrum of 5 sweeps of 0.9 s is five commands of one, and a cancel between two ends it there", "[facade]")
{
    fake_fck_aos_reset();
    fake_fck_aos_firmware(FCK_AOS_NRF, NRF_20);
    int8_t dbm[16];
    size_t n = 0U;
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_radio_spectrum(2402U, 2410U, 100000U, 5U, dbm, sizeof(dbm), &n));
    TEST_ASSERT_EQUAL_UINT(5U, g_aos.commands);
    TEST_ASSERT_EQUAL_STRING("rf spec 2 10 100000 1\n", g_aos.cmd_line);

    fake_fck_aos_reset();
    fake_fck_aos_firmware(FCK_AOS_NRF, NRF_20);
    g_aos.cancel_after = 2U;
    n                  = 77U;
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_radio_spectrum(2402U, 2410U, 100000U, 5U, dbm, sizeof(dbm), &n));
    TEST_ASSERT_EQUAL_UINT(2U, g_aos.commands);
    TEST_ASSERT_EQUAL_MESSAGE(77U, n, "a cancelled call wrote its count");
    TEST_ASSERT_EQUAL_MESSAGE(CALL_RELEASE, g_aos.calls[g_aos.n_calls - 1U], "the nRF was not let go");
}

TEST_CASE("status copies every field the owner reports, the chips in the facade's words", "[facade]")
{
    fake_fck_aos_reset();
    g_aos.st.generation     = 7U;
    g_aos.st.cc1101_ok      = true;
    g_aos.st.cc1101_part    = 0x00;
    g_aos.st.cc1101_version = 0x14;
    g_aos.st.gnss_running   = true;
    g_aos.st.chip_on[FCK_AOS_C5]  = true;
    g_aos.st.chip_on[FCK_AOS_NRF] = false;
    fck_status_t s;
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_status(&s));
    TEST_ASSERT_TRUE(s.present);
    TEST_ASSERT_EQUAL_UINT32(7U, s.generation);
    TEST_ASSERT_TRUE(s.c5_on);
    TEST_ASSERT_FALSE(s.nrf_on);
    TEST_ASSERT_TRUE(s.cc1101_ok);
    TEST_ASSERT_EQUAL_UINT8(0x14, s.cc1101_version);
    TEST_ASSERT_TRUE(s.gnss_running);
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_status(NULL));   /* a null out is refused, nothing touched */
    TEST_ASSERT_EQUAL_STRING("out is NULL", fck_error());
}

TEST_CASE("header_position passes the position down and the answer back", "[facade]")
{
    fake_fck_aos_reset();
    g_aos.pos_ret = 9U;
    TEST_ASSERT_EQUAL_UINT8(9U, fck_header_position(9U));
    g_aos.pos_ret = 0U;
    TEST_ASSERT_EQUAL_UINT8(0U, fck_header_position(9U));
}

/* ---- The chips ------------------------------------------------------------------- */

TEST_CASE("power and reset name the chip the owner is asked about, and refuse a chip that is not", "[facade]")
{
    fake_fck_aos_reset();
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_power(FCK_NRF, false));
    TEST_ASSERT_EQUAL(FCK_AOS_NRF, g_aos.last_chip);
    TEST_ASSERT_FALSE(g_aos.last_on);
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_power(FCK_C5, true));
    TEST_ASSERT_EQUAL(FCK_AOS_C5, g_aos.last_chip);
    TEST_ASSERT_TRUE(g_aos.last_on);
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_reset(FCK_C5));
    TEST_ASSERT_EQUAL(FCK_AOS_C5, g_aos.last_chip);

    unsigned const before = g_aos.n_calls;
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_power((fck_chip_t)5, true));
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_reset((fck_chip_t)5));
    TEST_ASSERT_EQUAL_UINT(before, g_aos.n_calls);

    g_aos.power_rc = KODE_IO_ERROR;
    TEST_ASSERT_EQUAL(KODE_IO_ERROR, fck_core_power(FCK_C5, false));
}

TEST_CASE("firmware says what a chip announced, in the facade's terms", "[facade]")
{
    fake_fck_aos_reset();
    fake_fck_aos_firmware(FCK_AOS_C5, C5_20);
    fck_firmware_t a;
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_firmware(FCK_C5, &a));
    TEST_ASSERT_TRUE(a.known);
    TEST_ASSERT_TRUE(a.official);
    TEST_ASSERT_EQUAL_STRING("c5_fck_agent", a.fw);
    TEST_ASSERT_EQUAL_UINT8(2U, a.ver_major);
    TEST_ASSERT_EQUAL_UINT8(0U, a.ver_minor);
    TEST_ASSERT_TRUE(a.compatible);
    TEST_ASSERT_EQUAL_UINT8(6U, a.id_len);
    TEST_ASSERT_EQUAL_HEX8(0xAAU, a.id[0]);
    TEST_ASSERT_EQUAL_HEX8(0x01U, a.id[5]);

    fake_fck_aos_firmware(FCK_AOS_NRF, NRF_20);
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_firmware(FCK_NRF, &a));
    TEST_ASSERT_TRUE(a.official);
    TEST_ASSERT_EQUAL_STRING("nrf_fck_agent", a.fw);
    TEST_ASSERT_EQUAL_UINT8(8U, a.id_len);
    TEST_ASSERT_EQUAL_HEX8(0x01U, a.id[0]);
    TEST_ASSERT_EQUAL_HEX8(0xEFU, a.id[7]);
}

TEST_CASE("firmware of a chip that has not said is a timeout; a bad chip or a null out is refused, and says why", "[facade]")
{
    fake_fck_aos_reset();
    g_aos.firmware_rc[FCK_AOS_C5] = KODE_TIMEOUT;
    fck_firmware_t a;
    TEST_ASSERT_EQUAL_MESSAGE(KODE_TIMEOUT, fck_core_firmware(FCK_C5, &a), "a timeout is a timeout");
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_firmware((fck_chip_t)3, &a));
    TEST_ASSERT_EQUAL_STRING("chip is not FCK_C5 or FCK_NRF, or out is NULL", fck_error());
    g_aos.why = NULL;
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_firmware(FCK_NRF, NULL));
    TEST_ASSERT_EQUAL_STRING("chip is not FCK_C5 or FCK_NRF, or out is NULL", fck_error());
    g_aos.firmware_rc[FCK_AOS_C5] = KODE_IO_ERROR;
    TEST_ASSERT_EQUAL(KODE_IO_ERROR, fck_core_firmware(FCK_C5, &a));
}

/* ---- A command is gated by what the chip runs ----------------------------------------- */

TEST_CASE("a command to a chip that has not said what it runs is not sent", "[facade]")
{
    fake_fck_aos_reset();
    g_aos.firmware_rc[FCK_AOS_C5] = KODE_TIMEOUT;
    TEST_ASSERT_EQUAL(KODE_TIMEOUT, fck_core_command(FCK_C5, "info", "info", NULL, NULL, 1000U, NULL));
    TEST_ASSERT_EQUAL_UINT(0U, g_aos.commands);
}

TEST_CASE("a 2.0 firmware gets its own words, tied to the banner it was judged on", "[facade]")
{
    fake_fck_aos_reset();
    fake_fck_aos_firmware(FCK_AOS_C5, C5_20);
    int32_t rc = -1;
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_command(FCK_C5, "info", "info", NULL, NULL, 0U, &rc));
    TEST_ASSERT_EQUAL_UINT(1U, g_aos.commands);
    TEST_ASSERT_EQUAL(FCK_AOS_C5, g_aos.cmd_chip);
    TEST_ASSERT_EQUAL_UINT32(g_aos.firmware[FCK_AOS_C5].seq, g_aos.cmd_seq);
    TEST_ASSERT_EQUAL_STRING("info", g_aos.cmd_verb);
    TEST_ASSERT_TRUE_MESSAGE(g_aos.cmd_ms > 0U, "a wait of 0 is the vocabulary's own");
}

TEST_CASE("a word outside the 2.0 firmware's vocabulary is refused before the owner", "[facade]")
{
    fake_fck_aos_reset();
    fake_fck_aos_firmware(FCK_AOS_C5, C5_20);
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_command(FCK_C5, "deauth aa:bb:cc:dd:ee:ff", NULL, NULL, NULL, 1000U, NULL));
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_command(FCK_C5, "info", "scan", NULL, NULL, 1000U, NULL));
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_command(FCK_C5, NULL, "info", NULL, NULL, 1000U, NULL));
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_command((fck_chip_t)4, "info", "info", NULL, NULL, 1000U, NULL));
    TEST_ASSERT_EQUAL_UINT(0U, g_aos.commands);
}

TEST_CASE("another firmware's line goes as written, but only with the verb it ends on", "[facade]")
{
    fake_fck_aos_reset();
    fake_fck_aos_firmware(FCK_AOS_C5, "fw=my_own_app ver=0.1 chip=esp32c5");
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_command(FCK_C5, "go fast", NULL, NULL, NULL, 500U, NULL));
    TEST_ASSERT_EQUAL_UINT(0U, g_aos.commands);
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_command(FCK_C5, "go fast", "go", NULL, NULL, 500U, NULL));
    TEST_ASSERT_EQUAL_STRING("go fast", g_aos.cmd_line);
    TEST_ASSERT_EQUAL_STRING("go", g_aos.cmd_verb);
    TEST_ASSERT_EQUAL_UINT32(500U, g_aos.cmd_ms);
}

TEST_CASE("an nRF firmware's radio words are kept inside the limits whatever the app writes", "[facade]")
{
    fake_fck_aos_reset();
    fake_fck_aos_firmware(FCK_AOS_NRF, NRF_20);
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_command(FCK_NRF, "rf tx 99 0 0", NULL, NULL, NULL, 1000U, NULL));
    TEST_ASSERT_EQUAL_UINT(0U, g_aos.commands);
}

/* ---- The C5's Wi-Fi ---------------------------------------------------------------- */

TEST_CASE("a scan sends the firmware's scan, and the list comes back strongest first", "[facade]")
{
    fake_fck_aos_reset();
    fake_fck_aos_firmware(FCK_AOS_C5, C5_20);
    g_aos.replies[0] = (fake_reply_t){ "AP", "ssid=Weak bssid=aa:bb:cc:dd:ee:11 ch=1 band=2g rssi=-80 auth=wpa2_psk" };
    g_aos.replies[1] = (fake_reply_t){ "AP", "ssid=Strong bssid=aa:bb:cc:dd:ee:12 ch=36 band=5g rssi=-40 auth=open" };
    g_aos.replies[2] = (fake_reply_t){ "AP", "ssid=Mid bssid=aa:bb:cc:dd:ee:13 ch=6 band=2g rssi=-60 auth=wpa3_psk" };
    g_aos.command_out = 0;

    fck_wifi_network_t list[8];
    size_t             n = 0U;
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_wifi_scan(FCK_BAND_ALL, list, 8U, &n));
    TEST_ASSERT_EQUAL_UINT(1U, g_aos.commands);
    TEST_ASSERT_EQUAL_STRING("scan", g_aos.cmd_verb);
    TEST_ASSERT_EQUAL_UINT32(g_aos.firmware[FCK_AOS_C5].seq, g_aos.cmd_seq);
    TEST_ASSERT_EQUAL_UINT(3U, n);
    TEST_ASSERT_EQUAL_STRING("Strong", (const char *)list[0].ssid);
    TEST_ASSERT_EQUAL_STRING("Mid", (const char *)list[1].ssid);
    TEST_ASSERT_EQUAL_STRING("Weak", (const char *)list[2].ssid);
    TEST_ASSERT_EQUAL_INT(FCK_BAND_5G, list[0].band);
    TEST_ASSERT_EQUAL_UINT8(36U, list[0].channel);
    TEST_ASSERT_EQUAL_INT(FCK_WIFI_SECURITY_OPEN, list[0].auth);
}

TEST_CASE("a scan keeps the strongest when the caller's list is shorter than the firmware's", "[facade]")
{
    fake_fck_aos_reset();
    fake_fck_aos_firmware(FCK_AOS_C5, C5_20);
    g_aos.replies[0] = (fake_reply_t){ "AP", "ssid=A bssid=aa:bb:cc:dd:ee:21 ch=1 band=2g rssi=-70 auth=open" };
    g_aos.replies[1] = (fake_reply_t){ "AP", "ssid=B bssid=aa:bb:cc:dd:ee:22 ch=6 band=2g rssi=-50 auth=open" };
    g_aos.replies[2] = (fake_reply_t){ "AP", "ssid=C bssid=aa:bb:cc:dd:ee:23 ch=11 band=2g rssi=-60 auth=open" };
    fck_wifi_network_t list[2];
    size_t             n = 0U;
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_wifi_scan(FCK_BAND_2G, list, 2U, &n));
    TEST_ASSERT_EQUAL_UINT(2U, n);
    TEST_ASSERT_EQUAL_STRING("B", (const char *)list[0].ssid);
    TEST_ASSERT_EQUAL_STRING("C", (const char *)list[1].ssid);
}

TEST_CASE("a scan is refused for a firmware without the radio limits, and for bad arguments", "[facade]")
{
    fake_fck_aos_reset();
    fck_wifi_network_t list[4];
    size_t             n = 0U;

    fake_fck_aos_firmware(FCK_AOS_C5, C5_15);
    TEST_ASSERT_EQUAL_MESSAGE(KODE_DENIED, fck_core_wifi_scan(FCK_BAND_ALL, list, 4U, &n),
                              "a 1.x firmware would probe on both bands with no limit");
    fake_fck_aos_firmware(FCK_AOS_C5, C5_20);
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_wifi_scan(FCK_BAND_ALL, NULL, 4U, &n));
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_wifi_scan(FCK_BAND_ALL, list, 0U, &n));
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_wifi_scan((fck_band_t)7, list, 4U, &n));
    TEST_ASSERT_EQUAL_UINT(0U, g_aos.commands);
}

TEST_CASE("a scan's rc from the firmware comes back as a status", "[facade]")
{
    fake_fck_aos_reset();
    fake_fck_aos_firmware(FCK_AOS_C5, C5_20);
    fck_wifi_network_t list[4];
    size_t             n = 0U;
    g_aos.command_out = 1;
    TEST_ASSERT_EQUAL(KODE_IO_ERROR, fck_core_wifi_scan(FCK_BAND_2G, list, 4U, &n));
    g_aos.command_out = 0;
    g_aos.command_rc  = KODE_IO_ERROR;
    TEST_ASSERT_EQUAL_MESSAGE(KODE_IO_ERROR, fck_core_wifi_scan(FCK_BAND_2G, list, 4U, &n), "the module left on the way");
}

/* The records of ESPEC 1: a beacon "Kode" on 6, a deauthentication on 11, a data frame on 36. */
#define F_BEACON "gAAGzLQA////////qrvM3e4BqrvM3e4BAQIDBAUGBwhkABEEAARLb2Rl"
#define F_DEAUTH "wAALwx4AAhEiM0RVqrvM3e4BqrvM3e4BBwA="
#define F_DATA   "iAEk0AQGqrvM3e4BAhEiM0RVCgsMDQ4P"
#define F_PROBE  "QAAGujwA////////AhEiM0RV////////AAA="
#define FRAMES_END(sent) "ch=6 ms=251 heard=412 mgmt=300 data=100 ctrl=12 beacon=40 probe_req=22 probe_resp=18 deauth=3 " \
                         "disassoc=1 sent=" sent " dropped=2"

TEST_CASE("frames sends the firmware's `frames`, keeps the frames in the order heard and gives the counts", "[facade][frames]")
{
    fake_fck_aos_reset();
    fake_fck_aos_firmware(FCK_AOS_C5, C5_24);
    g_aos.replies[0] = (fake_reply_t){ "F", F_BEACON };
    g_aos.replies[1] = (fake_reply_t){ "F", F_DEAUTH };
    g_aos.replies[2] = (fake_reply_t){ "F", F_DATA };
    g_aos.replies[3] = (fake_reply_t){ "FRAMES", FRAMES_END("3") };
    g_aos.command_out = 0;

    fck_wifi_frame_t             list[8];
    fck_wifi_frames_statistics_t stats;
    size_t                       n = 0U;
    memset(&stats, 0x5A, sizeof(stats));
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_wifi_frames(6U, 250U, list, 8U, &n, &stats));
    TEST_ASSERT_EQUAL_UINT(1U, g_aos.commands);
    TEST_ASSERT_EQUAL_STRING("frames ch=6 ms=250\n", g_aos.cmd_line);
    TEST_ASSERT_EQUAL_STRING("frames", g_aos.cmd_verb);
    TEST_ASSERT_EQUAL_UINT32(g_aos.firmware[FCK_AOS_C5].seq, g_aos.cmd_seq);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(250U + FCK_DRV_C5_FRAMES_SLACK_MS, g_aos.cmd_ms, "its time and the UART's slack");

    TEST_ASSERT_EQUAL_UINT(3U, n);
    TEST_ASSERT_EQUAL_HEX8(0x80, list[0].fc[0]);
    TEST_ASSERT_EQUAL_UINT8(6U, list[0].channel);
    TEST_ASSERT_EQUAL_INT8(-52, list[0].rssi);
    TEST_ASSERT_EQUAL_UINT16(180U, list[0].len);
    TEST_ASSERT_EQUAL_HEX8(0xff, list[0].addr1[0]);
    TEST_ASSERT_EQUAL_HEX8(0x01, list[0].addr2[5]);
    TEST_ASSERT_EQUAL_UINT8(18U, list[0].head_len);
    TEST_ASSERT_EQUAL_HEX8('K', list[0].head[14]);
    TEST_ASSERT_EQUAL_HEX8(0, list[0].head[18]);
    TEST_ASSERT_EQUAL_HEX8(0xc0, list[1].fc[0]);
    TEST_ASSERT_EQUAL_UINT8(2U, list[1].head_len);
    TEST_ASSERT_EQUAL_HEX8(0x07, list[1].head[0]);
    TEST_ASSERT_EQUAL_UINT8(36U, list[2].channel);
    TEST_ASSERT_EQUAL_UINT16(1540U, list[2].len);
    TEST_ASSERT_EQUAL_UINT8(0U, list[2].head_len);

    TEST_ASSERT_EQUAL_UINT8(6U, stats.channel);
    TEST_ASSERT_EQUAL_UINT32(251U, stats.ms);
    TEST_ASSERT_EQUAL_UINT32(412U, stats.heard);
    TEST_ASSERT_EQUAL_UINT32(300U, stats.mgmt);
    TEST_ASSERT_EQUAL_UINT32(100U, stats.data);
    TEST_ASSERT_EQUAL_UINT32(12U, stats.ctrl);
    TEST_ASSERT_EQUAL_UINT32(40U, stats.beacon);
    TEST_ASSERT_EQUAL_UINT32(22U, stats.probe_request);
    TEST_ASSERT_EQUAL_UINT32(18U, stats.probe_response);
    TEST_ASSERT_EQUAL_UINT32(3U, stats.deauth);
    TEST_ASSERT_EQUAL_UINT32(1U, stats.disassoc);
    TEST_ASSERT_EQUAL_UINT32(3U, stats.sent);
    TEST_ASSERT_EQUAL_UINT32(2U, stats.dropped);

    /* n and stats are the caller's to ask for. */
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_wifi_frames(36U, 20U, list, 8U, NULL, NULL));
    TEST_ASSERT_EQUAL_STRING("frames ch=36 ms=20\n", g_aos.cmd_line);
    TEST_ASSERT_EQUAL_HEX8(0x80, list[0].fc[0]);
}

TEST_CASE("frames keeps the first cap frames and the counts say how many there were", "[facade][frames]")
{
    fake_fck_aos_reset();
    fake_fck_aos_firmware(FCK_AOS_C5, C5_24);
    g_aos.replies[0] = (fake_reply_t){ "F", F_PROBE };
    g_aos.replies[1] = (fake_reply_t){ "F", F_DEAUTH };
    g_aos.replies[2] = (fake_reply_t){ "F", F_DATA };
    g_aos.replies[3] = (fake_reply_t){ "F", F_BEACON };
    g_aos.replies[4] = (fake_reply_t){ "FRAMES", FRAMES_END("4") };
    fck_wifi_frame_t             list[3];
    fck_wifi_frames_statistics_t stats;
    size_t                       n = 0U;
    memset(list, 0x5A, sizeof(list));
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_wifi_frames(6U, 100U, list, 2U, &n, &stats));
    TEST_ASSERT_EQUAL_UINT(2U, n);
    TEST_ASSERT_EQUAL_HEX8(0x40, list[0].fc[0]);
    TEST_ASSERT_EQUAL_HEX8(0xc0, list[1].fc[0]);
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x5A, list[2].fc[0], "nothing past cap was written");
    TEST_ASSERT_EQUAL_UINT32(4U, stats.sent);
}

TEST_CASE("frames with nothing told is still a listen: no frame, the counts", "[facade][frames]")
{
    fake_fck_aos_reset();
    fake_fck_aos_firmware(FCK_AOS_C5, C5_24);
    g_aos.replies[0] = (fake_reply_t){ "FRAMES", "ch=6 ms=250 heard=0 mgmt=0 data=0 ctrl=0 beacon=0 probe_req=0 probe_resp=0 deauth=0 disassoc=0 sent=0 dropped=0" };
    fck_wifi_frame_t             list[1];
    fck_wifi_frames_statistics_t stats;
    size_t                       n = 9U;
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_wifi_frames(6U, 250U, list, 1U, &n, &stats));
    TEST_ASSERT_EQUAL_UINT(0U, n);
    TEST_ASSERT_EQUAL_UINT32(0U, stats.heard);
}

TEST_CASE("frames is refused for bad arguments and for a firmware without it (no \"frames\" in caps=, or before 2.4), and asks nothing", "[facade][frames]")
{
    fake_fck_aos_reset();
    fck_wifi_frame_t list[4];
    size_t           n = 77U;

    fake_fck_aos_firmware(FCK_AOS_C5, C5_24);
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_wifi_frames(6U, 250U, NULL, 4U, &n, NULL));
    TEST_ASSERT_TRUE_MESSAGE(strlen(fck_error()) > 10U, "a refusal with no reason");
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_wifi_frames(6U, 250U, list, 0U, &n, NULL));
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_wifi_frames(0U, 250U, list, 4U, &n, NULL));
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_wifi_frames(178U, 250U, list, 4U, &n, NULL));
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_wifi_frames(6U, 19U, list, 4U, &n, NULL));
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_wifi_frames(6U, 5001U, list, 4U, &n, NULL));
    TEST_ASSERT_EQUAL_UINT(0U, g_aos.commands);

    /* 2.3 is another line's and 2.2 the BLE advertisement's: no `frames` yet. It listens, so the radio
     * limits do not matter. */
    fake_fck_aos_firmware(FCK_AOS_C5, C5_23);
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_wifi_frames(6U, 250U, list, 4U, &n, NULL));
    TEST_ASSERT_TRUE_MESSAGE(strstr(fck_error(), "no frame listener") != NULL, fck_error());
    fake_fck_aos_firmware(FCK_AOS_C5, C5_22);
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_wifi_frames(6U, 250U, list, 4U, &n, NULL));
    fake_fck_aos_firmware(FCK_AOS_C5, C5_20);
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_wifi_frames(6U, 250U, list, 4U, &n, NULL));
    fake_fck_aos_firmware(FCK_AOS_C5, C5_15);
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_wifi_frames(6U, 250U, list, 4U, &n, NULL));
    /* With caps= it is the word that counts: 0.1.0 has no "frames", whatever its version says. */
    fake_fck_aos_firmware(FCK_AOS_C5, C5_010);
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_wifi_frames(6U, 250U, list, 4U, &n, NULL));
    TEST_ASSERT_TRUE_MESSAGE(strstr(fck_error(), "no frame listener") != NULL, fck_error());
    TEST_ASSERT_EQUAL_UINT(0U, g_aos.commands);
    TEST_ASSERT_EQUAL_UINT(77U, n);

    /* And 0.2.0 has it: the call is asked. */
    fake_fck_aos_firmware(FCK_AOS_C5, C5_020);
    (void)fck_core_wifi_frames(6U, 250U, list, 4U, &n, NULL);
    TEST_ASSERT_EQUAL_UINT(1U, g_aos.commands);
}

TEST_CASE("frames' rc from the firmware comes back as a status, and a call that did not end well writes nothing", "[facade][frames]")
{
    fake_fck_aos_reset();
    fake_fck_aos_firmware(FCK_AOS_C5, C5_24);
    fck_wifi_frame_t             list[4];
    fck_wifi_frames_statistics_t stats;
    size_t                       n = 77U;
    memset(list, 0x5A, sizeof(list));
    memset(&stats, 0x5A, sizeof(stats));
    g_aos.replies[0] = (fake_reply_t){ "F", F_BEACON };
    g_aos.replies[1] = (fake_reply_t){ "FRAMES", FRAMES_END("1") };

    g_aos.command_out = 1;    /* the driver refused the channel */
    TEST_ASSERT_EQUAL(KODE_IO_ERROR, fck_core_wifi_frames(14U, 250U, list, 4U, &n, &stats));
    TEST_ASSERT_TRUE_MESSAGE(strstr(fck_error(), "rc 1") != NULL, fck_error());
    g_aos.command_out = 2;    /* it does not know the command, or took its arguments for bad */
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_wifi_frames(6U, 250U, list, 4U, &n, &stats));
    TEST_ASSERT_TRUE_MESSAGE(strstr(fck_error(), "rc 2") != NULL, fck_error());
    g_aos.command_out = 7;
    TEST_ASSERT_EQUAL(KODE_IO_ERROR, fck_core_wifi_frames(6U, 250U, list, 4U, &n, &stats));
    TEST_ASSERT_TRUE_MESSAGE(strstr(fck_error(), "rc this SDK does not know") != NULL, fck_error());
    g_aos.command_out = 0;
    g_aos.command_rc  = KODE_IO_ERROR;
    TEST_ASSERT_EQUAL_MESSAGE(KODE_IO_ERROR, fck_core_wifi_frames(6U, 250U, list, 4U, &n, &stats), "the module left on the way");
    g_aos.command_rc = KODE_TIMEOUT;
    TEST_ASSERT_EQUAL(KODE_TIMEOUT, fck_core_wifi_frames(6U, 250U, list, 4U, &n, &stats));
    g_aos.command_rc = KODE_OK;
    TEST_ASSERT_EQUAL_UINT(77U, n);
    TEST_ASSERT_EQUAL_HEX8(0x5A, list[0].fc[0]);
    TEST_ASSERT_EQUAL_UINT8(0x5A, stats.channel);

    /* The firmware ends well with no summary: not a listen that can be trusted. */
    g_aos.replies[1] = (fake_reply_t){ NULL, NULL };
    TEST_ASSERT_EQUAL(KODE_IO_ERROR, fck_core_wifi_frames(6U, 250U, list, 4U, &n, &stats));
    TEST_ASSERT_EQUAL_UINT(77U, n);
    TEST_ASSERT_EQUAL_HEX8(0x5A, list[0].fc[0]);
}

TEST_CASE("frames skips a #F that is not a whole record and goes on", "[facade][frames]")
{
    fake_fck_aos_reset();
    fake_fck_aos_firmware(FCK_AOS_C5, C5_24);
    g_aos.replies[0] = (fake_reply_t){ "F", "not base64!" };
    g_aos.replies[1] = (fake_reply_t){ "F", "AAAA" };          /* 3 bytes: too short */
    g_aos.replies[2] = (fake_reply_t){ "F", F_DATA };
    g_aos.replies[3] = (fake_reply_t){ "FRAMES", FRAMES_END("3") };
    fck_wifi_frame_t list[4];
    size_t           n = 0U;
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_wifi_frames(36U, 250U, list, 4U, &n, NULL));
    TEST_ASSERT_EQUAL_UINT(1U, n);
    TEST_ASSERT_EQUAL_UINT8(36U, list[0].channel);
}

/* ---- The nRF's radio ---------------------------------------------------------------- */

TEST_CASE("a spectrum holds the nRF, speaks only the firmware's words, and lets it go", "[facade]")
{
    fake_fck_aos_reset();
    fake_fck_aos_firmware(FCK_AOS_NRF, NRF_20);
    int8_t dbm[16];
    size_t n = 0U;
    (void)fck_core_radio_spectrum(2402U, 2410U, 100U, 1U, dbm, sizeof(dbm), &n);
    TEST_ASSERT_EQUAL(CALL_HOLD, g_aos.calls[0]);
    TEST_ASSERT_EQUAL(CALL_COMMAND, g_aos.calls[g_aos.n_calls - 2U]);
    TEST_ASSERT_EQUAL(CALL_RELEASE, g_aos.calls[g_aos.n_calls - 1U]);
    TEST_ASSERT_EQUAL(FCK_AOS_NRF, g_aos.cmd_chip);
    TEST_ASSERT_EQUAL_STRING("rf spec 2 10 100 1\n", g_aos.cmd_line);
}

TEST_CASE("a spectrum of a range that does not fit the caller's list asks nothing", "[facade]")
{
    fake_fck_aos_reset();
    fake_fck_aos_firmware(FCK_AOS_NRF, NRF_20);
    int8_t dbm[4];
    size_t n = 0U;
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_radio_spectrum(2402U, 2410U, 100U, 1U, dbm, sizeof(dbm), &n));
    TEST_ASSERT_TRUE_MESSAGE(strlen(fck_error()) > 10U, "a refusal with no reason");
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_radio_spectrum(2410U, 2402U, 100U, 1U, dbm, sizeof(dbm), &n));
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_radio_spectrum(2402U, 2403U, 100U, 1U, NULL, 4U, &n));
    TEST_ASSERT_EQUAL_UINT(0U, g_aos.n_calls);
}

TEST_CASE("a spectrum on an nRF that does not run the firmware lets the hold go", "[facade]")
{
    fake_fck_aos_reset();
    fake_fck_aos_firmware(FCK_AOS_NRF, "fw=their_app ver=1.0");
    int8_t dbm[16];
    size_t n = 0U;
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_radio_spectrum(2402U, 2410U, 100U, 1U, dbm, sizeof(dbm), &n));
    TEST_ASSERT_EQUAL_UINT(0U, g_aos.commands);
    TEST_ASSERT_EQUAL(CALL_RELEASE, g_aos.calls[g_aos.n_calls - 1U]);
}

TEST_CASE("an nRF held by another task is not waited for beyond the owner's answer", "[facade]")
{
    fake_fck_aos_reset();
    fake_fck_aos_firmware(FCK_AOS_NRF, NRF_20);
    g_aos.hold_rc = KODE_DENIED;
    int8_t dbm[16];
    size_t n = 0U;
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_radio_spectrum(2402U, 2410U, 100U, 1U, dbm, sizeof(dbm), &n));
    TEST_ASSERT_EQUAL_UINT(0U, g_aos.commands);
}

/* ---- The GNSS receiver ------------------------------------------------------------- */

TEST_CASE("gnss start holds the C5, powers the receiver, turns the relay on and lets go", "[facade]")
{
    fake_fck_aos_reset();
    fake_fck_aos_firmware(FCK_AOS_C5, C5_20);
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_gnss_begin());
    TEST_ASSERT_EQUAL(CALL_HOLD, g_aos.calls[0]);
    TEST_ASSERT_TRUE(fake_fck_aos_called(CALL_GNSS_POWER));
    TEST_ASSERT_TRUE(g_aos.gnss_power_ons[0]);
    TEST_ASSERT_EQUAL_UINT(1U, g_aos.gnss_powers);
    TEST_ASSERT_EQUAL_STRING("gnss", g_aos.cmd_verb);
    TEST_ASSERT_EQUAL(CALL_RELEASE, g_aos.calls[g_aos.n_calls - 1U]);
}

TEST_CASE("gnss start puts the receiver back in reset when the relay will not turn on", "[facade]")
{
    fake_fck_aos_reset();
    fake_fck_aos_firmware(FCK_AOS_C5, C5_20);
    g_aos.command_rc = KODE_TIMEOUT;
    TEST_ASSERT_EQUAL(KODE_TIMEOUT, fck_core_gnss_begin());
    TEST_ASSERT_EQUAL_UINT(2U, g_aos.gnss_powers);
    TEST_ASSERT_TRUE(g_aos.gnss_power_ons[0]);
    TEST_ASSERT_FALSE_MESSAGE(g_aos.gnss_power_ons[1], "a receiver nobody hears costs energy for nothing");
    TEST_ASSERT_EQUAL(CALL_RELEASE, g_aos.calls[g_aos.n_calls - 1U]);
}

TEST_CASE("gnss start on a C5 that runs something else powers nothing", "[facade]")
{
    fake_fck_aos_reset();
    fake_fck_aos_firmware(FCK_AOS_C5, "fw=their_app ver=1.0 chip=esp32c5");
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_gnss_begin());
    TEST_ASSERT_EQUAL_UINT(0U, g_aos.gnss_powers);
    TEST_ASSERT_EQUAL(CALL_RELEASE, g_aos.calls[g_aos.n_calls - 1U]);
}

TEST_CASE("gnss read hands the fix over in the facade's words", "[facade]")
{
    fake_fck_aos_reset();
    g_aos.fix.valid   = true;
    g_aos.fix.quality = 1U;
    g_aos.fix.sats    = 9U;
    g_aos.fix.hdop    = 1.2f;
    g_aos.fix.has_time = true;
    g_aos.fix.hour    = 12U;
    g_aos.fix.min     = 34U;
    g_aos.fix_age_ms  = 730U;
    fck_gnss_fix_t f;
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_gnss_read(&f, 2500U));
    TEST_ASSERT_EQUAL_UINT32(2500U, g_aos.fix_ms);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(730U, f.age_ms, "the owner's count, as it gave it");
    TEST_ASSERT_TRUE(f.valid);
    TEST_ASSERT_EQUAL_UINT8(1U, f.quality);
    TEST_ASSERT_EQUAL_UINT8(9U, f.sats);
    TEST_ASSERT_TRUE(f.has_time);
    TEST_ASSERT_EQUAL_UINT8(12U, f.hour);
    TEST_ASSERT_EQUAL_UINT8(34U, f.min);
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_gnss_read(NULL, 100U));
}

TEST_CASE("gnss read with no wait and no fix yet is a refusal, not a timeout", "[facade]")
{
    fake_fck_aos_reset();
    g_aos.gnss_fix_rc = KODE_TIMEOUT;
    fck_gnss_fix_t f;
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_gnss_read(&f, 0U));
    TEST_ASSERT_EQUAL(KODE_TIMEOUT, fck_core_gnss_read(&f, 100U));
    g_aos.gnss_fix_rc = KODE_IO_ERROR;
    TEST_ASSERT_EQUAL(KODE_IO_ERROR, fck_core_gnss_read(&f, 100U));
}

TEST_CASE("gnss stop puts the receiver in reset first, then turns the relay off", "[facade]")
{
    fake_fck_aos_reset();
    fake_fck_aos_firmware(FCK_AOS_C5, C5_20);
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_gnss_end());
    TEST_ASSERT_EQUAL(CALL_GNSS_POWER, g_aos.calls[0]);
    TEST_ASSERT_FALSE(g_aos.gnss_power_ons[0]);
    TEST_ASSERT_EQUAL(CALL_FIRMWARE, g_aos.calls[1]);
    TEST_ASSERT_EQUAL(CALL_COMMAND, g_aos.calls[2]);
    TEST_ASSERT_EQUAL_STRING("gnss", g_aos.cmd_verb);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0U, g_aos.last_ms, "it asks the chip nothing and waits for nothing");
}

TEST_CASE("gnss stop with no module asks the C5 nothing", "[facade]")
{
    fake_fck_aos_reset();
    fake_fck_aos_firmware(FCK_AOS_C5, C5_20);
    g_aos.gnss_power_rc = KODE_DENIED;
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_gnss_end());
    TEST_ASSERT_EQUAL_UINT(0U, g_aos.commands);
}

TEST_CASE("gnss stop sends nothing to a C5 that has not said what it runs", "[facade]")
{
    fake_fck_aos_reset();
    g_aos.firmware_rc[FCK_AOS_C5] = KODE_TIMEOUT;
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_gnss_end());
    TEST_ASSERT_EQUAL_UINT(0U, g_aos.commands);
}
