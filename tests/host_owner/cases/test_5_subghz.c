/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_5_subghz.c
 * @brief The sub-GHz sends through the facade: the packet send's gate, and the
 *        hour's air time (the band's limit) every send is put to.
 *
 * The CC1101 is the model's (fake_cc1101.c): a send that reaches it is
 * counted, with what it was given, and answers what a case says. What these
 * cases check is the facade's side: which sends get that far, which are
 * refused before it, and how the hour's account moves - 10 % of an hour at
 * 433, 1 % at 868 - when the clock does, and the cap on what one call keeps
 * the carrier on for (3 s). The account is the owner's, kept across cases,
 * so each starts after two hours of silence.
 */
#include <string.h>

#include "unity.h"

#include "fck.h"
#include "fck_aos.h"
#include "fck_core.h"

#include "fake_world.h"

#define HZ_433   433920000U
#define HZ_868   868350000U
#define HOUR_MS  3600000U

/* The fast settings: GFSK, 38.4 kbaud, 80 kHz wide. */
static const fck_subghz_packet_settings_t k_fast = {
    .modulation = FCK_SUBGHZ_GFSK, .baud = 38400U, .deviation_hz = 20630U, .filter_hz = 101000U,
    .sync_word = 0xD391U, .length = 8U, .crc = true, .whitening = true,
};

/* The slowest and the longest: a packet is about 947 ms on air. */
static const fck_subghz_packet_settings_t k_slow = {
    .modulation = FCK_SUBGHZ_2FSK, .baud = 600U, .deviation_hz = 5000U, .filter_hz = 58035U,
    .sync_word = 0xD391U, .length = FCK_SUBGHZ_PACKET_MAX, .crc = true, .whitening = false,
};

static const uint8_t k_eight[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };

/* One second a repeat: carrier on for half of it, off for half. */
static const int32_t k_second[2] = { 500000, -500000 };

/* Two seconds a repeat, the carrier on for a quarter of it. */
static const int32_t k_pulse[2] = { 500000, -1500000 };

/* The most one call keeps the carrier on for: 6 repeats of k_second. */
static kode_status_t send_six(uint32_t hz)
{
    return fck_core_subghz_send_raw(hz, k_second, 2U, 6U);
}

static void begin(void)
{
    sim_begin_fck(NULL, NULL);
    sim_cc1101_reset();
    sim_clock_skip_ms(2U * HOUR_MS);
}

static uint32_t left_of(uint32_t hz)
{
    uint32_t ms = 0xDEADU;
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_subghz_air_time_left(hz, &ms));
    return ms;
}

/* ---- The packet send ----------------------------------------------------------------- */

TEST_CASE("a packet goes to the radio with what the app gave, once per call", "[subghz][send]")
{
    begin();
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_subghz_packet_send(HZ_433, &k_fast, k_eight, 8U, 3U));
    TEST_ASSERT_EQUAL_UINT32(1U, sim_cc1101.packet_sends);
    TEST_ASSERT_EQUAL_UINT32(0U, sim_cc1101.sends);
    TEST_ASSERT_EQUAL_UINT32(HZ_433, sim_cc1101.hz);
    TEST_ASSERT_EQUAL_UINT8(3U, sim_cc1101.repeats);
    TEST_ASSERT_EQUAL_UINT8(8U, sim_cc1101.length);
    TEST_ASSERT_EQUAL_UINT32(38400U, sim_cc1101.baud);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(k_eight, sim_cc1101.bytes, 8U);
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_subghz_packet_send(HZ_868, &k_fast, k_eight, 8U, 1U));
    TEST_ASSERT_EQUAL_UINT32(2U, sim_cc1101.packet_sends);
}

