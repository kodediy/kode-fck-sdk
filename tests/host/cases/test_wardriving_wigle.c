/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_wardriving_wigle.c
 * @brief examples/wardriving's rows: a network or a device and the fix that places it, as a line
 *        of a WiGLE CSV file (WigleWifi-1.6, https://api.wigle.net/csvFormat.html).
 *
 * The format is WiGLE's, so what is checked here is its page: the two lines a file starts with,
 * the fourteen columns in their order, the time in UTC to the second, a name quoted the way
 * RFC 4180 asks. Every name, address and position is made up; the position is the NMEA 0183
 * textbook's.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "unity.h"

#include "fck.h"
#include "wardriving_wigle.h"

/* 48 07.038 N, 011 31.000 E, on 29 September 2026 at 20:26:34 UTC, read 300 ms ago. */
static fck_gnss_fix_t fix_good(void)
{
    fck_gnss_fix_t f;
    memset(&f, 0, sizeof(f));
    f.quality  = 1U;
    f.sats     = 7U;
    f.valid    = true;
    f.lat      = 48.1173;
    f.lon      = 11.516667;
    f.alt_m    = 109.418;
    f.hdop     = 1.2;
    f.has_time = true;
    f.hour     = 20U;
    f.min      = 26U;
    f.sec      = 34U;
    f.has_date = true;
    f.day      = 29U;
    f.month    = 9U;
    f.year     = 2026U;
    f.age_ms   = 300U;
    return f;
}

static fck_wifi_network_t network(const char *ssid, uint8_t last, uint8_t channel, fck_band_t band, int8_t rssi,
                                  fck_wifi_security_t auth)
{
    fck_wifi_network_t n;
    memset(&n, 0, sizeof(n));
    strncpy(n.ssid, ssid, sizeof(n.ssid) - 1U);
    n.ssid_len = (uint8_t)strlen(n.ssid);
    const uint8_t bssid[6] = { 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, last };
    memcpy(n.bssid, bssid, sizeof(n.bssid));
    n.channel = channel;
    n.band    = band;
    n.rssi    = rssi;
    n.auth    = auth;
    return n;
}

static fck_ble_device_t device(const char *name, uint8_t last, int8_t rssi)
{
    fck_ble_device_t d;
    memset(&d, 0, sizeof(d));
    const uint8_t addr[6] = { 0x11, 0x22, 0x33, 0x44, 0x55, last };
    memcpy(d.addr, addr, sizeof(d.addr));
    strncpy(d.name, name, sizeof(d.name) - 1U);
    d.name_len = (uint8_t)strlen(d.name);
    d.rssi     = rssi;
    return d;
}

static unsigned commas_outside_quotes(const char *s)
{
    unsigned n      = 0U;
    bool     quoted = false;
    for (; *s != '\0'; s++) {
        if (*s == '"') {
            quoted = !quoted;
        } else if (*s == ',' && !quoted) {
            n++;
        } else {
            /* any other */
        }
    }
    return n;
}

TEST_CASE("a WiGLE file starts with its version and what wrote it, then the fourteen columns", "[wigle]")
{
    char   out[WARDRIVING_WIGLE_HEADER_MAX];
    size_t const n = wardriving_wigle_header(out, sizeof(out));
    TEST_ASSERT_EQUAL_UINT(strlen(out), n);
    const char *const second = strchr(out, '\n');
    TEST_ASSERT_NOT_NULL(second);
    TEST_ASSERT_EQUAL_STRING_LEN("WigleWifi-1.6,appRelease=", out, 25);
    /* The eleven fields WiGLE's page lists, the last three saying where: Earth. */
    TEST_ASSERT_NOT_NULL(strstr(out, ",model="));
    TEST_ASSERT_NOT_NULL(strstr(out, ",release="));
    TEST_ASSERT_NOT_NULL(strstr(out, ",device="));
    TEST_ASSERT_NOT_NULL(strstr(out, ",display="));
    TEST_ASSERT_NOT_NULL(strstr(out, ",board="));
    TEST_ASSERT_NOT_NULL(strstr(out, ",brand="));
    TEST_ASSERT_NOT_NULL(strstr(out, ",star=Sol,body=3,subBody=0\n"));
    TEST_ASSERT_EQUAL_STRING("MAC,SSID,AuthMode,FirstSeen,Channel,Frequency,RSSI,CurrentLatitude,CurrentLongitude,"
                             "AltitudeMeters,AccuracyMeters,RCOIs,MfgrId,Type\n",
                             second + 1);
    char first[WARDRIVING_WIGLE_HEADER_MAX];
    memcpy(first, out, (size_t)(second - out));
    first[second - out] = '\0';
    TEST_ASSERT_EQUAL_UINT(10, commas_outside_quotes(first));

    /* With no room for both lines, neither. */
    char small[40] = "untouched";
    TEST_ASSERT_EQUAL_UINT(0, wardriving_wigle_header(small, sizeof(small)));
    TEST_ASSERT_EQUAL_STRING("untouched", small);
    TEST_ASSERT_EQUAL_UINT(0, wardriving_wigle_header(NULL, 400U));
}

TEST_CASE("a network is a row: its address, name and security, when and where, on its frequency", "[wigle]")
{
    const fck_gnss_fix_t     fix = fix_good();
    const fck_wifi_network_t n   = network("Cafe Central", 0x01, 6U, FCK_BAND_2G, -50, FCK_WIFI_SECURITY_WPA2_PSK);
    char                     out[WARDRIVING_WIGLE_ROW_MAX];
    size_t const             len = wardriving_wigle_wifi_row(out, sizeof(out), &n, &fix);
    TEST_ASSERT_EQUAL_STRING("aa:bb:cc:dd:ee:01,\"Cafe Central\",[WPA2-PSK-CCMP][ESS],2026-09-29 20:26:34,6,2437,-50,"
                             "48.1173000,11.5166670,109,6.0,,,WIFI\n",
                             out);
    TEST_ASSERT_EQUAL_UINT(strlen(out), len);
    TEST_ASSERT_EQUAL_UINT(13, commas_outside_quotes(out));
}

TEST_CASE("a channel's frequency is its centre, in both bands", "[wigle]")
{
    const fck_gnss_fix_t fix = fix_good();
    char                 out[WARDRIVING_WIGLE_ROW_MAX];
    static const struct {
        uint8_t     channel;
        fck_band_t  band;
        const char *want;
    } t[] = {
        { 1U, FCK_BAND_2G, ",1,2412," },     { 13U, FCK_BAND_2G, ",13,2472," },   { 14U, FCK_BAND_2G, ",14,2484," },
        { 36U, FCK_BAND_5G, ",36,5180," },   { 100U, FCK_BAND_5G, ",100,5500," }, { 140U, FCK_BAND_5G, ",140,5700," },
    };
    for (size_t i = 0U; i < sizeof(t) / sizeof(t[0]); i++) {
        const fck_wifi_network_t n = network("x", 0x02, t[i].channel, t[i].band, -60, FCK_WIFI_SECURITY_OPEN);
        TEST_ASSERT_TRUE(wardriving_wigle_wifi_row(out, sizeof(out), &n, &fix) > 0U);
        TEST_ASSERT_NOT_NULL_MESSAGE(strstr(out, t[i].want), t[i].want);
    }
}

TEST_CASE("each security is said the way WiGLE's own app says it, and one unknown is just a network", "[wigle]")
{
    const fck_gnss_fix_t fix = fix_good();
    char                 out[WARDRIVING_WIGLE_ROW_MAX];
    static const struct {
        fck_wifi_security_t auth;
        const char         *want;
    } t[] = {
        { FCK_WIFI_SECURITY_OPEN, "\",[ESS],2026" },
        { FCK_WIFI_SECURITY_WEP, ",[WEP][ESS]," },
        { FCK_WIFI_SECURITY_WPA_PSK, ",[WPA-PSK-TKIP][ESS]," },
        { FCK_WIFI_SECURITY_WPA_WPA2_PSK, ",[WPA-PSK-TKIP][WPA2-PSK-CCMP][ESS]," },
        { FCK_WIFI_SECURITY_WPA2_ENTERPRISE, ",[WPA2-EAP-CCMP][ESS]," },
        { FCK_WIFI_SECURITY_WPA3_PSK, ",[WPA3-SAE-CCMP][ESS]," },
        { FCK_WIFI_SECURITY_WPA2_WPA3_PSK, ",[WPA2-PSK-CCMP][WPA3-SAE-CCMP][ESS]," },
        { FCK_WIFI_SECURITY_OWE, ",[OWE][ESS]," },
        { FCK_WIFI_SECURITY_UNKNOWN, "\",[ESS],2026" },
        { (fck_wifi_security_t)99, "\",[ESS],2026" },
    };
    for (size_t i = 0U; i < sizeof(t) / sizeof(t[0]); i++) {
        const fck_wifi_network_t n = network("x", 0x03, 6U, FCK_BAND_2G, -60, t[i].auth);
        TEST_ASSERT_TRUE(wardriving_wigle_wifi_row(out, sizeof(out), &n, &fix) > 0U);
        TEST_ASSERT_NOT_NULL_MESSAGE(strstr(out, t[i].want), t[i].want);
    }
}

TEST_CASE("a name cannot break the row: quotes are doubled, and what is no text is a question mark", "[wigle]")
{
    const fck_gnss_fix_t fix = fix_good();
    char                 out[WARDRIVING_WIGLE_ROW_MAX];

    fck_wifi_network_t n = network("He said \"hi\", ok", 0x04, 6U, FCK_BAND_2G, -60, FCK_WIFI_SECURITY_OPEN);
    TEST_ASSERT_TRUE(wardriving_wigle_wifi_row(out, sizeof(out), &n, &fix) > 0U);
    TEST_ASSERT_NOT_NULL(strstr(out, ",\"He said \"\"hi\"\", ok\",[ESS],"));
    TEST_ASSERT_EQUAL_UINT(13, commas_outside_quotes(out));

    /* A line break in a name would start a row of the name's own. */
    n = network("two\r\nlines\tand a tab", 0x05, 6U, FCK_BAND_2G, -60, FCK_WIFI_SECURITY_OPEN);
    TEST_ASSERT_TRUE(wardriving_wigle_wifi_row(out, sizeof(out), &n, &fix) > 0U);
    TEST_ASSERT_NOT_NULL(strstr(out, ",\"two??lines?and a tab\","));
    TEST_ASSERT_EQUAL_PTR(strchr(out, '\n'), out + strlen(out) - 1U);

    /* UTF-8 goes through as it is: the file is UTF-8. */
    n = network("Caf\xC3\xA9", 0x06, 6U, FCK_BAND_2G, -60, FCK_WIFI_SECURITY_OPEN);
    TEST_ASSERT_TRUE(wardriving_wigle_wifi_row(out, sizeof(out), &n, &fix) > 0U);
    TEST_ASSERT_NOT_NULL(strstr(out, ",\"Caf\xC3\xA9\","));

    /* A hidden network has no name, and its row says none. */
    n = network("", 0x07, 6U, FCK_BAND_2G, -60, FCK_WIFI_SECURITY_OPEN);
    TEST_ASSERT_TRUE(wardriving_wigle_wifi_row(out, sizeof(out), &n, &fix) > 0U);
    TEST_ASSERT_EQUAL_STRING_LEN("aa:bb:cc:dd:ee:07,\"\",[ESS],", out, 27);

    /* The longest there is: 32 quotes. */
    n = network("\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"", 0x08, 165U, FCK_BAND_5G, -127,
                FCK_WIFI_SECURITY_WPA2_WPA3_ENTERPRISE);
    fck_gnss_fix_t far = fix;
    far.lat   = -89.1234567;
    far.lon   = -179.1234567;
    far.alt_m = -412.6;
    far.hdop  = 99.9;
    size_t const len = wardriving_wigle_wifi_row(out, sizeof(out), &n, &far);
    TEST_ASSERT_TRUE(len > 0U && len < WARDRIVING_WIGLE_ROW_MAX);
    TEST_ASSERT_EQUAL_UINT(13, commas_outside_quotes(out));
    TEST_ASSERT_NOT_NULL(strstr(out, ",-89.1234567,-179.1234567,-413,499.5,,,WIFI\n"));
}

TEST_CASE("a Bluetooth device is a row too, with its manufacturer's number when it advertised one", "[wigle]")
{
    const fck_gnss_fix_t   fix = fix_good();
    const fck_ble_device_t d   = device("Pulsera", 0x09, -70);
    char                   out[WARDRIVING_WIGLE_ROW_MAX];
    const uint16_t         company = 0x004CU;
    size_t const           len     = wardriving_wigle_ble_row(out, sizeof(out), &d, &company, &fix);
    TEST_ASSERT_EQUAL_STRING("11:22:33:44:55:09,\"Pulsera\",Misc [LE],2026-09-29 20:26:34,0,,-70,"
                             "48.1173000,11.5166670,109,6.0,,76,BLE\n",
                             out);
    TEST_ASSERT_EQUAL_UINT(strlen(out), len);
    TEST_ASSERT_EQUAL_UINT(13, commas_outside_quotes(out));

    /* Most say no name, and many no manufacturer. */
    const fck_ble_device_t quiet = device("", 0x0A, -90);
    TEST_ASSERT_TRUE(wardriving_wigle_ble_row(out, sizeof(out), &quiet, NULL, &fix) > 0U);
    TEST_ASSERT_EQUAL_STRING("11:22:33:44:55:0a,\"\",Misc [LE],2026-09-29 20:26:34,0,,-90,"
                             "48.1173000,11.5166670,109,6.0,,,BLE\n",
                             out);
}

TEST_CASE("a fix places what was heard only when it is a position, dated, and recent", "[wigle]")
{
    fck_gnss_fix_t f = fix_good();
    TEST_ASSERT_TRUE(wardriving_wigle_fix_places(&f, 2000U));
    TEST_ASSERT_TRUE(wardriving_wigle_fix_places(&f, 300U));
    TEST_ASSERT_FALSE_MESSAGE(wardriving_wigle_fix_places(&f, 299U), "older than asked");
    TEST_ASSERT_FALSE(wardriving_wigle_fix_places(NULL, 2000U));

    f       = fix_good();
    f.valid = false;                        /* the receiver does not vouch for it */
    TEST_ASSERT_FALSE(wardriving_wigle_fix_places(&f, 2000U));
    f     = fix_good();
    f.lat = NAN;                            /* no sky */
    TEST_ASSERT_FALSE(wardriving_wigle_fix_places(&f, 2000U));
    f     = fix_good();
    f.lon = NAN;
    TEST_ASSERT_FALSE(wardriving_wigle_fix_places(&f, 2000U));
    f          = fix_good();
    f.has_date = false;                     /* a position with no day cannot be a row */
    TEST_ASSERT_FALSE(wardriving_wigle_fix_places(&f, 2000U));
    f          = fix_good();
    f.has_time = false;
    TEST_ASSERT_FALSE(wardriving_wigle_fix_places(&f, 2000U));
}

TEST_CASE("no row comes of a fix that is no position, of nothing, or with no room, and nothing is written", "[wigle]")
{
    const fck_gnss_fix_t     good = fix_good();
    const fck_wifi_network_t n    = network("Cafe Central", 0x01, 6U, FCK_BAND_2G, -50, FCK_WIFI_SECURITY_WPA2_PSK);
    const fck_ble_device_t   d    = device("Pulsera", 0x09, -70);
    char                     out[WARDRIVING_WIGLE_ROW_MAX] = "untouched";

    fck_gnss_fix_t bad = good;
    bad.lat = NAN;
    TEST_ASSERT_EQUAL_UINT(0, wardriving_wigle_wifi_row(out, sizeof(out), &n, &bad));
    TEST_ASSERT_EQUAL_UINT(0, wardriving_wigle_ble_row(out, sizeof(out), &d, NULL, &bad));
    bad          = good;
    bad.has_date = false;
    TEST_ASSERT_EQUAL_UINT(0, wardriving_wigle_wifi_row(out, sizeof(out), &n, &bad));
    TEST_ASSERT_EQUAL_UINT(0, wardriving_wigle_ble_row(out, sizeof(out), &d, NULL, &bad));

    TEST_ASSERT_EQUAL_UINT(0, wardriving_wigle_wifi_row(out, sizeof(out), NULL, &good));
    TEST_ASSERT_EQUAL_UINT(0, wardriving_wigle_wifi_row(out, sizeof(out), &n, NULL));
    TEST_ASSERT_EQUAL_UINT(0, wardriving_wigle_wifi_row(NULL, sizeof(out), &n, &good));
    TEST_ASSERT_EQUAL_UINT(0, wardriving_wigle_ble_row(out, sizeof(out), NULL, NULL, &good));
    TEST_ASSERT_EQUAL_UINT(0, wardriving_wigle_ble_row(out, sizeof(out), &d, NULL, NULL));
    TEST_ASSERT_EQUAL_UINT(0, wardriving_wigle_ble_row(NULL, sizeof(out), &d, NULL, &good));

    /* A row that does not fit whole is not written by halves. */
    TEST_ASSERT_EQUAL_UINT(0, wardriving_wigle_wifi_row(out, 60U, &n, &good));
    TEST_ASSERT_EQUAL_UINT(0, wardriving_wigle_ble_row(out, 60U, &d, NULL, &good));
    TEST_ASSERT_EQUAL_STRING("untouched", out);

    /* An altitude the receiver did not give is 0, and so is an accuracy without a dilution. */
    fck_gnss_fix_t flat = good;
    flat.alt_m = NAN;
    flat.hdop  = NAN;
    TEST_ASSERT_TRUE(wardriving_wigle_wifi_row(out, sizeof(out), &n, &flat) > 0U);
    TEST_ASSERT_NOT_NULL(strstr(out, ",48.1173000,11.5166670,0,0.0,,,WIFI\n"));
}


TEST_CASE("a position off the Earth, or one that is no number, places nothing", "[wigle]")
{
    const fck_wifi_network_t n = network("Cafe Central", 0x01, 6U, FCK_BAND_2G, -50, FCK_WIFI_SECURITY_WPA2_PSK);
    char                     out[WARDRIVING_WIGLE_ROW_MAX] = "untouched";
    const double             bad_lat[] = { 90.0001, -90.0001, INFINITY, -INFINITY, 1.0e300 };
    const double             bad_lon[] = { 180.0001, -180.0001, INFINITY, -INFINITY, -1.0e300 };

    for ( size_t i = 0U; i < (sizeof(bad_lat) / sizeof(bad_lat[0])); i++ )
    {
        fck_gnss_fix_t f = fix_good();
        f.lat = bad_lat[i];
        TEST_ASSERT_FALSE(wardriving_wigle_fix_places(&f, 2000U));
        TEST_ASSERT_EQUAL_UINT(0, wardriving_wigle_wifi_row(out, sizeof(out), &n, &f));
        f     = fix_good();
        f.lon = bad_lon[i];
        TEST_ASSERT_FALSE(wardriving_wigle_fix_places(&f, 2000U));
        TEST_ASSERT_EQUAL_UINT(0, wardriving_wigle_wifi_row(out, sizeof(out), &n, &f));
    }
    TEST_ASSERT_EQUAL_STRING("untouched", out);

    /* The poles and the date line are places. */
    fck_gnss_fix_t edge = fix_good();
    edge.lat = -90.0;
    edge.lon = 180.0;
    TEST_ASSERT_TRUE(wardriving_wigle_fix_places(&edge, 2000U));
    TEST_ASSERT_TRUE(wardriving_wigle_wifi_row(out, sizeof(out), &n, &edge) > 0U);
    TEST_ASSERT_NOT_NULL(strstr(out, ",-90.0000000,180.0000000,109,6.0,,,WIFI\n"));
}

TEST_CASE("a wild altitude or dilution is written as none, and the row keeps its fourteen columns", "[wigle]")
{
    const fck_wifi_network_t n = network("Cafe Central", 0x01, 6U, FCK_BAND_2G, -50, FCK_WIFI_SECURITY_WPA2_PSK);
    const fck_ble_device_t   d = device("Pulsera", 0x09, -70);
    char                     out[WARDRIVING_WIGLE_ROW_MAX];
    const double             wild[] = { 1.0e19, -1.0e19, 1.0e300, INFINITY, -INFINITY };

    for ( size_t i = 0U; i < (sizeof(wild) / sizeof(wild[0])); i++ )
    {
        fck_gnss_fix_t f = fix_good();
        f.alt_m = wild[i];
        f.hdop  = wild[i];
        TEST_ASSERT_TRUE(wardriving_wigle_wifi_row(out, sizeof(out), &n, &f) > 0U);
        TEST_ASSERT_NOT_NULL_MESSAGE(strstr(out, ",48.1173000,11.5166670,0,0.0,,,WIFI\n"), out);
        TEST_ASSERT_TRUE(wardriving_wigle_ble_row(out, sizeof(out), &d, NULL, &f) > 0U);
        TEST_ASSERT_NOT_NULL_MESSAGE(strstr(out, ",48.1173000,11.5166670,0,0.0,,,BLE\n"), out);
    }

    /* The highest and the lowest a receiver on the ground, or in a plane, can say. */
    fck_gnss_fix_t high = fix_good();
    high.alt_m = 8848.86;
    high.hdop  = 99.9;
    TEST_ASSERT_TRUE(wardriving_wigle_wifi_row(out, sizeof(out), &n, &high) > 0U);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(out, ",48.1173000,11.5166670,8849,499.5,,,WIFI\n"), out);
    high.alt_m = -430.5;
    TEST_ASSERT_TRUE(wardriving_wigle_wifi_row(out, sizeof(out), &n, &high) > 0U);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(out, ",48.1173000,11.5166670,-431,499.5,,,WIFI\n"), out);
}

