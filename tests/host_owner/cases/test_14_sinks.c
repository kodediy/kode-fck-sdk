/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_14_sinks.c
 * @brief Who the owner tells: the two sinks (fck.h's events, fck_sys.h's news). A sink is told on
 *        the task that raised the event, so a slow one is logged (one in a hundred), one that had no
 *        room is counted, and every sink is told, each its own way.
 */
#include <string.h>

#include "unity.h"

#include "fck.h"
#include "fck_core.h"
#include "fck_aos.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "fake_world.h"

static struct {
    volatile unsigned n;          /* events told                          */
    volatile uint32_t hold_ms;    /* how long each one takes              */
    volatile bool     take;       /* whether the sink says it was taken   */
    volatile unsigned news_n;     /* the second sink's                    */
    volatile fck_aos_event_t news_last;
} g;

static bool slow_sink(fck_aos_event_t ev, uint32_t arg, void *ctx)
{
    (void)ev;
    (void)arg;
    (void)ctx;
    g.n++;
    if (g.hold_ms > 0U) {
        vTaskDelay(pdMS_TO_TICKS(g.hold_ms));
    }
    return g.take;
}

static bool news_sink(fck_aos_event_t ev, uint32_t arg, void *ctx)
{
    (void)arg;
    (void)ctx;
    g.news_n++;
    g.news_last = ev;
    return true;
}

static void arm(uint32_t hold_ms, bool take)
{
    (void)memset((void *)&g, 0, sizeof(g));
    g.hold_ms = hold_ms;
    g.take    = take;
    fck_aos_sink_set(FCK_AOS_SINK_EVENTS, slow_sink, NULL);
}

static void disarm(void)
{
    fck_aos_sink_set(FCK_AOS_SINK_EVENTS, NULL, NULL);
    fck_aos_sink_set(FCK_AOS_SINK_NEWS, NULL, NULL);
}

TEST_CASE("a sink that takes over 10 ms is logged once in a hundred, with its time", "[sinks]")
{
    sim_begin_fck(NULL, NULL);
    sim_log_clear();
    arm(40U, true);
    fck_status_t info;
    fck_core_status(&info);
    sim_blip();                                  /* GONE, then PRESENT: two events */
    TEST_ASSERT_TRUE(sim_wait_generation(info.generation + 1U, 4000U));
    sim_sleep(600U);
    disarm();
    TEST_ASSERT_TRUE_MESSAGE(g.n >= 2U, "both events reached the sink");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, sim_log_count("the event sink"), "the first is said, the next 99 are not");
    TEST_ASSERT_TRUE(sim_log_has("must only queue or post"));
}

TEST_CASE("a quick sink says nothing", "[sinks]")
{
    sim_begin_fck(NULL, NULL);
    sim_log_clear();
    arm(0U, true);
    fck_status_t info;
    fck_core_status(&info);
    sim_blip();
    TEST_ASSERT_TRUE(sim_wait_generation(info.generation + 1U, 4000U));
    sim_sleep(300U);
    disarm();
    TEST_ASSERT_TRUE(g.n >= 2U);
    TEST_ASSERT_EQUAL_UINT(0U, sim_log_count("the event sink"));
}

TEST_CASE("a sink that had no room is counted as dropped, in its own counter", "[sinks]")
{
    sim_begin_fck(NULL, NULL);
    fck_aos_status_t before;
    fck_aos_status(&before);
    arm(0U, false);
    fck_aos_sink_set(FCK_AOS_SINK_NEWS, news_sink, NULL);
    fck_status_t info;
    fck_core_status(&info);
    sim_blip();
    TEST_ASSERT_TRUE(sim_wait_generation(info.generation + 1U, 4000U));
    sim_sleep(300U);
    fck_aos_status_t after;
    fck_aos_status(&after);
    disarm();
    TEST_ASSERT_TRUE(g.n >= 2U);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(before.events_dropped + g.n, after.events_dropped, "one drop for each event refused");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(before.news_dropped, after.news_dropped, "the other sink took its own");
}

TEST_CASE("both sinks are told every event, each by its own function", "[sinks]")
{
    sim_begin_fck(NULL, NULL);
    arm(0U, true);
    fck_aos_sink_set(FCK_AOS_SINK_NEWS, news_sink, NULL);
    fck_status_t info;
    fck_core_status(&info);
    sim_blip();
    TEST_ASSERT_TRUE(sim_wait_generation(info.generation + 1U, 4000U));
    sim_sleep(300U);
    disarm();
    TEST_ASSERT_TRUE(g.n >= 2U);
    TEST_ASSERT_EQUAL_UINT(g.n, g.news_n);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)FCK_AOS_EVENT_PRESENT, (uint32_t)g.news_last);
}

TEST_CASE("an event nobody listens for is told to nobody, and counted as nothing", "[sinks]")
{
    sim_begin_fck(NULL, NULL);
    disarm();
    fck_aos_status_t before;
    fck_aos_status(&before);
    fck_aos_news(FCK_AOS_EVENT_SEND_CANCELLED, FCK_AOS_CANCEL_CALL);
    fck_aos_status_t after;
    fck_aos_status(&after);
    TEST_ASSERT_EQUAL_UINT32(before.events_dropped, after.events_dropped);
    TEST_ASSERT_EQUAL_UINT32(before.news_dropped, after.news_dropped);
}

TEST_CASE("fck_aos_dropped_add counts for the sink asked, and ignores one that does not exist", "[sinks]")
{
    sim_begin_fck(NULL, NULL);
    fck_aos_status_t before;
    fck_aos_status(&before);
    fck_aos_dropped_add(FCK_AOS_SINK_NEWS, 3U);
    fck_aos_dropped_add(FCK_AOS_SINK_EVENTS, 1U);
    fck_aos_dropped_add(FCK_AOS_SINK_ANSWERS, 2U);
    fck_aos_dropped_add((fck_aos_sink_t)(FCK_AOS_SINKS + 5), 9U);
    fck_aos_status_t after;
    fck_aos_status(&after);
    TEST_ASSERT_EQUAL_UINT32(before.news_dropped + 3U, after.news_dropped);
    TEST_ASSERT_EQUAL_UINT32(before.events_dropped + 1U, after.events_dropped);
    TEST_ASSERT_EQUAL_UINT32(before.answers_dropped + 2U, after.answers_dropped);
}
