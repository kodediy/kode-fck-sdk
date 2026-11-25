/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_2_gnss.c
 * @brief The GNSS receiver: the owner's fix slot (fck_aos_gnss_fix()) and the
 *        facade's fck_core_gnss_begin(), fck_core_gnss_read() and fck_core_gnss_end().
 *
 * The C5's firmware is scripted: `gnss on` and `gnss off` are answered, and the
 * sentences the receiver sends arrive as #GNSSU lines, the way the relay sends
 * them. The coordinates are made up.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "unity.h"

#include "fck.h"
#include "fck_core.h"
#include "fck_aos.h"
#include "fck_bsp.h"

#include "fake_world.h"

/* Two seconds of a receiver: A and B differ in the position and the time. */
#define GGA_A "GNGGA,202634.00,4807.0380,N,01131.0000,E,1,04,6.2,109.418,M,-3.399,M,,"
#define RMC_A "GNRMC,202634.00,A,4807.0380,N,01131.0000,E,2.80,95.05,290926,,,A,V"
#define GGA_B "GNGGA,202635.00,4807.0560,N,01131.0120,E,1,07,1.1,110.250,M,-3.399,M,,"
#define RMC_B "GNRMC,202635.00,A,4807.0560,N,01131.0120,E,3.10,96.50,290926,,,A,V"

static void say(const char *body, uint32_t delay_ms)
{
    char s[160];
    char line[200];
    sim_nmea(s, sizeof(s), body);
    (void)snprintf(line, sizeof(line), "#GNSSU %s\r\n", s);
    sim_inject(SIM_C5, delay_ms, line);
}

static void script_relay(void)
{
    sim_script(SIM_C5, "gnss on", "#DONE gnss rc=0\r\n", 5U);
    sim_script(SIM_C5, "gnss off", "#DONE gnss rc=0\r\n", 5U);
}

/* A module with a receiver on and the relay up. */
static void begin_running(void)
{
    sim_begin_fck(NULL, NULL);
    script_relay();
    TEST_ASSERT_EQUAL_MESSAGE(KODE_OK, fck_core_gnss_begin(), fck_error());
}

static void assert_a(const fck_gnss_fix_t *g)
{
    TEST_ASSERT_TRUE(g->valid);
    TEST_ASSERT_EQUAL_UINT8(1, g->quality);
    TEST_ASSERT_EQUAL_UINT8(4, g->sats);
    TEST_ASSERT_DOUBLE_WITHIN(1e-6, 48.1173, g->lat);
    TEST_ASSERT_DOUBLE_WITHIN(1e-6, 11.516667, g->lon);
    TEST_ASSERT_DOUBLE_WITHIN(1e-3, 109.418, g->alt_m);
    TEST_ASSERT_DOUBLE_WITHIN(1e-3, 6.2, g->hdop);
    TEST_ASSERT_DOUBLE_WITHIN(1e-3, 2.80, g->speed_kn);
    TEST_ASSERT_DOUBLE_WITHIN(1e-3, 95.05, g->course_deg);
    TEST_ASSERT_TRUE(g->has_time);
    TEST_ASSERT_EQUAL_UINT8(20, g->hour);
    TEST_ASSERT_EQUAL_UINT8(26, g->min);
    TEST_ASSERT_EQUAL_UINT8(34, g->sec);
    TEST_ASSERT_TRUE(g->has_date);
    TEST_ASSERT_EQUAL_UINT8(29, g->day);
    TEST_ASSERT_EQUAL_UINT8(9, g->month);
    TEST_ASSERT_EQUAL_UINT16(2026, g->year);
}

TEST_CASE("with the receiver off there is no fix, and nothing is waited for", "[gnss]")
{
    sim_begin_fck(NULL, NULL);
    fck_drv_nmea_fix_t f;
    fck_gnss_fix_t g;
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_aos_gnss_fix(0U, &f, NULL));
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_gnss_read(&g, 0U));
    uint32_t const t0 = sim_now_ms();
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_aos_gnss_fix(3000U, &f, NULL));
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_gnss_read(&g, 3000U));
    TEST_ASSERT_TRUE_MESSAGE(sim_now_ms() - t0 < 300U, "a receiver that is off is not waited for");
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_gnss_read(NULL, 0U));
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_aos_gnss_fix(0U, NULL, NULL));
}

