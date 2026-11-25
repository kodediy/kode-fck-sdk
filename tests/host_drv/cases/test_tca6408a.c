/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_tca6408a.c
 * @brief fck_drv_tca6408a: the module's I2C pair and the expander on it,
 *        against a fake I2C bus with a TCA6408A at 0x21.
 */
#include <string.h>

#include "unity.h"

#include "fck_bsp.h"
#include "fck_drv_tca6408a.h"

#include "fake_idf.h"

#define OUT FCK_BSP_EXPANDER_REGISTER_OUTPUT
#define CFG FCK_BSP_EXPANDER_REGISTER_CONFIG
#define P2  ((uint8_t)(1U << FCK_BSP_EXPANDER_GNSS_RST))
#define P3  ((uint8_t)(1U << FCK_BSP_EXPANDER_CC_SW0))

static fck_drv_tca6408a_t s_t;

/* An expander at 0x21, its bus open, the shadows at the power-on values. */
static void begin(void)
{
    idf_fake_reset();
    idf_fake_expander(0xF7U);
    (void)memset(&s_t, 0, sizeof(s_t));
    fck_drv_tca6408a_forget(&s_t);
    TEST_ASSERT_EQUAL(KODE_OK, fck_drv_tca6408a_open(&s_t, idf_fake_hw()));
}

/* The log's writes, in order, as reg:val. */
static unsigned writes(uint8_t *regs, uint8_t *vals, unsigned cap)
{
    unsigned n = 0U;
    for (unsigned i = 0U; i < g_idf.i2c_log_n && i < IDF_I2C_LOG && n < cap; i++) {
        if (g_idf.i2c_log[i].write) {
            regs[n] = g_idf.i2c_log[i].reg;
            vals[n] = g_idf.i2c_log[i].val;
            n++;
        }
    }
    return n;
}

TEST_CASE("open makes a bus on the module's pair and the controller kode-sdk lent", "[tca6408a]")
{
    idf_fake_reset();
    (void)memset(&s_t, 0, sizeof(s_t));
    TEST_ASSERT_EQUAL(KODE_OK, fck_drv_tca6408a_open(&s_t, idf_fake_hw()));
    TEST_ASSERT_TRUE(fck_drv_tca6408a_is_open(&s_t));
    TEST_ASSERT_EQUAL_INT(idf_fake_gpio(FCK_BSP_POSITION_I2C_SCL), g_idf.i2c_scl);
    TEST_ASSERT_EQUAL_INT(idf_fake_gpio(FCK_BSP_POSITION_I2C_SDA), g_idf.i2c_sda);
    TEST_ASSERT_EQUAL_INT(idf_fake_hw()->i2c_port, g_idf.i2c_port);
    TEST_ASSERT_EQUAL_INT(1, g_idf.i2c_buses);
    TEST_ASSERT_EQUAL_INT(1, g_idf.i2c_devices);
    /* Open again: the one bus is the bus. */
    TEST_ASSERT_EQUAL(KODE_OK, fck_drv_tca6408a_open(&s_t, idf_fake_hw()));
    TEST_ASSERT_EQUAL_INT(1, g_idf.i2c_buses);
}

TEST_CASE("open that ESP-IDF refuses is an error, and a device it refuses takes the bus back", "[tca6408a]")
{
    idf_fake_reset();
    (void)memset(&s_t, 0, sizeof(s_t));
    g_idf.i2c_new_rc = ESP_FAIL;
    TEST_ASSERT_EQUAL(KODE_ERROR, fck_drv_tca6408a_open(&s_t, idf_fake_hw()));
    TEST_ASSERT_FALSE(fck_drv_tca6408a_is_open(&s_t));

    idf_fake_reset();
    g_idf.i2c_add_rc = ESP_FAIL;
    TEST_ASSERT_EQUAL(KODE_ERROR, fck_drv_tca6408a_open(&s_t, idf_fake_hw()));
    TEST_ASSERT_FALSE(fck_drv_tca6408a_is_open(&s_t));
    TEST_ASSERT_EQUAL_MESSAGE(0, g_idf.i2c_buses, "no bus is left holding the pair");
}

