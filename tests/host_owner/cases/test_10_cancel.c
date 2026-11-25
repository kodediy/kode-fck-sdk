/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_10_cancel.c
 * @brief fck_core_cancel(): what waits or runs when it is called ends, in steps; a
 *        call begun after it is not touched.
 *
 * Each case starts the call in a task of its own and cancels from the case's.
 * The CC1101 is the model's (a send plays for sim_cc1101.play_ms and ends within
 * a step of the driver's cut); the chips answer what a script says, after the
 * delay it says. The bounds are looser than the steps (20 ms for a wait) because
 * the suite runs on a loaded host.
 */
#include <string.h>

#include "unity.h"

#include "fck.h"
#include "fck_core.h"
#include "fck_aos.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "fake_world.h"

#define HZ_433   433920000U
#define HOUR_MS  3600000U
#define FAST_MS  300U      /* a cancelled wait is over within this */

static const int32_t k_frame[4] = { 350, -1050, 1050, -350 };

typedef struct {
    volatile bool          done;
    volatile kode_status_t rc;
    volatile uint32_t      ended_ms;
    volatile uint32_t      n;
    const char *volatile   why;
} job_t;

static job_t s_a;
static job_t s_b;

static void reset_jobs(void)
{
    memset((void *)&s_a, 0, sizeof(s_a));
    memset((void *)&s_b, 0, sizeof(s_b));
    s_a.rc = KODE_ERROR;
    s_b.rc = KODE_ERROR;
}

static bool a_done(void) { return s_a.done; }
static bool b_done(void) { return s_b.done; }

static void spawn(TaskFunction_t fn, const char *name)
{
    TEST_ASSERT_EQUAL(pdPASS, xTaskCreate(fn, name, 8192, NULL, 3, NULL));
}

/* ---- Waits ----------------------------------------------------------------------------- */

static void present_task(void *unused)
{
    (void)unused;
    s_a.rc       = fck_core_wait_present(20000U);
    s_a.why      = fck_error();
    s_a.ended_ms = sim_now_ms();
    s_a.done     = true;
    vTaskDelete(NULL);
}

static bool not_present(void) { return !fck_is_present(); }

TEST_CASE("fck_core_cancel() ends a wait for the module within a step, and the next wait is as it was", "[cancel]")
{
    sim_begin_fck(NULL, NULL);
    sim_unplug();
    TEST_ASSERT_TRUE(sim_wait_for(not_present, 3000U));
    reset_jobs();
    spawn(present_task, "present");
    sim_sleep(100U);
    TEST_ASSERT_FALSE(s_a.done);
    uint32_t const asked = sim_now_ms();
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_cancel());
    TEST_ASSERT_TRUE_MESSAGE(sim_wait_for(a_done, FAST_MS), "the wait went on");
    TEST_ASSERT_EQUAL(KODE_DENIED, s_a.rc);
    TEST_ASSERT_EQUAL_STRING("cancelled by fck_cancel()", s_a.why);
    TEST_ASSERT_TRUE((s_a.ended_ms - asked) < FAST_MS);
    /* A wait begun after the cancel is nobody's: it waits its time, then it is a timeout. */
    TEST_ASSERT_EQUAL(KODE_TIMEOUT, fck_core_wait_present(150U));
}

static void holder_task(void *unused)
{
    (void)unused;
    s_a.rc = fck_aos_hold(FCK_AOS_C5, 5000U);
    s_a.n  = 1U;
    vTaskDelay(pdMS_TO_TICKS(1500));
    (void)fck_aos_release(FCK_AOS_C5);
    s_a.done = true;
    vTaskDelete(NULL);
}

static void waiter_task(void *unused)
{
    (void)unused;
    s_b.rc       = fck_aos_hold(FCK_AOS_C5, 10000U);
    s_b.why      = fck_error();
    s_b.ended_ms = sim_now_ms();
    if (s_b.rc == KODE_OK) {
        (void)fck_aos_release(FCK_AOS_C5);
    }
    s_b.done = true;
    vTaskDelete(NULL);
}

static bool a_n(void) { return s_a.n != 0U; }

