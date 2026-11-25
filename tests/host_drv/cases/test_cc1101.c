/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_cc1101.c
 * @brief fck_drv_cc1101: the CC1101 as the P4 drives it - identified, tuned
 *        and read for its level - against a fake SPI with a chip on it, and
 *        the expander that switches the antenna legs against a fake I2C bus.
 *
 * Nothing here transmits: the driver's RMT calls are fakes that refuse, and
 * the cases name no send. The sends' arithmetic is tests/host's.
 */
#include <string.h>

#include "unity.h"

#include "fck_bsp.h"
#include "fck_drv_cc1101.h"
#include "fck_drv_subghz_scan.h"
#include "fck_drv_tca6408a.h"

#include "fake_cc1101.h"
#include "fake_idf.h"

#define REG_IOCFG2 0x00U
#define REG_FREQ2  0x0DU
#define REG_FREQ1  0x0EU
#define REG_FREQ0  0x0FU
#define REG_FSCAL2 0x24U
#define REG_TEST0  0x2EU
#define STB_SRES   0x30U
#define STB_SIDLE  0x36U
#define STB_SPWD   0x39U
#define SW0        ((uint8_t)FCK_BSP_EXPANDER_SW0_MASK)

static fck_drv_tca6408a_t s_t;

static int cs(void)
{
    return idf_fake_gpio(FCK_BSP_POSITION_CC_CS);
}

static int miso(void)
{
    return idf_fake_gpio(FCK_BSP_POSITION_CC_MISO);
}

static void begin(void)
{
    idf_fake_reset();
    fake_cc1101_reset(cs(), miso());
}

/* The expander too, its bus open: the antenna legs that need SW0 off its rest. */
static void begin_with_expander(void)
{
    begin();
    idf_fake_expander(0xF7U);
    (void)memset(&s_t, 0, sizeof(s_t));
    fck_drv_tca6408a_forget(&s_t);
    TEST_ASSERT_EQUAL(KODE_OK, fck_drv_tca6408a_open(&s_t, idf_fake_hw()));
}

/* The word the driver writes for @p hz: FREQ = hz * 2^16 / 26 MHz, rounded. */
static uint32_t freq_word(uint32_t hz)
{
    return (uint32_t)((((uint64_t)hz << 16) + 13000000U) / 26000000U) & 0xFFFFFFU;
}

static void assert_closed(const char *why)
{
    TEST_ASSERT_EQUAL_MESSAGE(0, g_cc.buses, why);
    TEST_ASSERT_EQUAL_MESSAGE(0, g_cc.devices, why);
    TEST_ASSERT_TRUE_MESSAGE(g_cc.powered_down, why);
    TEST_ASSERT_EQUAL_MESSAGE(GPIO_MODE_INPUT, g_idf.pin[cs()].mode, why);
    TEST_ASSERT_TRUE_MESSAGE(g_idf.pin[cs()].pull_up, why);
    TEST_ASSERT_EQUAL_MESSAGE(0U, g_idf.conflicts, why);
}

/* ---- Telling a CC1101 from anything else ---------------------------------------------- */

TEST_CASE("sleep finds a CC1101, resets it, powers it down and gives the bus back", "[cc1101]")
{
    begin();
    uint8_t part = 0xAAU;
    uint8_t ver  = 0xAAU;
    TEST_ASSERT_EQUAL(KODE_OK, fck_drv_cc1101_sleep(idf_fake_hw(), &part, &ver));
    TEST_ASSERT_EQUAL_HEX8(0x00U, part);
    TEST_ASSERT_EQUAL_HEX8(0x14U, ver);
    TEST_ASSERT_EQUAL_UINT(1U, fake_cc1101_strobes(STB_SRES));
    TEST_ASSERT_EQUAL_UINT(1U, fake_cc1101_strobes(STB_SIDLE));
    TEST_ASSERT_EQUAL_UINT(1U, fake_cc1101_strobes(STB_SPWD));
    assert_closed("after sleep");
}

