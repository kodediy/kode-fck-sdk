/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_events.c
 * @brief fck.h's events: listeners on the SDK's events task, fck_event_wait(), what each event
 *        carries, a full queue, a slow listener, and the calls a listener may and may not make.
 *
 * The owner is played by fake_aos_news(): it tells the sink the events task registered, on the
 * caller's task, as the owner does on its own. The events task is the real one (fck_events.c), on
 * FreeRTOS for the host.
 */
#include <stdio.h>
#include <string.h>

#include "unity.h"

#include "fck.h"
#include "fck_aos.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "fake_aos.h"
#include "fake_core.h"

#define SEEN_MAX 40U

/* What the listeners wrote down. A listener runs on the events task, so a case reads it after waiting. */
static struct {
    volatile unsigned n_a;
    volatile unsigned n_b;
    char              name_a[SEEN_MAX][FCK_EVENT_NAME_MAX];
    int32_t           value_a[SEEN_MAX];
    char              text_a[SEEN_MAX][16];
    volatile bool     hold;          /* listener A waits while this is set                 */
    volatile bool     inside;        /* listener A is running                              */
    volatile uint32_t hold_ms;       /* ... and takes this long                            */
    volatile bool     call_wait;     /* listener A tries fck_event_wait()                  */
    volatile bool     call_end;      /* listener A tries fck_end()                         */
    volatile bool     call_remove_self;
    volatile bool     wait_rc;
    volatile bool     end_rc;
    volatile bool     remove_rc;
    volatile uint32_t remove_took_ms;
    char              why[160];
} g;

static void listener_a(const fck_event_t *e)
{
    g.inside = true;
    unsigned const i = g.n_a;
    if (i < SEEN_MAX) {
        (void)snprintf(g.name_a[i], sizeof(g.name_a[i]), "%s", e->name);
        g.value_a[i] = e->value;
        (void)snprintf(g.text_a[i], sizeof(g.text_a[i]), "%s", e->text);
    }
    g.n_a = i + 1U;
    if (g.call_wait) {
        g.wait_rc = fck_event_wait("module_found", 50U, NULL);
        (void)snprintf(g.why, sizeof(g.why), "%s", fck_error());
    }
    if (g.call_end) {
        g.end_rc = fck_end();
        (void)snprintf(g.why, sizeof(g.why), "%s", fck_error());
    }
    if (g.call_remove_self) {
        uint32_t const t0 = fake_now_ms();
        g.remove_rc       = fck_event_remove_listener("module_lost", listener_a);
        g.remove_took_ms  = fake_now_ms() - t0;
    }
    while (g.hold) {
        vTaskDelay(1);
    }
    if (g.hold_ms > 0U) {
        vTaskDelay(pdMS_TO_TICKS(g.hold_ms));
    }
    g.inside = false;
}

static void listener_b(const fck_event_t *e)
{
    (void)e;
    g.n_b++;
}

static void listener_c(const fck_event_t *e)
{
    (void)e;
}

static const char *const k_names[] = {
    "module_found", "module_lost", "wrong_module", "setting_lost",
    "setting_restored", "packet_received", "send_cancelled",
};
#define NAMES (sizeof(k_names) / sizeof(k_names[0]))

static bool wait_until(bool (*cond)(void), uint32_t ms)
{
    for (uint32_t t = 0U; t <= ms; t += 5U) {
        if (cond()) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(5U) > 0U ? pdMS_TO_TICKS(5U) : 1U);
    }
    return cond();
}

/* Every listener out, the gate open, the counters at zero: the events task outlives the cases. */
static void clean(void)
{
    g.hold = false;
    for (size_t i = 0U; i < NAMES; i++) {
        (void)fck_event_remove_listener(k_names[i], listener_a);
        (void)fck_event_remove_listener(k_names[i], listener_b);
        (void)fck_event_remove_listener(k_names[i], listener_c);
    }
    vTaskDelay(pdMS_TO_TICKS(60U));
    (void)memset((void *)&g, 0, sizeof(g));
}

static void start(void)
{
    clean();
    fake_core_reset();
    fake_aos_reset();
    TEST_ASSERT_TRUE(fck_begin());
}

