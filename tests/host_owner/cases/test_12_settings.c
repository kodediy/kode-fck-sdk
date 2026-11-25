/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_12_settings.c
 * @brief What the app set and the module forgets when it leaves or loses power:
 *        told as FCK_AOS_EVENT_SETTING_LOST right after GONE, set again when it does not
 *        emit (the GNSS receiver, the sub-GHz listening) and told as
 *        FCK_AOS_EVENT_SETTING_RESTORED right after PRESENT. Nothing that emits is set again.
 */
#include <string.h>

#include "unity.h"

#include "fck.h"
#include "fck_core.h"
#include "fck_aos.h"

#include "fake_world.h"

#define EVENTS_MAX 12U
#define HZ_433     433920000U

static struct {
    unsigned    n;
    fck_aos_event_t ev[EVENTS_MAX];
    uint32_t    arg[EVENTS_MAX];
    bool        listening_when_lost;   /* what fck_aos_setting_listening() said inside LOST */
} g;

static void on_ev(fck_aos_event_t ev, uint32_t arg, void *ctx)
{
    (void)ctx;
    if (g.n < EVENTS_MAX) {
        g.ev[g.n]  = ev;
        g.arg[g.n] = arg;
    }
    if (ev == FCK_AOS_EVENT_SETTING_LOST) {
        uint32_t hz  = 0U;
        uint32_t gen = 0U;
        g.listening_when_lost = fck_aos_setting_listening(&hz, &gen);
    }
    g.n++;
}

static void listen_for_events(void)
{
    (void)memset(&g, 0, sizeof(g));
    sim_on_event(on_ev, NULL);
}

static void deafen(void)
{
    sim_on_event(NULL, NULL);
}

/* The module loses power and comes back, and the owner sets it up again. */
static void blip_and_wait(void)
{
    fck_status_t info;
    fck_core_status(&info);
    sim_blip();
    TEST_ASSERT_TRUE(sim_wait_generation(info.generation + 1U, 4000U));
    sim_sleep(1200U);     /* the C5 says what it runs, and the relay is set again */
}

/* The receiver on, with the C5's relay answering. */
static void gnss_on(void)
{
    sim_script(SIM_C5, "gnss on", "#DONE gnss rc=0\r\n", 5U);
    sim_script(SIM_C5, "gnss off", "#DONE gnss rc=0\r\n", 5U);
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_gnss_begin());
}

static void expect(unsigned i, fck_aos_event_t ev, uint32_t arg)
{
    TEST_ASSERT_TRUE_MESSAGE(i < g.n, "an event is missing");
    TEST_ASSERT_EQUAL_UINT32((uint32_t)ev, (uint32_t)g.ev[i]);
    TEST_ASSERT_EQUAL_UINT32(arg, g.arg[i]);
}

TEST_CASE("the receiver running when the module loses power: GONE, LOST, PRESENT, RESTORED, and it runs again", "[settings]")
{
    sim_begin_fck(NULL, NULL);
    gnss_on();
    listen_for_events();
    blip_and_wait();
    deafen();
    TEST_ASSERT_EQUAL_UINT(4U, g.n);
    expect(0U, FCK_AOS_EVENT_GONE, 0U);
    expect(1U, FCK_AOS_EVENT_SETTING_LOST, FCK_SETTING_GNSS);
    expect(2U, FCK_AOS_EVENT_PRESENT, 0U);
    expect(3U, FCK_AOS_EVENT_SETTING_RESTORED, FCK_SETTING_GNSS);
    fck_status_t info;
    fck_core_status(&info);
    TEST_ASSERT_TRUE(info.gnss_running);
    TEST_ASSERT_TRUE(sim_gnss_running());
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_gnss_end());
}

TEST_CASE("a module that leaves for good: LOST, nothing restored; stopping the receiver meanwhile forgets it", "[settings]")
{
    sim_begin_fck(NULL, NULL);
    gnss_on();
    listen_for_events();
    sim_unplug();
    TEST_ASSERT_TRUE(sim_wait_absent(5000U));
    sim_sleep(100U);
    TEST_ASSERT_EQUAL_UINT(2U, g.n);
    expect(0U, FCK_AOS_EVENT_GONE, 0U);
    expect(1U, FCK_AOS_EVENT_SETTING_LOST, FCK_SETTING_GNSS);
    /* The app gives it up while the module is away: it is not started when it comes back. */
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_gnss_end());
    sim_plug(SIM_DEV_EXPANDER, false);
    TEST_ASSERT_TRUE(sim_wait_present(3000U));
    sim_sleep(300U);
    deafen();
    TEST_ASSERT_EQUAL_UINT(3U, g.n);
    expect(2U, FCK_AOS_EVENT_PRESENT, 0U);
    fck_status_t info;
    fck_core_status(&info);
    TEST_ASSERT_FALSE(info.gnss_running);
}

