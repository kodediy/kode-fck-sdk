/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_15_any_call.c
 * @brief Any call of fck.h in any state, in a random order from a fixed seed.
 *
 * Every public call is made with good arguments, with NULL ones and with ones out of range,
 * between changes of the world: the module out and in, a flicker of its power, a chip that
 * says nothing of what it runs, the receiver on and the bus taken, a listening, a flash in
 * progress in another task, a cancel at a random moment, HOME and sleep, fck_core_end() and
 * fck_core_begin(). What must hold, in every state:
 *
 *  - nothing asserts, hangs or crashes (a call that cannot be answered is cancelled by a
 *    timer, as an app would);
 *  - every code a call answers is one its @return lists (the table below);
 *  - the state stays coherent: fck_core_status() answers, no receiver or chip is "on" without a
 *    module, the generation never goes back;
 *  - the module never records an emission or a write to a chip that no call asked for:
 *    a send on the CC1101, an nRF line that makes it transmit, a C5 line that scans or
 *    sends, a flash. A call that answers KODE_DENIED has recorded nothing at all, and a
 *    change of the world (HOME and sleep included) records nothing.
 *
 * A failure prints the seed, the iteration and the last operations, to be replayed.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "unity.h"

#include "fck.h"
#include "fck_core.h"
#include "fck_aos.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "fake_world.h"

/* ---- The dice --------------------------------------------------------------------------- */

static uint32_t s_rng;

static uint32_t rnd(void)
{
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    return s_rng;
}

static uint32_t below(uint32_t n)
{
    return rnd() % n;
}

static bool chance(uint32_t percent)
{
    return below(100U) < percent;
}

/** How an argument is made. */
typedef enum { A_GOOD = 0, A_NULL, A_BAD } arg_t;

/* A call is either made with good arguments throughout, or with some of them not. */
static bool s_all_good;

static arg_t kind(void)
{
    if (s_all_good) {
        return A_GOOD;
    }
    uint32_t const r = below(100U);
    return (r < 30U) ? A_GOOD : ((r < 60U) ? A_NULL : A_BAD);
}

/** An optional out pointer: given always on a good call, now and then not on the others. */
#define OPT(p) ((s_all_good || chance(70U)) ? (p) : NULL)

/** One of the values, at random. */
#define PICK(arr) ((arr)[below((uint32_t)(sizeof(arr) / sizeof((arr)[0])))])

/* ---- The record of what happened ------------------------------------------------------- */

#define TRACE_N 16U

static char     s_trace[TRACE_N][64];

static unsigned s_trace_n;
static uint32_t s_seed;
static unsigned s_iter;

static void trace(const char *what, int rc)
{
    (void)snprintf(s_trace[s_trace_n % TRACE_N], sizeof(s_trace[0]), "%s -> %d", what, rc);
    s_trace_n++;
}

static const char *s_now = "";

static const char *story(void)
{
    static char text[TRACE_N * 70U + 120U];
    int         n = snprintf(text, sizeof(text), "seed %08x iteration %u, in %s; last operations:", (unsigned)s_seed, s_iter, s_now);
    unsigned    from = (s_trace_n > TRACE_N) ? s_trace_n - TRACE_N : 0U;
    for (unsigned i = from; i < s_trace_n && n < (int)sizeof(text) - 70; i++) {
        n += snprintf(text + n, sizeof(text) - (size_t)n, " [%s]", s_trace[i % TRACE_N]);
    }
    return text;
}

#define CHECKF(cond, fmt, ...)                                                       \
    do {                                                                             \
        if (!(cond)) {                                                               \
            static char m_[1800];                                                     \
            (void)snprintf(m_, sizeof(m_), fmt " | %s", __VA_ARGS__, story());       \
            (void)printf("ANY-FAIL: %s\n", m_);                                      \
            TEST_FAIL_MESSAGE("see ANY-FAIL above");                                                   \
        }                                                                            \
    } while (0)

#define CHECK(cond, msg) CHECKF((cond), "%s", (msg))

/* ---- What the module was asked to do -------------------------------------------------- */

typedef struct {
    uint32_t cc;       /**< Sends on the CC1101, OOK and packets.                    */
    uint32_t nrf;      /**< Lines to the nRF that transmit.                          */
    uint32_t power;    /**< Lines that set the nRF's power: a step of a call that asked, which
                        *   may be cut before the next one.                              */
    uint32_t c5;       /**< Lines to the C5 that scan or send.                       */
    uint32_t flash;    /**< Writes of a chip's firmware.                             */
} marks_t;

#define E_CC    1U
#define E_NRF   2U
#define E_C5    4U
#define E_FLASH 8U

static marks_t marks_now(void)
{
    marks_t m;
    m.cc    = sim_cc1101.sends + sim_cc1101.packet_sends;
    m.nrf   = (uint32_t)(sim_sent(SIM_NRF, "rf ping") + sim_sent(SIM_NRF, "rf echo on") + sim_sent(SIM_NRF, "rf cw"));
    m.power = (uint32_t)sim_sent(SIM_NRF, "rf power");
    m.c5    = (uint32_t)(sim_sent(SIM_C5, "scan") + sim_sent(SIM_C5, "gtx"));
    m.flash = sim_rom.calls + sim_dfu.recovers;
    return m;
}

static marks_t s_marks;

/* A flash another task runs: its writes are asked for, on its own account. */
static volatile bool s_flash_running;
static volatile bool s_flash_done_unseen;
static volatile kode_status_t s_flash_rc;
static volatile bool s_flash_rc_set;
static volatile int  s_flash_bad_rc;
#define B(c) (1U << (c))

static void check_marks(const char *what, uint32_t asked, kode_status_t rc, bool changes_world)
{
    marks_t const now   = marks_now();
    bool const    flash = (asked & E_FLASH) != 0U;
    bool const    other = s_flash_running || s_flash_done_unseen;
    if (!s_flash_running) {
        s_flash_done_unseen = false;
    }
    uint32_t const dcc = now.cc - s_marks.cc;
    uint32_t const dn  = now.nrf - s_marks.nrf;
    uint32_t const dc5 = now.c5 - s_marks.c5;
    uint32_t const dfl = now.flash - s_marks.flash;
    uint32_t const dpw = now.power - s_marks.power;
    CHECKF(dpw == 0U || (asked & E_NRF) != 0U, "%s: %u line(s) that set the nRF's power nobody asked for", what, (unsigned)dpw);
    /* A refusal sends nothing. A call an fck_cancel() landed in is refused too, but may have sent
     * what it asked for before the cancel came (a line is not taken back), so it is judged by what
     * it asked, like a call that went on. */
    bool const cancelled = (rc == KODE_DENIED) && (strstr(fck_error(), "cancelled") != NULL);
    bool const denied    = (rc == KODE_DENIED) && !changes_world && !cancelled;
    CHECKF(dcc == 0U || ((asked & E_CC) != 0U && !denied), "%s: %u send(s) on the CC1101 nobody asked for (why: %s)", what, (unsigned)dcc, fck_error());
    CHECKF(dn == 0U || ((asked & E_NRF) != 0U && !denied), "%s: %u nRF line(s) that transmit nobody asked for (why: %s)", what, (unsigned)dn, fck_error());
    CHECKF(dc5 == 0U || ((asked & E_C5) != 0U && !denied), "%s: %u C5 line(s) that scan or send nobody asked for (why: %s)", what, (unsigned)dc5, fck_error());
    CHECKF(dfl == 0U || other || (flash && !denied), "%s: %u write(s) to a chip's firmware nobody asked for", what, (unsigned)dfl);
    s_marks = now;
}