static bool one_for_a(void)
{
    return g.n_a >= 1U;
}

static bool two_for_a(void)
{
    return g.n_a >= 2U;
}

static bool inside_a(void)
{
    return g.inside;
}

static bool outside_a(void)
{
    return !g.inside;
}

/* ---- What each event carries ---------------------------------------------------------------- */

static void check_event(const char *name, fck_aos_event_t ev, uint32_t arg, int32_t value, const char *text)
{
    start();
    TEST_ASSERT_TRUE(fck_event_add_listener(name, listener_a));
    TEST_ASSERT_TRUE(fake_aos_news(ev, arg));
    TEST_ASSERT_TRUE_MESSAGE(wait_until(one_for_a, 1000U), name);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, g.n_a, name);
    TEST_ASSERT_EQUAL_STRING(name, g.name_a[0]);
    TEST_ASSERT_EQUAL_INT32_MESSAGE(value, g.value_a[0], name);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(text, g.text_a[0], name);
    clean();
}

TEST_CASE("module_found is told with value 1, and no text", "[events][names]")
{
    check_event("module_found", FCK_AOS_EVENT_PRESENT, 0U, 1, "");
}

TEST_CASE("module_lost is told with value 0, and no text", "[events][names]")
{
    check_event("module_lost", FCK_AOS_EVENT_GONE, 0U, 0, "");
}

TEST_CASE("wrong_module is told with what is on J3 instead", "[events][names]")
{
    check_event("wrong_module", FCK_AOS_EVENT_WRONG_MODULE, FCK_AOS_SEEN_OTHER_CHIP, (int32_t)FCK_WRONG_MODULE_OTHER_CHIP, "");
    check_event("wrong_module", FCK_AOS_EVENT_WRONG_MODULE, FCK_AOS_SEEN_HOLDS_PAIR, (int32_t)FCK_WRONG_MODULE_HOLDS_PAIR, "");
}

TEST_CASE("setting_lost and setting_restored are told with the settings", "[events][names]")
{
    check_event("setting_lost", FCK_AOS_EVENT_SETTING_LOST, FCK_AOS_SETTING_GNSS | FCK_AOS_SETTING_LISTEN,
                (int32_t)(FCK_SETTING_GNSS | FCK_SETTING_LISTEN), "");
    check_event("setting_restored", FCK_AOS_EVENT_SETTING_RESTORED, FCK_AOS_SETTING_LISTEN, (int32_t)FCK_SETTING_LISTEN, "");
}

TEST_CASE("packet_received is told with the capture's count", "[events][names]")
{
    check_event("packet_received", FCK_AOS_EVENT_PACKET_RECEIVED, 7U, 7, "");
}

TEST_CASE("send_cancelled is told with its cause, in a number and in a word", "[events][names]")
{
    check_event("send_cancelled", FCK_AOS_EVENT_SEND_CANCELLED, FCK_AOS_CANCEL_CALL, FCK_CANCEL_CALL, "cancel");
    check_event("send_cancelled", FCK_AOS_EVENT_SEND_CANCELLED, FCK_AOS_CANCEL_LEAVING, FCK_CANCEL_LEAVING, "leaving");
}

TEST_CASE("a listener is told only of its own event, and in the order they came", "[events]")
{
    start();
    TEST_ASSERT_TRUE(fck_event_add_listener("module_lost", listener_a));
    TEST_ASSERT_TRUE(fck_event_add_listener("module_found", listener_b));
    (void)fake_aos_news(FCK_AOS_EVENT_GONE, 0U);
    (void)fake_aos_news(FCK_AOS_EVENT_PRESENT, 0U);
    (void)fake_aos_news(FCK_AOS_EVENT_PACKET_RECEIVED, 1U);
    (void)fake_aos_news(FCK_AOS_EVENT_GONE, 0U);
    TEST_ASSERT_TRUE(wait_until(two_for_a, 1000U));
    vTaskDelay(pdMS_TO_TICKS(100U));
    TEST_ASSERT_EQUAL_UINT(2U, g.n_a);
    TEST_ASSERT_EQUAL_UINT(1U, g.n_b);
    clean();
}