TEST_CASE("sleep many times leaves no claim on the chip select behind", "[cc1101]")
{
    begin();
    for (int i = 0; i < 5; i++) {
        TEST_ASSERT_EQUAL(KODE_OK, fck_drv_cc1101_sleep(idf_fake_hw(), NULL, NULL));
    }
    assert_closed("after five");
    TEST_ASSERT_EQUAL_MESSAGE(0U, g_idf.conflicts, "ESP-IDF warned of no conflict on the pin");
}

TEST_CASE("sleep with no chip is not a CC1101: nothing answers, and the bus is given back", "[cc1101]")
{
    begin();
    g_cc.present = false;
    uint8_t part = 0x11U;
    uint8_t ver  = 0x11U;
    /* The data line the driver pulls down reads as ready, and an empty bus
     * reads 0xFF: the answer is an I/O error, not a CC1101. */
    TEST_ASSERT_EQUAL(KODE_IO_ERROR, fck_drv_cc1101_sleep(idf_fake_hw(), &part, &ver));
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0xFFU, part, "nothing was read");
    TEST_ASSERT_EQUAL_HEX8(0xFFU, ver);
    TEST_ASSERT_EQUAL_INT(0, g_cc.buses);
    TEST_ASSERT_EQUAL_INT(0, g_cc.devices);
}

TEST_CASE("a chip that answers but is not a CC1101 is an I/O error, and is still powered down", "[cc1101]")
{
    begin();
    g_cc.version = 0x00U;
    TEST_ASSERT_EQUAL(KODE_IO_ERROR, fck_drv_cc1101_sleep(idf_fake_hw(), NULL, NULL));
    TEST_ASSERT_EQUAL_UINT(1U, fake_cc1101_strobes(STB_SPWD));

    begin();
    g_cc.partnum = 0x01U;
    TEST_ASSERT_EQUAL(KODE_IO_ERROR, fck_drv_cc1101_sleep(idf_fake_hw(), NULL, NULL));

    begin();
    g_cc.version = 0xFFU;
    TEST_ASSERT_EQUAL(KODE_IO_ERROR, fck_drv_cc1101_sleep(idf_fake_hw(), NULL, NULL));
}

TEST_CASE("a bus ESP-IDF will not make, or a device it will not add, is an error and the chip select rests", "[cc1101]")
{
    begin();
    g_cc.bus_rc = ESP_FAIL;
    TEST_ASSERT_EQUAL(KODE_ERROR, fck_drv_cc1101_sleep(idf_fake_hw(), NULL, NULL));
    TEST_ASSERT_EQUAL_UINT(0U, g_cc.transfers);
    TEST_ASSERT_EQUAL(GPIO_MODE_INPUT, g_idf.pin[cs()].mode);
    TEST_ASSERT_TRUE(g_idf.pin[cs()].pull_up);

    begin();
    g_cc.add_rc = ESP_FAIL;
    TEST_ASSERT_EQUAL(KODE_ERROR, fck_drv_cc1101_sleep(idf_fake_hw(), NULL, NULL));
    TEST_ASSERT_EQUAL_MESSAGE(0, g_cc.buses, "the bus it made is freed");
    TEST_ASSERT_EQUAL(GPIO_MODE_INPUT, g_idf.pin[cs()].mode);
}

TEST_CASE("the data line is pulled down while the bus is open and let float after", "[cc1101]")
{
    begin();
    g_cc.present = false;     /* it never becomes ready: the pull is what the driver left it with */
    (void)fck_drv_cc1101_sleep(idf_fake_hw(), NULL, NULL);
    TEST_ASSERT_FALSE(g_idf.pin[miso()].pull_up);
    TEST_ASSERT_FALSE(g_idf.pin[miso()].pull_down);
}

/* ---- Which frequencies it tunes -------------------------------------------------------- */

