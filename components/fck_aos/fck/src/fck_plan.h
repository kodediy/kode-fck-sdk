/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file fck_plan.h
 * @brief The owner's rules, in plain C: who the chip at the expander's address
 *        is, how often a header that holds no F*CK is asked again, when the
 *        module has left, what a command line may be, and what a caller
 *        waiting for a GNSS fix is told.
 *
 * This decides; it touches no chip, no bus and no clock. The owner gives it
 * what it read and what it counted, and it answers. Private to this
 * component. It is built on the host and run as it is (tools/check_layers.sh,
 * rule 4; tests/host).
 */
#ifndef FCK_PLAN_H_
#define FCK_PLAN_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "kode_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Asks in a row with no answer after which the module has left. */
#define FCK_PLAN_GONE_MISSES    2U
/** @brief Clean NAKs (or other chips) that call a header "not an F*CK". */
#define FCK_PLAN_RULEOUT_LOOKS  2U
/** @brief Looks between two asks of a header that was ruled out. */
#define FCK_PLAN_REASK_LOOKS    10U
/** @brief The longest command line, its terminator not counted. */
#define FCK_PLAN_COMMAND_LINE_MAX 255U

/** @brief What answered at the expander's address. */
typedef enum {
    FCK_PLAN_ID_FCK,      /**< The F*CK's expander.                              */
    FCK_PLAN_ID_OTHER,    /**< A chip that is not: it does not keep a register.  */
    FCK_PLAN_ID_UNSURE,   /**< Nothing could be asked, or the answers stopped.   */
} fck_plan_ident_t;

/**
 * @brief Whether the chip whose INPUT register read @p input is the F*CK's
 *        expander: five of its pins are held high by the module's resistors
 *        (FCK_BSP_EXPANDER_PULLUPS_MASK), so a chip that reads none of them
 *        high echoes the register byte or has its pins held low.
 * @return FCK_PLAN_ID_FCK or FCK_PLAN_ID_OTHER.
 */
fck_plan_ident_t fck_plan_identify(uint8_t input);

/**
 * @brief Whether the chip kept the power-loss mark @p mark written to its
 *        POLARITY register, when it read back @p read_back: only one that
 *        keeps registers does.
 * @return FCK_PLAN_ID_FCK or FCK_PLAN_ID_OTHER.
 */
fck_plan_ident_t fck_plan_canary_kept(uint8_t mark, uint8_t read_back);

/**
 * @brief Whether the power-loss mark is gone from POLARITY (@p polarity as
 *        read): everything on the module restarted.
 */
bool fck_plan_power_lost(uint8_t polarity);

/**
 * @brief Whether a header that holds the I2C pair is left alone this look: a
 *        module ruled out (@p agreed looks in a row), or whose bus misbehaves
 *        (@p troubled), is asked again only now and then (@p reask looks left).
 */
bool fck_plan_look_waits(uint32_t agreed, bool troubled, uint32_t reask);

/** @brief Whether @p agreed looks in a row are enough to call it "not an F*CK". */
bool fck_plan_ruled_out(uint32_t agreed);

/** @brief Whether @p misses asks in a row with no answer mean the module left. */
bool fck_plan_gone(uint32_t misses);

/**
 * @brief Whether @p line is one command and nothing more: 1 to
 *        FCK_PLAN_COMMAND_LINE_MAX printable characters, then at most the
 *        '\n' a builder ends it with. A '\n' or '\r' inside would reach the
 *        firmware as a second command that nothing checked.
 */
bool fck_plan_command_line_ok(const char *line);

/**
 * @brief What a caller waiting for a GNSS fix is told, from what the owner
 *        holds at that moment.
 * @param first        The first look of the call (before it, no module is a
 *                     refusal; after it, a module that went is an I/O error).
 * @param latest       Any fix will do, even one made before the call.
 * @param present      A module is set up.
 * @param same_module  It is the one the call began with (same generation).
 * @param running      The receiver is out of reset.
 * @param have         A fix is held.
 * @param newer        It was made after the call began.
 * @return KODE_OK: take the fix; KODE_TIMEOUT: none yet, look again;
 *         KODE_DENIED: no module, or the receiver is off; KODE_IO_ERROR: the
 *         module left or was replaced on the way.
 */
kode_status_t fck_plan_fix_verdict(bool first, bool latest, bool present, bool same_module,
                                   bool running, bool have, bool newer);

/**
 * @brief How old a fix is, from the clock's ticks.
 *
 * The tick counter wraps and the subtraction wraps with it, so an age is right
 * for as long as it is under one turn of the counter; what does not fit the
 * answer is said as the longest it can say, never as a short age.
 *
 * @param now     The tick count now; read after @p at was, so never before it.
 * @param at      The tick count when the fix was completed.
 * @param tick_hz Ticks in a second.
 * @return The age in ms; UINT32_MAX for one that does not fit, or when
 *         @p tick_hz is 0.
 */
uint32_t fck_plan_fix_age_ms(uint32_t now, uint32_t at, uint32_t tick_hz);

#ifdef __cplusplus
}
#endif

#endif /* FCK_PLAN_H_ */
