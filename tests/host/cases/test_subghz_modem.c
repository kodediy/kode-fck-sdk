/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_subghz_modem.c
 * @brief fck_drv_subghz_modem: the packet receiver's registers, from its baud
 *        rate, deviation and filter, and the status the chip appends.
 *
 * The vectors are the ones boards have run: the two base blocks
 * (38.4 kbaud GFSK, the one fck_drv_cc1101 starts from, and 1.2 kbaud),
 * and the module's two modes (433.92 MHz at
 * 4.8 kbaud +/- 5.16 kHz, 868.35 at 38.38 kbaud +/- 20.63 kHz). What must never
 * happen: a filter narrower than asked, or a value out of the chip's range
 * written as if it were in it.
 */
#include "unity.h"

#include "fck_drv_subghz_modem.h"
#include "fck_drv_subghz_scan.h"

static fck_drv_subghz_modem_t fckit_fast(void)
{
    return (fck_drv_subghz_modem_t){
        .gfsk = true, .baud = 38400U, .deviation_hz = 20630U, .filter_hz = 101000U,
        .sync_word = 0xD391U, .length = 8U, .crc = true, .whitening = true,
    };
}

TEST_CASE("the base 38.4 kbaud block, register for register", "[modem]")
{
    fck_drv_subghz_modem_t const       s = fckit_fast();
    fck_drv_subghz_modem_registers_t r;
    TEST_ASSERT_TRUE(fck_drv_subghz_modem_registers(&s, &r));
    TEST_ASSERT_EQUAL_HEX8(0xD3, r.sync1);
    TEST_ASSERT_EQUAL_HEX8(0x91, r.sync0);
    TEST_ASSERT_EQUAL_HEX8(0x08, r.pktlen);
    TEST_ASSERT_EQUAL_HEX8(0x44, r.pktctrl0);
    TEST_ASSERT_EQUAL_HEX8(0x06, r.fsctrl1);
    TEST_ASSERT_EQUAL_HEX8(0xCA, r.mdmcfg4);
    TEST_ASSERT_EQUAL_HEX8(0x83, r.mdmcfg3);
    TEST_ASSERT_EQUAL_HEX8(0x13, r.mdmcfg2);
    TEST_ASSERT_EQUAL_HEX8(0x35, r.deviatn);
    TEST_ASSERT_EQUAL_HEX8(0x43, r.agcctrl2);
    TEST_ASSERT_EQUAL_HEX8(0x56, r.frend1);
    TEST_ASSERT_EQUAL_HEX8(0x81, r.test2);
    TEST_ASSERT_EQUAL_HEX8(0x35, r.test1);
}

TEST_CASE("the base 1.2 kbaud block: 0xF5, 0x83, deviation 0x15, AGC 0x03", "[modem]")
{
    fck_drv_subghz_modem_t s = fckit_fast();
    s.baud                   = 1200U;
    s.deviation_hz           = 5200U;
    s.filter_hz              = 58000U;
    fck_drv_subghz_modem_registers_t r;
    TEST_ASSERT_TRUE(fck_drv_subghz_modem_registers(&s, &r));
    TEST_ASSERT_EQUAL_HEX8(0xF5, r.mdmcfg4);
    TEST_ASSERT_EQUAL_HEX8(0x83, r.mdmcfg3);
    TEST_ASSERT_EQUAL_HEX8(0x15, r.deviatn);
    TEST_ASSERT_EQUAL_HEX8(0x03, r.agcctrl2);
}

TEST_CASE("the module's modes: 4.8 kbaud +/- 5.16 kHz and 38.38 kbaud +/- 20.63 kHz", "[modem]")
{
    uint8_t  e;
    uint8_t  m;
    uint32_t got;
    TEST_ASSERT_TRUE(fck_drv_subghz_modem_baud(4800U, &e, &m, &got));
    TEST_ASSERT_EQUAL_UINT8(7, e);
    TEST_ASSERT_EQUAL_UINT8(131, m);
    TEST_ASSERT_UINT32_WITHIN(2, 4798, got);
    TEST_ASSERT_TRUE(fck_drv_subghz_modem_deviation(5160U, &e, &m, &got));
    TEST_ASSERT_EQUAL_UINT8(1, e);
    TEST_ASSERT_EQUAL_UINT8(5, m);
    TEST_ASSERT_UINT32_WITHIN(2, 5157, got);

    TEST_ASSERT_TRUE(fck_drv_subghz_modem_baud(38383U, &e, &m, &got));
    TEST_ASSERT_EQUAL_UINT8(10, e);
    TEST_ASSERT_EQUAL_UINT8(131, m);
    TEST_ASSERT_UINT32_WITHIN(2, 38383, got);
    TEST_ASSERT_TRUE(fck_drv_subghz_modem_deviation(20630U, &e, &m, &got));
    TEST_ASSERT_EQUAL_UINT8(3, e);
    TEST_ASSERT_EQUAL_UINT8(5, m);
    TEST_ASSERT_UINT32_WITHIN(2, 20629, got);
}

