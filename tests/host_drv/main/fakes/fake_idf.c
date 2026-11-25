/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file fake_idf.c
 * @brief fake_idf.h: the GPIO, I2C, UART, clock and shutdown hook of the
 *        drivers' suite.
 */
#include "fake_idf.h"

#include <string.h>
#include <time.h>

#include "esp_rom_sys.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

#include "fake_cc1101.h"

idf_fake_t g_idf;

/* The module as J3 lends it: position n is GPIO 10 + n. */
static fck_drv_pins_t s_hw;

void idf_fake_reset(void)
{
    (void)memset(&g_idf, 0, sizeof(g_idf));
    for (int i = 0; i < IDF_PINS; i++) {
        g_idf.pin[i].ext  = IDF_EXT_NONE;
        g_idf.pin[i].mode = GPIO_MODE_DISABLE;
    }
    g_idf.i2c_probe_rc = ESP_ERR_NOT_FOUND;
    g_idf.uart_max_write = -1;
    g_idf.exp_pins       = 0xF7U;
    g_idf.exp_reg[1]     = 0xFFU;   /* OUTPUT, CONFIG: the power-on values */
    g_idf.exp_reg[3]     = 0xFFU;
    for (uint8_t pos = 1U; pos <= FCK_BSP_POSITIONS; pos++) {
        s_hw.gpio[pos - 1U] = (uint8_t)(10U + pos);
    }
    s_hw.i2c_port = 1;
    s_hw.spi_host = 2;
}

const fck_drv_pins_t *idf_fake_hw(void)
{
    return &s_hw;
}

int idf_fake_gpio(uint8_t pos)
{
    return fck_drv_pins_gpio(&s_hw, pos);
}

void idf_fake_expander(uint8_t pins)
{
    g_idf.i2c_device = true;
    g_idf.exp_pins   = pins;
}

uint8_t idf_fake_exp(uint8_t reg)
{
    if (reg == 0U) {
        uint8_t const driven = (uint8_t)(g_idf.exp_reg[1] & (uint8_t)~g_idf.exp_reg[3]);
        uint8_t const mask   = (uint8_t)~g_idf.exp_reg[3];
        return (uint8_t)((g_idf.exp_pins & (uint8_t)~mask) | (driven & mask));
    }
    return g_idf.exp_reg[reg & 3U];
}

bool idf_fake_exp_high(uint8_t mask)
{
    uint8_t const in = idf_fake_exp(0U);
    return (in & mask) == mask;
}

/* ---- The clock ---------------------------------------------------------------------- */

void esp_rom_delay_us(uint32_t us)
{
    g_idf.skipped_us += us;
}

int64_t esp_timer_get_time(void)
{
    struct timespec ts;
    (void)clock_gettime(CLOCK_MONOTONIC, &ts);
    return ((int64_t)ts.tv_sec * 1000000) + ((int64_t)ts.tv_nsec / 1000) + g_idf.skipped_us;
}

/* ---- GPIO --------------------------------------------------------------------------- */

static idf_pin_t *pin_of(int gpio)
{
    return (gpio >= 0 && gpio < IDF_PINS) ? &g_idf.pin[gpio] : NULL;
}

