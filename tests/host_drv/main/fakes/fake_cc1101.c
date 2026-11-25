/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file fake_cc1101.c
 * @brief fake_cc1101.h: the chip, and the SPI bus it hangs from.
 */
#include "fake_cc1101.h"

#include <string.h>

#include "driver/spi_master.h"

#include "fake_idf.h"

fake_cc1101_t g_cc;

#define HDR_READ   0x80U
#define HDR_BURST  0x40U
#define ADDR_MASK  0x3FU

#define STA_PARTNUM   0x30U
#define STA_VERSION   0x31U
#define STA_RSSI      0x34U
#define STA_MARCSTATE 0x35U
#define STA_TXBYTES   0x3AU
#define REG_FIFO      0x3FU

#define STB_SRES  0x30U
#define STB_SRX   0x34U
#define STB_STX   0x35U
#define STB_SIDLE 0x36U
#define STB_SPWD  0x39U
#define STB_SFTX  0x3BU

#define MARC_IDLE 0x01U
#define MARC_RX   0x0DU

void fake_cc1101_reset(int cs_gpio, int miso_gpio)
{
    (void)memset(&g_cc, 0, sizeof(g_cc));
    g_cc.present       = true;
    g_cc.partnum       = 0x00U;
    g_cc.version       = 0x14U;
    g_cc.rssi_floor    = 0x40U;
    g_cc.peak_width_hz = 50000U;
    g_cc.cs_gpio       = cs_gpio;
    g_cc.miso_gpio     = miso_gpio;
    g_cc.state         = MARC_IDLE;
    g_cc.powered_down  = true;
}

bool fake_cc1101_miso(int gpio, int *level)
{
    if (gpio != g_cc.miso_gpio || g_cc.miso_gpio == 0) {
        return false;
    }
    /* A chip pulls its data output low once its crystal runs, which is when
     * its chip select is low. Without one the line reads high or low as the
     * driver's pull leaves it. */
    if (g_cc.present && g_idf.pin[g_cc.cs_gpio].level == 0) {
        *level = 0;
        return true;
    }
    return false;
}

uint32_t fake_cc1101_hz(void)
{
    uint32_t const word = ((uint32_t)g_cc.regs[0x0D] << 16) | ((uint32_t)g_cc.regs[0x0E] << 8) | g_cc.regs[0x0F];
    return (uint32_t)(((uint64_t)word * 26000000ULL) >> 16);
}

unsigned fake_cc1101_strobes(uint8_t s)
{
    unsigned n = 0U;
    for (unsigned i = 0U; i < g_cc.log_n && i < CC_LOG_MAX; i++) {
        n += (g_cc.log[i].kind == CC_EV_STROBE && g_cc.log[i].val == s) ? 1U : 0U;
    }
    return n;
}

unsigned fake_cc1101_writes(uint8_t reg, uint8_t *last)
{
    unsigned n = 0U;
    if (last != NULL) {
        *last = 0U;
    }
    for (unsigned i = 0U; i < g_cc.log_n && i < CC_LOG_MAX; i++) {
        if (g_cc.log[i].kind == CC_EV_WRITE && g_cc.log[i].reg == reg) {
            n++;
            if (last != NULL) {
                *last = g_cc.log[i].val;
            }
        }
    }
    return n;
}

static void note(fake_cc_kind_t kind, uint8_t reg, uint8_t val)
{
    if (g_cc.log_n < CC_LOG_MAX) {
        g_cc.log[g_cc.log_n].kind = kind;
        g_cc.log[g_cc.log_n].reg  = reg;
        g_cc.log[g_cc.log_n].val  = val;
    }
    g_cc.log_n++;
}

static uint8_t status(uint8_t reg)
{
    switch (reg) {
    case STA_PARTNUM:
        return g_cc.partnum;
    case STA_VERSION:
        return g_cc.version;
    case STA_TXBYTES:
        return g_cc.txbytes;
    case STA_MARCSTATE:
        g_cc.marc_reads++;
        if (g_cc.on_marc != NULL) {
            g_cc.on_marc(g_cc.marc_reads);
        }
        return g_cc.state;
    case STA_RSSI: {
        g_cc.rssi_reads++;
        if (g_cc.on_rssi != NULL) {
            g_cc.on_rssi(g_cc.rssi_reads);
        }
        uint32_t const hz = fake_cc1101_hz();
        uint32_t const d  = (hz > g_cc.peak_hz) ? hz - g_cc.peak_hz : g_cc.peak_hz - hz;
        return (g_cc.peak_hz != 0U && d <= g_cc.peak_width_hz) ? g_cc.peak_raw : g_cc.rssi_floor;
    }
    default:
        return 0U;
    }
}

