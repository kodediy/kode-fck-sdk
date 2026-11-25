/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file fck_drv_tca6408a.c
 * @brief The module's I2C bus and its expander. See fck_drv_tca6408a.h.
 */
#include "fck_drv_tca6408a.h"

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TCA_XFER_MS     20
#define TCA_TRIES       4U
#define TCA_TRY_GAP_MS  3U
#define TCA_PATIENT_GAP_MS 10U
/* One bit-banged half period: about 100 kHz. */
#define BB_US           5U

kode_status_t fck_drv_tca6408a_open(fck_drv_tca6408a_t *t, const fck_drv_pins_t *hw)
{
    if (t->bus != NULL) {
        return KODE_OK;
    }
    t->scl = fck_drv_pins_gpio(hw, FCK_BSP_POSITION_I2C_SCL);
    t->sda = fck_drv_pins_gpio(hw, FCK_BSP_POSITION_I2C_SDA);
    const i2c_master_bus_config_t cfg = {
        .i2c_port          = hw->i2c_port,   /* kode_sys_exp_ctrl()'s: I2C_NUM_1 */
        .sda_io_num        = (gpio_num_t)t->sda,
        .scl_io_num        = (gpio_num_t)t->scl,
        .clk_source        = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = false,   /* the module has 2.2 k to its 3V3 */
    };
    i2c_master_bus_handle_t bus = NULL;
    if (i2c_new_master_bus(&cfg, &bus) != ESP_OK) {
        return KODE_ERROR;
    }
    const i2c_device_config_t dcfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = FCK_BSP_EXPANDER_ADDRESS,
        .scl_speed_hz    = FCK_BSP_I2C_HZ,
    };
    i2c_master_dev_handle_t dev = NULL;
    if (i2c_master_bus_add_device(bus, &dcfg, &dev) != ESP_OK) {
        (void)i2c_del_master_bus(bus);
        return KODE_ERROR;
    }
    t->bus = bus;
    t->dev = dev;
    return KODE_OK;
}

bool fck_drv_tca6408a_close(fck_drv_tca6408a_t *t)
{
    if (t->dev != NULL) {
        if (i2c_master_bus_rm_device((i2c_master_dev_handle_t)t->dev) != ESP_OK) {
            return false;
        }
        t->dev = NULL;
    }
    if (t->bus != NULL) {
        if (i2c_del_master_bus((i2c_master_bus_handle_t)t->bus) != ESP_OK) {
            return false;
        }
        t->bus = NULL;
    }
    return true;
}

bool fck_drv_tca6408a_is_open(const fck_drv_tca6408a_t *t)
{
    return t->bus != NULL;
}

bool fck_drv_tca6408a_probe(fck_drv_tca6408a_t *t, uint32_t tries, uint32_t *nacked, uint32_t *failed)
{
    uint32_t n = 0U;
    uint32_t f = 0U;
    bool     found = false;
    if (t->bus != NULL) {
        for (uint32_t i = 0U; i < tries && !found; i++) {
            esp_err_t err = i2c_master_probe((i2c_master_bus_handle_t)t->bus,
                                             FCK_BSP_EXPANDER_ADDRESS, TCA_XFER_MS);
            if (err == ESP_OK) {
                found = true;
            } else {
                if (err == ESP_ERR_NOT_FOUND) {
                    n++;
                } else {
                    f++;
                }
                if (i + 1U < tries) {
                    vTaskDelay(pdMS_TO_TICKS(TCA_TRY_GAP_MS));
                }
            }
        }
    }
    if (nacked != NULL) {
        *nacked = n;
    }
    if (failed != NULL) {
        *failed = f;
    }
    return found;
}

void fck_drv_tca6408a_forget(fck_drv_tca6408a_t *t)
{
    t->out = 0xFFU;
    t->cfg = 0xFFU;
}

kode_status_t fck_drv_tca6408a_read(fck_drv_tca6408a_t *t, uint8_t reg, uint8_t *val)
{
    if (t->dev == NULL || val == NULL) {
        return KODE_DENIED;
    }
    for (uint32_t i = 0U; i < TCA_TRIES; i++) {
        if (i2c_master_transmit_receive((i2c_master_dev_handle_t)t->dev, &reg, 1U, val, 1U,
                                        TCA_XFER_MS) == ESP_OK) {
            return KODE_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(TCA_TRY_GAP_MS));
    }
    return KODE_IO_ERROR;
}

kode_status_t fck_drv_tca6408a_write(fck_drv_tca6408a_t *t, uint8_t reg, uint8_t val)
{
    if (t->dev == NULL) {
        return KODE_DENIED;
    }
    const uint8_t frame[2] = { reg, val };
    for (uint32_t i = 0U; i < TCA_TRIES; i++) {
        if (i2c_master_transmit((i2c_master_dev_handle_t)t->dev, frame, sizeof(frame),
                                TCA_XFER_MS) == ESP_OK) {
            if (reg == FCK_BSP_EXPANDER_REGISTER_OUTPUT) {
                t->out = val;
            } else if (reg == FCK_BSP_EXPANDER_REGISTER_CONFIG) {
                t->cfg = val;
            }
            return KODE_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(TCA_TRY_GAP_MS));
    }
    return KODE_IO_ERROR;
}

