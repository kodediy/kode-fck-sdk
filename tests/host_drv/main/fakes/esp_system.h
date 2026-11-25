/* SPDX-License-Identifier: GPL-3.0-or-later */
/** @file esp_system.h @brief The restart hook fck_drv_system.c registers, for the host. */
#ifndef FAKE_ESP_SYSTEM_H_
#define FAKE_ESP_SYSTEM_H_

#include "esp_err.h"

typedef void (*shutdown_handler_t)(void);

/* ESP-IDF's linux target has its own: the fake has another name, so the fake is what the driver calls. */
#define esp_register_shutdown_handler fake_esp_register_shutdown_handler

esp_err_t esp_register_shutdown_handler(shutdown_handler_t handle);

#endif /* FAKE_ESP_SYSTEM_H_ */