TEST_CASE("a module back after a receiver nobody forgot: it starts, with no fix yet", "[settings]")
{
    sim_begin_fck(NULL, NULL);
    gnss_on();
    sim_unplug();
    TEST_ASSERT_TRUE(sim_wait_absent(5000U));
    listen_for_events();
    sim_plug(SIM_DEV_EXPANDER, false);
    TEST_ASSERT_TRUE(sim_wait_present(3000U));
    sim_sleep(1500U);     /* the C5 says what it runs, and the relay is set again */
    deafen();
    TEST_ASSERT_EQUAL_UINT(2U, g.n);
    expect(0U, FCK_AOS_EVENT_PRESENT, 0U);
    expect(1U, FCK_AOS_EVENT_SETTING_RESTORED, FCK_SETTING_GNSS);
    fck_gnss_fix_t fix;
    TEST_ASSERT_NOT_EQUAL(KODE_OK, fck_core_gnss_read(&fix, 0U));
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_gnss_end());
}

TEST_CASE("the listening is out of force while the module is gone and in force when it is back", "[settings]")
{
    sim_begin_fck(NULL, NULL);
    sim_cc1101_reset();
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_subghz_listen_start(HZ_433));
    uint32_t hz  = 0U;
    uint32_t gen = 0U;
    TEST_ASSERT_TRUE(fck_aos_setting_listening(&hz, &gen));
    TEST_ASSERT_EQUAL_UINT32(HZ_433, hz);
    listen_for_events();
    fck_status_t info;
    fck_core_status(&info);
    uint32_t const starts = sim_cc1101.listen_starts;
    uint32_t const slices = sim_cc1101.listens;
    sim_blip();
    TEST_ASSERT_TRUE(sim_wait_generation(info.generation + 1U, 4000U));
    sim_sleep(300U);
    deafen();
    TEST_ASSERT_EQUAL_UINT(4U, g.n);
    expect(0U, FCK_AOS_EVENT_GONE, 0U);
    expect(1U, FCK_AOS_EVENT_SETTING_LOST, FCK_SETTING_LISTEN);
    TEST_ASSERT_FALSE_MESSAGE(g.listening_when_lost, "in force with the module gone");
    expect(2U, FCK_AOS_EVENT_PRESENT, 0U);
    expect(3U, FCK_AOS_EVENT_SETTING_RESTORED, FCK_SETTING_LISTEN);
    uint32_t gen2 = 0U;
    TEST_ASSERT_TRUE(fck_aos_setting_listening(&hz, &gen2));
    TEST_ASSERT_NOT_EQUAL(gen, gen2);
    /* The owner listens again on its own, on the same frequency, from a fresh start: it does not
     * wait for the app to ask. */
    sim_sleep(400U);
    TEST_ASSERT_TRUE_MESSAGE(sim_cc1101.listen_starts > starts, "the hop state started over");
    TEST_ASSERT_EQUAL_UINT32(HZ_433, sim_cc1101.listen_hz);
    TEST_ASSERT_TRUE_MESSAGE(sim_cc1101.listens > slices, "the slices run again");
    /* Nothing was heard: the read says so and the listening stays in force. */
    int32_t            runs[8];
    size_t             n = 0U;
    fck_subghz_heard_t heard;
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_subghz_listen_read(0U, 0U, runs, 8U, &n, &heard));
    TEST_ASSERT_FALSE(heard.heard);
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_subghz_listen_stop());
}

TEST_CASE("a listening that was stopped, also with the module away, is not set again", "[settings]")
{
    sim_begin_fck(NULL, NULL);
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_subghz_listen_start(HZ_433));
    sim_unplug();
    TEST_ASSERT_TRUE(sim_wait_absent(5000U));
    listen_for_events();
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_subghz_listen_stop());      /* no module: it is forgotten all the same */
    sim_plug(SIM_DEV_EXPANDER, false);
    TEST_ASSERT_TRUE(sim_wait_present(3000U));
    sim_sleep(300U);
    deafen();
    TEST_ASSERT_EQUAL_UINT(1U, g.n);
    expect(0U, FCK_AOS_EVENT_PRESENT, 0U);
    uint32_t hz = 0U;
    uint32_t gen = 0U;
    TEST_ASSERT_FALSE(fck_aos_setting_listening(&hz, &gen));
}