TEST_CASE("fck_core_cancel() ends a wait for a chip another task holds, and leaves the holder holding", "[cancel]")
{
    sim_begin_fck(NULL, NULL);
    reset_jobs();
    spawn(holder_task, "holder");
    TEST_ASSERT_TRUE(sim_wait_for(a_n, 1000U));
    TEST_ASSERT_EQUAL(KODE_OK, s_a.rc);
    spawn(waiter_task, "waiter");
    sim_sleep(100U);
    TEST_ASSERT_FALSE(s_b.done);
    uint32_t const asked = sim_now_ms();
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_cancel());
    TEST_ASSERT_TRUE_MESSAGE(sim_wait_for(b_done, FAST_MS), "the wait for the chip went on");
    TEST_ASSERT_EQUAL(KODE_DENIED, s_b.rc);
    TEST_ASSERT_EQUAL_STRING("cancelled by fck_cancel()", s_b.why);
    TEST_ASSERT_TRUE((s_b.ended_ms - asked) < FAST_MS);
    TEST_ASSERT_FALSE_MESSAGE(s_a.done, "the holder's hold was taken from it");
    TEST_ASSERT_TRUE(sim_wait_for(a_done, 3000U));
    /* The chip is free again, to a call begun after the cancel. */
    TEST_ASSERT_EQUAL(KODE_OK, fck_aos_hold(FCK_AOS_C5, 1000U));
    TEST_ASSERT_EQUAL(KODE_OK, fck_aos_release(FCK_AOS_C5));
}

/* ---- A command in flight ----------------------------------------------------------------- */

static volatile uint32_t s_reader;

static void reader(void *ctx, const fck_drv_link_message_t *m)
{
    (void)ctx;
    (void)m;
    s_reader++;
}

static void slow_task(void *unused)
{
    (void)unused;
    int32_t rc   = -1;
    s_a.rc       = fck_aos_command(FCK_AOS_C5, "slow", "slow", reader, NULL, 10000U, &rc);
    s_a.why      = fck_error();
    s_a.ended_ms = sim_now_ms();
    s_a.done     = true;
    vTaskDelete(NULL);
}

TEST_CASE("fck_core_cancel() lets go of a command in flight, its reader is never called, and the next waits for the chip", "[cancel]")
{
    sim_begin_fck(NULL, NULL);
    sim_script(SIM_C5, "slow", "#LATE a=1\r\n#DONE slow rc=0\r\n", 1200U);
    sim_script(SIM_C5, "next", "#DONE next rc=0\r\n", 5U);
    sim_sent_clear(SIM_C5);
    reset_jobs();
    s_reader = 0U;
    spawn(slow_task, "slow");
    sim_sleep(100U);
    TEST_ASSERT_FALSE(s_a.done);
    uint32_t const asked = sim_now_ms();
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_cancel());
    TEST_ASSERT_TRUE_MESSAGE(sim_wait_for(a_done, FAST_MS), "the caller went on waiting for the chip");
    TEST_ASSERT_EQUAL(KODE_DENIED, s_a.rc);
    TEST_ASSERT_EQUAL_STRING("cancelled by fck_cancel()", s_a.why);
    TEST_ASSERT_TRUE((s_a.ended_ms - asked) < FAST_MS);
    /* A command after it is the next in line, not on top of the first. */
    int32_t rc = -1;
    TEST_ASSERT_EQUAL(KODE_OK, fck_aos_command(FCK_AOS_C5, "next", "next", NULL, NULL, 5000U, &rc));
    TEST_ASSERT_EQUAL_INT32(0, rc);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0U, s_reader, "the reader of a call that was let go was called");
    TEST_ASSERT_EQUAL_STRING("slow", sim_sent_line(SIM_C5, 0U));
    TEST_ASSERT_EQUAL_STRING("next", sim_sent_line(SIM_C5, 1U));
}

TEST_CASE("a short command after a long one was let go has its own time, counted from when it is sent", "[cancel]")
{
    /* A frames listen of seconds cancelled, and the next channel's listen of 300 ms asked at once:
     * the chip finishes the first before it reads the second, and the second is not out of time
     * for having waited for the line. */
    sim_begin_fck(NULL, NULL);
    sim_script(SIM_C5, "slow", "#LATE a=1\r\n#DONE slow rc=0\r\n", 1200U);
    sim_script(SIM_C5, "next", "#DONE next rc=0\r\n", 5U);
    sim_sent_clear(SIM_C5);
    reset_jobs();
    s_reader = 0U;
    spawn(slow_task, "slow");
    sim_sleep(100U);
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_cancel());
    TEST_ASSERT_TRUE(sim_wait_for(a_done, FAST_MS));
    int32_t        rc = -1;
    uint32_t const t0 = sim_now_ms();
    TEST_ASSERT_EQUAL_MESSAGE(KODE_OK, fck_aos_command(FCK_AOS_C5, "next", "next", NULL, NULL, 300U, &rc),
                              "the short command ran out while the line was still busy with the long one");
    TEST_ASSERT_EQUAL_INT32(0, rc);
    TEST_ASSERT_TRUE_MESSAGE((sim_now_ms() - t0) >= 900U, "it was sent before the long one had ended");
    TEST_ASSERT_EQUAL_STRING("next", sim_sent_line(SIM_C5, 1U));
    TEST_ASSERT_EQUAL_UINT32(0U, s_reader);
}