TEST_CASE("with no module there is nothing to start, read or stop", "[gnss]")
{
    sim_begin_empty();
    fck_gnss_fix_t g;
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_gnss_begin());
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_gnss_read(&g, 0U));
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_gnss_end());
}

TEST_CASE("start takes the receiver out of reset and turns the relay on, once", "[gnss]")
{
    sim_begin_fck(NULL, NULL);
    script_relay();
    TEST_ASSERT_FALSE(sim_gnss_running());
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_gnss_begin());
    TEST_ASSERT_TRUE_MESSAGE(sim_gnss_running(), "P2 let go");
    /* With the receiver on the bus the expander is out of reach: SW0 was
     * left driven at rest, high, so a listen goes through 433's leg or
     * through 868 and 915's. */
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(FCK_BSP_EXPANDER_SW0_MASK, (uint8_t)(sim_exp_out() & FCK_BSP_EXPANDER_SW0_MASK),
                                   "SW0 at rest");
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x00, (uint8_t)(sim_exp_cfg() & FCK_BSP_EXPANDER_SW0_MASK), "SW0 driven");
    fck_status_t info;
    fck_core_status(&info);
    TEST_ASSERT_TRUE(info.gnss_running);
    TEST_ASSERT_EQUAL_UINT(1, sim_sent(SIM_C5, "gnss on"));
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0, sim_sent(SIM_C5, "gnss off"),
                                   "a 2.0 firmware takes a repeated `gnss on`");
    /* Again: the same, and harmless. */
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_gnss_begin());
    TEST_ASSERT_EQUAL_UINT(2, sim_sent(SIM_C5, "gnss on"));
    TEST_ASSERT_EQUAL_UINT(0, sim_sent(SIM_C5, "gnss off"));
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_gnss_end());
}

TEST_CASE("a firmware before 1.5 is told gnss off first: a repeated gnss on froze it", "[gnss]")
{
    sim_begin_fck("#C5INFO fw=c5_fck_agent ver=1.3", NULL);
    script_relay();
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_gnss_begin());
    TEST_ASSERT_EQUAL_UINT(1, sim_sent(SIM_C5, "gnss off"));
    TEST_ASSERT_EQUAL_UINT(1, sim_sent(SIM_C5, "gnss on"));
    TEST_ASSERT_EQUAL_STRING("gnss off", sim_sent_line(SIM_C5, 0));
    TEST_ASSERT_EQUAL_STRING("gnss on", sim_sent_line(SIM_C5, 1));
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_gnss_end());
}

TEST_CASE("a relay that would not turn on puts the receiver back in reset", "[gnss]")
{
    sim_begin_fck(NULL, NULL);
    sim_script(SIM_C5, "gnss on", "#DONE gnss rc=1\r\n", 5U);
    sim_script(SIM_C5, "gnss off", "#DONE gnss rc=0\r\n", 5U);
    TEST_ASSERT_EQUAL(KODE_IO_ERROR, fck_core_gnss_begin());
    TEST_ASSERT_FALSE(sim_gnss_running());
    fck_status_t info;
    fck_core_status(&info);
    TEST_ASSERT_FALSE(info.gnss_running);
}

TEST_CASE("a start on a C5 that runs another firmware is refused, and the receiver stays off", "[gnss]")
{
    sim_begin_fck(SIM_C5_BANNER_OWN, NULL);
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_gnss_begin());
    TEST_ASSERT_FALSE(sim_gnss_running());
    TEST_ASSERT_EQUAL_UINT(0, sim_sent(SIM_C5, "gnss"));
}

TEST_CASE("a fix is a GGA and an RMC, and is read whole", "[gnss]")
{
    begin_running();
    fck_gnss_fix_t g;
    fck_drv_nmea_fix_t f;
    TEST_ASSERT_EQUAL(KODE_TIMEOUT, fck_aos_gnss_fix(0U, &f, NULL));
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_gnss_read(&g, 0U));

    say(GGA_A, 10U);
    sim_sleep(150U);
    TEST_ASSERT_EQUAL_MESSAGE(KODE_DENIED, fck_core_gnss_read(&g, 0U), "half a fix is none");
    say(RMC_A, 10U);
    sim_sleep(150U);
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_gnss_read(&g, 0U));
    assert_a(&g);

    /* Read again: it is still the latest. */
    memset(&g, 0, sizeof(g));
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_gnss_read(&g, 0U));
    assert_a(&g);
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_gnss_end());
}