TEST_CASE("tunes says yes inside the chip's three bands and no outside, at their very edges too", "[cc1101]")
{
    TEST_ASSERT_FALSE(fck_drv_cc1101_tunes(0U));
    TEST_ASSERT_FALSE(fck_drv_cc1101_tunes(299999999U));
    TEST_ASSERT_TRUE(fck_drv_cc1101_tunes(300000000U));
    TEST_ASSERT_TRUE(fck_drv_cc1101_tunes(315000000U));
    TEST_ASSERT_TRUE(fck_drv_cc1101_tunes(348000000U));
    TEST_ASSERT_FALSE(fck_drv_cc1101_tunes(348000001U));
    TEST_ASSERT_FALSE(fck_drv_cc1101_tunes(386999999U));
    TEST_ASSERT_TRUE(fck_drv_cc1101_tunes(387000000U));
    TEST_ASSERT_TRUE(fck_drv_cc1101_tunes(433920000U));
    TEST_ASSERT_TRUE(fck_drv_cc1101_tunes(464000000U));
    TEST_ASSERT_FALSE(fck_drv_cc1101_tunes(464000001U));
    TEST_ASSERT_FALSE(fck_drv_cc1101_tunes(778999999U));
    TEST_ASSERT_TRUE(fck_drv_cc1101_tunes(779000000U));
    TEST_ASSERT_TRUE(fck_drv_cc1101_tunes(868350000U));
    TEST_ASSERT_TRUE(fck_drv_cc1101_tunes(928000000U));
    TEST_ASSERT_FALSE(fck_drv_cc1101_tunes(928000001U));
}

/* ---- A level ----------------------------------------------------------------------- */

TEST_CASE("a level at 433.92 MHz tunes the chip there and reads RSSI in half-dB", "[cc1101]")
{
    begin();
    g_cc.peak_hz  = 433920000U;
    g_cc.peak_raw = 0x60U;         /* 96 half-dB steps: 96 - 148 = -52 */
    int16_t half  = 0;
    TEST_ASSERT_EQUAL(KODE_OK, fck_drv_cc1101_rssi(idf_fake_hw(), NULL, 433920000U, FCK_DRV_CC1101_WIDE, &half));
    TEST_ASSERT_EQUAL_INT16(-52, half);

    uint32_t const word = freq_word(433920000U);
    uint8_t        v    = 0U;
    TEST_ASSERT_TRUE(fake_cc1101_writes(REG_FREQ2, &v) >= 1U);
    TEST_ASSERT_EQUAL_HEX8((uint8_t)(word >> 16), v);
    (void)fake_cc1101_writes(REG_FREQ1, &v);
    TEST_ASSERT_EQUAL_HEX8((uint8_t)(word >> 8), v);
    (void)fake_cc1101_writes(REG_FREQ0, &v);
    TEST_ASSERT_EQUAL_HEX8((uint8_t)word, v);
    assert_closed("after a level");
}

TEST_CASE("a quiet channel reads the chip's floor, a carrier on it reads higher", "[cc1101]")
{
    begin();
    g_cc.peak_hz  = 433920000U;
    g_cc.peak_raw = 0x70U;
    int16_t quiet = 0;
    int16_t loud  = 0;
    TEST_ASSERT_EQUAL(KODE_OK, fck_drv_cc1101_rssi(idf_fake_hw(), NULL, 433000000U, FCK_DRV_CC1101_NARROW, &quiet));
    TEST_ASSERT_EQUAL(KODE_OK, fck_drv_cc1101_rssi(idf_fake_hw(), NULL, 433920000U, FCK_DRV_CC1101_NARROW, &loud));
    TEST_ASSERT_EQUAL_INT16(0x40 - 148, quiet);
    TEST_ASSERT_EQUAL_INT16(0x70 - 148, loud);
    TEST_ASSERT_TRUE(loud > quiet);
}

TEST_CASE("the VCO core follows the carrier: the low core below 322.88 MHz, 387-430.5 and 779-861", "[cc1101]")
{
    struct {
        uint32_t hz;
        uint8_t  test0;
        uint8_t  fscal2;
        bool     needs_sw0;
    } const row[] = {
        { 387000000U, 0x0BU, 0x0AU, false },
        { 430000000U, 0x0BU, 0x0AU, false },
        { 433920000U, 0x09U, 0x2AU, false },
        { 464000000U, 0x09U, 0x2AU, false },
    };
    for (size_t i = 0U; i < sizeof(row) / sizeof(row[0]); i++) {
        begin();
        int16_t half = 0;
        TEST_ASSERT_EQUAL(KODE_OK, fck_drv_cc1101_rssi(idf_fake_hw(), NULL, row[i].hz, FCK_DRV_CC1101_WIDE, &half));
        uint8_t v = 0U;
        TEST_ASSERT_TRUE(fake_cc1101_writes(REG_TEST0, &v) >= 1U);
        TEST_ASSERT_EQUAL_HEX8(row[i].test0, v);
        (void)fake_cc1101_writes(REG_FSCAL2, &v);
        TEST_ASSERT_EQUAL_HEX8(row[i].fscal2, v);
    }
}

