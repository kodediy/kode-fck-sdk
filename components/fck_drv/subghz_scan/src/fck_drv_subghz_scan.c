/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file fck_drv_subghz_scan.c
 * @brief Finding a remote's frequency. See fck_drv_subghz_scan.h.
 */
#include "fck_drv_subghz_scan.h"

#define EDGE_POINTS  2U
#define POINTS_MAX   64U

static const uint32_t REMOTES[] = {
    /* 300-348 MHz */
    300000000U, 302757000U, 303875000U, 304250000U, 307000000U, 307500000U,
    307800000U, 309000000U, 310000000U, 312000000U, 313000000U, 313850000U,
    314350000U, 315000000U, 318000000U, 330000000U, 345000000U, 348000000U,
    /* 387-464 MHz */
    387000000U, 390000000U, 418000000U, 430000000U, 431000000U, 433075000U,
    433420000U, 433920000U, 434420000U, 434775000U, 438900000U, 440175000U,
    464000000U,
    /* 779-928 MHz */
    779000000U, 868350000U, 868950000U, 906400000U, 915000000U, 925000000U,
    928000000U,
};

fck_bsp_leg_t fck_drv_subghz_scan_leg(uint32_t hz)
{
    if (hz <= FCK_BSP_LEG_TOP_MAX_HZ) {
        return FCK_BSP_LEG_TOP;
    }
    return (hz <= FCK_BSP_LEG_MIDDLE_MAX_HZ) ? FCK_BSP_LEG_MIDDLE : FCK_BSP_LEG_BOTTOM;
}

bool fck_drv_subghz_scan_leg_open(uint32_t hz, bool expander)
{
    unsigned const sw0 = FCK_BSP_LEG_SW0(fck_drv_subghz_scan_leg(hz)) ? 1U : 0U;
    return expander || (sw0 == FCK_BSP_LEG_SW0_REST);
}

bool fck_drv_subghz_scan_may_transmit(uint32_t hz)
{
    return fck_drv_subghz_scan_may_transmit_width(hz, FCK_DRV_SUBGHZ_SCAN_OOK_WIDTH_HZ);
}

bool fck_drv_subghz_scan_may_transmit_width(uint32_t hz, uint32_t occupied_hz)
{
    uint32_t const width = (occupied_hz / 2U) + (occupied_hz % 2U);
    if (width > (UINT32_MAX - FCK_DRV_SUBGHZ_SCAN_TX_GUARD_HZ)) {
        return false;
    }
    uint32_t const half = width + FCK_DRV_SUBGHZ_SCAN_TX_GUARD_HZ;
    if ((half > hz) || (half > (UINT32_MAX - hz))) {
        return false;
    }
    uint32_t const lo = hz - half;
    uint32_t const hi = hz + half;
    return ((lo >= FCK_BSP_TX_433_MIN_HZ) && (hi <= FCK_BSP_TX_433_MAX_HZ))
           || ((lo >= FCK_BSP_TX_868_MIN_HZ) && (hi <= FCK_BSP_TX_868_MAX_HZ));
}

uint64_t fck_drv_subghz_scan_on_air_us(const int32_t *runs, size_t n, uint8_t repeats)
{
    if (runs == NULL) {
        return UINT64_MAX;
    }
    uint64_t on = 0U;
    for (size_t i = 0U; i < n; i++) {
        if (runs[i] > 0) {
            on += (uint64_t)(uint32_t)runs[i];
        }
    }
    return (on > (UINT64_MAX / 255U)) ? UINT64_MAX : (on * repeats);
}

bool fck_drv_subghz_scan_send_fits(uint64_t on_air_us)
{
    return on_air_us <= ((uint64_t)FCK_DRV_SUBGHZ_SCAN_SEND_MS_MAX * 1000U);
}

/* The list and its count in the header, kept the same. */
_Static_assert(sizeof(REMOTES) / sizeof(REMOTES[0]) == FCK_DRV_SUBGHZ_SCAN_REMOTES, "the list's length");

const uint32_t *fck_drv_subghz_scan_remotes(size_t *n)
{
    if (n != NULL) {
        *n = sizeof(REMOTES) / sizeof(REMOTES[0]);
    }
    return REMOTES;
}

const uint32_t *fck_drv_subghz_scan_hops(size_t *n)
{
    static const uint32_t HOPS[FCK_DRV_SUBGHZ_SCAN_HOPS] = {
        433920000U, 315000000U, 868350000U, 310000000U, 318000000U, 390000000U,
    };
    if (n != NULL) {
        *n = FCK_DRV_SUBGHZ_SCAN_HOPS;
    }
    return HOPS;
}

bool fck_drv_subghz_scan_neighbours(uint32_t a, uint32_t b)
{
    uint32_t const d = (a > b) ? a - b : b - a;
    return d != 0U && d <= FCK_DRV_SUBGHZ_SCAN_NEIGHBOUR_HZ
           && fck_drv_subghz_scan_leg(a) == fck_drv_subghz_scan_leg(b);
}

bool fck_drv_subghz_scan_louder(int16_t half, int16_t best_half)
{
    return half != FCK_DRV_SUBGHZ_SCAN_NO_RECEIVE
           && (int32_t)half >= (int32_t)best_half + FCK_DRV_SUBGHZ_SCAN_NEIGHBOUR_OVER_HALF;
}

bool fck_drv_subghz_scan_worth(size_t runs, bool decoded)
{
    return decoded || runs >= FCK_DRV_SUBGHZ_SCAN_WORTH_RUNS;
}

fck_drv_subghz_scan_hold_t fck_drv_subghz_scan_hold_start(uint32_t hz)
{
    return (fck_drv_subghz_scan_hold_t){ .hz = hz, .hops = (hz == 0U), .until_us = 0 };
}