TEST_CASE("a 5 GHz channel the band does not have has no frequency", "[wigle]")
{
    const fck_gnss_fix_t fix = fix_good();
    char                 out[WARDRIVING_WIGLE_ROW_MAX];
    const uint8_t        none[] = { 0U, 14U, 31U, 178U, 255U };

    for ( size_t i = 0U; i < (sizeof(none) / sizeof(none[0])); i++ )
    {
        const fck_wifi_network_t n = network("Atico", 0x02, none[i], FCK_BAND_5G, -60, FCK_WIFI_SECURITY_OPEN);
        char                     want[32];
        TEST_ASSERT_TRUE(wardriving_wigle_wifi_row(out, sizeof(out), &n, &fix) > 0U);
        (void)snprintf(want, sizeof(want), ",%u,0,-60,", (unsigned)none[i]);
        TEST_ASSERT_NOT_NULL_MESSAGE(strstr(out, want), out);
    }
    const fck_wifi_network_t low  = network("Atico", 0x02, 32U, FCK_BAND_5G, -60, FCK_WIFI_SECURITY_OPEN);
    const fck_wifi_network_t high = network("Atico", 0x02, 177U, FCK_BAND_5G, -60, FCK_WIFI_SECURITY_OPEN);
    TEST_ASSERT_TRUE(wardriving_wigle_wifi_row(out, sizeof(out), &low, &fix) > 0U);
    TEST_ASSERT_NOT_NULL(strstr(out, ",32,5160,-60,"));
    TEST_ASSERT_TRUE(wardriving_wigle_wifi_row(out, sizeof(out), &high, &fix) > 0U);
    TEST_ASSERT_NOT_NULL(strstr(out, ",177,5885,-60,"));
}

