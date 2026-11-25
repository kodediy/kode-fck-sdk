/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file fake_aos.h
 * @brief The owner under the extended layer, as far as fck_sys.c uses it: the per-task error and its
 *        class, the cancel marks, the two sinks, the drop counters and whether the module is there.
 *
 * fake_aos_news() is what the owner does when something happens: it tells the news sink fck_sys
 * set, on the calling task, and the sink's answer (taken or not) comes back for the owner to count.
 */
#ifndef FAKE_AOS_H_
#define FAKE_AOS_H_

#include <stdbool.h>
#include <stdint.h>

#include "fck_aos.h"

/** @brief Whether fck_aos_start() ran (what fck_aos_status().started says). */
extern volatile bool g_aos_started;
/** @brief What fck_aos_present() says. */
extern volatile bool g_aos_present;
/** @brief The drops fck_aos_dropped_add() was told, per sink. */
extern volatile uint32_t g_aos_dropped[FCK_AOS_SINKS];
/** @brief fck_aos_cancel() calls (each one a new mark). */
extern volatile uint32_t g_aos_cancels;

/** @brief Everything back to the start: begun, the module present, no drops, no cancels. */
void fake_aos_reset(void);

/** @brief fck_aos_cancel(), as the core's fck_core_cancel() does it: every call that began before is cut. */
void fake_aos_cancel(void);

/** @brief The owner tells @p ev to the news sink: its answer (taken or not) comes back. true with no sink. */
bool fake_aos_news(fck_aos_event_t ev, uint32_t arg);

/** @brief Whether the news sink is set (fck_sys did it). */
bool fake_aos_news_sink_is_set(void);

/** @brief Set the calling task's error as the real core does when a call fails: status, class and text. */
void fake_aos_error(kode_status_t st, fck_aos_why_t why, const char *text);

/** @brief The calling task's error text, and its class. */
const char *fake_aos_error_text(void);

#endif /* FAKE_AOS_H_ */
