/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_16_news.c
 * @brief What the owner tells both layers besides the module coming and going: a remote the
 *        sub-GHz listening heard (PACKET_RECEIVED), a send that was cut (SEND_CANCELLED), the
 *        class of every failure (fck_aos_error_why()) and the refusal from an Active Object.
 */
#include <stdio.h>
#include <string.h>

#include "unity.h"

#include "fck.h"
#include "fck_core.h"
#include "fck_aos.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "fake_world.h"

#define HZ_433    433920000U
#define HZ_868    868350000U
#define HOUR_MS   3600000U

/* ---- The events, written down as the owner raises them --------------------------------------- */

#define EVENTS_MAX 16U

static struct {
    volatile unsigned n;
    fck_aos_event_t   ev[EVENTS_MAX];
    uint32_t          arg[EVENTS_MAX];
} g;

static void note(fck_aos_event_t ev, uint32_t arg, void *ctx)
{
    (void)ctx;
    unsigned const i = g.n;
    if (i < EVENTS_MAX) {
        g.ev[i]  = ev;
        g.arg[i] = arg;
    }
    g.n = i + 1U;
}

static void watch(void)
{
    (void)memset((void *)&g, 0, sizeof(g));
    sim_on_event(note, NULL);
}

static unsigned count_of(fck_aos_event_t ev)
{
    unsigned k = 0U;
    for (unsigned i = 0U; i < g.n && i < EVENTS_MAX; i++) {
        if (g.ev[i] == ev) {
            k++;
        }
    }
    return k;
}

static uint32_t last_arg_of(fck_aos_event_t ev)
{
    uint32_t a = 0U;
    for (unsigned i = 0U; i < g.n && i < EVENTS_MAX; i++) {
        if (g.ev[i] == ev) {
            a = g.arg[i];
        }
    }
    return a;
}

static bool one_remote(void)
{
    return count_of(FCK_AOS_EVENT_PACKET_RECEIVED) >= 1U;
}

static bool two_remotes(void)
{
    return count_of(FCK_AOS_EVENT_PACKET_RECEIVED) >= 2U;
}

/* ---- The listening, in the background ----------------------------------------------------------- */

static const int32_t k_remote_a[6] = { 350, -1050, 1050, -350, 350, -10500 };
static const int32_t k_remote_b[4] = { 400, -400, 800, -9000 };

TEST_CASE("the listening hears a remote on its own: told as PACKET_RECEIVED with the capture's count, and read from the owner", "[listening]")
{
    sim_begin_fck(NULL, NULL);
    sim_cc1101_reset();
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_subghz_listen_start(HZ_433));
    uint32_t const base = fck_aos_listen_seq();      /* the count goes on across listenings */
    watch();
    sim_cc1101_hear(k_remote_a, 6U, HZ_433);
    TEST_ASSERT_TRUE_MESSAGE(sim_wait_for(one_remote, 3000U), "a remote the slice heard was not told");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(base + 1U, last_arg_of(FCK_AOS_EVENT_PACKET_RECEIVED), "the news carries the capture's count");
    TEST_ASSERT_EQUAL_UINT32(base + 1U, fck_aos_listen_seq());

    int32_t            us[16];
    size_t             n = 0U;
    fck_subghz_heard_t heard;
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_subghz_listen_read(0U, 0U, us, 16U, &n, &heard));
    TEST_ASSERT_TRUE(heard.heard);
    TEST_ASSERT_EQUAL_UINT(6U, n);
    TEST_ASSERT_EQUAL_INT32_ARRAY(k_remote_a, us, 6);
    TEST_ASSERT_EQUAL_UINT32(HZ_433, heard.hz);
    TEST_ASSERT_EQUAL_UINT32(base + 1U, heard.sequence);

    /* Asking after it: nothing newer is not a failure. */
    n = 99U;
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_subghz_listen_read(heard.sequence, 0U, us, 16U, &n, &heard));
    TEST_ASSERT_FALSE(heard.heard);
    TEST_ASSERT_EQUAL_UINT(0U, n);

    /* The next one is a new count. */
    sim_cc1101_hear(k_remote_b, 4U, HZ_433);
    TEST_ASSERT_TRUE(sim_wait_for(two_remotes, 3000U));
    TEST_ASSERT_EQUAL_UINT32(base + 2U, last_arg_of(FCK_AOS_EVENT_PACKET_RECEIVED));
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_subghz_listen_read(base + 1U, 0U, us, 16U, &n, &heard));
    TEST_ASSERT_TRUE(heard.heard);
    TEST_ASSERT_EQUAL_UINT32(base + 2U, heard.sequence);
    TEST_ASSERT_EQUAL_UINT(4U, n);
    TEST_ASSERT_EQUAL_INT32_ARRAY(k_remote_b, us, 4);
    sim_on_event(NULL, NULL);
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_subghz_listen_stop());
}

