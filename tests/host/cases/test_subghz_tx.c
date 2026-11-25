/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_subghz_tx.c
 * @brief Where the module may transmit: the two European SRD bands it sends
 *        in, and nowhere else.
 *
 * Receiving is legal on any frequency the CC1101 tunes; this gate is the one
 * thing that keeps the SDK from sending outside 433.05-434.79 and
 * 868.0-868.6 MHz (fck_bsp.h). A change to it has to fail here first.
 */
#include "unity.h"

#include "fck_bsp.h"
#include "fck_drv_ook.h"
#include "fck_drv_subghz_scan.h"

/* An OOK remote's signal: its own half width and the guard, from each edge. */
#define CLEAR (FCK_DRV_SUBGHZ_SCAN_OOK_WIDTH_HZ / 2U + FCK_DRV_SUBGHZ_SCAN_TX_GUARD_HZ)
#define GUARD FCK_DRV_SUBGHZ_SCAN_TX_GUARD_HZ

TEST_CASE("the 433 and 868 SRD bands may transmit, 45 kHz clear of each edge", "[tx]")
{
    TEST_ASSERT_EQUAL_UINT32(45000U, CLEAR);
    TEST_ASSERT_TRUE(fck_drv_subghz_scan_may_transmit(433920000U));  /* the common one */
    TEST_ASSERT_TRUE(fck_drv_subghz_scan_may_transmit(868350000U));
    TEST_ASSERT_TRUE(fck_drv_subghz_scan_may_transmit(FCK_BSP_TX_433_MIN_HZ + CLEAR));
    TEST_ASSERT_TRUE(fck_drv_subghz_scan_may_transmit(FCK_BSP_TX_433_MAX_HZ - CLEAR));
    TEST_ASSERT_TRUE(fck_drv_subghz_scan_may_transmit(FCK_BSP_TX_868_MIN_HZ + CLEAR));
    TEST_ASSERT_TRUE(fck_drv_subghz_scan_may_transmit(FCK_BSP_TX_868_MAX_HZ - CLEAR));
}

TEST_CASE("other regions' bands are received, never transmitted", "[tx]")
{
    TEST_ASSERT_FALSE(fck_drv_subghz_scan_may_transmit(315000000U));  /* US/Asia */
    TEST_ASSERT_FALSE(fck_drv_subghz_scan_may_transmit(310000000U));
    TEST_ASSERT_FALSE(fck_drv_subghz_scan_may_transmit(318000000U));
    TEST_ASSERT_FALSE(fck_drv_subghz_scan_may_transmit(390000000U));
    TEST_ASSERT_FALSE(fck_drv_subghz_scan_may_transmit(915000000U));  /* bottom leg, but not a transmit band */
    TEST_ASSERT_FALSE(fck_drv_subghz_scan_may_transmit(0U));
}

TEST_CASE("closer to an edge than that, on it, or outside, is refused", "[tx]")
{
    TEST_ASSERT_FALSE(fck_drv_subghz_scan_may_transmit(FCK_BSP_TX_433_MIN_HZ + CLEAR - 1U));
    TEST_ASSERT_FALSE(fck_drv_subghz_scan_may_transmit(FCK_BSP_TX_433_MAX_HZ - CLEAR + 1U));
    TEST_ASSERT_FALSE(fck_drv_subghz_scan_may_transmit(FCK_BSP_TX_868_MIN_HZ + CLEAR - 1U));
    TEST_ASSERT_FALSE(fck_drv_subghz_scan_may_transmit(FCK_BSP_TX_868_MAX_HZ - CLEAR + 1U));
    TEST_ASSERT_FALSE(fck_drv_subghz_scan_may_transmit(FCK_BSP_TX_433_MIN_HZ));
    TEST_ASSERT_FALSE(fck_drv_subghz_scan_may_transmit(FCK_BSP_TX_868_MAX_HZ));
    TEST_ASSERT_FALSE(fck_drv_subghz_scan_may_transmit(FCK_BSP_TX_433_MIN_HZ - 1U));
    TEST_ASSERT_FALSE(fck_drv_subghz_scan_may_transmit(FCK_BSP_TX_868_MAX_HZ + 1U));
    TEST_ASSERT_FALSE(fck_drv_subghz_scan_may_transmit(600000000U));  /* the gap between bands */
}