TEST_CASE("one listener for several events, and two for one, are each told", "[events]")
{
    start();
    TEST_ASSERT_TRUE(fck_event_add_listener("module_lost", listener_a));
    TEST_ASSERT_TRUE(fck_event_add_listener("module_found", listener_a));
    TEST_ASSERT_TRUE(fck_event_add_listener("module_found", listener_b));
    (void)fake_aos_news(FCK_AOS_EVENT_PRESENT, 0U);
    (void)fake_aos_news(FCK_AOS_EVENT_GONE, 0U);
    TEST_ASSERT_TRUE(wait_until(two_for_a, 1000U));
    vTaskDelay(pdMS_TO_TICKS(50U));
    TEST_ASSERT_EQUAL_UINT(2U, g.n_a);
    TEST_ASSERT_EQUAL_STRING("module_found", g.name_a[0]);
    TEST_ASSERT_EQUAL_STRING("module_lost", g.name_a[1]);
    TEST_ASSERT_EQUAL_UINT(1U, g.n_b);
    clean();
}

/* ---- Nothing is queued for nobody, and a full queue loses and counts ------------------------------ */

TEST_CASE("with nobody listening nothing is queued, and the owner is told it was taken", "[events][queue]")
{
    start();
    for (unsigned i = 0U; i < 5U * FCK_EVENT_QUEUE; i++) {
        TEST_ASSERT_TRUE(fake_aos_news(FCK_AOS_EVENT_GONE, 0U));
    }
}

TEST_CASE("an event nobody listens for is not queued while another is", "[events][queue]")
{
    start();
    g.hold = true;
    TEST_ASSERT_TRUE(fck_event_add_listener("module_lost", listener_a));
    (void)fake_aos_news(FCK_AOS_EVENT_GONE, 0U);
    TEST_ASSERT_TRUE(wait_until(inside_a, 1000U));      /* the events task is held in the listener */
    for (unsigned i = 0U; i < 4U * FCK_EVENT_QUEUE; i++) {
        TEST_ASSERT_TRUE_MESSAGE(fake_aos_news(FCK_AOS_EVENT_PRESENT, 0U), "module_found has no listener: not a loss");
    }
    g.hold = false;
    clean();
}

TEST_CASE("a full queue loses the event and tells the owner, which counts it; the ones queued are told in order", "[events][queue]")
{
    start();
    g.hold = true;
    TEST_ASSERT_TRUE(fck_event_add_listener("packet_received", listener_a));
    (void)fake_aos_news(FCK_AOS_EVENT_PACKET_RECEIVED, 1U);
    TEST_ASSERT_TRUE(wait_until(inside_a, 1000U));      /* held in the first: the queue is empty and idle */
    unsigned taken   = 0U;
    unsigned dropped = 0U;
    for (uint32_t i = 2U; i < 2U + 3U * FCK_EVENT_QUEUE; i++) {
        if (fake_aos_news(FCK_AOS_EVENT_PACKET_RECEIVED, i)) {
            taken++;
        } else {
            dropped++;
        }
    }
    TEST_ASSERT_EQUAL_UINT_MESSAGE(FCK_EVENT_QUEUE, taken, "FCK_EVENT_QUEUE events wait behind the one running");
    TEST_ASSERT_EQUAL_UINT(2U * FCK_EVENT_QUEUE, dropped);
    g.hold = false;
    for (unsigned t = 0U; (t < 200U) && (g.n_a < 1U + FCK_EVENT_QUEUE); t++) {
        vTaskDelay(pdMS_TO_TICKS(10U));
    }
    TEST_ASSERT_EQUAL_UINT(1U + FCK_EVENT_QUEUE, g.n_a);
    TEST_ASSERT_EQUAL_INT32(1, g.value_a[0]);
    TEST_ASSERT_EQUAL_INT32(2, g.value_a[1]);
    TEST_ASSERT_EQUAL_INT32((int32_t)(1U + FCK_EVENT_QUEUE), g.value_a[FCK_EVENT_QUEUE]);
    clean();
}

/* ---- add and remove ------------------------------------------------------------------------- */

