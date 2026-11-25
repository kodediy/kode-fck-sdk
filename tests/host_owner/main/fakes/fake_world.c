/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file fake_world.c
 * @brief The model of the module and its header. See fake_world.h.
 *
 * It stands where the hardware does: it provides the functions fck_drv's
 * hardware files (fck_drv_pins.c, fck_drv_tca6408a.c, fck_drv_uart.c, fck_drv_cc1101.c,
 * fck_drv_system.c) provide, and kode-sdk's loan of J3. The owner is linked to it
 * instead of to them, and cannot tell.
 *
 * One lock guards the model. It is a FreeRTOS mutex and not a critical
 * section on purpose: the model allocates, and a task that is switched out
 * inside malloc() while another waits for the heap in a critical section
 * would never run again.
 */
#include "fake_world.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "unity.h"

#include "kode_sys.h"
#include "kode_log.h"

#include "fck_drv_cc1101.h"
#include "fck_drv_pins.h"
#include "fck_aos.h"
#include "fck_drv_system.h"
#include "fck_drv_tca6408a.h"
#include "fck_drv_uart.h"

#define WIRE_MAX     1024
#define LOG_MAX      256
#define LOG_LINE     256
#define SENT_MAX     64
#define SENT_LINE    200
#define SCRIPT_MAX   24
#define RX_MAX       96
#define PORTS        8

#define P2_BIT       ((uint8_t)(1U << FCK_BSP_EXPANDER_GNSS_RST))
#define P6_BIT       ((uint8_t)(1U << FCK_BSP_EXPANDER_NRF_RST))

/* Where a chip says what it runs after it starts, and how long a firmware takes
 * to answer: nothing a case depends on, only that they are not zero. */
#define BOOT_BANNER_MS 60U
#define ANSWER_MS      5U

typedef struct {
    char    *text;
    size_t   len;
    size_t   off;
    uint32_t at;
    bool     used;
} rx_t;

typedef struct {
    char     prefix[48];
    char    *response;
    uint32_t delay;
    bool     used;
} script_t;

typedef struct {
    bool     running;
    bool     quiet;                      /* answers `info`, prints nothing by itself */
    uint32_t ready_at;                   /* it hears nothing before this: it is still starting */
    char     banner[220];
    int      port;                       /* -1 while the owner has none open */
    char     partial[SENT_LINE];
    size_t   partial_n;
    char     sent[SENT_MAX][SENT_LINE];
    size_t   sent_n;
    size_t   blank;      /* empty lines it heard */
    rx_t     rx[RX_MAX];
    script_t script[SCRIPT_MAX];
} chip_t;

static struct {
    /* the header */
    bool       pair_held;
    bool       no_controller;
    sim_dev_t  dev;
    /* the expander, and the pins' levels when nothing on the chip drives them */
    uint8_t    out;
    uint8_t    pol;
    uint8_t    cfg;
    uint8_t    ext;
    uint8_t    pcf_port;
    uint32_t   pcf_writes;
    /* the GNSS receiver */
    bool       gnss_std;
    bool       gnss_run;
    uint32_t   gnss_run_at;
    /* the chips behind the UARTs */
    chip_t     chip[2];
    int        port_chip[PORTS];         /* -1 free */
    uint32_t   c5_resets;
    uint32_t   empty_looks;      /* times the owner looked at a header with nothing on it */
    /* the record */
    sim_wire_t wire[WIRE_MAX];
    size_t     wire_n;
    char       log[LOG_MAX][LOG_LINE];
    size_t     log_n;
    fck_drv_system_fn restart;
} W;

static SemaphoreHandle_t s_lock;

static void lock(void)
{
    (void)xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
}

static void unlock(void)
{
    (void)xSemaphoreGiveRecursive(s_lock);
}

