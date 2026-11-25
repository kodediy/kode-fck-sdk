/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file fck_drv_cc1101.c
 * @brief The module's CC1101, asleep and listening. See fck_drv_cc1101.h.
 *
 * The datasheet sections are SWRS061I's.
 */
#include "fck_drv_cc1101.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/rmt_encoder.h"
#include "driver/rmt_tx.h"
#include "driver/spi_master.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "fck_drv_ook.h"
#include "fck_drv_subghz_modem.h"
#include "fck_drv_subghz_scan.h"

/* A cut: the app is going home, to sleep or to a restart, and nothing may be
 * left on the air; or fck_cancel() ended what was running. Set from any task, at any
 * time (the restart hook's is the owner's own task or the asker's), read by the sends and
 * the listenings between their steps. One aligned byte, written whole. */
static volatile bool s_abort;

#define CC_SPI_HZ     (4 * 1000 * 1000)   /* well under the 6.5 MHz of a burst */
#define CC_READY_US   10000               /* the crystal starts in well under 1 ms */
#define CC_XOSC_HZ    26000000U           /* X601 */

#define HDR_READ        0x80U
#define HDR_READ_BURST  0xC0U
#define HDR_WRITE_BURST 0x40U

#define REG_IOCFG2    0x00U
#define REG_IOCFG1    0x01U
#define REG_IOCFG0    0x02U
#define REG_FIFOTHR   0x03U
#define REG_SYNC1     0x04U
#define REG_SYNC0     0x05U
#define REG_PKTLEN    0x06U
#define REG_PKTCTRL1  0x07U
#define REG_PKTCTRL0  0x08U
#define REG_ADDR      0x09U
#define REG_CHANNR    0x0AU
#define REG_FSCTRL1   0x0BU
#define REG_FSCTRL0   0x0CU
#define REG_FREQ2     0x0DU
#define REG_FREQ1     0x0EU
#define REG_FREQ0     0x0FU
#define REG_MDMCFG4   0x10U
#define REG_MDMCFG3   0x11U
#define REG_MDMCFG2   0x12U
#define REG_MDMCFG1   0x13U
#define REG_MDMCFG0   0x14U
#define REG_DEVIATN   0x15U
#define REG_MCSM2     0x16U
#define REG_MCSM1     0x17U
#define REG_MCSM0     0x18U
#define REG_FOCCFG    0x19U
#define REG_BSCFG     0x1AU
#define REG_AGCCTRL2  0x1BU
#define REG_AGCCTRL1  0x1CU
#define REG_AGCCTRL0  0x1DU
#define REG_FREND1    0x21U
#define REG_FREND0    0x22U
#define REG_FSCAL3    0x23U
#define REG_FSCAL2    0x24U
#define REG_FSCAL1    0x25U
#define REG_FSCAL0    0x26U
#define REG_TEST2     0x2CU
#define REG_TEST1     0x2DU
#define REG_TEST0     0x2EU

#define STA_PARTNUM     0x30U
#define STA_VERSION     0x31U
#define STA_FREQEST     0x32U
#define STA_RSSI        0x34U
#define STA_MARCSTATE   0x35U
#define STA_TXBYTES     0x3AU
#define STA_RXBYTES     0x3BU
#define REG_PATABLE     0x3EU
#define REG_FIFO        0x3FU

#define STB_SRES        0x30U
#define STB_SRX         0x34U
#define STB_STX         0x35U
#define STB_SIDLE       0x36U
#define STB_SPWD        0x39U
#define STB_SFRX        0x3AU
#define STB_SFTX        0x3BU

#define MARC_IDLE       0x01U
#define MARC_RX         0x0DU
#define MARC_RX_OVERFLOW 0x11U
#define MARC_TX          0x13U
#define MARC_TX_UNDERFLOW 0x16U

/* RSSI counts half-dB, two's complement, 74 dB over dBm [S17.3]. */
#define RSSI_OFFSET_HALF 148

#define SW0_MASK        ((uint8_t)FCK_BSP_EXPANDER_SW0_MASK)
#define SW0_REST        (FCK_BSP_LEG_SW0_REST != 0U)
/* SW0 put back at rest is given what the owner gives the expander after the
 * GNSS receiver lets go of the bus (fck_drv_tca6408a_mask_patient()). */
#define SW0_BACK_MS     600U

#define IDLE_POINT_US   1000U    /* to IDLE between two points of a sweep */
#define IDLE_US         2000U
#define RX_US           3000U    /* to RX: 721 us of calibration, and settling */
#define MARC_POLL_US    50U

#define REMOTES_MAX     48U
#define SWEEP_YIELD     8U       /* a wide sweep yields every 8 points (~14 ms) */
#define FINE_YIELD      4U

#define AGC_SETTLE_MS   3U       /* the first readings after SRX are the AGC's */
#define FLOOR_READS     32U
#define FLOOR_GAP_US    500U
#define POLL_US         200U
#define YIELD_US        10000
#define TAIL_US         3000U

typedef struct {
    uint8_t reg;
    uint8_t val;
} regval_t;

typedef struct {
    spi_device_handle_t dev;
    int                 host;      /* the SPI host kode_sys_exp_ctrl() gave   */
    int                 cs;
    int                 miso;
    int                 gdo0;
    const fck_drv_pins_t     *hw;
    fck_drv_tca6408a_t          *tca;
    int                 leg;       /* the leg set, -1 for none yet       */
    bool                sw0_back;  /* SW0 may be off rest: it goes back */
} cc_t;

/* ---- The bus --------------------------------------------------------------------- */

static bool cc_shift(const cc_t *c, const uint8_t *tx, uint8_t *rx, size_t n)
{
    spi_transaction_t t = { .length = n * 8U, .tx_buffer = tx, .rx_buffer = rx };
    return spi_device_polling_transmit(c->dev, &t) == ESP_OK;
}

/* The chip select low, then the chip's data output falling: its crystal runs. */
static bool cc_begin(const cc_t *c)
{
    int64_t const until = esp_timer_get_time() + CC_READY_US;
    (void)gpio_set_level(c->cs, 0);
    while (esp_timer_get_time() < until) {
        if (gpio_get_level(c->miso) == 0) {
            return true;
        }
        esp_rom_delay_us(5U);
    }
    (void)gpio_set_level(c->cs, 1);
    return false;
}

static void cc_end(const cc_t *c)
{
    (void)gpio_set_level(c->cs, 1);
}

static bool cc_strobe(const cc_t *c, uint8_t s)
{
    uint8_t rx = 0U;
    if (!cc_begin(c)) {
        return false;
    }
    bool const ok = cc_shift(c, &s, &rx, 1U);
    cc_end(c);
    return ok;
}

/* One register read, the header's bits included in @p hdr_reg. */
static bool cc_read(const cc_t *c, uint8_t hdr_reg, uint8_t *val)
{
    uint8_t tx[2] = { hdr_reg, 0U };
    uint8_t rx[2] = { 0xFFU, 0xFFU };
    if (!cc_begin(c)) {
        return false;
    }
    bool const ok = cc_shift(c, tx, rx, 2U);
    cc_end(c);
    *val = rx[1];
    return ok;
}

static uint8_t cc_status(const cc_t *c, uint8_t reg)
{
    uint8_t v = 0xFFU;
    return cc_read(c, (uint8_t)(reg | HDR_READ_BURST), &v) ? v : 0xFFU;
}

static bool cc_write(const cc_t *c, uint8_t reg, uint8_t val)
{
    uint8_t tx[2] = { reg, val };
    uint8_t rx[2] = { 0U, 0U };
    if (!cc_begin(c)) {
        return false;
    }
    bool const ok = cc_shift(c, tx, rx, 2U);
    cc_end(c);
    return ok;
}

static bool cc_write_all(const cc_t *c, const regval_t *r, size_t n)
{
    bool ok = true;
    for (size_t i = 0U; i < n; i++) {
        ok = cc_write(c, r[i].reg, r[i].val) && ok;
    }
    return ok;
}

/* The datasheet's manual reset after power-up (section 19.1.2, Figure 27). */
static bool cc_reset(const cc_t *c)
{
    uint8_t const sres = STB_SRES;
    uint8_t       rx   = 0U;
    (void)gpio_set_level(c->cs, 1);
    esp_rom_delay_us(10U);
    (void)gpio_set_level(c->cs, 0);
    esp_rom_delay_us(10U);
    (void)gpio_set_level(c->cs, 1);
    esp_rom_delay_us(60U);
    if (!cc_begin(c)) {
        return false;
    }
    (void)cc_shift(c, &sres, &rx, 1U);
    int64_t const until = esp_timer_get_time() + CC_READY_US;
    bool          done  = false;
    while (!done && esp_timer_get_time() < until) {
        done = (gpio_get_level(c->miso) == 0);
        esp_rom_delay_us(5U);
    }
    cc_end(c);
    return done;
}

/* ---- Open and closed ------------------------------------------------------------- */

/* Held high by the pull-up of the lines' rest state. bus_open()'s
 * gpio_config() claimed the pin as an output, and ESP-IDF keeps that claim
 * until gpio_reset_pin() revokes it: without that, this gpio_config() and
 * the next open's each warned "conflict found for GPIO[19]", 915 times in a
 * 10-minute soak. gpio_reset_pin() puts the pull-up on before it
 * lets the output go, so the line never falls. */
