/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_9_waits.c
 * @brief What blocks, and for how long: nothing from an Active Object's task,
 *        nothing without a bound, and a stuck owner is a TIMEOUT.
 *
 * "An Active Object's task" is marked with sim_ao_mark_task(), as QP/C's table
 * says it in the image. A stuck owner is sim_ao_stall(); the suite shortens
 * the bounds (main/CMakeLists.txt: FCK_AOS_JOB_MS_MAX 1500 ms, FCK_AOS_ANSWER_GRACE_MS
 * 200 ms), so a request not answered is given up after its own time plus 3.2 s.
 */
#include <string.h>

#include "unity.h"

#include "fck.h"
#include "fck_core.h"
#include "fck_aos.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "fake_world.h"

#define GIVE_UP_MS  (3000U + 200U)   /* 2 * FCK_AOS_JOB_MS_MAX + FCK_AOS_ANSWER_GRACE_MS */

typedef struct {
    kode_status_t job, hold, wait, command, fix, stop, run, c5_on;
    uint32_t      took_ms;
    volatile bool done;
} refused_t;

static refused_t s_r;

static kode_status_t nothing(void *arg, const fck_aos_job_context_t *job)
{
    (void)arg;
    (void)job;
    return KODE_OK;
}

/* Every call that blocks, made on the task of an app's Active Object. */
static void ao_task(void *unused)
{
    (void)unused;
    sim_ao_mark_task(xTaskGetCurrentTaskHandle());
    uint32_t const t0 = sim_now_ms();
    fck_drv_nmea_fix_t fix;
    int32_t rc = 0;
    s_r.job     = fck_aos_run_job(nothing, NULL, 5000U);
    s_r.hold    = fck_aos_hold(FCK_AOS_C5, 5000U);
    s_r.wait    = fck_aos_wait_present(5000U);
    s_r.command = fck_aos_command(FCK_AOS_C5, "info", "info", NULL, NULL, 5000U, &rc);
    s_r.fix     = fck_aos_gnss_fix(5000U, &fix, NULL);
    s_r.stop    = fck_aos_stop();
    s_r.c5_on   = fck_aos_power(FCK_AOS_C5, true);
    s_r.took_ms = sim_now_ms() - t0;
    sim_ao_mark_task(NULL);
    s_r.done = true;
    vTaskDelete(NULL);
}

static bool refused_done(void)
{
    return s_r.done;
}

TEST_CASE("a call that blocks, from an Active Object's task, is refused at once with KODE_DENIED", "[waits]")
{
    sim_begin_fck(NULL, NULL);
    memset(&s_r, 0, sizeof(s_r));
    TEST_ASSERT_EQUAL(pdPASS, xTaskCreate(ao_task, "app_ao", 8192, NULL, 3, NULL));
    TEST_ASSERT_TRUE_MESSAGE(sim_wait_for(refused_done, 2000U), "a call blocked an Active Object's task");
    TEST_ASSERT_EQUAL(KODE_DENIED, s_r.job);
    TEST_ASSERT_EQUAL(KODE_DENIED, s_r.hold);
    TEST_ASSERT_EQUAL(KODE_DENIED, s_r.wait);
    TEST_ASSERT_EQUAL(KODE_DENIED, s_r.command);
    TEST_ASSERT_EQUAL(KODE_DENIED, s_r.fix);
    TEST_ASSERT_EQUAL(KODE_DENIED, s_r.stop);
    TEST_ASSERT_EQUAL(KODE_DENIED, s_r.c5_on);
    TEST_ASSERT_TRUE_MESSAGE(s_r.took_ms < 200U, "the refusals waited");
    /* The module is as it was: the refused fck_aos_stop() stopped nothing. */
    TEST_ASSERT_EQUAL(KODE_OK, fck_aos_wait_present(0U));
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_end());
}

TEST_CASE("fck_aos_wait_present(0), which does not wait, is the same from an Active Object's task as from any", "[waits]")
{
    sim_begin_fck(NULL, NULL);
    sim_ao_mark_task(xTaskGetCurrentTaskHandle());
    kode_status_t const rc = fck_aos_wait_present(0U);
    sim_ao_mark_task(NULL);
    TEST_ASSERT_TRUE(rc == KODE_OK || rc == KODE_TIMEOUT);
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_end());
}

static volatile kode_status_t s_stuck_rc;
static volatile uint32_t      s_stuck_ms;
static volatile bool          s_stuck_done;

static void stuck_task(void *unused)
{
    (void)unused;
    int32_t        rc = 0;
    uint32_t const t0 = sim_now_ms();
    s_stuck_rc   = fck_aos_command(FCK_AOS_C5, "wifi scan", "wifi", NULL, NULL, 100U, &rc);
    s_stuck_ms   = sim_now_ms() - t0;
    s_stuck_done = true;
    vTaskDelete(NULL);
}

static bool stuck_done(void)
{
    return s_stuck_done;
}

TEST_CASE("an owner that does not answer: the caller gives up with KODE_TIMEOUT after the bound, and the owner then frees what is left", "[waits]")
{
    sim_begin_fck(NULL, NULL);
    s_stuck_done = false;
    s_stuck_rc   = KODE_ERROR;
    size_t const sent_before = sim_sent(SIM_C5, "wifi");
    sim_ao_stall(true);
    TEST_ASSERT_EQUAL(pdPASS, xTaskCreate(stuck_task, "stuck", 8192, NULL, 3, NULL));
    TEST_ASSERT_TRUE_MESSAGE(sim_wait_for(stuck_done, GIVE_UP_MS + 3000U), "the caller never gave up");
    TEST_ASSERT_EQUAL(KODE_TIMEOUT, s_stuck_rc);
    TEST_ASSERT_TRUE(s_stuck_ms >= 100U + GIVE_UP_MS - 50U);
    TEST_ASSERT_TRUE(s_stuck_ms < 100U + GIVE_UP_MS + 1500U);
    /* The owner wakes and finds a request nobody waits for: it neither sends it nor touches it. */
    sim_ao_stall(false);
    sim_sleep(300U);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(sent_before, sim_sent(SIM_C5, "wifi"), sim_sent_line(SIM_C5, sim_sent(SIM_C5, "") - 1U));
    /* And it is the same owner: the next call is served. */
    fck_aos_firmware_t firmware;
    TEST_ASSERT_EQUAL(KODE_OK, fck_aos_firmware(FCK_AOS_C5, 2000U, &firmware));
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_end());
}