TEST_CASE("sentences that are not a fix are counted out: bad sums, other kinds", "[gnss]")
{
    begin_running();
    fck_gnss_fix_t g;
    sim_inject(SIM_C5, 5U, "#GNSSU $GNGGA,202634.00,4807.0380,N,01131.0000,E,1,04,6.2,109.418,M,-3.399,M,,*00\r\n");
    say("GNGSA,A,3,01,02,03,04,,,,,,,,,2.0,1.0,1.7", 10U);
    say(RMC_A, 20U);
    sim_sleep(200U);
    TEST_ASSERT_EQUAL_MESSAGE(KODE_DENIED, fck_core_gnss_read(&g, 0U), "a RMC alone, a GGA that did not read");
    say(GGA_A, 10U);
    sim_sleep(150U);
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_gnss_read(&g, 0U));
    assert_a(&g);
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_gnss_end());
}

TEST_CASE("waiting gives the next fix, not the one that is there", "[gnss]")
{
    begin_running();
    fck_gnss_fix_t g;
    say(GGA_A, 10U);
    say(RMC_A, 20U);
    sim_sleep(150U);
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_gnss_read(&g, 0U));
    assert_a(&g);

    /* B comes 400 ms into the wait. */
    say(GGA_B, 400U);
    say(RMC_B, 420U);
    uint32_t const t0 = sim_now_ms();
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_gnss_read(&g, 3000U));
    uint32_t const took = sim_now_ms() - t0;
    TEST_ASSERT_TRUE_MESSAGE(took >= 380U, "it waited for B and did not return A");
    TEST_ASSERT_TRUE(took < 1500U);
    TEST_ASSERT_DOUBLE_WITHIN(1e-6, 48.1176, g.lat);
    TEST_ASSERT_EQUAL_UINT8(7, g.sats);
    TEST_ASSERT_EQUAL_UINT8(35, g.sec);

    /* And with nothing coming, it runs out. */
    uint32_t const t1 = sim_now_ms();
    TEST_ASSERT_EQUAL(KODE_TIMEOUT, fck_core_gnss_read(&g, 400U));
    uint32_t const ran = sim_now_ms() - t1;
    TEST_ASSERT_TRUE(ran >= 380U && ran < 900U);
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_gnss_end());
}

TEST_CASE("a fix says how long ago it was read, and a newer one starts from nothing", "[gnss]")
{
    begin_running();
    fck_gnss_fix_t g;
    say(GGA_A, 10U);
    say(RMC_A, 20U);
    sim_sleep(150U);
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_gnss_read(&g, 0U));
    assert_a(&g);
    uint32_t const young = g.age_ms;
    TEST_ASSERT_TRUE_MESSAGE(young >= 50U && young < 600U, "read some 130 ms ago");

    /* Nothing comes for a while - a receiver that lost the sky, a relay that stopped - and the
     * fix is the same one, older. */
    sim_sleep(700U);
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_gnss_read(&g, 0U));
    assert_a(&g);
    TEST_ASSERT_TRUE_MESSAGE(g.age_ms >= young + 600U, "700 ms older");
    TEST_ASSERT_TRUE(g.age_ms < young + 1500U);
    uint32_t const old = g.age_ms;

    say(GGA_B, 10U);
    say(RMC_B, 20U);
    sim_sleep(150U);
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_gnss_read(&g, 0U));
    TEST_ASSERT_EQUAL_UINT8(35, g.sec);
    TEST_ASSERT_TRUE_MESSAGE(g.age_ms < old, "B's age, not A's");
    TEST_ASSERT_TRUE(g.age_ms < 600U);

    /* The one waited for is read as it comes. */
    say(GGA_A, 300U);
    say(RMC_A, 320U);
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_gnss_read(&g, 3000U));
    TEST_ASSERT_EQUAL_UINT8(34, g.sec);
    TEST_ASSERT_TRUE_MESSAGE(g.age_ms < 300U, "just read");
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_gnss_end());
}

