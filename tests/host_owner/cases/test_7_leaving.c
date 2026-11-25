/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_7_leaving.c
 * @brief The app leaving - HOME, sleep, a restart, fck_core_end() - while a send is
 *        on the air: nothing is left transmitting, the CC1101 is at rest
 *        before the C5 is held, and nothing new is started.
 *
 * The CC1101 is the model's (fake_cc1101.c): a send keeps its carrier on for
 * sim_cc1101.play_ms in 5 ms steps, as a send of up to 3 s does, and ends
 * within one step of fck_drv_cc1101_abort(). kodeOS gives HOME_SIG 200 ms
 * (KODE_HOME_GRACE_MS) before it resets. A case that leaves puts the module
 * down with fck_core_end() at the end, so that the next starts afresh.
 */
#include <string.h>

#include "unity.h"

#include "fck.h"
#include "fck_core.h"
#include "fck_aos.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "fake_world.h"

#define HZ_433        433920000U
#define HOUR_MS       3600000U
#define GRACE_MS      200U      /* kodeOS's, from HOME_SIG to the reset      */
#define PARK_WAIT_MS  250U      /* the restart hook's patience (fck_aos.c)   */

/* A frame of 2.8 ms, repeated: the duty account is charged 28 ms. */
static const int32_t k_frame[4] = { 350, -1050, 1050, -350 };

static volatile kode_status_t s_rc;
static volatile fck_aos_why_t s_why;           /* the class of the send's failure  */
static volatile unsigned      s_cancelled;     /* SEND_CANCELLED told              */
static volatile uint32_t      s_cancel_cause;  /* and what it said cut the send    */

static void note_cancel(fck_aos_event_t ev, uint32_t arg, void *ctx)
{
    (void)ctx;
    if (ev == FCK_AOS_EVENT_SEND_CANCELLED) {
        s_cancelled++;
        s_cancel_cause = arg;
    }
}
static volatile bool          s_done;

static void begin(void)
{
    sim_begin_fck(NULL, NULL);
    sim_cc1101_reset();
    sim_clock_skip_ms(2U * HOUR_MS);
    sim_parks_clear();
    s_done = false;
    s_rc   = KODE_ERROR;
    s_why  = FCK_AOS_WHY_NONE;
    s_cancelled    = 0U;
    s_cancel_cause = 0U;
    sim_on_event(note_cancel, NULL);
}

static void send_task(void *unused)
{
    (void)unused;
    s_rc   = fck_core_subghz_send_raw(HZ_433, k_frame, 4U, 10U);
    s_why  = fck_aos_error_why();      /* a task's own: read where the call failed */
    s_done = true;
    vTaskDelete(NULL);
}

static void hook_task(void *unused)
{
    (void)unused;
    fck_drv_system_fn const fn = sim_restart_hook();
    if (fn != NULL) {
        fn();
    }
    s_done = true;
    vTaskDelete(NULL);
}

static bool on_air(void)
{
    return sim_cc1101.on_air;
}

static bool done(void)
{
    return s_done;
}

static bool off_air(void)
{
    return !sim_cc1101.on_air;
}

/* A send of 800 ms, begun and on the air. */
static void send_long(void)
{
    sim_cc1101.play_ms = 800U;
    TEST_ASSERT_EQUAL(pdPASS, xTaskCreate(send_task, "send", 8192, NULL, 3, NULL));
    TEST_ASSERT_TRUE_MESSAGE(sim_wait_for(on_air, 1000U), "the send never reached the radio");
}

static bool c5_held(void)
{
    return sim_c5_parks() > 0U;
}

static bool power_is_on(void)
{
    return sim_exp_power_on();
}

static bool c5_rested(void)
{
    return sim_c5_holds() > 0U;
}

TEST_CASE("HOME with a send on the air: the carrier ends within kodeOS's grace, then the CC1101 sleeps, then the C5 is held", "[leaving]")
{
    begin();
    send_long();
    uint32_t const asked = sim_now_ms();
    sim_ao_home();
    TEST_ASSERT_TRUE_MESSAGE(sim_wait_for(off_air, 1000U), "the carrier is still on");
    TEST_ASSERT_TRUE_MESSAGE((sim_cc1101.idle_at_ms - asked) < GRACE_MS, "the carrier outlasted kodeOS's grace");
    TEST_ASSERT_TRUE(sim_wait_for(done, 1000U));
    TEST_ASSERT_EQUAL(KODE_IO_ERROR, s_rc);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE((uint32_t)FCK_AOS_WHY_CANCELLED, (uint32_t)s_why,
                                     "a send cut by the way home is cancelled, not a module that failed");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, s_cancelled, "the send that was cut is told as SEND_CANCELLED, once");
    TEST_ASSERT_EQUAL_UINT32(FCK_AOS_CANCEL_LEAVING, s_cancel_cause);
    TEST_ASSERT_EQUAL_UINT32(1U, sim_cc1101.cut);
    TEST_ASSERT_TRUE(sim_wait_for(c5_held, 1000U));
    TEST_ASSERT_TRUE_MESSAGE(sim_cc1101_sleeps() >= 1U, "the CC1101 was not put to sleep");
    TEST_ASSERT_TRUE_MESSAGE((int32_t)(sim_c5_park_ms() - sim_cc1101_sleep_ms()) >= 0,
                             "the C5 was held before the CC1101 was at rest");
    TEST_ASSERT_TRUE((int32_t)(sim_cc1101_sleep_ms() - sim_cc1101.idle_at_ms) >= 0);
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_end());
}