TEST_CASE("a packet is refused before the radio: bad argument, band, width or modem", "[subghz][send]")
{
    begin();
    fck_subghz_packet_settings_t s = k_fast;
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_subghz_packet_send(HZ_433, NULL, k_eight, 8U, 1U));
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_subghz_packet_send(HZ_433, &s, NULL, 8U, 1U));
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_subghz_packet_send(HZ_433, &s, k_eight, 0U, 1U));
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_subghz_packet_send(HZ_433, &s, k_eight, 7U, 1U));   /* not the settings' */
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_subghz_packet_send(HZ_433, &s, k_eight, 8U, 0U));
    TEST_ASSERT_EQUAL(KODE_DENIED,
                      fck_core_subghz_packet_send(HZ_433, &s, k_eight, 8U, FCK_SUBGHZ_SEND_REPEATS_MAX + 1U));
    /* Other regions' bands, and the gap between the two. */
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_subghz_packet_send(315000000U, &s, k_eight, 8U, 1U));
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_subghz_packet_send(915000000U, &s, k_eight, 8U, 1U));
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_subghz_packet_send(600000000U, &s, k_eight, 8U, 1U));
    /* Centre in the band, 80 kHz of signal not: 868.6 is the edge. */
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_subghz_packet_send(FCK_BSP_TX_868_MAX_HZ, &s, k_eight, 8U, 1U));
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_subghz_packet_send(FCK_BSP_TX_433_MIN_HZ, &s, k_eight, 8U, 1U));
    /* Wide enough for 433's 1.74 MHz, not for 868's 600 kHz. */
    s.baud = 250000U;
    s.deviation_hz = 380000U;
    s.filter_hz = 812500U;
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_subghz_packet_send(868300000U, &s, k_eight, 8U, 1U));
    /* A modem the chip cannot be set to. */
    s = k_fast;
    s.baud = 100U;
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_subghz_packet_send(HZ_433, &s, k_eight, 8U, 1U));
    s = k_fast;
    s.modulation = (fck_subghz_modulation_t)7;
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_subghz_packet_send(HZ_433, &s, k_eight, 8U, 1U));
    s = k_fast;
    s.length = 0U;
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_subghz_packet_send(HZ_433, &s, k_eight, 0U, 1U));
    TEST_ASSERT_EQUAL_UINT32(0U, sim_cc1101.packet_sends);
    /* And nothing was charged. */
    TEST_ASSERT_EQUAL_UINT32(360000U, left_of(HZ_433));
    TEST_ASSERT_EQUAL_UINT32(36000U, left_of(HZ_868));
}

TEST_CASE("a packet that fills the band up to the guard still goes", "[subghz][send]")
{
    begin();
    /* 79 641 Hz wide: 39 821 Hz either side, rounded up, and 20 kHz of guard. */
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_subghz_packet_send(FCK_BSP_TX_868_MAX_HZ - 59821U, &k_fast, k_eight, 8U, 1U));
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_subghz_packet_send(FCK_BSP_TX_868_MAX_HZ - 59820U, &k_fast, k_eight, 8U, 1U));
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_subghz_packet_send(FCK_BSP_TX_433_MIN_HZ + 59821U, &k_fast, k_eight, 8U, 1U));
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_subghz_packet_send(FCK_BSP_TX_433_MIN_HZ + 59820U, &k_fast, k_eight, 8U, 1U));
}

TEST_CASE("what the radio says comes back, and the radio's own refusal is not charged", "[subghz][send]")
{
    begin();
    sim_cc1101.st = KODE_IO_ERROR;
    TEST_ASSERT_EQUAL(KODE_IO_ERROR, fck_core_subghz_packet_send(HZ_868, &k_fast, k_eight, 8U, 2U));
    /* It may have been on air: 2 packets of 3 751.7 us, 4 ms each, are charged. */
    TEST_ASSERT_EQUAL_UINT32(36000U - 8U, left_of(HZ_868));
    /* A refusal before the chip was touched is not. */
    sim_cc1101.st = KODE_DENIED;
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_subghz_packet_send(HZ_868, &k_fast, k_eight, 8U, 2U));
    sim_cc1101.st = KODE_NO_RESOURCES;
    TEST_ASSERT_EQUAL(KODE_NO_RESOURCES, fck_core_subghz_packet_send(HZ_868, &k_fast, k_eight, 8U, 2U));
    TEST_ASSERT_EQUAL_UINT32(36000U - 8U, left_of(HZ_868));
}

/* ---- The hour ------------------------------------------------------------------------- */

TEST_CASE("an empty hour: 360 s at 433, 36 s at 868, nothing elsewhere", "[subghz][duty]")
{
    begin();
    TEST_ASSERT_EQUAL_UINT32(360000U, left_of(HZ_433));
    TEST_ASSERT_EQUAL_UINT32(36000U, left_of(HZ_868));
    uint32_t ms = 0xDEADU;
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_subghz_air_time_left(315000000U, &ms));
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_subghz_air_time_left(915000000U, &ms));
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_subghz_air_time_left(HZ_433, NULL));
    TEST_ASSERT_EQUAL_UINT32(0xDEADU, ms);
}