uint32_t sim_now_ms(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

void sim_sleep(uint32_t ms)
{
    vTaskDelay(pdMS_TO_TICKS(ms));
}

typedef struct {
    uint32_t ms;
    void   (*fn)(void);
} later_t;

static void later_task(void *arg)
{
    later_t l = *(later_t *)arg;
    free(arg);
    vTaskDelay(pdMS_TO_TICKS(l.ms));
    l.fn();
    vTaskDelete(NULL);
}

void sim_later(uint32_t ms, void (*fn)(void))
{
    later_t *const l = malloc(sizeof(*l));
    TEST_ASSERT_NOT_NULL(l);
    *l = (later_t){ .ms = ms, .fn = fn };
    TEST_ASSERT_EQUAL(pdPASS, xTaskCreate(later_task, "later", 4096, l, 5, NULL));
}

/* ---- The record ------------------------------------------------------------------ */

static void wire_add(sim_wire_kind_t kind, uint8_t reg, uint8_t val)
{
    lock();
    if (W.wire_n < WIRE_MAX) {
        W.wire[W.wire_n++] = (sim_wire_t){ .kind = kind, .dev = W.dev, .reg = reg, .val = val };
    }
    unlock();
}

void sim_wire_clear(void)
{
    lock();
    W.wire_n = 0U;
    unlock();
}

size_t sim_wire_n(void)
{
    lock();
    size_t const n = W.wire_n;
    unlock();
    return n;
}

const sim_wire_t *sim_wire_at(size_t i)
{
    return (i < W.wire_n) ? &W.wire[i] : NULL;
}

size_t sim_wire_count(sim_wire_kind_t kind)
{
    size_t n = 0U;
    lock();
    for (size_t i = 0U; i < W.wire_n; i++) {
        n += (W.wire[i].kind == kind) ? 1U : 0U;
    }
    unlock();
    return n;
}

int sim_wire_first_write(void)
{
    int at = -1;
    lock();
    for (size_t i = 0U; i < W.wire_n && at < 0; i++) {
        if (W.wire[i].kind == SIM_W_WRITE || W.wire[i].kind == SIM_W_BLIND) {
            at = (int)i;
        }
    }
    unlock();
    return at;
}

void sim_log_write(char level, const char *tag, const char *fmt, ...)
{
    char    line[LOG_LINE];
    int     k = snprintf(line, sizeof(line), "%c %s: ", level, tag);
    va_list ap;
    va_start(ap, fmt);
    if (k > 0 && (size_t)k < sizeof(line)) {
        (void)vsnprintf(line + k, sizeof(line) - (size_t)k, fmt, ap);
    }
    va_end(ap);
    lock();
    if (W.log_n < LOG_MAX) {
        memcpy(W.log[W.log_n++], line, sizeof(line));
    }
    unlock();
    if (getenv("FCK_SIM_VERBOSE") != NULL) {
        printf("[%6lu] %s\n", (unsigned long)sim_now_ms(), line);
    }
}

void sim_log_clear(void)
{
    lock();
    W.log_n = 0U;
    unlock();
}

size_t sim_log_count(const char *needle)
{
    size_t n = 0U;
    lock();
    for (size_t i = 0U; i < W.log_n; i++) {
        n += (strstr(W.log[i], needle) != NULL) ? 1U : 0U;
    }
    unlock();
    return n;
}

bool sim_log_has(const char *needle)
{
    return sim_log_count(needle) > 0U;
}

/* ---- The chips' UARTs -------------------------------------------------------------- */

static void rx_free_locked(rx_t *r)
{
    free(r->text);
    *r = (rx_t){ 0 };
}

static void rx_clear_locked(chip_t *c)
{
    for (size_t i = 0U; i < RX_MAX; i++) {
        if (c->rx[i].used) {
            rx_free_locked(&c->rx[i]);
        }
    }
}

/* The model is called from the owner's task as well: an assertion there would
 * jump into a case that is not running. */
static void die(const char *why)
{
    printf("the model died: %s\n", why);
    fflush(stdout);
    abort();
}

static void rx_add_locked(chip_t *c, uint32_t delay_ms, const char *text)
{
    for (size_t i = 0U; i < RX_MAX; i++) {
        if (!c->rx[i].used) {
            size_t const n = strlen(text);
            char *const  p = malloc(n + 1U);
            if (p == NULL) {
                die("no memory");
            }
            memcpy(p, text, n + 1U);
            c->rx[i] = (rx_t){ .text = p, .len = n, .off = 0U,
                               .at = sim_now_ms() + delay_ms, .used = true };
            return;
        }
    }
    die("the receive queue is full");
}

static chip_t *chip_of_port(int port)
{
    if (port < 0 || port >= PORTS || W.port_chip[port] < 0) {
        return NULL;
    }
    return &W.chip[W.port_chip[port]];
}

static void say_banner_locked(chip_t *c, uint32_t delay_ms)
{
    if (c->running && c->banner[0] != '\0' && !c->quiet) {
        char line[sizeof(c->banner) + 4U];
        (void)snprintf(line, sizeof(line), "%s\r\n", c->banner);
        rx_add_locked(c, delay_ms, line);
    }
}

/* The chip starts: whatever it was saying is gone, and it says what it runs. */
static void chip_start_locked(chip_t *c)
{
    rx_clear_locked(c);
    c->partial_n = 0U;
    c->running   = true;
    c->ready_at  = sim_now_ms() + BOOT_BANNER_MS;
    say_banner_locked(c, BOOT_BANNER_MS);
}

static void chip_hold_locked(chip_t *c)
{
    rx_clear_locked(c);
    c->partial_n = 0U;
    c->running   = false;
}

static void respond_locked(chip_t *c, const char *line)
{
    if (!c->running || c->banner[0] == '\0' || (int32_t)(sim_now_ms() - c->ready_at) < 0) {
        return;     /* in reset, still starting, or a firmware that answers nothing */
    }
    for (size_t i = 0U; i < SCRIPT_MAX; i++) {
        script_t *const sc = &c->script[i];
        if (sc->used && strncmp(line, sc->prefix, strlen(sc->prefix)) == 0) {
            rx_add_locked(c, sc->delay, sc->response);
            return;
        }
    }
    char verb[32];
    size_t n = 0U;
    while (line[n] != '\0' && line[n] != ' ' && n + 1U < sizeof(verb)) {
        verb[n] = line[n];
        n++;
    }
    verb[n] = '\0';
    char text[sizeof(c->banner) + 64U];
    if (strcmp(verb, "info") == 0) {
        (void)snprintf(text, sizeof(text), "%s\r\n#DONE info rc=0\r\n", c->banner);
    } else {
        (void)snprintf(text, sizeof(text), "#DONE %s rc=2\r\n", verb);
    }
    rx_add_locked(c, ANSWER_MS, text);
}

void sim_firmware(int chip, const char *banner)
{
    lock();
    chip_t *const c   = &W.chip[chip];
    bool const    same = (banner != NULL) && strcmp(c->banner, banner) == 0;
    c->quiet          = false;
    c->banner[0]      = '\0';
    if (banner != NULL) {
        (void)snprintf(c->banner, sizeof(c->banner), "%s", banner);
        /* A chip does not say again what it said, unless it starts again. */
        if (!same) {
            say_banner_locked(c, ANSWER_MS);
        }
    }
    unlock();
}

void sim_firmware_quiet(int chip, const char *banner)
{
    lock();
    chip_t *const c = &W.chip[chip];
    (void)snprintf(c->banner, sizeof(c->banner), "%s", banner);
    c->quiet = true;
    unlock();
}

void sim_script(int chip, const char *prefix, const char *response, uint32_t delay_ms)
{
    char *const copy = strdup(response);
    TEST_ASSERT_NOT_NULL(copy);
    lock();
    chip_t *const c = &W.chip[chip];
    bool placed     = false;
    for (size_t i = 0U; i < SCRIPT_MAX && !placed; i++) {
        if (!c->script[i].used) {
            (void)snprintf(c->script[i].prefix, sizeof(c->script[i].prefix), "%s", prefix);
            c->script[i].response = copy;
            c->script[i].delay    = delay_ms;
            c->script[i].used     = true;
            placed                = true;
        }
    }
    unlock();
    TEST_ASSERT_TRUE_MESSAGE(placed, "the model has no room for another script");
}

void sim_inject(int chip, uint32_t delay_ms, const char *text)
{
    lock();
    rx_add_locked(&W.chip[chip], delay_ms, text);
    unlock();
}

size_t sim_sent(int chip, const char *prefix)
{
    size_t n = 0U;
    lock();
    chip_t *const c = &W.chip[chip];
    for (size_t i = 0U; i < c->sent_n; i++) {
        n += (strncmp(c->sent[i], prefix, strlen(prefix)) == 0) ? 1U : 0U;
    }
    unlock();
    return n;
}

const char *sim_sent_line(int chip, size_t n)
{
    lock();
    const char *const r = (n < W.chip[chip].sent_n) ? W.chip[chip].sent[n] : "";
    unlock();
    return r;
}

size_t sim_blank_lines(int chip)
{
    lock();
    size_t const n = W.chip[chip].blank;
    unlock();
    return n;
}

void sim_half_line(int chip, const char *text)
{
    lock();
    chip_t *const c = &W.chip[chip];
    c->partial_n    = strlen(text);
    memcpy(c->partial, text, c->partial_n);
    unlock();
}

void sim_sent_clear(int chip)
{
    lock();
    W.chip[chip].sent_n    = 0U;
    W.chip[chip].partial_n = 0U;
    W.chip[chip].blank     = 0U;
    unlock();
}

bool sim_chip_running(int chip)
{
    lock();
    bool const r = W.chip[chip].running;
    unlock();
    return r;
}

void sim_chip_boot(int chip, uint32_t banner_ms)
{
    lock();
    chip_t *const c = &W.chip[chip];
    rx_clear_locked(c);
    c->partial_n = 0U;
    c->running   = true;
    c->ready_at  = sim_now_ms() + banner_ms;
    say_banner_locked(c, banner_ms);
    unlock();
}

static TaskHandle_t s_case_task;

bool sim_in_case_task(void)
{
    return xTaskGetCurrentTaskHandle() == s_case_task;
}

uint32_t sim_c5_resets(void)
{
    return W.c5_resets;
}

kode_status_t fck_drv_uart_open(int port, int tx, int rx, uint32_t baud)
{
    (void)rx;
    (void)baud;
    kode_status_t st = KODE_DENIED;
    lock();
    if (port >= 0 && port < PORTS && W.port_chip[port] < 0) {
        int const chip = (tx == 100 + (int)FCK_BSP_POSITION_C5_RX) ? SIM_C5
                         : (tx == 100 + (int)FCK_BSP_POSITION_NRF_RX) ? SIM_NRF : -1;
        if (chip >= 0) {
            W.port_chip[port] = chip;
            W.chip[chip].port = port;
            st                = KODE_OK;
        }
    }
    unlock();
    return st;
}

void fck_drv_uart_close(int port)
{
    lock();
    if (port >= 0 && port < PORTS && W.port_chip[port] >= 0) {
        W.chip[W.port_chip[port]].port = -1;
        W.port_chip[port]              = -1;
    }
    unlock();
}

kode_status_t fck_drv_uart_baud(int port, uint32_t baud)
{
    (void)port;
    (void)baud;
    return KODE_OK;
}

size_t fck_drv_uart_read(int port, uint8_t *buf, size_t cap)
{
    size_t n = 0U;
    lock();
    chip_t *const c = chip_of_port(port);
    if (c != NULL) {
        uint32_t const now = sim_now_ms();
        /* In the order they were due, as a wire would deliver them. */
        for (;;) {
            rx_t *best = NULL;
            for (size_t i = 0U; i < RX_MAX; i++) {
                rx_t *const r = &c->rx[i];
                if (r->used && (int32_t)(now - r->at) >= 0
                    && (best == NULL || (int32_t)(best->at - r->at) > 0)) {
                    best = r;
                }
            }
            if (best == NULL || n >= cap) {
                break;
            }
            size_t const take = ((best->len - best->off) < (cap - n)) ? (best->len - best->off)
                                                                     : (cap - n);
            memcpy(buf + n, best->text + best->off, take);
            n += take;
            best->off += take;
            if (best->off >= best->len) {
                rx_free_locked(best);
            }
        }
    }
    unlock();
    return n;
}

size_t fck_drv_uart_read_wait(int port, uint8_t *buf, size_t cap, uint32_t ms)
{
    uint32_t const until = sim_now_ms() + ms;
    size_t         n     = fck_drv_uart_read(port, buf, cap);
    while (n == 0U && (int32_t)(sim_now_ms() - until) < 0) {
        sim_sleep(2U);
        n = fck_drv_uart_read(port, buf, cap);
    }
    return n;
}

kode_status_t fck_drv_uart_write(int port, const void *data, size_t n)
{
    kode_status_t st = KODE_DENIED;
    lock();
    chip_t *const c = chip_of_port(port);
    if (c != NULL) {
        st = KODE_OK;
        const char *const p = data;
        for (size_t i = 0U; i < n; i++) {
            if (p[i] == '\n') {
                c->partial[c->partial_n] = '\0';
                if (c->partial_n == 0U) {
                    /* An empty line: both firmwares say nothing to it. */
                    c->blank += c->running ? 1U : 0U;
                } else if (c->running) {
                    /* One line the chip heard: kept for the case to look at,
                     * and answered. */
                    if (c->sent_n < SENT_MAX) {
                        memcpy(c->sent[c->sent_n++], c->partial, sizeof(c->partial));
                    }
                    respond_locked(c, c->partial);
                }
                c->partial_n = 0U;
            } else if (p[i] != '\r' && c->partial_n + 1U < sizeof(c->partial)) {
                c->partial[c->partial_n++] = p[i];
            }
        }
    }
    unlock();
    return st;
}

void fck_drv_uart_flush(int port)
{
    lock();
    chip_t *const c = chip_of_port(port);
    if (c != NULL) {
        uint32_t const now = sim_now_ms();
        for (size_t i = 0U; i < RX_MAX; i++) {
            if (c->rx[i].used && (int32_t)(now - c->rx[i].at) >= 0) {
                rx_free_locked(&c->rx[i]);
            }
        }
    }
    unlock();
}

/* ---- The header's lines ------------------------------------------------------------- */

static kode_status_t s_lend_rc = KODE_OK;
static bool          s_no_radio;

void sim_exp_lend_answers(kode_status_t st)
{
    s_lend_rc = st;
}

void sim_exp_no_radio(bool none)
{
    s_no_radio = none;
}

kode_status_t kode_sys_exp_lend(const uint8_t *pos, size_t n, const char *tag, uint8_t *gpio)
{
    (void)tag;
    if (s_lend_rc != KODE_OK) {
        return s_lend_rc;
    }
    for (size_t i = 0U; i < n; i++) {
        gpio[i] = (uint8_t)(100U + pos[i]);
    }
    return KODE_OK;
}

/* kode-sdk's books for J3's controllers, as it keeps them with the
 * console on the USB: I2C1, SPI3, and UART0 up, the lowest free first. */
static uint8_t s_uarts_lent;

kode_status_t kode_sys_exp_ctrl(kode_sys_exp_bus_t bus, const char *tag, uint8_t *id)
{
    (void)tag;
    switch (bus) {
    case KODE_SYS_EXP_I2C:
        *id = 1U;
        return KODE_OK;
    case KODE_SYS_EXP_SPI:
        *id = 2U;
        return KODE_OK;
    case KODE_SYS_EXP_UART:
        if (s_uarts_lent > 4U) {
            return KODE_NO_RESOURCES;
        }
        *id = s_uarts_lent++;
        return KODE_OK;
    default:
        return KODE_DENIED;
    }
}

static bool     s_power_on;
static uint32_t s_power_calls;
static uint32_t s_returns;

static kode_status_t s_power_rc = KODE_OK;

void sim_exp_power_answers(kode_status_t st)
{
    s_power_rc = st;
}

static uint32_t s_power_busy;

void sim_exp_power_busy(uint32_t n)
{
    s_power_busy = n;
}

/* As kode-sdk's: the command is taken (or the radio owner's queue is short), and the rail follows with
 * EXP_POWER_SIG, whose rc is s_power_rc for an "on" that could not be done. */
kode_status_t kode_sys_exp_power(bool on)
{
    s_power_calls++;
    if (s_no_radio) {
        return KODE_DENIED;     /* no radio in the image: J3 is always powered */
    }
    if (s_power_busy > 0U) {
        s_power_busy--;
        return KODE_NO_RESOURCES;
    }
    uint8_t rc = (uint8_t)KODE_OK;
    if (s_power_rc != KODE_OK && on) {
        rc = (uint8_t)s_power_rc;       /* the supply could not be switched on: it stays as it was */
    } else {
        s_power_on = on;
    }
    sim_ao_exp_power(on, rc);
    return KODE_OK;
}

kode_status_t kode_sys_exp_return(const char *tag)
{
    (void)tag;
    s_returns++;
    s_uarts_lent = 0U;
    return KODE_OK;
}

bool sim_exp_power_on(void)
{
    return s_power_on;
}

uint32_t sim_exp_power_calls(void)
{
    return s_power_calls;
}

uint32_t sim_exp_returns(void)
{
    return s_returns;
}

void fck_drv_pins_rest(const fck_drv_pins_t *hw)
{
    (void)hw;
}

bool fck_drv_pins_pair_held(const fck_drv_pins_t *hw)
{
    (void)hw;
    lock();
    bool const held = W.pair_held;
    W.empty_looks += held ? 0U : 1U;
    unlock();
    return held;
}

bool fck_drv_pins_scl_held(const fck_drv_pins_t *hw)
{
    return fck_drv_pins_pair_held(hw);
}

static uint32_t s_c5_holds, s_c5_hold_ms;

void fck_drv_pins_c5_reset(const fck_drv_pins_t *hw, bool to_loader, bool hold)
{
    (void)hw;
    lock();
    W.c5_resets++;
    if (hold) {
        chip_hold_locked(&W.chip[SIM_C5]);
        s_c5_holds++;
        s_c5_hold_ms = sim_now_ms();
    } else if (to_loader) {
        chip_hold_locked(&W.chip[SIM_C5]);     /* the ROM loader answers nothing the firmware would */
    } else {
        chip_start_locked(&W.chip[SIM_C5]);
    }
    unlock();
}

void fck_drv_pins_c5_run(const fck_drv_pins_t *hw)
{
    (void)hw;
    lock();
    chip_start_locked(&W.chip[SIM_C5]);
    unlock();
}

void fck_drv_pins_c5_release_boot(const fck_drv_pins_t *hw)
{
    (void)hw;
}

static uint32_t s_c5_parks, s_c5_park_ms, s_cc_sleeps, s_cc_sleep_ms;

uint32_t sim_c5_holds(void)
{
    return s_c5_holds;
}

uint32_t sim_c5_hold_ms(void)
{
    return s_c5_hold_ms;
}

void fck_drv_pins_c5_park(const fck_drv_pins_t *hw)
{
    (void)hw;
    lock();
    chip_hold_locked(&W.chip[SIM_C5]);
    s_c5_parks++;
    s_c5_park_ms = sim_now_ms();
    unlock();
}

void sim_parks_clear(void)
{
    lock();
    s_c5_parks = s_c5_park_ms = s_cc_sleeps = s_cc_sleep_ms = 0U;
    s_c5_holds = s_c5_hold_ms = 0U;
    unlock();
}

uint32_t sim_c5_parks(void)
{
    return s_c5_parks;
}

uint32_t sim_c5_park_ms(void)
{
    return s_c5_park_ms;
}

uint32_t sim_cc1101_sleeps(void)
{
    return s_cc_sleeps;
}

uint32_t sim_cc1101_sleep_ms(void)
{
    return s_cc_sleep_ms;
}

kode_status_t fck_drv_cc1101_sleep(const fck_drv_pins_t *hw, uint8_t *partnum, uint8_t *version)
{
    (void)hw;
    lock();
    s_cc_sleeps++;
    s_cc_sleep_ms = sim_now_ms();
    unlock();
    if (partnum != NULL) {
        *partnum = 0x00U;
    }
    if (version != NULL) {
        *version = 0x14U;
    }
    return KODE_OK;
}

kode_status_t fck_drv_system_on_restart(fck_drv_system_fn fn)
{
    lock();
    W.restart = fn;
    unlock();
    return KODE_OK;
}

fck_drv_system_fn sim_restart_hook(void)
{
    lock();
    fck_drv_system_fn const fn = W.restart;
    unlock();
    return fn;
}

/* ---- The expander and what hangs off it -------------------------------------------- */

/* The receiver runs unless P2 is driven low; its standard firmware takes SDA
 * 420 ms after it starts, and keeps it until it is reset again. */
static bool bus_dead_locked(void)
{
    return W.dev == SIM_DEV_EXPANDER && W.gnss_std && W.gnss_run
           && (int32_t)(sim_now_ms() - W.gnss_run_at) >= (int32_t)FCK_BSP_GNSS_SDA_BUSY_MS;
}

/* The expander's pins moved: the receiver and the nRF follow. */
static void expander_moved_locked(void)
{
    bool const reset2 = ((W.cfg & P2_BIT) == 0U) && ((W.out & P2_BIT) == 0U);
    if (reset2) {
        W.gnss_run = false;
    } else if (!W.gnss_run) {
        W.gnss_run    = true;
        W.gnss_run_at = sim_now_ms();
    }
    bool const reset6 = ((W.cfg & P6_BIT) == 0U) && ((W.out & P6_BIT) == 0U);
    chip_t *const nrf = &W.chip[SIM_NRF];
    if (reset6 && nrf->running) {
        chip_hold_locked(nrf);
    } else if (!reset6 && !nrf->running) {
        chip_start_locked(nrf);
    }
}

static uint8_t pins_locked(void)
{
    return (uint8_t)((W.out & (uint8_t)~W.cfg) | (W.ext & W.cfg));
}

/* 0 acknowledged, 1 NAK, 2 failed (the bus is held, or has no pull-ups). */
static int probe_outcome_locked(void)
{
    if (!W.pair_held || W.dev == SIM_DEV_STUCK || bus_dead_locked()) {
        return 2;
    }
    return (W.dev == SIM_DEV_NONE) ? 1 : 0;
}

kode_status_t fck_drv_tca6408a_open(fck_drv_tca6408a_t *t, const fck_drv_pins_t *hw)
{
    lock();
    bool const none = W.no_controller;
    unlock();
    if (none) {
        return KODE_NO_RESOURCES;
    }
    t->bus = (void *)1;
    t->dev = (void *)1;
    t->scl = fck_drv_pins_gpio(hw, FCK_BSP_POSITION_I2C_SCL);
    t->sda = fck_drv_pins_gpio(hw, FCK_BSP_POSITION_I2C_SDA);
    return KODE_OK;
}

bool fck_drv_tca6408a_close(fck_drv_tca6408a_t *t)
{
    t->bus = NULL;
    t->dev = NULL;
    return true;
}

bool fck_drv_tca6408a_is_open(const fck_drv_tca6408a_t *t)
{
    return t->bus != NULL;
}

void fck_drv_tca6408a_forget(fck_drv_tca6408a_t *t)
{
    t->out = 0xFFU;
    t->cfg = 0xFFU;
}

bool fck_drv_tca6408a_probe(fck_drv_tca6408a_t *t, uint32_t tries, uint32_t *nacked, uint32_t *failed)
{
    uint32_t n     = 0U;
    uint32_t f     = 0U;
    bool     found = false;
    if (t->bus != NULL) {
        for (uint32_t i = 0U; i < tries && !found; i++) {
            lock();
            int const outcome = probe_outcome_locked();
            unlock();
            wire_add(SIM_W_PROBE, 0U, (uint8_t)outcome);
            if (outcome == 0) {
                found = true;
            } else {
                n += (outcome == 1) ? 1U : 0U;
                f += (outcome == 2) ? 1U : 0U;
                sim_sleep(2U);
            }
        }
    }
    if (nacked != NULL) {
        *nacked = n;
    }
    if (failed != NULL) {
        *failed = f;
    }
    return found;
}

kode_status_t fck_drv_tca6408a_read(fck_drv_tca6408a_t *t, uint8_t reg, uint8_t *val)
{
    if (t->dev == NULL || val == NULL) {
        return KODE_DENIED;
    }
    kode_status_t st = KODE_IO_ERROR;
    uint8_t       v  = 0U;
    lock();
    if (probe_outcome_locked() == 0) {
        st = KODE_OK;
        switch (W.dev) {
        case SIM_DEV_EXPANDER:
            v = (reg == FCK_BSP_EXPANDER_REGISTER_INPUT)      ? (uint8_t)(pins_locked() ^ W.pol)
                : (reg == FCK_BSP_EXPANDER_REGISTER_OUTPUT)   ? W.out
                : (reg == FCK_BSP_EXPANDER_REGISTER_POLARITY) ? W.pol
                : (reg == FCK_BSP_EXPANDER_REGISTER_CONFIG)   ? W.cfg
                                                    : 0U;
            break;
        case SIM_DEV_LOOKALIKE:
            v = (reg == FCK_BSP_EXPANDER_REGISTER_INPUT) ? W.ext : 0U;
            break;
        case SIM_DEV_PCF8574:
            /* The register's number is a byte for its port; it reads back what
             * its pins show. */
            W.pcf_port = reg;
            W.pcf_writes++;
            v = (uint8_t)(W.pcf_port & 0xFFU);
            break;
        default:
            st = KODE_IO_ERROR;
            break;
        }
    }
    unlock();
    wire_add(SIM_W_READ, reg, v);
    if (st == KODE_OK) {
        *val = v;
    } else {
        sim_sleep(2U);
    }
    return st;
}

kode_status_t fck_drv_tca6408a_write(fck_drv_tca6408a_t *t, uint8_t reg, uint8_t val)
{
    if (t->dev == NULL) {
        return KODE_DENIED;
    }
    kode_status_t st = KODE_IO_ERROR;
    lock();
    if (probe_outcome_locked() == 0) {
        st = KODE_OK;
        switch (W.dev) {
        case SIM_DEV_EXPANDER:
            if (reg == FCK_BSP_EXPANDER_REGISTER_OUTPUT) {
                W.out = val;
            } else if (reg == FCK_BSP_EXPANDER_REGISTER_POLARITY) {
                W.pol = val;
            } else if (reg == FCK_BSP_EXPANDER_REGISTER_CONFIG) {
                W.cfg = val;
            }
            expander_moved_locked();
            break;
        case SIM_DEV_LOOKALIKE:
            break;      /* acknowledged, kept nowhere */
        case SIM_DEV_PCF8574:
            W.pcf_port = val;
            W.pcf_writes += 2U;
            break;
        default:
            st = KODE_IO_ERROR;
            break;
        }
    }
    unlock();
    wire_add(SIM_W_WRITE, reg, val);
    if (st == KODE_OK) {
        if (reg == FCK_BSP_EXPANDER_REGISTER_OUTPUT) {
            t->out = val;
        } else if (reg == FCK_BSP_EXPANDER_REGISTER_CONFIG) {
            t->cfg = val;
        }
    } else {
        sim_sleep(2U);
    }
    return st;
}

/* fck_drv_tca6408a.c's own, for the two that are only reads and writes. */
kode_status_t fck_drv_tca6408a_mask(fck_drv_tca6408a_t *t, uint8_t mask, bool drive, bool level)
{
    if (mask == 0U || t->dev == NULL) {
        return KODE_DENIED;
    }
    uint8_t out = 0U;
    uint8_t cfg = 0U;
    if (fck_drv_tca6408a_read(t, FCK_BSP_EXPANDER_REGISTER_OUTPUT, &out) != KODE_OK) {
        out = t->out;
    }
    if (fck_drv_tca6408a_read(t, FCK_BSP_EXPANDER_REGISTER_CONFIG, &cfg) != KODE_OK) {
        cfg = t->cfg;
    }
    out = level ? (uint8_t)(out | mask) : (uint8_t)(out & (uint8_t)~mask);
    cfg = drive ? (uint8_t)(cfg & (uint8_t)~mask) : (uint8_t)(cfg | mask);
    uint8_t const first  = drive ? FCK_BSP_EXPANDER_REGISTER_OUTPUT : FCK_BSP_EXPANDER_REGISTER_CONFIG;
    uint8_t const second = drive ? FCK_BSP_EXPANDER_REGISTER_CONFIG : FCK_BSP_EXPANDER_REGISTER_OUTPUT;
    kode_status_t st     = fck_drv_tca6408a_write(t, first, drive ? out : cfg);
    if (st != KODE_OK) {
        return st;
    }
    return fck_drv_tca6408a_write(t, second, drive ? cfg : out);
}

kode_status_t fck_drv_tca6408a_mask_patient(fck_drv_tca6408a_t *t, const fck_drv_pins_t *hw, uint8_t mask, bool drive,
                                   bool level, uint32_t ms)
{
    uint32_t const until = sim_now_ms() + ms;
    kode_status_t  st    = KODE_IO_ERROR;
    do {
        if (fck_drv_tca6408a_open(t, hw) == KODE_OK) {
            st = fck_drv_tca6408a_mask(t, mask, drive, level);
            if (st == KODE_OK) {
                return st;
            }
            (void)fck_drv_tca6408a_close(t);
        }
        sim_sleep(20U);
    } while ((int32_t)(sim_now_ms() - until) < 0);
    return st;
}

/* Push-pull, blind: whatever sits at 0x21 is written OUTPUT and CONFIG with P2
 * low. The expander's receiver goes to reset and lets go of SDA; any other chip
 * is written. */
void fck_drv_tca6408a_blind_gnss_reset(fck_drv_tca6408a_t *t, const fck_drv_pins_t *hw)
{
    uint8_t const out = (uint8_t)(t->out & (uint8_t)~P2_BIT);
    uint8_t const cfg = (uint8_t)(t->cfg & (uint8_t)~P2_BIT);
    if (!fck_drv_tca6408a_close(t)) {
        return;
    }
    wire_add(SIM_W_BLIND, FCK_BSP_EXPANDER_REGISTER_OUTPUT, out);
    wire_add(SIM_W_BLIND, FCK_BSP_EXPANDER_REGISTER_CONFIG, cfg);
    lock();
    if (W.pair_held) {
        if (W.dev == SIM_DEV_EXPANDER) {
            W.out = out;
            W.cfg = cfg;
            expander_moved_locked();
        } else if (W.dev == SIM_DEV_PCF8574) {
            W.pcf_port = cfg;
            W.pcf_writes += 4U;
        }
    }
    unlock();
    t->out = out;
    t->cfg = cfg;
    (void)fck_drv_tca6408a_open(t, hw);
}

/* ---- Plugging ------------------------------------------------------------------------ */

static void forget_chip_locked(chip_t *c)
{
    rx_clear_locked(c);
    for (size_t i = 0U; i < SCRIPT_MAX; i++) {
        if (c->script[i].used) {
            free(c->script[i].response);
            c->script[i] = (script_t){ 0 };
        }
    }
    c->running   = false;
    c->quiet     = false;
    c->ready_at  = 0U;
    c->banner[0] = '\0';
    c->partial_n = 0U;
    c->sent_n    = 0U;
}

void sim_init(void)
{
    s_case_task = xTaskGetCurrentTaskHandle();
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateRecursiveMutex();
        TEST_ASSERT_NOT_NULL(s_lock);
        for (int i = 0; i < PORTS; i++) {
            W.port_chip[i] = -1;
        }
        W.chip[SIM_C5].port  = -1;
        W.chip[SIM_NRF].port = -1;
    }
}

