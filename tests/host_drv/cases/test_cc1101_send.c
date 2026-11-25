/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_cc1101_send.c
 * @brief fck_drv_cc1101's sends and what stops them: fck_drv_cc1101_abort(),
 *        the app going home, to sleep or to a restart.
 *
 * The RMT is fake_idf.c's, on the virtual clock: a transmit "plays" for
 * g_idf.rmt.play_ms and nothing reaches an antenna. What is looked at is
 * what the driver did to the chip (its strobes) and to the RMT.
 */
#include <string.h>

#include "esp_timer.h"
#include "unity.h"

#include "fck_bsp.h"
#include "fck_drv_cc1101.h"
#include "fck_drv_subghz_modem.h"

#include "fake_cc1101.h"
#include "fake_idf.h"

#define STB_STX   0x35U
#define REG_FREQ2   0x0DU
#define REG_FREQ0   0x0FU
#define REG_MDMCFG4 0x10U
#define REG_MDMCFG2 0x12U
#define STB_SIDLE 0x36U
#define MARC_TX   0x13U
#define HZ_433    433920000U

static const int32_t k_runs[] = { 500, -500, 500, -500 };

static void begin(void)
{
    idf_fake_reset();
    fake_cc1101_reset(idf_fake_gpio(FCK_BSP_POSITION_CC_CS), idf_fake_gpio(FCK_BSP_POSITION_CC_MISO));
    fck_drv_cc1101_abort_clear();
    g_idf.rmt.on      = true;
    g_idf.rmt.play_ms = 15U;
}

static kode_status_t send(uint8_t repeats)
{
    return fck_drv_cc1101_send(idf_fake_hw(), NULL, HZ_433, k_runs, sizeof(k_runs) / sizeof(k_runs[0]), repeats);
}

static void abort_at_wait(unsigned n)
{
    if (n == 1U) {
        fck_drv_cc1101_abort();
    }
}

static void abort_at_second_wait(unsigned n)
{
    if (n == 2U) {
        fck_drv_cc1101_abort();
    }
}

TEST_CASE("a send plays every repeat and leaves the chip in IDLE", "[cc1101][send]")
{
    begin();
    TEST_ASSERT_EQUAL(KODE_OK, send(3U));
    TEST_ASSERT_EQUAL_UINT(3U, g_idf.rmt.transmits);
    TEST_ASSERT_EQUAL_UINT(1U, fake_cc1101_strobes(STB_STX));
    TEST_ASSERT_TRUE(g_cc.powered_down);
    TEST_ASSERT_FALSE(fck_drv_cc1101_aborted());
}

TEST_CASE("leaving in the middle of a repeat ends the carrier within one step and plays no more", "[cc1101][send]")
{
    begin();
    g_idf.rmt.play_ms = 800U;         /* a long send: the 3 s of a call, or part of it */
    g_idf.rmt.on_wait = abort_at_wait;
    int64_t const t0 = esp_timer_get_time();
    TEST_ASSERT_EQUAL(KODE_IO_ERROR, send(5U));
    int64_t const took_ms = (esp_timer_get_time() - t0) / 1000;
    TEST_ASSERT_EQUAL_UINT(1U, g_idf.rmt.transmits);
    TEST_ASSERT_EQUAL_UINT(1U, g_idf.rmt.waits);
    TEST_ASSERT_TRUE_MESSAGE(took_ms < 100, "the send went on after it was told to stop");
    TEST_ASSERT_EQUAL_UINT(1U, g_idf.rmt.disables);
    TEST_ASSERT_TRUE_MESSAGE(fake_cc1101_strobes(STB_SIDLE) >= 1U, "SIDLE ends the carrier");
    TEST_ASSERT_TRUE(g_cc.powered_down);
    TEST_ASSERT_EQUAL(0, g_idf.pin[idf_fake_gpio(FCK_BSP_POSITION_CC_GDO0)].level);
}