TEST_CASE("close removes the device and the bus; one ESP-IDF keeps stays open", "[tca6408a]")
{
    begin();
    TEST_ASSERT_TRUE(fck_drv_tca6408a_close(&s_t));
    TEST_ASSERT_FALSE(fck_drv_tca6408a_is_open(&s_t));
    TEST_ASSERT_EQUAL_INT(0, g_idf.i2c_buses);
    TEST_ASSERT_EQUAL_INT(0, g_idf.i2c_devices);

    begin();
    g_idf.i2c_rm_rc = ESP_FAIL;
    TEST_ASSERT_FALSE(fck_drv_tca6408a_close(&s_t));
    TEST_ASSERT_TRUE_MESSAGE(fck_drv_tca6408a_is_open(&s_t), "the handles are kept: nothing opens a second bus over them");
    g_idf.i2c_rm_rc = ESP_OK;
    g_idf.i2c_del_rc = ESP_FAIL;
    TEST_ASSERT_FALSE(fck_drv_tca6408a_close(&s_t));
    TEST_ASSERT_TRUE(fck_drv_tca6408a_is_open(&s_t));
}

TEST_CASE("probe stops at the first answer and writes nothing", "[tca6408a]")
{
    begin();
    uint32_t nacked = 9U;
    uint32_t failed = 9U;
    TEST_ASSERT_TRUE(fck_drv_tca6408a_probe(&s_t, 4U, &nacked, &failed));
    TEST_ASSERT_EQUAL_UINT(1U, g_idf.i2c_probes);
    TEST_ASSERT_EQUAL_UINT32(0U, nacked);
    TEST_ASSERT_EQUAL_UINT32(0U, failed);
    TEST_ASSERT_EQUAL_UINT(0U, g_idf.i2c_log_n);
}

TEST_CASE("probe tells an absent chip (NAK) from a bus that misbehaves", "[tca6408a]")
{
    begin();
    g_idf.i2c_device = false;
    uint32_t nacked  = 0U;
    uint32_t failed  = 0U;
    TEST_ASSERT_FALSE(fck_drv_tca6408a_probe(&s_t, 3U, &nacked, &failed));
    TEST_ASSERT_EQUAL_UINT32(3U, nacked);
    TEST_ASSERT_EQUAL_UINT32(0U, failed);

    g_idf.i2c_always_fail = 1U;
    TEST_ASSERT_FALSE(fck_drv_tca6408a_probe(&s_t, 2U, &nacked, &failed));
    TEST_ASSERT_EQUAL_UINT32(0U, nacked);
    TEST_ASSERT_EQUAL_UINT32(2U, failed);
}

TEST_CASE("probe on a closed bus finds nothing and counts nothing", "[tca6408a]")
{
    idf_fake_reset();
    idf_fake_expander(0xF7U);
    (void)memset(&s_t, 0, sizeof(s_t));
    uint32_t nacked = 7U;
    uint32_t failed = 7U;
    TEST_ASSERT_FALSE(fck_drv_tca6408a_probe(&s_t, 4U, &nacked, &failed));
    TEST_ASSERT_EQUAL_UINT32(0U, nacked);
    TEST_ASSERT_EQUAL_UINT32(0U, failed);
    TEST_ASSERT_EQUAL_UINT(0U, g_idf.i2c_probes);
}

TEST_CASE("read gives a register, tries four times, and says IO error after that", "[tca6408a]")
{
    begin();
    uint8_t v = 0U;
    TEST_ASSERT_EQUAL(KODE_OK, fck_drv_tca6408a_read(&s_t, FCK_BSP_EXPANDER_REGISTER_INPUT, &v));
    TEST_ASSERT_EQUAL_HEX8(0xF7U, v);

    g_idf.i2c_fail_next = 3U;
    g_idf.i2c_transfers = 0U;
    TEST_ASSERT_EQUAL(KODE_OK, fck_drv_tca6408a_read(&s_t, FCK_BSP_EXPANDER_REGISTER_INPUT, &v));
    TEST_ASSERT_EQUAL_UINT(4U, g_idf.i2c_transfers);

    g_idf.i2c_always_fail = 1U;
    g_idf.i2c_transfers   = 0U;
    TEST_ASSERT_EQUAL(KODE_IO_ERROR, fck_drv_tca6408a_read(&s_t, FCK_BSP_EXPANDER_REGISTER_INPUT, &v));
    TEST_ASSERT_EQUAL_UINT(4U, g_idf.i2c_transfers);
}

TEST_CASE("read and write on a closed bus or with no place for the answer are refused", "[tca6408a]")
{
    idf_fake_reset();
    (void)memset(&s_t, 0, sizeof(s_t));
    uint8_t v = 0U;
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_drv_tca6408a_read(&s_t, 0U, &v));
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_drv_tca6408a_write(&s_t, OUT, 0U));
    begin();
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_drv_tca6408a_read(&s_t, 0U, NULL));
}

