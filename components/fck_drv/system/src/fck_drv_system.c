/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file fck_drv_system.c
 * @brief The system's hooks. See fck_drv_system.h.
 */
#include "fck_drv_system.h"

#include "esp_system.h"

kode_status_t fck_drv_system_on_restart(fck_drv_system_fn fn)
{
    esp_err_t const err = esp_register_shutdown_handler((shutdown_handler_t)fn);
    if (err == ESP_OK || err == ESP_ERR_INVALID_STATE) {
        return KODE_OK;    /* INVALID_STATE: this one is registered already */
    }
    return KODE_NO_RESOURCES;
}