TEST_CASE("leaving between two repeats plays no further one", "[cc1101][send]")
{
    begin();
    g_idf.rmt.on_wait = abort_at_second_wait;      /* the repeat ends in its second step */
    TEST_ASSERT_EQUAL(KODE_IO_ERROR, send(4U));
    TEST_ASSERT_EQUAL_UINT(1U, g_idf.rmt.transmits);
    TEST_ASSERT_TRUE(g_cc.powered_down);
}

static void expect_not_keyed(uint8_t reg, const char *what)
{
    begin();
    g_cc.write_fails     = true;
    g_cc.write_fails_reg = reg;
    TEST_ASSERT_EQUAL_MESSAGE(KODE_IO_ERROR, send(1U), what);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, fake_cc1101_strobes(STB_STX), what);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, g_idf.rmt.transmits, what);
    TEST_ASSERT_TRUE(g_cc.powered_down);
}

TEST_CASE("a send whose settings the chip did not take keys nothing", "[cc1101][send]")
{
    expect_not_keyed(REG_FREQ0, "the frequency was not written: the chip would send where it was left");
    expect_not_keyed(REG_FREQ2, "the frequency was not written: the chip would send where it was left");
    expect_not_keyed(REG_MDMCFG2, "the modulation was not written");
    expect_not_keyed(REG_MDMCFG4, "the rate was not written");
    /* And with every write taken, the same send goes out. */
    begin();
    TEST_ASSERT_EQUAL(KODE_OK, send(1U));
    TEST_ASSERT_EQUAL_UINT(1U, fake_cc1101_strobes(STB_STX));
}

TEST_CASE("a send asked for after leaving keys nothing: refused, the chip untouched", "[cc1101][send]")
{
    begin();
    fck_drv_cc1101_abort();
    TEST_ASSERT_TRUE(fck_drv_cc1101_aborted());
    TEST_ASSERT_EQUAL(KODE_DENIED, send(1U));
    TEST_ASSERT_EQUAL_UINT(0U, fake_cc1101_strobes(STB_STX));
    TEST_ASSERT_EQUAL_UINT(0U, g_idf.rmt.transmits);
    TEST_ASSERT_EQUAL_UINT(0U, g_cc.log_n);
    fck_drv_cc1101_abort_clear();
    TEST_ASSERT_EQUAL(KODE_OK, send(1U));
}

static void packet_modem(fck_drv_subghz_modem_t *m)
{
    *m = (fck_drv_subghz_modem_t){
        .gfsk = true, .baud = 38400U, .deviation_hz = 20630U, .filter_hz = 101000U,
        .sync_word = 0xD391U, .length = 8U, .crc = true, .whitening = true,
    };
}

static void abort_at_third_read(unsigned n)
{
    if (n == 3U) {
        fck_drv_cc1101_abort();
    }
}