void sim_unplug(void)
{
    lock();
    W.pair_held = false;
    W.dev       = SIM_DEV_NONE;
    for (int i = 0; i < 2; i++) {
        chip_hold_locked(&W.chip[i]);
    }
    W.gnss_run = false;
    unlock();
}

static sim_event_fn      s_ev_fn;
static void             *s_ev_ctx;
static volatile unsigned s_ev_in;

static bool ev_sink(fck_aos_event_t ev, uint32_t arg, void *ctx)
{
    (void)ctx;
    s_ev_in++;
    sim_event_fn const fn = s_ev_fn;
    if (fn != NULL) {
        fn(ev, arg, s_ev_ctx);
    }
    s_ev_in--;
    return true;
}

void sim_on_event(sim_event_fn fn, void *ctx)
{
    if (fn == NULL) {
        fck_aos_sink_set(FCK_AOS_SINK_EVENTS, NULL, NULL);
        s_ev_fn = NULL;
        for (unsigned i = 0U; (i < 400U) && (s_ev_in != 0U); i++) {
            vTaskDelay(pdMS_TO_TICKS(5U));
        }
        return;
    }
    s_ev_ctx = ctx;
    s_ev_fn  = fn;
    fck_aos_sink_set(FCK_AOS_SINK_EVENTS, ev_sink, NULL);
}