TEST_CASE("fck_event_add_listener answers true for each of the seven names", "[events][add]")
{
    start();
    for (size_t i = 0U; i < NAMES; i++) {
        TEST_ASSERT_TRUE_MESSAGE(fck_event_add_listener(k_names[i], listener_a), k_names[i]);
        TEST_ASSERT_EQUAL_STRING("", fck_error());
    }
    clean();
}

TEST_CASE("fck_event_add_listener is false for a name that is none, a name too long, NULL, or a NULL function", "[events][add]")
{
    start();
    static const char too_long[] = "module_found_module_found_module_found_module_found";
    TEST_ASSERT_FALSE(fck_event_add_listener("module_gone", listener_a));
    TEST_ASSERT_TRUE(strlen(fck_error()) > 0U);
    TEST_ASSERT_FALSE(fck_event_add_listener(too_long, listener_a));
    TEST_ASSERT_FALSE(fck_event_add_listener("MODULE_FOUND", listener_a));
    TEST_ASSERT_FALSE(fck_event_add_listener("", listener_a));
    TEST_ASSERT_FALSE(fck_event_add_listener(NULL, listener_a));
    TEST_ASSERT_FALSE(fck_event_add_listener("module_found", NULL));
    TEST_ASSERT_NOT_NULL(strstr(fck_error(), "NULL"));
}

TEST_CASE("the same function on the same event twice is false; on another event it is not", "[events][add]")
{
    start();
    TEST_ASSERT_TRUE(fck_event_add_listener("module_found", listener_a));
    TEST_ASSERT_FALSE(fck_event_add_listener("module_found", listener_a));
    TEST_ASSERT_NOT_NULL(strstr(fck_error(), "already"));
    TEST_ASSERT_TRUE(fck_event_add_listener("module_lost", listener_a));
    clean();
}

TEST_CASE("past FCK_LISTENERS_MAX listeners it is false, and a removed one gives its place back", "[events][add]")
{
    start();
    /* Eight places over all the names: seven names with A, and one with B. */
    for (size_t i = 0U; i < NAMES; i++) {
        TEST_ASSERT_TRUE(fck_event_add_listener(k_names[i], listener_a));
    }
    TEST_ASSERT_TRUE(fck_event_add_listener("module_found", listener_b));
    TEST_ASSERT_FALSE(fck_event_add_listener("module_found", listener_c));
    TEST_ASSERT_NOT_NULL(strstr(fck_error(), "FCK_LISTENERS_MAX"));
    TEST_ASSERT_TRUE(fck_event_remove_listener("module_found", listener_b));
    TEST_ASSERT_TRUE(fck_event_add_listener("module_found", listener_c));
    clean();
}

TEST_CASE("listeners may be added before fck_begin and stay across fck_end and fck_begin", "[events][add]")
{
    clean();
    fake_core_reset();
    fake_aos_reset();
    g_aos_started = false;
    TEST_ASSERT_TRUE(fck_event_add_listener("module_found", listener_a));
    g_aos_started = true;
    TEST_ASSERT_TRUE(fck_begin());
    TEST_ASSERT_TRUE(fck_end());
    TEST_ASSERT_TRUE(fck_begin());
    (void)fake_aos_news(FCK_AOS_EVENT_PRESENT, 0U);
    TEST_ASSERT_TRUE(wait_until(one_for_a, 1000U));
    clean();
}

TEST_CASE("fck_event_add_listener and fck_event_remove_listener are refused from an Active Object", "[events][add][ao]")
{
    start();
    fake_aos_mark_ao(xTaskGetCurrentTaskHandle());
    TEST_ASSERT_FALSE(fck_event_add_listener("module_found", listener_a));
    TEST_ASSERT_NOT_NULL(strstr(fck_error(), "fck_event_add_listener()"));
    TEST_ASSERT_NOT_NULL(strstr(fck_error(), "fck_sys_*()"));
    fake_aos_mark_ao(NULL);
    TEST_ASSERT_TRUE(fck_event_add_listener("module_found", listener_a));
    fake_aos_mark_ao(xTaskGetCurrentTaskHandle());
    TEST_ASSERT_FALSE(fck_event_remove_listener("module_found", listener_a));
    TEST_ASSERT_NOT_NULL(strstr(fck_error(), "fck_event_remove_listener()"));
    fake_aos_mark_ao(NULL);
    clean();
}