TEST_CASE("the fix goes on coming while the C5 scans, and the relay's lines are no network or device", "[gnss][c5]")
{
    begin_running();
    fck_gnss_fix_t g;
    say(GGA_A, 10U);
    say(RMC_A, 20U);
    sim_sleep(150U);
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_gnss_read(&g, 0U));
    assert_a(&g);

    /* The C5 is one chip with one UART to the Kode: the receiver's second arrives between the
     * lines of the scan's list. */
    char gga[160];
    char rmc[160];
    char text[900];
    sim_nmea(gga, sizeof(gga), GGA_B);
    sim_nmea(rmc, sizeof(rmc), RMC_B);
    (void)snprintf(text, sizeof(text),
                   "#AP ssid=\"one\" bssid=aa:bb:cc:dd:ee:01 ch=1 band=2g rssi=-50 auth=wpa2_psk\r\n"
                   "#GNSSU %s\r\n"
                   "#AP ssid=\"two\" bssid=aa:bb:cc:dd:ee:02 ch=100 band=5g rssi=-60 auth=open\r\n"
                   "#GNSSU %s\r\n"
                   "#SCANDONE n=2 ms=4000\r\n#DONE scan rc=0\r\n",
                   gga, rmc);
    sim_script(SIM_C5, "scan", text, 5U);
    fck_wifi_network_t ap[4];
    size_t             n = 0;
    TEST_ASSERT_EQUAL_MESSAGE(KODE_OK, fck_core_wifi_scan(FCK_BAND_ALL, ap, 4U, &n), fck_error());
    TEST_ASSERT_EQUAL_UINT(2, n);
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_gnss_read(&g, 0U));
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(35, g.sec, "the second that came inside the scan");
    TEST_ASSERT_DOUBLE_WITHIN(1e-6, 48.1176, g.lat);
    TEST_ASSERT_TRUE_MESSAGE(g.age_ms < 1500U, "its age is the scan's end, not the fix before it");

    /* And between a Bluetooth scan's. */
    sim_nmea(gga, sizeof(gga), GGA_A);
    sim_nmea(rmc, sizeof(rmc), RMC_A);
    (void)snprintf(text, sizeof(text),
                   "#BLE addr=aa:bb:cc:dd:ee:01 type=rnd rssi=-50 name=\"\" adv=3\r\n"
                   "#GNSSU %s\r\n"
                   "#GNSSU %s\r\n"
                   "#BLE addr=aa:bb:cc:dd:ee:02 type=pub rssi=-70 name=\"\" adv=1\r\n"
                   "#BLEDONE n=2 ms=5003\r\n#DONE ble rc=0\r\n",
                   gga, rmc);
    sim_script(SIM_C5, "ble scan", text, 5U);
    fck_ble_device_t d[4];
    TEST_ASSERT_EQUAL_MESSAGE(KODE_OK, fck_core_ble_scan(5U, d, 4U, &n), fck_error());
    TEST_ASSERT_EQUAL_UINT(2, n);
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_gnss_read(&g, 0U));
    assert_a(&g);
    TEST_ASSERT_TRUE(g.age_ms < 1500U);
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_gnss_end());
}

TEST_CASE("a fix that came before the call is not the one waited for, even half of it", "[gnss]")
{
    begin_running();
    fck_gnss_fix_t g;
    /* B's GGA is in before the call; its RMC arrives during it. That fix is
     * completed after the call, and counts. */
    say(GGA_B, 5U);
    sim_sleep(100U);
    say(RMC_B, 300U);
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_gnss_read(&g, 2000U));
    TEST_ASSERT_DOUBLE_WITHIN(1e-6, 48.1176, g.lat);
}

typedef struct {
    unsigned n;
    char     first[24];
} listen_t;

static void on_line(void *ctx, const fck_drv_link_message_t *m)
{
    listen_t *const l = ctx;
    if (strcmp(m->tag, "GNSSU") == 0) {
        if (l->n == 0U) {
            (void)snprintf(l->first, sizeof(l->first), "%.15s", m->body);
        }
        l->n++;
    }
}

TEST_CASE("the app's listener goes on getting the sentences the owner reads", "[gnss]")
{
    begin_running();
    listen_t l = { 0 };
    TEST_ASSERT_EQUAL(KODE_OK, fck_aos_listen(FCK_AOS_C5, on_line, &l));
    fck_gnss_fix_t g;
    say(GGA_A, 10U);
    say(RMC_A, 20U);
    sim_sleep(200U);
    TEST_ASSERT_EQUAL_UINT(2, l.n);
    TEST_ASSERT_EQUAL_STRING("$GNGGA,202634.0", l.first);
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_gnss_read(&g, 0U));
    assert_a(&g);
    TEST_ASSERT_EQUAL(KODE_OK, fck_aos_listen(FCK_AOS_C5, NULL, NULL));
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_gnss_end());
}

