/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file spi_master.h
 * @brief ESP-IDF's SPI master, the part fck_drv_cc1101.c calls, for the host:
 *        fake_cc1101.c plays the chip on the other end.
 */
#ifndef FAKE_DRIVER_SPI_MASTER_H_
#define FAKE_DRIVER_SPI_MASTER_H_

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef int spi_host_device_t;
typedef enum { SPI_DMA_DISABLED = 0 } spi_common_dma_t;

typedef struct {
    int mosi_io_num;
    int miso_io_num;
    int sclk_io_num;
    int quadwp_io_num;
    int quadhd_io_num;
    int max_transfer_sz;
} spi_bus_config_t;

typedef struct {
    int      clock_speed_hz;
    uint8_t  mode;
    int      spics_io_num;
    int      queue_size;
} spi_device_interface_config_t;

typedef struct spi_device_t *spi_device_handle_t;

typedef struct {
    size_t      length;        /* bits */
    const void *tx_buffer;
    void       *rx_buffer;
} spi_transaction_t;

esp_err_t spi_bus_initialize(spi_host_device_t host_id, const spi_bus_config_t *bus_config,
                             spi_common_dma_t dma_chan);
esp_err_t spi_bus_free(spi_host_device_t host_id);
esp_err_t spi_bus_add_device(spi_host_device_t host_id, const spi_device_interface_config_t *dev_config,
                             spi_device_handle_t *handle);
esp_err_t spi_bus_remove_device(spi_device_handle_t handle);
esp_err_t spi_device_polling_transmit(spi_device_handle_t handle, spi_transaction_t *trans_desc);

#endif /* FAKE_DRIVER_SPI_MASTER_H_ */
