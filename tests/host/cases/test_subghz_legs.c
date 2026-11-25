/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_subghz_legs.c
 * @brief The sub-GHz antenna legs: which one each frequency is listened on,
 *        and the switch levels each one takes.
 *
 * The table is the module's (fck_bsp.h): 315 MHz
 * on the top leg, 433.92 MHz on the middle one, with its ladder filter, and
 * 868 and 915 MHz on the bottom one, to receive and to send. A change to any
 * of it has to fail here first.
 */
#include "unity.h"

#include "fck_bsp.h"
#include "fck_drv_subghz_scan.h"

TEST_CASE("the remotes' bands each go to their leg of the table", "[legs]")
{
    TEST_ASSERT_EQUAL(FCK_BSP_LEG_TOP, fck_drv_subghz_scan_leg(300000000U));
    TEST_ASSERT_EQUAL(FCK_BSP_LEG_TOP, fck_drv_subghz_scan_leg(315000000U));
    TEST_ASSERT_EQUAL(FCK_BSP_LEG_MIDDLE, fck_drv_subghz_scan_leg(433920000U));
    TEST_ASSERT_EQUAL(FCK_BSP_LEG_MIDDLE, fck_drv_subghz_scan_leg(464000000U));
    TEST_ASSERT_EQUAL(FCK_BSP_LEG_BOTTOM, fck_drv_subghz_scan_leg(868350000U));
    TEST_ASSERT_EQUAL(FCK_BSP_LEG_BOTTOM, fck_drv_subghz_scan_leg(915000000U));
    TEST_ASSERT_EQUAL(FCK_BSP_LEG_BOTTOM, fck_drv_subghz_scan_leg(928000000U));
}

TEST_CASE("the two bands the module sends on leave by their own legs", "[legs]")
{
    /* 433.05-434.79 MHz: the middle leg, (SW1, SW0) = (0, 1). */
    TEST_ASSERT_EQUAL(FCK_BSP_LEG_MIDDLE, fck_drv_subghz_scan_leg(FCK_BSP_TX_433_MIN_HZ));
    TEST_ASSERT_EQUAL(FCK_BSP_LEG_MIDDLE, fck_drv_subghz_scan_leg(FCK_BSP_TX_433_MAX_HZ));
    TEST_ASSERT_FALSE(FCK_BSP_LEG_SW1(fck_drv_subghz_scan_leg(433920000U)));
    TEST_ASSERT_TRUE(FCK_BSP_LEG_SW0(fck_drv_subghz_scan_leg(433920000U)));
    /* 868.0-868.6 MHz: the bottom leg, (1, 1). */
    TEST_ASSERT_EQUAL(FCK_BSP_LEG_BOTTOM, fck_drv_subghz_scan_leg(FCK_BSP_TX_868_MIN_HZ));
    TEST_ASSERT_EQUAL(FCK_BSP_LEG_BOTTOM, fck_drv_subghz_scan_leg(FCK_BSP_TX_868_MAX_HZ));
    TEST_ASSERT_TRUE(FCK_BSP_LEG_SW1(fck_drv_subghz_scan_leg(868350000U)));
    TEST_ASSERT_TRUE(FCK_BSP_LEG_SW0(fck_drv_subghz_scan_leg(868350000U)));
}

TEST_CASE("the legs change at 348 and at 600 MHz, both ends inclusive below", "[legs]")
{
    TEST_ASSERT_EQUAL_UINT32(348000000U, FCK_BSP_LEG_TOP_MAX_HZ);
    TEST_ASSERT_EQUAL_UINT32(600000000U, FCK_BSP_LEG_MIDDLE_MAX_HZ);
    TEST_ASSERT_EQUAL(FCK_BSP_LEG_TOP, fck_drv_subghz_scan_leg(348000000U));
    TEST_ASSERT_EQUAL(FCK_BSP_LEG_MIDDLE, fck_drv_subghz_scan_leg(348000001U));
    TEST_ASSERT_EQUAL(FCK_BSP_LEG_MIDDLE, fck_drv_subghz_scan_leg(600000000U));
    TEST_ASSERT_EQUAL(FCK_BSP_LEG_BOTTOM, fck_drv_subghz_scan_leg(600000001U));
}

