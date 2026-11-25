/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_arm.c
 * @brief examples/subghz's arming of the rows that transmit (subghz_arm.c): given by hand,
 *        for a while, and taken back by a stop, by the module leaving, by fck_arm 0 or by the
 *        time running out; a request taken after any of them is dropped.
 */
#include <stdint.h>

#include "unity.h"

#include "subghz_arm.c"

TEST_CASE("the arming starts off: nothing transmits until fck_arm 1 is given", "[arm]")
{
    subghz_arm_t a = { 0 };
    TEST_ASSERT_FALSE(subghz_arm_ok(&a, 0U));
    TEST_ASSERT_FALSE(subghz_arm_ok(&a, 12345U));
    TEST_ASSERT_FALSE(subghz_arm_holds(&a, 0U, 0U));
}

TEST_CASE("fck_arm 1 allows for SUBGHZ_ARM_MS, to the millisecond, and then refuses", "[arm]")
{
    subghz_arm_t a = { 0 };
    subghz_arm_set(&a, true, 1000U);
    TEST_ASSERT_TRUE(subghz_arm_ok(&a, 1000U));
    TEST_ASSERT_TRUE(subghz_arm_ok(&a, 1000U + SUBGHZ_ARM_MS - 1U));
    TEST_ASSERT_FALSE_MESSAGE(subghz_arm_ok(&a, 1000U + SUBGHZ_ARM_MS), "run out");
    TEST_ASSERT_FALSE(subghz_arm_ok(&a, 1000U + SUBGHZ_ARM_MS + 5000U));
}

TEST_CASE("arming again extends it from the new time", "[arm]")
{
    subghz_arm_t a = { 0 };
    subghz_arm_set(&a, true, 0U);
    subghz_arm_set(&a, true, 40000U);
    TEST_ASSERT_TRUE(subghz_arm_ok(&a, 40000U + SUBGHZ_ARM_MS - 1U));
    TEST_ASSERT_FALSE(subghz_arm_ok(&a, 40000U + SUBGHZ_ARM_MS));
}

TEST_CASE("fck_arm 0 disarms at once and a request taken before no longer holds", "[arm]")
{
    subghz_arm_t a = { 0 };
    subghz_arm_set(&a, true, 100U);
    uint32_t const cuts = subghz_arm_cuts(&a);
    TEST_ASSERT_TRUE(subghz_arm_holds(&a, cuts, 200U));
    subghz_arm_set(&a, false, 300U);
    TEST_ASSERT_FALSE(subghz_arm_ok(&a, 301U));
    TEST_ASSERT_FALSE(subghz_arm_holds(&a, cuts, 301U));
}

TEST_CASE("a stop, or the module leaving, disarms and cancels what was posted, also if armed again", "[arm]")
{
    subghz_arm_t a = { 0 };
    subghz_arm_set(&a, true, 0U);
    uint32_t const posted = subghz_arm_cuts(&a);
    subghz_arm_cut(&a);                              /* fck_sg_stop, or FCK_EVENT_GONE */
    TEST_ASSERT_FALSE(subghz_arm_ok(&a, 1U));
    TEST_ASSERT_FALSE(subghz_arm_holds(&a, posted, 1U));
    subghz_arm_set(&a, true, 2U);                    /* armed again by hand ...         */
    TEST_ASSERT_TRUE(subghz_arm_ok(&a, 3U));
    TEST_ASSERT_FALSE_MESSAGE(subghz_arm_holds(&a, posted, 3U), "... the old request stays dropped");
    TEST_ASSERT_TRUE_MESSAGE(subghz_arm_holds(&a, subghz_arm_cuts(&a), 3U), "a new one goes");
}

TEST_CASE("an arming that ran out while a request waited for the pause does not let it go", "[arm]")
{
    subghz_arm_t a = { 0 };
    subghz_arm_set(&a, true, 0U);
    uint32_t const posted = subghz_arm_cuts(&a);
    TEST_ASSERT_TRUE(subghz_arm_holds(&a, posted, SUBGHZ_ARM_MS - 1U));
    TEST_ASSERT_FALSE(subghz_arm_holds(&a, posted, SUBGHZ_ARM_MS));
}

TEST_CASE("the clock wrapping at 49 days changes nothing", "[arm]")
{
    subghz_arm_t a = { 0 };
    uint32_t const t0 = 0xFFFFFFF0U;
    subghz_arm_set(&a, true, t0);
    TEST_ASSERT_TRUE(subghz_arm_ok(&a, t0));
    TEST_ASSERT_TRUE(subghz_arm_ok(&a, 100U));                              /* wrapped, 116 ms on */
    TEST_ASSERT_TRUE(subghz_arm_ok(&a, t0 + SUBGHZ_ARM_MS - 1U));
    TEST_ASSERT_FALSE(subghz_arm_ok(&a, t0 + SUBGHZ_ARM_MS));
    TEST_ASSERT_FALSE(subghz_arm_ok(&a, t0 + SUBGHZ_ARM_MS + 0x40000000U));
}

TEST_CASE("a NULL arming is a refusal, never a crash", "[arm]")
{
    subghz_arm_set(NULL, true, 0U);
    subghz_arm_cut(NULL);
    TEST_ASSERT_FALSE(subghz_arm_ok(NULL, 0U));
    TEST_ASSERT_FALSE(subghz_arm_holds(NULL, 0U, 0U));
    TEST_ASSERT_EQUAL_UINT32(0U, subghz_arm_cuts(NULL));
}