TEST_CASE("fck_event_remove_listener is false for one that was not listening, and the others stay", "[events][remove]")
{
    start();
    TEST_ASSERT_TRUE(fck_event_add_listener("module_found", listener_a));
    TEST_ASSERT_FALSE(fck_event_remove_listener("module_lost", listener_a));
    TEST_ASSERT_FALSE(fck_event_remove_listener("module_found", listener_b));
    TEST_ASSERT_FALSE(fck_event_remove_listener("module_gone", listener_a));
    TEST_ASSERT_FALSE(fck_event_remove_listener(NULL, listener_a));
    TEST_ASSERT_FALSE(fck_event_remove_listener("module_found", NULL));
    TEST_ASSERT_TRUE(fck_event_remove_listener("module_found", listener_a));
    TEST_ASSERT_EQUAL_STRING("", fck_error());
    TEST_ASSERT_FALSE_MESSAGE(fck_event_remove_listener("module_found", listener_a), "twice");
}

TEST_CASE("a listener removed is not told again", "[events][remove]")
{
    start();
    TEST_ASSERT_TRUE(fck_event_add_listener("module_found", listener_a));
    (void)fake_aos_news(FCK_AOS_EVENT_PRESENT, 0U);
    TEST_ASSERT_TRUE(wait_until(one_for_a, 1000U));
    TEST_ASSERT_TRUE(fck_event_remove_listener("module_found", listener_a));
    (void)fake_aos_news(FCK_AOS_EVENT_PRESENT, 0U);
    vTaskDelay(pdMS_TO_TICKS(150U));
    TEST_ASSERT_EQUAL_UINT(1U, g.n_a);
    clean();
}

TEST_CASE("removing a listener waits for the one running: it is free when the call returns", "[events][remove]")
{
    start();
    g.hold_ms = 300U;
    TEST_ASSERT_TRUE(fck_event_add_listener("module_found", listener_a));
    (void)fake_aos_news(FCK_AOS_EVENT_PRESENT, 0U);
    TEST_ASSERT_TRUE(wait_until(inside_a, 1000U));
    uint32_t const t0 = fake_now_ms();
    TEST_ASSERT_TRUE(fck_event_remove_listener("module_found", listener_a));
    uint32_t const took = fake_now_ms() - t0;
    TEST_ASSERT_FALSE_MESSAGE(g.inside, "the listener had returned when the call did");
    TEST_ASSERT_TRUE_MESSAGE(took < 1500U, "it waited for the listener, not for the limit");
    clean();
}

TEST_CASE("a listener that blocks past 2 s does not hold the removal for good: false, and it says the listener still runs", "[events][remove][slow]")
{
    start();
    g.hold = true;
    TEST_ASSERT_TRUE(fck_event_add_listener("module_found", listener_a));
    (void)fake_aos_news(FCK_AOS_EVENT_PRESENT, 0U);
    TEST_ASSERT_TRUE(wait_until(inside_a, 1000U));
    uint32_t const t0 = fake_now_ms();
    TEST_ASSERT_FALSE(fck_event_remove_listener("module_found", listener_a));
    uint32_t const took = fake_now_ms() - t0;
    TEST_ASSERT_EQUAL_STRING("the listener is still running", fck_error());
    TEST_ASSERT_TRUE_MESSAGE(took >= 1900U && took < 2800U, "the 2 s it waits at most");
    TEST_ASSERT_TRUE_MESSAGE(g.inside, "the listener still runs");
    /* Removed all the same: the next event does not call it, and it is not there to remove. */
    (void)fake_aos_news(FCK_AOS_EVENT_PRESENT, 0U);
    g.hold = false;
    TEST_ASSERT_TRUE(wait_until(outside_a, 1000U));
    vTaskDelay(pdMS_TO_TICKS(100U));
    TEST_ASSERT_EQUAL_UINT(1U, g.n_a);
    TEST_ASSERT_FALSE_MESSAGE(fck_event_remove_listener("module_found", listener_a), "it was already removed");
    clean();
}

