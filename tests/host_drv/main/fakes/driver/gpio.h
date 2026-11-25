/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file gpio.h
 * @brief ESP-IDF's GPIO calls the drivers use, for the host: each pin keeps
 *        its level, mode and pulls, and what an outside driver holds it at.
 *        fake_idf.c has the functions.
 */
#ifndef FAKE_DRIVER_GPIO_H_
#define FAKE_DRIVER_GPIO_H_

#include <stdint.h>

#include "esp_err.h"

typedef int gpio_num_t;

typedef enum {
    GPIO_MODE_DISABLE      = 0,
    GPIO_MODE_INPUT        = 1,
    GPIO_MODE_OUTPUT       = 2,
    GPIO_MODE_INPUT_OUTPUT = 3,
} gpio_mode_t;
typedef enum { GPIO_PULLUP_DISABLE = 0, GPIO_PULLUP_ENABLE = 1 } gpio_pullup_t;
typedef enum { GPIO_PULLDOWN_DISABLE = 0, GPIO_PULLDOWN_ENABLE = 1 } gpio_pulldown_t;
typedef enum { GPIO_INTR_DISABLE = 0, GPIO_INTR_ANYEDGE = 3 } gpio_int_type_t;
typedef enum {
    GPIO_PULLUP_ONLY,
    GPIO_PULLDOWN_ONLY,
    GPIO_PULLUP_PULLDOWN,
    GPIO_FLOATING,
} gpio_pull_mode_t;
typedef enum {
    GPIO_DRIVE_CAP_DEFAULT = 2,
    GPIO_DRIVE_CAP_3       = 3,
} gpio_drive_cap_t;

typedef struct {
    uint64_t        pin_bit_mask;
    gpio_mode_t     mode;
    gpio_pullup_t   pull_up_en;
    gpio_pulldown_t pull_down_en;
    gpio_int_type_t intr_type;
} gpio_config_t;

typedef void (*gpio_isr_t)(void *arg);

esp_err_t gpio_set_level(gpio_num_t gpio_num, uint32_t level);
int       gpio_get_level(gpio_num_t gpio_num);
esp_err_t gpio_config(const gpio_config_t *config);
esp_err_t gpio_set_pull_mode(gpio_num_t gpio_num, gpio_pull_mode_t pull);
esp_err_t gpio_reset_pin(gpio_num_t gpio_num);
esp_err_t gpio_set_direction(gpio_num_t gpio_num, gpio_mode_t mode);
esp_err_t gpio_set_drive_capability(gpio_num_t gpio_num, gpio_drive_cap_t strength);
esp_err_t gpio_set_intr_type(gpio_num_t gpio_num, gpio_int_type_t intr_type);
esp_err_t gpio_intr_disable(gpio_num_t gpio_num);
esp_err_t gpio_pulldown_en(gpio_num_t gpio_num);
esp_err_t gpio_install_isr_service(int intr_alloc_flags);
esp_err_t gpio_isr_handler_add(gpio_num_t gpio_num, gpio_isr_t isr_handler, void *args);
esp_err_t gpio_isr_handler_remove(gpio_num_t gpio_num);

#endif /* FAKE_DRIVER_GPIO_H_ */
