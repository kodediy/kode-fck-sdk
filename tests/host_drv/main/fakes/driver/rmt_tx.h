/* SPDX-License-Identifier: GPL-3.0-or-later */
/** @file rmt_tx.h @brief The RMT TX types fck_drv_cc1101.c names, for the host; a channel is never made. */
#ifndef FAKE_DRIVER_RMT_TX_H_
#define FAKE_DRIVER_RMT_TX_H_

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "driver/gpio.h"
#include "driver/rmt_encoder.h"

typedef struct rmt_channel_t *rmt_channel_handle_t;
typedef enum { RMT_CLK_SRC_DEFAULT = 0 } rmt_clock_source_t;

typedef union {
    struct {
        uint32_t duration0 : 15;
        uint32_t level0 : 1;
        uint32_t duration1 : 15;
        uint32_t level1 : 1;
    };
    uint32_t val;
} rmt_symbol_word_t;

typedef struct {
    gpio_num_t         gpio_num;
    rmt_clock_source_t clk_src;
    uint32_t           resolution_hz;
    size_t             mem_block_symbols;
    size_t             trans_queue_depth;
} rmt_tx_channel_config_t;

typedef struct {
    int loop_count;
} rmt_transmit_config_t;

esp_err_t rmt_new_tx_channel(const rmt_tx_channel_config_t *config, rmt_channel_handle_t *ret_chan);
esp_err_t rmt_del_channel(rmt_channel_handle_t channel);
esp_err_t rmt_enable(rmt_channel_handle_t channel);
esp_err_t rmt_disable(rmt_channel_handle_t channel);
esp_err_t rmt_transmit(rmt_channel_handle_t channel, rmt_encoder_handle_t encoder,
                       const void *payload, size_t payload_bytes, const rmt_transmit_config_t *config);
esp_err_t rmt_tx_wait_all_done(rmt_channel_handle_t channel, int timeout_ms);

#endif /* FAKE_DRIVER_RMT_TX_H_ */