TEST_CASE("a raw send is counted whole, off runs and repeats included", "[subghz][duty]")
{
    begin();
    const int32_t frame[4] = { 350, -1050, 1050, -350 };   /* 2.8 ms */
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_subghz_send_raw(HZ_868, frame, 4U, 10U));      /* 28 ms */
    TEST_ASSERT_EQUAL_UINT32(1U, sim_cc1101.sends);
    TEST_ASSERT_EQUAL_UINT32(36000U - 28U, left_of(HZ_868));
    TEST_ASSERT_EQUAL_UINT32(360000U, left_of(HZ_433));
}

TEST_CASE("one call keeps the carrier on for 3 s at most: exactly that goes, a microsecond more does not", "[subghz][cap]")
{
    begin();
    /* 6 repeats of 0.5 s on is the cap to the microsecond; 7 is 3.5 s. */
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_subghz_send_raw(HZ_433, k_second, 2U, 6U));
    TEST_ASSERT_EQUAL_UINT32(1U, sim_cc1101.sends);
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_subghz_send_raw(HZ_433, k_second, 2U, 7U));
    const int32_t at[6]   = { 1000000, -1, 1000000, -1, 1000000, -1 };
    const int32_t over[8] = { 1000000, -1, 1000000, -1, 1000000, -1, 1, -1 };
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_subghz_send_raw(HZ_433, over, 8U, 1U));
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_subghz_send_raw(HZ_433, at, 6U, 1U));
    TEST_ASSERT_EQUAL_UINT32(2U, sim_cc1101.sends);
}

TEST_CASE("a runaway signal is refused before the radio: 36 hours of carrier through the facade", "[subghz][cap]")
{
    begin();
    static int32_t runs[4096];
    for (size_t i = 0U; i < 4096U; i += 2U) {
        runs[i]      = 1000000;
        runs[i + 1U] = -1;
    }
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_subghz_send_raw(HZ_868, runs, 4096U, FCK_SUBGHZ_SEND_REPEATS_MAX));
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_subghz_send_raw(HZ_433, runs, 4096U, 1U));
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_subghz_send_raw(HZ_433, runs, 8U, 1U));      /* 4 s */
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_subghz_send_raw(HZ_433, runs, 2U, 3U));          /* 3 s: the cap */
    TEST_ASSERT_EQUAL_UINT32(1U, sim_cc1101.sends);
}

TEST_CASE("the off runs do not count to the cap but do to the hour", "[subghz][cap][duty]")
{
    begin();
    /* 0.5 s on, 1.5 s off, four times: 2 s of carrier, 8 s in TX. */
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_subghz_send_raw(HZ_868, k_pulse, 2U, 4U));
    TEST_ASSERT_EQUAL_UINT32(36000U - 8000U, left_of(HZ_868));
    /* The hour allows 28 s more; 14 repeats are 7 s of carrier, over the cap. */
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_subghz_send_raw(HZ_868, k_pulse, 2U, 14U));
    TEST_ASSERT_EQUAL_UINT32(1U, sim_cc1101.sends);
}