TEST_CASE("write remembers what OUTPUT and CONFIG were last given, and only those", "[tca6408a]")
{
    begin();
    TEST_ASSERT_EQUAL_HEX8(0xFFU, s_t.out);
    TEST_ASSERT_EQUAL(KODE_OK, fck_drv_tca6408a_write(&s_t, OUT, 0xA5U));
    TEST_ASSERT_EQUAL(KODE_OK, fck_drv_tca6408a_write(&s_t, CFG, 0x5AU));
    TEST_ASSERT_EQUAL(KODE_OK, fck_drv_tca6408a_write(&s_t, FCK_BSP_EXPANDER_REGISTER_POLARITY, 0x80U));
    TEST_ASSERT_EQUAL_HEX8(0xA5U, s_t.out);
    TEST_ASSERT_EQUAL_HEX8(0x5AU, s_t.cfg);
    TEST_ASSERT_EQUAL_HEX8(0xA5U, idf_fake_exp(OUT));
    TEST_ASSERT_EQUAL_HEX8(0x80U, idf_fake_exp(FCK_BSP_EXPANDER_REGISTER_POLARITY));

    g_idf.i2c_always_fail = 1U;
    TEST_ASSERT_EQUAL(KODE_IO_ERROR, fck_drv_tca6408a_write(&s_t, OUT, 0x00U));
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0xA5U, s_t.out, "a write that did not happen is not remembered");
    g_idf.i2c_always_fail = 0U;
    fck_drv_tca6408a_forget(&s_t);
    TEST_ASSERT_EQUAL_HEX8(0xFFU, s_t.out);
    TEST_ASSERT_EQUAL_HEX8(0xFFU, s_t.cfg);
}

TEST_CASE("mask drives a pin: the level first, then the direction, so it is never wrong for a moment", "[tca6408a]")
{
    begin();
    TEST_ASSERT_EQUAL(KODE_OK, fck_drv_tca6408a_mask(&s_t, P3, true, true));
    uint8_t regs[4];
    uint8_t vals[4];
    TEST_ASSERT_EQUAL_UINT(2U, writes(regs, vals, 4U));
    TEST_ASSERT_EQUAL_HEX8(OUT, regs[0]);
    TEST_ASSERT_EQUAL_HEX8(CFG, regs[1]);
    TEST_ASSERT_EQUAL_HEX8(0xFFU, vals[0]);                           /* high */
    TEST_ASSERT_EQUAL_HEX8((uint8_t)~P3, vals[1]);                    /* an output */
    TEST_ASSERT_TRUE(idf_fake_exp_high(P3));
}

TEST_CASE("mask lets a pin go: the direction first, then the level", "[tca6408a]")
{
    begin();
    TEST_ASSERT_EQUAL(KODE_OK, fck_drv_tca6408a_mask(&s_t, P3, true, false));
    g_idf.i2c_log_n = 0U;
    TEST_ASSERT_EQUAL(KODE_OK, fck_drv_tca6408a_mask(&s_t, P3, false, false));
    uint8_t regs[4];
    uint8_t vals[4];
    TEST_ASSERT_EQUAL_UINT(2U, writes(regs, vals, 4U));
    TEST_ASSERT_EQUAL_HEX8(CFG, regs[0]);
    TEST_ASSERT_EQUAL_HEX8(OUT, regs[1]);
    TEST_ASSERT_EQUAL_HEX8(0xFFU, vals[0]);
}

TEST_CASE("mask touches only the bits it was given, whatever else the chip holds", "[tca6408a]")
{
    begin();
    g_idf.exp_reg[1] = 0x5CU;
    g_idf.exp_reg[3] = 0xB3U;
    TEST_ASSERT_EQUAL(KODE_OK, fck_drv_tca6408a_mask(&s_t, P2, true, true));
    TEST_ASSERT_EQUAL_HEX8(0x5CU | P2, idf_fake_exp(OUT));
    TEST_ASSERT_EQUAL_HEX8(0xB3U & (uint8_t)~P2, idf_fake_exp(CFG));
}

TEST_CASE("mask with no bits, or on a closed bus, is refused", "[tca6408a]")
{
    begin();
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_drv_tca6408a_mask(&s_t, 0U, true, true));
    TEST_ASSERT_TRUE(fck_drv_tca6408a_close(&s_t));
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_drv_tca6408a_mask(&s_t, P2, true, true));
}

