/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_11_error.c
 * @brief fck_error(): the reason of a task's last failure, per task, a literal,
 *        and a code that is returned as it is (a timeout is a timeout).
 */
#include <string.h>

#include "unity.h"

#include "fck.h"
#include "fck_core.h"
#include "fck_aos.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "fake_world.h"

typedef struct {
    volatile bool        done;
    volatile bool        go;
    const char *volatile why;
    volatile kode_status_t rc;
    int                  which;
} who_t;

/* Twice the table and one more: enough tasks that the table must go round all of it. */
#define WHO (2U * FCK_AOS_ERROR_TASKS + 1U)
static who_t s_who[WHO];

static bool all_done(void)
{
    for (unsigned i = 0; i < WHO; i++) {
        if (!s_who[i].done) {
            return false;
        }
    }
    return true;
}

/* Fails with a reason of its own, waits for the signal, reads what it is told. */
static void failing_task(void *arg)
{
    who_t *const w = (who_t *)arg;
    int32_t      n = 0;
    switch (w->which) {
    case 0:
        w->rc = fck_core_command(FCK_C5, NULL, "x", NULL, NULL, 100U, &n);                  /* line is NULL   */
        break;
    case 1:
        w->rc = fck_core_radio_sniff(2476U, 1U, -60, NULL, 0U, NULL, NULL);                 /* out is NULL    */
        break;
    case 2:
        w->rc = fck_core_subghz_frequencies(NULL, 0U, NULL);                                /* hz is NULL     */
        break;
    case 3:
        w->rc = fck_core_firmware((fck_chip_t)9, NULL);                                        /* bad chip       */
        break;
    default:
        w->rc = fck_core_flash((fck_chip_t)9, NULL, 0U, NULL);                              /* bad chip       */
        break;
    }
    while (!w->go) {
        vTaskDelay(1);
    }
    w->why  = fck_error();
    w->done = true;
    vTaskDelete(NULL);
}

TEST_CASE("each task reads the reason of its own last failure", "[error]")
{
    sim_begin_fck(NULL, NULL);
    (void)memset((void *)s_who, 0, sizeof(s_who));
    for (int i = 0; i < 4; i++) {
        s_who[i].which = i;
        TEST_ASSERT_EQUAL(pdPASS, xTaskCreate(failing_task, "failing", 4096, &s_who[i], 3, NULL));
        sim_sleep(30U);
    }
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_subghz_frequencies(NULL, 0U, NULL));
    const char *const mine = fck_error();
    for (int i = 0; i < 4; i++) {
        s_who[i].go = true;
    }
    for (unsigned i = 4U; i < WHO; i++) {
        s_who[i].done = true;
    }
    TEST_ASSERT_TRUE(sim_wait_for(all_done, 2000U));
    for (int i = 0; i < 4; i++) {
        TEST_ASSERT_EQUAL(KODE_DENIED, s_who[i].rc);
        TEST_ASSERT_TRUE_MESSAGE(strlen(s_who[i].why) > 10U, "a refusal with no reason");
    }
    TEST_ASSERT_TRUE(strstr(s_who[0].why, "line is NULL") != NULL);
    TEST_ASSERT_TRUE(strstr(s_who[1].why, "out is NULL") != NULL);
    TEST_ASSERT_TRUE(strstr(s_who[2].why, "hz or n is NULL") != NULL);
    TEST_ASSERT_TRUE(strstr(s_who[3].why, "chip is not FCK_C5 or FCK_NRF") != NULL);
    /* The case's own task was not touched by theirs. */
    TEST_ASSERT_TRUE(strstr(mine, "hz or n is NULL") != NULL);
    TEST_ASSERT_EQUAL_STRING(mine, fck_error());
}

TEST_CASE("one task more than are kept takes the place of the one that failed longest ago; the others keep theirs", "[error]")
{
    sim_begin_fck(NULL, NULL);
    (void)memset((void *)s_who, 0, sizeof(s_who));
    /* The case's own task is among those kept: it fails first. */
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_flash((fck_chip_t)9, NULL, 0U, NULL));
    /* Twice as many tasks as the table keeps, each failing as it starts. A new task can be given
     * the handle of one that has ended and take its place without pushing anyone out, so one table
     * of tasks may not be enough: with twice as many, the places handed round cover every slot,
     * the case's among them. */
    for (unsigned i = 0; i < WHO - 1U; i++) {
        s_who[i].which = (int)(i % 4U);
        TEST_ASSERT_EQUAL(pdPASS, xTaskCreate(failing_task, "failing", 4096, &s_who[i], 3, NULL));
        sim_sleep(30U);
    }
    for (unsigned i = 0; i < WHO - 1U; i++) {
        s_who[i].go = true;
    }
    s_who[WHO - 1U].done = true;
    TEST_ASSERT_TRUE(sim_wait_for(all_done, 6000U));
    /* The case's reason was pushed out; the last tasks to fail keep theirs. */
    for (unsigned i = WHO - 5U; i < WHO - 1U; i++) {
        TEST_ASSERT_TRUE_MESSAGE(strlen(s_who[i].why) > 10U, "a task that failed lost its reason");
    }
    TEST_ASSERT_EQUAL_STRING("", fck_error());
}

TEST_CASE("a call that succeeds leaves the reason as it was", "[error]")
{
    sim_begin_fck(NULL, NULL);
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_firmware((fck_chip_t)9, NULL));
    const char *const was = fck_error();
    TEST_ASSERT_TRUE(strlen(was) > 10U);
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_wait_present(0U));
    TEST_ASSERT_EQUAL_STRING(was, fck_error());
}

TEST_CASE("after fck_end every call says the module is not set up", "[error]")
{
    sim_begin_fck(NULL, NULL);
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_end());
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_wait_present(10U));
    TEST_ASSERT_TRUE(strstr(fck_error(), "fck_begin() has not run") != NULL);
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_cancel());
    TEST_ASSERT_TRUE(strstr(fck_error(), "fck_begin() has not run") != NULL);
}