TEST_CASE("a transmission's width and the guard must lie whole inside one sub-band", "[tx]")
{
    /* 0 wide is the carrier alone, still the guard from each edge. */
    TEST_ASSERT_TRUE(fck_drv_subghz_scan_may_transmit_width(FCK_BSP_TX_433_MIN_HZ + GUARD, 0U));
    TEST_ASSERT_FALSE(fck_drv_subghz_scan_may_transmit_width(FCK_BSP_TX_433_MIN_HZ + GUARD - 1U, 0U));
    /* 100 kHz wide: 50 kHz either side of the centre, and the guard. */
    TEST_ASSERT_TRUE(fck_drv_subghz_scan_may_transmit_width(FCK_BSP_TX_433_MIN_HZ + 50000U + GUARD, 100000U));
    TEST_ASSERT_FALSE(fck_drv_subghz_scan_may_transmit_width(FCK_BSP_TX_433_MIN_HZ + 49999U + GUARD, 100000U));
    TEST_ASSERT_TRUE(fck_drv_subghz_scan_may_transmit_width(FCK_BSP_TX_433_MAX_HZ - 50000U - GUARD, 100000U));
    TEST_ASSERT_FALSE(fck_drv_subghz_scan_may_transmit_width(FCK_BSP_TX_433_MAX_HZ - 49999U - GUARD, 100000U));
    TEST_ASSERT_TRUE(fck_drv_subghz_scan_may_transmit_width(FCK_BSP_TX_868_MIN_HZ + 50000U + GUARD, 100000U));
    TEST_ASSERT_FALSE(fck_drv_subghz_scan_may_transmit_width(FCK_BSP_TX_868_MIN_HZ + 49999U + GUARD, 100000U));
    TEST_ASSERT_TRUE(fck_drv_subghz_scan_may_transmit_width(FCK_BSP_TX_868_MAX_HZ - 50000U - GUARD, 100000U));
    TEST_ASSERT_FALSE(fck_drv_subghz_scan_may_transmit_width(FCK_BSP_TX_868_MAX_HZ - 49999U - GUARD, 100000U));
}

TEST_CASE("an odd width rounds its half up: the safe side", "[tx]")
{
    /* 99 999 Hz wide takes 50 000 either side, not 49 999. */
    TEST_ASSERT_TRUE(fck_drv_subghz_scan_may_transmit_width(FCK_BSP_TX_868_MIN_HZ + 50000U + GUARD, 99999U));
    TEST_ASSERT_FALSE(fck_drv_subghz_scan_may_transmit_width(FCK_BSP_TX_868_MIN_HZ + 49999U + GUARD, 99999U));
}

TEST_CASE("a transmission never straddles the two sub-bands, nor a band's gap", "[tx]")
{
    /* A width that would span 434.79 to 868.0 (433 MHz wide) from a centre in the middle. */
    TEST_ASSERT_FALSE(fck_drv_subghz_scan_may_transmit_width(651000000U, 433000000U));
    TEST_ASSERT_FALSE(fck_drv_subghz_scan_may_transmit_width(433920000U, 2000000U));   /* wider than the band */
    TEST_ASSERT_FALSE(fck_drv_subghz_scan_may_transmit_width(868300000U, 600000U));    /* the band, and no guard */
    TEST_ASSERT_TRUE(fck_drv_subghz_scan_may_transmit_width(868300000U, 600000U - 2U * GUARD));
    TEST_ASSERT_FALSE(fck_drv_subghz_scan_may_transmit_width(868300000U, 600000U - 2U * GUARD + 1U));
    TEST_ASSERT_FALSE(fck_drv_subghz_scan_may_transmit_width(315000000U, 1000U));
    TEST_ASSERT_FALSE(fck_drv_subghz_scan_may_transmit_width(915000000U, 1000U));
}

TEST_CASE("no width wraps around the frequency's range", "[tx]")
{
    TEST_ASSERT_FALSE(fck_drv_subghz_scan_may_transmit_width(100U, UINT32_MAX));
    TEST_ASSERT_FALSE(fck_drv_subghz_scan_may_transmit_width(UINT32_MAX, 100U));
    TEST_ASSERT_FALSE(fck_drv_subghz_scan_may_transmit_width(0U, 0U));
    TEST_ASSERT_FALSE(fck_drv_subghz_scan_may_transmit_width(433920000U, UINT32_MAX));
    TEST_ASSERT_FALSE(fck_drv_subghz_scan_may_transmit_width(433920000U, UINT32_MAX - 1U));
}

/* ---- What one call may keep the carrier on for ---------------------------------------- */