static void cs_rest(const cc_t *c)
{
    (void)gpio_reset_pin((gpio_num_t)c->cs);
    const gpio_config_t rest = {
        .pin_bit_mask = 1ULL << c->cs,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    (void)gpio_reset_pin((gpio_num_t)c->cs);
    (void)gpio_config(&rest);
}

static kode_status_t bus_open(cc_t *c, const fck_drv_pins_t *hw)
{
    /* Chip select high before the bus exists, so the chip never sees a clock
     * it could take for a command. */
    (void)gpio_set_level(c->cs, 1);
    const gpio_config_t cs = {
        .pin_bit_mask = 1ULL << c->cs,
        .mode         = GPIO_MODE_OUTPUT,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    (void)gpio_config(&cs);
    (void)gpio_set_level(c->cs, 1);

    const spi_bus_config_t bus = {
        .mosi_io_num     = fck_drv_pins_gpio(hw, FCK_BSP_POSITION_CC_MOSI),
        .miso_io_num     = c->miso,
        .sclk_io_num     = fck_drv_pins_gpio(hw, FCK_BSP_POSITION_CC_CLK),
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = 64,
    };
    const spi_device_interface_config_t dcfg = {
        .clock_speed_hz = CC_SPI_HZ,
        .mode           = 0,
        .spics_io_num   = -1,
        .queue_size     = 1,
    };
    c->host = hw->spi_host;
    if (spi_bus_initialize((spi_host_device_t)c->host, &bus, SPI_DMA_DISABLED) != ESP_OK) {
        cs_rest(c);
        return KODE_ERROR;
    }
    /* A header with nothing on it reads as a chip that never becomes ready. */
    (void)gpio_set_pull_mode((gpio_num_t)c->miso, GPIO_PULLDOWN_ONLY);
    if (spi_bus_add_device((spi_host_device_t)c->host, &dcfg, &c->dev) != ESP_OK) {
        (void)spi_bus_free((spi_host_device_t)c->host);
        (void)gpio_set_pull_mode((gpio_num_t)c->miso, GPIO_FLOATING);
        cs_rest(c);
        return KODE_ERROR;
    }
    return KODE_OK;
}

static void bus_close(const cc_t *c)
{
    (void)spi_bus_remove_device(c->dev);
    (void)spi_bus_free((spi_host_device_t)c->host);
    /* The lines' rest state: no pull on the data line. */
    (void)gpio_set_pull_mode((gpio_num_t)c->miso, GPIO_FLOATING);
    cs_rest(c);
}

static kode_status_t identify(const cc_t *c, uint8_t *part, uint8_t *ver)
{
    if (!cc_reset(c)) {
        return KODE_TIMEOUT;
    }
    *part = cc_status(c, STA_PARTNUM);
    *ver  = cc_status(c, STA_VERSION);
    return (*part == 0x00U && *ver != 0x00U && *ver != 0xFFU) ? KODE_OK : KODE_IO_ERROR;
}

/* IDLE first: SPWD is only taken from IDLE. The chip powers down when its
 * chip select goes high, and stays down while it is held high. */
static void power_down(const cc_t *c)
{
    (void)cc_strobe(c, STB_SIDLE);
    (void)cc_strobe(c, STB_SPWD);
}

kode_status_t fck_drv_cc1101_sleep(const fck_drv_pins_t *hw, uint8_t *partnum, uint8_t *version)
{
    cc_t c = {
        .cs   = fck_drv_pins_gpio(hw, FCK_BSP_POSITION_CC_CS),
        .miso = fck_drv_pins_gpio(hw, FCK_BSP_POSITION_CC_MISO),
    };
    uint8_t       part = 0xFFU;
    uint8_t       ver  = 0xFFU;
    kode_status_t st   = bus_open(&c, hw);
    if (st == KODE_OK) {
        st = identify(&c, &part, &ver);
        if (st != KODE_TIMEOUT) {
            power_down(&c);
        }
        bus_close(&c);
    }
    if (partnum != NULL) {
        *partnum = part;
    }
    if (version != NULL) {
        *version = ver;
    }
    return st;
}

/* ---- The radio ----------------------------------------------------------------------
 *
 * The base loaded after the reset: the synthesizer's
 * calibration (FSCAL3/1/0), MCSM1 without CCA, MCSM0 calibrating on every
 * IDLE to RX. The level receiver and OOK rewrite only their own registers on
 * top. IOCFG2 is not here: it is SW1. */
static const regval_t BASE[] = {
    { REG_IOCFG0, 0x06U },   { REG_IOCFG1, 0x2EU },   { REG_FIFOTHR, 0x47U },
    { REG_SYNC1, 0xD3U },    { REG_SYNC0, 0x91U },    { REG_PKTLEN, 0x08U },
    { REG_PKTCTRL1, 0x04U }, { REG_PKTCTRL0, 0x44U }, { REG_ADDR, 0x00U },
    { REG_CHANNR, 0x00U },   { REG_FSCTRL1, 0x06U },  { REG_FSCTRL0, 0x00U },
    { REG_MDMCFG2, 0x13U },  { REG_MDMCFG1, 0x22U },  { REG_MDMCFG0, 0xF8U },
    { REG_MCSM2, 0x07U },    { REG_MCSM1, 0x03U },    { REG_MCSM0, 0x18U },
    { REG_FOCCFG, 0x16U },   { REG_BSCFG, 0x6CU },    { REG_AGCCTRL1, 0x40U },
    { REG_AGCCTRL0, 0x91U }, { REG_FREND1, 0x56U },   { REG_FREND0, 0x10U },
    { REG_FSCAL3, 0xE9U },   { REG_FSCAL2, 0x2AU },   { REG_FSCAL1, 0x00U },
    { REG_FSCAL0, 0x1FU },   { REG_TEST2, 0x81U },    { REG_TEST1, 0x35U },
    { REG_TEST0, 0x09U },    { REG_MDMCFG4, 0xCAU },  { REG_MDMCFG3, 0x83U },
    { REG_DEVIATN, 0x35U },  { REG_AGCCTRL2, 0x43U },
};

/* OOK in asynchronous serial mode: GDO0 is what it
 * demodulates (IOCFG0 0x0D), infinite length (PKTCTRL0 0x32), ASK/OOK with
 * no sync word (MDMCFG2 0x30), a 650 kHz filter at 3.79 kBaud - a quantum of
 * 32.9 us - (MDMCFG4 0x17, last). */
static const regval_t OOK[] = {
    { REG_IOCFG0, 0x0DU },   { REG_FIFOTHR, 0x07U },  { REG_PKTCTRL0, 0x32U },
    { REG_FSCTRL1, 0x06U },  { REG_MDMCFG3, 0x32U },  { REG_MDMCFG2, 0x30U },
    { REG_MDMCFG1, 0x00U },  { REG_MDMCFG0, 0x00U },  { REG_MCSM0, 0x18U },
    { REG_FOCCFG, 0x18U },   { REG_AGCCTRL2, 0x07U }, { REG_AGCCTRL1, 0x00U },
    { REG_AGCCTRL0, 0x91U }, { REG_FREND1, 0xB6U },   { REG_FREND0, 0x11U },
    { REG_TEST2, 0x88U },    { REG_TEST1, 0x31U },    { REG_MDMCFG4, 0x17U },
};

/* How a level is read with each filter: the wait from RX to the first
 * reading, then the highest of so many readings so far apart. The narrow
 * filter's wait is five RSSI updates of 8 / 58 kHz; its 8 readings span a
 * whole bit of a remote at a te of 350 us. */
typedef struct {
    uint32_t settle_us;
    uint32_t reads;
    uint32_t gap_us;
} reading_t;

static const reading_t READING[] = {
    [FCK_DRV_CC1101_WIDE]   = { 250U, 3U, 120U },
    [FCK_DRV_CC1101_NARROW] = { 689U, 8U, 150U },
};

typedef struct {
    uint32_t lo;
    uint32_t hi;
} band_t;

static const band_t BANDS[] = {
    { 300000000U, 348000000U },
    { 387000000U, 464000000U },
    { 779000000U, 928000000U },
};

static const band_t *band_of(uint32_t hz)
{
    for (size_t i = 0U; i < sizeof(BANDS) / sizeof(BANDS[0]); i++) {
        if (hz >= BANDS[i].lo && hz <= BANDS[i].hi) {
            return &BANDS[i];
        }
    }
    return NULL;
}

bool fck_drv_cc1101_tunes(uint32_t hz)
{
    return band_of(hz) != NULL;
}

static bool filter_ok(fck_drv_cc1101_filter_t f)
{
    return f == FCK_DRV_CC1101_WIDE || f == FCK_DRV_CC1101_NARROW;
}

static int64_t now_us(void)
{
    return esp_timer_get_time();
}

/* MARCSTATE is updated by the chip: a single read can be corrupt, so it is
 * read until two agree [S10.3]. */
static uint8_t marc(const cc_t *c)
{
    uint8_t m = cc_status(c, STA_MARCSTATE);
    for (unsigned i = 0U; i < 4U; i++) {
        uint8_t const m2 = cc_status(c, STA_MARCSTATE);
        if (m2 == m) {
            break;
        }
        m = m2;
    }
    return (m == 0xFFU) ? 0xFFU : (uint8_t)(m & 0x1FU);
}

static bool wait_marc(const cc_t *c, uint8_t state, uint32_t us)
{
    int64_t const t0 = now_us();
    do {
        if (marc(c) == state) {
            return true;
        }
        esp_rom_delay_us(MARC_POLL_US);
    } while (now_us() - t0 < (int64_t)us);
    return marc(c) == state;
}

static void idle(const cc_t *c)
{
    (void)cc_strobe(c, STB_SIDLE);
    (void)wait_marc(c, MARC_IDLE, IDLE_US);
}

/* FREQ2/1/0, and the VCO core that has to follow the carrier [TEST0 and
 * FSCAL2]: the low core below 322.88 MHz, from 387 to 430.5 and from 779 to
 * 861; the high one elsewhere. With the high core everywhere the PLL did not
 * lock in the lower part of each band (no packet below about 309, 410 and
 * 822 MHz). The chip is in IDLE, and calibrates on the next SRX. */
static bool tune(const cc_t *c, uint32_t hz)
{
    uint32_t const word =
        (uint32_t)((((uint64_t)hz << 16) + CC_XOSC_HZ / 2U) / CC_XOSC_HZ) & 0xFFFFFFU;
    bool const low = hz < 322880000U || (hz >= 387000000U && hz < 430500000U)
                     || (hz >= 779000000U && hz < 861000000U);
    regval_t const r[] = {
        { REG_FREQ2, (uint8_t)(word >> 16) },
        { REG_FREQ1, (uint8_t)(word >> 8) },
        { REG_FREQ0, (uint8_t)word },
        { REG_TEST0, low ? 0x0BU : 0x09U },
        { REG_FSCAL2, low ? 0x0AU : 0x2AU },
    };
    return cc_write_all(c, r, sizeof(r) / sizeof(r[0]));
}

/* The level, in half-dB; FCK_DRV_SUBGHZ_SCAN_NO_RECEIVE when the read failed, which every
 * reader takes for a bad read. */
static int16_t rssi_half(const cc_t *c)
{
    uint8_t raw = 0U;
    if (!cc_read(c, (uint8_t)(STA_RSSI | HDR_READ_BURST), &raw)) {
        return FCK_DRV_SUBGHZ_SCAN_NO_RECEIVE;
    }
    return (int16_t)((int8_t)raw - RSSI_OFFSET_HALF);
}

/* ---- The legs ---------------------------------------------------------------------- */

/* Whether @p hz's leg can be taken: SW0 off its rest needs the expander. */
static bool leg_open(const fck_drv_tca6408a_t *tca, uint32_t hz)
{
    return fck_drv_subghz_scan_leg_open(hz, tca != NULL);
}

/* The leg @p hz is on, set only when it is not the one set already: SW1 is
 * written and read back, SW0 written and read back from the pins. */
static kode_status_t take_leg(cc_t *c, uint32_t hz)
{
    fck_bsp_leg_t const leg = fck_drv_subghz_scan_leg(hz);
    bool const          sw0 = FCK_BSP_LEG_SW0(leg);
    if (!leg_open(c->tca, hz)) {
        return KODE_DENIED;
    }
    if ((int)leg == c->leg) {
        return KODE_OK;
    }
    c->leg = -1;
    uint8_t const want = FCK_BSP_LEG_SW1(leg) ? FCK_BSP_CC1101_IOCFG2_HIGH : FCK_BSP_CC1101_IOCFG2_LOW;
    uint8_t       got  = 0U;
    (void)cc_strobe(c, STB_SIDLE);
    if (!cc_write(c, REG_IOCFG2, want) || !cc_read(c, (uint8_t)(REG_IOCFG2 | HDR_READ), &got)
        || got != want) {
        return KODE_IO_ERROR;
    }
    if (c->tca != NULL) {
        uint8_t in  = 0U;
        c->sw0_back = true;
        if (fck_drv_tca6408a_mask(c->tca, SW0_MASK, true, sw0) != KODE_OK
            || fck_drv_tca6408a_read(c->tca, FCK_BSP_EXPANDER_REGISTER_INPUT, &in) != KODE_OK
            || (uint8_t)(in & SW0_MASK) != (sw0 ? SW0_MASK : 0U)) {
            return KODE_IO_ERROR;
        }
        c->sw0_back = !fck_drv_subghz_scan_leg_open(hz, false);
    }
    c->leg = (int)leg;
    return KODE_OK;
}

/* ---- One call's start and end ---------------------------------------------------- */

static kode_status_t listen_open(cc_t *c, const fck_drv_pins_t *hw, fck_drv_tca6408a_t *tca)
{
    *c = (cc_t){
        .cs   = fck_drv_pins_gpio(hw, FCK_BSP_POSITION_CC_CS),
        .miso = fck_drv_pins_gpio(hw, FCK_BSP_POSITION_CC_MISO),
        .gdo0 = fck_drv_pins_gpio(hw, FCK_BSP_POSITION_CC_GDO0),
        .hw   = hw,
        .tca  = tca,
        .leg  = -1,
    };
    kode_status_t st = bus_open(c, hw);
    if (st != KODE_OK) {
        return st;
    }
    uint8_t part = 0xFFU;
    uint8_t ver  = 0xFFU;
    st           = identify(c, &part, &ver);
    if (st == KODE_OK && !cc_write_all(c, BASE, sizeof(BASE) / sizeof(BASE[0]))) {
        st = KODE_IO_ERROR;
    }
    if (st != KODE_OK) {
        if (st != KODE_TIMEOUT) {
            power_down(c);
        }
        bus_close(c);
    }
    return st;
}

static void gdo0_rest(const cc_t *c)
{
    const gpio_config_t in = {
        .pin_bit_mask = 1ULL << c->gdo0,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    (void)gpio_config(&in);
}

/* Back as fck_drv_cc1101_sleep() leaves it - reset, so IOCFG2 and the rest are
 * the chip's own again, and powered down - and SW0 back at rest. */
static kode_status_t listen_close(cc_t *c, kode_status_t st)
{
    gdo0_rest(c);
    if (cc_reset(c)) {
        power_down(c);
    }
    bus_close(c);
    if (c->sw0_back
        && fck_drv_tca6408a_mask_patient(c->tca, c->hw, SW0_MASK, true, SW0_REST, SW0_BACK_MS) != KODE_OK
        && st == KODE_OK) {
        st = KODE_IO_ERROR;
    }
    return st;
}

/* ---- Levels -------------------------------------------------------------------------- */

/* The receiver of level alone, with filter @p f:
 * asynchronous serial with no sync word, because with sync detection on the
 * RSSI freezes on the first word [S17.3]; the IF at half the filter plus two
 * steps of 25.4 kHz, so the IF's DC notch stays out of the channel; no
 * frequency compensation; full gain. The chip is in IDLE. */
static void level_profile(const cc_t *c, fck_drv_cc1101_filter_t f)
{
    uint32_t const e    = (f == FCK_DRV_CC1101_NARROW) ? 3U : 0U;
    uint32_t const m    = (f == FCK_DRV_CC1101_NARROW) ? 3U : 1U;
    uint32_t const bw   = CC_XOSC_HZ / ((8U * (4U + m)) << e);   /* [S13] */
    bool const     wide = bw >= 325000U;
    uint32_t       fif  = bw / 2U / 25391U + 2U;
    fif                 = (fif < 0x06U) ? 0x06U : (fif > 0x0EU) ? 0x0EU : fif;
    regval_t const r[] = {
        { REG_IOCFG0, 0x2EU },
        { REG_PKTCTRL0, 0x32U },
        { REG_MDMCFG4, (uint8_t)((e << 6) | (m << 4) | 0x0AU) },
        { REG_MDMCFG3, 0x83U },
        { REG_MDMCFG2, 0x00U },
        { REG_FSCTRL1, (uint8_t)fif },
        { REG_MCSM0, 0x18U },
        { REG_FOCCFG, 0x18U },
        { REG_AGCCTRL2, (bw > 110000U) ? 0x07U : 0x03U },
        { REG_AGCCTRL1, 0x40U },
        { REG_AGCCTRL0, 0x91U },
        { REG_FREND1, wide ? 0xB6U : 0x56U },
        { REG_TEST2, wide ? 0x88U : 0x81U },
        { REG_TEST1, wide ? 0x31U : 0x35U },
    };
    (void)cc_write_all(c, r, sizeof(r) / sizeof(r[0]));
}

/* One point, its leg already taken: tuned in IDLE, SRX (which calibrates),
 * and the highest of its readings. */
static int16_t point(const cc_t *c, uint32_t hz, fck_drv_cc1101_filter_t f)
{
    reading_t const *const r = &READING[f];
    (void)cc_strobe(c, STB_SIDLE);
    (void)wait_marc(c, MARC_IDLE, IDLE_POINT_US);
    (void)tune(c, hz);
    (void)cc_strobe(c, STB_SRX);
    if (!wait_marc(c, MARC_RX, RX_US)) {
        return FCK_DRV_SUBGHZ_SCAN_NO_RECEIVE;
    }
    esp_rom_delay_us(r->settle_us);
    int16_t best = FCK_DRV_SUBGHZ_SCAN_NO_RECEIVE;
    for (uint32_t k = 0U; k < r->reads; k++) {
        if (k > 0U) {
            esp_rom_delay_us(r->gap_us);
        }
        int16_t const v = rssi_half(c);
        if (v >= FCK_DRV_OOK_BOGUS_HALF && v > best) {
            best = v;
        }
    }
    return best;
}

kode_status_t fck_drv_cc1101_rssi(const fck_drv_pins_t *hw, fck_drv_tca6408a_t *tca, uint32_t hz,
                              fck_drv_cc1101_filter_t f, int16_t *half)
{
    if (hw == NULL || half == NULL || !filter_ok(f) || !fck_drv_cc1101_tunes(hz)
        || !leg_open(tca, hz)) {
        return KODE_DENIED;
    }
    cc_t          c;
    kode_status_t st = listen_open(&c, hw, tca);
    if (st != KODE_OK) {
        return st;
    }
    int16_t v = FCK_DRV_SUBGHZ_SCAN_NO_RECEIVE;
    idle(&c);
    level_profile(&c, f);
    st = take_leg(&c, hz);
    if (st == KODE_OK) {
        v  = point(&c, hz, f);
        st = (v != FCK_DRV_SUBGHZ_SCAN_NO_RECEIVE) ? KODE_OK : KODE_IO_ERROR;
    }
    st = listen_close(&c, st);
    if (st == KODE_OK) {
        *half = v;
    }
    return st;
}

kode_status_t fck_drv_cc1101_spectrum(const fck_drv_pins_t *hw, fck_drv_tca6408a_t *tca, uint32_t first_hz,
                                  uint32_t step_hz, size_t count, fck_drv_cc1101_filter_t f,
                                  int16_t *half)
{
    if (hw == NULL || half == NULL || count == 0U || !filter_ok(f)
        || (uint64_t)first_hz + (uint64_t)(count - 1U) * step_hz > UINT32_MAX) {
        return KODE_DENIED;
    }
    cc_t          c;
    kode_status_t st = listen_open(&c, hw, tca);
    if (st != KODE_OK) {
        return st;
    }
    size_t asked = 0U;
    size_t live  = 0U;
    idle(&c);
    level_profile(&c, f);
    size_t i = 0U;
    for (; (i < count) && !s_abort; i++) {
        uint32_t const hz = first_hz + (uint32_t)i * step_hz;
        int16_t        v  = FCK_DRV_SUBGHZ_SCAN_NO_RECEIVE;
        if (fck_drv_cc1101_tunes(hz) && leg_open(tca, hz)) {
            asked++;
            if (take_leg(&c, hz) == KODE_OK) {
                v = point(&c, hz, f);
            }
        }
        live += (v != FCK_DRV_SUBGHZ_SCAN_NO_RECEIVE) ? 1U : 0U;
        half[i] = v;
        if ((i % SWEEP_YIELD) == SWEEP_YIELD - 1U) {
            vTaskDelay(1);
        }
    }
    for (size_t j = i; j < count; j++) {
        half[j] = FCK_DRV_SUBGHZ_SCAN_NO_RECEIVE;     /* cut short (fck_drv_cc1101_abort()) */
    }
    idle(&c);
    st = (asked == 0U) ? KODE_DENIED : (live == 0U) ? KODE_IO_ERROR : KODE_OK;
    return listen_close(&c, st);
}

/* ---- Finding a remote -------------------------------------------------------------- */

/* The narrow sweep around @p centre, within its band, twice, each point's
 * highest kept: where it peaks (fck_drv_subghz_scan_peak()). */
static bool fine(cc_t *c, uint32_t centre, uint32_t *hz, int16_t *half)
{
    band_t const *const b = band_of(centre);
    if (b == NULL) {
        return false;
    }
    uint32_t const f0 =
        (centre >= b->lo + FCK_DRV_SUBGHZ_SCAN_FINE_SPAN_HZ) ? centre - FCK_DRV_SUBGHZ_SCAN_FINE_SPAN_HZ : b->lo;
    uint32_t const f1 =
        (centre + FCK_DRV_SUBGHZ_SCAN_FINE_SPAN_HZ <= b->hi) ? centre + FCK_DRV_SUBGHZ_SCAN_FINE_SPAN_HZ : b->hi;
    size_t n = (size_t)((f1 - f0) / FCK_DRV_SUBGHZ_SCAN_FINE_STEP_HZ) + 1U;
    n        = (n > FCK_DRV_SUBGHZ_SCAN_FINE_POINTS) ? FCK_DRV_SUBGHZ_SCAN_FINE_POINTS : n;

    idle(c);
    level_profile(c, FCK_DRV_CC1101_NARROW);
    if (take_leg(c, centre) != KODE_OK) {
        idle(c);
        return false;
    }
    int16_t v[FCK_DRV_SUBGHZ_SCAN_FINE_POINTS];
    for (size_t k = 0U; k < n; k++) {
        v[k] = FCK_DRV_SUBGHZ_SCAN_NO_RECEIVE;
    }
    for (uint32_t lap = 0U; (lap < FCK_DRV_SUBGHZ_SCAN_FINE_LAPS) && !s_abort; lap++) {
        for (size_t k = 0U; (k < n) && !s_abort; k++) {
            int16_t const x =
                point(c, f0 + (uint32_t)k * FCK_DRV_SUBGHZ_SCAN_FINE_STEP_HZ, FCK_DRV_CC1101_NARROW);
            v[k] = (x > v[k]) ? x : v[k];
            if ((k % FINE_YIELD) == FINE_YIELD - 1U) {
                vTaskDelay(1);
            }
        }
    }
    idle(c);
    return fck_drv_subghz_scan_peak(v, n, f0, FCK_DRV_SUBGHZ_SCAN_FINE_STEP_HZ, hz, half);
}

kode_status_t fck_drv_cc1101_find(const fck_drv_pins_t *hw, fck_drv_tca6408a_t *tca, uint32_t ms,
                              fck_drv_cc1101_hit_t *hit)
{
    size_t                nf = 0U;
    const uint32_t *const hz = fck_drv_subghz_scan_remotes(&nf);
    if (hw == NULL || hit == NULL || nf > REMOTES_MAX) {
        return KODE_DENIED;
    }
    cc_t          c;
    kode_status_t st = listen_open(&c, hw, tca);
    if (st != KODE_OK) {
        return st;
    }
    int16_t noise[REMOTES_MAX];
    int16_t lv[REMOTES_MAX];
    for (size_t i = 0U; i < nf; i++) {
        noise[i] = FCK_DRV_SUBGHZ_SCAN_NO_RECEIVE;
    }
    fck_drv_subghz_scan_lock_t  lock   = { 0 };
    fck_drv_cc1101_hit_t out    = { .found = false };
    uint32_t         sweeps = 0U;
    int64_t const    end    = now_us() + (int64_t)ms * 1000;

    while ((now_us() < end) && !s_abort) {
        size_t live = 0U;
        idle(&c);
        level_profile(&c, FCK_DRV_CC1101_WIDE);
        for (size_t i = 0U; i < nf; i++) {
            lv[i] = FCK_DRV_SUBGHZ_SCAN_NO_RECEIVE;
            if (leg_open(tca, hz[i]) && take_leg(&c, hz[i]) == KODE_OK) {
                lv[i] = point(&c, hz[i], FCK_DRV_CC1101_WIDE);
            }
            live += (lv[i] != FCK_DRV_SUBGHZ_SCAN_NO_RECEIVE) ? 1U : 0U;
            if ((i % SWEEP_YIELD) == SWEEP_YIELD - 1U) {
                vTaskDelay(1);
            }
        }
        idle(&c);
        if (live == 0U) {
            st = KODE_IO_ERROR;
            break;
        }

        bool const learning = sweeps < FCK_DRV_SUBGHZ_SCAN_LEARN_SWEEPS;
        int16_t    excess   = 0;
        size_t     best     = nf;
        for (size_t i = 0U; i < nf; i++) {
            int16_t const rise = fck_drv_subghz_scan_rise(&noise[i], lv[i]);
            if (!learning && rise > excess) {
                excess = rise;
                best   = i;
            }
        }
        sweeps++;
        if (best == nf || excess < FCK_DRV_SUBGHZ_SCAN_RISE_HALF) {
            continue;
        }

        /* What rose most is narrowed at once, while the remote is still
         * pressed. */
        uint32_t   det      = hz[best];
        int16_t    det_half = lv[best];
        bool const narrowed = fine(&c, hz[best], &det, &det_half);
        if (fck_drv_subghz_scan_lock(&lock, det, narrowed, excess, now_us())) {
            out = (fck_drv_cc1101_hit_t){
                .found      = true,
                .hz         = fck_drv_subghz_scan_snap(lock.hz),
                .half       = det_half,
                .noise_half = (int16_t)(lv[best] - excess),
            };
            break;
        }
    }
    st = listen_close(&c, st);
    if (st == KODE_OK) {
        *hit = out;
    }
    return st;
}

/* ---- A remote's frames ------------------------------------------------------------
 *
 * GPIO's ISR stamps each edge of GDO0 with esp_timer and the level it finds
 * (fck_drv_ook_edge()). Not the RMT: it cuts a frame at the first gap over
 * 32.7 ms, and a sync can be 37 ms. */

static struct {
    uint32_t         *buf;
    uint32_t          max;
    volatile uint32_t n;
    int64_t           t0;
    int               gdo0;
} s_cap;

static void IRAM_ATTR on_edge(void *arg)
{
    (void)arg;
    uint32_t const i = s_cap.n;
    if (i >= s_cap.max) {
        return;
    }
    uint32_t const t = (uint32_t)(esp_timer_get_time() - s_cap.t0);
    s_cap.buf[i]     = (t << 1) | (uint32_t)(gpio_get_level(s_cap.gdo0) & 1);
    s_cap.n          = i + 1U;
}

/* GPIO's ISR service is shared: installed once, and someone else's taken. */
static bool isr_service(void)
{
    static bool s_ok;
    if (!s_ok) {
        esp_err_t const err = gpio_install_isr_service(0);
        s_ok                = (err == ESP_OK || err == ESP_ERR_INVALID_STATE);
    }
    return s_ok;
}

/* OOK on @p hz through its leg, the AGC settled. */
static kode_status_t ook_tune(cc_t *c, uint32_t hz)
{
    idle(c);
    (void)cc_write_all(c, OOK, sizeof(OOK) / sizeof(OOK[0]));
    kode_status_t const st = take_leg(c, hz);
    if (st != KODE_OK) {
        return st;
    }
    (void)tune(c, hz);
    gdo0_rest(c);
    (void)cc_strobe(c, STB_SRX);
    if (!wait_marc(c, MARC_RX, RX_US)) {
        return KODE_IO_ERROR;
    }
    vTaskDelay(pdMS_TO_TICKS(AGC_SETTLE_MS));
    return KODE_OK;
}

/* Until the power rises FCK_DRV_OOK_TRIGGER_HALF over the noise, following the
 * noise meanwhile; false at @p end. @p at is the reading that rose. */
static bool wait_rise(const cc_t *c, fck_drv_ook_floor_t *fl, int64_t end, int16_t *at)
{
    int64_t yield = now_us();
    for (;;) {
        int16_t const v = rssi_half(c);
        if (v >= FCK_DRV_OOK_BOGUS_HALF && v >= fck_drv_ook_floor_half(fl) + FCK_DRV_OOK_TRIGGER_HALF) {
            *at = v;
            return true;
        }
        fck_drv_ook_floor_step(fl, v);
        int64_t const t = now_us();
        if (t >= end || s_abort) {
            return false;
        }
        if (t - yield >= YIELD_US) {
            vTaskDelay(1);
            yield = now_us();
        } else {
            esp_rom_delay_us(POLL_US);
        }
    }
}

/* One capture, from now: GDO0's edges until the power has stood under
 * FCK_DRV_OOK_HOT_HALF for FCK_DRV_CC1101_QUIET_MS, then runs. @p got is 0 for a
 * click. Everything the ISR reads is set before gpio_isr_handler_add(),
 * which also enables the pin's interrupt. @p peak starts at the reading that
 * rose and is the loudest one after it. */
static kode_status_t capture(const cc_t *c, const fck_drv_ook_floor_t *fl, uint32_t *edges,
                             size_t *got, int16_t *peak)
{
    s_cap.buf  = edges;
    s_cap.max  = FCK_DRV_CC1101_EDGES;
    s_cap.n    = 0U;
    s_cap.gdo0 = c->gdo0;
    (void)gpio_set_intr_type(c->gdo0, GPIO_INTR_ANYEDGE);
    s_cap.t0          = now_us();
    bool const level0 = gpio_get_level(c->gdo0) != 0;
    if (gpio_isr_handler_add(c->gdo0, on_edge, NULL) != ESP_OK) {
        (void)gpio_set_intr_type(c->gdo0, GPIO_INTR_DISABLE);
        return KODE_ERROR;
    }
    int64_t last_hot = s_cap.t0;
    bool    full     = false;
    for (;;) {
        vTaskDelay(1);
        if (s_abort) {
            break;     /* a cut (fck_cancel(), HOME): what was seen so far is all there is */
        }
        int64_t const t = now_us();
        int16_t const v = rssi_half(c);
        if (v >= fck_drv_ook_floor_half(fl) + FCK_DRV_OOK_HOT_HALF) {
            last_hot = t;
        }
        if (v > *peak) {
            *peak = v;
        }
        if (s_cap.n >= s_cap.max) {
            full = true;
            break;
        }
        if (t - last_hot >= (int64_t)FCK_DRV_CC1101_QUIET_MS * 1000
            || t - s_cap.t0 >= (int64_t)FCK_DRV_CC1101_BURST_MS * 1000) {
            break;
        }
    }
    (void)gpio_intr_disable(c->gdo0);
    esp_rom_delay_us(5U);   /* an ISR running on the other core ends its write */
    (void)gpio_isr_handler_remove(c->gdo0);
    (void)gpio_set_intr_type(c->gdo0, GPIO_INTR_DISABLE);

    size_t const k   = s_cap.n;
    uint32_t     end = (uint32_t)(last_hot - s_cap.t0) + TAIL_US;
    /* A full buffer has no room for the run after its last edge, whether the loop above saw it
     * fill or an edge filled it after its last look. */
    if ((full || k >= FCK_DRV_CC1101_EDGES) && k > 0U && (edges[k - 1U] >> 1) < end) {
        end = edges[k - 1U] >> 1;
    }
    size_t runs = fck_drv_ook_runs(edges, k, level0, end);
    runs        = fck_drv_ook_deglitch((int32_t *)edges, runs, FCK_DRV_OOK_GLITCH_US);
    *got        = (runs >= FCK_DRV_OOK_RUNS_MIN) ? runs : 0U;
    return KODE_OK;
}

/* The edges of one capture, for one call: internal RAM, which the ISR can
 * write with the cache off. */
static uint32_t *edges_take(void)
{
    return heap_caps_malloc(FCK_DRV_CC1101_EDGES * sizeof(uint32_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

kode_status_t fck_drv_cc1101_record(const fck_drv_pins_t *hw, fck_drv_tca6408a_t *tca, uint32_t hz, uint32_t ms,
                                int32_t *runs, size_t cap, size_t *n)
{
    if (hw == NULL || runs == NULL || cap == 0U || n == NULL || !fck_drv_cc1101_tunes(hz)
        || !leg_open(tca, hz)) {
        return KODE_DENIED;
    }
    if (!isr_service()) {
        return KODE_ERROR;
    }
    uint32_t *const edges = edges_take();
    if (edges == NULL) {
        return KODE_NO_RESOURCES;
    }
    cc_t          c;
    kode_status_t st = listen_open(&c, hw, tca);
    if (st != KODE_OK) {
        heap_caps_free(edges);
        return st;
    }
    size_t got = 0U;
    st         = ook_tune(&c, hz);
    if (st == KODE_OK) {
        /* The noise on arrival: a low percentile of 32 readings over 16 ms,
         * not the median - a remote already pressed puts half of them in its
         * pulses, and its gaps leave the low ones on the noise. */
        int16_t         v[FLOOR_READS];
        fck_drv_ook_floor_t fl;
        for (size_t i = 0U; i < FLOOR_READS; i++) {
            v[i] = rssi_half(&c);
            esp_rom_delay_us(FLOOR_GAP_US);
        }
        fck_drv_ook_floor_start(&fl, v, FLOOR_READS);
        int64_t const end  = now_us() + (int64_t)ms * 1000;
        int16_t       peak = 0;
        while (got == 0U && st == KODE_OK && wait_rise(&c, &fl, end, &peak)) {
            st = capture(&c, &fl, edges, &got, &peak);
        }
    }
    st = listen_close(&c, st);
    if (st == KODE_OK) {
        size_t const k = (got < cap) ? got : cap;
        memcpy(runs, edges, k * sizeof(int32_t));
        *n = k;
    }
    heap_caps_free(edges);
    return st;
}

/* ---- Sending a signal -------------------------------------------------------------
 *
 * The inverse of record(): the runs an app kept (signed microseconds,
 * positive the carrier on, negative off) are played onto GDO0 by the RMT at
 * one tick a microsecond, and the CC1101 keys its carrier to them in
 * asynchronous serial mode (in TX the chip reads GDO0, which the RMT drives).
 * PATABLE[1] is the band's power - 0xC0, +10 dBm at 433, and the 0xCB the
 * 868 band is capped at - and PATABLE[0] is off. Both values, and the leg
 * each band leaves by (fck_bsp.h: 433 the middle one, 868 the bottom one),
 * are the module's. Nothing reaches
 * here for a frequency outside the two transmit bands: the facade and
 * fck_drv_subghz_scan_may_transmit() keep it there. */

#define RMT_HZ        1000000U   /* one RMT tick is a microsecond */
#define RMT_MAX_TICKS 32767U     /* 15 bits of one symbol half */
#define RMT_MEM_SYMS  48U        /* one channel's memory block on the P4, streamed into */
#define PA_OFF        0x00U
#define PA_433        0xC0U      /* the datasheet's +10 dBm row at 433 */
#define PA_868        0xCBU      /* under the +10 dBm row: the 868 band's cap */
#define MDMCFG4_TX    0x1AU      /* 30 kBaud, so the chip samples GDO0 every
                                  * ~4 us in TX rather than every 33 */
#define TX_WAIT_US    3000U      /* STX to TX: calibration and settling */
#define TX_SLACK_MS   1000U      /* past the signal's own length, for one transmission */
#define TX_STEP_MS    10U        /* leaving is looked for this often while a repeat plays */

static uint8_t pa_of(uint32_t hz)
{
    return (hz <= FCK_BSP_TX_433_MAX_HZ) ? PA_433 : PA_868;
}

/* A run's length in microseconds. Taken in unsigned arithmetic, so even
 * INT32_MIN has one (and is then refused as too long). */
static uint32_t run_us(int32_t r)
{
    return (r > 0) ? (uint32_t)r : (0U - (uint32_t)r);
}

/* PATABLE index 0 (off) and 1 (on) in one burst: the chip's index pointer
 * resets on access, so a single write to REG_PATABLE would set index 0 only. */
static bool cc_write_patable(const cc_t *c, uint8_t on)
{
    const uint8_t tx[3] = { (uint8_t)(REG_PATABLE | HDR_WRITE_BURST), PA_OFF, on };
    uint8_t rx[3] = { 0U, 0U, 0U };
    if (!cc_begin(c)) {
        return false;
    }
    bool const ok = cc_shift(c, tx, rx, 3U);
    cc_end(c);
    return ok;
}

/* The RMT symbol halves the runs need: one per run, plus one more for every
 * whole RMT_MAX_TICKS a run runs over the first; and in @p ms how long they
 * play, rounded up. 0 when a run is 0 or longer than
 * FCK_DRV_CC1101_SEND_RUN_US_MAX. */
static size_t tx_halves(const int32_t *runs, size_t n, uint32_t *ms)
{
    size_t   h  = 0U;
    uint64_t us = 0U;
    for (size_t i = 0U; i < n; i++) {
        uint32_t const a = run_us(runs[i]);
        if (a == 0U || a > FCK_DRV_CC1101_SEND_RUN_US_MAX) {
            return 0U;
        }
        h += (a + RMT_MAX_TICKS - 1U) / RMT_MAX_TICKS;
        us += a;
    }
    *ms = (uint32_t)((us + 999U) / 1000U);
    return h;
}

/* The runs as RMT symbols: two (duration, level) halves a symbol, a run over
 * RMT_MAX_TICKS split across halves at its level. A dangling half0 closes
 * with duration1 0, which ends the burst. @p cap is tx_halves()/2 + 1; the
 * bound is kept all the same. Returns the symbols, 0 on overflow. */
static size_t tx_fill(const int32_t *runs, size_t n, rmt_symbol_word_t *sym, size_t cap)
{
    size_t   s     = 0U;
    uint32_t d0    = 0U;
    uint32_t l0    = 0U;
    bool     have0 = false;
    for (size_t i = 0U; i < n; i++) {
        int32_t const  r   = runs[i];
        uint32_t const lvl = (r > 0) ? 1U : 0U;
        uint32_t       a   = run_us(r);
        while (a > 0U) {
            uint32_t const d = (a > RMT_MAX_TICKS) ? RMT_MAX_TICKS : a;
            if (!have0) {
                d0    = d;
                l0    = lvl;
                have0 = true;
            } else {
                if (s >= cap) {
                    return 0U;
                }
                sym[s].val = d0 | (l0 << 15) | (d << 16) | (lvl << 31);
                s++;
                have0 = false;
            }
            a -= d;
        }
    }
    if (have0) {
        if (s >= cap) {
            return 0U;
        }
        sym[s].val = d0 | (l0 << 15);
        s++;
    }
    return s;
}

void fck_drv_cc1101_abort(void)
{
    s_abort = true;
}

void fck_drv_cc1101_abort_clear(void)
{
    s_abort = false;
}

bool fck_drv_cc1101_aborted(void)
{
    return s_abort;
}

/* OOK on @p hz through its leg, PATABLE set. GDO0 stays the serial data pin
 * (IOCFG0 0x0D): the chip drives it until STX, and in TX reads it. */
static kode_status_t tx_tune(cc_t *c, uint32_t hz)
{
    idle(c);
    /* A chip that did not take every setting is not keyed: it would send as it was left. */
    if (!cc_write_all(c, OOK, sizeof(OOK) / sizeof(OOK[0])) || !cc_write(c, REG_MDMCFG4, MDMCFG4_TX)) {
        return KODE_IO_ERROR;
    }
    kode_status_t const st = take_leg(c, hz);
    if (st != KODE_OK) {
        return st;
    }
    if (!tune(c, hz)) {
        return KODE_IO_ERROR;
    }
    return cc_write_patable(c, pa_of(hz)) ? KODE_OK : KODE_IO_ERROR;
}

/* The wait for one repeat to play, in steps of TX_STEP_MS so that leaving
 * (s_abort) is seen within one: KODE_OK when the RMT is done, KODE_IO_ERROR
 * when it failed, was told to stop, or did not finish in @p total_ms. */
static kode_status_t tx_wait(rmt_channel_handle_t chan, uint32_t total_ms)
{
    uint32_t waited = 0U;
    while (waited < total_ms) {
        uint32_t const step = ((total_ms - waited) < TX_STEP_MS) ? (total_ms - waited) : TX_STEP_MS;
        esp_err_t const e = rmt_tx_wait_all_done(chan, (int)step);
        if (e == ESP_OK) {
            return KODE_OK;
        }
        if ((e != ESP_ERR_TIMEOUT) || s_abort) {
            return KODE_IO_ERROR;
        }
        waited += step;
    }
    return KODE_IO_ERROR;
}

/* STX, the signal @p repeats times, SIDLE, with GDO0 never driven from both
 * ends, in this order:
 * the P4's pad takes its pull-down while the chip still drives the pin; STX
 * turns the pin into the chip's data input, held low (carrier off) by the
 * pull-down; only then does the RMT take the pad, idle low; and it lets go
 * before SIDLE gives the pin back to the chip. listen_close() puts the pad
 * back as an input without pulls. GDO0 rests low between repeats, so the
 * carrier is simply off there. Each repeat is given the signal's @p ms and
 * TX_SLACK_MS more: a capture of several seconds plays whole. */
static kode_status_t tx_play(const cc_t *c, const rmt_symbol_word_t *sym, size_t ns, uint32_t ms,
                             uint8_t repeats)
{
    uint32_t const wait_ms = ms + TX_SLACK_MS;
    rmt_channel_handle_t chan = NULL;
    rmt_encoder_handle_t enc  = NULL;
    rmt_tx_channel_config_t const ch = {
        .gpio_num          = c->gdo0,
        .clk_src           = RMT_CLK_SRC_DEFAULT,
        .resolution_hz     = RMT_HZ,
        .mem_block_symbols = RMT_MEM_SYMS,
        .trans_queue_depth = 1U,
    };
    /* cppcheck-suppress misra-c2012-9.2 ; the IDF copy-encoder config is an empty struct */
    rmt_copy_encoder_config_t const ec = {};
    if (s_abort) {
        return KODE_DENIED;     /* leaving: nothing is keyed */
    }
    (void)gpio_pulldown_en(c->gdo0);
    if (!cc_strobe(c, STB_STX) || !wait_marc(c, MARC_TX, TX_WAIT_US)) {
        (void)cc_strobe(c, STB_SIDLE);
        return KODE_IO_ERROR;
    }
    if (rmt_new_tx_channel(&ch, &chan) != ESP_OK) {
        (void)cc_strobe(c, STB_SIDLE);
        return KODE_ERROR;
    }
    if (rmt_new_copy_encoder(&ec, &enc) != ESP_OK || rmt_enable(chan) != ESP_OK) {
        if (enc != NULL) {
            (void)rmt_del_encoder(enc);
        }
        (void)rmt_del_channel(chan);
        (void)cc_strobe(c, STB_SIDLE);
        return KODE_ERROR;
    }
    kode_status_t                st = KODE_OK;
    rmt_transmit_config_t const tc = { .loop_count = 0 };
    for (uint8_t r = 0U; (r < repeats) && (st == KODE_OK); r++) {
        if (s_abort) {
            st = KODE_IO_ERROR;     /* between two repeats */
        } else if (rmt_transmit(chan, enc, sym, ns * sizeof(sym[0]), &tc) != ESP_OK) {
            st = KODE_IO_ERROR;
        } else {
            st = tx_wait(chan, wait_ms);
        }
    }
    /* The carrier ends with SIDLE, whatever the RMT is still clocking out:
     * before the channel is stopped, so that a send cut short is off at once. */
    if (st != KODE_OK) {
        (void)cc_strobe(c, STB_SIDLE);
    }
    (void)rmt_disable(chan);
    (void)rmt_del_encoder(enc);
    (void)rmt_del_channel(chan);
    (void)cc_strobe(c, STB_SIDLE);
    return st;
}

kode_status_t fck_drv_cc1101_send(const fck_drv_pins_t *hw, fck_drv_tca6408a_t *tca, uint32_t hz,
                                  const int32_t *runs, size_t n, uint8_t repeats)
{
    if (s_abort || hw == NULL || runs == NULL || n == 0U || n > FCK_DRV_CC1101_SEND_RUNS_MAX || repeats == 0U
        || repeats > FCK_DRV_CC1101_SEND_REPEATS_MAX || !fck_drv_subghz_scan_may_transmit(hz)
        || !leg_open(tca, hz)
        || !fck_drv_subghz_scan_send_fits(fck_drv_subghz_scan_on_air_us(runs, n, repeats))) {
        return KODE_DENIED;
    }
    uint32_t     ms     = 0U;
    size_t const halves = tx_halves(runs, n, &ms);
    if (halves == 0U) {
        return KODE_DENIED;
    }
    size_t const cap = (halves / 2U) + 1U;
    rmt_symbol_word_t *const sym =
        heap_caps_malloc(cap * sizeof(rmt_symbol_word_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (sym == NULL) {
        return KODE_NO_RESOURCES;
    }
    size_t const  ns = tx_fill(runs, n, sym, cap);
    cc_t          c;
    kode_status_t st = (ns == 0U) ? KODE_DENIED : listen_open(&c, hw, tca);
    if (ns != 0U && st == KODE_OK) {
        st = tx_tune(&c, hz);
        if (st == KODE_OK) {
            st = tx_play(&c, sym, ns, ms, repeats);
        }
        st = listen_close(&c, st);
    }
    heap_caps_free(sym);
    return st;
}

/* ---- Listening, read after read -------------------------------------------------- */

/* The slot of l->noise a listen on one frequency keeps its noise in. */
#define FIXED FCK_DRV_SUBGHZ_SCAN_HOPS

/* What fck_drv_cc1101_listen() listens on now, for fck_drv_cc1101_listen_now()
 * from another task: one aligned 32-bit word, written whole. */
static volatile uint32_t s_listen_hz;

void fck_drv_cc1101_listen_start(fck_drv_cc1101_listen_t *l, uint32_t hz)
{
    if (l != NULL) {
        *l = (fck_drv_cc1101_listen_t){ .hold = fck_drv_subghz_scan_hold_start(hz) };
    }
}

uint32_t fck_drv_cc1101_listen_now(void)
{
    return s_listen_hz;
}

/* The loudest reading over @p ms; FCK_DRV_SUBGHZ_SCAN_NO_RECEIVE when none was
 * good. */
static int16_t loudest(const cc_t *c, uint32_t ms)
{
    int16_t       best  = FCK_DRV_SUBGHZ_SCAN_NO_RECEIVE;
    int64_t const end   = now_us() + (int64_t)ms * 1000;
    int64_t       yield = now_us();
    for (int64_t t = yield; (t < end) && !s_abort; t = now_us()) {
        int16_t const v = rssi_half(c);
        if (v >= FCK_DRV_OOK_BOGUS_HALF && v > best) {
            best = v;
        }
        if (t - yield >= YIELD_US) {
            vTaskDelay(1);
            yield = now_us();
        } else {
            esp_rom_delay_us(POLL_US);
        }
    }
    return best;
}

/* On @p hz until @p end, for a capture worth keeping (fck_drv_subghz_scan_worth()):
 * a click is not one, and it listens on. Slot @p k's noise is learned on
 * arrival the first time, as fck_drv_cc1101_record() learns it, and followed
 * from there; it is kept for the next time. @p got is 0 when nothing was. */
static kode_status_t hear(cc_t *c, fck_drv_cc1101_listen_t *l, size_t k, uint32_t hz, int64_t end,
                          uint32_t *edges, size_t *got, int16_t *peak, int16_t *noise, bool *decoded)
{
    *got             = 0U;
    *decoded         = false;
    s_listen_hz      = hz;
    kode_status_t st = ook_tune(c, hz);
    if (st != KODE_OK) {
        return st;
    }
    fck_drv_ook_floor_t noise_floor = l->noise[k];
    if (!l->known[k]) {
        int16_t v[FLOOR_READS];
        for (size_t i = 0U; i < FLOOR_READS; i++) {
            v[i] = rssi_half(c);
            esp_rom_delay_us(FLOOR_GAP_US);
        }
        fck_drv_ook_floor_start(&noise_floor, v, FLOOR_READS);
    }
    while (*got == 0U && st == KODE_OK && wait_rise(c, &noise_floor, end, peak)) {
        *noise = fck_drv_ook_floor_half(&noise_floor);
        st     = capture(c, &noise_floor, edges, got, peak);
        if (st == KODE_OK && *got > 0U) {
            fck_drv_ook_code_t code;
            *decoded = fck_drv_ook_decode((const int32_t *)edges, *got, &code, 1U) > 0U;
            if (!fck_drv_subghz_scan_worth(*got, *decoded)) {
                *got = 0U;
            }
        }
    }
    l->noise[k] = noise_floor;
    l->known[k] = true;
    return st;
}

/* Where a remote heard on hop @p i is listened to from now on: there, or on a
 * neighbour louder by FCK_DRV_SUBGHZ_SCAN_NEIGHBOUR_OVER_HALF, each listened to for
 * FCK_DRV_SUBGHZ_SCAN_NEIGHBOUR_MS while the remote is still pressed. */
static size_t stay(cc_t *c, const uint32_t *hops, size_t nh, size_t i, int16_t *half)
{
    size_t best = i;
    for (size_t k = 0U; k < nh; k++) {
        if (!fck_drv_subghz_scan_neighbours(hops[i], hops[k]) || !leg_open(c->tca, hops[k])
            || ook_tune(c, hops[k]) != KODE_OK) {
            continue;
        }
        s_listen_hz = hops[k];
        int16_t const p = loudest(c, FCK_DRV_SUBGHZ_SCAN_NEIGHBOUR_MS);
        if (fck_drv_subghz_scan_louder(p, *half)) {
            best  = k;
            *half = p;
        }
    }
    return best;
}

kode_status_t fck_drv_cc1101_listen(const fck_drv_pins_t *hw, fck_drv_tca6408a_t *tca,
                                    fck_drv_cc1101_listen_t *l, uint32_t ms, int32_t *runs, size_t cap,
                                    size_t *n, fck_drv_cc1101_heard_t *heard)
{
    if (hw == NULL || l == NULL || runs == NULL || cap == 0U || n == NULL || heard == NULL
        || (!l->hold.hops && (!fck_drv_cc1101_tunes(l->hold.hz) || !leg_open(tca, l->hold.hz)))) {
        return KODE_DENIED;
    }
    if (!isr_service()) {
        return KODE_ERROR;
    }
    uint32_t *const edges = edges_take();
    if (edges == NULL) {
        return KODE_NO_RESOURCES;
    }
    cc_t          c;
    kode_status_t st = listen_open(&c, hw, tca);
    if (st != KODE_OK) {
        heap_caps_free(edges);
        return st;
    }
    /* What the call learns is kept only when it returns KODE_OK. */
    fck_drv_cc1101_listen_t w     = *l;
    fck_drv_cc1101_heard_t  out   = { .heard = false };
    size_t                  got   = 0U;
    int16_t                 peak    = 0;
    int16_t                 noise   = 0;
    bool                    decoded = false;
    int64_t const           end     = now_us() + (int64_t)ms * 1000;
    bool const              exp     = (tca != NULL);
    fck_drv_subghz_scan_hold_check(&w.hold, now_us(), exp);
    if (w.hold.hz != 0U) {
        /* Held on one frequency: to the read's end, or the hold's first. */
        st = hear(&c, &w, FIXED, w.hold.hz, fck_drv_subghz_scan_hold_end(&w.hold, end), edges, &got,
                  &peak, &noise, &decoded);
        if (st == KODE_OK && got > 0U) {
            out = (fck_drv_cc1101_heard_t){ .heard = true, .hz = w.hold.hz, .half = peak, .noise_half = noise };
            fck_drv_subghz_scan_hold_heard(&w.hold, w.hold.hz, decoded, now_us());
        }
        fck_drv_subghz_scan_hold_check(&w.hold, now_us(), exp);
    }
    if (w.hold.hz == 0U) {
        /* Hopping: from the start, or for what is left of a read whose hold
         * ended in it. */
        size_t                nh   = 0U;
        const uint32_t *const hops = fck_drv_subghz_scan_hops(&nh);
        while (st == KODE_OK && got == 0U && now_us() < end && !s_abort) {
            size_t const i = w.next;
            w.next         = (i + 1U) % nh;
            if (!leg_open(tca, hops[i])) {
                continue;   /* SW0 off its rest, with the GNSS receiver on the bus */
            }
            int64_t const window = now_us() + (int64_t)FCK_DRV_SUBGHZ_SCAN_HOP_MS * 1000;
            st = hear(&c, &w, i, hops[i], (window < end) ? window : end, edges, &got, &peak, &noise,
                      &decoded);
            if (st != KODE_OK || got == 0U) {
                continue;
            }
            /* Something real, said where it is loudest: there or on the louder
             * neighbour. A code read from it holds the listen there. */
            size_t const k = stay(&c, hops, nh, i, &peak);
            if (decoded) {
                w.noise[FIXED] = w.noise[k];
                w.known[FIXED] = w.known[k];
            }
            fck_drv_subghz_scan_hold_heard(&w.hold, hops[k], decoded, now_us());
            out = (fck_drv_cc1101_heard_t){
                             .heard      = true,
                             .hz         = hops[k],
                             .half       = peak,
                             .noise_half = w.known[k] ? fck_drv_ook_floor_half(&w.noise[k]) : noise,
            };
        }
    }
    s_listen_hz = 0U;
    st          = listen_close(&c, st);
    if (st == KODE_OK) {
        size_t const k = (got < cap) ? got : cap;
        (void)memcpy(runs, edges, k * sizeof(int32_t));
        *n     = k;
        *heard = out;
        *l     = w;
    }
    heap_caps_free(edges);
    return st;
}

/* ---- Packets ----------------------------------------------------------------------
 *
 * The packet receiver: BASE,
 * the 13 registers fck_drv_subghz_modem gives over it, and the chip left to
 * receive one packet and park in IDLE (MCSM1's RXOFF_MODE), its payload and
 * two status bytes in the FIFO. The receiver writes no TX FIFO and strobes
 * no STX: its strobes are SIDLE, SFRX and SRX. Sending a packet is
 * fck_drv_cc1101_packet_send(), below. */

/* RXBYTES and TXBYTES can read wrong while the FIFO fills or drains: read
 * until two agree [SWRZ020E]. */
static uint8_t fifo_count(const cc_t *c, uint8_t reg)
{
    uint8_t a = cc_status(c, reg);
    for (unsigned i = 0U; i < 8U; i++) {
        uint8_t const b = cc_status(c, reg);
        if (a == b) {
            break;
        }
        a = b;
    }
    return (uint8_t)(a & 0x7FU);
}

static uint8_t rx_bytes(const cc_t *c)
{
    return fifo_count(c, STA_RXBYTES);
}

static bool fifo_read(const cc_t *c, uint8_t *out, size_t n)
{
    uint8_t tx[FCK_DRV_SUBGHZ_MODEM_LENGTH_MAX + 3U] = { 0 };
    uint8_t rx[FCK_DRV_SUBGHZ_MODEM_LENGTH_MAX + 3U];
    tx[0] = (uint8_t)(REG_FIFO | HDR_READ_BURST);
    if (n + 1U > sizeof(tx) || !cc_begin(c)) {
        return false;
    }
    bool const ok = cc_shift(c, tx, rx, n + 1U);
    cc_end(c);
    if (ok) {
        (void)memcpy(out, &rx[1], n);
    }
    return ok;
}

/* The FIFO emptied and the chip receiving again. */
static bool rearm(const cc_t *c)
{
    idle(c);
    (void)cc_strobe(c, STB_SFRX);
    (void)cc_strobe(c, STB_SRX);
    return wait_marc(c, MARC_RX, RX_US);
}

/* The registers a modem changes over BASE, written in IDLE: the receiver's
 * and the transmitter's alike. BASE is already written: listen_open() wrote it. */
static bool modem_write(const cc_t *c, const fck_drv_subghz_modem_registers_t *r)
{
    idle(c);
    regval_t const w[] = {
        { REG_SYNC1, r->sync1 },       { REG_SYNC0, r->sync0 },     { REG_PKTLEN, r->pktlen },
        { REG_PKTCTRL0, r->pktctrl0 }, { REG_FSCTRL1, r->fsctrl1 }, { REG_MDMCFG4, r->mdmcfg4 },
        { REG_MDMCFG3, r->mdmcfg3 },   { REG_MDMCFG2, r->mdmcfg2 }, { REG_DEVIATN, r->deviatn },
        { REG_AGCCTRL2, r->agcctrl2 }, { REG_FREND1, r->frend1 },   { REG_TEST2, r->test2 },
        { REG_TEST1, r->test1 },
    };
    return cc_write_all(c, w, sizeof(w) / sizeof(w[0]));
}

static kode_status_t packet_tune(cc_t *c, uint32_t hz, const fck_drv_subghz_modem_registers_t *r)
{
    if (!modem_write(c, r)) {
        return KODE_IO_ERROR;
    }
    kode_status_t const st = take_leg(c, hz);
    if (st != KODE_OK) {
        return st;
    }
    (void)tune(c, hz);
    return rearm(c) ? KODE_OK : KODE_IO_ERROR;
}

kode_status_t fck_drv_cc1101_packets(const fck_drv_pins_t *hw, fck_drv_tca6408a_t *tca, uint32_t hz,
                                     const fck_drv_subghz_modem_t *modem, uint32_t ms,
                                     fck_drv_cc1101_packet_t *out, size_t cap, size_t *got)
{
    fck_drv_subghz_modem_registers_t r;
    if (hw == NULL || modem == NULL || out == NULL || cap == 0U || got == NULL || !fck_drv_cc1101_tunes(hz)
        || !leg_open(tca, hz) || !fck_drv_subghz_modem_registers(modem, &r)) {
        return KODE_DENIED;
    }
    cc_t          c;
    kode_status_t st = listen_open(&c, hw, tca);
    if (st != KODE_OK) {
        return st;
    }
    size_t const  len   = modem->length;
    size_t        k     = 0U;
    int64_t const end   = now_us() + (int64_t)ms * 1000;
    int64_t       yield = now_us();
    st                  = packet_tune(&c, hz, &r);
    for (int64_t t = now_us(); st == KODE_OK && k < cap && t < end && !s_abort; t = now_us()) {
        uint8_t const m = marc(&c);
        if (m == MARC_RX_OVERFLOW) {
            st = rearm(&c) ? KODE_OK : KODE_IO_ERROR;
        } else if (m == MARC_IDLE) {
            /* RXOFF_MODE IDLE: a packet ended. Shorter than one is a sync
             * word found in noise, cut short; it is dropped. */
            uint8_t buf[FCK_DRV_SUBGHZ_MODEM_LENGTH_MAX + 2U];
            if (rx_bytes(&c) >= len + 2U && fifo_read(&c, buf, len + 2U)) {
                fck_drv_cc1101_packet_t *const p = &out[k];
                (void)memcpy(p->bytes, buf, len);
                p->length = (uint8_t)len;
                p->status = fck_drv_subghz_modem_status(buf[len], buf[len + 1U], cc_status(&c, STA_FREQEST),
                                                        modem->crc);
                k++;
            }
            st = rearm(&c) ? KODE_OK : KODE_IO_ERROR;
        } else {
            /* Receiving, or between states: nothing to read yet. */
        }
        if (t - yield >= YIELD_US) {
            vTaskDelay(1);
            yield = now_us();
        } else {
            esp_rom_delay_us(POLL_US);
        }
    }
    st = listen_close(&c, st);
    if (st == KODE_OK) {
        *got = k;
    }
    return st;
}

/* ---- Packets, sent --------------------------------------------------------------------
 *
 * 2-FSK or GFSK packets the receiver above receives, from the same
 * registers. TX writes what it needs over BASE: MCSM1 with TXOFF_MODE IDLE
 * (BASE's goes to RX after a packet) and no clear-channel check, as BASE has
 * it; FREND0 with PA_POWER 0, so that FSK keys PATABLE index 0 - OOK's
 * FREND0 selects index 1, with index 0 the carrier off -; and PATABLE index 0
 * at the same power as OOK's "on" level on that band, pa_of(). GDO2 is SW1
 * (take_leg()) and stays; the end of a packet is read from MARCSTATE. */

#define MCSM1_TX_IDLE     0x00U   /* CCA_MODE 00 (always), RXOFF_MODE and TXOFF_MODE IDLE */
#define FREND0_FSK        0x10U   /* BASE's: LODIV_BUF_CURRENT_TX 1, PA_POWER 0           */
#define PACKET_PAUSE_MS   5U      /* between the repeats of a packet                      */
#define PACKET_SLACK_MS   50U     /* past a packet's air time, for the SPI polling        */

static bool fifo_write(const cc_t *c, const uint8_t *bytes, size_t n)
{
    uint8_t tx[FCK_DRV_SUBGHZ_MODEM_LENGTH_MAX + 1U];
    uint8_t rx[FCK_DRV_SUBGHZ_MODEM_LENGTH_MAX + 1U];
    if ((n == 0U) || (n > FCK_DRV_SUBGHZ_MODEM_LENGTH_MAX)) {
        return false;
    }
    tx[0] = (uint8_t)(REG_FIFO | HDR_WRITE_BURST);
    (void)memcpy(&tx[1], bytes, n);
    if (!cc_begin(c)) {
        return false;
    }
    bool const ok = cc_shift(c, tx, rx, n + 1U);
    cc_end(c);
    return ok;
}

static kode_status_t packet_tx_tune(cc_t *c, uint32_t hz, const fck_drv_subghz_modem_registers_t *r)
{
    if (!modem_write(c, r)) {
        return KODE_IO_ERROR;
    }
    regval_t const w[] = { { REG_MCSM1, MCSM1_TX_IDLE }, { REG_FREND0, FREND0_FSK } };
    if (!cc_write_all(c, w, sizeof(w) / sizeof(w[0]))) {
        return KODE_IO_ERROR;
    }
    kode_status_t const st = take_leg(c, hz);
    if (st != KODE_OK) {
        return st;
    }
    if (!tune(c, hz)) {
        return KODE_IO_ERROR;     /* not keyed on the frequency it was left on */
    }
    /* A single write reaches index 0 alone (see cc_write_patable()). */
    return cc_write(c, REG_PATABLE, pa_of(hz)) ? KODE_OK : KODE_IO_ERROR;
}

/* One packet: the TX FIFO flushed and filled, STX, and the wait for the chip
 * to leave TX for IDLE. The chip is in IDLE. It leaves IDLE within
 * TX_WAIT_US (calibration alone takes 721 us), else STX did nothing; then it
 * has @p air_us and PACKET_SLACK_MS to end. A packet that ended with bytes
 * still in the FIFO, or on the FIFO's underflow, is an error: SFTX clears
 * the underflow state, and the caller's SIDLE does the rest. */
static kode_status_t packet_tx_once(const cc_t *c, const uint8_t *bytes, size_t n, uint32_t air_us)
{
    (void)cc_strobe(c, STB_SFTX);
    if (!fifo_write(c, bytes, n) || (fifo_count(c, STA_TXBYTES) != n) || !cc_strobe(c, STB_STX)) {
        (void)cc_strobe(c, STB_SIDLE);
        (void)cc_strobe(c, STB_SFTX);
        return KODE_IO_ERROR;
    }
    int64_t const start = now_us();
    uint8_t       m     = marc(c);
    while ((m == MARC_IDLE) && ((now_us() - start) < (int64_t)TX_WAIT_US)) {
        esp_rom_delay_us(MARC_POLL_US);
        m = marc(c);
    }
    if (m == MARC_IDLE) {
        (void)cc_strobe(c, STB_SFTX);
        return KODE_IO_ERROR;
    }
    int64_t const end   = start + (int64_t)air_us + ((int64_t)PACKET_SLACK_MS * 1000);
    int64_t       yield = start;
    while ((m != MARC_IDLE) && (m != MARC_TX_UNDERFLOW) && (now_us() < end) && !s_abort) {
        if ((now_us() - yield) >= YIELD_US) {
            vTaskDelay(1);
            yield = now_us();
        } else {
            esp_rom_delay_us(MARC_POLL_US);
        }
        m = marc(c);
    }
    kode_status_t st = KODE_OK;
    if (m == MARC_TX_UNDERFLOW) {
        (void)cc_strobe(c, STB_SFTX);
        st = KODE_IO_ERROR;
    } else if (m != MARC_IDLE) {
        idle(c);   /* SFTX only in IDLE or on the underflow */
        (void)cc_strobe(c, STB_SFTX);
        st = s_abort ? KODE_IO_ERROR : KODE_TIMEOUT;     /* cut short, or never ended */
    } else if (fifo_count(c, STA_TXBYTES) != 0U) {
        (void)cc_strobe(c, STB_SFTX);
        st = KODE_IO_ERROR;
    } else {
        /* Ended in IDLE with the FIFO empty: sent. */
    }
    return st;
}

kode_status_t fck_drv_cc1101_packet_send(const fck_drv_pins_t *hw, fck_drv_tca6408a_t *tca, uint32_t hz,
                                         const fck_drv_subghz_modem_t *modem, const uint8_t *bytes,
                                         uint8_t length, uint8_t repeats)
{
    fck_drv_subghz_modem_registers_t r;
    uint32_t                         air_us = 0U;
    if (s_abort || (hw == NULL) || (modem == NULL) || (bytes == NULL) || (length == 0U) || (length != modem->length)
        || (repeats == 0U) || (repeats > FCK_DRV_CC1101_SEND_REPEATS_MAX) || !fck_drv_subghz_scan_may_transmit(hz)
        || !fck_drv_subghz_modem_may_transmit(modem, hz) || !leg_open(tca, hz)
        || !fck_drv_subghz_modem_registers(modem, &r) || !fck_drv_subghz_modem_air_time_us(modem, &air_us)
        || !fck_drv_subghz_scan_send_fits((uint64_t)air_us * repeats)) {
        return KODE_DENIED;   /* KODE_DENIED only before the chip is opened: never charged */
    }
    cc_t          c;
    kode_status_t st = listen_open(&c, hw, tca);
    if (st != KODE_OK) {
        return st;
    }
    st = packet_tx_tune(&c, hz, &r);
    for (uint8_t i = 0U; (st == KODE_OK) && (i < repeats); i++) {
        st = (s_abort && (i > 0U)) ? KODE_IO_ERROR : packet_tx_once(&c, bytes, length, air_us);
        if ((st == KODE_OK) && ((i + 1U) < repeats)) {
            vTaskDelay(pdMS_TO_TICKS(PACKET_PAUSE_MS));   /* the worker's core is free meanwhile */
        }
    }
    (void)cc_strobe(&c, STB_SIDLE);
    return listen_close(&c, st);
}
