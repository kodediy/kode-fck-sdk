/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_subghz_duty.c
 * @brief fck_drv_subghz_duty: the module's air time over the last hour, per
 *        band (the limit without listen-before-talk).
 *
 * 10 % of any hour at 433.05-434.79 MHz is 360 s, 1 % at 868.0-868.6 MHz is
 * 36 s. The clock is the test's: what must never happen is a transmission
 * that goes over the hour's budget, a transmission forgotten before its hour
 * has passed, or an account that loses its way where the 32-bit clock wraps.
 */
#include <stdint.h>
#include <string.h>

#include "unity.h"

#include "fck_bsp.h"
#include "fck_drv_subghz_duty.h"

#define HZ_433     433920000U
#define HZ_868     868350000U
#define MINUTE_MS  60000U
#define HOUR_MS    3600000U

static uint32_t left_of(fck_drv_subghz_duty_t *d, uint32_t now, uint32_t hz)
{
    uint32_t left = 0xDEADU;
    TEST_ASSERT_TRUE(fck_drv_subghz_duty_left(d, now, hz, &left));
    return left;
}

TEST_CASE("an empty hour: 360 s at 433 and 36 s at 868", "[duty]")
{
    fck_drv_subghz_duty_t d;
    (void)memset(&d, 0, sizeof(d));
    TEST_ASSERT_EQUAL_UINT32(360000U, left_of(&d, 1000U, HZ_433));
    TEST_ASSERT_EQUAL_UINT32(36000U, left_of(&d, 1000U, HZ_868));
    TEST_ASSERT_EQUAL_UINT32(360000U, left_of(&d, 1000U, FCK_BSP_TX_433_MIN_HZ));
    TEST_ASSERT_EQUAL_UINT32(360000U, left_of(&d, 1000U, FCK_BSP_TX_433_MAX_HZ));
    TEST_ASSERT_EQUAL_UINT32(36000U, left_of(&d, 1000U, FCK_BSP_TX_868_MIN_HZ));
    TEST_ASSERT_EQUAL_UINT32(36000U, left_of(&d, 1000U, FCK_BSP_TX_868_MAX_HZ));
}

TEST_CASE("a frequency outside both bands has no account", "[duty]")
{
    fck_drv_subghz_duty_t d;
    (void)memset(&d, 0, sizeof(d));
    uint32_t left = 0xDEADU;
    TEST_ASSERT_FALSE(fck_drv_subghz_duty_left(&d, 0U, 315000000U, &left));
    TEST_ASSERT_FALSE(fck_drv_subghz_duty_left(&d, 0U, FCK_BSP_TX_433_MIN_HZ - 1U, &left));
    TEST_ASSERT_FALSE(fck_drv_subghz_duty_left(&d, 0U, FCK_BSP_TX_868_MAX_HZ + 1U, &left));
    TEST_ASSERT_EQUAL_UINT32(0xDEADU, left);
    TEST_ASSERT_FALSE(fck_drv_subghz_duty_may(&d, 0U, 915000000U, 1U));
    TEST_ASSERT_FALSE(fck_drv_subghz_duty_record(&d, 0U, 915000000U, 1U));
    TEST_ASSERT_FALSE(fck_drv_subghz_duty_left(NULL, 0U, HZ_433, &left));
    TEST_ASSERT_FALSE(fck_drv_subghz_duty_left(&d, 0U, HZ_433, NULL));

}

TEST_CASE("the budget is spent to the millisecond and no further", "[duty]")
{
    fck_drv_subghz_duty_t d;
    (void)memset(&d, 0, sizeof(d));
    TEST_ASSERT_TRUE(fck_drv_subghz_duty_may(&d, 0U, HZ_868, 36000U));
    TEST_ASSERT_FALSE(fck_drv_subghz_duty_may(&d, 0U, HZ_868, 36001U));
    TEST_ASSERT_TRUE(fck_drv_subghz_duty_record(&d, 0U, HZ_868, 35999U));
    TEST_ASSERT_EQUAL_UINT32(1U, left_of(&d, 0U, HZ_868));
    TEST_ASSERT_TRUE(fck_drv_subghz_duty_may(&d, 0U, HZ_868, 1U));
    TEST_ASSERT_FALSE(fck_drv_subghz_duty_may(&d, 0U, HZ_868, 2U));
    TEST_ASSERT_TRUE(fck_drv_subghz_duty_record(&d, 0U, HZ_868, 1U));
    TEST_ASSERT_EQUAL_UINT32(0U, left_of(&d, 0U, HZ_868));
    TEST_ASSERT_FALSE(fck_drv_subghz_duty_may(&d, 0U, HZ_868, 1U));
    /* Charged past the budget (a send that ran on), it stays at 0. */
    TEST_ASSERT_TRUE(fck_drv_subghz_duty_record(&d, 0U, HZ_868, 5000U));
    TEST_ASSERT_EQUAL_UINT32(0U, left_of(&d, 0U, HZ_868));
}

