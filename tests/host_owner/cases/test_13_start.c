/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_13_start.c
 * @brief What fck_core_begin() and the typed calls refuse or say, at the edges: a supply that cannot
 *        be switched on, a cap that is not a sane size, a chip that answers off the limits.
 */
#include <stdint.h>
#include <string.h>

#include "unity.h"

#include "fck.h"
#include "fck_core.h"
#include "fck_aos.h"

#include "fake_world.h"

static uint8_t j3_now(void)
{
    fck_aos_status_t st;
    fck_aos_status(&st);
    return st.j3_supply;
}

static bool j3_is_on(void)       { return j3_now() == FCK_AOS_J3_ON; }
static bool j3_is_failed(void)   { return j3_now() == FCK_AOS_J3_FAILED; }
static bool j3_is_no_link(void)  { return j3_now() == FCK_AOS_J3_NO_LINK; }
static bool j3_is_always(void)   { return j3_now() == FCK_AOS_J3_ALWAYS; }

TEST_CASE("J3's supply is asked for once at the start, the owner does not wait for it, and the status says it is on", "[init][j3]")
{
    sim_begin_fck(NULL, NULL);
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_end());
    TEST_ASSERT_EQUAL_UINT8(FCK_AOS_J3_OFF, j3_now());
    TEST_ASSERT_FALSE(sim_exp_power_on());
    uint32_t const calls = sim_exp_power_calls();
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_begin());
    TEST_ASSERT_TRUE_MESSAGE(sim_wait_for(j3_is_on, 2000U), "EXP_POWER_SIG's OK did not reach the status");
    TEST_ASSERT_TRUE(sim_exp_power_on());
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(calls + 1U, sim_exp_power_calls(), "asked once, not every tick");
    sim_plug(SIM_DEV_EXPANDER, false);
    TEST_ASSERT_TRUE(sim_wait_present(3000U));
}

TEST_CASE("a supply the Dot's C5 refused is FAILED, said once, and the module is found all the same", "[init][j3]")
{
    sim_begin_fck(NULL, NULL);
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_end());
    sim_log_clear();
    sim_exp_power_answers(KODE_IO_ERROR);
    TEST_ASSERT_EQUAL_MESSAGE(KODE_OK, fck_core_begin(), "the start does not wait for the supply, so it does not fail for it");
    TEST_ASSERT_TRUE_MESSAGE(sim_wait_for(j3_is_failed, 2000U), "the refusal did not reach the status");
    TEST_ASSERT_FALSE(sim_exp_power_on());
    TEST_ASSERT_TRUE(sim_log_has("the wish was dropped"));
    /* A module powered by R72 is found whatever the switch said. */
    sim_plug(SIM_DEV_EXPANDER, false);
    TEST_ASSERT_TRUE_MESSAGE(sim_wait_present(3000U), "the looks stopped when the supply failed");
    TEST_ASSERT_EQUAL_UINT8(FCK_AOS_J3_FAILED, j3_now());
}

TEST_CASE("a Dot whose C5 link is not up leaves the wish standing: NO_LINK, and nothing fails", "[init][j3]")
{
    sim_begin_fck(NULL, NULL);
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_end());
    sim_log_clear();
    sim_exp_power_answers(KODE_DENIED);
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_begin());
    TEST_ASSERT_TRUE(sim_wait_for(j3_is_no_link, 2000U));
    TEST_ASSERT_TRUE(sim_log_has("the next link carries it out"));
    sim_plug(SIM_DEV_EXPANDER, false);
    TEST_ASSERT_TRUE(sim_wait_present(3000U));
}

TEST_CASE("an image with no radio: J3 is always powered, the status says so and fck_end() cannot switch it off", "[init][j3]")
{
    sim_begin_fck(NULL, NULL);
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_end());
    sim_exp_no_radio(true);
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_begin());
    TEST_ASSERT_TRUE(sim_wait_for(j3_is_always, 2000U));
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_end());
    TEST_ASSERT_EQUAL_UINT8(FCK_AOS_J3_ALWAYS, j3_now());
}

TEST_CASE("a radio owner whose queue is short is asked again at the next tick, until it takes the command", "[init][j3]")
{
    sim_begin_fck(NULL, NULL);
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_end());
    uint32_t const calls = sim_exp_power_calls();
    sim_exp_power_busy(3U);
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_begin());
    TEST_ASSERT_TRUE(sim_wait_for(j3_is_on, 3000U));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(calls + 4U, sim_exp_power_calls(), "three refusals and the fourth taken");
}

