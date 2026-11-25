/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file fck_events.h
 * @brief The standard layer's events: what fck_standard.c shares with fck_events.c. Not an app's header.
 */
#ifndef FCK_EVENTS_H_
#define FCK_EVENTS_H_

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Make the queue and the task of the listeners, and tell the owner to hand its events to
 *        them. Once; later calls return the first one's answer. Needs the heap for about 3.5 KB.
 * @return true when they run.
 */
bool fck_events_start(void);

/** @brief Whether the calling task is the listeners' task (the SDK's fck_events). */
bool fck_events_in_listener(void);

#ifdef __cplusplus
}
#endif

#endif /* FCK_EVENTS_H_ */