static void strobe(uint8_t s)
{
    note(CC_EV_STROBE, 0U, s);
    switch (s) {
    case STB_SRES:
        (void)memset(g_cc.regs, 0, sizeof(g_cc.regs));
        g_cc.state        = MARC_IDLE;
        g_cc.powered_down = false;
        break;
    case STB_SIDLE:
        g_cc.state = MARC_IDLE;
        break;
    case STB_SRX:
        g_cc.state = g_cc.never_rx ? MARC_IDLE : MARC_RX;
        break;
    case STB_STX:
        g_cc.state = 0x13U;
        break;
    case STB_SFTX:
        g_cc.txbytes = 0U;
        break;
    case STB_SPWD:
        g_cc.powered_down = true;
        g_cc.state        = 0x00U;
        break;
    default:
        break;
    }
}

/* ---- The bus ------------------------------------------------------------------------ */

esp_err_t spi_bus_initialize(spi_host_device_t host_id, const spi_bus_config_t *bus_config,
                             spi_common_dma_t dma_chan)
{
    (void)host_id;
    (void)bus_config;
    (void)dma_chan;
    if (g_cc.bus_rc != ESP_OK) {
        return g_cc.bus_rc;
    }
    g_cc.buses++;
    return ESP_OK;
}

esp_err_t spi_bus_free(spi_host_device_t host_id)
{
    (void)host_id;
    g_cc.buses--;
    return ESP_OK;
}

esp_err_t spi_bus_add_device(spi_host_device_t host_id, const spi_device_interface_config_t *dev_config,
                             spi_device_handle_t *handle)
{
    (void)host_id;
    (void)dev_config;
    if (g_cc.add_rc != ESP_OK) {
        return g_cc.add_rc;
    }
    g_cc.devices++;
    *handle = (spi_device_handle_t)&g_cc.devices;
    return ESP_OK;
}

esp_err_t spi_bus_remove_device(spi_device_handle_t handle)
{
    (void)handle;
    g_cc.devices--;
    return ESP_OK;
}

esp_err_t spi_device_polling_transmit(spi_device_handle_t handle, spi_transaction_t *t)
{
    (void)handle;
    g_cc.transfers++;
    if (g_cc.transmit_rc != ESP_OK) {
        return g_cc.transmit_rc;
    }
    size_t const         n  = t->length / 8U;
    const uint8_t *const tx = t->tx_buffer;
    uint8_t *const       rx = t->rx_buffer;
    if (rx != NULL) {
        (void)memset(rx, 0xFF, n);
    }
    if (!g_cc.present || n == 0U) {
        return ESP_OK;
    }
    uint8_t const hdr  = tx[0];
    uint8_t const addr = hdr & ADDR_MASK;
    if (rx != NULL) {
        rx[0] = (uint8_t)(g_cc.state << 4);     /* the chip's status byte */
    }
    if (n == 1U && (hdr & HDR_READ) == 0U && addr >= STB_SRES && addr <= 0x3DU) {
        strobe(addr);
    } else if ((hdr & HDR_READ) != 0U && n == 2U && rx != NULL) {
        rx[1] = ((hdr & HDR_BURST) != 0U && addr >= STA_PARTNUM) ? status(addr) : g_cc.regs[addr];
    } else if ((hdr & HDR_READ) == 0U && (hdr & HDR_BURST) != 0U && addr == REG_FIFO && n > 1U) {
        g_cc.txbytes = (uint8_t)(n - 1U);     /* the TX FIFO, filled */
    } else if ((hdr & HDR_READ) == 0U && n == 2U && addr < 0x30U) {
        if (g_cc.write_fails && addr == g_cc.write_fails_reg) {
            return ESP_FAIL;     /* the transfer failed: the chip keeps what it had */
        }
        g_cc.regs[addr] = tx[1];
        note(CC_EV_WRITE, addr, tx[1]);
    }
    return ESP_OK;
}