TEST_CASE("the filter is the narrowest not narrower than asked, 58 to 812 kHz", "[modem]")
{
    uint8_t  e;
    uint8_t  m;
    uint32_t got;
    TEST_ASSERT_TRUE(fck_drv_subghz_modem_filter(0U, &e, &m, &got));
    TEST_ASSERT_EQUAL_UINT32(FCK_DRV_SUBGHZ_MODEM_FILTER_MIN, got);
    TEST_ASSERT_EQUAL_UINT8(3, e);
    TEST_ASSERT_EQUAL_UINT8(3, m);
    TEST_ASSERT_TRUE(fck_drv_subghz_modem_filter(101563U, &e, &m, &got));
    TEST_ASSERT_EQUAL_UINT32(116071, got);   /* 101 562 is narrower than asked */
    TEST_ASSERT_TRUE(fck_drv_subghz_modem_filter(101562U, &e, &m, &got));
    TEST_ASSERT_EQUAL_UINT32(101562, got);
    TEST_ASSERT_TRUE(fck_drv_subghz_modem_filter(650000U, &e, &m, &got));
    TEST_ASSERT_EQUAL_UINT32(650000, got);
    TEST_ASSERT_TRUE(fck_drv_subghz_modem_filter(812500U, &e, &m, &got));
    TEST_ASSERT_EQUAL_UINT8(0, e);
    TEST_ASSERT_EQUAL_UINT8(0, m);
    TEST_ASSERT_FALSE(fck_drv_subghz_modem_filter(812501U, &e, &m, &got));

    /* A wide filter takes the wide front end, as the level receiver does. */
    fck_drv_subghz_modem_t s = fckit_fast();
    s.filter_hz              = 400000U;
    fck_drv_subghz_modem_registers_t r;
    TEST_ASSERT_TRUE(fck_drv_subghz_modem_registers(&s, &r));
    TEST_ASSERT_EQUAL_HEX8(0xB6, r.frend1);
    TEST_ASSERT_EQUAL_HEX8(0x88, r.test2);
    TEST_ASSERT_EQUAL_HEX8(0x31, r.test1);
    TEST_ASSERT_EQUAL_HEX8(0x09, r.fsctrl1);   /* 406 250 / 2 / 25 391 + 2 */
}

TEST_CASE("2-FSK, no CRC and no whitening change their bits alone", "[modem]")
{
    fck_drv_subghz_modem_t s = fckit_fast();
    s.gfsk                   = false;
    s.crc                    = false;
    s.whitening              = false;
    s.length                 = 61U;
    s.sync_word              = 0x2DD4U;
    fck_drv_subghz_modem_registers_t r;
    TEST_ASSERT_TRUE(fck_drv_subghz_modem_registers(&s, &r));
    TEST_ASSERT_EQUAL_HEX8(0x03, r.mdmcfg2);
    TEST_ASSERT_EQUAL_HEX8(0x00, r.pktctrl0);
    TEST_ASSERT_EQUAL_HEX8(61, r.pktlen);
    TEST_ASSERT_EQUAL_HEX8(0x2D, r.sync1);
    TEST_ASSERT_EQUAL_HEX8(0xD4, r.sync0);
}

