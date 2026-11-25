/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_plan.c
 * @brief fck_plan: the owner's rules, in plain C - who answered at the
 *        expander's address, how often a header that holds no F*CK is asked
 *        again, when the module has left, what a command line may be, and what
 *        a caller waiting for a GNSS fix is told.
 */
#include <string.h>

#include "unity.h"

#include "fck_bsp.h"
#include "fck_plan.h"

TEST_CASE("a chip whose pull-up pins read high is the expander, one whose pins read low is not", "[plan]")
{
    uint8_t const pullups = (uint8_t)FCK_BSP_EXPANDER_PULLUPS_MASK;
    TEST_ASSERT_EQUAL(FCK_PLAN_ID_FCK, fck_plan_identify(pullups));
    TEST_ASSERT_EQUAL(FCK_PLAN_ID_FCK, fck_plan_identify(0xFFU));
    TEST_ASSERT_EQUAL(FCK_PLAN_ID_FCK, fck_plan_identify((uint8_t)(pullups & (uint8_t)(-pullups))));
    TEST_ASSERT_EQUAL(FCK_PLAN_ID_OTHER, fck_plan_identify(0x00U));
    TEST_ASSERT_EQUAL(FCK_PLAN_ID_OTHER, fck_plan_identify((uint8_t)~pullups));
}

TEST_CASE("the power-loss mark must come back as written to count", "[plan]")
{
    uint8_t const mark = (uint8_t)FCK_BSP_EXPANDER_CANARY_BIT;
    TEST_ASSERT_EQUAL(FCK_PLAN_ID_FCK, fck_plan_canary_kept(mark, mark));
    TEST_ASSERT_EQUAL(FCK_PLAN_ID_OTHER, fck_plan_canary_kept(mark, 0x00U));
    TEST_ASSERT_EQUAL(FCK_PLAN_ID_OTHER, fck_plan_canary_kept(mark, (uint8_t)(mark ^ 0xFFU)));
}

TEST_CASE("power is lost when the mark is gone from POLARITY, whatever else it holds", "[plan]")
{
    uint8_t const mark = (uint8_t)FCK_BSP_EXPANDER_CANARY_BIT;
    TEST_ASSERT_FALSE(fck_plan_power_lost(mark));
    TEST_ASSERT_FALSE(fck_plan_power_lost(0xFFU));
    TEST_ASSERT_TRUE(fck_plan_power_lost(0x00U));
    TEST_ASSERT_TRUE(fck_plan_power_lost((uint8_t)~mark));
}

TEST_CASE("a header ruled out, or with a troubled bus, is asked again only now and then", "[plan]")
{
    /* A header that may still turn out to be an F*CK is looked at every time. */
    TEST_ASSERT_FALSE(fck_plan_look_waits(0U, false, 0U));
    TEST_ASSERT_FALSE(fck_plan_look_waits(0U, false, 5U));
    TEST_ASSERT_FALSE(fck_plan_look_waits(FCK_PLAN_RULEOUT_LOOKS - 1U, false, 5U));
    /* Ruled out, or troubled: it waits while looks are owed. */
    TEST_ASSERT_TRUE(fck_plan_look_waits(FCK_PLAN_RULEOUT_LOOKS, false, 1U));
    TEST_ASSERT_TRUE(fck_plan_look_waits(0U, true, 1U));
    /* And is asked again once they are paid. */
    TEST_ASSERT_FALSE(fck_plan_look_waits(FCK_PLAN_RULEOUT_LOOKS, false, 0U));
    TEST_ASSERT_FALSE(fck_plan_look_waits(0U, true, 0U));
}

TEST_CASE("it takes two agreeing looks to rule a module out, and two misses to lose one", "[plan]")
{
    TEST_ASSERT_FALSE(fck_plan_ruled_out(0U));
    TEST_ASSERT_FALSE(fck_plan_ruled_out(FCK_PLAN_RULEOUT_LOOKS - 1U));
    TEST_ASSERT_TRUE(fck_plan_ruled_out(FCK_PLAN_RULEOUT_LOOKS));
    TEST_ASSERT_TRUE(fck_plan_ruled_out(FCK_PLAN_RULEOUT_LOOKS + 7U));
    TEST_ASSERT_FALSE(fck_plan_gone(0U));
    TEST_ASSERT_FALSE(fck_plan_gone(FCK_PLAN_GONE_MISSES - 1U));
    TEST_ASSERT_TRUE(fck_plan_gone(FCK_PLAN_GONE_MISSES));
    TEST_ASSERT_TRUE(fck_plan_gone(FCK_PLAN_GONE_MISSES + 1U));
}