TEST_CASE("sleep with a send on the air ends it just the same", "[leaving]")
{
    begin();
    send_long();
    uint32_t const asked = sim_now_ms();
    sim_ao_sleep();
    TEST_ASSERT_TRUE_MESSAGE(sim_wait_for(off_air, 1000U), "the carrier is still on");
    TEST_ASSERT_TRUE((sim_cc1101.idle_at_ms - asked) < GRACE_MS);
    TEST_ASSERT_TRUE(sim_wait_for(done, 1000U));
    TEST_ASSERT_EQUAL(KODE_IO_ERROR, s_rc);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE((uint32_t)FCK_AOS_WHY_CANCELLED, (uint32_t)s_why,
                                     "a send cut by a rest is cancelled, not a module that failed");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, s_cancelled, "the send that was cut is told as SEND_CANCELLED, once");
    TEST_ASSERT_EQUAL_UINT32(FCK_AOS_CANCEL_LEAVING, s_cancel_cause);
    TEST_ASSERT_TRUE(sim_wait_for(c5_rested, 1000U));
    TEST_ASSERT_TRUE((int32_t)(sim_c5_hold_ms() - sim_cc1101_sleep_ms()) >= 0);
    /* It is told as the module leaving, and the board waking brings it back. */
    TEST_ASSERT_FALSE(fck_is_present());
    sim_ao_wake();
    sim_plug(SIM_DEV_EXPANDER, false);
    TEST_ASSERT_TRUE_MESSAGE(sim_wait_present(4000U), "the module was not looked for again after the wake");
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_end());
}

TEST_CASE("the restart hook with a send on the air ends it, and holds the C5 after the CC1101 rests, before its own patience runs out", "[leaving]")
{
    begin();
    send_long();
    uint32_t const asked = sim_now_ms();
    TEST_ASSERT_EQUAL(pdPASS, xTaskCreate(hook_task, "hook", 8192, NULL, 3, NULL));
    s_done = false;
    TEST_ASSERT_TRUE_MESSAGE(sim_wait_for(off_air, 1000U), "the carrier is still on");
    TEST_ASSERT_TRUE_MESSAGE((sim_cc1101.idle_at_ms - asked) < GRACE_MS, "the carrier outlasted kodeOS's grace");
    TEST_ASSERT_TRUE(sim_wait_for(c5_held, 1000U));
    TEST_ASSERT_TRUE_MESSAGE((sim_c5_park_ms() - asked) < PARK_WAIT_MS,
                             "the C5 was held by the hook's timeout, not by the park");
    TEST_ASSERT_TRUE_MESSAGE((int32_t)(sim_c5_park_ms() - sim_cc1101_sleep_ms()) >= 0,
                             "the C5 was held before the CC1101 was at rest");
    TEST_ASSERT_TRUE_MESSAGE((int32_t)(sim_cc1101_sleep_ms() - sim_cc1101.idle_at_ms) >= 0,
                             "the CC1101 was put to sleep with the carrier still on");
    TEST_ASSERT_EQUAL_UINT32(1U, sim_c5_parks());
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_end());
}

TEST_CASE("after a way home nothing new is started: a send is refused and keys nothing", "[leaving]")
{
    begin();
    sim_ao_home();
    TEST_ASSERT_TRUE(sim_wait_for(c5_held, 1000U));
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_subghz_send_raw(HZ_433, k_frame, 4U, 1U));
    TEST_ASSERT_EQUAL_UINT32(0U, sim_cc1101.sends);
    TEST_ASSERT_EQUAL_UINT32(0U, sim_cc1101.packet_sends);
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_end());
}

static kode_status_t slow_job(void *arg, const fck_aos_job_context_t *job)
{
    (void)arg;
    (void)job;
    vTaskDelay(pdMS_TO_TICKS(400));
    return KODE_OK;
}

static void job_task(void *unused)
{
    (void)unused;
    s_rc   = fck_aos_run_job(slow_job, NULL, 2000U);
    s_done = true;
    vTaskDelete(NULL);
}