TEST_CASE("a listener may remove itself: the call returns at once and the listener finishes", "[events][remove]")
{
    start();
    g.call_remove_self = true;
    TEST_ASSERT_TRUE(fck_event_add_listener("module_lost", listener_a));
    (void)fake_aos_news(FCK_AOS_EVENT_GONE, 0U);
    TEST_ASSERT_TRUE(wait_until(one_for_a, 1000U));
    TEST_ASSERT_TRUE(wait_until(outside_a, 1000U));
    TEST_ASSERT_TRUE(g.remove_rc);
    TEST_ASSERT_TRUE_MESSAGE(g.remove_took_ms < 100U, "it did not wait for itself");
    (void)fake_aos_news(FCK_AOS_EVENT_GONE, 0U);
    vTaskDelay(pdMS_TO_TICKS(100U));
    TEST_ASSERT_EQUAL_UINT(1U, g.n_a);
    clean();
}

/* ---- A slow listener ------------------------------------------------------------------------ */

TEST_CASE("a listener that takes over 10 ms is logged once in a hundred, with its time", "[events][slow]")
{
    start();
    g.hold_ms = 25U;
    TEST_ASSERT_TRUE(fck_event_add_listener("module_found", listener_a));
    unsigned const before = g_log_warnings;
    /* Any hundred slow calls in a row hold exactly one line, whatever came before. */
    for (unsigned sent = 0U; sent < 100U; sent += 10U) {
        for (unsigned i = 0U; i < 10U; i++) {
            TEST_ASSERT_TRUE(fake_aos_news(FCK_AOS_EVENT_PRESENT, 0U));
        }
        for (unsigned t = 0U; (t < 400U) && (g.n_a < sent + 10U); t++) {
            vTaskDelay(pdMS_TO_TICKS(10U));
        }
    }
    TEST_ASSERT_EQUAL_UINT(100U, g.n_a);
    TEST_ASSERT_TRUE(wait_until(outside_a, 1000U));
    TEST_ASSERT_EQUAL_UINT_MESSAGE(before + 1U, g_log_warnings, "the first is said, the next 99 are not");
    TEST_ASSERT_NOT_NULL(strstr(g_log_last, "module_found"));
    TEST_ASSERT_NOT_NULL(strstr(g_log_last, "ms"));
    clean();
}

TEST_CASE("a quick listener says nothing", "[events]")
{
    start();
    TEST_ASSERT_TRUE(fck_event_add_listener("module_found", listener_a));
    unsigned const before = g_log_warnings;
    (void)fake_aos_news(FCK_AOS_EVENT_PRESENT, 0U);
    (void)fake_aos_news(FCK_AOS_EVENT_PRESENT, 0U);
    TEST_ASSERT_TRUE(wait_until(two_for_a, 1000U));
    TEST_ASSERT_EQUAL_UINT(before, g_log_warnings);
    clean();
}

/* ---- What a listener may call ---------------------------------------------------------------- */

TEST_CASE("a listener may call fck.h, but not fck_event_wait: it would wait for itself", "[events][listener]")
{
    start();
    g.call_wait = true;
    TEST_ASSERT_TRUE(fck_event_add_listener("module_lost", listener_a));
    (void)fake_aos_news(FCK_AOS_EVENT_GONE, 0U);
    TEST_ASSERT_TRUE(wait_until(one_for_a, 1000U));
    TEST_ASSERT_TRUE(wait_until(outside_a, 1000U));
    TEST_ASSERT_FALSE(g.wait_rc);
    TEST_ASSERT_NOT_NULL(strstr(g.why, "listener"));
    clean();
}

TEST_CASE("a listener may not call fck_end: it would stop the task it runs on", "[events][listener]")
{
    start();
    g.call_end = true;
    TEST_ASSERT_TRUE(fck_event_add_listener("module_lost", listener_a));
    unsigned const calls = g_core.calls;
    (void)fake_aos_news(FCK_AOS_EVENT_GONE, 0U);
    TEST_ASSERT_TRUE(wait_until(one_for_a, 1000U));
    TEST_ASSERT_TRUE(wait_until(outside_a, 1000U));
    TEST_ASSERT_FALSE(g.end_rc);
    TEST_ASSERT_NOT_NULL(strstr(g.why, "listener"));
    TEST_ASSERT_EQUAL_UINT_MESSAGE(calls, g_core.calls, "the core was not asked to end");
    clean();
}