TEST_CASE("each leg's (SW1, SW0): top (1, 0), middle (0, 1), bottom (1, 1)", "[legs]")
{
    TEST_ASSERT_TRUE(FCK_BSP_LEG_SW1(FCK_BSP_LEG_TOP));
    TEST_ASSERT_FALSE(FCK_BSP_LEG_SW0(FCK_BSP_LEG_TOP));
    TEST_ASSERT_FALSE(FCK_BSP_LEG_SW1(FCK_BSP_LEG_MIDDLE));
    TEST_ASSERT_TRUE(FCK_BSP_LEG_SW0(FCK_BSP_LEG_MIDDLE));
    TEST_ASSERT_TRUE(FCK_BSP_LEG_SW1(FCK_BSP_LEG_BOTTOM));
    TEST_ASSERT_TRUE(FCK_BSP_LEG_SW0(FCK_BSP_LEG_BOTTOM));
}

TEST_CASE("SW1 is GDO2 held high (0x6F) or low (0x2F)", "[legs]")
{
    TEST_ASSERT_EQUAL_HEX8(0x6FU, FCK_BSP_CC1101_IOCFG2_HIGH);
    TEST_ASSERT_EQUAL_HEX8(0x2FU, FCK_BSP_CC1101_IOCFG2_LOW);
}

TEST_CASE("SW0 is written to P3 alone: P4 is the nRF's wake line", "[legs]")
{
    TEST_ASSERT_EQUAL_UINT(3U, FCK_BSP_EXPANDER_CC_SW0);
    TEST_ASSERT_EQUAL_HEX8(0x08U, FCK_BSP_EXPANDER_SW0_MASK);
    TEST_ASSERT_EQUAL_UINT(4U, FCK_BSP_EXPANDER_HOST_WP_NRF);
}

TEST_CASE("SW0 rests high: without the expander 433, 868 and 915 are had, 315 is not", "[legs]")
{
    /* Without the expander - the GNSS receiver holding the bus - SW0 stays
     * where it rests, high, and the middle and the bottom leg can be had: of
     * the hops, 433.92, 868.35 and 390 MHz; 315, 310 and 318 MHz are skipped. */
    TEST_ASSERT_EQUAL_UINT(1U, FCK_BSP_LEG_SW0_REST);
    TEST_ASSERT_TRUE(fck_drv_subghz_scan_leg_open(433920000U, false));
    TEST_ASSERT_TRUE(fck_drv_subghz_scan_leg_open(390000000U, false));
    TEST_ASSERT_TRUE(fck_drv_subghz_scan_leg_open(348000001U, false));
    TEST_ASSERT_TRUE(fck_drv_subghz_scan_leg_open(600000000U, false));
    TEST_ASSERT_TRUE(fck_drv_subghz_scan_leg_open(600000001U, false));
    TEST_ASSERT_TRUE(fck_drv_subghz_scan_leg_open(868350000U, false));
    TEST_ASSERT_TRUE(fck_drv_subghz_scan_leg_open(915000000U, false));
    TEST_ASSERT_FALSE(fck_drv_subghz_scan_leg_open(348000000U, false));
    TEST_ASSERT_FALSE(fck_drv_subghz_scan_leg_open(315000000U, false));
    TEST_ASSERT_FALSE(fck_drv_subghz_scan_leg_open(310000000U, false));
    TEST_ASSERT_FALSE(fck_drv_subghz_scan_leg_open(318000000U, false));
    TEST_ASSERT_FALSE(fck_drv_subghz_scan_leg_open(300000000U, false));

    size_t                n;
    const uint32_t *const hz   = fck_drv_subghz_scan_hops(&n);
    unsigned              open = 0U;
    for (size_t i = 0U; i < n; i++) {
        TEST_ASSERT_TRUE(fck_drv_subghz_scan_leg_open(hz[i], true));
        open += fck_drv_subghz_scan_leg_open(hz[i], false) ? 1U : 0U;
    }
    TEST_ASSERT_EQUAL_UINT(3U, open);
}
