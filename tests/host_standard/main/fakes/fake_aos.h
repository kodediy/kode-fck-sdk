/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file fake_aos.h
 * @brief The owner under the standard layer: the per-task error, the sinks, the cancel count and
 *        kode-sdk's kode_init(), as far as fck_standard.c and fck_events.c use them.
 *
 * fake_aos_news() is what the owner does when something happens: it tells every sink, on the
 * calling task. A case plays the owner with it, and the sink the events task registered answers
 * as it would on the board.
 */
#ifndef FAKE_AOS_H_
#define FAKE_AOS_H_

#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "fck_aos.h"

/** @brief Whether fck_aos_start() ran (what fck_aos_status().started says). Default true. */
extern bool g_aos_started;

/** @brief kode_init() calls. */
extern unsigned g_kode_inits;

/** @brief Warnings and errors logged, and the last line. */
extern unsigned g_log_warnings;
extern char     g_log_last[160];

/** @brief Everything back to the start: no error, no sink, the owner started, nobody an AO's. */
void fake_aos_reset(void);

/** @brief Say that @p task is an Active Object's: fck_aos_refuse_ao() refuses it. NULL: nobody's. */
void fake_aos_mark_ao(TaskHandle_t task);

/** @brief fck_aos_cancel(), as the core's fck_core_cancel() does it. */
void fake_aos_cancel(void);

/** @brief The owner tells @p ev to every sink: the sink's answer (taken or not) comes back. */
bool fake_aos_news(fck_aos_event_t ev, uint32_t arg);

/** @brief Whether the events sink is set (the events task did it). */
bool fake_aos_sink_is_set(void);

/** @brief Milliseconds on the host's clock, for the cases' timings. */
uint32_t fake_now_ms(void);

#endif /* FAKE_AOS_H_ */