esp_err_t gpio_set_level(gpio_num_t gpio_num, uint32_t level)
{
    idf_pin_t *const p = pin_of(gpio_num);
    if (p == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    p->level = (level != 0U) ? 1 : 0;
    return ESP_OK;
}

int gpio_get_level(gpio_num_t gpio_num)
{
    idf_pin_t *const p = pin_of(gpio_num);
    if (p == NULL) {
        return 0;
    }
    int cc = 0;
    if (fake_cc1101_miso(gpio_num, &cc)) {
        return cc;
    }
    if (p->ext != IDF_EXT_NONE) {
        return p->ext;
    }
    if (p->mode == GPIO_MODE_OUTPUT || p->mode == GPIO_MODE_INPUT_OUTPUT) {
        return p->level;
    }
    if (p->pull_up && !p->pull_down) {
        return 1;
    }
    return 0;
}

esp_err_t gpio_config(const gpio_config_t *config)
{
    for (int i = 0; i < IDF_PINS; i++) {
        if ((config->pin_bit_mask & (1ULL << i)) == 0U) {
            continue;
        }
        idf_pin_t *const p = &g_idf.pin[i];
        bool const       out = config->mode == GPIO_MODE_OUTPUT || config->mode == GPIO_MODE_INPUT_OUTPUT;
        if ((g_idf.claimed & (1ULL << i)) != 0U && !out) {
            g_idf.conflicts++;   /* "conflict found for GPIO" on the board */
        }
        if (out) {
            g_idf.claimed |= 1ULL << i;
        }
        p->mode      = config->mode;
        p->pull_up   = (config->pull_up_en == GPIO_PULLUP_ENABLE);
        p->pull_down = (config->pull_down_en == GPIO_PULLDOWN_ENABLE);
        p->intr      = config->intr_type;
        p->configs++;
    }
    return ESP_OK;
}

esp_err_t gpio_set_pull_mode(gpio_num_t gpio_num, gpio_pull_mode_t pull)
{
    idf_pin_t *const p = pin_of(gpio_num);
    if (p == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    p->pull_up   = (pull == GPIO_PULLUP_ONLY || pull == GPIO_PULLUP_PULLDOWN);
    p->pull_down = (pull == GPIO_PULLDOWN_ONLY || pull == GPIO_PULLUP_PULLDOWN);
    return ESP_OK;
}

esp_err_t gpio_reset_pin(gpio_num_t gpio_num)
{
    idf_pin_t *const p = pin_of(gpio_num);
    if (p == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    g_idf.claimed &= ~(1ULL << gpio_num);
    p->mode    = GPIO_MODE_DISABLE;
    p->pull_up = true;      /* gpio_reset_pin() puts the pull-up on */
    p->pull_down = false;
    p->resets++;
    return ESP_OK;
}

esp_err_t gpio_set_direction(gpio_num_t gpio_num, gpio_mode_t mode)
{
    idf_pin_t *const p = pin_of(gpio_num);
    if (p == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    p->mode = mode;
    return ESP_OK;
}

esp_err_t gpio_set_drive_capability(gpio_num_t gpio_num, gpio_drive_cap_t strength)
{
    idf_pin_t *const p = pin_of(gpio_num);
    if (p == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    p->drive_caps += (strength == GPIO_DRIVE_CAP_3) ? 1U : 0U;
    return ESP_OK;
}

esp_err_t gpio_set_intr_type(gpio_num_t gpio_num, gpio_int_type_t intr_type)
{
    idf_pin_t *const p = pin_of(gpio_num);
    if (p == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    p->intr = intr_type;
    return ESP_OK;
}

esp_err_t gpio_intr_disable(gpio_num_t gpio_num)
{
    if (g_idf.intr_on_disable != NULL) {
        g_idf.intr_on_disable((int)gpio_num);
    }
    return gpio_set_intr_type(gpio_num, GPIO_INTR_DISABLE);
}

esp_err_t gpio_pulldown_en(gpio_num_t gpio_num)
{
    idf_pin_t *const p = pin_of(gpio_num);
    if (p == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    p->pull_down = true;
    return ESP_OK;
}

esp_err_t gpio_install_isr_service(int intr_alloc_flags)
{
    (void)intr_alloc_flags;
    return ESP_OK;
}

esp_err_t gpio_isr_handler_add(gpio_num_t gpio_num, gpio_isr_t isr_handler, void *args)
{
    g_idf.isr      = isr_handler;
    g_idf.isr_arg  = args;
    g_idf.isr_gpio = (int)gpio_num;
    return ESP_OK;
}

esp_err_t gpio_isr_handler_remove(gpio_num_t gpio_num)
{
    if (g_idf.isr_gpio == (int)gpio_num) {
        g_idf.isr = NULL;
    }
    return ESP_OK;
}

bool idf_fake_gpio_edge(int gpio, int level)
{
    idf_pin_t *const p = pin_of(gpio);
    if (p == NULL || g_idf.isr == NULL || g_idf.isr_gpio != gpio || p->intr == (int)GPIO_INTR_DISABLE) {
        return false;
    }
    p->ext = (level != 0) ? 1 : 0;
    g_idf.isr(g_idf.isr_arg);
    return true;
}

/* ---- I2C ---------------------------------------------------------------------------- */

esp_err_t i2c_new_master_bus(const i2c_master_bus_config_t *bus_config, i2c_master_bus_handle_t *ret_bus_handle)
{
    if (g_idf.i2c_new_rc != ESP_OK) {
        return g_idf.i2c_new_rc;
    }
    g_idf.i2c_buses++;
    g_idf.i2c_port = bus_config->i2c_port;
    g_idf.i2c_scl  = bus_config->scl_io_num;
    g_idf.i2c_sda  = bus_config->sda_io_num;
    *ret_bus_handle = (i2c_master_bus_handle_t)&g_idf.i2c_buses;
    return ESP_OK;
}

esp_err_t i2c_del_master_bus(i2c_master_bus_handle_t bus_handle)
{
    (void)bus_handle;
    if (g_idf.i2c_del_rc != ESP_OK) {
        return g_idf.i2c_del_rc;
    }
    g_idf.i2c_buses--;
    return ESP_OK;
}

esp_err_t i2c_master_bus_add_device(i2c_master_bus_handle_t bus_handle, const i2c_device_config_t *dev_config,
                                    i2c_master_dev_handle_t *ret_handle)
{
    (void)bus_handle;
    (void)dev_config;
    if (g_idf.i2c_add_rc != ESP_OK) {
        return g_idf.i2c_add_rc;
    }
    g_idf.i2c_devices++;
    *ret_handle = (i2c_master_dev_handle_t)&g_idf.i2c_devices;
    return ESP_OK;
}

esp_err_t i2c_master_bus_rm_device(i2c_master_dev_handle_t handle)
{
    (void)handle;
    if (g_idf.i2c_rm_rc != ESP_OK) {
        return g_idf.i2c_rm_rc;
    }
    g_idf.i2c_devices--;
    return ESP_OK;
}

esp_err_t i2c_master_probe(i2c_master_bus_handle_t bus_handle, uint16_t address, int xfer_timeout_ms)
{
    (void)bus_handle;
    (void)xfer_timeout_ms;
    g_idf.i2c_probes++;
    if (g_idf.i2c_always_fail > 0U) {
        return ESP_ERR_TIMEOUT;
    }
    return (g_idf.i2c_device && address == 0x21U) ? ESP_OK : g_idf.i2c_probe_rc;
}

static bool transfer_fails(void)
{
    g_idf.i2c_transfers++;
    if (g_idf.i2c_always_fail > 0U) {
        return true;
    }
    if (g_idf.i2c_fail_next > 0U) {
        g_idf.i2c_fail_next--;
        return true;
    }
    return !g_idf.i2c_device;
}

static void note(bool write, uint8_t reg, uint8_t val)
{
    if (g_idf.i2c_log_n < IDF_I2C_LOG) {
        g_idf.i2c_log[g_idf.i2c_log_n].write = write;
        g_idf.i2c_log[g_idf.i2c_log_n].reg   = reg;
        g_idf.i2c_log[g_idf.i2c_log_n].val   = val;
    }
    g_idf.i2c_log_n++;
}

esp_err_t i2c_master_transmit(i2c_master_dev_handle_t i2c_dev, const uint8_t *write_buffer, size_t write_size,
                              int xfer_timeout_ms)
{
    (void)i2c_dev;
    (void)xfer_timeout_ms;
    if (transfer_fails()) {
        return ESP_ERR_TIMEOUT;
    }
    if (write_size == 2U) {
        g_idf.exp_reg[write_buffer[0] & 3U] = write_buffer[1];
        note(true, write_buffer[0], write_buffer[1]);
        if (g_idf.i2c_on_write != NULL) {
            g_idf.i2c_on_write(write_buffer[0], write_buffer[1]);
        }
    }
    return ESP_OK;
}

esp_err_t i2c_master_transmit_receive(i2c_master_dev_handle_t i2c_dev, const uint8_t *write_buffer,
                                      size_t write_size, uint8_t *read_buffer, size_t read_size,
                                      int xfer_timeout_ms)
{
    (void)i2c_dev;
    (void)xfer_timeout_ms;
    if (transfer_fails() || write_size != 1U || read_size != 1U) {
        return ESP_ERR_TIMEOUT;
    }
    read_buffer[0] = idf_fake_exp(write_buffer[0] & 3U);
    note(false, write_buffer[0], read_buffer[0]);
    return ESP_OK;
}

/* ---- UART --------------------------------------------------------------------------- */

#define PORT_OK(p) ((p) >= 0 && (p) < UART_NUM_MAX)

bool uart_is_driver_installed(uart_port_t uart_num)
{
    return PORT_OK(uart_num) && g_idf.uart[uart_num].installed;
}

esp_err_t uart_driver_install(uart_port_t uart_num, int rx_buffer_size, int tx_buffer_size, int queue_size,
                              void *uart_queue, int intr_alloc_flags)
{
    (void)tx_buffer_size;
    (void)queue_size;
    (void)uart_queue;
    (void)intr_alloc_flags;
    if (!PORT_OK(uart_num) || g_idf.uart_install_rc != ESP_OK) {
        return (g_idf.uart_install_rc != ESP_OK) ? g_idf.uart_install_rc : ESP_ERR_INVALID_ARG;
    }
    g_idf.uart[uart_num].installed = true;
    g_idf.uart[uart_num].rx_size   = rx_buffer_size;
    return ESP_OK;
}

esp_err_t uart_driver_delete(uart_port_t uart_num)
{
    if (!PORT_OK(uart_num)) {
        return ESP_ERR_INVALID_ARG;
    }
    g_idf.uart[uart_num].installed = false;
    g_idf.uart_deletes++;
    return ESP_OK;
}

esp_err_t uart_param_config(uart_port_t uart_num, const uart_config_t *uart_config)
{
    if (g_idf.uart_config_rc != ESP_OK) {
        return g_idf.uart_config_rc;
    }
    g_idf.uart[uart_num].baud = (uint32_t)uart_config->baud_rate;
    return ESP_OK;
}

esp_err_t uart_set_pin(uart_port_t uart_num, int tx_io_num, int rx_io_num, int rts_io_num, int cts_io_num)
{
    (void)rts_io_num;
    (void)cts_io_num;
    if (g_idf.uart_pin_rc != ESP_OK) {
        return g_idf.uart_pin_rc;
    }
    g_idf.uart[uart_num].tx_pin = tx_io_num;
    g_idf.uart[uart_num].rx_pin = rx_io_num;
    return ESP_OK;
}

esp_err_t uart_set_baudrate(uart_port_t uart_num, uint32_t baudrate)
{
    if (g_idf.uart_baud_rc != ESP_OK) {
        return g_idf.uart_baud_rc;
    }
    g_idf.uart[uart_num].baud = baudrate;
    return ESP_OK;
}

int uart_read_bytes(uart_port_t uart_num, void *buf, uint32_t length, uint32_t ticks_to_wait)
{
    if (g_idf.uart_on_read != NULL) {
        g_idf.uart_on_read(uart_num);
    }
    if (g_idf.uart_virtual_wait) {
        uint32_t left = ticks_to_wait * portTICK_PERIOD_MS;
        while (g_idf.uart[uart_num].rx_n == g_idf.uart[uart_num].rx_at && left > 0U) {
            g_idf.skipped_us += 1000;
            left--;
            if (g_idf.uart_on_read != NULL) {
                g_idf.uart_on_read(uart_num);
            }
        }
    }
    size_t const have = g_idf.uart[uart_num].rx_n - g_idf.uart[uart_num].rx_at;
    size_t const take = (have < length) ? have : length;
    (void)memcpy(buf, &g_idf.uart[uart_num].rx[g_idf.uart[uart_num].rx_at], take);
    g_idf.uart[uart_num].rx_at += take;
    return (int)take;
}

esp_err_t uart_get_buffered_data_len(uart_port_t uart_num, size_t *size)
{
    *size = g_idf.uart[uart_num].rx_n - g_idf.uart[uart_num].rx_at;
    return ESP_OK;
}

int uart_write_bytes(uart_port_t uart_num, const void *src, size_t size)
{
    if (g_idf.uart_on_write != NULL) {
        g_idf.uart_on_write(uart_num, (const uint8_t *)src, size);
        return (int)size;
    }
    size_t take = size;
    if (g_idf.uart_max_write >= 0 && take > (size_t)g_idf.uart_max_write) {
        take = (size_t)g_idf.uart_max_write;
    }
    size_t const room = IDF_UART_TX - g_idf.uart[uart_num].tx_n;
    take              = (take < room) ? take : room;
    (void)memcpy(&g_idf.uart[uart_num].tx[g_idf.uart[uart_num].tx_n], src, take);
    g_idf.uart[uart_num].tx_n += take;
    return (int)take;
}

esp_err_t uart_flush_input(uart_port_t uart_num)
{
    if (g_idf.uart_on_read != NULL) {
        g_idf.uart_on_read(uart_num);
    }
    g_idf.uart[uart_num].rx_at = g_idf.uart[uart_num].rx_n;
    g_idf.uart[uart_num].flushes++;
    return ESP_OK;
}

void idf_fake_uart_feed(int port, const void *data, size_t n)
{
    /* What was read is made room of: a chip that talks for seconds on end
     * would otherwise fill the buffer with what was long since taken. */
    if (g_idf.uart[port].rx_at > 0U) {
        size_t const left = g_idf.uart[port].rx_n - g_idf.uart[port].rx_at;
        (void)memmove(g_idf.uart[port].rx, &g_idf.uart[port].rx[g_idf.uart[port].rx_at], left);
        g_idf.uart[port].rx_n  = left;
        g_idf.uart[port].rx_at = 0U;
    }
    size_t const room = IDF_UART_RX - g_idf.uart[port].rx_n;
    n                 = (n < room) ? n : room;
    (void)memcpy(&g_idf.uart[port].rx[g_idf.uart[port].rx_n], data, n);
    g_idf.uart[port].rx_n += n;
}

/* ---- The shutdown hook ---------------------------------------------------------------- */

esp_err_t esp_register_shutdown_handler(shutdown_handler_t handle)
{
    g_idf.shutdowns++;
    g_idf.shutdown_fn = handle;
    return g_idf.shutdown_rc;
}

/* ---- Things the cc1101 driver links to and the suite never plays ------------------------- */

static int s_rmt_chan, s_rmt_enc;   /* handles: only their addresses matter */

esp_err_t rmt_new_tx_channel(const rmt_tx_channel_config_t *config, rmt_channel_handle_t *ret_chan)
{
    (void)config;
    if (!g_idf.rmt.on) {
        return ESP_ERR_NOT_SUPPORTED;     /* nothing is played unless a case says */
    }
    *ret_chan = (rmt_channel_handle_t)&s_rmt_chan;
    return ESP_OK;
}

esp_err_t rmt_new_copy_encoder(const rmt_copy_encoder_config_t *config, rmt_encoder_handle_t *ret_encoder)
{
    (void)config;
    if (!g_idf.rmt.on) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    *ret_encoder = (rmt_encoder_handle_t)&s_rmt_enc;
    return ESP_OK;
}

esp_err_t rmt_del_encoder(rmt_encoder_handle_t encoder)
{
    (void)encoder;
    return ESP_OK;
}

esp_err_t rmt_del_channel(rmt_channel_handle_t channel)
{
    (void)channel;
    g_idf.rmt.deletes++;
    return ESP_OK;
}

esp_err_t rmt_enable(rmt_channel_handle_t channel)
{
    (void)channel;
    return g_idf.rmt.on ? ESP_OK : ESP_ERR_NOT_SUPPORTED;
}

esp_err_t rmt_disable(rmt_channel_handle_t channel)
{
    (void)channel;
    g_idf.rmt.disables++;
    g_idf.rmt.until_us = 0;     /* stopped: what was left of it is not played */
    return ESP_OK;
}

esp_err_t rmt_transmit(rmt_channel_handle_t channel, rmt_encoder_handle_t encoder, const void *payload,
                       size_t payload_bytes, const rmt_transmit_config_t *config)
{
    (void)channel;
    (void)encoder;
    (void)payload;
    (void)payload_bytes;
    (void)config;
    if (!g_idf.rmt.on) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    g_idf.rmt.transmits++;
    g_idf.rmt.until_us = esp_timer_get_time() + ((int64_t)g_idf.rmt.play_ms * 1000);
    return ESP_OK;
}

esp_err_t rmt_tx_wait_all_done(rmt_channel_handle_t channel, int timeout_ms)
{
    (void)channel;
    if (!g_idf.rmt.on) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    g_idf.rmt.waits++;
    if (g_idf.rmt.on_wait != NULL) {
        g_idf.rmt.on_wait(g_idf.rmt.waits);
    }
    int64_t const left = g_idf.rmt.until_us - esp_timer_get_time();
    if (left <= 0) {
        return ESP_OK;
    }
    int64_t const wait = (left < ((int64_t)timeout_ms * 1000)) ? left : ((int64_t)timeout_ms * 1000);
    g_idf.skipped_us += wait;
    return (wait == left) ? ESP_OK : ESP_ERR_TIMEOUT;
}