TEST_CASE("the carrier's time is the positive runs times the repeats, the off runs left out", "[tx][cap]")
{
    const int32_t frame[4] = { 350, -1050, 1050, -350 };
    TEST_ASSERT_EQUAL_UINT64(1400U, fck_drv_subghz_scan_on_air_us(frame, 4U, 1U));
    TEST_ASSERT_EQUAL_UINT64(14000U, fck_drv_subghz_scan_on_air_us(frame, 4U, 10U));
    TEST_ASSERT_EQUAL_UINT64(0U, fck_drv_subghz_scan_on_air_us(frame, 4U, 0U));
    TEST_ASSERT_EQUAL_UINT64(0U, fck_drv_subghz_scan_on_air_us(frame, 0U, 5U));
    const int32_t off[2] = { INT32_MIN, -5 };
    TEST_ASSERT_EQUAL_UINT64(0U, fck_drv_subghz_scan_on_air_us(off, 2U, 32U));
    TEST_ASSERT_EQUAL_UINT64(UINT64_MAX, fck_drv_subghz_scan_on_air_us(NULL, 4U, 1U));
}

TEST_CASE("the repeats do not overflow what they multiply", "[tx][cap]")
{
    static int32_t runs[4096];
    for (size_t i = 0U; i < 4096U; i++) {
        runs[i] = INT32_MAX;
    }
    uint64_t const one = 4096U * (uint64_t)INT32_MAX;
    TEST_ASSERT_EQUAL_UINT64(one, fck_drv_subghz_scan_on_air_us(runs, 4096U, 1U));
    TEST_ASSERT_EQUAL_UINT64(one * 255U, fck_drv_subghz_scan_on_air_us(runs, 4096U, 255U));
    TEST_ASSERT_FALSE(fck_drv_subghz_scan_send_fits(fck_drv_subghz_scan_on_air_us(runs, 4096U, 255U)));
}

TEST_CASE("a call keeps the carrier on for 3 s at most: the cap itself goes, a microsecond more does not", "[tx][cap]")
{
    TEST_ASSERT_EQUAL_UINT32(3000U, FCK_DRV_SUBGHZ_SCAN_SEND_MS_MAX);
    TEST_ASSERT_TRUE(fck_drv_subghz_scan_send_fits(0U));
    TEST_ASSERT_TRUE(fck_drv_subghz_scan_send_fits(2999999U));
    TEST_ASSERT_TRUE(fck_drv_subghz_scan_send_fits(3000000U));
    TEST_ASSERT_FALSE(fck_drv_subghz_scan_send_fits(3000001U));
    TEST_ASSERT_FALSE(fck_drv_subghz_scan_send_fits(UINT64_MAX));
    /* By signals: three runs of a second, then a microsecond more; and with repeats. */
    const int32_t three[3]  = { 1000000, 1000000, 1000000 };
    const int32_t three1[4] = { 1000000, 1000000, 1000000, 1 };
    TEST_ASSERT_TRUE(fck_drv_subghz_scan_send_fits(fck_drv_subghz_scan_on_air_us(three, 3U, 1U)));
    TEST_ASSERT_FALSE(fck_drv_subghz_scan_send_fits(fck_drv_subghz_scan_on_air_us(three1, 4U, 1U)));
    const int32_t half[2] = { 1500000, -3 };
    TEST_ASSERT_TRUE(fck_drv_subghz_scan_send_fits(fck_drv_subghz_scan_on_air_us(half, 2U, 2U)));
    TEST_ASSERT_FALSE(fck_drv_subghz_scan_send_fits(fck_drv_subghz_scan_on_air_us(half, 2U, 3U)));
}

TEST_CASE("the runaway: 4096 seconds of carrier, 32 times over, is refused", "[tx][cap]")
{
    static int32_t runs[4096];
    for (size_t i = 0U; i < 4096U; i += 2U) {
        runs[i]      = 1000000;
        runs[i + 1U] = -1;
    }
    TEST_ASSERT_EQUAL_UINT64((uint64_t)2048U * 1000000U * 32U, fck_drv_subghz_scan_on_air_us(runs, 4096U, 32U));
    TEST_ASSERT_FALSE(fck_drv_subghz_scan_send_fits(fck_drv_subghz_scan_on_air_us(runs, 4096U, 32U)));
}

TEST_CASE("a remote's own signal fits: 16 frames of the slowest te", "[tx][cap]")
{
    static int32_t runs[16U * FCK_DRV_OOK_FRAME_RUNS];
    size_t const   n = fck_drv_ook_encode(FCK_DRV_OOK_EV1527, 24U, 0xFFFFFFU, FCK_DRV_OOK_TE_MAX_US, 16U, runs,
                                          sizeof(runs) / sizeof(runs[0]));
    TEST_ASSERT_TRUE(n > 0U);
    uint64_t const on = fck_drv_subghz_scan_on_air_us(runs, n, 1U);
    TEST_ASSERT_TRUE(on > 1000000U);
    TEST_ASSERT_TRUE(fck_drv_subghz_scan_send_fits(on));
}