TEST_CASE("stopping forgets the fix, and so does starting again", "[gnss]")
{
    begin_running();
    fck_gnss_fix_t g;
    say(GGA_A, 10U);
    say(RMC_A, 20U);
    sim_sleep(150U);
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_gnss_read(&g, 0U));

    TEST_ASSERT_EQUAL(KODE_OK, fck_core_gnss_end());
    TEST_ASSERT_FALSE_MESSAGE(sim_gnss_running(), "back in reset");
    fck_status_t info;
    fck_core_status(&info);
    TEST_ASSERT_FALSE(info.gnss_running);
    TEST_ASSERT_EQUAL_UINT(1, sim_sent(SIM_C5, "gnss off"));
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_gnss_read(&g, 0U));

    /* A sentence that was still on its way is not a fix of the next run. */
    say(GGA_B, 5U);
    say(RMC_B, 6U);
    sim_sleep(100U);
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_gnss_begin());
    TEST_ASSERT_EQUAL_MESSAGE(KODE_DENIED, fck_core_gnss_read(&g, 0U), "the old fix is gone");
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_gnss_end());
}

TEST_CASE("a reset of the C5 takes the fix with it, and the receiver runs on", "[gnss]")
{
    begin_running();
    fck_gnss_fix_t g;
    say(GGA_A, 10U);
    say(RMC_A, 20U);
    sim_sleep(150U);
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_gnss_read(&g, 0U));

    TEST_ASSERT_EQUAL(KODE_OK, fck_core_reset(FCK_C5));
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_gnss_read(&g, 0U));
    fck_status_t info;
    fck_core_status(&info);
    TEST_ASSERT_TRUE_MESSAGE(info.gnss_running, "the receiver is not the C5's");
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_gnss_end());
}

/* A job of the CC1101's kind: it waits and leaves both chips alone, and
 * halfway through looks at the fix, waiting for nothing. */
typedef struct {
    uint32_t           ms;
    int                uart[FCK_AOS_CHIPS];
    kode_status_t      midway;
    fck_drv_nmea_fix_t fix;
} side_job_t;

static kode_status_t side_job(void *arg, const fck_aos_job_context_t *job)
{
    side_job_t *const j  = arg;
    j->uart[FCK_AOS_C5]  = job->uart[FCK_AOS_C5];
    j->uart[FCK_AOS_NRF] = job->uart[FCK_AOS_NRF];
    sim_sleep(j->ms / 2U);
    j->midway = fck_aos_gnss_fix(0U, &j->fix, NULL);
    sim_sleep(j->ms / 2U);
    return KODE_OK;
}

TEST_CASE("a job without the chips gets no UART, and the fix comes on through it", "[gnss]")
{
    begin_running();
    fck_gnss_fix_t g;
    say(GGA_A, 10U);
    say(RMC_A, 20U);
    sim_sleep(150U);
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_gnss_read(&g, 0U));

    /* B comes 200 ms into a 1 s job, and is read as it comes. */
    say(GGA_B, 200U);
    say(RMC_B, 220U);
    side_job_t j = { .ms = 1000U };
    TEST_ASSERT_EQUAL(KODE_OK, fck_aos_run_job_without_chips(side_job, &j, 1000U));
    TEST_ASSERT_EQUAL(-1, j.uart[FCK_AOS_C5]);
    TEST_ASSERT_EQUAL(-1, j.uart[FCK_AOS_NRF]);
    TEST_ASSERT_EQUAL(KODE_OK, j.midway);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(35, j.fix.sec, "B was read while the job ran");
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_gnss_read(&g, 0U));
    TEST_ASSERT_EQUAL_UINT8(35, g.sec);

    /* A job with the chips - a write - takes the fix with it. */
    j.ms = 100U;
    TEST_ASSERT_EQUAL(KODE_OK, fck_aos_run_job(side_job, &j, 1000U));
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_gnss_read(&g, 0U));
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_gnss_end());
}