void sim_reset(void)
{
    sim_on_event(NULL, NULL);
    sim_exp_power_answers(KODE_OK);
    sim_exp_lend_answers(KODE_OK);
    sim_exp_no_radio(false);
    sim_exp_power_busy(0U);
    sim_unplug();
    sim_writers_reset();
    lock();
    W.no_controller = false;
    W.wire_n        = 0U;
    W.log_n         = 0U;
    W.c5_resets     = 0U;
    W.pcf_port      = 0xFFU;
    W.pcf_writes    = 0U;
    for (int i = 0; i < 2; i++) {
        forget_chip_locked(&W.chip[i]);
    }
    unlock();
}

void sim_plug_ex(const sim_plug_t *p)
{
    lock();
    W.dev       = p->dev;
    W.pair_held = true;
    /* The expander's power-on values, and the pins its module's resistors and
     * chips hold. */
    W.out      = 0xFFU;
    W.pol      = 0x00U;
    W.cfg      = 0xFFU;
    W.ext      = p->pins;
    W.pcf_port = 0xFFU;
    W.gnss_std = p->gnss_standard_fw;
    if (p->dev == SIM_DEV_EXPANDER) {
        W.gnss_run    = true;
        W.gnss_run_at = sim_now_ms() - p->gnss_age_ms;
        chip_start_locked(&W.chip[SIM_C5]);
        chip_start_locked(&W.chip[SIM_NRF]);
    }
    unlock();
}