/* ---- A send in the air --------------------------------------------------------------------- */

static void send_task_a(void *unused)
{
    (void)unused;
    s_a.rc       = fck_core_subghz_send_raw(HZ_433, k_frame, 4U, 10U);
    s_a.why      = fck_error();
    s_a.ended_ms = sim_now_ms();
    s_a.done     = true;
    vTaskDelete(NULL);
}

static void send_task_b(void *unused)
{
    (void)unused;
    s_b.rc       = fck_core_subghz_send_raw(HZ_433, k_frame, 4U, 10U);
    s_b.ended_ms = sim_now_ms();
    s_b.done     = true;
    vTaskDelete(NULL);
}

static bool on_air(void) { return sim_cc1101.on_air; }
static bool off_air(void) { return !sim_cc1101.on_air; }

static void begin_radio(void)
{
    sim_begin_fck(NULL, NULL);
    sim_cc1101_reset();
    sim_clock_skip_ms(2U * HOUR_MS);
    reset_jobs();
}

TEST_CASE("fck_core_cancel() ends a send on the air within a step; the one after it goes out whole", "[cancel]")
{
    begin_radio();
    sim_cc1101.play_ms = 2000U;
    spawn(send_task_a, "send");
    TEST_ASSERT_TRUE_MESSAGE(sim_wait_for(on_air, 1000U), "the send never reached the radio");
    uint32_t const asked = sim_now_ms();
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_cancel());
    TEST_ASSERT_TRUE_MESSAGE(sim_wait_for(off_air, FAST_MS), "the carrier is still on");
    TEST_ASSERT_TRUE((sim_cc1101.idle_at_ms - asked) < FAST_MS);
    TEST_ASSERT_TRUE(sim_wait_for(a_done, FAST_MS));
    TEST_ASSERT_EQUAL(KODE_DENIED, s_a.rc);
    TEST_ASSERT_EQUAL_STRING("cancelled by fck_cancel()", s_a.why);
    TEST_ASSERT_EQUAL_UINT32(1U, sim_cc1101.cut);
    /* Not a mode: the next send plays to its end. */
    sim_cc1101.play_ms = 60U;
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_subghz_send_raw(HZ_433, k_frame, 4U, 1U));
    TEST_ASSERT_EQUAL_UINT32(1U, sim_cc1101.cut);
    TEST_ASSERT_EQUAL_UINT32(2U, sim_cc1101.sends);
}

TEST_CASE("fck_core_cancel() also stops the send waiting behind the one on the air: it never keys", "[cancel]")
{
    begin_radio();
    sim_cc1101.play_ms = 2000U;
    spawn(send_task_a, "send_a");
    TEST_ASSERT_TRUE(sim_wait_for(on_air, 1000U));
    spawn(send_task_b, "send_b");
    sim_sleep(100U);
    TEST_ASSERT_FALSE(s_b.done);
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_cancel());
    TEST_ASSERT_TRUE(sim_wait_for(b_done, FAST_MS));
    TEST_ASSERT_TRUE(sim_wait_for(a_done, FAST_MS));
    TEST_ASSERT_EQUAL(KODE_DENIED, s_a.rc);
    TEST_ASSERT_EQUAL(KODE_DENIED, s_b.rc);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1U, sim_cc1101.sends, "the second send reached the radio");
}

TEST_CASE("fck_core_cancel() with nothing to cancel is OK, and touches nothing", "[cancel]")
{
    begin_radio();
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_cancel());
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_cancel());
    sim_cc1101.play_ms = 20U;
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_subghz_send_raw(HZ_433, k_frame, 4U, 1U));
    TEST_ASSERT_EQUAL_UINT32(0U, sim_cc1101.cut);
}