/* ---- fck_event_wait --------------------------------------------------------------------------- */

typedef struct {
    const char *event;
    uint32_t    ms;
    fck_event_t out;
    bool        rc;
    uint32_t    took_ms;
    char        why[160];
    volatile bool done;
} waiter_t;

static void waiter_task(void *arg)
{
    waiter_t *const w = arg;
    uint32_t const t0 = fake_now_ms();
    w->rc             = fck_event_wait(w->event, w->ms, &w->out);
    w->took_ms        = fake_now_ms() - t0;
    (void)snprintf(w->why, sizeof(w->why), "%s", fck_error());
    w->done = true;
    vTaskDelete(NULL);
}

static void spawn(waiter_t *w, const char *event, uint32_t ms)
{
    (void)memset(w, 0, sizeof(*w));
    w->event = event;
    w->ms    = ms;
    TEST_ASSERT_EQUAL(pdPASS, xTaskCreate(waiter_task, "waiter", 4096, w, 3, NULL));
}

static waiter_t s_w[6];

static bool w0_done(void) { return s_w[0].done; }
static bool w1_done(void) { return s_w[1].done; }
static bool w4_done(void) { return s_w[4].done; }

TEST_CASE("fck_event_wait returns true with the event when it comes, and ends the wait there", "[events][wait]")
{
    start();
    spawn(&s_w[0], "packet_received", 5000U);
    vTaskDelay(pdMS_TO_TICKS(100U));
    TEST_ASSERT_FALSE(s_w[0].done);
    (void)fake_aos_news(FCK_AOS_EVENT_PACKET_RECEIVED, 9U);
    TEST_ASSERT_TRUE(wait_until(w0_done, 1000U));
    TEST_ASSERT_TRUE(s_w[0].rc);
    TEST_ASSERT_EQUAL_STRING("packet_received", s_w[0].out.name);
    TEST_ASSERT_EQUAL_INT32(9, s_w[0].out.value);
    TEST_ASSERT_TRUE_MESSAGE(s_w[0].took_ms < 1000U, "it did not wait for the time");
    TEST_ASSERT_EQUAL_STRING("", s_w[0].why);
    clean();
}

TEST_CASE("fck_event_wait is false after the time, and says the event did not happen", "[events][wait]")
{
    start();
    spawn(&s_w[0], "module_lost", 150U);
    TEST_ASSERT_TRUE(wait_until(w0_done, 2000U));
    TEST_ASSERT_FALSE(s_w[0].rc);
    TEST_ASSERT_NOT_NULL(strstr(s_w[0].why, "did not happen"));
    TEST_ASSERT_TRUE(s_w[0].took_ms >= 140U);
    clean();
}

TEST_CASE("only an event that arrives after the wait began counts", "[events][wait]")
{
    start();
    (void)fake_aos_news(FCK_AOS_EVENT_GONE, 0U);          /* before: nobody waited, nothing queued */
    spawn(&s_w[0], "module_lost", 200U);
    TEST_ASSERT_TRUE(wait_until(w0_done, 2000U));
    TEST_ASSERT_FALSE(s_w[0].rc);
    clean();
}

TEST_CASE("two tasks waiting for the same event are both woken by it; one for another event is not", "[events][wait]")
{
    start();
    spawn(&s_w[0], "module_found", 3000U);
    spawn(&s_w[1], "module_found", 3000U);
    spawn(&s_w[2], "module_lost", 300U);
    vTaskDelay(pdMS_TO_TICKS(100U));
    (void)fake_aos_news(FCK_AOS_EVENT_PRESENT, 0U);
    TEST_ASSERT_TRUE(wait_until(w0_done, 1000U));
    TEST_ASSERT_TRUE(wait_until(w1_done, 1000U));
    TEST_ASSERT_TRUE(s_w[0].rc);
    TEST_ASSERT_TRUE(s_w[1].rc);
    vTaskDelay(pdMS_TO_TICKS(400U));
    TEST_ASSERT_TRUE(s_w[2].done);
    TEST_ASSERT_FALSE(s_w[2].rc);
    clean();
}

