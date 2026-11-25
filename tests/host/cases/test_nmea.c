/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_nmea.c
 * @brief fck_drv_nmea: the GNSS receiver's sentences, read into a fix.
 *
 * The ME31GR02's sentences have the shape a module's receiver gave over a
 * night, logged with the positions hidden. Every position
 * here is made up, or is the NMEA 0183 textbook's: none is anybody's.
 * Most sentences get their checksum from nmea() below, and the textbook's
 * keep their own, so the checksum code is checked against numbers it did
 * not compute.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "unity.h"

#include "fck_drv_c5.h"
#include "fck_drv_link.h"
#include "fck_drv_nmea.h"

/* "$" + body + "*hh", with the checksum worked out here. */
static const char *nmea(const char *body)
{
    static char s[256];
    uint8_t     x = 0U;
    for (const char *p = body; *p != '\0'; p++) {
        x ^= (uint8_t)*p;
    }
    snprintf(s, sizeof(s), "$%s*%02X", body, x);
    return s;
}

static void near(double want, double got, double tol)
{
    char why[96];
    snprintf(why, sizeof(why), "want %.9f, got %.9f", want, got);
    TEST_ASSERT_TRUE_MESSAGE(fabs(want - got) <= tol, why);
}

/* The NMEA 0183 textbook's pair. */
#define BOOK_GGA "$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47"
#define BOOK_RMC "$GPRMC,123519,A,4807.038,N,01131.000,E,022.4,084.4,230394,003.1,W*6A"

TEST_CASE("a checksum is the XOR between $ and *, and only a line ending may follow it", "[nmea]")
{
    TEST_ASSERT_TRUE(fck_drv_nmea_checksum_ok(BOOK_GGA));
    TEST_ASSERT_TRUE(fck_drv_nmea_checksum_ok(BOOK_RMC));
    TEST_ASSERT_TRUE(fck_drv_nmea_checksum_ok(BOOK_GGA "\r\n"));
    TEST_ASSERT_TRUE(fck_drv_nmea_checksum_ok("$GPRMC,123519,A,4807.038,N,01131.000,E,022.4,084.4,230394,003.1,W*6a"));
    /* The protocol's example, and a line from that night's log. */
    TEST_ASSERT_TRUE(fck_drv_nmea_checksum_ok(
        "$GPGSV,5,1,17,05,46,010,24,06,00,000,09,11,00,000,00,12,00,000,00,1*6A"));
    TEST_ASSERT_TRUE(fck_drv_nmea_checksum_ok(
        "$GNGST,202634.00,999.00,999.00,999.00,0.00,999.00,999.00,999.00*78"));

    TEST_ASSERT_FALSE(fck_drv_nmea_checksum_ok("$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.5,M,46.9,M,,*47"));
    TEST_ASSERT_FALSE(fck_drv_nmea_checksum_ok("$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*4"));
    TEST_ASSERT_FALSE(fck_drv_nmea_checksum_ok("$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,"));
    TEST_ASSERT_FALSE(fck_drv_nmea_checksum_ok(BOOK_GGA "x"));
    TEST_ASSERT_FALSE(fck_drv_nmea_checksum_ok("GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47"));
    TEST_ASSERT_FALSE(fck_drv_nmea_checksum_ok("$GP\tGGA*00"));
    TEST_ASSERT_FALSE(fck_drv_nmea_checksum_ok(NULL));
}

TEST_CASE("a GGA with a fix gives the position, height, quality, satellites and HDOP", "[nmea]")
{
    fck_drv_nmea_t n;
    fck_drv_nmea_init(&n);
    TEST_ASSERT_EQUAL(FCK_DRV_NMEA_GGA, fck_drv_nmea_feed(&n, BOOK_GGA));
    near(48.0 + 7.038 / 60.0, n.fix.lat, 1e-12);
    near(11.0 + 31.0 / 60.0, n.fix.lon, 1e-12);
    near(545.4, n.fix.alt_m, 1e-9);
    near(0.9, n.fix.hdop, 1e-9);
    TEST_ASSERT_EQUAL_UINT8(1, n.fix.quality);
    TEST_ASSERT_EQUAL_UINT8(8, n.fix.sats);
    TEST_ASSERT_TRUE(n.fix.has_time);
    TEST_ASSERT_EQUAL_UINT8(12, n.fix.hour);
    TEST_ASSERT_EQUAL_UINT8(35, n.fix.min);
    TEST_ASSERT_EQUAL_UINT8(19, n.fix.sec);
    TEST_ASSERT_EQUAL_UINT16(0, n.fix.ms);
    /* GGA carries neither the date, nor speed, nor validity. */
    TEST_ASSERT_FALSE(n.fix.has_date);
    TEST_ASSERT_TRUE(isnan(n.fix.speed_kn));
    TEST_ASSERT_FALSE(n.fix.valid);
    TEST_ASSERT_EQUAL_UINT32(1, n.gga);
}

