/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file fake_idf.h
 * @brief The ESP-IDF the drivers' suite gives fck_drv_pins.c, fck_drv_uart.c,
 *        fck_drv_tca6408a.c, fck_drv_system.c and fck_drv_cc1101.c: pins that
 *        keep their level, mode and pulls (and what an outside driver holds
 *        them at), an I2C bus with a TCA6408A on it, two UARTs that keep what
 *        arrives and what is written, a clock, and the answers each call
 *        should give. The chip on the SPI is fake_cc1101.h's.
 */
#ifndef FAKE_IDF_H_
#define FAKE_IDF_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/rmt_encoder.h"
#include "driver/rmt_tx.h"
#include "driver/spi_master.h"
#include "driver/uart.h"
#include "fck_drv_pins.h"

#define IDF_PINS     64
#define IDF_EXT_NONE (-1)
#define IDF_I2C_LOG  64U
#define IDF_UART_RX  512U
#define IDF_UART_TX  512U

typedef struct {
    int      level;          /* the last level written                           */
    int      mode;           /* gpio_mode_t                                      */
    bool     pull_up;
    bool     pull_down;
    int      ext;            /* what an outside driver holds it at, or IDF_EXT_NONE */
    unsigned configs;        /* gpio_config() calls that named it                */
    unsigned resets;
    unsigned drive_caps;
    int      intr;
} idf_pin_t;

typedef struct {
    idf_pin_t pin[IDF_PINS];
    unsigned  conflicts;         /* a gpio_config() on a pin claimed as an output and not given back */
    uint64_t  claimed;

    /* The I2C bus and the expander at 0x21. */
    bool      i2c_device;        /* something acknowledges 0x21                       */
    esp_err_t i2c_new_rc;
    esp_err_t i2c_add_rc;
    esp_err_t i2c_del_rc;
    esp_err_t i2c_rm_rc;
    esp_err_t i2c_probe_rc;      /* the answer when a device is not there, ESP_ERR_NOT_FOUND by default */
    unsigned  i2c_fail_next;     /* transfers that fail before one succeeds           */
    unsigned  i2c_always_fail;   /* all transfers fail while non-zero                  */
    int       i2c_buses;         /* made and not yet deleted                           */
    int       i2c_devices;
    int       i2c_port;          /* the last bus made                                  */
    int       i2c_scl;
    int       i2c_sda;
    unsigned  i2c_probes;
    unsigned  i2c_transfers;     /* attempts, failed ones included                     */
    uint8_t   exp_reg[4];        /* INPUT (as driven), OUTPUT, POLARITY, CONFIG        */
    uint8_t   exp_pins;          /* what the module holds its pins at when not driven  */
    struct {
        bool    write;
        uint8_t reg;
        uint8_t val;
    } i2c_log[IDF_I2C_LOG];
    unsigned  i2c_log_n;

    /* The UARTs. */
    struct {
        bool     installed;
        int      rx_size;
        int      tx_pin;
        int      rx_pin;
        uint32_t baud;
        uint8_t  rx[IDF_UART_RX];
        size_t   rx_n;
        size_t   rx_at;
        uint8_t  tx[IDF_UART_TX];
        size_t   tx_n;
        unsigned flushes;
    } uart[UART_NUM_MAX];
    esp_err_t uart_install_rc;
    esp_err_t uart_config_rc;
    esp_err_t uart_pin_rc;
    esp_err_t uart_baud_rc;
    unsigned  uart_deletes;
    int       uart_max_write;    /* a write takes at most this many bytes; -1 for all   */

    /* A chip behind the UART that answers on its own (fake_mcuboot.h). With
     * uart_on_write set, a write goes to it and is not kept in tx, so tx never
     * fills; uart_on_read is called before every read and flush, to deliver
     * what is due by now through idf_fake_uart_feed(). With uart_virtual_wait,
     * a read that finds nothing waits on the virtual clock: it moves it on a
     * millisecond at a time, looking again each time, so a 15 s wait costs no
     * time. */
    void    (*uart_on_write)(int port, const uint8_t *data, size_t n);
    void    (*uart_on_read)(int port);
    bool      uart_virtual_wait;

    /* Called after every write to the expander has taken effect. */
    void    (*i2c_on_write)(uint8_t reg, uint8_t val);

    /* The RMT, on the virtual clock: nothing is ever clocked out. Off until a
     * case turns it on (every call is then a refusal). A transmit "plays" for
     * play_ms; a wait moves the clock by what it waits and says ESP_ERR_TIMEOUT
     * until the play is over. */
    struct {
        bool     on;
        uint32_t play_ms;
        int64_t  until_us;           /* when the transmit now playing is over              */
        unsigned transmits;
        unsigned waits;
        unsigned disables;
        unsigned deletes;
        void   (*on_wait)(unsigned n); /* called as the nth wait begins                    */
    } rmt;

    /* The clock. */
    int64_t   skipped_us;        /* what esp_rom_delay_us() has moved it by            */

    /* The GPIO interrupt. The fake raises none by itself: a case stands in
     * for an edge with idf_fake_gpio_edge(), which calls the handler the
     * driver added. intr_on_disable is called as gpio_intr_disable() begins,
     * while the handler can still be called. */
    gpio_isr_t isr;
    void      *isr_arg;
    int        isr_gpio;
    void     (*intr_on_disable)(int gpio);

    /* The shutdown hook. */
    esp_err_t shutdown_rc;
    void    (*shutdown_fn)(void);
    unsigned  shutdowns;
} idf_fake_t;

extern idf_fake_t g_idf;

/** @brief Nothing written, no bus, no device on it, every call answering ESP_OK. */
void idf_fake_reset(void);

/** @brief The module's lines, as kode_sys_exp_lend() would give them. */
const fck_drv_pins_t *idf_fake_hw(void);

/** @brief Position @p pos's GPIO. */
int idf_fake_gpio(uint8_t pos);

/** @brief An expander at 0x21 whose pins read @p pins when nothing drives them. */
void idf_fake_expander(uint8_t pins);

/** @brief The expander's register @p reg (0 INPUT, 1 OUTPUT, 2 POLARITY, 3 CONFIG). */
uint8_t idf_fake_exp(uint8_t reg);

/** @brief Whether the expander's output bits @p mask are driven high. */
bool idf_fake_exp_high(uint8_t mask);

/** @brief An edge on @p gpio, which then reads @p level: the handler added for it runs, as its
 *         interrupt would run it. false, and nothing called, when no handler is added for
 *         @p gpio or its interrupt is disabled. */
bool idf_fake_gpio_edge(int gpio, int level);

/** @brief Bytes the chip on @p port "sends" the P4. */
void idf_fake_uart_feed(int port, const void *data, size_t n);

#endif /* FAKE_IDF_H_ */