TEST_CASE("a send over the hour is refused before the radio, and goes when the hour has passed", "[subghz][duty]")
{
    begin();
    for (unsigned i = 0U; i < 5U; i++) {
        TEST_ASSERT_EQUAL(KODE_OK, send_six(HZ_868));   /* 6 s of the 36 each */
    }
    TEST_ASSERT_EQUAL_UINT32(6000U, left_of(HZ_868));
    TEST_ASSERT_EQUAL_UINT32(5U, sim_cc1101.sends);
    /* 4 repeats of 2 s are 8 s of the hour, with 2 s of carrier: over the hour. */
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_subghz_send_raw(HZ_868, k_pulse, 2U, 4U));
    /* The refusal names the limit in words a reader can look up, and is classed as one. */
    TEST_ASSERT_EQUAL_STRING("the hour's air time for this band is used up", fck_aos_error());
    TEST_ASSERT_EQUAL_UINT32(FCK_AOS_WHY_BEYOND_LIMITS, fck_aos_error_why());
    TEST_ASSERT_EQUAL_UINT32(5U, sim_cc1101.sends);
    TEST_ASSERT_EQUAL_UINT32(6000U, left_of(HZ_868));
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_subghz_send_raw(HZ_868, k_pulse, 2U, 3U));
    TEST_ASSERT_EQUAL_UINT32(0U, left_of(HZ_868));
    /* Spent to the last millisecond: even a short one is refused. */
    const int32_t blip[2] = { 350, -350 };
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_subghz_send_raw(HZ_868, blip, 2U, 1U));
    TEST_ASSERT_EQUAL_UINT32(6U, sim_cc1101.sends);
    /* Just under an hour on, still; at 61 minutes, the whole hour is back. */
    sim_clock_skip_ms(HOUR_MS - 1000U);
    TEST_ASSERT_EQUAL_UINT32(0U, left_of(HZ_868));
    sim_clock_skip_ms(2U * 60000U);
    TEST_ASSERT_EQUAL_UINT32(36000U, left_of(HZ_868));
    TEST_ASSERT_EQUAL(KODE_OK, send_six(HZ_868));
    TEST_ASSERT_EQUAL_UINT32(7U, sim_cc1101.sends);
}

TEST_CASE("433 and 868 keep separate hours", "[subghz][duty]")
{
    begin();
    for (unsigned i = 0U; i < 6U; i++) {
        TEST_ASSERT_EQUAL(KODE_OK, send_six(HZ_868));
    }
    TEST_ASSERT_EQUAL(KODE_DENIED, send_six(HZ_868));
    TEST_ASSERT_EQUAL(KODE_OK, send_six(HZ_433));
    TEST_ASSERT_EQUAL_UINT32(360000U - 6000U, left_of(HZ_433));
    TEST_ASSERT_EQUAL_UINT32(0U, left_of(HZ_868));
}

TEST_CASE("a packet call carries 3 s of packets at most, whatever its repeats", "[subghz][cap]")
{
    begin();
    /* A slow packet is the same for every repeat: one repeat's charge is d. */
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_subghz_packet_send(HZ_433, &k_slow, sim_cc1101.bytes, FCK_SUBGHZ_PACKET_MAX, 1U));
    uint32_t const d = 360000U - left_of(HZ_433);
    TEST_ASSERT_UINT32_WITHIN(5U, 947U, d);
    uint8_t const most = (uint8_t)(FCK_SUBGHZ_SEND_MS_MAX / d);
    TEST_ASSERT_EQUAL_UINT8(3U, most);
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_subghz_packet_send(HZ_433, &k_slow, sim_cc1101.bytes, FCK_SUBGHZ_PACKET_MAX, most));
    TEST_ASSERT_EQUAL(KODE_DENIED,
                      fck_core_subghz_packet_send(HZ_433, &k_slow, sim_cc1101.bytes, FCK_SUBGHZ_PACKET_MAX, most + 1U));
    TEST_ASSERT_EQUAL(KODE_DENIED,
                      fck_core_subghz_packet_send(HZ_433, &k_slow, sim_cc1101.bytes, FCK_SUBGHZ_PACKET_MAX,
                                             FCK_SUBGHZ_SEND_REPEATS_MAX));
    TEST_ASSERT_EQUAL_UINT32(2U, sim_cc1101.packet_sends);
    /* The fast packet is 3.8 ms: all 32 repeats are 0.12 s. */
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_subghz_packet_send(HZ_433, &k_fast, k_eight, 8U, FCK_SUBGHZ_SEND_REPEATS_MAX));
}

