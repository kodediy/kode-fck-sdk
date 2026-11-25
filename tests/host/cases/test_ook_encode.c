/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_ook_encode.c
 * @brief fck_drv_ook_encode(): a code written as the runs its remote sends, and
 *        read back by fck_drv_ook_decode() as the same code.
 *
 * The main case is the same PT2262 remote (315 MHz, PT2262, 0x5155C3, te 436 us).
 * What must never happen: a run out of PT2262's timing, a PT2262 key
 * no PT2262 sends (a pair 10), or a buffer written in part on a refusal.
 */
#include <string.h>

#include "unity.h"

#include "fck_drv_ook.h"

#define REMOTE_KEY 0x5155C3U
#define REMOTE_TE  436U

/* A sentinel no run takes: a refusal leaves the buffer full of it. */
#define UNTOUCHED  0x7EADBEEF

static int32_t s_runs[16U * FCK_DRV_OOK_FRAME_RUNS + 1U];

static void fill_untouched(void)
{
    for (size_t i = 0U; i < sizeof(s_runs) / sizeof(s_runs[0]); i++) {
        s_runs[i] = UNTOUCHED;
    }
}

TEST_CASE("a frame is 24 bits, the first highest, then the sync: te high and 31 te low", "[ook]")
{
    fill_untouched();
    TEST_ASSERT_EQUAL_size_t(FCK_DRV_OOK_FRAME_RUNS,
                             fck_drv_ook_encode(FCK_DRV_OOK_PT2262, 24U, REMOTE_KEY, REMOTE_TE, 1U, s_runs,
                                                FCK_DRV_OOK_FRAME_RUNS));
    int32_t const te = (int32_t)REMOTE_TE;
    for (unsigned b = 0U; b < 24U; b++) {
        bool const one = ((REMOTE_KEY >> (23U - b)) & 1U) != 0U;
        TEST_ASSERT_EQUAL_INT32(one ? 3 * te : te, s_runs[2U * b]);
        TEST_ASSERT_EQUAL_INT32(one ? -te : -3 * te, s_runs[2U * b + 1U]);
    }
    TEST_ASSERT_EQUAL_INT32(te, s_runs[48]);
    TEST_ASSERT_EQUAL_INT32(-31 * te, s_runs[49]);
    TEST_ASSERT_EQUAL_INT32_MESSAGE(UNTOUCHED, s_runs[50], "written past the frame");
}

TEST_CASE("A PT2262 remote, written and read back: PT2262 0x5155C3 at 436 us, 3 frames", "[ook]")
{
    /* A quiet channel before it, as a capture has. */
    s_runs[0]      = -8000;
    size_t const n = fck_drv_ook_encode(FCK_DRV_OOK_PT2262, 24U, REMOTE_KEY, REMOTE_TE, 3U, &s_runs[1],
                                        sizeof(s_runs) / sizeof(s_runs[0]) - 1U);
    TEST_ASSERT_EQUAL_size_t(3U * FCK_DRV_OOK_FRAME_RUNS, n);
    fck_drv_ook_code_t c[2];
    TEST_ASSERT_EQUAL_size_t(1, fck_drv_ook_decode(s_runs, n + 1U, c, 2));
    TEST_ASSERT_EQUAL(FCK_DRV_OOK_PT2262, c[0].proto);
    TEST_ASSERT_EQUAL_HEX32(REMOTE_KEY, c[0].key);
    TEST_ASSERT_EQUAL_UINT8(24, c[0].bits);
    TEST_ASSERT_EQUAL_UINT16(REMOTE_TE, c[0].te_us);
    TEST_ASSERT_EQUAL_UINT8(3, c[0].frames);
}

