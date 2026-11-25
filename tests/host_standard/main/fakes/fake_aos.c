/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file fake_aos.c
 * @brief fake_aos.h.
 */
#include "fake_aos.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "kode.h"
#include "kode_log.h"

bool     g_aos_started = true;
unsigned g_kode_inits;
unsigned g_log_warnings;
char     g_log_last[160];

#define ERROR_TEXT 160U

static char          s_text[ERROR_TEXT];
static fck_aos_why_t s_why;
static TaskHandle_t  s_ao_task;
static uint32_t      s_cancel_gen;
static fck_aos_sink_fn s_sink[FCK_AOS_SINKS];
static void           *s_sink_ctx[FCK_AOS_SINKS];

void fake_aos_reset(void)
{
    s_text[0]      = '\0';
    s_why          = FCK_AOS_WHY_NONE;
    s_ao_task      = NULL;
    g_aos_started  = true;
    g_kode_inits   = 0U;
    g_log_warnings = 0U;
    g_log_last[0]  = '\0';
}

void fake_aos_mark_ao(TaskHandle_t task)
{
    s_ao_task = task;
}

void fake_aos_cancel(void)
{
    s_cancel_gen++;
}

bool fake_aos_news(fck_aos_event_t ev, uint32_t arg)
{
    bool taken = true;
    if (s_sink[FCK_AOS_SINK_EVENTS] != NULL) {
        taken = s_sink[FCK_AOS_SINK_EVENTS](ev, arg, s_sink_ctx[FCK_AOS_SINK_EVENTS]);
    }
    return taken;
}

bool fake_aos_sink_is_set(void)
{
    return s_sink[FCK_AOS_SINK_EVENTS] != NULL;
}

uint32_t fake_now_ms(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

/* ---- kode-sdk's ------------------------------------------------------------------------- */

void kode_init(void)
{
    g_kode_inits++;
}

void fake_log_write(char level, const char *tag, const char *fmt, ...)
{
    char line[sizeof(g_log_last)];
    va_list ap;
    va_start(ap, fmt);
    (void)vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (level != 'I') {
        g_log_warnings++;
    }
    (void)snprintf(g_log_last, sizeof(g_log_last), "%s", line);
    (void)printf("[%c %s] %s\n", level, tag, line);
}

/* ---- The owner's ------------------------------------------------------------------------ */

bool fck_aos_refuse_ao(const char *call_name)
{
    if ((s_ao_task != NULL) && (xTaskGetCurrentTaskHandle() == s_ao_task)) {
        (void)snprintf(s_text, sizeof(s_text),
                       "%s was called from an Active Object (fake); fck.h calls block - call fck_sys_*() instead",
                       call_name);
        s_why = FCK_AOS_WHY_AO_CALLER;
        return true;
    }
    return false;
}

void fck_aos_error_ok(void)
{
    s_text[0] = '\0';
    s_why     = FCK_AOS_WHY_NONE;
}

void fck_aos_error_set(const char *why)
{
    (void)snprintf(s_text, sizeof(s_text), "%s", (why != NULL) ? why : "");
}

void fck_aos_error_set_for(kode_status_t st, const char *why)
{
    fck_aos_error_set(why);
    s_why = (st == KODE_OK) ? FCK_AOS_WHY_NONE : FCK_AOS_WHY_REFUSED;
}

const char *fck_aos_error(void)
{
    return s_text;
}

fck_aos_why_t fck_aos_error_why(void)
{
    return s_why;
}

void fck_aos_status(fck_aos_status_t *out)
{
    (void)memset(out, 0, sizeof(*out));
    out->started = g_aos_started;
}

uint32_t fck_aos_cancel_mark(void)
{
    return s_cancel_gen;
}

bool fck_aos_cancelled(uint32_t mark)
{
    return s_cancel_gen != mark;
}

void fck_aos_sink_set(fck_aos_sink_t which, fck_aos_sink_fn fn, void *ctx)
{
    if ((unsigned)which < (unsigned)FCK_AOS_SINKS) {
        s_sink[which]     = fn;
        s_sink_ctx[which] = ctx;
    }
}