TEST_CASE("an RMC adds validity, speed, course and the date, and GGA's fields stay", "[nmea]")
{
    fck_drv_nmea_t n;
    fck_drv_nmea_init(&n);
    TEST_ASSERT_EQUAL(FCK_DRV_NMEA_GGA, fck_drv_nmea_feed(&n,
        nmea("GNGGA,202634.00,4807.0380,N,01131.0000,E,1,04,6.2,109.418,M,-3.399,M,,")));
    TEST_ASSERT_EQUAL(FCK_DRV_NMEA_RMC, fck_drv_nmea_feed(&n,
        nmea("GNRMC,202634.00,A,4807.0380,N,01131.0000,E,2.80,95.05,290926,,,A,V")));
    TEST_ASSERT_TRUE(n.fix.valid);
    near(2.80, n.fix.speed_kn, 1e-9);
    near(95.05, n.fix.course_deg, 1e-9);
    TEST_ASSERT_TRUE(n.fix.has_date);
    TEST_ASSERT_EQUAL_UINT8(29, n.fix.day);
    TEST_ASSERT_EQUAL_UINT8(9, n.fix.month);
    TEST_ASSERT_EQUAL_UINT16(2026, n.fix.year);
    TEST_ASSERT_EQUAL_UINT8(20, n.fix.hour);
    TEST_ASSERT_EQUAL_UINT8(26, n.fix.min);
    TEST_ASSERT_EQUAL_UINT8(34, n.fix.sec);
    /* From the GGA, untouched by the RMC. */
    TEST_ASSERT_EQUAL_UINT8(4, n.fix.sats);
    near(6.2, n.fix.hdop, 1e-9);
    near(109.418, n.fix.alt_m, 1e-9);
    near(48.0 + 7.038 / 60.0, n.fix.lat, 1e-12);
}

TEST_CASE("the ME31GR02 without a fix: quality 0, no position, HDOP 100, time and date kept", "[nmea]")
{
    fck_drv_nmea_t n;
    fck_drv_nmea_init(&n);
    TEST_ASSERT_EQUAL(FCK_DRV_NMEA_GGA, fck_drv_nmea_feed(&n, nmea("GNGGA,201435.96,,,,,0,00,100.00,,M,,M,,")));
    TEST_ASSERT_EQUAL_UINT8(0, n.fix.quality);
    TEST_ASSERT_EQUAL_UINT8(0, n.fix.sats);
    TEST_ASSERT_TRUE(isnan(n.fix.lat));
    TEST_ASSERT_TRUE(isnan(n.fix.lon));
    TEST_ASSERT_TRUE(isnan(n.fix.alt_m));
    near(100.0, n.fix.hdop, 1e-9);
    TEST_ASSERT_EQUAL_UINT16(960, n.fix.ms);

    TEST_ASSERT_EQUAL(FCK_DRV_NMEA_RMC, fck_drv_nmea_feed(&n, nmea("GNRMC,201435.96,V,,,,,,,290926,,,N,V")));
    TEST_ASSERT_FALSE(n.fix.valid);
    TEST_ASSERT_TRUE(isnan(n.fix.speed_kn));
    TEST_ASSERT_TRUE(n.fix.has_date);
    TEST_ASSERT_EQUAL_UINT16(2026, n.fix.year);

    /* Right after a reset: no time either. */
    TEST_ASSERT_EQUAL(FCK_DRV_NMEA_RMC, fck_drv_nmea_feed(&n, "$GNRMC,,V,,,,,,,,,,N,V*37"));
    TEST_ASSERT_FALSE(n.fix.has_time);
    TEST_ASSERT_FALSE(n.fix.has_date);
}

TEST_CASE("losing the fix clears the position: nothing is kept from an older sentence", "[nmea]")
{
    fck_drv_nmea_t n;
    fck_drv_nmea_init(&n);
    TEST_ASSERT_EQUAL(FCK_DRV_NMEA_GGA, fck_drv_nmea_feed(&n, BOOK_GGA));
    TEST_ASSERT_FALSE(isnan(n.fix.lat));
    TEST_ASSERT_EQUAL(FCK_DRV_NMEA_GGA, fck_drv_nmea_feed(&n, nmea("GNGGA,123520.00,,,,,0,00,100.00,,M,,M,,")));
    TEST_ASSERT_TRUE(isnan(n.fix.lat));
    TEST_ASSERT_TRUE(isnan(n.fix.alt_m));
    TEST_ASSERT_EQUAL_UINT8(0, n.fix.quality);
}