TEST_CASE("out of the chip's range is refused, and the registers left as they were", "[modem]")
{
    static const struct {
        uint32_t    baud;
        uint32_t    dev;
        uint32_t    bw;
        uint8_t     len;
        const char *why;
    } bad[] = {
        { 599U, 20630U, 101000U, 8U, "baud under 600" },
        { 250001U, 20630U, 101000U, 8U, "baud over 250 k" },
        { 38400U, 1585U, 101000U, 8U, "deviation under 1586 Hz" },
        { 38400U, 380860U, 101000U, 8U, "deviation over 380 859 Hz" },
        { 38400U, 20630U, 812501U, 8U, "filter over 812.5 kHz" },
        { 38400U, 20630U, 101000U, 0U, "length 0" },
        { 38400U, 20630U, 101000U, 62U, "length over 61" },
    };
    for (size_t i = 0U; i < sizeof(bad) / sizeof(bad[0]); i++) {
        fck_drv_subghz_modem_t s = fckit_fast();
        s.baud                   = bad[i].baud;
        s.deviation_hz           = bad[i].dev;
        s.filter_hz              = bad[i].bw;
        s.length                 = bad[i].len;
        fck_drv_subghz_modem_registers_t r = { .mdmcfg4 = 0xA5U };
        TEST_ASSERT_FALSE_MESSAGE(fck_drv_subghz_modem_registers(&s, &r), bad[i].why);
        TEST_ASSERT_EQUAL_HEX8_MESSAGE(0xA5, r.mdmcfg4, bad[i].why);
    }
    uint8_t  e;
    uint8_t  m;
    uint32_t got;
    TEST_ASSERT_TRUE(fck_drv_subghz_modem_deviation(FCK_DRV_SUBGHZ_MODEM_DEVIATION_MIN, &e, &m, &got));
    TEST_ASSERT_EQUAL_UINT32(FCK_DRV_SUBGHZ_MODEM_DEVIATION_MIN, got);
    TEST_ASSERT_TRUE(fck_drv_subghz_modem_deviation(FCK_DRV_SUBGHZ_MODEM_DEVIATION_MAX, &e, &m, &got));
    TEST_ASSERT_EQUAL_UINT32(FCK_DRV_SUBGHZ_MODEM_DEVIATION_MAX, got);
    TEST_ASSERT_TRUE(fck_drv_subghz_modem_baud(600U, &e, &m, &got));
    TEST_ASSERT_TRUE(fck_drv_subghz_modem_baud(250000U, &e, &m, &got));
    TEST_ASSERT_UINT32_WITHIN(250000U / 200U, 250000U, got);
}

TEST_CASE("the status: RSSI to half-dB, LQI, CRC only when on, FREQEST in 1587 Hz steps", "[modem]")
{
    /* RSSI byte 0xF0 is -16: -16 / 2 - 74 = -82 dBm, -164 half-dB. */
    fck_drv_subghz_modem_status_t st = fck_drv_subghz_modem_status(0xF0U, 0x80U | 12U, 0x02U, true);
    TEST_ASSERT_EQUAL_INT16(-164, st.half);
    TEST_ASSERT_EQUAL_UINT8(12, st.link_quality);
    TEST_ASSERT_TRUE(st.crc_ok);
    TEST_ASSERT_EQUAL_INT32(3173, st.frequency_error_hz);
    st = fck_drv_subghz_modem_status(0x10U, 0x80U | 127U, 0xFEU, false);
    TEST_ASSERT_EQUAL_INT16(16 - 148, st.half);
    TEST_ASSERT_EQUAL_UINT8(127, st.link_quality);
    TEST_ASSERT_FALSE_MESSAGE(st.crc_ok, "CRC off cannot be good");
    TEST_ASSERT_EQUAL_INT32(-3173, st.frequency_error_hz);
    st = fck_drv_subghz_modem_status(0x80U, 0x00U, 0x80U, true);
    TEST_ASSERT_EQUAL_INT16(-128 - 148, st.half);
    TEST_ASSERT_FALSE(st.crc_ok);
    TEST_ASSERT_EQUAL_INT32(-203125, st.frequency_error_hz);
}

/* ---- What a packet takes on air, and the spectrum it occupies ---------------------- */

TEST_CASE("a packet's air time: preamble 4, sync 4, the bytes and the CRC, at the chip's baud", "[modem][tx]")
{
    /* 38.4 kbaud is 38 383 on the chip: 18 bytes are 144 bits, 3 751.7 us. */
    fck_drv_subghz_modem_t s = fckit_fast();
    uint32_t               us = 0U;
    TEST_ASSERT_TRUE(fck_drv_subghz_modem_air_time_us(&s, &us));
    TEST_ASSERT_EQUAL_UINT32(3752U, us);
    s.crc = false;   /* 16 bytes, 128 bits: 3 334.7 us */
    TEST_ASSERT_TRUE(fck_drv_subghz_modem_air_time_us(&s, &us));
    TEST_ASSERT_EQUAL_UINT32(3335U, us);
    s.length = FCK_DRV_SUBGHZ_MODEM_LENGTH_MAX;   /* 69 bytes: 552 bits, 14 381.4 us */
    TEST_ASSERT_TRUE(fck_drv_subghz_modem_air_time_us(&s, &us));
    TEST_ASSERT_EQUAL_UINT32(14382U, us);
}