/* ---- The nRF's long calls: slices ------------------------------------------------------------ */

#define SNIFF_REPLY \
    "#SNIFFTOP24 i=0 cnt=3 rssi=-40 len=2 data=0a1b\r\n" \
    "#SNIFFDONE24 ch=76 rx=5 gated=1 uniq=1 ms=1000\r\n#DONE rf_sniff rc=0\r\n"

static void sniff_task(void *unused)
{
    (void)unused;
    static fck_radio_frame_t out[4];
    size_t                   n = 0U;
    fck_radio_sniff_statistics_t st;
    s_a.rc   = fck_core_radio_sniff(2476U, 2500U, -60, out, 4U, &n, &st);
    s_a.n    = (uint32_t)n;
    s_b.n    = st.heard;
    s_b.rc   = (kode_status_t)out[0].seen;
    s_a.done = true;
    vTaskDelete(NULL);
}

TEST_CASE("a sniff of 2.5 s is three commands, and what they heard is put together", "[cancel][radio]")
{
    sim_begin_fck(NULL, NULL);
    sim_script(SIM_NRF, "rf sniff", SNIFF_REPLY, 20U);
    sim_sent_clear(SIM_NRF);
    reset_jobs();
    spawn(sniff_task, "sniff");
    TEST_ASSERT_TRUE(sim_wait_for(a_done, 3000U));
    TEST_ASSERT_EQUAL(KODE_OK, s_a.rc);
    TEST_ASSERT_EQUAL_UINT32(3U, sim_sent(SIM_NRF, "rf sniff"));
    TEST_ASSERT_EQUAL_STRING("rf sniff 76 1000 -60", sim_sent_line(SIM_NRF, 0U));
    TEST_ASSERT_EQUAL_STRING("rf sniff 76 1000 -60", sim_sent_line(SIM_NRF, 1U));
    TEST_ASSERT_EQUAL_STRING("rf sniff 76 500 -60", sim_sent_line(SIM_NRF, 2U));
    TEST_ASSERT_EQUAL_UINT32(1U, s_a.n);            /* the same frame, once ...               */
    TEST_ASSERT_EQUAL_UINT32(9U, (uint32_t)s_b.rc); /* ... heard 3 times in each slice        */
    TEST_ASSERT_EQUAL_UINT32(15U, s_b.n);           /* frames heard: 5 in each                */
}

TEST_CASE("fck_core_cancel() in the middle of a sniff stops it at the end of the slice: no more are sent", "[cancel][radio]")
{
    sim_begin_fck(NULL, NULL);
    sim_script(SIM_NRF, "rf sniff", SNIFF_REPLY, 400U);
    sim_sent_clear(SIM_NRF);
    reset_jobs();
    spawn(sniff_task, "sniff");
    sim_sleep(100U);
    TEST_ASSERT_EQUAL_UINT32(1U, sim_sent(SIM_NRF, "rf sniff"));
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_cancel());
    TEST_ASSERT_TRUE_MESSAGE(sim_wait_for(a_done, FAST_MS), "the sniff went on");
    TEST_ASSERT_EQUAL(KODE_DENIED, s_a.rc);
    sim_sleep(1000U);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1U, sim_sent(SIM_NRF, "rf sniff"), "a slice was started after the cancel");
}

static void spectrum_task(void *unused)
{
    (void)unused;
    int8_t dbm[3];
    size_t n = 0U;
    s_a.rc   = fck_core_radio_spectrum(2402U, 2404U, 100000U, 7U, dbm, sizeof(dbm), &n);
    s_a.n    = (uint32_t)n;
    s_b.n    = (uint32_t)(uint8_t)dbm[1];
    s_a.done = true;
    vTaskDelete(NULL);
}