TEST_CASE("south and west are negative, and the minutes are read exactly", "[nmea]")
{
    fck_drv_nmea_t n;
    fck_drv_nmea_init(&n);
    TEST_ASSERT_EQUAL(FCK_DRV_NMEA_GGA, fck_drv_nmea_feed(&n,
        nmea("GNGGA,235960.5,3351.12345,S,15112.56789,W,2,12,0.8,-12.5,M,,M,,")));
    near(-(33.0 + 51.12345 / 60.0), n.fix.lat, 1e-12);
    near(-(151.0 + 12.56789 / 60.0), n.fix.lon, 1e-12);
    near(-12.5, n.fix.alt_m, 1e-9);
    TEST_ASSERT_EQUAL_UINT8(2, n.fix.quality);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(60, n.fix.sec, "a leap second");
    TEST_ASSERT_EQUAL_UINT16(500, n.fix.ms);

    /* Every talker: GPS, GLONASS, Galileo, BeiDou both ways. */
    const char *talkers[] = { "GP", "GL", "GA", "GB", "BD" };
    for (unsigned i = 0U; i < 5U; i++) {
        char body[96];
        snprintf(body, sizeof(body), "%sGGA,000001.00,0000.000,N,00000.000,E,1,05,1.0,0.0,M,,M,,",
                 talkers[i]);
        TEST_ASSERT_EQUAL(FCK_DRV_NMEA_GGA, fck_drv_nmea_feed(&n, nmea(body)));
    }
    near(0.0, n.fix.lat, 0.0);
}

TEST_CASE("a GGA or RMC that does not read is refused whole, and the fix stays as it was", "[nmea]")
{
    fck_drv_nmea_t n;
    fck_drv_nmea_init(&n);
    TEST_ASSERT_EQUAL(FCK_DRV_NMEA_GGA, fck_drv_nmea_feed(&n, BOOK_GGA));
    TEST_ASSERT_EQUAL(FCK_DRV_NMEA_RMC, fck_drv_nmea_feed(&n, BOOK_RMC));
    fck_drv_nmea_fix_t before = n.fix;

    const char *bad[] = {
        "GNGGA,123600,4807.038,X,01131.000,E,1,08,0.9,545.4,M,,M,,",   /* hemisphere */
        "GNGGA,123600,4860.000,N,01131.000,E,1,08,0.9,545.4,M,,M,,",   /* 60 minutes */
        "GNGGA,123600,9100.000,N,01131.000,E,1,08,0.9,545.4,M,,M,,",   /* 91 degrees */
        "GNGGA,123600,4807.038,N,18100.000,E,1,08,0.9,545.4,M,,M,,",   /* 181 degrees */
        "GNGGA,123600,4807.038,N,,,1,08,0.9,545.4,M,,M,,",             /* half a position */
        "GNGGA,123600,4807.038,N,01131.000,E,x,08,0.9,545.4,M,,M,,",   /* quality */
        "GNGGA,123600,4807.038,N,01131.000,E,1,08,0.9.1,545.4,M,,M,,", /* two points */
        "GNGGA,123600,4807.038,N,01131.000,E,1,08,0.9,-,M,,M,,",       /* a sign alone */
        "GNGGA,246000,4807.038,N,01131.000,E,1,08,0.9,545.4,M,,M,,",   /* hour 24 */
        "GNGGA,1236,4807.038,N,01131.000,E,1,08,0.9,545.4,M,,M,,",     /* short time */
        "GNGGA,123600.,4807.038,N,01131.000,E,1,08,0.9,545.4,M,,M,,",  /* a point alone */
        "GNGGA,123600,4807.038,N,01131.000,E,1,08,0.9",                /* cut short */
        "GNRMC,123600,A,4807.038,N,01131.000,E,1.0,2.0,320926,,,A",    /* day 32 */
        "GNRMC,123600,A,4807.038,N,01131.000,E,1.0,2.0,291326,,,A",    /* month 13 */
        "GNRMC,123600,A,4807.038,N,01131.000,E,1.0,2.0,2909,,,A",      /* short date */
        "GNRMC,123600,AV,4807.038,N,01131.000,E,1.0,2.0,290926,,,A",   /* status */
        "GNRMC,123600,A,4807.038,N,01131.000,E,-1.0,2.0,290926,,,A",   /* negative speed */
    };
    for (unsigned i = 0U; i < sizeof(bad) / sizeof(bad[0]); i++) {
        TEST_ASSERT_EQUAL_MESSAGE(FCK_DRV_NMEA_BAD, fck_drv_nmea_feed(&n, nmea(bad[i])), bad[i]);
    }
    TEST_ASSERT_EQUAL_MEMORY(&before, &n.fix, sizeof(before));
    TEST_ASSERT_EQUAL_UINT32(sizeof(bad) / sizeof(bad[0]), n.bad);
    TEST_ASSERT_EQUAL_UINT32(1, n.gga);
    TEST_ASSERT_EQUAL_UINT32(1, n.rmc);
}