TEST_CASE("a name's bytes that are not UTF-8 become question marks, and the ones that are go through", "[wigle]")
{
    const fck_gnss_fix_t fix = fix_good();
    char                 out[WARDRIVING_WIGLE_ROW_MAX];

    /* Two, three and four bytes a character: a cafe, a euro, a face. */
    fck_wifi_network_t n = network("Caf\xC3\xA9 \xE2\x82\xAC \xF0\x9F\x98\x80", 0x03, 1U, FCK_BAND_2G, -40,
                                   FCK_WIFI_SECURITY_OPEN);
    TEST_ASSERT_TRUE(wardriving_wigle_wifi_row(out, sizeof(out), &n, &fix) > 0U);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(out, ",\"Caf\xC3\xA9 \xE2\x82\xAC \xF0\x9F\x98\x80\",[ESS],"), out);

    /* A lone byte of Latin-1, a continuation with nothing before it, a byte UTF-8 never has. */
    n = network("Caf\xE9 \x80 \xFF", 0x03, 1U, FCK_BAND_2G, -40, FCK_WIFI_SECURITY_OPEN);
    TEST_ASSERT_TRUE(wardriving_wigle_wifi_row(out, sizeof(out), &n, &fix) > 0U);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(out, ",\"Caf? ? ?\",[ESS],"), out);

    /* A name cut in the middle of a character, as 32 bytes cut a long one. */
    n = network("Ni\xC3\xB1o\xE2\x82", 0x03, 1U, FCK_BAND_2G, -40, FCK_WIFI_SECURITY_OPEN);
    TEST_ASSERT_TRUE(wardriving_wigle_wifi_row(out, sizeof(out), &n, &fix) > 0U);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(out, ",\"Ni\xC3\xB1o??\",[ESS],"), out);

    /* The long way to write a slash, and half of a UTF-16 pair: neither is UTF-8. */
    n = network("a\xC0\xAF" "b\xED\xA0\x80" "c", 0x03, 1U, FCK_BAND_2G, -40, FCK_WIFI_SECURITY_OPEN);
    TEST_ASSERT_TRUE(wardriving_wigle_wifi_row(out, sizeof(out), &n, &fix) > 0U);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(out, ",\"a??b???c\",[ESS],"), out);
}