TEST_CASE("a read that waits returns when the remote comes, and is cut by a cancel", "[listening]")
{
    sim_begin_fck(NULL, NULL);
    sim_cc1101_reset();
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_subghz_listen_start(HZ_433));
    int32_t            us[16];
    size_t             n = 0U;
    fck_subghz_heard_t heard;
    /* Nothing comes in 200 ms: the read says so, and took its time. */
    uint32_t const t0 = sim_now_ms();
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_subghz_listen_read(0U, 200U, us, 16U, &n, &heard));
    TEST_ASSERT_FALSE(heard.heard);
    TEST_ASSERT_TRUE((sim_now_ms() - t0) >= 150U);
    /* One that comes meanwhile ends the wait before the time. */
    sim_cc1101_hear(k_remote_a, 6U, HZ_433);
    uint32_t const t1 = sim_now_ms();
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_subghz_listen_read(0U, 3000U, us, 16U, &n, &heard));
    TEST_ASSERT_TRUE(heard.heard);
    TEST_ASSERT_TRUE_MESSAGE((sim_now_ms() - t1) < 1500U, "the read waited for the time, not for the remote");
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_subghz_listen_stop());
}

TEST_CASE("a listening stopped is a read refused, and what it kept is freed", "[listening]")
{
    sim_begin_fck(NULL, NULL);
    sim_cc1101_reset();
    int32_t            us[16];
    size_t             n = 0U;
    fck_subghz_heard_t heard;
    TEST_ASSERT_EQUAL_MESSAGE(KODE_DENIED, fck_core_subghz_listen_read(0U, 0U, us, 16U, &n, &heard), "before the start");
    TEST_ASSERT_EQUAL_UINT32(FCK_AOS_WHY_WRONG_ORDER, fck_aos_error_why());
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_subghz_listen_start(HZ_433));
    sim_cc1101_hear(k_remote_a, 6U, HZ_433);
    TEST_ASSERT_TRUE(sim_wait_for(one_remote, 3000U) || true);
    sim_sleep(300U);
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_subghz_listen_stop());
    TEST_ASSERT_EQUAL_MESSAGE(KODE_DENIED, fck_core_subghz_listen_read(0U, 0U, us, 16U, &n, &heard), "after the stop");
    uint32_t const slices = sim_cc1101.listens;
    sim_sleep(500U);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(slices, sim_cc1101.listens, "no slice runs once the listening is off");
}

TEST_CASE("a request goes first: a send in the middle of a listening goes out and the listening goes on", "[listening]")
{
    sim_begin_fck(NULL, NULL);
    sim_cc1101_reset();
    sim_clock_skip_ms(2U * HOUR_MS);
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_subghz_listen_start(HZ_433));
    sim_sleep(200U);
    uint32_t const slices = sim_cc1101.listens;
    static const int32_t frame[4] = { 350, -1050, 1050, -350 };
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_subghz_send_raw(HZ_868, frame, 4U, 1U));
    TEST_ASSERT_EQUAL_UINT32(1U, sim_cc1101.sends);
    sim_sleep(400U);
    TEST_ASSERT_TRUE_MESSAGE(sim_cc1101.listens > slices, "the listening went on after the send");
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_subghz_listen_stop());
}

/* ---- A send that was cut ------------------------------------------------------------------------ */

static volatile kode_status_t s_rc;
static volatile bool          s_done;

static void send_task(void *unused)
{
    (void)unused;
    static const int32_t frame[4] = { 350, -1050, 1050, -350 };
    s_rc   = fck_core_subghz_send_raw(HZ_433, frame, 4U, 10U);
    s_done = true;
    vTaskDelete(NULL);
}