void sim_plug(sim_dev_t dev, bool gnss_standard_fw)
{
    /* All high but P3, which nothing drives until the owner does. */
    sim_plug_t const p = { .dev = dev, .gnss_standard_fw = gnss_standard_fw,
                           .gnss_age_ms = 0U, .pins = 0xF7U };
    sim_plug_ex(&p);
}

void sim_blip(void)
{
    lock();
    W.out = 0xFFU;
    W.pol = 0x00U;
    W.cfg = 0xFFU;
    W.gnss_run    = true;
    W.gnss_run_at = sim_now_ms();
    chip_start_locked(&W.chip[SIM_C5]);
    chip_start_locked(&W.chip[SIM_NRF]);
    unlock();
}

void sim_gnss_take_bus(void)
{
    lock();
    W.gnss_std = true;
    /* P2 let go: the receiver runs, and has been for long enough. */
    W.out         = (uint8_t)(W.out | P2_BIT);
    W.cfg         = (uint8_t)(W.cfg | P2_BIT);
    W.gnss_run    = true;
    W.gnss_run_at = sim_now_ms() - (FCK_BSP_GNSS_SDA_BUSY_MS + 100U);
    unlock();
}

void sim_gnss_standard_fw(bool on)
{
    lock();
    W.gnss_std = on;
    unlock();
}