TEST_CASE("fck_end switches the supply off and gives J3 back; a radio owner short of room is asked again", "[init][j3]")
{
    sim_begin_fck(NULL, NULL);
    uint32_t const returns = sim_exp_returns();
    sim_exp_power_busy(5U);
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_end());
    TEST_ASSERT_EQUAL_UINT8(FCK_AOS_J3_OFF, j3_now());
    TEST_ASSERT_FALSE(sim_exp_power_on());
    TEST_ASSERT_EQUAL_UINT32(returns + 1U, sim_exp_returns());
}

TEST_CASE("J3 taken by the console: the start says so, gives nothing back that it did not take, and the owner stays idle", "[init]")
{
    sim_begin_fck(NULL, NULL);
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_end());
    sim_exp_lend_answers(KODE_EXISTS);
    TEST_ASSERT_EQUAL(KODE_EXISTS, fck_core_begin());
    TEST_ASSERT_NOT_NULL(strstr(fck_error(), "console took it"));
    fck_firmware_t a;
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_firmware(FCK_C5, &a));           /* nothing runs */
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_wait_present(10U));
    sim_wire_clear();
    sim_plug(SIM_DEV_EXPANDER, false);
    sim_sleep(1500U);                                   /* more than one look of the owner's */
    TEST_ASSERT_FALSE_MESSAGE(fck_is_present(), "an owner that was let go of found the module");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, sim_wire_n(), "an owner that was let go of drove J3's bus");
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_end());
    /* The console let go: the next start takes it. */
    sim_exp_lend_answers(KODE_OK);
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_begin());
    TEST_ASSERT_TRUE_MESSAGE(sim_wait_present(3000U), "the module was not found after the retry");
}

TEST_CASE("fck_wifi_channels: a cap above FCK_WIFI_CHANNELS_CAP_MAX is refused before anything is allocated or sent", "[wifi][cap]")
{
    sim_begin_fck(NULL, NULL);
    sim_sent_clear(SIM_C5);
    static fck_wifi_channel_t ch[FCK_WIFI_CHANNELS_CAP_MAX + 1U];
    size_t                    n = 7U;
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_wifi_channels(FCK_BAND_ALL, 100U, ch, FCK_WIFI_CHANNELS_CAP_MAX + 1U, &n));
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_wifi_channels(FCK_BAND_ALL, 100U, ch, (size_t)-1, &n));
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_wifi_channels(FCK_BAND_2G, 100U, ch, SIZE_MAX / sizeof(ch[0]) + 1U, &n));
    TEST_ASSERT_NOT_NULL(strstr(fck_error(), "FCK_WIFI_CHANNELS_CAP_MAX"));
    TEST_ASSERT_EQUAL_UINT(7U, n);
    TEST_ASSERT_EQUAL_UINT(0U, sim_sent(SIM_C5, "chan"));
    /* The cap itself is allowed: it reaches the firmware. */
    sim_script(SIM_C5, "chan", "#DONE chan rc=0\r\n", 5U);
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_wifi_channels(FCK_BAND_ALL, 100U, ch, FCK_WIFI_CHANNELS_CAP_MAX, &n));
    TEST_ASSERT_EQUAL_UINT(1U, sim_sent(SIM_C5, "chan"));
    TEST_ASSERT_EQUAL_UINT(0U, n);
}

TEST_CASE("a power step above the level asked is an I/O error of the firmware, and nothing goes on the air", "[radio][power]")
{
    sim_begin_fck(NULL, NULL);
    /* A model of a firmware that answers a step above the level: nothing is transmitted by it. */
    sim_script(SIM_NRF, "rf power", "#POWER24 req=0 dbm=6\r\n#DONE rf_power rc=0\r\n", 5U);
    sim_sent_clear(SIM_NRF);
    fck_radio_ping_statistics_t st;
    TEST_ASSERT_EQUAL(KODE_IO_ERROR, fck_core_radio_ping(1U, 2440U, 10U, 0, &st));
    TEST_ASSERT_NOT_NULL(strstr(fck_error(), "power step"));
    TEST_ASSERT_EQUAL_UINT(1U, sim_sent(SIM_NRF, "rf power"));
    TEST_ASSERT_EQUAL_UINT(0U, sim_sent(SIM_NRF, "rf ping"));
    TEST_ASSERT_EQUAL(KODE_IO_ERROR, fck_core_radio_echo_start(2440U, 0));
    TEST_ASSERT_EQUAL_UINT(0U, sim_sent(SIM_NRF, "rf echo"));
}