TEST_CASE("mask that cannot read the chip goes on from the shadows, not from the power-on values", "[tca6408a]")
{
    begin();
    TEST_ASSERT_EQUAL(KODE_OK, fck_drv_tca6408a_mask(&s_t, P3, true, true));   /* SW0 driven high */
    /* Both reads fail (four tries each); the writes after them do not. */
    g_idf.i2c_fail_next = 8U;
    TEST_ASSERT_EQUAL(KODE_OK, fck_drv_tca6408a_mask(&s_t, P2, true, false));
    TEST_ASSERT_TRUE_MESSAGE(idf_fake_exp_high(P3), "SW0, driven before, is not let go by a read that missed");
    TEST_ASSERT_EQUAL_HEX8(0x00U, (uint8_t)(idf_fake_exp(CFG) & (P2 | P3)));
}

TEST_CASE("a mask that fails to write says IO error", "[tca6408a]")
{
    begin();
    g_idf.i2c_always_fail = 1U;
    TEST_ASSERT_EQUAL(KODE_IO_ERROR, fck_drv_tca6408a_mask(&s_t, P2, true, true));
}

TEST_CASE("mask_patient asks again until the bus answers, or the time is up", "[tca6408a]")
{
    begin();
    TEST_ASSERT_TRUE(fck_drv_tca6408a_close(&s_t));
    TEST_ASSERT_EQUAL(KODE_OK, fck_drv_tca6408a_mask_patient(&s_t, idf_fake_hw(), P3, true, true, 100U));
    TEST_ASSERT_TRUE_MESSAGE(fck_drv_tca6408a_is_open(&s_t), "it opened the bus itself");
    TEST_ASSERT_TRUE(idf_fake_exp_high(P3));

    g_idf.i2c_always_fail = 1U;
    TEST_ASSERT_EQUAL(KODE_IO_ERROR, fck_drv_tca6408a_mask_patient(&s_t, idf_fake_hw(), P3, true, false, 30U));
    TEST_ASSERT_FALSE_MESSAGE(fck_drv_tca6408a_is_open(&s_t),
                              "a bus that timed out is closed: the next try gets one made afresh");
    TEST_ASSERT_EQUAL_INT(0, g_idf.i2c_buses);
}

TEST_CASE("the blind reset of the GNSS receiver is by hand, with the bus away and then back", "[tca6408a]")
{
    begin();
    TEST_ASSERT_EQUAL(KODE_OK, fck_drv_tca6408a_mask(&s_t, P3, true, true));
    fck_drv_tca6408a_blind_gnss_reset(&s_t, idf_fake_hw());
    int const scl = idf_fake_gpio(FCK_BSP_POSITION_I2C_SCL);
    int const sda = idf_fake_gpio(FCK_BSP_POSITION_I2C_SDA);
    TEST_ASSERT_TRUE_MESSAGE(g_idf.pin[scl].drive_caps > 0U, "the lines are driven hard to win the bus");
    TEST_ASSERT_TRUE(g_idf.pin[sda].drive_caps > 0U);
    TEST_ASSERT_EQUAL_MESSAGE(GPIO_MODE_INPUT, g_idf.pin[scl].mode, "and let go after");
    TEST_ASSERT_EQUAL(GPIO_MODE_INPUT, g_idf.pin[sda].mode);
    TEST_ASSERT_EQUAL_HEX8(0x00U, (uint8_t)(s_t.out & P2));
    TEST_ASSERT_EQUAL_HEX8(0x00U, (uint8_t)(s_t.cfg & P2));
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(P3, (uint8_t)(s_t.out & P3), "what else the owner had driven stays");
    TEST_ASSERT_TRUE_MESSAGE(fck_drv_tca6408a_is_open(&s_t), "the bus is opened again");
    TEST_ASSERT_EQUAL_INT(1, g_idf.i2c_buses);
}

TEST_CASE("the blind reset does not touch the pair while a bus of ESP-IDF's still holds it", "[tca6408a]")
{
    begin();
    g_idf.i2c_rm_rc = ESP_FAIL;
    int const scl   = idf_fake_gpio(FCK_BSP_POSITION_I2C_SCL);
    int const sda   = idf_fake_gpio(FCK_BSP_POSITION_I2C_SDA);
    fck_drv_tca6408a_blind_gnss_reset(&s_t, idf_fake_hw());
    TEST_ASSERT_EQUAL_UINT(0U, g_idf.pin[scl].configs);
    TEST_ASSERT_EQUAL_UINT(0U, g_idf.pin[sda].configs);
    TEST_ASSERT_EQUAL_HEX8(0xFFU, s_t.out);
}