bool sim_gnss_running(void)
{
    lock();
    bool const r = W.gnss_run;
    unlock();
    return r;
}

bool sim_bus_dead(void)
{
    lock();
    bool const r = bus_dead_locked();
    unlock();
    return r;
}

uint8_t sim_exp_out(void)
{
    return W.out;
}

uint8_t sim_exp_cfg(void)
{
    return W.cfg;
}

uint8_t sim_exp_pol(void)
{
    return W.pol;
}

uint8_t sim_pcf_port(void)
{
    return W.pcf_port;
}

uint32_t sim_pcf_writes(void)
{
    return W.pcf_writes;
}

void sim_no_i2c_controller(bool none)
{
    lock();
    W.no_controller = none;
    unlock();
}

/* ---- Waiting and starting -------------------------------------------------------------- */

bool sim_wait_for(bool (*cond)(void), uint32_t ms)
{
    uint32_t const until = sim_now_ms() + ms;
    while (!cond()) {
        if ((int32_t)(sim_now_ms() - until) >= 0) {
            return false;
        }
        sim_sleep(5U);
    }
    return true;
}

static bool is_present(void)
{
    return fck_aos_present();
}

static bool is_absent(void)
{
    return !fck_aos_present();
}

static uint32_t s_gen_target;

static bool generation_reached(void)
{
    fck_aos_status_t st;
    fck_aos_status(&st);
    return st.generation >= s_gen_target;
}