TEST_CASE("a spectrum of 7 sweeps at 100 ms a MHz is three commands, 3 + 3 + 1, and the peaks are kept", "[cancel][radio]")
{
    sim_begin_fck(NULL, NULL);
    sim_script(SIM_NRF, "rf spec",
               "#SPEC24 f0=2402 step=1 n=3 sweep=1 v=-90,-60,-90\r\n#DONE rf_spec rc=0\r\n", 20U);
    sim_sent_clear(SIM_NRF);
    reset_jobs();
    spawn(spectrum_task, "spectrum");
    TEST_ASSERT_TRUE(sim_wait_for(a_done, 3000U));
    TEST_ASSERT_EQUAL(KODE_OK, s_a.rc);
    TEST_ASSERT_EQUAL_UINT32(3U, s_a.n);
    TEST_ASSERT_EQUAL_UINT32(3U, sim_sent(SIM_NRF, "rf spec"));
    TEST_ASSERT_EQUAL_STRING("rf spec 2 4 100000 3", sim_sent_line(SIM_NRF, 0U) );
    TEST_ASSERT_EQUAL_STRING("rf spec 2 4 100000 3", sim_sent_line(SIM_NRF, 1U));
    TEST_ASSERT_EQUAL_STRING("rf spec 2 4 100000 1", sim_sent_line(SIM_NRF, 2U));
    TEST_ASSERT_EQUAL_INT8(-60, (int8_t)s_b.n);
}

static void ping_task(void *unused)
{
    (void)unused;
    fck_radio_ping_statistics_t st;
    s_a.rc   = fck_core_radio_ping(100U, 2440U, 10U, 0, &st);
    s_a.n    = st.sent;
    s_b.n    = st.ok;
    s_b.rc   = (kode_status_t)(uint32_t)(int32_t)st.fwd_mean_x10;
    s_a.done = true;
    vTaskDelete(NULL);
}

TEST_CASE("a ping of 100 packets is five commands, and their statistics add up", "[cancel][radio]")
{
    sim_begin_fck(NULL, NULL);
    sim_script(SIM_NRF, "rf power", "#POWER24 req=0 dbm=0\r\n#DONE rf_power rc=0\r\n", 5U);
    sim_script(SIM_NRF, "rf ping",
               "#PINGDONE24 ch=40 sent=10 ok=9 fwd_mean=-40.0 rev_mean=-30.0 mhz=2440 dbm=0 "
               "timeout=1 crcbad=0 mismatch=0 txfail=0 fwd_min=-45 fwd_max=-35 "
               "rev_min=-35 rev_max=-25 peer=0A peer_dbm=-4 txslow=0\r\n#DONE rf_ping rc=0\r\n", 20U);
    sim_sent_clear(SIM_NRF);
    reset_jobs();
    spawn(ping_task, "ping");
    TEST_ASSERT_TRUE(sim_wait_for(a_done, 4000U));
    TEST_ASSERT_EQUAL(KODE_OK, s_a.rc);
    TEST_ASSERT_TRUE_MESSAGE(sim_sent(SIM_NRF, "rf ping") >= 2U, "one command for the whole call");
    /* Each slice's answer is the script's, 10 sent: the sum is what the slices were. */
    TEST_ASSERT_EQUAL_UINT32(10U * sim_sent(SIM_NRF, "rf ping"), s_a.n);
    TEST_ASSERT_EQUAL_UINT32(9U * sim_sent(SIM_NRF, "rf ping"), s_b.n);
    TEST_ASSERT_EQUAL_INT32(-400, (int32_t)s_b.rc);
}

TEST_CASE("fck_core_cancel() in the middle of a ping sends no further slice", "[cancel][radio]")
{
    sim_begin_fck(NULL, NULL);
    sim_script(SIM_NRF, "rf power", "#POWER24 req=0 dbm=0\r\n#DONE rf_power rc=0\r\n", 5U);
    sim_script(SIM_NRF, "rf ping",
               "#PINGDONE24 ch=40 sent=10 ok=9 fwd_mean=-40.0 rev_mean=-30.0 mhz=2440 dbm=0 "
               "timeout=1 crcbad=0 mismatch=0 txfail=0 fwd_min=-45 fwd_max=-35 "
               "rev_min=-35 rev_max=-25 peer=0A peer_dbm=-4 txslow=0\r\n#DONE rf_ping rc=0\r\n", 400U);
    sim_sent_clear(SIM_NRF);
    reset_jobs();
    spawn(ping_task, "ping");
    sim_sleep(150U);
    TEST_ASSERT_EQUAL_UINT32(1U, sim_sent(SIM_NRF, "rf ping"));
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_cancel());
    TEST_ASSERT_TRUE(sim_wait_for(a_done, FAST_MS));
    TEST_ASSERT_EQUAL(KODE_DENIED, s_a.rc);
    sim_sleep(800U);
    TEST_ASSERT_EQUAL_UINT32(1U, sim_sent(SIM_NRF, "rf ping"));
}