void fck_drv_subghz_scan_hold_check(fck_drv_subghz_scan_hold_t *hold, int64_t now_us, bool expander)
{
    if ((hold != NULL) && hold->hops && (hold->hz != 0U)
        && ((now_us >= hold->until_us) || !fck_drv_subghz_scan_leg_open(hold->hz, expander))) {
        hold->hz = 0U;
    }
}

int64_t fck_drv_subghz_scan_hold_end(const fck_drv_subghz_scan_hold_t *hold, int64_t end_us)
{
    bool const sooner = (hold != NULL) && hold->hops && (hold->until_us < end_us);
    return sooner ? hold->until_us : end_us;
}

void fck_drv_subghz_scan_hold_heard(fck_drv_subghz_scan_hold_t *hold, uint32_t hz, bool decoded,
                                    int64_t now_us)
{
    if ((hold != NULL) && hold->hops && decoded && (hz != 0U)) {
        hold->hz       = hz;
        hold->until_us = now_us + ((int64_t)FCK_DRV_SUBGHZ_SCAN_STAY_MS * 1000);
    }
}

int16_t fck_drv_subghz_scan_rise(int16_t *noise, int16_t half)
{
    if (noise == NULL || half == FCK_DRV_SUBGHZ_SCAN_NO_RECEIVE) {
        return 0;
    }
    if (*noise == FCK_DRV_SUBGHZ_SCAN_NO_RECEIVE) {
        *noise = half;
        return 0;
    }
    int16_t const rise = (int16_t)(half - *noise);
    if (half < *noise) {
        *noise = (int16_t)((*noise + half) / 2);
    } else {
        *noise = (int16_t)(*noise + 1);
    }
    return rise;
}

bool fck_drv_subghz_scan_peak(const int16_t *half, size_t n, uint32_t first_hz, uint32_t step_hz,
                   uint32_t *hz, int16_t *peak_half)
{
    if (half == NULL || hz == NULL || peak_half == NULL || n < 2U * EDGE_POINTS + 1U
        || n > POINTS_MAX) {
        return false;
    }
    size_t  km = n;
    size_t  m  = 0U;
    int16_t sorted[POINTS_MAX];
    for (size_t k = 0U; k < n; k++) {
        if (half[k] == FCK_DRV_SUBGHZ_SCAN_NO_RECEIVE) {
            continue;
        }
        sorted[m++] = half[k];
        if (km == n || half[k] > half[km]) {
            km = k;
        }
    }
    if (m == 0U) {
        return false;
    }
    for (size_t i = 1U; i < m; i++) {
        int16_t const v = sorted[i];
        size_t        j = i;
        while (j > 0U && sorted[j - 1U] > v) {
            sorted[j] = sorted[j - 1U];
            j--;
        }
        sorted[j] = v;
    }
    if (half[km] - sorted[m / 2U] < FCK_DRV_SUBGHZ_SCAN_FINE_OVER_HALF || km < EDGE_POINTS
        || km > n - 1U - EDGE_POINTS) {
        return false;
    }
    /* The parabola through (-1, a), (0, b), (1, c) peaks at (a - c) / 2(a - 2b + c). */
    int32_t const a   = half[km - 1U];
    int32_t const b   = half[km];
    int32_t const c   = half[km + 1U];
    int32_t const den = a - 2 * b + c;
    int64_t       off = 0;
    if (den != 0 && a != FCK_DRV_SUBGHZ_SCAN_NO_RECEIVE && c != FCK_DRV_SUBGHZ_SCAN_NO_RECEIVE) {
        off = ((int64_t)(a - c) * (int64_t)step_hz) / (2 * (int64_t)den);
    }
    int64_t const lim = (int64_t)step_hz / 2;
    off               = (off > lim) ? lim : (off < -lim) ? -lim : off;
    int64_t const f   = (int64_t)first_hz + (int64_t)km * step_hz + off;
    *hz               = (uint32_t)(((f + 500) / 1000) * 1000);
    *peak_half        = half[km];
    return true;
}

bool fck_drv_subghz_scan_lock(fck_drv_subghz_scan_lock_t *lock, uint32_t hz, bool narrowed, int16_t rise,
                   int64_t t_us)
{
    if (lock == NULL) {
        return false;
    }
    uint32_t const d = (hz > lock->hz) ? hz - lock->hz : lock->hz - hz;
    if (lock->hits == 0U || d > FCK_DRV_SUBGHZ_SCAN_SAME_HZ
        || t_us - lock->t_us > (int64_t)FCK_DRV_SUBGHZ_SCAN_SAME_US) {
        *lock = (fck_drv_subghz_scan_lock_t){ 0 };
    }
    lock->hits++;
    if (narrowed) {
        lock->sum += hz;
        lock->fines++;
    }
    lock->hz   = (lock->fines > 0U) ? (uint32_t)(lock->sum / lock->fines) : hz;
    lock->t_us = t_us;
    return lock->fines > 0U && (lock->hits >= 2U || rise >= FCK_DRV_SUBGHZ_SCAN_STRONG_HALF);
}

uint32_t fck_drv_subghz_scan_snap(uint32_t hz)
{
    for (size_t i = 0U; i < sizeof(REMOTES) / sizeof(REMOTES[0]); i++) {
        uint32_t const d = (hz > REMOTES[i]) ? hz - REMOTES[i] : REMOTES[i] - hz;
        if (d <= FCK_DRV_SUBGHZ_SCAN_SNAP_HZ) {
            return REMOTES[i];
        }
    }
    return hz;
}