bool sim_wait_generation(uint32_t gen, uint32_t ms)
{
    s_gen_target = gen;
    return sim_wait_for(generation_reached, ms);
}

bool sim_wait_present(uint32_t ms)
{
    return sim_wait_for(is_present, ms);
}

bool sim_wait_absent(uint32_t ms)
{
    return sim_wait_for(is_absent, ms);
}

void sim_begin_empty(void)
{
    TEST_ASSERT_EQUAL(KODE_OK, fck_aos_start());
    fck_aos_setting_listen(false, 0U);    /* a listening a case left is not the next one's */
    /* Nor is a receiver: with the module gone this only forgets that it was to be set again. */
    (void)fck_aos_gnss_power(false);
    sim_unplug();
    TEST_ASSERT_TRUE_MESSAGE(sim_wait_absent(8000U), "the owner did not notice the module leave");
    /* It forgets what it knew of a module when it looks at an empty header. */
    uint32_t const looks = W.empty_looks;
    uint32_t const until = sim_now_ms() + 3000U;
    while (W.empty_looks == looks) {
        if ((int32_t)(sim_now_ms() - until) >= 0) {
            TEST_FAIL_MESSAGE("the owner did not look at the empty header");
        }
        sim_sleep(5U);
    }
    sim_reset();
}

