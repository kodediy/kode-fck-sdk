/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file fck_drv_uart.c
 * @brief A UART to one of the module's chips. See fck_drv_uart.h.
 */
#include "fck_drv_uart.h"

#include <stdbool.h>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"

/* The owner polls every 20 ms, but it runs below the renderer, which can keep
 * a core for a frame and more. At 115200 this is 350 ms of a stream - a C5
 * scan's burst of results, the GNSS relay - before a byte is lost. */
#define UART_RX_BUF  4096
#define UART_TX_BUF  0          /* writes wait for the FIFO: lines are short */

static bool port_ok(int port)
{
    return port >= 0 && port < (int)UART_NUM_MAX;
}

kode_status_t fck_drv_uart_open(int port, int tx, int rx, uint32_t baud)
{
    if (!port_ok(port)) {
        return KODE_DENIED;
    }
    if (uart_is_driver_installed((uart_port_t)port)) {
        return KODE_EXISTS;
    }
    const uart_config_t c = {
        .baud_rate  = (int)baud,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    if (uart_driver_install((uart_port_t)port, UART_RX_BUF, UART_TX_BUF, 0, NULL, 0) != ESP_OK) {
        return KODE_ERROR;
    }
    if (uart_param_config((uart_port_t)port, &c) != ESP_OK
        || uart_set_pin((uart_port_t)port, tx, rx, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE) != ESP_OK) {
        (void)uart_driver_delete((uart_port_t)port);
        return KODE_ERROR;
    }
    /* The chip's TX idles high; a pull-up keeps an absent one from reading
     * as a stream of zeros. */
    (void)gpio_set_pull_mode((gpio_num_t)rx, GPIO_PULLUP_ONLY);
    return KODE_OK;
}

void fck_drv_uart_close(int port)
{
    if (port_ok(port) && uart_is_driver_installed((uart_port_t)port)) {
        (void)uart_driver_delete((uart_port_t)port);
    }
}

kode_status_t fck_drv_uart_baud(int port, uint32_t baud)
{
    if (!port_ok(port)) {
        return KODE_DENIED;
    }
    return (uart_set_baudrate((uart_port_t)port, baud) == ESP_OK) ? KODE_OK : KODE_ERROR;
}

size_t fck_drv_uart_read(int port, uint8_t *buf, size_t cap)
{
    return fck_drv_uart_read_wait(port, buf, cap, 0U);
}

size_t fck_drv_uart_read_wait(int port, uint8_t *buf, size_t cap, uint32_t ms)
{
    if (!port_ok(port) || !uart_is_driver_installed((uart_port_t)port) || (cap == 0U)) {
        return 0U;
    }
    /* The first byte, waited for; then what else has come, without waiting.
     * Asked for cap bytes at once, the driver waits ms again after the last
     * of them: 20 ms more for every nRF answer, some 11 s of a 37 s image
     * by the count. */
    int const first = uart_read_bytes((uart_port_t)port, buf, 1U, pdMS_TO_TICKS(ms));
    if (first <= 0) {
        return 0U;
    }
    size_t more = 0U;
    if ((cap > 1U) && (uart_get_buffered_data_len((uart_port_t)port, &more) == ESP_OK)
        && (more > 0U)) {
        size_t const take = (more < (cap - 1U)) ? more : (cap - 1U);
        int const    rest = uart_read_bytes((uart_port_t)port, &buf[1], (uint32_t)take, 0);
        if (rest > 0) {
            return 1U + (size_t)rest;
        }
    }
    return 1U;
}

kode_status_t fck_drv_uart_write(int port, const void *data, size_t n)
{
    if (!port_ok(port) || !uart_is_driver_installed((uart_port_t)port)) {
        return KODE_DENIED;
    }
    int w = uart_write_bytes((uart_port_t)port, data, n);
    return (w == (int)n) ? KODE_OK : KODE_IO_ERROR;
}

void fck_drv_uart_flush(int port)
{
    if (port_ok(port) && uart_is_driver_installed((uart_port_t)port)) {
        (void)uart_flush_input((uart_port_t)port);
    }
}