TEST_CASE("a level the driver cannot ask for is refused before anything is touched", "[cc1101]")
{
    begin();
    int16_t half = 0;
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_drv_cc1101_rssi(idf_fake_hw(), NULL, 500000000U, FCK_DRV_CC1101_WIDE, &half));
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_drv_cc1101_rssi(idf_fake_hw(), NULL, 433920000U, (fck_drv_cc1101_filter_t)9, &half));
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_drv_cc1101_rssi(idf_fake_hw(), NULL, 433920000U, FCK_DRV_CC1101_WIDE, NULL));
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_drv_cc1101_rssi(NULL, NULL, 433920000U, FCK_DRV_CC1101_WIDE, &half));
    TEST_ASSERT_EQUAL_UINT(0U, g_cc.transfers);
    TEST_ASSERT_EQUAL_UINT(0U, g_cc.log_n);
}

TEST_CASE("a leg that needs the expander is refused without one, and the chip is not touched", "[cc1101]")
{
    begin();
    int16_t half = 0;
    TEST_ASSERT_EQUAL_MESSAGE(KODE_DENIED,
                              fck_drv_cc1101_rssi(idf_fake_hw(), NULL, 315000000U, FCK_DRV_CC1101_WIDE, &half),
                              "315 MHz needs SW0 moved through the expander");
    TEST_ASSERT_EQUAL_UINT(0U, g_cc.transfers);
}

TEST_CASE("868 MHz without the expander is had: SW0 rests at its level, and GDO2 goes high", "[cc1101]")
{
    begin();
    int16_t half = 0;
    TEST_ASSERT_EQUAL(KODE_OK, fck_drv_cc1101_rssi(idf_fake_hw(), NULL, 868350000U, FCK_DRV_CC1101_WIDE, &half));
    uint8_t v = 0U;
    TEST_ASSERT_EQUAL_UINT(1U, fake_cc1101_writes(REG_IOCFG2, &v));
    TEST_ASSERT_EQUAL_HEX8(FCK_BSP_CC1101_IOCFG2_HIGH, v);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, g_idf.i2c_transfers, "nothing is asked of the expander");
    assert_closed("after 868 without the expander");
}

TEST_CASE("433 MHz without the expander is had: SW0 rests at its level, and GDO2 goes low", "[cc1101]")
{
    begin();
    int16_t half = 0;
    TEST_ASSERT_EQUAL(KODE_OK, fck_drv_cc1101_rssi(idf_fake_hw(), NULL, 433920000U, FCK_DRV_CC1101_WIDE, &half));
    uint8_t v = 0U;
    TEST_ASSERT_EQUAL_UINT(1U, fake_cc1101_writes(REG_IOCFG2, &v));
    TEST_ASSERT_EQUAL_HEX8(FCK_BSP_CC1101_IOCFG2_LOW, v);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, g_idf.i2c_transfers, "nothing is asked of the expander");
    assert_closed("after 433 without the expander");
}

/* Whether SW0 was written high, and whether it was written low, in the expander's OUTPUT register. */
static void sw0_writes(bool *high, bool *low)
{
    *high = false;
    *low  = false;
    for (unsigned i = 0U; i < g_idf.i2c_log_n && i < IDF_I2C_LOG; i++) {
        if (g_idf.i2c_log[i].write && g_idf.i2c_log[i].reg == FCK_BSP_EXPANDER_REGISTER_OUTPUT) {
            if ((g_idf.i2c_log[i].val & SW0) != 0U) {
                *high = true;
            } else {
                *low = true;
            }
        }
    }
}