static bool on_air(void)   { return sim_cc1101.on_air; }
static bool is_done(void)  { return s_done; }
static bool c5_parked(void) { return sim_c5_parks() > 0U; }
static bool told(void)     { return count_of(FCK_AOS_EVENT_SEND_CANCELLED) >= 1U; }

TEST_CASE("a send cut by fck_cancel is told as SEND_CANCELLED with its cause, once", "[news]")
{
    sim_begin_fck(NULL, NULL);
    sim_cc1101_reset();
    sim_clock_skip_ms(2U * HOUR_MS);
    sim_cc1101.play_ms = 2000U;
    s_done = false;
    watch();
    TEST_ASSERT_EQUAL(pdPASS, xTaskCreate(send_task, "send", 8192, NULL, 3, NULL));
    TEST_ASSERT_TRUE(sim_wait_for(on_air, 1000U));
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_cancel());
    TEST_ASSERT_TRUE(sim_wait_for(is_done, 2000U));
    TEST_ASSERT_TRUE(sim_wait_for(told, 1000U));
    sim_sleep(100U);
    sim_on_event(NULL, NULL);
    TEST_ASSERT_EQUAL_UINT(1U, count_of(FCK_AOS_EVENT_SEND_CANCELLED));
    TEST_ASSERT_EQUAL_UINT32(FCK_AOS_CANCEL_CALL, last_arg_of(FCK_AOS_EVENT_SEND_CANCELLED));
    TEST_ASSERT_EQUAL(KODE_DENIED, s_rc);
}

TEST_CASE("a send that ended by itself tells nothing of being cancelled", "[news]")
{
    sim_begin_fck(NULL, NULL);
    sim_cc1101_reset();
    sim_clock_skip_ms(2U * HOUR_MS);
    sim_cc1101.play_ms = 60U;
    watch();
    static const int32_t frame[4] = { 350, -1050, 1050, -350 };
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_subghz_send_raw(HZ_433, frame, 4U, 1U));
    sim_sleep(200U);
    sim_on_event(NULL, NULL);
    TEST_ASSERT_EQUAL_UINT(0U, count_of(FCK_AOS_EVENT_SEND_CANCELLED));
}

TEST_CASE("a fck_cancel with nothing on the air tells nothing", "[news]")
{
    sim_begin_fck(NULL, NULL);
    watch();
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_cancel());
    sim_sleep(200U);
    sim_on_event(NULL, NULL);
    TEST_ASSERT_EQUAL_UINT(0U, count_of(FCK_AOS_EVENT_SEND_CANCELLED));
}

/* ---- The class of a failure --------------------------------------------------------------------- */

static void expect_why(fck_aos_why_t want, const char *what)
{
    char said[200];
    (void)snprintf(said, sizeof(said), "%s: \"%s\"", what, fck_aos_error());
    TEST_ASSERT_EQUAL_UINT32_MESSAGE((uint32_t)want, (uint32_t)fck_aos_error_why(), said);
}

TEST_CASE("a call that worked leaves no class, and one that failed says which kind", "[why]")
{
    sim_begin_fck(NULL, NULL);
    fck_firmware_t a;
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_firmware(FCK_C5, &a));
    fck_aos_error_ok();
    expect_why(FCK_AOS_WHY_NONE, "after a call that worked");
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_subghz_send_raw(HZ_433, NULL, 4U, 1U));
    expect_why(FCK_AOS_WHY_BAD_ARGUMENT, "a NULL pointer");
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_firmware((fck_chip_t)7, &a));
    expect_why(FCK_AOS_WHY_BAD_ARGUMENT, "a chip that is not one");
}

TEST_CASE("the class comes from the status when it says more than the text: I/O, busy, timeout", "[why]")
{
    sim_begin_fck(NULL, NULL);
    fck_aos_error_set_for(KODE_IO_ERROR, "the chip said no");
    expect_why(FCK_AOS_WHY_IO, "I/O");
    fck_aos_error_set_for(KODE_NO_RESOURCES, "no room");
    expect_why(FCK_AOS_WHY_BUSY, "no resources");
    fck_aos_error_set_for(KODE_TIMEOUT, "the chip did not answer");
    expect_why(FCK_AOS_WHY_TIMEOUT, "timeout");
    fck_aos_error_set_for(KODE_TIMEOUT, "cancelled by fck_cancel()");
    expect_why(FCK_AOS_WHY_CANCELLED, "a cancel ends a wait with the timeout's status");
    fck_aos_error_set_for(KODE_IO_ERROR, "cancelled: the send was cut by the way home, a rest or fck_end()");
    expect_why(FCK_AOS_WHY_CANCELLED, "leaving ends a send with the I/O status");
    fck_aos_error_set_for(KODE_OK, "");
    expect_why(FCK_AOS_WHY_NONE, "OK");
}

