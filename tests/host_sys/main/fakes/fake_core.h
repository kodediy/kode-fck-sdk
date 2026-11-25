/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file fake_core.h
 * @brief The core under the extended layer: every call fck_sys.c makes (fck_core.h), written down and
 *        answered as a case scripts it.
 *
 * fck_sys.c is the real extended layer; this stands in for the core (fck.c, fck_wifi.c, ...). The
 * calls a request is carried out with (the ones that wait on the board) hold while g_core.hold is
 * set, and are cut by the cancel marks as the real ones are, so a case can keep an executor busy,
 * fill the queue behind it, cancel, and let it go. A call that works writes what it was lent a
 * buffer for: every pointer it is given is filled with 0x5A in its first element, and the scans
 * write their lists, so that a case sees what the answer's count and the buffers say.
 */
#ifndef FAKE_CORE_H_
#define FAKE_CORE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "kode_defs.h"

#include "fck.h"
#include "fck_aos.h"

#define FAKE_CORE_ARGS 8U
#define FAKE_CORE_LOG  256U

typedef struct {
    const char *name;                 /**< Without the fck_core_ prefix.   */
    unsigned    nargs;
    uint64_t    arg[FAKE_CORE_ARGS];  /**< Each argument cast to uintptr_t. */
} fake_call_t;

typedef struct {
    volatile kode_status_t next;      /**< What every call answers.                                        */
    volatile fck_aos_why_t why;       /**< Its class when it fails.                                        */
    volatile bool          hold;      /**< The calls that wait on the board (and the end) wait while this is set. */
    const char *volatile   hold_name; /**< ... only the call of this name (without the prefix); NULL: all.  */
    volatile unsigned      inside;    /**< How many of them are waiting or running now.                    */
    volatile unsigned      calls;     /**< Every call since fake_core_reset().                             */
    fake_call_t            log[FAKE_CORE_LOG];
    size_t                 out_n;     /**< The count a call that writes a list reports.                    */
    size_t                 wifi_n;    /**< The networks a Wi-Fi scan finds.                                */
    size_t                 ble_n;     /**< The devices a BLE scan finds.                                   */
    unsigned               n_lines;   /**< A command's reply: this many tagged lines ...                   */
    struct { const char *tag; const char *body; } line[4];
    int32_t                cmd_rc;    /**< ... and this #DONE rc.                                          */
    char                   cmd_line[256];  /**< A command's line, verb and time, as the core got them. */
    char                   cmd_verb[32];
    bool                   cmd_has_verb;
    uint32_t               cmd_ms;
    fck_subghz_code_t      code;      /**< A code, a packet's settings, bytes and length, as the core got them. */
    fck_subghz_packet_settings_t settings;
    uint8_t                bytes[FCK_SUBGHZ_PACKET_MAX];
    uint8_t                length;
    bool                   present;   /**< fck_is_present().                                               */
    bool                   may;       /**< fck_subghz_may_transmit().                                      */
    bool                   rolling;   /**< fck_subghz_is_rolling().                                        */
    uint32_t               hz;        /**< fck_subghz_listen_hz().                                         */
} fake_core_t;

extern fake_core_t g_core;

/** @brief Nothing asked, everything answered KODE_OK, nothing held, a module present. */
void fake_core_reset(void);

/** @brief How many times the call @p name (without the prefix) was made. */
unsigned fake_core_count(const char *name);

/** @brief The @p nth (from 0) call of @p name, or NULL. */
const fake_call_t *fake_core_call(const char *name, unsigned nth);

#endif /* FAKE_CORE_H_ */
