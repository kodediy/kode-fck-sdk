/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_17_hold.c
 * @brief A chip held by a task (fck_aos_hold(), as fck_radio_ping() and
 *        fck_gnss_begin() hold theirs for the length of the call) while
 *        another task ends the SDK: the chip is given back all the same.
 */
#include <stdint.h>

#include "unity.h"

#include "fck.h"
#include "fck_core.h"
#include "fck_aos.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "fake_world.h"

static volatile unsigned      s_step;      /* 1 held, 2 told to let go, 3 let go */
static volatile kode_status_t s_held;
static volatile kode_status_t s_given;

static void hold_task(void *unused)
{
    (void)unused;
    s_held = fck_aos_hold(FCK_AOS_NRF, 1000U);
    s_step = 1U;
    while (s_step < 2U) {
        vTaskDelay(1);
    }
    s_given = fck_aos_release(FCK_AOS_NRF);
    s_step  = 3U;
    vTaskDelete(NULL);
}

static bool held(void)
{
    return s_step == 1U;
}

static bool given(void)
{
    return s_step == 3U;
}

TEST_CASE("a chip held across the end is given back, and another task has it after the next init", "[end][hold]")
{
    sim_begin_fck(NULL, NULL);
    s_step  = 0U;
    s_held  = KODE_ERROR;
    s_given = KODE_ERROR;
    TEST_ASSERT_EQUAL(pdPASS, xTaskCreate(hold_task, "hold", 8192, NULL, 3, NULL));
    TEST_ASSERT_TRUE(sim_wait_for(held, 1000U));
    TEST_ASSERT_EQUAL(KODE_OK, s_held);

    /* The end, from this task, while the other holds the nRF. */
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_end());
    s_step = 2U;
    TEST_ASSERT_TRUE(sim_wait_for(given, 1000U));
    TEST_ASSERT_EQUAL_MESSAGE(KODE_OK, s_given, "the task that held the chip could not give it back after the end");

    TEST_ASSERT_EQUAL(KODE_OK, fck_core_begin());
    TEST_ASSERT_TRUE(sim_wait_present(3000U));
    TEST_ASSERT_EQUAL_MESSAGE(KODE_OK, fck_aos_hold(FCK_AOS_NRF, 200U), "the nRF stayed held by a task that is gone");
    TEST_ASSERT_EQUAL(KODE_OK, fck_aos_release(FCK_AOS_NRF));
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_end());
}
