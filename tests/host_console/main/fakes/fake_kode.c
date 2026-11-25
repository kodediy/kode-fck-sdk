/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file fake_kode.c
 * @brief See fake_kode.h.
 */
#include "fake_kode.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

kode_fake_t g_kode;

static char        s_line[KODE_FAKE_LINES][KODE_FAKE_LINE];
static size_t      s_lines;   /* logged since the reset; the last KODE_FAKE_LINES kept */

void kode_fake_reset(void)
{
    (void)memset(&g_kode, 0, sizeof(g_kode));
    s_lines  = 0U;
}

void kode_fake_start(QActive * const me, uint8_t prio, size_t queue_len, size_t stack_bytes)
{
    g_kode.starts++;
    g_kode.ao          = me;
    g_kode.prio        = prio;
    g_kode.queue_len   = queue_len;
    g_kode.stack_bytes = stack_bytes;
    if (g_kode.initial != NULL) {
        g_kode.initial();
    }
}

QState QHsm_top(void * const me, QEvt const * const e)
{
    (void)me;
    (void)e;
    return Q_HANDLED();
}

void QActive_ctor(QActive * const me, QStateHandler const initial)
{
    me->state = initial;
}

void QActive_setAttr(QActive * const me, uint32_t attr, void const * const value)
{
    (void)me;
    if (attr == TASK_NAME_ATTR) {
        g_kode.task_name = value;
    }
}

void kode_log(kode_log_lvl_t lvl, const char *tag, const char *fmt, ...)
{
    char *const line = s_line[s_lines % KODE_FAKE_LINES];
    va_list ap;
    va_start(ap, fmt);
    (void)vsnprintf(line, KODE_FAKE_LINE, fmt, ap);
    va_end(ap);
    s_lines++;
    if (lvl >= KODE_LOG_ERROR) {
        g_kode.errors++;
    } else if (lvl >= KODE_LOG_WARN) {
        g_kode.warnings++;
    } else {
        g_kode.infos++;
    }
    (void)printf("[%s] %s\n", tag, line);
}

bool kode_fake_logged(const char *text)
{
    size_t const kept = (s_lines < KODE_FAKE_LINES) ? s_lines : KODE_FAKE_LINES;
    for (size_t k = 0U; k < kept; k++) {
        if (strstr(s_line[(s_lines - 1U - k) % KODE_FAKE_LINES], text) != NULL) {
            return true;
        }
    }
    return false;
}

const char *kode_fake_last_line(void)
{
    return (s_lines == 0U) ? "" : s_line[(s_lines - 1U) % KODE_FAKE_LINES];
}