TEST_CASE("the slowest rate sends the longest packet in about a second", "[modem][tx]")
{
    fck_drv_subghz_modem_t s = fckit_fast();
    s.baud         = FCK_DRV_SUBGHZ_MODEM_BAUD_MIN;
    s.deviation_hz = 5000U;
    s.length       = FCK_DRV_SUBGHZ_MODEM_LENGTH_MAX;
    uint32_t us = 0U;
    TEST_ASSERT_TRUE(fck_drv_subghz_modem_air_time_us(&s, &us));
    /* 71 bytes = 568 bits at ~600 baud */
    TEST_ASSERT_UINT32_WITHIN(20000U, 946000U, us);
}

TEST_CASE("occupied bandwidth is Carson's: twice the deviation plus the baud rate, as the chip has them", "[modem][tx]")
{
    uint32_t               hz = 0U;
    fck_drv_subghz_modem_t s  = fckit_fast();
    TEST_ASSERT_TRUE(fck_drv_subghz_modem_occupied_hz(&s, &hz));
    TEST_ASSERT_EQUAL_UINT32(2U * 20629U + 38383U, hz);
    /* The module's slow mode: 4.8 kbaud, +/- 5.16 kHz. */
    s.baud         = 4800U;
    s.deviation_hz = 5160U;
    TEST_ASSERT_TRUE(fck_drv_subghz_modem_occupied_hz(&s, &hz));
    TEST_ASSERT_UINT32_WITHIN(500U, 2U * 5160U + 4800U, hz);
}

TEST_CASE("a packet that does not fit its sub-band is not sent", "[modem][tx]")
{
    fck_drv_subghz_modem_t s = fckit_fast();   /* 79 641 Hz */
    TEST_ASSERT_TRUE(fck_drv_subghz_modem_may_transmit(&s, 433920000U));
    TEST_ASSERT_TRUE(fck_drv_subghz_modem_may_transmit(&s, 868350000U));
    TEST_ASSERT_FALSE(fck_drv_subghz_modem_may_transmit(&s, 315000000U));
    TEST_ASSERT_FALSE(fck_drv_subghz_modem_may_transmit(&s, 915000000U));
    /* The centre in the band but not its skirts and the guard: 868.6 is the edge. */
    TEST_ASSERT_FALSE(fck_drv_subghz_modem_may_transmit(&s, 868600000U));
    TEST_ASSERT_TRUE(fck_drv_subghz_modem_may_transmit(&s, 868600000U - 39821U - FCK_DRV_SUBGHZ_SCAN_TX_GUARD_HZ));
    TEST_ASSERT_FALSE(fck_drv_subghz_modem_may_transmit(&s, 868600000U - 39820U - FCK_DRV_SUBGHZ_SCAN_TX_GUARD_HZ));
    /* 250 kbaud with 380 kHz of deviation, a megahertz wide, goes in 433's
     * 1.74 MHz and not in 868's 600 kHz. */
    s.baud         = FCK_DRV_SUBGHZ_MODEM_BAUD_MAX;
    s.deviation_hz = FCK_DRV_SUBGHZ_MODEM_DEVIATION_MAX;
    s.filter_hz    = FCK_DRV_SUBGHZ_MODEM_FILTER_MAX;
    TEST_ASSERT_TRUE(fck_drv_subghz_modem_may_transmit(&s, 433920000U));
    TEST_ASSERT_FALSE(fck_drv_subghz_modem_may_transmit(&s, 868300000U));
}

TEST_CASE("the transmitter's numbers refuse what the receiver's refuse", "[modem][tx]")
{
    fck_drv_subghz_modem_t s  = fckit_fast();
    uint32_t               v  = 0xDEADU;
    s.length = 0U;
    TEST_ASSERT_FALSE(fck_drv_subghz_modem_air_time_us(&s, &v));
    TEST_ASSERT_FALSE(fck_drv_subghz_modem_occupied_hz(&s, &v));
    TEST_ASSERT_FALSE(fck_drv_subghz_modem_may_transmit(&s, 433920000U));
    s        = fckit_fast();
    s.baud   = 100U;
    TEST_ASSERT_FALSE(fck_drv_subghz_modem_air_time_us(&s, &v));
    s        = fckit_fast();
    s.deviation_hz = 400000U;
    TEST_ASSERT_FALSE(fck_drv_subghz_modem_occupied_hz(&s, &v));
    TEST_ASSERT_EQUAL_UINT32(0xDEADU, v);
    s = fckit_fast();
    TEST_ASSERT_FALSE(fck_drv_subghz_modem_air_time_us(&s, NULL));
    TEST_ASSERT_FALSE(fck_drv_subghz_modem_air_time_us(NULL, &v));
    TEST_ASSERT_FALSE(fck_drv_subghz_modem_may_transmit(NULL, 433920000U));
}