TEST_CASE("a module set up again has no fix of the last one", "[gnss]")
{
    begin_running();
    fck_gnss_fix_t g;
    say(GGA_A, 10U);
    say(RMC_A, 20U);
    sim_sleep(150U);
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_gnss_read(&g, 0U));
    fck_status_t before;
    fck_core_status(&before);
    size_t const before_on = sim_sent(SIM_C5, "gnss on");

    sim_blip();
    TEST_ASSERT_TRUE(sim_wait_generation(before.generation + 1U, 4000U));
    TEST_ASSERT_NOT_EQUAL_MESSAGE(KODE_OK, fck_core_gnss_read(&g, 0U), "the last module's fix");
    /* The receiver the app had running is set up again with the module (FCK_EVENT_SETTING_RESTORED):
     * it has no fix yet, and the last one is not given. */
    sim_sleep(1500U);
    fck_status_t info;
    fck_core_status(&info);
    TEST_ASSERT_TRUE_MESSAGE(info.gnss_running, "the owner started the receiver again");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, sim_sent(SIM_C5, "gnss on") - before_on, "and the C5 relays it again");
    TEST_ASSERT_NOT_EQUAL_MESSAGE(KODE_OK, fck_core_gnss_read(&g, 0U), "the last module's fix");
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_gnss_end());
}

static void unplug_now(void)
{
    sim_unplug();
}

TEST_CASE("a module that leaves while a read waits ends the wait with IO_ERROR", "[gnss]")
{
    begin_running();
    fck_gnss_fix_t g;
    sim_later(500U, unplug_now);
    uint32_t const t0 = sim_now_ms();
    TEST_ASSERT_EQUAL(KODE_IO_ERROR, fck_core_gnss_read(&g, 20000U));
    TEST_ASSERT_TRUE_MESSAGE(sim_now_ms() - t0 < 6000U, "it did not wait the whole time");
}

TEST_CASE("a receiver whose firmware takes SDA is noticed, and stop gives the bus back", "[gnss]")
{
    sim_begin_fck(NULL, NULL);
    sim_gnss_standard_fw(true);
    script_relay();
    TEST_ASSERT_EQUAL_MESSAGE(KODE_OK, fck_core_gnss_begin(), fck_error());
    sim_sleep(1000U);
    TEST_ASSERT_TRUE(sim_bus_dead());
    TEST_ASSERT_TRUE(sim_log_has("holds SDA"));
    /* The expander is out of reach: what needs it says so. */
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_reset(FCK_NRF));
    fck_status_t info;
    fck_core_status(&info);
    TEST_ASSERT_TRUE(info.present && info.gnss_running);

    TEST_ASSERT_EQUAL(KODE_OK, fck_core_gnss_end());
    TEST_ASSERT_FALSE(sim_bus_dead());
    TEST_ASSERT_FALSE(sim_gnss_running());
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1, sim_wire_count(SIM_W_BLIND) / 2U, "put back in reset by hand");
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_reset(FCK_NRF));
    TEST_ASSERT_TRUE(fck_is_present());
}

TEST_CASE("nothing of a position or a sentence reaches the log", "[gnss][privacy]")
{
    begin_running();
    fck_gnss_fix_t g;
    listen_t l = { 0 };
    TEST_ASSERT_EQUAL(KODE_OK, fck_aos_listen(FCK_AOS_C5, on_line, &l));
    sim_inject(SIM_C5, 5U, "#GNSSU $GNGGA,202634.00,4807.0380,N,01131.0000,E,1,04,6.2,109.418,M,-3.399,M,,*00\r\n");
    say(GGA_A, 10U);
    say("GNGSA,A,3,01,02,03,04,,,,,,,,,2.0,1.0,1.7", 15U);
    say(RMC_A, 20U);
    say(GGA_B, 1000U);
    say(RMC_B, 1010U);
    sim_sleep(200U);
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_gnss_read(&g, 0U));
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_gnss_read(&g, 3000U));
    TEST_ASSERT_EQUAL(KODE_OK, fck_aos_listen(FCK_AOS_C5, NULL, NULL));
    fck_status_t before;
    fck_core_status(&before);
    sim_blip();
    TEST_ASSERT_TRUE(sim_wait_generation(before.generation + 1U, 4000U));
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_gnss_end());

    static const char *const needles[] = {
        "$", "GGA", "RMC", "GSA", "GNSSU", "4807", "1131", "48.1", "11.5", "109.4", "110.2",
        "202634", "202635", "290926", "2.80", "95.05",
    };
    for (size_t i = 0U; i < sizeof(needles) / sizeof(needles[0]); i++) {
        TEST_ASSERT_FALSE_MESSAGE(sim_log_has(needles[i]), needles[i]);
    }
}
