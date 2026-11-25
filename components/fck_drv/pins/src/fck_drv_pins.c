/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file fck_drv_pins.c
 * @brief The module's lines as plain GPIOs. See fck_drv_pins.h.
 */
#include "fck_drv_pins.h"

#include "driver/gpio.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* How long a line is given to settle against a pull before it is read. */
#define LINES_SETTLE_US   300U
/* EN low long enough to discharge C503 (1 uF against R502's 10 k). */
#define C5_RESET_MS       60U

/* Set once, by fck_drv_pins_c5_park(); the P4's restart is what clears it. */
static volatile bool s_c5_parked;

static void line_input(int gpio, bool pull_up, bool pull_down)
{
    const gpio_config_t g = {
        .pin_bit_mask = 1ULL << gpio,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = pull_up ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
        .pull_down_en = pull_down ? GPIO_PULLDOWN_ENABLE : GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    (void)gpio_config(&g);
}

/* Low, and only ever low: the level is set before the direction, so the pin
 * becomes an output already at 0. */
static void line_low(int gpio)
{
    (void)gpio_set_level(gpio, 0);
    const gpio_config_t g = {
        .pin_bit_mask = 1ULL << gpio,
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    (void)gpio_config(&g);
    (void)gpio_set_level(gpio, 0);
}

void fck_drv_pins_rest(const fck_drv_pins_t *hw)
{
    for (uint8_t pos = 1U; pos <= FCK_BSP_POSITIONS; pos++) {
        line_input(fck_drv_pins_gpio(hw, pos), pos == FCK_BSP_POSITION_CC_CS, false);
    }
}

static bool held(int gpio)
{
    line_input(gpio, false, true);
    esp_rom_delay_us(LINES_SETTLE_US);
    bool high = gpio_get_level(gpio) != 0;
    line_input(gpio, false, false);
    return high;
}

bool fck_drv_pins_pair_held(const fck_drv_pins_t *hw)
{
    bool scl = held(fck_drv_pins_gpio(hw, FCK_BSP_POSITION_I2C_SCL));
    bool sda = held(fck_drv_pins_gpio(hw, FCK_BSP_POSITION_I2C_SDA));
    return scl && sda;
}

bool fck_drv_pins_scl_held(const fck_drv_pins_t *hw)
{
    return held(fck_drv_pins_gpio(hw, FCK_BSP_POSITION_I2C_SCL));
}

void fck_drv_pins_c5_reset(const fck_drv_pins_t *hw, bool to_loader, bool hold)
{
    if (s_c5_parked) {
        return;
    }
    int boot = fck_drv_pins_gpio(hw, FCK_BSP_POSITION_C5_BOOT);
    if (to_loader) {
        line_low(boot);
    } else {
        line_input(boot, true, false);
    }
    line_low(fck_drv_pins_gpio(hw, FCK_BSP_POSITION_C5_EN));
    vTaskDelay(pdMS_TO_TICKS(C5_RESET_MS));
    if (!hold) {
        fck_drv_pins_c5_run(hw);
    }
}

void fck_drv_pins_c5_run(const fck_drv_pins_t *hw)
{
    if (s_c5_parked) {
        return;
    }
    line_input(fck_drv_pins_gpio(hw, FCK_BSP_POSITION_C5_EN), true, false);
}

void fck_drv_pins_c5_release_boot(const fck_drv_pins_t *hw)
{
    if (s_c5_parked) {
        return;
    }
    line_input(fck_drv_pins_gpio(hw, FCK_BSP_POSITION_C5_BOOT), true, false);
}

void fck_drv_pins_c5_park(const fck_drv_pins_t *hw)
{
    s_c5_parked = true;
    /* Twice: a call that passed its check before the flag was set may still
     * be writing a line, and it ends well within the tick between the two. */
    for (int i = 0; i < 2; i++) {
        line_input(fck_drv_pins_gpio(hw, FCK_BSP_POSITION_C5_BOOT), true, false);
        line_low(fck_drv_pins_gpio(hw, FCK_BSP_POSITION_C5_EN));
        if (i == 0) {
            vTaskDelay(1);
        }
    }
}