TEST_CASE("fck_event_wait is false for a time of 0, a time over the limit, a name that is none or NULL, and before fck_begin", "[events][wait]")
{
    start();
    fck_event_t out;
    TEST_ASSERT_FALSE(fck_event_wait("module_found", 0U, &out));
    TEST_ASSERT_NOT_NULL(strstr(fck_error(), "ms is 0"));
    TEST_ASSERT_FALSE(fck_event_wait("module_found", FCK_AOS_CALL_MS_MAX + 1U, &out));
    TEST_ASSERT_FALSE(fck_event_wait("module_gone", 100U, &out));
    TEST_ASSERT_FALSE(fck_event_wait(NULL, 100U, &out));
    g_aos_started = false;
    TEST_ASSERT_FALSE(fck_event_wait("module_found", 100U, &out));
    TEST_ASSERT_NOT_NULL(strstr(fck_error(), "fck_begin() has not run"));
    g_aos_started = true;
}

TEST_CASE("fck_event_wait is refused from an Active Object and from a listener", "[events][wait][ao]")
{
    start();
    fake_aos_mark_ao(xTaskGetCurrentTaskHandle());
    TEST_ASSERT_FALSE(fck_event_wait("module_found", 100U, NULL));
    TEST_ASSERT_NOT_NULL(strstr(fck_error(), "fck_event_wait()"));
    TEST_ASSERT_NOT_NULL(strstr(fck_error(), "fck_sys_*()"));
    fake_aos_mark_ao(NULL);
}

TEST_CASE("fck_cancel ends a wait for an event: false, and cancelled", "[events][wait]")
{
    start();
    spawn(&s_w[0], "module_found", 10000U);
    vTaskDelay(pdMS_TO_TICKS(100U));
    TEST_ASSERT_FALSE(s_w[0].done);
    uint32_t const t0 = fake_now_ms();
    fake_aos_cancel();
    TEST_ASSERT_TRUE(wait_until(w0_done, 1000U));
    TEST_ASSERT_FALSE(s_w[0].rc);
    TEST_ASSERT_NOT_NULL(strstr(s_w[0].why, "cancelled"));
    TEST_ASSERT_TRUE_MESSAGE((fake_now_ms() - t0) < 500U, "within a step or two");
    clean();
}

TEST_CASE("more tasks than there are places wait: the fifth is false at once, and says so", "[events][wait]")
{
    start();
    for (unsigned i = 0U; i < 4U; i++) {
        spawn(&s_w[i], "module_found", 2000U);
    }
    vTaskDelay(pdMS_TO_TICKS(150U));
    spawn(&s_w[4], "module_found", 2000U);
    TEST_ASSERT_TRUE(wait_until(w4_done, 500U));
    TEST_ASSERT_FALSE(s_w[4].rc);
    TEST_ASSERT_NOT_NULL(strstr(s_w[4].why, "too many"));
    (void)fake_aos_news(FCK_AOS_EVENT_PRESENT, 0U);
    TEST_ASSERT_TRUE(wait_until(w0_done, 1000U));
    TEST_ASSERT_TRUE(s_w[0].rc);
    vTaskDelay(pdMS_TO_TICKS(100U));
    clean();
}

TEST_CASE("a place a wait gave up is free for the next", "[events][wait]")
{
    start();
    for (unsigned round = 0U; round < 6U; round++) {
        spawn(&s_w[0], "module_found", 60U);
        TEST_ASSERT_TRUE(wait_until(w0_done, 1000U));
        TEST_ASSERT_FALSE(s_w[0].rc);
        TEST_ASSERT_NOT_NULL(strstr(s_w[0].why, "did not happen"));
    }
    clean();
}

TEST_CASE("fck_event_wait with out NULL is allowed", "[events][wait]")
{
    start();
    spawn(&s_w[0], "module_found", 3000U);
    s_w[0].out.name = "untouched";
    vTaskDelay(pdMS_TO_TICKS(100U));
    (void)fake_aos_news(FCK_AOS_EVENT_PRESENT, 0U);
    TEST_ASSERT_TRUE(wait_until(w0_done, 1000U));
    TEST_ASSERT_TRUE(s_w[0].rc);
    clean();
}