TEST_CASE("868 MHz sets both switches: GDO2 high and SW0 high through the expander, where it rests", "[cc1101]")
{
    begin_with_expander();
    int16_t half = 0;
    TEST_ASSERT_EQUAL(KODE_OK, fck_drv_cc1101_rssi(idf_fake_hw(), &s_t, 868350000U, FCK_DRV_CC1101_WIDE, &half));
    uint8_t v = 0U;
    TEST_ASSERT_EQUAL_UINT(1U, fake_cc1101_writes(REG_IOCFG2, &v));
    TEST_ASSERT_EQUAL_HEX8(FCK_BSP_CC1101_IOCFG2_HIGH, v);

    bool high = false;
    bool low  = false;
    sw0_writes(&high, &low);
    TEST_ASSERT_TRUE_MESSAGE(high, "SW0 was written high");
    TEST_ASSERT_FALSE_MESSAGE(low, "and never low");
    TEST_ASSERT_TRUE_MESSAGE(idf_fake_exp_high(SW0), "and stays there: it is its rest");
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x00U, (uint8_t)(idf_fake_exp(FCK_BSP_EXPANDER_REGISTER_CONFIG) & SW0),
                                   "still driven there, as the owner leaves it");
    assert_closed("after 868");
}

TEST_CASE("315 MHz sets GDO2 high and SW0 low through the expander, its low VCO core, then SW0 back at rest", "[cc1101]")
{
    begin_with_expander();
    int16_t half = 0;
    TEST_ASSERT_EQUAL(KODE_OK, fck_drv_cc1101_rssi(idf_fake_hw(), &s_t, 315000000U, FCK_DRV_CC1101_WIDE, &half));
    uint8_t v = 0U;
    (void)fake_cc1101_writes(REG_IOCFG2, &v);
    TEST_ASSERT_EQUAL_HEX8(FCK_BSP_CC1101_IOCFG2_HIGH, v);
    (void)fake_cc1101_writes(REG_TEST0, &v);
    TEST_ASSERT_EQUAL_HEX8(0x0BU, v);

    bool high = false;
    bool low  = false;
    sw0_writes(&high, &low);
    TEST_ASSERT_TRUE_MESSAGE(low, "SW0 was written low");
    TEST_ASSERT_TRUE_MESSAGE(idf_fake_exp_high(SW0), "and is back at its rest, high");
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x00U, (uint8_t)(idf_fake_exp(FCK_BSP_EXPANDER_REGISTER_CONFIG) & SW0),
                                   "still driven there, as the owner leaves it");
    assert_closed("after 315");
}

TEST_CASE("433 MHz sets GDO2 low and SW0 high, and with the expander at hand never lowers SW0", "[cc1101]")
{
    begin_with_expander();
    int16_t half = 0;
    TEST_ASSERT_EQUAL(KODE_OK, fck_drv_cc1101_rssi(idf_fake_hw(), &s_t, 433920000U, FCK_DRV_CC1101_WIDE, &half));
    uint8_t v = 0U;
    TEST_ASSERT_EQUAL_UINT(1U, fake_cc1101_writes(REG_IOCFG2, &v));
    TEST_ASSERT_EQUAL_HEX8(FCK_BSP_CC1101_IOCFG2_LOW, v);

    bool high = false;
    bool low  = false;
    sw0_writes(&high, &low);
    TEST_ASSERT_TRUE_MESSAGE(high, "SW0 was written high");
    TEST_ASSERT_FALSE_MESSAGE(low, "and never low");
    TEST_ASSERT_TRUE(idf_fake_exp_high(SW0));
}

TEST_CASE("a chip that never reaches receive is an I/O error, and is shut as it was", "[cc1101]")
{
    begin();
    g_cc.never_rx = true;
    int16_t half  = 0x7777;
    TEST_ASSERT_EQUAL(KODE_IO_ERROR, fck_drv_cc1101_rssi(idf_fake_hw(), NULL, 433920000U, FCK_DRV_CC1101_WIDE, &half));
    TEST_ASSERT_EQUAL_INT16_MESSAGE(0x7777, half, "no level is claimed");
    assert_closed("after a chip that would not receive");
}