TEST_CASE("other sentences are counted and not read: GST, GSV, the receiver's own", "[nmea]")
{
    fck_drv_nmea_t n;
    fck_drv_nmea_init(&n);
    TEST_ASSERT_EQUAL(FCK_DRV_NMEA_OTHER, fck_drv_nmea_feed(&n,
        "$GNGST,202634.00,999.00,999.00,999.00,0.00,999.00,999.00,999.00*78"));
    TEST_ASSERT_EQUAL(FCK_DRV_NMEA_OTHER, fck_drv_nmea_feed(&n,
        "$GPGSV,5,1,17,05,46,010,24,06,00,000,09,11,00,000,00,12,00,000,00,1*6A"));
    TEST_ASSERT_EQUAL(FCK_DRV_NMEA_OTHER, fck_drv_nmea_feed(&n, nmea("FWVER,V5.26.35.5")));
    TEST_ASSERT_EQUAL(FCK_DRV_NMEA_OTHER, fck_drv_nmea_feed(&n, nmea("PODYN,1")));
    TEST_ASSERT_EQUAL(FCK_DRV_NMEA_BAD, fck_drv_nmea_feed(&n, "junk"));
    TEST_ASSERT_EQUAL(FCK_DRV_NMEA_BAD, fck_drv_nmea_feed(NULL, BOOK_GGA));
    TEST_ASSERT_EQUAL_UINT32(4, n.other);
    TEST_ASSERT_EQUAL_UINT32(1, n.bad);
    TEST_ASSERT_TRUE(isnan(n.fix.lat));
}

/* ---- Through the C5's relay ----------------------------------------------------- */

static void on_relay(void *ctx, const fck_drv_link_message_t *m)
{
    if (strcmp(m->tag, "GNSSU") == 0) {
        (void)fck_drv_nmea_feed(ctx, m->body);
    }
}

TEST_CASE("a #GNSSU's body is the sentence, even in the middle of a command", "[nmea]")
{
    static fck_drv_link_t l;
    fck_drv_nmea_t        n;
    fck_drv_nmea_init(&n);
    fck_drv_link_config_t cfg = { .async = fck_drv_c5_async, .on_async = on_relay, .async_ctx = &n };
    TEST_ASSERT_EQUAL(KODE_OK, fck_drv_link_init(&l, &cfg));

    fck_drv_c5_command_t c;
    TEST_ASSERT_EQUAL(KODE_OK, fck_drv_c5_command_gnss_transmit(&c, "POLCFGMSG,0,2,1"));
    TEST_ASSERT_EQUAL(KODE_OK, fck_drv_link_begin(&l, c.verb, NULL, NULL));

    char line[160];
    snprintf(line, sizeof(line), "#GNSSU %s\r\n", BOOK_GGA);
    fck_drv_link_feed(&l, (const uint8_t *)line, strlen(line));
    const char *rest = "#GTX sent=\"$POLCFGMSG,0,2,1\"\r\n#DONE gtx rc=0\r\n";
    fck_drv_link_feed(&l, (const uint8_t *)rest, strlen(rest));
    snprintf(line, sizeof(line), "#GNSSU %s\r\n", BOOK_RMC);
    fck_drv_link_feed(&l, (const uint8_t *)line, strlen(line));

    TEST_ASSERT_TRUE(fck_drv_link_done(&l, NULL));
    TEST_ASSERT_EQUAL_UINT32(1, n.gga);
    TEST_ASSERT_EQUAL_UINT32(1, n.rmc);
    TEST_ASSERT_TRUE(n.fix.valid);
    TEST_ASSERT_EQUAL_UINT8(8, n.fix.sats);
}