TEST_CASE("an EV1527 key and the shortest and longest te come back as they went", "[ook]")
{
    static const struct {
        fck_drv_ook_protocol_t proto;
        uint32_t               key;
        uint16_t               te;
    } cases[] = {
        { FCK_DRV_OOK_EV1527, 0xA1B2C3U, 300U },
        { FCK_DRV_OOK_PT2262, REMOTE_KEY, FCK_DRV_OOK_TE_MIN_US },
        { FCK_DRV_OOK_PT2262, REMOTE_KEY, FCK_DRV_OOK_TE_MAX_US },
        { FCK_DRV_OOK_EV1527, 0x000002U, 350U },
        { FCK_DRV_OOK_EV1527, 0xFFFFFEU, 350U },
    };
    for (size_t i = 0U; i < sizeof(cases) / sizeof(cases[0]); i++) {
        s_runs[0]      = -8000;
        size_t const n = fck_drv_ook_encode(cases[i].proto, 24U, cases[i].key, cases[i].te, 2U, &s_runs[1],
                                            sizeof(s_runs) / sizeof(s_runs[0]) - 1U);
        TEST_ASSERT_EQUAL_size_t(2U * FCK_DRV_OOK_FRAME_RUNS, n);
        fck_drv_ook_code_t c;
        TEST_ASSERT_EQUAL_size_t_MESSAGE(1, fck_drv_ook_decode(s_runs, n + 1U, &c, 1), "not read back");
        TEST_ASSERT_EQUAL(cases[i].proto, c.proto);
        TEST_ASSERT_EQUAL_HEX32(cases[i].key, c.key);
        TEST_ASSERT_EQUAL_UINT16(cases[i].te, c.te_us);
    }
}

TEST_CASE("encode refuses what no remote sends, and writes nothing then", "[ook]")
{
    size_t const cap = sizeof(s_runs) / sizeof(s_runs[0]);
    fill_untouched();
    TEST_ASSERT_EQUAL_size_t(0, fck_drv_ook_encode(FCK_DRV_OOK_EV1527, 24U, 0x1000000U, 300U, 1U, s_runs, cap));
    TEST_ASSERT_EQUAL_size_t(0, fck_drv_ook_encode(FCK_DRV_OOK_EV1527, 24U, 0x123456U, 79U, 1U, s_runs, cap));
    TEST_ASSERT_EQUAL_size_t(0, fck_drv_ook_encode(FCK_DRV_OOK_EV1527, 24U, 0x123456U, 1201U, 1U, s_runs, cap));
    TEST_ASSERT_EQUAL_size_t(0, fck_drv_ook_encode(FCK_DRV_OOK_EV1527, 24U, 0x123456U, 300U, 0U, s_runs, cap));
    TEST_ASSERT_EQUAL_size_t(0, fck_drv_ook_encode(FCK_DRV_OOK_EV1527, 24U, 0x123456U, 300U, 3U, s_runs,
                                                   3U * FCK_DRV_OOK_FRAME_RUNS - 1U));
    TEST_ASSERT_EQUAL_size_t(0, fck_drv_ook_encode(FCK_DRV_OOK_EV1527, 24U, 0x123456U, 300U, 1U, NULL, cap));
    /* 0xA1B2C3 starts 10 10 00 01: a pair 10, which a PT2262 never sends. */
    TEST_ASSERT_EQUAL_size_t(0, fck_drv_ook_encode(FCK_DRV_OOK_PT2262, 24U, 0xA1B2C3U, 300U, 1U, s_runs, cap));
    /* A pair 10 in the last two bits too. */
    TEST_ASSERT_EQUAL_size_t(0, fck_drv_ook_encode(FCK_DRV_OOK_PT2262, 24U, 0x555556U, 300U, 1U, s_runs, cap));
    for (size_t i = 0U; i < cap; i++) {
        TEST_ASSERT_EQUAL_INT32_MESSAGE(UNTOUCHED, s_runs[i], "a refusal wrote runs");
    }
}

TEST_CASE("a key reads as PT2262 when its pairs are trits and one is open", "[ook]")
{
    TEST_ASSERT_EQUAL(FCK_DRV_OOK_PT2262, fck_drv_ook_protocol_of(REMOTE_KEY));
    TEST_ASSERT_EQUAL(FCK_DRV_OOK_EV1527, fck_drv_ook_protocol_of(0xA1B2C3U));
    /* Trits all tied (00 and 11), none open: nothing tells it from an EV1527. */
    TEST_ASSERT_EQUAL(FCK_DRV_OOK_EV1527, fck_drv_ook_protocol_of(0xF0F0F0U));
    /* And the other way: an EV1527 key whose pairs all happen to be trits, one
     * open, is on the air what a PT2262 sends, and reads back as one. */
    TEST_ASSERT_EQUAL(FCK_DRV_OOK_PT2262, fck_drv_ook_protocol_of(0x000001U));
}
