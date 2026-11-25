/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file fck_drv_subghz_duty.h
 * @brief How long the module has been on air in the last hour, per band,
 *        and whether one more transmission may go: the band's limit on air
 *        time.
 *
 * The module is on air at most 10 % of any hour in 433.05-434.79 MHz
 * (360 s) and 1 % in 868.0-868.6 MHz (36 s), and the SDK keeps the account
 * itself.
 *
 * Each band keeps the air time of the last hour in a ring of one-minute
 * buckets: FCK_DRV_SUBGHZ_DUTY_BUCKETS of them, the 60 of the hour and the
 * one being filled, so that a transmission is counted for a full hour
 * whatever second of its minute it began (it leaves the account 60 to 61
 * minutes after, never before). A transmission is charged whole, to the
 * minute it began in.
 *
 * Plain C, like fck_drv_ook: no clock of its own. The caller passes
 * kode_sys_time_ms() (milliseconds since boot, wrapping after 49 days) with
 * each call, so tests/host drives time; the account follows the clock across
 * its wrap, and a clock that stands still or goes a little back (a caller
 * that stamped a transmission before another's) is the same minute.
 * Nothing survives a restart: after one, the hour is empty.
 */
#ifndef FCK_DRV_SUBGHZ_DUTY_H_
#define FCK_DRV_SUBGHZ_DUTY_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** The two bands with an air-time limit, and how long the module may be on air in each,
 *  in milliseconds of any hour: 10 % and 1 %. */
#define FCK_DRV_SUBGHZ_DUTY_BANDS          2U
#define FCK_DRV_SUBGHZ_DUTY_433_LIMIT_MS   360000U
#define FCK_DRV_SUBGHZ_DUTY_868_LIMIT_MS   36000U
/** One bucket's length, and how many make the ring. */
#define FCK_DRV_SUBGHZ_DUTY_BUCKET_MS      60000U
#define FCK_DRV_SUBGHZ_DUTY_BUCKETS        61U

/** @brief The account, kept by the caller: zeroed (a static is) it is an empty
 *         hour. About 500 bytes. */
typedef struct {
    bool     started;      /**< The clock below has been read once.            */
    uint32_t seen_ms;      /**< The last clock reading, to follow its wrap.    */
    uint64_t now_ms;       /**< The clock, unwrapped.                          */
    uint64_t head_bucket;  /**< The bucket being filled, by number since the clock began. */
    uint32_t air_ms[FCK_DRV_SUBGHZ_DUTY_BANDS][FCK_DRV_SUBGHZ_DUTY_BUCKETS];
} fck_drv_subghz_duty_t;

/** @brief The air time of a signal's runs, @p repeats times, in
 *         milliseconds, rounded up: the whole signal, the carrier's off
 *         runs as well as its on ones, which is the conservative count -
 *         the chip stays in TX for all of it. UINT32_MAX when it does not fit. */
uint32_t fck_drv_subghz_duty_signal_ms(const int32_t *runs, size_t n, uint8_t repeats);

/** @brief How much of @p hz's band is left of the hour, in milliseconds.
 *  @return false, @p left_ms untouched, for a frequency outside both bands. */
bool fck_drv_subghz_duty_left(fck_drv_subghz_duty_t *duty, uint32_t now_ms, uint32_t hz, uint32_t *left_ms);

/** @brief Whether @p air_ms more on air in @p hz's band fits the hour: it is
 *         no more than fck_drv_subghz_duty_left(). false outside both bands. */
bool fck_drv_subghz_duty_may(fck_drv_subghz_duty_t *duty, uint32_t now_ms, uint32_t hz, uint32_t air_ms);

/** @brief Charge @p air_ms to @p hz's band, in the minute of @p now_ms. The
 *         account never overflows: past what a bucket holds it stays full.
 *  @return false for a frequency outside both bands, nothing charged. */
bool fck_drv_subghz_duty_record(fck_drv_subghz_duty_t *duty, uint32_t now_ms, uint32_t hz, uint32_t air_ms);

#ifdef __cplusplus
}
#endif

#endif /* FCK_DRV_SUBGHZ_DUTY_H_ */