kode_status_t fck_drv_tca6408a_mask(fck_drv_tca6408a_t *t, uint8_t mask, bool drive, bool level)
{
    if (mask == 0U) {
        return KODE_DENIED;
    }
    if (t->dev == NULL) {
        return KODE_DENIED;
    }
    uint8_t out = 0U;
    uint8_t cfg = 0U;
    if (fck_drv_tca6408a_read(t, FCK_BSP_EXPANDER_REGISTER_OUTPUT, &out) != KODE_OK) {
        out = t->out;
    }
    if (fck_drv_tca6408a_read(t, FCK_BSP_EXPANDER_REGISTER_CONFIG, &cfg) != KODE_OK) {
        cfg = t->cfg;
    }
    out = level ? (uint8_t)(out | mask) : (uint8_t)(out & (uint8_t)~mask);
    cfg = drive ? (uint8_t)(cfg & (uint8_t)~mask) : (uint8_t)(cfg | mask);
    uint8_t const first  = drive ? FCK_BSP_EXPANDER_REGISTER_OUTPUT : FCK_BSP_EXPANDER_REGISTER_CONFIG;
    uint8_t const second = drive ? FCK_BSP_EXPANDER_REGISTER_CONFIG : FCK_BSP_EXPANDER_REGISTER_OUTPUT;
    kode_status_t st = fck_drv_tca6408a_write(t, first, drive ? out : cfg);
    if (st != KODE_OK) {
        return st;
    }
    return fck_drv_tca6408a_write(t, second, drive ? cfg : out);
}

kode_status_t fck_drv_tca6408a_mask_patient(fck_drv_tca6408a_t *t, const fck_drv_pins_t *hw, uint8_t mask,
                                   bool drive, bool level, uint32_t ms)
{
    int64_t const until = esp_timer_get_time() + (int64_t)ms * 1000;
    kode_status_t st    = KODE_IO_ERROR;
    do {
        if (fck_drv_tca6408a_open(t, hw) == KODE_OK) {
            st = fck_drv_tca6408a_mask(t, mask, drive, level);
            if (st == KODE_OK) {
                return st;
            }
            /* A probe or a transfer that timed out leaves the driver's state
             * behind it: the next try gets a bus made afresh. */
            (void)fck_drv_tca6408a_close(t);
        }
        vTaskDelay(pdMS_TO_TICKS(TCA_PATIENT_GAP_MS));
    } while (esp_timer_get_time() < until);
    return st;
}

/* ---- By hand ---------------------------------------------------------------- */

static void bb(const fck_drv_tca6408a_t *t, int scl, int sda)
{
    (void)gpio_set_level(t->scl, scl);
    (void)gpio_set_level(t->sda, sda);
    esp_rom_delay_us(BB_US);
}

static void bb_byte(const fck_drv_tca6408a_t *t, uint8_t b)
{
    for (int i = 7; i >= 0; i--) {
        int v = (b >> i) & 1;
        bb(t, 0, v);
        bb(t, 1, v);
        bb(t, 0, v);
    }
    /* The ninth clock, the acknowledgement, is not looked at, and SDA is let
     * go for it rather than fought over with the chip. */
    (void)gpio_set_direction(t->sda, GPIO_MODE_INPUT);
    esp_rom_delay_us(BB_US);
    (void)gpio_set_level(t->scl, 1);
    esp_rom_delay_us(BB_US);
    (void)gpio_set_level(t->scl, 0);
    esp_rom_delay_us(BB_US);
    (void)gpio_set_direction(t->sda, GPIO_MODE_OUTPUT);
    (void)gpio_set_drive_capability(t->sda, GPIO_DRIVE_CAP_3);
    (void)gpio_set_level(t->sda, 0);
}

static void bb_write(const fck_drv_tca6408a_t *t, uint8_t reg, uint8_t val)
{
    bb(t, 1, 1);
    esp_rom_delay_us(50U);
    bb(t, 1, 0);
    bb(t, 0, 0);                      /* START */
    bb_byte(t, (uint8_t)(FCK_BSP_EXPANDER_ADDRESS << 1));
    bb_byte(t, reg);
    bb_byte(t, val);
    bb(t, 0, 0);
    bb(t, 1, 0);
    bb(t, 1, 1);                      /* STOP */
}

static void bb_pin(int gpio, int level)
{
    (void)gpio_set_level(gpio, level);
    const gpio_config_t g = {
        .pin_bit_mask = 1ULL << gpio,
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    (void)gpio_config(&g);
    (void)gpio_set_drive_capability(gpio, GPIO_DRIVE_CAP_3);
    (void)gpio_set_level(gpio, level);
}

static void bb_let_go(int gpio)
{
    (void)gpio_set_drive_capability(gpio, GPIO_DRIVE_CAP_DEFAULT);
    const gpio_config_t g = {
        .pin_bit_mask = 1ULL << gpio,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    (void)gpio_config(&g);
}

void fck_drv_tca6408a_blind_gnss_reset(fck_drv_tca6408a_t *t, const fck_drv_pins_t *hw)
{
    uint8_t const p2  = (uint8_t)(1U << FCK_BSP_EXPANDER_GNSS_RST);
    uint8_t const out = (uint8_t)(t->out & (uint8_t)~p2);
    uint8_t const cfg = (uint8_t)(t->cfg & (uint8_t)~p2);

    if (!fck_drv_tca6408a_close(t)) {
        return;
    }
    t->scl = fck_drv_pins_gpio(hw, FCK_BSP_POSITION_I2C_SCL);
    t->sda = fck_drv_pins_gpio(hw, FCK_BSP_POSITION_I2C_SDA);
    bb_pin(t->scl, 1);
    bb_pin(t->sda, 1);
    bb_write(t, FCK_BSP_EXPANDER_REGISTER_OUTPUT, out);   /* P2 low first ... */
    bb_write(t, FCK_BSP_EXPANDER_REGISTER_CONFIG, cfg);   /* ... then an output */
    bb_let_go(t->scl);
    bb_let_go(t->sda);
    esp_rom_delay_us(200U);
    t->out = out;
    t->cfg = cfg;
    (void)fck_drv_tca6408a_open(t, hw);
}