TEST_CASE("a chip the app switched off is on again after the power loss: LOST, never RESTORED", "[settings]")
{
    sim_begin_fck(NULL, NULL);
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_power(FCK_NRF, false));
    listen_for_events();
    blip_and_wait();
    deafen();
    TEST_ASSERT_EQUAL_UINT(3U, g.n);
    expect(0U, FCK_AOS_EVENT_GONE, 0U);
    expect(1U, FCK_AOS_EVENT_SETTING_LOST, FCK_SETTING_NRF_POWER);
    expect(2U, FCK_AOS_EVENT_PRESENT, 0U);
    fck_status_t info;
    fck_core_status(&info);
    TEST_ASSERT_TRUE(info.nrf_on);
}

TEST_CASE("the app's switching a chip off and on again leaves nothing to lose", "[settings]")
{
    sim_begin_fck(NULL, NULL);
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_power(FCK_NRF, false));
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_power(FCK_NRF, true));
    listen_for_events();
    blip_and_wait();
    deafen();
    TEST_ASSERT_EQUAL_UINT(2U, g.n);
    expect(0U, FCK_AOS_EVENT_GONE, 0U);
    expect(1U, FCK_AOS_EVENT_PRESENT, 0U);
}

TEST_CASE("everything at once is one LOST with every bit, then RESTORED for the listening and for the receiver, and not for the chip's power", "[settings]")
{
    sim_begin_fck(NULL, NULL);
    gnss_on();
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_subghz_listen_start(HZ_433));
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_power(FCK_C5, false));
    listen_for_events();
    blip_and_wait();
    deafen();
    TEST_ASSERT_EQUAL_UINT(5U, g.n);
    expect(0U, FCK_AOS_EVENT_GONE, 0U);
    expect(1U, FCK_AOS_EVENT_SETTING_LOST, FCK_SETTING_GNSS | FCK_SETTING_LISTEN | FCK_SETTING_C5_POWER);
    expect(2U, FCK_AOS_EVENT_PRESENT, 0U);
    /* The listening at once; the receiver when the C5 relays it again. */
    expect(3U, FCK_AOS_EVENT_SETTING_RESTORED, FCK_SETTING_LISTEN);
    expect(4U, FCK_AOS_EVENT_SETTING_RESTORED, FCK_SETTING_GNSS);
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_gnss_end());
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_subghz_listen_stop());
}

TEST_CASE("restoring emits nothing and writes no chip: no send, no packet, no command to the chips", "[settings]")
{
    sim_begin_fck(NULL, NULL);
    gnss_on();
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_subghz_listen_start(HZ_433));
    sim_cc1101_reset();
    sim_sent_clear(SIM_C5);
    sim_sent_clear(SIM_NRF);
    blip_and_wait();
    TEST_ASSERT_EQUAL_UINT32(0U, sim_cc1101.sends);
    TEST_ASSERT_EQUAL_UINT32(0U, sim_cc1101.packet_sends);
    TEST_ASSERT_FALSE(sim_cc1101.on_air);
    /* The listening is put in force again and listens - receiving is all it does. */
    TEST_ASSERT_TRUE(sim_cc1101.listens > 0U);
    /* The one thing the C5 is told: the receiver's relay, which listens and sends nothing. */
    TEST_ASSERT_EQUAL_UINT(1U, sim_sent(SIM_C5, "gnss on"));
    /* Nothing else of the app's. */
    for (int chip = 0; chip < 2; chip++) {
        TEST_ASSERT_EQUAL_UINT(0U, sim_sent(chip, "rf"));
        TEST_ASSERT_EQUAL_UINT(0U, sim_sent(chip, "tx"));
        TEST_ASSERT_EQUAL_UINT(0U, sim_sent(chip, "echo"));
    }
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_gnss_end());
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_subghz_listen_stop());
}

TEST_CASE("fck_end forgets the settings: init and a power loss tell nothing of them", "[settings]")
{
    sim_begin_fck(NULL, NULL);
    gnss_on();
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_subghz_listen_start(HZ_433));
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_end());
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_begin());
    TEST_ASSERT_TRUE(sim_wait_present(3000U));
    sim_sleep(500U);
    uint32_t hz = 0U;
    uint32_t gen = 0U;
    TEST_ASSERT_FALSE(fck_aos_setting_listening(&hz, &gen));
    listen_for_events();
    blip_and_wait();
    deafen();
    TEST_ASSERT_EQUAL_UINT(2U, g.n);
    expect(0U, FCK_AOS_EVENT_GONE, 0U);
    expect(1U, FCK_AOS_EVENT_PRESENT, 0U);
}
