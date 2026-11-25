/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file subghz_arm.h
 * @brief The console's arming for the rows that transmit: given by hand, for a while, and
 *        taken back by a stop, by the module leaving, or by the time running out.
 *
 * Plain C over a clock the caller reads, so tests/host_console runs it as it is. The rows of
 * subghz_main.c ask it before a send, and again in the task just before the call, since a
 * request waits for the end of a round.
 *
 * Several tasks touch one subghz_arm_t - the console's (fck_arm, fck_sg_stop), the SDK's events
 * task (the listener of module_lost) and the app's own - so its fields are volatile and each
 * operation writes the disarming first: a stop that races an arming leaves it disarmed or armed
 * from after the stop, never half of each.
 */
#ifndef SUBGHZ_ARM_H_
#define SUBGHZ_ARM_H_

#include <stdbool.h>
#include <stdint.h>

/** @brief How long an arming lasts: long enough to press a remote and type fck_sg_send twice. */
#define SUBGHZ_ARM_MS 60000U

/** @brief Whether transmitting is allowed, until when, and how many cuts there have been. */
typedef struct {
    volatile bool     on;        /**< fck_arm 1 was given, and nothing took it back.       */
    volatile uint32_t until_ms;  /**< When it runs out, in the caller's clock (wraps).     */
    volatile uint32_t cuts;      /**< One more for every disarm: a request carries it.     */
} subghz_arm_t;

/**
 * @brief fck_arm: arm for SUBGHZ_ARM_MS from @p now_ms (again, to extend), or disarm.
 */
void subghz_arm_set(subghz_arm_t *a, bool on, uint32_t now_ms);

/**
 * @brief A stop, or the module leaving: disarmed at once, and every request taken before it
 *        no longer holds.
 */
void subghz_arm_cut(subghz_arm_t *a);

/**
 * @brief Whether transmitting is allowed at @p now_ms: armed, and not run out.
 */
bool subghz_arm_ok(const subghz_arm_t *a, uint32_t now_ms);

/**
 * @brief The cuts so far, for a request to carry when its row posts it.
 */
uint32_t subghz_arm_cuts(const subghz_arm_t *a);

/**
 * @brief Whether a request that carried @p cuts may still go at @p now_ms: allowed now, and no
 *        stop, leave or disarm since the row posted it.
 */
bool subghz_arm_holds(const subghz_arm_t *a, uint32_t cuts, uint32_t now_ms);

#endif /* SUBGHZ_ARM_H_ */