TEST_CASE("a packet send is counted as its packets' air time, and refused over the hour", "[subghz][duty]")
{
    begin();
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_subghz_packet_send(HZ_868, &k_slow, sim_cc1101.bytes, FCK_SUBGHZ_PACKET_MAX, 1U));
    uint32_t const d = 36000U - left_of(HZ_868);
    TEST_ASSERT_UINT32_WITHIN(5U, 947U, d);
    /* Calls of the most a call may send, until the hour has no room for one. */
    unsigned calls = 1U;
    while (left_of(HZ_868) >= 3U * d) {
        TEST_ASSERT_EQUAL(KODE_OK, fck_core_subghz_packet_send(HZ_868, &k_slow, sim_cc1101.bytes, FCK_SUBGHZ_PACKET_MAX, 3U));
        calls++;
        TEST_ASSERT_TRUE(calls < 40U);
    }
    uint32_t const l = left_of(HZ_868);
    TEST_ASSERT_EQUAL_UINT32(36000U - d - (calls - 1U) * 3U * d, l);
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_subghz_packet_send(HZ_868, &k_slow, sim_cc1101.bytes, FCK_SUBGHZ_PACKET_MAX, 3U));
    TEST_ASSERT_EQUAL_UINT32(calls, sim_cc1101.packet_sends);
    TEST_ASSERT_EQUAL_UINT32(l, left_of(HZ_868));
    /* The ones that fit, do, and not one more. */
    uint8_t const fit = (uint8_t)(l / d);
    TEST_ASSERT_TRUE(fit < 3U);
    if (fit > 0U) {
        TEST_ASSERT_EQUAL(KODE_OK, fck_core_subghz_packet_send(HZ_868, &k_slow, sim_cc1101.bytes, FCK_SUBGHZ_PACKET_MAX, fit));
    }
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_subghz_packet_send(HZ_868, &k_slow, sim_cc1101.bytes, FCK_SUBGHZ_PACKET_MAX, fit + 1U));
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_subghz_packet_send(HZ_868, &k_slow, sim_cc1101.bytes, FCK_SUBGHZ_PACKET_MAX, 3U));
    /* An OOK send shares the account. */
    TEST_ASSERT_EQUAL(KODE_DENIED, send_six(HZ_868));
}

TEST_CASE("a code sent is counted as its frames' length, and 16 of the slowest go", "[subghz][duty]")
{
    begin();
    fck_subghz_code_t const code = { .proto = FCK_SUBGHZ_EV1527, .bits = 24U, .key = 0x123456U, .te_us = 350U };
    int32_t                 us[FCK_SUBGHZ_FRAMES_MAX * FCK_SUBGHZ_FRAME_RUNS];
    size_t                  n = 0U;
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_subghz_encode(&code, 4U, us, sizeof(us) / sizeof(us[0]), &n));
    uint32_t total_us = 0U;
    for (size_t i = 0U; i < n; i++) {
        total_us += (uint32_t)((us[i] > 0) ? us[i] : -us[i]);
    }
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_subghz_send_code(HZ_433, &code, 4U));
    TEST_ASSERT_EQUAL_UINT32(1U, sim_cc1101.sends);
    TEST_ASSERT_EQUAL_UINT32(360000U - (total_us + 999U) / 1000U, left_of(HZ_433));
    /* The slowest te, the most frames, the key with most carrier: still under the cap. */
    fck_subghz_code_t const slow = { .proto = FCK_SUBGHZ_EV1527, .bits = 24U, .key = 0xFFFFFFU,
                                     .te_us = FCK_SUBGHZ_TE_MAX_US };
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_subghz_send_code(HZ_433, &slow, FCK_SUBGHZ_FRAMES_MAX));
    TEST_ASSERT_EQUAL_UINT32(2U, sim_cc1101.sends);
}

TEST_CASE("sends within the hour share it, and the oldest leaves first", "[subghz][duty]")
{
    begin();
    for (unsigned i = 0U; i < 3U; i++) {
        TEST_ASSERT_EQUAL(KODE_OK, send_six(HZ_868));   /* 18 s */
    }
    sim_clock_skip_ms(30U * 60000U);
    for (unsigned i = 0U; i < 3U; i++) {
        TEST_ASSERT_EQUAL(KODE_OK, send_six(HZ_868));   /* 18 s more */
    }
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_subghz_send_raw(HZ_868, k_second, 2U, 1U));
    /* The first three leave 31 minutes later, the second three stay. */
    sim_clock_skip_ms(31U * 60000U);
    TEST_ASSERT_EQUAL_UINT32(18000U, left_of(HZ_868));
    for (unsigned i = 0U; i < 3U; i++) {
        TEST_ASSERT_EQUAL(KODE_OK, send_six(HZ_868));
    }
    TEST_ASSERT_EQUAL(KODE_DENIED, send_six(HZ_868));
}