TEST_CASE("433 and 868 are separate accounts", "[duty]")
{
    fck_drv_subghz_duty_t d;
    (void)memset(&d, 0, sizeof(d));
    TEST_ASSERT_TRUE(fck_drv_subghz_duty_record(&d, 0U, HZ_868, 36000U));
    TEST_ASSERT_EQUAL_UINT32(0U, left_of(&d, 0U, HZ_868));
    TEST_ASSERT_EQUAL_UINT32(360000U, left_of(&d, 0U, HZ_433));
    TEST_ASSERT_TRUE(fck_drv_subghz_duty_record(&d, 0U, HZ_433, 100000U));
    TEST_ASSERT_EQUAL_UINT32(260000U, left_of(&d, 0U, HZ_433));
    TEST_ASSERT_EQUAL_UINT32(0U, left_of(&d, 0U, HZ_868));
    /* Any frequency of a band is that band's account. */
    TEST_ASSERT_EQUAL_UINT32(260000U, left_of(&d, 0U, FCK_BSP_TX_433_MAX_HZ));
}

TEST_CASE("a send refused for the budget goes once the hour has moved on", "[duty]")
{
    fck_drv_subghz_duty_t d;
    (void)memset(&d, 0, sizeof(d));
    uint32_t const t0 = 5U * MINUTE_MS;
    TEST_ASSERT_TRUE(fck_drv_subghz_duty_may(&d, t0, HZ_868, 30000U));
    TEST_ASSERT_TRUE(fck_drv_subghz_duty_record(&d, t0, HZ_868, 30000U));
    /* Ten minutes later, 7 s more fit and 7.001 do not. */
    uint32_t const t1 = t0 + 10U * MINUTE_MS;
    TEST_ASSERT_TRUE(fck_drv_subghz_duty_may(&d, t1, HZ_868, 6000U));
    TEST_ASSERT_TRUE(fck_drv_subghz_duty_record(&d, t1, HZ_868, 6000U));
    TEST_ASSERT_FALSE(fck_drv_subghz_duty_may(&d, t1, HZ_868, 1U));
    /* Just under an hour from the first: still refused. */
    TEST_ASSERT_FALSE(fck_drv_subghz_duty_may(&d, t0 + HOUR_MS - 1U, HZ_868, 1U));
    /* Once the first has left, only the second (6 s) is in the account. */
    uint32_t const gone = t0 + HOUR_MS + MINUTE_MS;   /* 61 minutes */
    TEST_ASSERT_EQUAL_UINT32(30000U, left_of(&d, gone, HZ_868));
    TEST_ASSERT_TRUE(fck_drv_subghz_duty_may(&d, gone, HZ_868, 30000U));
    /* And after the second's hour, all of it. */
    TEST_ASSERT_EQUAL_UINT32(36000U, left_of(&d, t1 + HOUR_MS + MINUTE_MS, HZ_868));
}

TEST_CASE("the hour rolls: a send is counted for its full hour, never less", "[duty]")
{
    fck_drv_subghz_duty_t d;
    (void)memset(&d, 0, sizeof(d));
    /* The last second of minute 0 and the first of minute 1: 59 s apart in
     * the worst case, 2 s in the best. */
    TEST_ASSERT_TRUE(fck_drv_subghz_duty_record(&d, MINUTE_MS - 1U, HZ_433, 10000U));
    TEST_ASSERT_TRUE(fck_drv_subghz_duty_record(&d, MINUTE_MS, HZ_433, 20000U));
    /* An hour after the first send (it began in the last ms of minute 0) it
     * is still counted: its bucket is not dropped until minute 61 begins. */
    TEST_ASSERT_EQUAL_UINT32(330000U, left_of(&d, MINUTE_MS - 1U + HOUR_MS, HZ_433));
    TEST_ASSERT_EQUAL_UINT32(330000U, left_of(&d, 61U * MINUTE_MS - 1U, HZ_433));
    /* The first leaves with minute 0's bucket, at minute 61; the second with minute 1's, at 62. */
    TEST_ASSERT_EQUAL_UINT32(340000U, left_of(&d, 61U * MINUTE_MS, HZ_433));
    TEST_ASSERT_EQUAL_UINT32(340000U, left_of(&d, 62U * MINUTE_MS - 1U, HZ_433));
    TEST_ASSERT_EQUAL_UINT32(360000U, left_of(&d, 62U * MINUTE_MS, HZ_433));
}

TEST_CASE("a long silence empties the whole ring", "[duty]")
{
    fck_drv_subghz_duty_t d;
    (void)memset(&d, 0, sizeof(d));
    for (uint32_t m = 0U; m < 61U; m++) {
        TEST_ASSERT_TRUE(fck_drv_subghz_duty_record(&d, m * MINUTE_MS, HZ_433, 5000U));
    }
    TEST_ASSERT_EQUAL_UINT32(360000U - 61U * 5000U, left_of(&d, 60U * MINUTE_MS, HZ_433));
    TEST_ASSERT_EQUAL_UINT32(360000U, left_of(&d, 60U * MINUTE_MS + 10U * HOUR_MS, HZ_433));
}

