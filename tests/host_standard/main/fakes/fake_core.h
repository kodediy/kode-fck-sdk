/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file fake_core.h
 * @brief The core under the standard layer, written down and answered: every fck_core_*() call
 *        records its name and its arguments and answers what a case set.
 *
 * fck_standard.c and fck_events.c are the real standard layer; this stands in for the core
 * (fck.c, fck_wifi.c, ...) and for the owner under it (fake_aos.c). A case sets what the core
 * answers (g_core.next), calls an fck.h function, and reads back which core call it made and with
 * what: that each bool call is one core call, with its arguments, and that its answer is the
 * core's, as true or false with the reason in fck_error().
 */
#ifndef FAKE_CORE_H_
#define FAKE_CORE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "kode_defs.h"

#define FAKE_CORE_ARGS_MAX 8U

typedef struct {
    kode_status_t next;                    /**< What every core call answers.                      */
    unsigned      calls;                   /**< Core calls since fake_core_reset().                */
    const char   *last;                    /**< The last one's name, without the fck_core_ prefix.  */
    unsigned      nargs;                   /**< Its argument count ...                              */
    uint64_t      arg[FAKE_CORE_ARGS_MAX]; /**< ... and its arguments, each cast to uintptr_t.      */
    bool          present;                 /**< fck_is_present().                                   */
    bool          may;                     /**< fck_subghz_may_transmit().                          */
    bool          rolling;                 /**< fck_subghz_is_rolling().                            */
    uint32_t      hz;                      /**< fck_subghz_listen_hz().                             */
} fake_core_t;

extern fake_core_t g_core;

/** @brief Nothing asked, everything answered KODE_OK, a module present. */
void fake_core_reset(void);

#endif /* FAKE_CORE_H_ */