static bool firmware_is(int chip, const char *banner)
{
    fck_aos_firmware_t a;
    if (fck_aos_firmware((fck_aos_chip_t)chip, 0U, &a) != KODE_OK) {
        return false;
    }
    /* The owner keeps what follows the tag. */
    const char *const space = strchr(banner, ' ');
    return space != NULL && strcmp(a.body, space + 1) == 0;
}

void sim_begin_fck(const char *c5_banner, const char *nrf_banner)
{
    c5_banner  = (c5_banner != NULL) ? c5_banner : SIM_C5_BANNER_20;
    nrf_banner = (nrf_banner != NULL) ? nrf_banner : SIM_NRF_BANNER_20;
    TEST_ASSERT_EQUAL(KODE_OK, fck_aos_start());
    fck_aos_status_t st;
    fck_aos_status(&st);
    bool const reuse = st.present && W.dev == SIM_DEV_EXPANDER;
    if (!reuse) {
        sim_begin_empty();
        sim_firmware(SIM_C5, c5_banner);
        sim_firmware(SIM_NRF, nrf_banner);
        sim_plug(SIM_DEV_EXPANDER, false);
        TEST_ASSERT_TRUE_MESSAGE(sim_wait_present(3000U), "the F*CK was not found");
    } else {
        /* The module stays; what the cases changed is put back. */
        fck_aos_setting_listen(false, 0U);
        if (st.gnss_running) {
            (void)fck_aos_gnss_power(false);
        }
        for (int i = 0; i < 2; i++) {
            if (!st.chip_on[i]) {
                (void)fck_aos_power((fck_aos_chip_t)i, true);
            }
        }
        sim_writers_reset();
        lock();
        W.gnss_std  = false;
        W.c5_resets = 0U;
        for (int i = 0; i < 2; i++) {
            chip_t *const c = &W.chip[i];
            rx_clear_locked(c);
            for (size_t k = 0U; k < SCRIPT_MAX; k++) {
                if (c->script[k].used) {
                    free(c->script[k].response);
                    c->script[k] = (script_t){ 0 };
                }
            }
            c->sent_n    = 0U;
            c->partial_n = 0U;
        }
        unlock();
        sim_firmware(SIM_C5, c5_banner);
        sim_firmware(SIM_NRF, nrf_banner);
        /* A chip the owner has not heard saying this starts afresh and says it: a
         * case may have left one in the middle of a reset. */
        if (!firmware_is(SIM_C5, c5_banner)) {
            sim_chip_boot(SIM_C5, 20U);
        }
        if (!firmware_is(SIM_NRF, nrf_banner)) {
            sim_chip_boot(SIM_NRF, 20U);
        }
        sim_log_clear();
        sim_wire_clear();
    }
    /* Both chips have said what they run, as the case gave it. */
    uint32_t const until = sim_now_ms() + 3000U;
    while (!(firmware_is(SIM_C5, c5_banner) && firmware_is(SIM_NRF, nrf_banner))) {
        if ((int32_t)(sim_now_ms() - until) >= 0) {
            for (int i = 0; i < 2; i++) {
                fck_aos_firmware_t a = { 0 };
                kode_status_t const st = fck_aos_firmware((fck_aos_chip_t)i, 0U, &a);
                printf("  chip %d: st=%d known=%d seq=%lu running=%d body='%s' wanted='%s'\n", i,
                       (int)st, (int)a.known, (unsigned long)a.seq, (int)sim_chip_running(i), a.body,
                       W.chip[i].banner);
            }
            TEST_FAIL_MESSAGE("a chip did not say what it runs");
        }
        sim_sleep(5U);
    }
    sim_sent_clear(SIM_C5);
    sim_sent_clear(SIM_NRF);
    sim_log_clear();
    sim_wire_clear();
}

void sim_nmea(char *out, size_t cap, const char *body)
{
    uint8_t sum = 0U;
    for (const char *p = body; *p != '\0'; p++) {
        sum ^= (uint8_t)*p;
    }
    (void)snprintf(out, cap, "$%s*%02X", body, (unsigned)sum);
}