TEST_CASE("a level on a header with no chip is not a level, and leaves the bus alone", "[cc1101]")
{
    begin();
    g_cc.present  = false;
    int16_t half  = 0;
    TEST_ASSERT_NOT_EQUAL(KODE_OK, fck_drv_cc1101_rssi(idf_fake_hw(), NULL, 433920000U, FCK_DRV_CC1101_WIDE, &half));
    TEST_ASSERT_EQUAL_INT(0, g_cc.buses);
    TEST_ASSERT_EQUAL_INT(0, g_cc.devices);
}

/* ---- A sweep ------------------------------------------------------------------------- */

TEST_CASE("a sweep gives one level a point, the carrier's point loudest", "[cc1101]")
{
    begin();
    g_cc.peak_hz  = 433500000U;
    g_cc.peak_raw = 0x70U;
    int16_t half[3] = { 0, 0, 0 };
    TEST_ASSERT_EQUAL(KODE_OK, fck_drv_cc1101_spectrum(idf_fake_hw(), NULL, 433000000U, 500000U, 3U,
                                                    FCK_DRV_CC1101_WIDE, half));
    TEST_ASSERT_EQUAL_INT16(0x40 - 148, half[0]);
    TEST_ASSERT_EQUAL_INT16(0x70 - 148, half[1]);
    TEST_ASSERT_EQUAL_INT16(0x40 - 148, half[2]);
    assert_closed("after a sweep");
}

TEST_CASE("a sweep marks the points the chip does not tune and still reads the ones it does", "[cc1101]")
{
    begin();
    int16_t half[3] = { 0, 0, 0 };
    TEST_ASSERT_EQUAL(KODE_OK, fck_drv_cc1101_spectrum(idf_fake_hw(), NULL, 463000000U, 1000000U, 3U,
                                                    FCK_DRV_CC1101_WIDE, half));
    TEST_ASSERT_TRUE(half[0] != FCK_DRV_SUBGHZ_SCAN_NO_RECEIVE);
    TEST_ASSERT_TRUE(half[1] != FCK_DRV_SUBGHZ_SCAN_NO_RECEIVE);
    TEST_ASSERT_EQUAL_INT16(FCK_DRV_SUBGHZ_SCAN_NO_RECEIVE, half[2]);
}

TEST_CASE("a sweep with nothing to ask is refused, with the chip opened and shut as it was", "[cc1101]")
{
    begin();
    int16_t half[2] = { 0, 0 };
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_drv_cc1101_spectrum(idf_fake_hw(), NULL, 500000000U, 1000000U, 2U,
                                                        FCK_DRV_CC1101_WIDE, half));
    assert_closed("after a sweep with nothing in it");

    begin();
    TEST_ASSERT_EQUAL_MESSAGE(KODE_DENIED,
                              fck_drv_cc1101_spectrum(idf_fake_hw(), NULL, 315000000U, 1000000U, 2U, FCK_DRV_CC1101_WIDE, half),
                              "315 MHz without the expander is nothing to ask");
}

TEST_CASE("a sweep with bad arguments is refused before anything is touched", "[cc1101]")
{
    begin();
    int16_t half[2] = { 0, 0 };
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_drv_cc1101_spectrum(idf_fake_hw(), NULL, 433000000U, 1000000U, 0U,
                                                        FCK_DRV_CC1101_WIDE, half));
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_drv_cc1101_spectrum(idf_fake_hw(), NULL, 433000000U, 1000000U, 2U,
                                                        (fck_drv_cc1101_filter_t)7, half));
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_drv_cc1101_spectrum(idf_fake_hw(), NULL, 433000000U, 1000000U, 2U,
                                                        FCK_DRV_CC1101_WIDE, NULL));
    TEST_ASSERT_EQUAL_MESSAGE(KODE_DENIED,
                              fck_drv_cc1101_spectrum(idf_fake_hw(), NULL, 4294967000U, 1000U, 3U, FCK_DRV_CC1101_WIDE, half),
                              "a sweep that runs past 2^32 Hz");
    TEST_ASSERT_EQUAL_UINT(0U, g_cc.transfers);
}
