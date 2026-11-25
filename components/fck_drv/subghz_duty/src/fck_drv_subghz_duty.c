/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file fck_drv_subghz_duty.c
 * @brief The module's air time, hour by hour. See fck_drv_subghz_duty.h.
 */
#include "fck_drv_subghz_duty.h"

#include <string.h>

#include "fck_bsp.h"

#define BAND_433 0U
#define BAND_868 1U

/* The band @p hz is in, as an index; false outside both. */
static bool duty_band(uint32_t hz, size_t *band)
{
    if ((hz >= FCK_BSP_TX_433_MIN_HZ) && (hz <= FCK_BSP_TX_433_MAX_HZ)) {
        *band = BAND_433;
        return true;
    }
    if ((hz >= FCK_BSP_TX_868_MIN_HZ) && (hz <= FCK_BSP_TX_868_MAX_HZ)) {
        *band = BAND_868;
        return true;
    }
    return false;
}

static void duty_clear(fck_drv_subghz_duty_t *duty, uint64_t bucket)
{
    size_t const slot = (size_t)(bucket % FCK_DRV_SUBGHZ_DUTY_BUCKETS);
    for (size_t b = 0U; b < FCK_DRV_SUBGHZ_DUTY_BANDS; b++) {
        duty->air_ms[b][slot] = 0U;
    }
}

/* Move the account on to @p now_ms: the clock unwrapped, and every bucket
 * the clock has left behind emptied. */
static void duty_advance(fck_drv_subghz_duty_t *duty, uint32_t now_ms)
{
    if (!duty->started) {
        duty->started     = true;
        duty->seen_ms     = now_ms;
        duty->now_ms      = now_ms;
        duty->head_bucket = duty->now_ms / FCK_DRV_SUBGHZ_DUTY_BUCKET_MS;
        return;
    }
    /* Modular, so across the wrap it is the time that passed. A reading a
     * little behind the last (more than half the range "ahead") is the same
     * instant: the clock only goes forward. */
    uint32_t const delta = now_ms - duty->seen_ms;
    if (delta <= (UINT32_MAX / 2U)) {
        duty->seen_ms = now_ms;
        duty->now_ms += delta;
    }
    uint64_t const bucket = duty->now_ms / FCK_DRV_SUBGHZ_DUTY_BUCKET_MS;
    if (bucket > duty->head_bucket) {
        uint64_t const gap = bucket - duty->head_bucket;
        if (gap >= FCK_DRV_SUBGHZ_DUTY_BUCKETS) {
            (void)memset(duty->air_ms, 0, sizeof(duty->air_ms));
        } else {
            for (uint64_t i = 1U; i <= gap; i++) {
                duty_clear(duty, duty->head_bucket + i);
            }
        }
        duty->head_bucket = bucket;
    }
}

static uint32_t duty_used_ms(const fck_drv_subghz_duty_t *duty, size_t band)
{
    uint64_t used = 0U;
    for (size_t i = 0U; i < FCK_DRV_SUBGHZ_DUTY_BUCKETS; i++) {
        used += duty->air_ms[band][i];
    }
    return (used > UINT32_MAX) ? UINT32_MAX : (uint32_t)used;
}

uint32_t fck_drv_subghz_duty_signal_ms(const int32_t *runs, size_t n, uint8_t repeats)
{
    if (runs == NULL) {
        return UINT32_MAX;
    }
    uint64_t us = 0U;
    for (size_t i = 0U; i < n; i++) {
        /* Unsigned negation, so that even INT32_MIN has a length. */
        uint32_t const run = (runs[i] > 0) ? (uint32_t)runs[i] : (0U - (uint32_t)runs[i]);
        us += run;
    }
    uint64_t const ms = (((us * repeats) + 999U) / 1000U);
    return (ms >= UINT32_MAX) ? UINT32_MAX : (uint32_t)ms;
}

bool fck_drv_subghz_duty_left(fck_drv_subghz_duty_t *duty, uint32_t now_ms, uint32_t hz, uint32_t *left_ms)
{
    size_t band = 0U;
    if ((duty == NULL) || (left_ms == NULL) || !duty_band(hz, &band)) {
        return false;
    }
    duty_advance(duty, now_ms);
    uint32_t const used  = duty_used_ms(duty, band);
    uint32_t const limit = (band == BAND_433) ? FCK_DRV_SUBGHZ_DUTY_433_LIMIT_MS : FCK_DRV_SUBGHZ_DUTY_868_LIMIT_MS;
    *left_ms             = (used < limit) ? (limit - used) : 0U;
    return true;
}

bool fck_drv_subghz_duty_may(fck_drv_subghz_duty_t *duty, uint32_t now_ms, uint32_t hz, uint32_t air_ms)
{
    uint32_t left = 0U;
    return fck_drv_subghz_duty_left(duty, now_ms, hz, &left) && (air_ms <= left);
}

bool fck_drv_subghz_duty_record(fck_drv_subghz_duty_t *duty, uint32_t now_ms, uint32_t hz, uint32_t air_ms)
{
    size_t band = 0U;
    if ((duty == NULL) || !duty_band(hz, &band)) {
        return false;
    }
    duty_advance(duty, now_ms);
    uint32_t *const slot = &duty->air_ms[band][duty->head_bucket % FCK_DRV_SUBGHZ_DUTY_BUCKETS];
    *slot                = (air_ms > (UINT32_MAX - *slot)) ? UINT32_MAX : (*slot + air_ms);
    return true;
}
