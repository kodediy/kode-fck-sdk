/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_pins.c
 * @brief fck_drv_pins: the module's lines as plain GPIOs, against a fake GPIO.
 *
 * The cases share one process, and fck_drv_pins_c5_park() is one-way (only a
 * restart of the P4 clears it): the case that parks is the file's last.
 */
#include "unity.h"

#include "fck_drv_pins.h"

#include "fake_idf.h"

static int gpio_of(uint8_t pos)
{
    return idf_fake_gpio(pos);
}

TEST_CASE("rest makes every line an input with no pull, but the CC1101's chip select", "[pins]")
{
    idf_fake_reset();
    fck_drv_pins_rest(idf_fake_hw());
    for (uint8_t pos = 1U; pos <= FCK_BSP_POSITIONS; pos++) {
        char why[40];
        (void)snprintf(why, sizeof(why), "position %u", (unsigned)pos);
        TEST_ASSERT_EQUAL_MESSAGE(GPIO_MODE_INPUT, g_idf.pin[gpio_of(pos)].mode, why);
        TEST_ASSERT_EQUAL_MESSAGE(pos == FCK_BSP_POSITION_CC_CS, g_idf.pin[gpio_of(pos)].pull_up, why);
        TEST_ASSERT_FALSE_MESSAGE(g_idf.pin[gpio_of(pos)].pull_down, why);
    }
}

TEST_CASE("the I2C pair is held when both lines read high against a pull-down", "[pins]")
{
    idf_fake_reset();
    g_idf.pin[gpio_of(FCK_BSP_POSITION_I2C_SCL)].ext = 1;
    g_idf.pin[gpio_of(FCK_BSP_POSITION_I2C_SDA)].ext = 1;
    TEST_ASSERT_TRUE(fck_drv_pins_pair_held(idf_fake_hw()));
    TEST_ASSERT_TRUE(fck_drv_pins_scl_held(idf_fake_hw()));
    TEST_ASSERT_FALSE_MESSAGE(g_idf.pin[gpio_of(FCK_BSP_POSITION_I2C_SDA)].pull_down,
                              "the pull-down used to ask is taken off again");
    TEST_ASSERT_FALSE(g_idf.pin[gpio_of(FCK_BSP_POSITION_I2C_SCL)].pull_down);
}

TEST_CASE("an empty header, or one line low, does not hold the pair", "[pins]")
{
    idf_fake_reset();
    TEST_ASSERT_FALSE_MESSAGE(fck_drv_pins_pair_held(idf_fake_hw()), "nothing holds them: the pull-down wins");
    TEST_ASSERT_FALSE(fck_drv_pins_scl_held(idf_fake_hw()));

    g_idf.pin[gpio_of(FCK_BSP_POSITION_I2C_SCL)].ext = 1;
    g_idf.pin[gpio_of(FCK_BSP_POSITION_I2C_SDA)].ext = 0;
    TEST_ASSERT_FALSE(fck_drv_pins_pair_held(idf_fake_hw()));
    TEST_ASSERT_TRUE_MESSAGE(fck_drv_pins_scl_held(idf_fake_hw()), "SCL alone is held");
}

TEST_CASE("a C5 reset to its loader holds BOOT low, pulls EN low, then lets EN go", "[pins]")
{
    idf_fake_reset();
    int const boot = gpio_of(FCK_BSP_POSITION_C5_BOOT);
    int const en   = gpio_of(FCK_BSP_POSITION_C5_EN);
    fck_drv_pins_c5_reset(idf_fake_hw(), true, false);
    TEST_ASSERT_EQUAL(GPIO_MODE_OUTPUT, g_idf.pin[boot].mode);
    TEST_ASSERT_EQUAL_INT(0, g_idf.pin[boot].level);
    TEST_ASSERT_EQUAL_MESSAGE(GPIO_MODE_INPUT, g_idf.pin[en].mode, "EN is left to its resistor");
    TEST_ASSERT_TRUE(g_idf.pin[en].pull_up);
    TEST_ASSERT_TRUE_MESSAGE(g_idf.pin[en].configs >= 2U, "it was pulled low first");
}

TEST_CASE("a C5 reset to its own firmware lets BOOT go, and holds EN low when asked to", "[pins]")
{
    idf_fake_reset();
    int const boot = gpio_of(FCK_BSP_POSITION_C5_BOOT);
    int const en   = gpio_of(FCK_BSP_POSITION_C5_EN);
    fck_drv_pins_c5_reset(idf_fake_hw(), false, true);
    TEST_ASSERT_EQUAL(GPIO_MODE_INPUT, g_idf.pin[boot].mode);
    TEST_ASSERT_TRUE(g_idf.pin[boot].pull_up);
    TEST_ASSERT_EQUAL(GPIO_MODE_OUTPUT, g_idf.pin[en].mode);
    TEST_ASSERT_EQUAL_INT(0, g_idf.pin[en].level);

    fck_drv_pins_c5_run(idf_fake_hw());
    TEST_ASSERT_EQUAL(GPIO_MODE_INPUT, g_idf.pin[en].mode);
    TEST_ASSERT_TRUE(g_idf.pin[en].pull_up);
}

TEST_CASE("release_boot lets BOOT go once the ROM has latched it", "[pins]")
{
    idf_fake_reset();
    int const boot = gpio_of(FCK_BSP_POSITION_C5_BOOT);
    fck_drv_pins_c5_reset(idf_fake_hw(), true, true);
    TEST_ASSERT_EQUAL(GPIO_MODE_OUTPUT, g_idf.pin[boot].mode);
    fck_drv_pins_c5_release_boot(idf_fake_hw());
    TEST_ASSERT_EQUAL(GPIO_MODE_INPUT, g_idf.pin[boot].mode);
    TEST_ASSERT_TRUE(g_idf.pin[boot].pull_up);
}

TEST_CASE("a parked C5 is held in reset and no call after that touches its lines", "[pins]")
{
    idf_fake_reset();
    int const boot = gpio_of(FCK_BSP_POSITION_C5_BOOT);
    int const en   = gpio_of(FCK_BSP_POSITION_C5_EN);
    fck_drv_pins_c5_park(idf_fake_hw());
    TEST_ASSERT_EQUAL(GPIO_MODE_OUTPUT, g_idf.pin[en].mode);
    TEST_ASSERT_EQUAL_INT(0, g_idf.pin[en].level);
    TEST_ASSERT_EQUAL(GPIO_MODE_INPUT, g_idf.pin[boot].mode);

    unsigned const boot_configs = g_idf.pin[boot].configs;
    unsigned const en_configs   = g_idf.pin[en].configs;
    fck_drv_pins_c5_reset(idf_fake_hw(), true, false);
    fck_drv_pins_c5_run(idf_fake_hw());
    fck_drv_pins_c5_release_boot(idf_fake_hw());
    TEST_ASSERT_EQUAL_UINT(boot_configs, g_idf.pin[boot].configs);
    TEST_ASSERT_EQUAL_UINT(en_configs, g_idf.pin[en].configs);
    TEST_ASSERT_EQUAL_MESSAGE(GPIO_MODE_OUTPUT, g_idf.pin[en].mode, "EN stays low");
}