TEST_CASE("a wake while a way home waits for a job does not undo it: the cut stays, and the module is parked", "[leaving]")
{
    begin();
    TEST_ASSERT_EQUAL(pdPASS, xTaskCreate(job_task, "job", 8192, NULL, 3, NULL));
    vTaskDelay(pdMS_TO_TICKS(100));      /* the job has the worker */
    sim_ao_home();
    vTaskDelay(pdMS_TO_TICKS(50));
    TEST_ASSERT_TRUE_MESSAGE(fck_drv_cc1101_aborted(), "the way home did not cut the air");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0U, sim_c5_parks(), "the job is in the way: not parked yet");
    sim_ao_wake();
    vTaskDelay(pdMS_TO_TICKS(50));
    TEST_ASSERT_TRUE_MESSAGE(fck_drv_cc1101_aborted(), "a wake lifted the cut of a way home that waits for its job");
    TEST_ASSERT_TRUE(sim_wait_for(done, 2000U));
    TEST_ASSERT_TRUE_MESSAGE(sim_wait_for(c5_held, 1000U), "the module was not parked when the job ended");
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_subghz_send_raw(HZ_433, k_frame, 4U, 1U));
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_end());
}

/* ---- fck_core_end() --------------------------------------------------------------------- */

TEST_CASE("end puts the module down, switches the supply off and gives J3 back; init takes it again", "[end]")
{
    begin();
    uint32_t const returns = sim_exp_returns();
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_end());
    TEST_ASSERT_FALSE(fck_is_present());
    TEST_ASSERT_FALSE(sim_exp_power_on());
    TEST_ASSERT_EQUAL_UINT32(returns + 1U, sim_exp_returns());
    TEST_ASSERT_FALSE(sim_gnss_running());
    TEST_ASSERT_FALSE(sim_chip_running(SIM_C5));
    /* Every call answers, none waits. */
    fck_firmware_t a;
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_firmware(FCK_C5, &a));
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_wait_present(100U));
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_power(FCK_NRF, true));
    /* Again does nothing. */
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_end());
    TEST_ASSERT_EQUAL_UINT32(returns + 1U, sim_exp_returns());
    /* And the module is looked for afresh. */
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_begin());
    TEST_ASSERT_TRUE_MESSAGE(sim_wait_for(power_is_on, 2000U), "the supply was not asked for again");
    sim_plug(SIM_DEV_EXPANDER, false);
    TEST_ASSERT_TRUE_MESSAGE(sim_wait_present(3000U), "the module was not found again");
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_end());
}

TEST_CASE("end with a send on the air cuts it; a send after init goes again", "[end]")
{
    begin();
    send_long();
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_end());
    TEST_ASSERT_TRUE(sim_wait_for(done, 1000U));
    TEST_ASSERT_EQUAL(KODE_IO_ERROR, s_rc);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE((uint32_t)FCK_AOS_WHY_CANCELLED, (uint32_t)s_why,
                                     "a send cut by the end is cancelled, not a module that failed");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, s_cancelled, "the send that was cut is told as SEND_CANCELLED, once");
    TEST_ASSERT_EQUAL_UINT32(FCK_AOS_CANCEL_LEAVING, s_cancel_cause);
    TEST_ASSERT_FALSE(sim_cc1101.on_air);
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_begin());
    TEST_ASSERT_TRUE(sim_wait_present(3000U));
    sim_cc1101.play_ms = 0U;
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_subghz_send_raw(HZ_433, k_frame, 4U, 1U));
    TEST_ASSERT_EQUAL_UINT32(2U, sim_cc1101.sends);
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_end());
}

TEST_CASE("end stops the GNSS receiver", "[end]")
{
    begin();
    sim_script(SIM_C5, "gnss on", "#DONE gnss rc=0\r\n", 5U);
    sim_script(SIM_C5, "gnss off", "#DONE gnss rc=0\r\n", 5U);
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_gnss_begin());
    TEST_ASSERT_TRUE(sim_gnss_running());
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_end());
    TEST_ASSERT_FALSE(sim_gnss_running());
    fck_status_t st;
    fck_core_status(&st);
    TEST_ASSERT_FALSE(st.present);
    TEST_ASSERT_FALSE(st.gnss_running);
}

static volatile kode_status_t s_cb_rc = KODE_OK;

static void end_from_callback(fck_aos_event_t ev, uint32_t arg, void *ctx)
{
    (void)arg;
    (void)ctx;
    if (ev == FCK_AOS_EVENT_PRESENT) {
        s_cb_rc = fck_core_end();
    }
}

TEST_CASE("end from the event callback is refused, not waited for", "[end]")
{
    sim_begin_empty();
    s_cb_rc = KODE_OK;
    sim_on_event(end_from_callback, NULL);
    sim_plug(SIM_DEV_EXPANDER, false);
    TEST_ASSERT_TRUE(sim_wait_present(3000U));
    sim_sleep(200U);
    sim_on_event(NULL, NULL);
    TEST_ASSERT_EQUAL(KODE_DENIED, s_cb_rc);
    TEST_ASSERT_TRUE(fck_is_present());
}
