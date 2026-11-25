/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file fake_fck.h
 * @brief fck_sys.h as examples/console_actions reaches it: each call it makes
 *        written down, refused as the SDK refuses an argument, and answered
 *        with what the case set.
 *
 * The real fck_sys.h is the header (its signals, its answer type); what answers it is
 * fake_fck.c. fck_sys_is_present() and fck_sys_status_read(), the two immediate reads, give
 * g_fck.st. A request returns at once with g_fck.answer; the module's answer to it is an event
 * the case delivers to the Active Object itself.
 */
#ifndef FCK_MOCK_H_
#define FCK_MOCK_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "fck_sys.h"

/** @brief fck_sys.h's requests, the ones the example's rows ask for. */
typedef enum {
    FCK_CALL_NONE = 0,
    FCK_CALL_FIRMWARE,
    FCK_CALL_POWER,
    FCK_CALL_RESET,
    FCK_CALL_GNSS_START,
    FCK_CALL_GNSS_STOP,
    FCK_CALL_BLE_SCAN,
    FCK_CALL_RESCUE_PROBE
} fck_call_t;

typedef struct {
    /* Set by the case; fck_mock_reset() leaves every answer KODE_OK. */
    fck_status_t        st;           /**< What fck_sys_is_present() and fck_sys_status_read() say. */
    kode_status_t       begin_answer; /**< fck_sys_begin()'s.                        */
    kode_status_t       subscribe_answer; /**< fck_sys_subscribe()'s.                */
    kode_status_t       answer;       /**< A request's, its arguments good.          */

    /* Written down. */
    unsigned  begins;                 /**< fck_sys_begin()s.                         */
    unsigned  subscribes;             /**< fck_sys_subscribe()s.                     */
    QActive  *subscriber;             /**< ... for this AO.                          */
    unsigned  calls;                  /**< Requests, refused ones too.               */
    unsigned  bad_args;               /**< ... refused for an argument the SDK refuses. */
    struct {
        fck_call_t call;
        QActive   *me;                /**< The asking AO.                            */
        fck_chip_t chip;
        bool       on;
        uint8_t    secs;
        const void *lent;             /**< The buffer lent, NULL when none.          */
    } last;                           /**< The request made last.                    */
} fck_mock_t;

extern fck_mock_t g_fck;

/** @brief No module, nothing called, every answer KODE_OK. */
void fck_mock_reset(void);

#endif /* FCK_MOCK_H_ */
