/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file fake_kode.h
 * @brief What the example asked of kode-sdk, for a case to read, and what
 *        kode-sdk answers, for a case to set: the AO started and the log.
 */
#ifndef KODE_FAKE_H_
#define KODE_FAKE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "kode_sys.h"
#include "kode_log.h"

#define KODE_FAKE_LINES  16U   /**< Log lines kept, the oldest dropped.        */
#define KODE_FAKE_LINE   200U  /**< Characters kept of each.                   */

typedef struct {
    /* Set by the case; kode_fake_reset() leaves every answer KODE_OK. */
    void (*initial)(void);             /**< QACTIVE_START() calls it, as QP/C takes
                                        *   the initial transition there; NULL: none. */

    /* Written down. */
    unsigned      starts;              /**< QACTIVE_START()s.                          */
    QActive      *ao;                  /**< ... the AO,                                */
    uint8_t       prio;                /**< ... its level,                             */
    size_t        queue_len;           /**< ... its queue,                             */
    size_t        stack_bytes;         /**< ... and its stack.                         */
    char const   *task_name;           /**< QActive_setAttr(TASK_NAME_ATTR).           */
    unsigned      infos;               /**< kode_log() lines, by level.                */
    unsigned      warnings;
    unsigned      errors;
} kode_fake_t;

extern kode_fake_t g_kode;

/** @brief Every record cleared, every answer KODE_OK, the pool and the queue empty. */
void kode_fake_reset(void);

/** @brief Whether one of the last KODE_FAKE_LINES lines logged since
 *         kode_fake_reset() contains @p text. */
bool kode_fake_logged(const char *text);

/** @brief The line logged last; "" when none. */
const char *kode_fake_last_line(void);

#endif /* KODE_FAKE_H_ */
