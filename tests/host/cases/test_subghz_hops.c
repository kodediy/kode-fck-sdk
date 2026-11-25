/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_subghz_hops.c
 * @brief fck_drv_subghz_scan's hopping: the frequencies a listen with none hops
 *        over, which of them are neighbours, and what is worth staying for.
 *
 * What must
 * never happen: a neighbour on another leg (its antenna is not the one the
 * remote was heard on), or a click of noise taken for a remote.
 */
#include "unity.h"

#include "fck_bsp.h"
#include "fck_drv_subghz_scan.h"

TEST_CASE("the hops: 433.92, 315 and 868.35 MHz first, then 310, 318 and 390", "[hops]")
{
    static const uint32_t want[] = { 433920000U, 315000000U, 868350000U, 310000000U, 318000000U, 390000000U };
    size_t                n      = 0U;
    const uint32_t *const hz     = fck_drv_subghz_scan_hops(&n);
    TEST_ASSERT_EQUAL_size_t(FCK_DRV_SUBGHZ_SCAN_HOPS, n);
    TEST_ASSERT_EQUAL_UINT32_ARRAY(want, hz, 6);
    /* Each one is a remotes' frequency, the one fck_drv_subghz_scan_snap() says. */
    for (size_t i = 0U; i < n; i++) {
        TEST_ASSERT_EQUAL_UINT32(hz[i], fck_drv_subghz_scan_snap(hz[i]));
    }
}

TEST_CASE("neighbours: the hops of the top leg within 10 MHz, never another leg's", "[hops]")
{
    TEST_ASSERT_TRUE(fck_drv_subghz_scan_neighbours(315000000U, 310000000U));
    TEST_ASSERT_TRUE(fck_drv_subghz_scan_neighbours(315000000U, 318000000U));
    TEST_ASSERT_TRUE(fck_drv_subghz_scan_neighbours(318000000U, 310000000U));   /* 8 MHz */
    TEST_ASSERT_FALSE(fck_drv_subghz_scan_neighbours(315000000U, 315000000U));
    /* 390 and 433.92 share the middle leg, 43.92 MHz apart. */
    TEST_ASSERT_FALSE(fck_drv_subghz_scan_neighbours(433920000U, 390000000U));
    /* 348 is the top leg's last, 349 the middle's first: 1 MHz, two antennas. */
    TEST_ASSERT_FALSE(fck_drv_subghz_scan_neighbours(348000000U, 349000000U));
    TEST_ASSERT_EQUAL(FCK_BSP_LEG_TOP, fck_drv_subghz_scan_leg(348000000U));
    TEST_ASSERT_EQUAL(FCK_BSP_LEG_MIDDLE, fck_drv_subghz_scan_leg(349000000U));
    TEST_ASSERT_TRUE(fck_drv_subghz_scan_neighbours(305000000U, 315000000U));   /* 10 MHz, inclusive */
    TEST_ASSERT_FALSE(fck_drv_subghz_scan_neighbours(304999999U, 315000000U));

    /* Of the hops, only the top leg's three have neighbours. */
    size_t                n;
    const uint32_t *const hz = fck_drv_subghz_scan_hops(&n);
    unsigned              pairs = 0U;
    for (size_t i = 0U; i < n; i++) {
        for (size_t k = 0U; k < n; k++) {
            pairs += fck_drv_subghz_scan_neighbours(hz[i], hz[k]) ? 1U : 0U;
        }
    }
    TEST_ASSERT_EQUAL_UINT(6, pairs);   /* 310-315, 310-318, 315-318, both ways */
}