TEST_CASE("leaving while a packet is on the air puts the chip in IDLE and returns", "[cc1101][send]")
{
    begin();
    fck_drv_subghz_modem_t m;
    packet_modem(&m);
    static const uint8_t bytes[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    /* The fake chip never leaves TX by itself: only leaving ends this. */
    g_cc.on_marc = abort_at_third_read;
    TEST_ASSERT_EQUAL(KODE_IO_ERROR, fck_drv_cc1101_packet_send(idf_fake_hw(), NULL, HZ_433, &m, bytes, 8U, 3U));
    TEST_ASSERT_EQUAL_UINT(1U, fake_cc1101_strobes(STB_STX));
    TEST_ASSERT_TRUE(g_cc.powered_down);
}

TEST_CASE("a packet asked for after leaving keys nothing", "[cc1101][send]")
{
    begin();
    fck_drv_subghz_modem_t m;
    packet_modem(&m);
    static const uint8_t bytes[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    fck_drv_cc1101_abort();
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_drv_cc1101_packet_send(idf_fake_hw(), NULL, HZ_433, &m, bytes, 8U, 1U));
    TEST_ASSERT_EQUAL_UINT(0U, fake_cc1101_strobes(STB_STX));
    TEST_ASSERT_EQUAL_UINT(0U, g_cc.log_n);
}

TEST_CASE("a packet whose frequency the chip did not take is not sent", "[cc1101][send]")
{
    begin();
    fck_drv_subghz_modem_t m;
    packet_modem(&m);
    static const uint8_t bytes[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    g_cc.write_fails     = true;
    g_cc.write_fails_reg = REG_FREQ0;
    TEST_ASSERT_EQUAL(KODE_IO_ERROR, fck_drv_cc1101_packet_send(idf_fake_hw(), NULL, HZ_433, &m, bytes, 8U, 1U));
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, fake_cc1101_strobes(STB_STX), "a packet went out on the frequency the chip was left on");
    TEST_ASSERT_TRUE(g_cc.powered_down);
}

/* ---- What listens ends too ---------------------------------------------------------- */

TEST_CASE("a sweep begun after the cut reads no point and leaves the chip shut", "[cc1101][abort]")
{
    begin();
    int16_t half[3] = { 7, 7, 7 };
    fck_drv_cc1101_abort();
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_drv_cc1101_spectrum(idf_fake_hw(), NULL, 433000000U, 500000U, 3U,
                                                           FCK_DRV_CC1101_WIDE, half));
    for (size_t i = 0U; i < 3U; i++) {
        TEST_ASSERT_EQUAL_INT16(FCK_DRV_SUBGHZ_SCAN_NO_RECEIVE, half[i]);
    }
    TEST_ASSERT_EQUAL_INT(0, g_cc.buses);
    TEST_ASSERT_TRUE(g_cc.powered_down);
    /* And once cleared (the next job), the same sweep reads. */
    fck_drv_cc1101_abort_clear();
    TEST_ASSERT_EQUAL(KODE_OK, fck_drv_cc1101_spectrum(idf_fake_hw(), NULL, 433000000U, 500000U, 3U,
                                                       FCK_DRV_CC1101_WIDE, half));
}

TEST_CASE("a search for a remote cut at its start returns at once, not after its time", "[cc1101][abort]")
{
    begin();
    fck_drv_cc1101_hit_t hit;
    int64_t const        t0 = esp_timer_get_time();
    fck_drv_cc1101_abort();
    (void)fck_drv_cc1101_find(idf_fake_hw(), NULL, 5000U, &hit);
    TEST_ASSERT_TRUE_MESSAGE((esp_timer_get_time() - t0) < 200000, "the search ran its 5 s");
    TEST_ASSERT_FALSE(hit.found);
    TEST_ASSERT_TRUE(g_cc.powered_down);
}

TEST_CASE("a wait for packets cut while it listens returns at once, not after its time", "[cc1101][abort]")
{
    begin();
    fck_drv_subghz_modem_t m;
    packet_modem(&m);
    fck_drv_cc1101_packet_t out[2];
    size_t                  got = 7U;
    /* The fake chip receives nothing: only the cut ends this before its 5 s. */
    g_cc.on_marc     = abort_at_third_read;
    int64_t const t0 = esp_timer_get_time();
    TEST_ASSERT_EQUAL(KODE_OK, fck_drv_cc1101_packets(idf_fake_hw(), NULL, HZ_433, &m, 5000U, out, 2U, &got));
    TEST_ASSERT_TRUE_MESSAGE((esp_timer_get_time() - t0) < 200000, "the wait ran its 5 s");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, got, "nothing was received");
    TEST_ASSERT_TRUE(g_cc.powered_down);
}

TEST_CASE("a wait for packets begun after the cut listens for nothing", "[cc1101][abort]")
{
    begin();
    fck_drv_subghz_modem_t m;
    packet_modem(&m);
    fck_drv_cc1101_packet_t out[2];
    size_t                  got = 7U;
    fck_drv_cc1101_abort();
    int64_t const t0 = esp_timer_get_time();
    TEST_ASSERT_EQUAL(KODE_OK, fck_drv_cc1101_packets(idf_fake_hw(), NULL, HZ_433, &m, 5000U, out, 2U, &got));
    TEST_ASSERT_TRUE_MESSAGE((esp_timer_get_time() - t0) < 200000, "the wait ran its 5 s");
    TEST_ASSERT_EQUAL_UINT(0U, got);
    TEST_ASSERT_TRUE(g_cc.powered_down);
}
