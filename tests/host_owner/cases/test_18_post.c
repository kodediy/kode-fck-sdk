/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_18_post.c
 * @brief The commands posted to the owner and not waited for
 *        (fck_aos_command_post()): the extended layer's sniffer tune, two
 *        lines to the nRF's firmware, one after the other.
 */
#include <stdint.h>
#include <string.h>

#include "unity.h"

#include "fck.h"
#include "fck_core.h"
#include "fck_aos.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "fake_world.h"

#define SCFG_REPLY  "#SCFG24 rate=1M crc=0 len=32\r\n#DONE rf_scfg rc=0\r\n"
#define SADDR_REPLY "#SADDR24 balen=2 addr_bytes=3 base=0xaaaa prefix=0xaa\r\n#DONE rf_saddr rc=0\r\n"

static const fck_radio_sniff_config_t CFG = { 1U, 0U, 32U, 2U, 0xAAAAAAAAU, 0xAAU };

static void begin(uint32_t scfg_ms)
{
    sim_begin_fck(NULL, NULL);
    sim_script(SIM_NRF, "rf scfg", SCFG_REPLY, scfg_ms);
    sim_script(SIM_NRF, "rf saddr", SADDR_REPLY, 5U);
    fck_firmware_t fw;
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_firmware(FCK_NRF, &fw));
    sim_sent_clear(SIM_NRF);
}

static bool both_sent(void)
{
    return (sim_sent(SIM_NRF, "rf scfg") >= 1U) && (sim_sent(SIM_NRF, "rf saddr") >= 1U);
}

static bool both_sent_twice(void)
{
    return (sim_sent(SIM_NRF, "rf scfg") >= 2U) && (sim_sent(SIM_NRF, "rf saddr") >= 2U);
}

TEST_CASE("the sniffer's tune, posted, sends both of its lines to the nRF", "[post]")
{
    begin(5U);
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_radio_sniff_tune_post(&CFG));
    (void)sim_wait_for(both_sent, 1000U);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, sim_sent(SIM_NRF, "rf scfg"), "the first line, the sniffer's PHY");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, sim_sent(SIM_NRF, "rf saddr"), "the second line, the address it locks on to");
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_end());
}

TEST_CASE("two tunes posted back to back send their four lines, in the order they came", "[post]")
{
    begin(5U);
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_radio_sniff_tune_post(&CFG));
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_radio_sniff_tune_post(&CFG));
    (void)sim_wait_for(both_sent_twice, 1000U);
    TEST_ASSERT_EQUAL_UINT(2U, sim_sent(SIM_NRF, "rf scfg"));
    TEST_ASSERT_EQUAL_UINT(2U, sim_sent(SIM_NRF, "rf saddr"));
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, strncmp(sim_sent_line(SIM_NRF, 0U), "rf scfg", 7U), "the first line sent");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, strncmp(sim_sent_line(SIM_NRF, 1U), "rf saddr", 8U), "the second");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, strncmp(sim_sent_line(SIM_NRF, 2U), "rf scfg", 7U), "the third");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, strncmp(sim_sent_line(SIM_NRF, 3U), "rf saddr", 8U), "the fourth");
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_end());
}

TEST_CASE("a posted line that waits for the chip is let go of when the module leaves, and never sent", "[post]")
{
    begin(400U);     /* the first line holds the chip for long enough */
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_radio_sniff_tune_post(&CFG));
    vTaskDelay(pdMS_TO_TICKS(50));
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, sim_sent(SIM_NRF, "rf scfg"), "the first line is with the chip");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, sim_sent(SIM_NRF, "rf saddr"), "the second waits for the first to end");
    sim_unplug();
    vTaskDelay(pdMS_TO_TICKS(700));
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, sim_sent(SIM_NRF, "rf saddr"), "a line that waited was sent to a module that left");
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_end());
}

TEST_CASE("more posted lines than the owner keeps are dropped, and the log says so", "[post]")
{
    begin(400U);
    /* One line with the chip and FCK_AOS_POSTED_MAX waiting: the tune after that does not fit. */
    for (unsigned i = 0U; i < 1U + (FCK_AOS_POSTED_MAX / 2U); i++) {
        TEST_ASSERT_EQUAL(KODE_OK, fck_core_radio_sniff_tune_post(&CFG));
    }
    vTaskDelay(pdMS_TO_TICKS(50));
    TEST_ASSERT_TRUE_MESSAGE(sim_log_has("a posted `rf_saddr` failed"), "the line that did not fit went without a word");
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_end());
}
