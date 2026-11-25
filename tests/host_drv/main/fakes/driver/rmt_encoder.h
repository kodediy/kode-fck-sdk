/* SPDX-License-Identifier: GPL-3.0-or-later */
/** @file rmt_encoder.h @brief The RMT types fck_drv_cc1101.c names, for the host; no RMT is played. */
#ifndef FAKE_DRIVER_RMT_ENCODER_H_
#define FAKE_DRIVER_RMT_ENCODER_H_

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct rmt_encoder_t *rmt_encoder_handle_t;
typedef struct {
    int unused;
} rmt_copy_encoder_config_t;

esp_err_t rmt_new_copy_encoder(const rmt_copy_encoder_config_t *config, rmt_encoder_handle_t *ret_encoder);
esp_err_t rmt_del_encoder(rmt_encoder_handle_t encoder);

#endif /* FAKE_DRIVER_RMT_ENCODER_H_ */