TEST_CASE("a command line is one line of printable characters, with at most a final newline", "[plan]")
{
    TEST_ASSERT_TRUE(fck_plan_command_line_ok("info"));
    TEST_ASSERT_TRUE(fck_plan_command_line_ok("info\n"));
    TEST_ASSERT_TRUE(fck_plan_command_line_ok("wifi scan 6 ~"));
    TEST_ASSERT_FALSE(fck_plan_command_line_ok(NULL));
    TEST_ASSERT_FALSE(fck_plan_command_line_ok(""));
    TEST_ASSERT_FALSE(fck_plan_command_line_ok("\n"));
    TEST_ASSERT_FALSE_MESSAGE(fck_plan_command_line_ok("info\nreset"), "a second command inside");
    TEST_ASSERT_FALSE(fck_plan_command_line_ok("info\r"));
    TEST_ASSERT_FALSE(fck_plan_command_line_ok("in\tfo"));
    TEST_ASSERT_FALSE(fck_plan_command_line_ok("info\x7f"));
    TEST_ASSERT_FALSE(fck_plan_command_line_ok("caf\xc3\xa9"));
}

TEST_CASE("a command line is at most FCK_PLAN_COMMAND_LINE_MAX characters long", "[plan]")
{
    char line[FCK_PLAN_COMMAND_LINE_MAX + 4U];
    (void)memset(line, 'a', sizeof(line));
    line[FCK_PLAN_COMMAND_LINE_MAX] = '\0';
    TEST_ASSERT_TRUE(fck_plan_command_line_ok(line));
    line[FCK_PLAN_COMMAND_LINE_MAX]      = '\n';
    line[FCK_PLAN_COMMAND_LINE_MAX + 1U] = '\0';
    TEST_ASSERT_TRUE_MESSAGE(fck_plan_command_line_ok(line), "the builder's newline is not counted");
    line[FCK_PLAN_COMMAND_LINE_MAX]      = 'a';
    line[FCK_PLAN_COMMAND_LINE_MAX + 1U] = '\0';
    TEST_ASSERT_FALSE(fck_plan_command_line_ok(line));
}

TEST_CASE("a fix's age is the ticks since it was completed, in ms, across the counter's wrap", "[plan]")
{
    TEST_ASSERT_EQUAL_UINT32(0U, fck_plan_fix_age_ms(5000U, 5000U, 1000U));
    TEST_ASSERT_EQUAL_UINT32(730U, fck_plan_fix_age_ms(5730U, 5000U, 1000U));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(730U, fck_plan_fix_age_ms(73U, 0U, 100U), "a 10 ms tick");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(32U, fck_plan_fix_age_ms(0x00000010U, 0xFFFFFFF0U, 1000U),
                                     "the counter wrapped between the two");
}

TEST_CASE("a fix far older than a short age can say is never said to be young", "[plan]")
{
    /* Twenty-five days of 1 ms ticks: more than half a turn of the counter. */
    TEST_ASSERT_EQUAL_UINT32(0x80000005U, fck_plan_fix_age_ms(0x80000005U, 0U, 1000U));
    TEST_ASSERT_EQUAL_UINT32(0xFFFFFFFFU, fck_plan_fix_age_ms(0xFFFFFFFFU, 0U, 1000U));
    /* At 10 ms a tick the milliseconds outgrow the answer before the ticks do. */
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(UINT32_MAX, fck_plan_fix_age_ms(0x30000000U, 0U, 100U),
                                     "what does not fit is the longest, not a remainder");
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, fck_plan_fix_age_ms(10U, 0U, 0U));
}

TEST_CASE("a fix call with no module is refused at first and an I/O error after", "[plan]")
{
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_plan_fix_verdict(true, false, false, true, true, true, true));
    TEST_ASSERT_EQUAL(KODE_IO_ERROR, fck_plan_fix_verdict(false, false, false, true, true, true, true));
}

TEST_CASE("a fix call for a module that was replaced on the way is an I/O error", "[plan]")
{
    TEST_ASSERT_EQUAL(KODE_IO_ERROR, fck_plan_fix_verdict(false, false, true, false, true, true, true));
}

TEST_CASE("a fix call with the receiver off is refused", "[plan]")
{
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_plan_fix_verdict(true, true, true, true, false, true, true));
}

TEST_CASE("a fix is taken when one was made after the call, or any will do", "[plan]")
{
    TEST_ASSERT_EQUAL(KODE_OK, fck_plan_fix_verdict(false, false, true, true, true, true, true));
    TEST_ASSERT_EQUAL(KODE_OK, fck_plan_fix_verdict(true, true, true, true, true, true, false));
    TEST_ASSERT_EQUAL_MESSAGE(KODE_TIMEOUT, fck_plan_fix_verdict(false, false, true, true, true, true, false),
                              "the fix there was before the call is not the call's");
    TEST_ASSERT_EQUAL(KODE_TIMEOUT, fck_plan_fix_verdict(false, true, true, true, true, false, false));
}