TEST_CASE("a neighbour is where it stays when 6 dB louder, and never when not there", "[hops]")
{
    TEST_ASSERT_TRUE(fck_drv_subghz_scan_louder(-100, -112));
    TEST_ASSERT_FALSE(fck_drv_subghz_scan_louder(-101, -112));
    TEST_ASSERT_FALSE(fck_drv_subghz_scan_louder(-112, -112));
    TEST_ASSERT_FALSE(fck_drv_subghz_scan_louder(FCK_DRV_SUBGHZ_SCAN_NO_RECEIVE, -299));
    TEST_ASSERT_TRUE(fck_drv_subghz_scan_louder(-288, FCK_DRV_SUBGHZ_SCAN_NO_RECEIVE));
}

TEST_CASE("a capture is a remote when a code is read from it or it has 48 runs", "[hops]")
{
    TEST_ASSERT_TRUE(fck_drv_subghz_scan_worth(16U, true));
    TEST_ASSERT_TRUE(fck_drv_subghz_scan_worth(48U, false));
    TEST_ASSERT_FALSE(fck_drv_subghz_scan_worth(47U, false));
    TEST_ASSERT_FALSE(fck_drv_subghz_scan_worth(0U, false));
}

/* Where a listen is held, read after read (fck_drv_subghz_scan_hold_t). What
 * must never happen again: a device sending no code - one on 310 MHz
 * every half second - holding a hopping listen for good. */

#define STAY_US ((int64_t)FCK_DRV_SUBGHZ_SCAN_STAY_MS * 1000)
#define T0_US   1000000000LL   /* any time: esp_timer counts from boot */

TEST_CASE("hold: the hold is 5 s, and a listen starts on its frequency or hopping", "[hops]")
{
    TEST_ASSERT_EQUAL_UINT32(5000U, FCK_DRV_SUBGHZ_SCAN_STAY_MS);
    fck_drv_subghz_scan_hold_t const on  = fck_drv_subghz_scan_hold_start(433920000U);
    fck_drv_subghz_scan_hold_t const hop = fck_drv_subghz_scan_hold_start(0U);
    TEST_ASSERT_EQUAL_UINT32(433920000U, on.hz);
    TEST_ASSERT_FALSE(on.hops);
    TEST_ASSERT_EQUAL_UINT32(0U, hop.hz);
    TEST_ASSERT_TRUE(hop.hops);
}

TEST_CASE("hold: hopping, a capture with no code never holds it, however often", "[hops]")
{
    fck_drv_subghz_scan_hold_t h = fck_drv_subghz_scan_hold_start(0U);
    for (int64_t t = T0_US; t < T0_US + (60 * 1000000LL); t += 500000) {
        fck_drv_subghz_scan_hold_check(&h, t, true);
        fck_drv_subghz_scan_hold_heard(&h, 310000000U, false, t);
        TEST_ASSERT_EQUAL_UINT32(0U, h.hz);
    }
}

TEST_CASE("hold: a code holds it there 5 s, to the microsecond, then it hops", "[hops]")
{
    fck_drv_subghz_scan_hold_t h = fck_drv_subghz_scan_hold_start(0U);
    fck_drv_subghz_scan_hold_heard(&h, 868350000U, true, T0_US);
    TEST_ASSERT_EQUAL_UINT32(868350000U, h.hz);
    fck_drv_subghz_scan_hold_check(&h, T0_US + STAY_US - 1, true);
    TEST_ASSERT_EQUAL_UINT32(868350000U, h.hz);
    fck_drv_subghz_scan_hold_check(&h, T0_US + STAY_US, true);
    TEST_ASSERT_EQUAL_UINT32(0U, h.hz);
}

TEST_CASE("hold: each code read there gives it 5 s more; a capture with none, nothing", "[hops]")
{
    fck_drv_subghz_scan_hold_t h = fck_drv_subghz_scan_hold_start(0U);
    fck_drv_subghz_scan_hold_heard(&h, 433920000U, true, T0_US);
    fck_drv_subghz_scan_hold_heard(&h, 433920000U, true, T0_US + 4000000);
    fck_drv_subghz_scan_hold_heard(&h, 433920000U, false, T0_US + 8000000);
    fck_drv_subghz_scan_hold_check(&h, T0_US + 8999999, true);
    TEST_ASSERT_EQUAL_UINT32(433920000U, h.hz);
    fck_drv_subghz_scan_hold_check(&h, T0_US + 9000000, true);
    TEST_ASSERT_EQUAL_UINT32(0U, h.hz);
}