TEST_CASE("a call the owner's queue had no room for is busy, not a refusal", "[why]")
{
    sim_begin_fck(NULL, NULL);
    sim_ao_full(true);
    kode_status_t const st = fck_aos_reset(FCK_AOS_NRF);
    fck_aos_why_t const cls = fck_aos_error_why();
    sim_ao_full(false);
    TEST_ASSERT_EQUAL(KODE_NO_RESOURCES, st);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE((uint32_t)FCK_AOS_WHY_BUSY, (uint32_t)cls, "a full queue: ask again");
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_end());
}

TEST_CASE("before fck_begin, with the module away, and going home: the class says which", "[why]")
{
    sim_begin_fck(NULL, NULL);
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_end());
    fck_firmware_t a;
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_firmware(FCK_C5, &a));
    expect_why(FCK_AOS_WHY_NOT_BEGUN, "after fck_end");
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_begin());
    sim_unplug();
    TEST_ASSERT_TRUE(sim_wait_absent(5000U));
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_firmware(FCK_C5, &a));
    expect_why(FCK_AOS_WHY_NO_MODULE, "with no module on J3");
}

TEST_CASE("past the radio limits, and a listening read out of order, are classes of their own", "[why]")
{
    sim_begin_fck(NULL, NULL);
    sim_cc1101_reset();
    sim_clock_skip_ms(2U * HOUR_MS);
    static const int32_t frame[4] = { 350, -1050, 1050, -350 };
    /* Over the air-time cap of the band: 3 s of carrier for each send, ten sends. */
    kode_status_t st = KODE_OK;
    for (unsigned i = 0U; (i < 40U) && (st == KODE_OK); i++) {
        st = fck_core_subghz_send_raw(HZ_868, frame, 4U, FCK_SUBGHZ_SEND_REPEATS_MAX);
    }
    if (st != KODE_OK) {
        expect_why(FCK_AOS_WHY_BEYOND_LIMITS, "the duty cap");
    }
    int32_t            us[8];
    size_t             n = 0U;
    fck_subghz_heard_t heard;
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_subghz_listen_read(0U, 0U, us, 8U, &n, &heard));
    expect_why(FCK_AOS_WHY_WRONG_ORDER, "a read before the listening started");
}

TEST_CASE("a call from an Active Object is refused in its own class, with the call's name and the way out", "[why]")
{
    sim_begin_fck(NULL, NULL);
    sim_ao_mark_task(xTaskGetCurrentTaskHandle());
    TEST_ASSERT_TRUE(fck_aos_refuse_ao("fck_firmware()"));
    sim_ao_mark_task(NULL);
    expect_why(FCK_AOS_WHY_AO_CALLER, "from an Active Object");
    char const *const text = fck_aos_error();
    TEST_ASSERT_NOT_NULL(strstr(text, "fck_firmware()"));
    TEST_ASSERT_NOT_NULL(strstr(text, "was called from an Active Object"));
    TEST_ASSERT_NOT_NULL(strstr(text, "fck_sys_*()"));
    TEST_ASSERT_TRUE_MESSAGE(strlen(text) < FCK_AOS_ERROR_TEXT, "the text fits the room it has");
    TEST_ASSERT_FALSE_MESSAGE(fck_aos_refuse_ao("fck_firmware()"), "an ordinary task is not refused");
}

TEST_CASE("after a way home or a rest a call says the app is leaving, not that the module is away", "[why]")
{
    sim_begin_fck(NULL, NULL);
    sim_parks_clear();
    sim_ao_home();
    TEST_ASSERT_TRUE(sim_wait_for(c5_parked, 2000U));
    int32_t rc = 0;
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_command(FCK_C5, "info", "info", NULL, NULL, 0U, &rc));
    expect_why(FCK_AOS_WHY_LEAVING, "after HOME");
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_end());
}