/* ---- Helper tasks: a flash in another task, and a cancel at a random moment ---------------- */

static volatile unsigned s_helpers;

typedef struct {
    uint32_t chip;
    uint32_t delay_ms;
} helper_arg_t;

static const uint8_t k_boot[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
static const uint8_t k_part[4] = { 9, 10, 11, 12 };
static const uint8_t k_app[16] = { 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28 };
static const uint8_t k_nrf[64] = { 0x3D, 0xB8, 0xF3, 0x96 };

static void flash_task(void *p)
{
    helper_arg_t const a = *(helper_arg_t *)p;
    free(p);
    fck_flash_report_t rep;
    kode_status_t      rc;
    if (a.chip == (uint32_t)FCK_C5) {
        const fck_image_t img[3] = {
            { .addr = FCK_BSP_C5_BOOTLOADER, .data = k_boot, .len = sizeof(k_boot) },
            { .addr = FCK_BSP_C5_PARTITIONS, .data = k_part, .len = sizeof(k_part) },
            { .addr = FCK_BSP_C5_APP, .data = k_app, .len = sizeof(k_app) },
        };
        rc = fck_core_flash(FCK_C5, img, 3U, &rep);
    } else {
        const fck_image_t img = { .addr = FCK_BSP_NRF_SLOT0, .data = k_nrf, .len = sizeof(k_nrf) };
        rc = fck_core_flash(FCK_NRF, &img, 1U, &rep);
    }
    s_flash_rc     = rc;
    s_flash_rc_set = true;
    if ((unsigned)rc >= 32U || ((B(KODE_OK) | B(KODE_DENIED) | B(KODE_TIMEOUT) | B(KODE_IO_ERROR) | B(KODE_ERROR)
                                  | B(KODE_NO_RESOURCES)) & B(rc)) == 0U) {
        s_flash_bad_rc = (int)rc + 1000;
    }
    s_flash_done_unseen = true;
    s_flash_running = false;
    s_helpers--;
    vTaskDelete(NULL);
}

static void start_flash(void)
{
    if (s_flash_running) {
        return;
    }
    helper_arg_t *const a = malloc(sizeof(*a));
    TEST_ASSERT_NOT_NULL(a);
    a->chip     = below(2U);
    a->delay_ms = 0U;
    sim_rom.ms  = 60U + below(200U);
    sim_dfu.ms  = 60U + below(200U);
    s_flash_running = true;
    s_helpers++;
    if (xTaskCreate(flash_task, "anyflash", 16384, a, 5, NULL) != pdPASS) {
        s_flash_running = false;
        s_helpers--;
        free(a);
    }
}

static void cancel_task(void *p)
{
    helper_arg_t const a = *(helper_arg_t *)p;
    free(p);
    vTaskDelay(pdMS_TO_TICKS(a.delay_ms));
    (void)fck_core_cancel();
    s_helpers--;
    vTaskDelete(NULL);
}

/** A cancel in @p ms from another task: the way out of a wait an app would not sit through. */
static void cancel_later(uint32_t ms)
{
    helper_arg_t *const a = malloc(sizeof(*a));
    TEST_ASSERT_NOT_NULL(a);
    a->chip     = 0U;
    a->delay_ms = ms;
    s_helpers++;
    if (xTaskCreate(cancel_task, "anycancel", 4096, a, 5, NULL) != pdPASS) {
        s_helpers--;
        free(a);
    }
}

/* ---- The app's event callback: it only counts ---------------------------------------------- */

static volatile unsigned s_events;
static volatile unsigned s_bad_event;

static void count_cb(fck_aos_event_t ev, uint32_t arg, void *ctx)
{
    (void)arg;
    (void)ctx;
    s_events++;
    if ((unsigned)ev < 1U || (unsigned)ev > 7U) {
        s_bad_event++;
    }
}

/* ---- Arguments ------------------------------------------------------------------------------ */

static fck_chip_t chip_arg(void)
{
    uint32_t const r = below(100U);
    return (r < 45U) ? FCK_C5 : ((r < 90U) ? FCK_NRF : (fck_chip_t)(2 + (int)below(200U)));
}

static uint32_t ms_short(void)
{
    static const uint32_t v[] = { 0U, 5U, 20U, 40U };
    return PICK(v);
}

static uint32_t hz_arg(arg_t k)
{
    static const uint32_t good[] = { 433920000U, 868300000U, 433050000U, 434790000U, 868000000U, 868600000U };
    static const uint32_t bad[]  = { 0U, 315000000U, 433049999U, 434790001U, 867999999U, 868600001U, 920000000U, 0xFFFFFFFFU };
    return (k == A_BAD) ? PICK(bad) : PICK(good);
}

/* A frequency the CC1101 tunes, or one it does not. */
static uint32_t tune_arg(arg_t k)
{
    static const uint32_t good[] = { 433920000U, 868300000U, 315000000U, 433050000U, 868000000U };
    static const uint32_t bad[]  = { 0U, 1U, 299000000U, 350000000U, 500000000U, 1000000000U, 2400000000U, 0xFFFFFFFFU };
    return (k == A_BAD) ? PICK(bad) : PICK(good);
}

static uint32_t listen_hz_arg(arg_t k)
{
    return (k == A_GOOD && chance(30U)) ? FCK_SUBGHZ_HOP : tune_arg(k);
}

static size_t cap_arg(arg_t k, size_t good)
{
    static const size_t bad[] = { 0U, 1U, 2U };
    return (k == A_BAD) ? PICK(bad) : good;
}

/* Buffers, each as large as the biggest cap the SDK takes (a larger one is clamped or refused). */
static fck_wifi_network_t s_net[FCK_WIFI_NETWORK_MAX];
static fck_wifi_channel_t s_chan[FCK_WIFI_CHANNELS_CAP_MAX];
static fck_ble_device_t   s_ble[FCK_BLE_DEVICE_MAX];
static fck_wifi_frame_t   s_wifi_frames[FCK_WIFI_FRAME_MAX];
static int8_t             s_dbm8[256];
static int16_t            s_dbm16[FCK_SUBGHZ_POINTS_MAX];
static fck_radio_frame_t  s_frames[FCK_RADIO_FRAMES_MAX];
static int32_t            s_us[4200];
static int32_t            s_enc[FCK_SUBGHZ_FRAMES_MAX * FCK_SUBGHZ_FRAME_RUNS + 8U];
static size_t             s_enc_n;
static int32_t            s_noise[400];
static int32_t            s_long[8];
static uint32_t           s_freqs[64];
static fck_subghz_code_t  s_codes[16];
static fck_subghz_rolling_t s_roll[8];
static fck_subghz_packet_t s_packets[FCK_SUBGHZ_PACKETS_MAX];
static char               s_text[4096];
static char               s_file[4096];
static size_t             s_file_n;
static uint8_t            s_bytes[FCK_SUBGHZ_PACKET_MAX + 8U];

static void arms_init(void)
{
    const fck_subghz_code_t code = { .proto = FCK_SUBGHZ_EV1527, .bits = 24, .key = 0xABCDEFU, .te_us = 320U };
    s_enc_n = 0U;
    (void)fck_core_subghz_encode(&code, 3U, s_enc, sizeof(s_enc) / sizeof(s_enc[0]), &s_enc_n);
    for (size_t i = 0U; i < sizeof(s_noise) / sizeof(s_noise[0]); i++) {
        int32_t const w = (int32_t)(80U + below(1500U));
        s_noise[i]      = (i % 2U == 0U) ? w : -w;
    }
    for (size_t i = 0U; i < sizeof(s_long) / sizeof(s_long[0]); i++) {
        s_long[i] = (i % 2U == 0U) ? 900000 : -900000;       /* a carrier of seconds */
    }
    s_file_n = 0U;
    (void)memset(s_file, 0, sizeof(s_file));
    if (s_enc_n > 0U) {
        (void)fck_core_subghz_to_file(433920000U, s_enc, s_enc_n, s_file, sizeof(s_file), &s_file_n);
    }
    for (size_t i = 0U; i < sizeof(s_bytes); i++) {
        s_bytes[i] = (uint8_t)i;
    }
}

static const int32_t *runs_arg(arg_t k, size_t *n)
{
    if (k == A_NULL) {
        *n = 1U + below(10U);
        return NULL;
    }
    if (k == A_BAD) {
        switch (below(4U)) {
        case 0: *n = 0U; return s_enc;
        case 1: *n = sizeof(s_long) / sizeof(s_long[0]); return s_long;
        case 2: *n = 1U; return s_noise;
        default: *n = sizeof(s_noise) / sizeof(s_noise[0]); return s_noise;
        }
    }
    *n = (s_enc_n > 0U) ? s_enc_n : 2U;
    return s_enc;
}

static fck_subghz_code_t code_arg(arg_t k)
{
    fck_subghz_code_t c = { .proto = FCK_SUBGHZ_EV1527, .bits = 24, .key = 0xABCDEFU, .te_us = 320U };
    if (k == A_BAD) {
        switch (below(5U)) {
        case 0: c.proto = (fck_subghz_protocol_t)(8 + (int)below(100U)); break;
        case 1: c.bits = (uint8_t)(1U + below(8U)); break;
        case 2: c.key = 0xFFFFFFFFU; break;
        case 3: c.te_us = 0U; break;
        default: c.proto = FCK_SUBGHZ_PT2262; c.key = 0xAAAAAAU; c.bits = 24; c.te_us = 1U; break;
        }
    } else {
        static const fck_subghz_protocol_t p[] = { FCK_SUBGHZ_EV1527, FCK_SUBGHZ_PT2262, FCK_SUBGHZ_HT12E };
        c.proto = PICK(p);
        c.bits  = (c.proto == FCK_SUBGHZ_HT12E) ? 12U : 24U;
        c.key   = 0x5U;
        c.te_us = 320U;
    }
    return c;
}

static fck_subghz_packet_settings_t settings_arg(arg_t k)
{
    fck_subghz_packet_settings_t s = { FCK_SUBGHZ_GFSK, 38400U, 20630U, 101000U, 0xD391U, 8U, true, true };
    if (k == A_BAD) {
        switch (below(6U)) {
        case 0: s.baud = 0U; break;
        case 1: s.baud = 10000000U; break;
        case 2: s.length = 0U; break;
        case 3: s.length = (uint8_t)(FCK_SUBGHZ_PACKET_MAX + 1U); break;
        case 4: s.modulation = (fck_subghz_modulation_t)(5 + (int)below(100U)); break;
        default: s.filter_hz = 5U; s.deviation_hz = 0U; break;
        }
    }
    return s;
}

/* ---- The calls, one wrapper each ------------------------------------------------------------ */

typedef kode_status_t (*call_fn)(void);

typedef struct {
    const char *name;
    call_fn     fn;
    uint32_t    allowed;   /**< Bit per code the @return lists.            */
    uint32_t    asks;      /**< What it may make the module do (E_*).      */
    bool        world;     /**< It changes the world (a perturbation).     */
} call_t;

#define M_STD (B(KODE_OK) | B(KODE_DENIED) | B(KODE_TIMEOUT) | B(KODE_IO_ERROR) | B(KODE_NO_RESOURCES))
#define M_RES M_STD
#define M_ERR (M_RES | B(KODE_ERROR))
#define M_PURE (B(KODE_OK) | B(KODE_DENIED))

/* The module: */

static kode_status_t c_begin(void)
{
    kode_status_t const rc = fck_core_begin();
    if (rc == KODE_OK) {
        fck_status_t st;
        TEST_ASSERT_EQUAL(KODE_OK, fck_core_status(&st));
        CHECK(st.j3_supply != FCK_J3_OFF, "fck_core_begin() answered OK and the status says J3's supply was switched off");
    }
    return rc;
}

static kode_status_t c_end(void)
{
    kode_status_t const rc = fck_core_end();
    if (rc == KODE_OK) {
        CHECK(!sim_exp_power_on(), "fck_core_end() answered OK and J3's supply is on");
        CHECK(!fck_is_present(), "fck_core_end() answered OK and the module is present");
        fck_status_t st;
        TEST_ASSERT_EQUAL(KODE_OK, fck_core_status(&st));
        CHECK(!st.present && !st.gnss_running, "fck_core_end() answered OK and the status still has a module");
        CHECK(!sim_cc1101.on_air, "fck_core_end() answered OK with a carrier on the air");
    }
    return rc;
}

static kode_status_t c_error(void)
{
    const char *const t = fck_error();
    CHECK(t != NULL, "fck_error() answered NULL");
    return KODE_OK;
}

static kode_status_t c_cancel(void)
{
    return fck_core_cancel();
}

static kode_status_t c_is_present(void)
{
    (void)fck_is_present();
    return KODE_OK;
}

static kode_status_t c_wait_present(void)
{
    return fck_core_wait_present(ms_short());
}

static uint32_t s_generation;

static kode_status_t c_status(void)
{
    fck_status_t st;
    memset(&st, 0xA5, sizeof(st));
    if (kind() == A_NULL) {
        kode_status_t const rc = fck_core_status(NULL);
        CHECK(rc == KODE_DENIED, "fck_core_status(NULL) did not answer KODE_DENIED");
        return rc;
    }
    kode_status_t const rc = fck_core_status(&st);
    CHECK(rc == KODE_OK, "fck_core_status(&st) did not answer KODE_OK");
    if (!st.present) {
        CHECK(!st.gnss_running, "no module, and the receiver is running");
        CHECK(!st.cc1101_ok, "no module, and the CC1101 answers");
    }
    CHECK(st.generation >= s_generation, "the generation went back");
    s_generation = st.generation;
    return rc;
}

static kode_status_t c_header_position(void)
{
    uint32_t const pos = below(24U);
    uint8_t const  r   = fck_header_position((uint8_t)pos);
    CHECK(r == 0U || r == (uint8_t)pos, "fck_header_position() answered a position that is not the asked one");
    CHECK(r <= 14U, "fck_header_position() answered a position J3 does not have");
    return KODE_OK;
}

static kode_status_t c_event_sink(void)
{
    sim_on_event(chance(70U) ? count_cb : NULL, NULL);
    return KODE_OK;
}

/* The chips: */

static void line_fn(void *ctx, const fck_line_t *line)
{
    (void)ctx;
    (void)line;
}

static kode_status_t c_command(void)
{
    static const char *const lines[] = { "info", "scan 2g", "scan all", "chan 2g 50", "ble scan 1", "gnss on", "gnss off",
                                         "gtx -n GGA", "wp", "rf ping 1 2 3", "rf cw", "rf power 9", "rf power 0",
                                         "rf echo on 10", "rf spec 0 10 100 1", "reboot", "", "rf" };
    static char        longer[400];
    static const char *const verbs[] = { NULL, "info", "scan", "nope", "" };
    arg_t const        k = kind();
    const char *line;
    if (k == A_NULL) {
        line = NULL;
    } else if (k == A_BAD && chance(50U)) {
        (void)memset(longer, 'x', sizeof(longer) - 1U);
        longer[sizeof(longer) - 1U] = '\0';
        line = longer;
    } else {
        line = PICK(lines);
    }
    int32_t rcv = 7;
    return fck_core_command(chip_arg(), line, PICK(verbs), chance(50U) ? line_fn : NULL, NULL, ms_short(),
                       OPT(&rcv));
}

static kode_status_t c_reset(void)
{
    return fck_core_reset(chip_arg());
}

static kode_status_t c_power(void)
{
    return fck_core_power(chip_arg(), chance(60U));
}

static kode_status_t c_firmware(void)
{
    fck_firmware_t a;
    return fck_core_firmware(chip_arg(), (kind() == A_NULL) ? NULL : &a);
}

static kode_status_t c_flash(void)
{
    fck_flash_report_t rep;
    arg_t const        k = kind();
    fck_chip_t const   chip = chip_arg();
    sim_rom.ms = 20U;
    sim_dfu.ms = 20U;
    const fck_image_t c5[3] = {
        { .addr = FCK_BSP_C5_BOOTLOADER, .data = k_boot, .len = sizeof(k_boot) },
        { .addr = FCK_BSP_C5_PARTITIONS, .data = k_part, .len = sizeof(k_part) },
        { .addr = FCK_BSP_C5_APP, .data = k_app, .len = sizeof(k_app) },
    };
    const fck_image_t nrf = { .addr = FCK_BSP_NRF_SLOT0, .data = k_nrf, .len = sizeof(k_nrf) };
    const fck_image_t bad[2] = {
        { .addr = 0x2001U, .data = k_boot, .len = 8U },
        { .addr = FCK_BSP_C5_APP, .data = NULL, .len = 8U },
    };
    if (k == A_NULL) {
        return fck_core_flash(chip, NULL, 1U + below(3U), OPT(&rep));
    }
    if (k == A_BAD) {
        return fck_core_flash(chip, bad, (size_t)below(3U), &rep); /* n never past the 2 rows: that is the caller's array */
    }
    return (chip == FCK_NRF) ? fck_core_flash(chip, &nrf, 1U, &rep) : fck_core_flash(chip, c5, 3U, &rep);
}

static kode_status_t c_rescue_probe(void)
{
    fck_rescue_report_t rep;
    return fck_core_rescue_probe(chip_arg(), (kind() == A_NULL) ? NULL : &rep);
}

/* The C5's radio: */

static fck_band_t band_arg(arg_t k)
{
    return (k == A_BAD) ? (fck_band_t)(3 + (int)below(100U)) : (fck_band_t)below(3U);
}

static kode_status_t c_wifi_scan(void)
{
    arg_t const k = kind();
    size_t n = 5U;
    size_t const cap = (k == A_BAD && chance(50U)) ? SIZE_MAX : cap_arg(k, FCK_WIFI_NETWORK_MAX);
    return fck_core_wifi_scan(band_arg(k), (k == A_NULL) ? NULL : s_net, cap, OPT(&n));
}

static kode_status_t c_wifi_channels(void)
{
    arg_t const k = kind();
    size_t n = 5U;
    static const uint16_t dwell[] = { 0U, 20U, 100U, 500U, 65535U };
    size_t const cap = (k == A_BAD && chance(40U)) ? FCK_WIFI_CHANNELS_CAP_MAX + 1U
                                                    : ((k == A_BAD && chance(30U)) ? SIZE_MAX : cap_arg(k, FCK_WIFI_CHANNELS_CAP_MAX));
    return fck_core_wifi_channels(band_arg(k), PICK(dwell), (k == A_NULL) ? NULL : s_chan, cap, OPT(&n));
}

static kode_status_t c_wifi_frames(void)
{
    arg_t const k = kind();
    size_t      n = 5U;
    fck_wifi_frames_statistics_t stats;
    static const uint8_t  good[] = { 1U, 6U, 36U, 177U };
    static const uint8_t  bad[]  = { 0U, 178U, 255U };
    static const uint16_t ms_good[] = { 20U, 100U, 250U };
    static const uint16_t ms_bad[]  = { 0U, 19U, 5001U, 65535U };
    size_t const cap = (k == A_BAD && chance(40U)) ? (chance(50U) ? SIZE_MAX : FCK_WIFI_FRAME_MAX + 1U) : cap_arg(k, FCK_WIFI_FRAME_MAX);
    return fck_core_wifi_frames((k == A_BAD && chance(40U)) ? PICK(bad) : PICK(good),
                                (k == A_BAD && chance(40U)) ? PICK(ms_bad) : PICK(ms_good),
                                (k == A_NULL) ? NULL : s_wifi_frames, cap, OPT(&n), chance(50U) ? &stats : NULL);
}

static kode_status_t c_ble_scan(void)
{
    arg_t const k = kind();
    size_t n = 5U;
    static const uint8_t secs_good[] = { 1U, 2U, 5U };
    static const uint8_t secs_bad[]  = { 0U, 61U, 255U };
    size_t const cap = (k == A_BAD && chance(50U)) ? SIZE_MAX : cap_arg(k, FCK_BLE_DEVICE_MAX);
    return fck_core_ble_scan((k == A_BAD && chance(50U)) ? PICK(secs_bad) : PICK(secs_good), (k == A_NULL) ? NULL : s_ble, cap,
                        OPT(&n));
}

/* A device for the calls that read what one advertised: none, one whose bytes and length are
 * anything at all (a length no scan gives included), or one whose fields are well formed. */
static const fck_ble_device_t *device_arg(arg_t k)
{
    static fck_ble_device_t d;
    if (k == A_NULL) {
        return NULL;
    }
    memset(&d, 0, sizeof(d));
    if (k == A_BAD) {
        for (size_t i = 0U; i < sizeof(d.data); i++) {
            d.data[i] = (uint8_t)below(256U);
        }
        d.data_len = (uint8_t)below(256U);
        return &d;
    }
    /* Fields of the four kinds the calls read and one they do not, until one does not fit. */
    static const uint8_t types[] = { 0xFFU, 0x16U, 0x02U, 0x03U, 0x19U, 0x01U };
    size_t at = 0U;
    for (;;) {
        size_t const body = below(9U);
        if (at + 2U + body > sizeof(d.data)) {
            break;
        }
        d.data[at]      = (uint8_t)(1U + body);
        d.data[at + 1U] = PICK(types);
        for (size_t i = 0U; i < body; i++) {
            d.data[at + 2U + i] = (uint8_t)below(256U);
        }
        at += 2U + body;
    }
    d.data_len = (uint8_t)at;
    return &d;
}

static kode_status_t c_ble_manufacturer_data(void)
{
    uint16_t company = 0U;
    uint8_t  out[FCK_BLE_DATA_MAX];
    size_t   n = 0U;
    const fck_ble_device_t *const d = device_arg(kind());
    (void)fck_ble_manufacturer_data(d, OPT(&company), (kind() == A_NULL) ? NULL : out, cap_arg(kind(), sizeof(out)), OPT(&n));
    return KODE_OK;
}

static kode_status_t c_ble_service_data(void)
{
    uint8_t out[FCK_BLE_DATA_MAX];
    size_t  n = 0U;
    const fck_ble_device_t *const d = device_arg(kind());
    (void)fck_ble_service_data(d, (uint16_t)below(65536U), (kind() == A_NULL) ? NULL : out, cap_arg(kind(), sizeof(out)),
                               OPT(&n));
    return KODE_OK;
}

static kode_status_t c_ble_services(void)
{
    uint16_t out[16];
    size_t   n = 0U;
    const fck_ble_device_t *const d = device_arg(kind());
    (void)fck_ble_services(d, (kind() == A_NULL) ? NULL : out, cap_arg(kind(), sizeof(out) / sizeof(out[0])), OPT(&n));
    return KODE_OK;
}

static kode_status_t c_ble_appearance(void)
{
    uint16_t appearance = 0U;
    const fck_ble_device_t *const d = device_arg(kind());
    (void)fck_ble_appearance(d, OPT(&appearance));
    return KODE_OK;
}

/* The GNSS receiver: */

static kode_status_t c_gnss_begin(void)
{
    return fck_core_gnss_begin();
}

static void gnss_feed(void);

static kode_status_t c_gnss_read(void)
{
    fck_gnss_fix_t fix;
    if (chance(70U)) {
        gnss_feed();
    }
    return fck_core_gnss_read((kind() == A_NULL) ? NULL : &fix, 40U);
}

static kode_status_t c_gnss_end(void)
{
    return fck_core_gnss_end();
}

/* The nRF's radio: */

static uint16_t mhz_arg(arg_t k)
{
    static const uint16_t good[] = { 2402U, 2440U, 2480U };
    static const uint16_t bad[]  = { 0U, 2399U, 2401U, 2481U, 2501U, 5000U, 65535U };
    return (k == A_BAD) ? PICK(bad) : PICK(good);
}

static int8_t dbm_arg(arg_t k)
{
    static const int8_t good[] = { -20, -10, 0, 4, 6 };
    static const int8_t bad[]  = { 7, 20, 127, -47, -128 };
    return (k == A_BAD) ? PICK(bad) : PICK(good);
}

static kode_status_t c_radio_spectrum(void)
{
    arg_t const k = kind();
    size_t n = 3U;
    uint16_t first = 2402U, last = 2404U;     /* the three values the model's firmware answers */
    if (k == A_BAD) {
        first = PICK(((const uint16_t[]){ 0U, 2399U, 2501U, 2450U }));
        last  = PICK(((const uint16_t[]){ 0U, 2400U, 2501U, 65535U }));
    }
    uint32_t const dw = (k == A_BAD && chance(30U)) ? 0U : 100U;
    return fck_core_radio_spectrum(first, last, dw, (uint16_t)((k == A_BAD && chance(30U)) ? 0U : 1U),
                              (k == A_NULL) ? NULL : s_dbm8, cap_arg(k, sizeof(s_dbm8)), OPT(&n));
}

static kode_status_t c_radio_sniff(void)
{
    arg_t const k = kind();
    size_t n = 3U;
    fck_radio_sniff_statistics_t st;
    static const uint32_t sniff_ms[] = { 200U, 500U, 1000U };
    return fck_core_radio_sniff(mhz_arg(k), (k == A_BAD && chance(30U)) ? 0xFFFFFFFFU : PICK(sniff_ms), (int8_t)-90,
                           (k == A_NULL) ? NULL : s_frames, cap_arg(k, FCK_RADIO_FRAMES_MAX), OPT(&n),
                           OPT(&st));
}

static kode_status_t c_radio_sniff_tune(void)
{
    fck_radio_sniff_config_t cfg = { 1U, 0U, 32U, 2U, 0xAAAAAAAAU, 0xAAU };
    arg_t const k = kind();
    if (k == A_NULL) {
        return fck_core_radio_sniff_tune(NULL);
    }
    if (k == A_BAD) {
        switch (below(4U)) {
        case 0: cfg.rate_mbps = 3U; break;
        case 1: cfg.len = 0U; break;
        case 2: cfg.len = 200U; break;
        default: cfg.base_len = 9U; break;
        }
    }
    return fck_core_radio_sniff_tune(&cfg);
}

static kode_status_t c_radio_ping(void)
{
    arg_t const k = kind();
    fck_radio_ping_statistics_t st;
    return fck_core_radio_ping((uint16_t)((k == A_BAD && chance(50U)) ? PICK(((const uint16_t[]){ 0U, 65535U })) : 1U + below(3U)),
                          mhz_arg(k), (uint16_t)((k == A_BAD && chance(30U)) ? 0U : 5U + below(10U)), dbm_arg(k),
                          (k == A_NULL) ? NULL : &st);
}

static kode_status_t c_radio_echo_start(void)
{
    arg_t const k = kind();
    return fck_core_radio_echo_start(mhz_arg(k), dbm_arg(k));
}

static kode_status_t c_radio_echo_stop(void)
{
    fck_radio_echo_statistics_t st;
    return fck_core_radio_echo_stop((kind() == A_NULL) ? NULL : &st);
}

/* The CC1101, listening: */

static fck_subghz_bandwidth_t bw_arg(arg_t k)
{
    return (k == A_BAD) ? (fck_subghz_bandwidth_t)(2 + (int)below(100U)) : (fck_subghz_bandwidth_t)below(2U);
}

static kode_status_t c_subghz_level(void)
{
    arg_t const k = kind();
    int16_t     dbm = 0;
    return fck_core_subghz_level(tune_arg(k), bw_arg(k), (k == A_NULL) ? NULL : &dbm);
}

static kode_status_t c_subghz_spectrum(void)
{
    arg_t const k = kind();
    size_t n = 3U;
    uint16_t const count = (uint16_t)((k == A_BAD) ? PICK(((const uint16_t[]){ 0U, 1025U, 65535U })) : 2U + below(20U));
    return fck_core_subghz_spectrum(433000000U + (k == A_BAD ? 0xF0000000U : 0U), (k == A_BAD && chance(30U)) ? 0U : 20000U, count,
                               bw_arg(k), (k == A_NULL) ? NULL : s_dbm16, cap_arg(k, FCK_SUBGHZ_POINTS_MAX),
                               OPT(&n));
}

static kode_status_t c_subghz_listen_start(void)
{
    return fck_core_subghz_listen_start(listen_hz_arg(kind()));
}

static kode_status_t c_subghz_listen_read(void)
{
    arg_t const k = kind();
    size_t n = 3U;
    fck_subghz_heard_t heard;
    return fck_core_subghz_listen_read(0U, (k == A_BAD && chance(40U)) ? FCK_SUBGHZ_MS_MAX + 1U : ms_short(),
                                  (k == A_NULL) ? NULL : s_us, (k == A_BAD && chance(40U)) ? 4097U + 5000U : cap_arg(k, 4096U),
                                  OPT(&n), OPT(&heard));
}

static kode_status_t c_subghz_listen_hz(void)
{
    (void)fck_subghz_listen_hz();
    return KODE_OK;
}

static kode_status_t c_subghz_listen_stop(void)
{
    return fck_core_subghz_listen_stop();
}

static kode_status_t c_subghz_frequencies(void)
{
    arg_t const k = kind();
    size_t n = 3U;
    return fck_core_subghz_frequencies((k == A_NULL) ? NULL : s_freqs, (k == A_BAD) ? PICK(((const size_t[]){ 0U, 1U, 37U })) : 38U,
                                  OPT(&n));
}

static kode_status_t c_subghz_find(void)
{
    fck_subghz_hit_t hit;
    arg_t const      k = kind();
    return fck_core_subghz_find((k == A_BAD) ? FCK_SUBGHZ_MS_MAX + 1U : ms_short(), (k == A_NULL) ? NULL : &hit);
}

static kode_status_t c_subghz_capture(void)
{
    arg_t const k = kind();
    size_t n = 3U;
    return fck_core_subghz_capture(tune_arg(k), (k == A_BAD && chance(40U)) ? FCK_SUBGHZ_MS_MAX + 1U : ms_short(),
                              (k == A_NULL) ? NULL : s_us, cap_arg(k, 4096U), OPT(&n));
}

/* Codes, pure: */

static kode_status_t c_subghz_decode(void)
{
    arg_t const k = kind();
    size_t      n;
    size_t      got = 3U;
    const int32_t *const us = runs_arg(k, &n);
    return fck_core_subghz_decode(us, n, (kind() == A_NULL) ? NULL : s_codes, cap_arg(kind(), 16U), OPT(&got));
}

static kode_status_t c_subghz_encode(void)
{
    arg_t const k = kind();
    size_t n = 3U;
    fck_subghz_code_t const code = code_arg(k);
    return fck_core_subghz_encode((k == A_NULL) ? NULL : &code, (uint8_t)((k == A_BAD && chance(40U)) ? PICK(((const uint8_t[]){ 0U, 17U, 255U })) : 1U + below(4U)),
                             (kind() == A_NULL) ? NULL : s_enc, cap_arg(kind(), sizeof(s_enc) / sizeof(s_enc[0])),
                             OPT(&n));
}

static kode_status_t c_subghz_protocol_info(void)
{
    arg_t const k = kind();
    fck_subghz_protocol_info_t info;
    return fck_core_subghz_protocol_info((k == A_BAD) ? (fck_subghz_protocol_t)(8 + (int)below(1000U)) : (fck_subghz_protocol_t)below(8U),
                                    (k == A_NULL) ? NULL : &info);
}

static kode_status_t c_subghz_decode_rolling(void)
{
    arg_t const k = kind();
    size_t      n;
    size_t      got = 3U;
    const int32_t *const us = runs_arg(k, &n);
    return fck_core_subghz_decode_rolling(us, n, (kind() == A_NULL) ? NULL : s_roll, cap_arg(kind(), 8U), OPT(&got));
}

static kode_status_t c_subghz_may_transmit(void)
{
    (void)fck_subghz_may_transmit(hz_arg(kind()));
    return KODE_OK;
}

static kode_status_t c_subghz_is_rolling(void)
{
    size_t n;
    const int32_t *const us = runs_arg(kind(), &n);
    (void)fck_subghz_is_rolling(us, n);
    return KODE_OK;
}

static kode_status_t c_subghz_air_time_left(void)
{
    uint32_t ms = 0U;
    return fck_core_subghz_air_time_left(hz_arg(kind()), (kind() == A_NULL) ? NULL : &ms);
}

/* The CC1101, transmitting: */

static kode_status_t c_subghz_send_raw(void)
{
    arg_t const k = kind();
    size_t      n;
    const int32_t *const us = runs_arg(k, &n);
    return fck_core_subghz_send_raw(hz_arg(kind()), us, n,
                               (uint8_t)((kind() == A_BAD) ? PICK(((const uint8_t[]){ 0U, 33U, 255U })) : 1U + below(2U)));
}

static kode_status_t c_subghz_send_code(void)
{
    arg_t const k = kind();
    fck_subghz_code_t const code = code_arg(k);
    return fck_core_subghz_send_code(hz_arg(kind()), (k == A_NULL) ? NULL : &code,
                                (uint8_t)((kind() == A_BAD) ? PICK(((const uint8_t[]){ 0U, 17U, 255U })) : 1U + below(3U)));
}

static kode_status_t c_subghz_packet_receive(void)
{
    arg_t const k = kind();
    size_t got = 3U;
    fck_subghz_packet_settings_t const s = settings_arg(kind());
    size_t const cap = (k == A_BAD && chance(40U)) ? SIZE_MAX : cap_arg(k, FCK_SUBGHZ_PACKETS_MAX);
    return fck_core_subghz_packet_receive(tune_arg(kind()),
                                     (kind() == A_NULL) ? NULL : &s, (k == A_BAD && chance(30U)) ? FCK_SUBGHZ_MS_MAX + 1U : ms_short(),
                                     (kind() == A_NULL) ? NULL : s_packets, cap, OPT(&got));
}

static kode_status_t c_subghz_packet_send(void)
{
    arg_t const k = kind();
    fck_subghz_packet_settings_t const s = settings_arg(kind());
    uint8_t const length = (uint8_t)((kind() == A_BAD) ? PICK(((const uint8_t[]){ 0U, 3U, 62U, 255U })) : 8U);
    return fck_core_subghz_packet_send(hz_arg(k), (kind() == A_NULL) ? NULL : &s, (kind() == A_NULL) ? NULL : s_bytes, length,
                                  (uint8_t)((kind() == A_BAD) ? PICK(((const uint8_t[]){ 0U, 33U, 255U })) : 1U + below(2U)));
}

/* .ksub files, pure: */

static kode_status_t c_subghz_to_file(void)
{
    arg_t const k = kind();
    size_t n;
    size_t length = 3U;
    const int32_t *const us = runs_arg(k, &n);
    return fck_core_subghz_to_file((kind() == A_BAD) ? 0U : 433920000U, us, n, (kind() == A_NULL) ? NULL : s_text,
                                      cap_arg(kind(), sizeof(s_text)), OPT(&length));
}

static kode_status_t c_subghz_code_to_file(void)
{
    arg_t const k = kind();
    size_t length = 3U;
    fck_subghz_code_t const code = code_arg(k);
    return fck_core_subghz_code_to_file((kind() == A_BAD) ? 0U : 433920000U, (k == A_NULL) ? NULL : &code,
                                           (kind() == A_NULL) ? NULL : s_text, cap_arg(kind(), sizeof(s_text)),
                                           OPT(&length));
}

static kode_status_t c_subghz_from_file(void)
{
    arg_t const k = kind();
    fck_subghz_file_t file;
    size_t runs_n = 0U;
    static char junk[512];
    const char *text = s_file;
    size_t length = s_file_n;
    if (k == A_NULL) {
        text = NULL;
    } else if (k == A_BAD) {
        for (size_t i = 0U; i < sizeof(junk) - 1U; i++) {
            junk[i] = (char)(' ' + below(95U));
            if (chance(8U)) {
                junk[i] = '\n';
            }
        }
        junk[sizeof(junk) - 1U] = '\0';
        text   = chance(50U) ? junk : s_file;
        length = (text == junk) ? strlen(junk) : below((uint32_t)s_file_n + 1U);
    }
    return fck_core_subghz_from_file(text, length, (kind() == A_NULL) ? NULL : &file, (kind() == A_NULL) ? NULL : s_us,
                                        cap_arg(kind(), 4096U), OPT(&runs_n));
}

/* ---- The table --------------------------------------------------------------------------- */

#define CALL(fn, mask, asks) { #fn, c_##fn, (mask), (asks), false }

static const call_t k_calls[] = {
    CALL(begin, B(KODE_OK) | B(KODE_EXISTS) | B(KODE_DENIED) | B(KODE_NO_RESOURCES) | B(KODE_ERROR), 0),
    CALL(end, B(KODE_OK) | B(KODE_DENIED) | B(KODE_NO_RESOURCES), 0),
    CALL(error, M_PURE, 0),
    CALL(cancel, M_PURE, 0),
    CALL(is_present, M_PURE, 0),
    CALL(wait_present, B(KODE_OK) | B(KODE_TIMEOUT) | B(KODE_DENIED), 0),
    CALL(status, M_PURE, 0),
    CALL(header_position, M_PURE, 0),
    CALL(event_sink, M_PURE, 0),
    CALL(command, M_STD, E_NRF | E_C5),
    CALL(reset, M_STD, 0),
    CALL(power, M_STD, 0),
    CALL(firmware, M_STD, 0),
    CALL(flash, M_ERR, E_FLASH),
    CALL(rescue_probe, M_STD, 0),
    CALL(wifi_scan, M_RES, E_C5),
    CALL(wifi_channels, M_RES, 0),
    CALL(wifi_frames, M_RES, 0),
    CALL(ble_scan, M_RES, 0),
    CALL(ble_manufacturer_data, M_PURE, 0),
    CALL(ble_service_data, M_PURE, 0),
    CALL(ble_services, M_PURE, 0),
    CALL(ble_appearance, M_PURE, 0),
    CALL(gnss_begin, M_STD, 0),
    CALL(gnss_read, M_STD, 0),
    CALL(gnss_end, M_STD, 0),
    CALL(radio_spectrum, M_STD, 0),
    CALL(radio_sniff, M_STD, 0),
    CALL(radio_sniff_tune, M_STD, 0),
    CALL(radio_ping, M_STD, E_NRF),
    CALL(radio_echo_start, M_STD, E_NRF),
    CALL(radio_echo_stop, M_STD, 0),
    CALL(subghz_level, M_STD, 0),
    CALL(subghz_spectrum, M_STD, 0),
    CALL(subghz_listen_start, M_STD, 0),
    CALL(subghz_listen_read, M_ERR, 0),
    CALL(subghz_listen_hz, M_PURE, 0),
    CALL(subghz_listen_stop, M_STD, 0),
    CALL(subghz_frequencies, M_PURE, 0),
    CALL(subghz_find, M_STD, 0),
    CALL(subghz_capture, M_ERR, 0),
    CALL(subghz_decode, M_PURE, 0),
    CALL(subghz_encode, M_PURE, 0),
    CALL(subghz_protocol_info, M_PURE, 0),
    CALL(subghz_decode_rolling, M_PURE, 0),
    CALL(subghz_may_transmit, M_PURE, 0),
    CALL(subghz_is_rolling, M_PURE, 0),
    CALL(subghz_air_time_left, M_STD, 0),
    CALL(subghz_send_raw, M_ERR, E_CC),
    CALL(subghz_send_code, M_ERR, E_CC),
    CALL(subghz_packet_receive, M_ERR, 0),
    CALL(subghz_packet_send, M_ERR, E_CC),
    CALL(subghz_to_file, M_PURE, 0),
    CALL(subghz_code_to_file, M_PURE, 0),
    CALL(subghz_from_file, M_PURE, 0),
};

#define CALLS_N (sizeof(k_calls) / sizeof(k_calls[0]))
#define CALLS_N_MAX 64U

/* The calls that need a firmware and wait for one: a timer ends the wait, as an app would. */

/* ---- The world: changes of state ----------------------------------------------------------- */

static bool s_chip_mute[2];

static void gnss_feed(void)
{
    char s[160];
    char line[200];
    sim_nmea(s, sizeof(s), "GNGGA,202634.00,4807.0380,N,01131.0000,E,1,04,6.2,109.418,M,-3.399,M,,");
    (void)snprintf(line, sizeof(line), "#GNSSU %s\r\n", s);
    sim_inject(SIM_C5, 5U, line);
    sim_nmea(s, sizeof(s), "GNRMC,202634.00,A,4807.0380,N,01131.0000,E,2.80,95.05,290926,,,A,V");
    (void)snprintf(line, sizeof(line), "#GNSSU %s\r\n", s);
    sim_inject(SIM_C5, 10U, line);
}

static void world_step(void)
{
    uint32_t const r = below(16U);
    static char    what[40];
    (void)snprintf(what, sizeof(what), "world %u", (unsigned)r);
    s_now = what;
    switch (r) {
    case 0:
        sim_unplug();
        break;
    case 1:
    case 2:
        sim_plug(SIM_DEV_EXPANDER, chance(50U));
        break;
    case 3:
        sim_blip();
        break;
    case 4: {
        int const chip = (int)below(2U);
        s_chip_mute[chip] = !s_chip_mute[chip];
        if (s_chip_mute[chip]) {
            if (chance(50U)) {
                sim_firmware(chip, NULL);
            } else {
                sim_firmware_quiet(chip, NULL);
            }
        } else {
            sim_firmware(chip, (chip == SIM_C5) ? SIM_C5_BANNER_20 : SIM_NRF_BANNER_20);
        }
        break;
    }
    case 5:
        sim_gnss_take_bus();
        break;
    case 6:
        start_flash();
        break;
    case 7:
        sim_ao_home();
        break;
    case 8:
        sim_ao_sleep();
        break;
    case 9:
        sim_chip_boot((int)below(2U), 50U + below(300U));
        break;
    case 10:
        sim_sleep(below(40U));
        break;
    case 11:
        sim_clock_skip_ms(below(2U) ? 3600U * 1000U : 70U * 1000U);
        break;
    case 12:
        sim_exp_power_answers(chance(80U) ? KODE_OK : KODE_ERROR);
        break;
    case 13:
        sim_gnss_standard_fw(chance(50U));
        break;
    default: {
        /* The receiver says where it is, whether anyone asked or not. */
        gnss_feed();
        break;
    }
    }
    trace(what, 0);
    s_now = "";
    /* The world changed: it records no emission, and a carrier is cut at once on the way home. */
    check_marks(what, 0U, KODE_OK, true);
    if (r == 7U || r == 8U) {
        uint32_t const until = sim_now_ms() + 500U;
        while (sim_cc1101.on_air && (int32_t)(sim_now_ms() - until) < 0) {
            sim_sleep(5U);
        }
        CHECKF(!sim_cc1101.on_air, "%s: the carrier is still on the air half a second after the way home", what);
    }
}

/* ---- The case -------------------------------------------------------------------------------- */

static void wait_helpers(uint32_t ms)
{
    uint32_t const until = sim_now_ms() + ms;
    while (s_helpers != 0U && (int32_t)(sim_now_ms() - until) < 0) {
        sim_sleep(10U);
    }
}

/* What an app does when the module is not there: it waits, gives the supply back, plugs it in. */
static void repair(void)
{
    s_now = "repair";
    sim_exp_power_answers(KODE_OK);
    for (int chip = 0; chip < 2; chip++) {
        if (s_chip_mute[chip] && chance(60U)) {
            s_chip_mute[chip] = false;
            sim_firmware(chip, (chip == SIM_C5) ? SIM_C5_BANNER_20 : SIM_NRF_BANNER_20);
        }
    }
    if (chance(50U)) {
        (void)c_end();
    }
    (void)c_begin();
    sim_plug(SIM_DEV_EXPANDER, chance(50U));
    (void)sim_wait_present(2500U);
    trace("repair", 0);
    s_now = "";
    check_marks("repair", 0U, KODE_OK, true);
}

static unsigned s_stats[CALLS_N_MAX][9];

/* Why each call refused, by the text it gave: to see that the states are reached. */
typedef struct {
    size_t      call;
    const char *why;
    unsigned    n;
} why_t;

static why_t  s_whys[600];
static size_t s_whys_n;

static void why_count(size_t call, const char *why)
{
    for (size_t i = 0U; i < s_whys_n; i++) {
        if (s_whys[i].call == call && s_whys[i].why == why) {
            s_whys[i].n++;
            return;
        }
    }
    if (s_whys_n < sizeof(s_whys) / sizeof(s_whys[0])) {
        s_whys[s_whys_n++] = (why_t){ call, why, 1U };
    }
}

static void print_stats(void)
{
    (void)printf("ANY-STATS seed %08x: call: OK ERROR TIMEOUT DENIED NO_RES IO_ERR EXISTS\n", (unsigned)s_seed);
    for (size_t i = 0U; i < s_whys_n; i++) {
        (void)printf("ANY-WHY %-24s %4u %.120s\n", k_calls[s_whys[i].call].name, s_whys[i].n, s_whys[i].why);
    }
    for (size_t i = 0U; i < CALLS_N; i++) {
        (void)printf("ANY-STATS %-28s %5u %5u %5u %5u %5u %5u %5u\n", k_calls[i].name, s_stats[i][0], s_stats[i][1],
                     s_stats[i][2], s_stats[i][3], s_stats[i][4], s_stats[i][5], s_stats[i][6]);
    }
}

static void run_fuzz(uint32_t seed, unsigned iterations)
{
    (void)memset(s_stats, 0, sizeof(s_stats));
    s_whys_n = 0U;
    s_seed       = seed;
    s_rng        = seed;
    s_trace_n    = 0U;
    s_generation = 0U;
    s_events     = 0U;
    s_bad_event  = 0U;

    sim_begin_fck(NULL, NULL);
    sim_script(SIM_C5, "gnss on", "#DONE gnss rc=0\r\n", 5U);
    sim_script(SIM_C5, "gnss off", "#DONE gnss rc=0\r\n", 5U);
    sim_script(SIM_C5, "scan", "#DONE scan rc=0\r\n", 5U);
    sim_script(SIM_C5, "chan", "#DONE chan rc=0\r\n", 5U);
    sim_script(SIM_C5, "ble", "#DONE ble rc=0\r\n", 5U);
    sim_script(SIM_NRF, "rf power", "#POWER24 req=0 dbm=0\r\n#DONE rf_power rc=0\r\n", 5U);
    sim_script(SIM_NRF, "rf ping",
               "#PINGDONE24 ch=40 sent=10 ok=9 fwd_mean=-40.0 rev_mean=-30.0 mhz=2440 dbm=0 "
               "timeout=1 crcbad=0 mismatch=0 txfail=0 fwd_min=-45 fwd_max=-35 "
               "rev_min=-35 rev_max=-25 peer=0A peer_dbm=-4 txslow=0\r\n#DONE rf_ping rc=0\r\n", 10U);
    sim_script(SIM_NRF, "rf sniff",
               "#SNIFFTOP24 i=0 cnt=3 rssi=-40 len=2 data=0a1b\r\n"
               "#SNIFFDONE24 ch=76 rx=5 gated=1 uniq=1 ms=1000\r\n#DONE rf_sniff rc=0\r\n", 10U);
    sim_script(SIM_NRF, "rf echo on", "#ECHO24 on=1 ch=2 mhz=2402 dbm=-20 node=15\r\n#DONE rf_echo rc=0\r\n", 5U);
    sim_script(SIM_NRF, "rf spec", "#SPEC24 f0=2402 step=1 n=3 sweep=1 v=-90,-60,-90\r\n#DONE rf_spec rc=0\r\n", 10U);
    sim_script(SIM_NRF, "rf echo", "#ECHOSTAT24 on=0 ch=2 rx=0 crcbad=0 replied=0 foreign=0\r\n#DONE rf_echo rc=0\r\n", 5U);
    sim_script(SIM_NRF, "rf off", "#DONE rf_off rc=0\r\n", 5U);
    sim_cc1101_reset();
    sim_cc1101.reads_ok = true;
    sim_sent_clear(SIM_C5);
    sim_sent_clear(SIM_NRF);
    sim_writers_reset();
    s_flash_running     = false;
    s_flash_done_unseen = false;
    s_flash_bad_rc      = 0;
    s_chip_mute[0] = s_chip_mute[1] = false;
    arms_init();
    sim_on_event(count_cb, NULL);
    s_marks = marks_now();

    for (s_iter = 0U; s_iter < iterations; s_iter++) {
        if (s_iter % 100U == 0U) {
            (void)printf("ANY-PROGRESS iteration %u at %u ms\n", s_iter, (unsigned)sim_now_ms());
        }
        if (chance(6U)) {
            world_step();
            continue;
        }
        if (!fck_is_present() && chance(25U)) {
            repair();
            continue;
        }
        const call_t *const c = &k_calls[below((uint32_t)CALLS_N)];
        s_all_good = chance(60U);
        /* A chip that says nothing is waited for 7.5 s: end the wait at a random moment, as an app would. */
        if (s_chip_mute[0] || s_chip_mute[1] || s_flash_running) {
            cancel_later(15U + below(40U));
        }
        s_now = c->name;
        kode_status_t const rc = c->fn();
        s_now = "";
        trace(c->name, (int)rc);
        if ((unsigned)rc < 9U) {
            s_stats[c - k_calls][rc]++;
        }
        CHECKF(((unsigned)rc < 32U) && ((c->allowed & B(rc)) != 0U), "%s answered %d, which its @return does not list", c->name, (int)rc);
        if (rc != KODE_OK) {
            const char *const why = fck_error();
            if (why != NULL) {
                why_count((size_t)(c - k_calls), why);
            }
            CHECKF((why != NULL && why[0] != '\0') || c->allowed == M_PURE, "%s answered %d with no reason in fck_error()", c->name, (int)rc);
        }
        check_marks(c->name, c->asks, rc, false);
        CHECKF(s_bad_event == 0U, "the callback was told an event that is none (%u)", s_bad_event);
        CHECKF(s_flash_bad_rc == 0, "a flash in another task answered a code its @return does not list (%d)", s_flash_bad_rc);
        CHECKF(sim_sent(SIM_NRF, "rf cw") == 0U, "an nRF line `rf cw` reached the chip (%d)", 0);
        /* Every so often, the state in one piece. */
        if (below(10U) == 0U) {
            fck_status_t st;
            CHECKF(fck_core_status(&st) == KODE_OK, "fck_core_status() did not answer OK (%d)", 0);
            CHECKF(!(st.gnss_running && !st.present), "the receiver runs with no module (%d)", 0);
        }
    }

    print_stats();
    /* The end: everything let go, and nothing left behind. */
    wait_helpers(3000U);
    CHECKF(s_helpers == 0U, "%u helper task(s) never finished", s_helpers);
    sim_on_event(NULL, NULL);
    sim_exp_power_answers(KODE_OK);
    TEST_ASSERT_EQUAL_MESSAGE(KODE_OK, fck_core_end(), story());
    CHECK(!sim_exp_power_on(), "after fck_core_end() J3's supply is on");
    CHECK(!fck_is_present(), "after fck_core_end() the module is present");
    sim_cc1101.play_ms = 0U;
    TEST_ASSERT_EQUAL_MESSAGE(KODE_OK, fck_core_begin(), story());
    /* The supply is asked for at the owner's first tick: it follows. */
    sim_sleep(400U);
    CHECK(sim_exp_power_on(), "after fck_core_begin() J3's supply is off");
}

TEST_CASE("any call in any state: good, NULL and out-of-range arguments in random order, the world changing", "[any][slow]")
{
    /* FCK_ANY_SEED and FCK_ANY_ITERATIONS: to replay a failure, or to look further. */
    const char *const seed = getenv("FCK_ANY_SEED");
    const char *const iter = getenv("FCK_ANY_ITERATIONS");
    run_fuzz((seed != NULL) ? (uint32_t)strtoul(seed, NULL, 0) : 0xF0C4A11CU,
             (iter != NULL) ? (unsigned)strtoul(iter, NULL, 0) : 1600U);
}

TEST_CASE("any call in any state: a second seed, and the module model never records what nobody asked for", "[any][slow]")
{
    run_fuzz(0x5EED0511U, 1000U);
}