TEST_CASE("the 32-bit clock wraps and the account follows", "[duty]")
{
    fck_drv_subghz_duty_t d;
    (void)memset(&d, 0, sizeof(d));
    uint32_t const before = UINT32_MAX - 2000U;   /* 2 s before 49.7 days */
    TEST_ASSERT_TRUE(fck_drv_subghz_duty_record(&d, before, HZ_433, 300000U));
    /* 10 s later by the clock, which has wrapped to 7 999: still in the hour. */
    uint32_t const after = before + 10000U;
    TEST_ASSERT_TRUE(after < before);
    TEST_ASSERT_EQUAL_UINT32(60000U, left_of(&d, after, HZ_433));
    TEST_ASSERT_FALSE(fck_drv_subghz_duty_may(&d, after, HZ_433, 60001U));
    TEST_ASSERT_TRUE(fck_drv_subghz_duty_record(&d, after, HZ_433, 60000U));
    TEST_ASSERT_EQUAL_UINT32(0U, left_of(&d, after + 30U * MINUTE_MS, HZ_433));
    /* An hour and a minute on (the clock far into its second lap), it is free. */
    TEST_ASSERT_EQUAL_UINT32(360000U, left_of(&d, after + 62U * MINUTE_MS, HZ_433));
}

TEST_CASE("a clock reading a little behind the last is the same instant", "[duty]")
{
    fck_drv_subghz_duty_t d;
    (void)memset(&d, 0, sizeof(d));
    TEST_ASSERT_TRUE(fck_drv_subghz_duty_record(&d, 10U * MINUTE_MS, HZ_433, 1000U));
    TEST_ASSERT_EQUAL_UINT32(359000U, left_of(&d, 10U * MINUTE_MS - 5U, HZ_433));
    TEST_ASSERT_TRUE(fck_drv_subghz_duty_record(&d, 10U * MINUTE_MS - 5U, HZ_433, 1000U));
    TEST_ASSERT_EQUAL_UINT32(358000U, left_of(&d, 10U * MINUTE_MS, HZ_433));
}

TEST_CASE("a charge that would overflow stays full and does not wrap", "[duty]")
{
    fck_drv_subghz_duty_t d;
    (void)memset(&d, 0, sizeof(d));
    TEST_ASSERT_TRUE(fck_drv_subghz_duty_record(&d, 0U, HZ_433, UINT32_MAX));
    TEST_ASSERT_TRUE(fck_drv_subghz_duty_record(&d, 0U, HZ_433, UINT32_MAX));
    TEST_ASSERT_TRUE(fck_drv_subghz_duty_record(&d, MINUTE_MS, HZ_433, UINT32_MAX));
    TEST_ASSERT_EQUAL_UINT32(0U, left_of(&d, MINUTE_MS, HZ_433));
    TEST_ASSERT_FALSE(fck_drv_subghz_duty_may(&d, MINUTE_MS, HZ_433, 1U));
    TEST_ASSERT_FALSE(fck_drv_subghz_duty_may(&d, MINUTE_MS, HZ_433, UINT32_MAX));
    TEST_ASSERT_EQUAL_UINT32(360000U, left_of(&d, 62U * MINUTE_MS, HZ_433));
}

TEST_CASE("a signal's air time: every run, times the repeats, rounded up to the ms", "[duty]")
{
    const int32_t runs[] = { 350, -1050, 1050, -350, 12000 };
    TEST_ASSERT_EQUAL_UINT32(15U, fck_drv_subghz_duty_signal_ms(runs, 5U, 1U));   /* 14.8 ms */
    TEST_ASSERT_EQUAL_UINT32(45U, fck_drv_subghz_duty_signal_ms(runs, 5U, 3U));   /* 44.4 ms */
    TEST_ASSERT_EQUAL_UINT32(1U, fck_drv_subghz_duty_signal_ms(runs, 1U, 1U));    /* 0.35 ms */
    TEST_ASSERT_EQUAL_UINT32(0U, fck_drv_subghz_duty_signal_ms(runs, 0U, 1U));
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, fck_drv_subghz_duty_signal_ms(NULL, 1U, 1U));
}

TEST_CASE("a signal too long to count is refused by any budget", "[duty]")
{
    int32_t runs[4096];
    for (size_t i = 0U; i < 4096U; i++) {
        runs[i] = (i % 2U == 0U) ? 1000000 : -1000000;   /* 4096 s */
    }
    uint32_t const ms = fck_drv_subghz_duty_signal_ms(runs, 4096U, 32U);   /* 131 072 s */
    TEST_ASSERT_EQUAL_UINT32(131072000U, ms);
    fck_drv_subghz_duty_t d;
    (void)memset(&d, 0, sizeof(d));
    TEST_ASSERT_FALSE(fck_drv_subghz_duty_may(&d, 0U, HZ_433, ms));
    const int32_t one[] = { INT32_MIN };
    TEST_ASSERT_EQUAL_UINT32(2147484U, fck_drv_subghz_duty_signal_ms(one, 1U, 1U));
}
