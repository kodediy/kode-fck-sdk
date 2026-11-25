/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file fake_aos.c
 * @brief fake_aos.h.
 */
#include "fake_aos.h"

#include <stdio.h>
#include <string.h>

volatile bool     g_aos_started = true;
volatile bool     g_aos_present = true;
volatile uint32_t g_aos_dropped[FCK_AOS_SINKS];
volatile uint32_t g_aos_cancels;

/* The error is the calling task's: an executor's reason is not the test task's. */
static _Thread_local char          t_text[160];
static _Thread_local fck_aos_why_t t_why;

static fck_aos_sink_fn s_sink[FCK_AOS_SINKS];
static void           *s_sink_ctx[FCK_AOS_SINKS];

void fake_aos_reset(void)
{
    g_aos_started = true;
    g_aos_present = true;
    g_aos_cancels = 0U;
    for (unsigned i = 0U; i < (unsigned)FCK_AOS_SINKS; i++) {
        g_aos_dropped[i] = 0U;
    }
    t_text[0] = '\0';
    t_why     = FCK_AOS_WHY_NONE;
}

void fake_aos_cancel(void)
{
    g_aos_cancels++;
}

bool fake_aos_news(fck_aos_event_t ev, uint32_t arg)
{
    if (s_sink[FCK_AOS_SINK_NEWS] == NULL) {
        return true;
    }
    return s_sink[FCK_AOS_SINK_NEWS](ev, arg, s_sink_ctx[FCK_AOS_SINK_NEWS]);
}

bool fake_aos_news_sink_is_set(void)
{
    return s_sink[FCK_AOS_SINK_NEWS] != NULL;
}

void fake_aos_error(kode_status_t st, fck_aos_why_t why, const char *text)
{
    (void)snprintf(t_text, sizeof(t_text), "%s", (text != NULL) ? text : "");
    t_why = (st == KODE_OK) ? FCK_AOS_WHY_NONE : why;
}

const char *fake_aos_error_text(void)
{
    return t_text;
}

/* ---- fck_aos.h, as far as fck_sys.c reaches it --------------------------------------------- */

void fck_aos_error_ok(void)
{
    t_text[0] = '\0';
    t_why     = FCK_AOS_WHY_NONE;
}

void fck_aos_error_set_for(kode_status_t st, const char *why)
{
    (void)snprintf(t_text, sizeof(t_text), "%s", (why != NULL) ? why : "");
    /* The owner classes by status and text; the one text that matters here is the cancelled one. */
    t_why = (st == KODE_OK) ? FCK_AOS_WHY_NONE
          : ((strstr(t_text, "cancelled") != NULL) ? FCK_AOS_WHY_CANCELLED : FCK_AOS_WHY_REFUSED);
}

fck_aos_why_t fck_aos_error_why(void)
{
    return t_why;
}

void fck_aos_status(fck_aos_status_t *out)
{
    (void)memset(out, 0, sizeof(*out));
    out->started      = g_aos_started;
    out->present      = g_aos_present;
    out->news_dropped = g_aos_dropped[FCK_AOS_SINK_NEWS];
    out->events_dropped = g_aos_dropped[FCK_AOS_SINK_EVENTS];
    out->answers_dropped = g_aos_dropped[FCK_AOS_SINK_ANSWERS];
}

bool fck_aos_present(void)
{
    return g_aos_present;
}

uint32_t fck_aos_cancel_mark(void)
{
    return g_aos_cancels;
}

bool fck_aos_cancelled(uint32_t mark)
{
    return g_aos_cancels != mark;
}

void fck_aos_sink_set(fck_aos_sink_t which, fck_aos_sink_fn fn, void *ctx)
{
    if ((unsigned)which < (unsigned)FCK_AOS_SINKS) {
        s_sink[which]     = fn;
        s_sink_ctx[which] = ctx;
    }
}

void fck_aos_news(fck_aos_event_t ev, uint32_t arg)
{
    /* The owner tells every sink; the extended layer's end_task is the only caller here. */
    (void)fake_aos_news(ev, arg);
}

void fck_aos_dropped_add(fck_aos_sink_t which, uint32_t n)
{
    if ((unsigned)which < (unsigned)FCK_AOS_SINKS) {
        g_aos_dropped[which] += n;
    }
}