TEST_CASE("hold: a read held on a hop ends when the hold does, if that is first", "[hops]")
{
    fck_drv_subghz_scan_hold_t h = fck_drv_subghz_scan_hold_start(0U);
    fck_drv_subghz_scan_hold_heard(&h, 315000000U, true, T0_US);
    /* A 60 s read: it is not deaf to the other hops for 60 s. */
    TEST_ASSERT_EQUAL_INT64(T0_US + STAY_US, fck_drv_subghz_scan_hold_end(&h, T0_US + 60000000LL));
    TEST_ASSERT_EQUAL_INT64(T0_US + 2000000, fck_drv_subghz_scan_hold_end(&h, T0_US + 2000000));
    /* A listen started on its frequency reads to the end it is given. */
    fck_drv_subghz_scan_hold_t const on = fck_drv_subghz_scan_hold_start(315000000U);
    TEST_ASSERT_EQUAL_INT64(T0_US + 60000000LL, fck_drv_subghz_scan_hold_end(&on, T0_US + 60000000LL));
}

TEST_CASE("hold: a hop whose leg cannot be had, without the expander, is let go at once", "[hops]")
{
    fck_drv_subghz_scan_hold_t h = fck_drv_subghz_scan_hold_start(0U);
    fck_drv_subghz_scan_hold_heard(&h, 315000000U, true, T0_US);
    fck_drv_subghz_scan_hold_check(&h, T0_US + 1, false);   /* the GNSS took the bus */
    TEST_ASSERT_EQUAL_UINT32(0U, h.hz);
    /* 433.92 and 868.35 MHz are on the legs SW0 rests for: kept without the expander. */
    fck_drv_subghz_scan_hold_heard(&h, 433920000U, true, T0_US);
    fck_drv_subghz_scan_hold_check(&h, T0_US + 1, false);
    TEST_ASSERT_EQUAL_UINT32(433920000U, h.hz);
    fck_drv_subghz_scan_hold_t g = fck_drv_subghz_scan_hold_start(0U);
    fck_drv_subghz_scan_hold_heard(&g, 868350000U, true, T0_US);
    fck_drv_subghz_scan_hold_check(&g, T0_US + 1, false);
    TEST_ASSERT_EQUAL_UINT32(868350000U, g.hz);
}

TEST_CASE("hold: a listen started on a frequency stays there, codes or none, for ever", "[hops]")
{
    fck_drv_subghz_scan_hold_t h = fck_drv_subghz_scan_hold_start(315000000U);
    fck_drv_subghz_scan_hold_heard(&h, 433920000U, true, T0_US);
    fck_drv_subghz_scan_hold_check(&h, T0_US + (3600 * 1000000LL), false);
    TEST_ASSERT_EQUAL_UINT32(315000000U, h.hz);
    TEST_ASSERT_FALSE(h.hops);
}

TEST_CASE("hold: NULL and a heard frequency of 0 change nothing", "[hops]")
{
    fck_drv_subghz_scan_hold_check(NULL, T0_US, true);
    fck_drv_subghz_scan_hold_heard(NULL, 433920000U, true, T0_US);
    TEST_ASSERT_EQUAL_INT64(T0_US, fck_drv_subghz_scan_hold_end(NULL, T0_US));
    fck_drv_subghz_scan_hold_t h = fck_drv_subghz_scan_hold_start(0U);
    fck_drv_subghz_scan_hold_heard(&h, 0U, true, T0_US);
    TEST_ASSERT_EQUAL_UINT32(0U, h.hz);
}
