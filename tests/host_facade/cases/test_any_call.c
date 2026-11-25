/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_any_call.c
 * @brief Any call of the facade in any state, in a random order from a fixed seed, over an owner
 *        that answers what it is told to.
 *
 * Between calls the owner changes: the module absent or present, a chip that says nothing of
 * what it runs, a firmware of another make, a firmware before 2.0, the receiver on, a chip kept
 * by a flash, the way home, and every answer an owner can give to every request (the codes a
 * real owner answers: OK, DENIED, TIMEOUT, IO_ERROR, NO_RESOURCES). Every public call that the
 * facade links is made with good arguments, with NULL ones and with ones out of range. What must
 * hold:
 *
 *  - nothing asserts, and nothing is written past what a caller gave;
 *  - every code a call answers is one its @return lists;
 *  - a call made with an argument out of range answers KODE_DENIED, says why in fck_error() and
 *    has not asked the owner for anything;
 *  - no line that transmits reaches a chip that the SDK's gate keeps quiet: no `rf cw`, no `rf
 *    power` above 6 dBm, no `scan` to a C5 whose firmware does not keep the radio limits itself (a
 *    firmware of another make is its programmer's);
 *  - a call that fails writes none of its outputs.
 *
 * The calls that need the real owner (flash, the rescue probe, sub-GHz) are in tests/host_owner.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "unity.h"

#include "fck.h"
#include "fck_core.h"
#include "fck_aos.h"

#include "fake_fck_aos.h"

#define C5_20    "fw=c5_fck_agent ver=2.0 chip=esp32c5 rev=1.2 mac=aa:bb:cc:dd:ee:01 reg=EU lim=ok"
#define C5_15    "fw=c5_fck_agent ver=1.5 chip=esp32c5 rev=1.2 mac=aa:bb:cc:dd:ee:02"
#define C5_OWN   "fw=my_firmware ver=0.3"
#define NRF_20   "fw=nrf_fck_agent ver=2.0 devid=0123456789ABCDEF reset=0x0 uptime_ms=1"
#define NRF_10   "fw=nrf_fck_agent ver=1.0 devid=0123456789ABCD15"
#define NRF_OWN  "fw=my_firmware ver=0.3"

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

#define PICK(arr) ((arr)[below((uint32_t)(sizeof(arr) / sizeof((arr)[0])))])

typedef enum { A_GOOD = 0, A_NULL, A_BAD } arg_t;

static bool s_all_good;
/* Set by a wrapper when it made an argument the facade must refuse before the owner. */
static bool s_refusable;

static arg_t kind(void)
{
    if (s_all_good) {
        return A_GOOD;
    }
    uint32_t const r = below(100U);
    arg_t const    k = (r < 30U) ? A_GOOD : ((r < 60U) ? A_NULL : A_BAD);
    if (k != A_GOOD) {
        s_refusable = true;
    }
    return k;
}

/** A pointer an optional out argument may leave out. */
#define OPT(p) ((s_all_good || chance(70U)) ? (p) : NULL)

/* ---- The story, for a failure ---------------------------------------------------------- */

#define TRACE_N 16U

static char       s_trace[TRACE_N][64];
static unsigned   s_trace_n;
static uint32_t   s_seed;
static unsigned   s_iter;
static const char *s_now = "";

static void trace(const char *what, int rc)
{
    (void)snprintf(s_trace[s_trace_n % TRACE_N], sizeof(s_trace[0]), "%s -> %d", what, rc);
    s_trace_n++;
}

static const char *story(void)
{
    static char text[TRACE_N * 70U + 120U];
    int         n = snprintf(text, sizeof(text), "seed %08x iteration %u, in %s; last operations:", (unsigned)s_seed,
                             s_iter, s_now);
    unsigned    from = (s_trace_n > TRACE_N) ? s_trace_n - TRACE_N : 0U;
    for (unsigned i = from; i < s_trace_n && n < (int)sizeof(text) - 70; i++) {
        n += snprintf(text + n, sizeof(text) - (size_t)n, " [%s]", s_trace[i % TRACE_N]);
    }
    return text;
}

#define CHECKF(cond, fmt, ...)                                                      \
    do {                                                                            \
        if (!(cond)) {                                                              \
            static char m_[1800];                                                   \
            (void)snprintf(m_, sizeof(m_), fmt " | %s", __VA_ARGS__, story());      \
            (void)printf("ANY-FAIL: %s\n", m_);                                     \
            TEST_FAIL_MESSAGE("see ANY-FAIL above");                                \
        }                                                                           \
    } while (0)

/* ---- The owner's state: what it answers ------------------------------------------------ */

/** What each chip announced, so that the checks know what the gate was shown. */
typedef enum { AG_NONE = 0, AG_20, AG_OLD, AG_OWN } firmware_kind_t;

static firmware_kind_t s_firmware_kind[FCK_AOS_CHIPS];
static uint32_t     s_generation;

static kode_status_t status_rc(void)
{
    static const kode_status_t v[] = { KODE_OK, KODE_OK, KODE_OK, KODE_OK, KODE_DENIED, KODE_TIMEOUT, KODE_IO_ERROR,
                                       KODE_NO_RESOURCES };
    return PICK(v);
}

static void chip_firmware(fck_aos_chip_t chip)
{
    uint32_t const r = below(10U);
    if (r < 2U) {
        fake_fck_aos_firmware(chip, NULL);
        g_aos.firmware_rc[chip] = chance(50U) ? KODE_TIMEOUT : KODE_DENIED;
        s_firmware_kind[chip]   = AG_NONE;
        return;
    }
    g_aos.firmware_rc[chip] = chance(90U) ? KODE_OK : status_rc();
    firmware_kind_t const k = (r < 6U) ? AG_20 : ((r < 8U) ? AG_OLD : AG_OWN);
    if (chip == FCK_AOS_C5) {
        fake_fck_aos_firmware(chip, (k == AG_20) ? C5_20 : ((k == AG_OLD) ? C5_15 : C5_OWN));
    } else {
        fake_fck_aos_firmware(chip, (k == AG_20) ? NRF_20 : ((k == AG_OLD) ? NRF_10 : NRF_OWN));
    }
    s_firmware_kind[chip] = k;
}

/* A little of what a firmware says back, good and not. */
static const fake_reply_t k_replies[][3] = {
    { { "AP", "ssid=Home bssid=aa:bb:cc:dd:ee:01 ch=6 rssi=-50 sec=wpa2_psk" }, { NULL, NULL }, { NULL, NULL } },
    { { "CH", "ch=1 aps=3 rssi=-60" }, { "BLE", "addr=aa:bb:cc:dd:ee:02 rssi=-70 name=x" }, { NULL, NULL } },
    { { "GARBAGE", "no keys at all" }, { "AP", "ssid=" }, { NULL, NULL } },
    { { NULL, NULL }, { NULL, NULL }, { NULL, NULL } },
};

static void new_world(void)
{
    uint32_t const gen = s_generation + 1U;
    fake_fck_aos_reset();
    bool const present = chance(80U);
    g_aos.present        = present;
    g_aos.st.present     = present;
    g_aos.st.generation  = gen;
    s_generation         = gen;
    g_aos.st.gnss_running = present && chance(30U);
    g_aos.st.chip_on[FCK_AOS_C5]  = present && chance(90U);
    g_aos.st.chip_on[FCK_AOS_NRF] = present && chance(90U);
    g_aos.st.cc1101_ok            = present && chance(90U);
    g_aos.pos_ret = (uint8_t)below(15U);
    g_aos.start_rc = chance(80U) ? KODE_OK : PICK(((const kode_status_t[]){ KODE_EXISTS, KODE_DENIED, KODE_NO_RESOURCES, KODE_ERROR }));
    g_aos.stop_rc  = chance(85U) ? KODE_OK : PICK(((const kode_status_t[]){ KODE_DENIED, KODE_NO_RESOURCES }));
    g_aos.cancel_rc = chance(90U) ? KODE_OK : KODE_DENIED;
    g_aos.wait_rc   = chance(60U) ? KODE_OK : PICK(((const kode_status_t[]){ KODE_TIMEOUT, KODE_DENIED }));
    g_aos.command_rc = chance(70U) ? KODE_OK : status_rc();
    g_aos.command_out = (int32_t)below(4U) - 1;
    const fake_reply_t *const rep = k_replies[below(4U)];
    for (unsigned i = 0U; i < 3U; i++) {
        g_aos.replies[i] = rep[i];
    }
    g_aos.hold_rc       = chance(90U) ? KODE_OK : status_rc();
    g_aos.power_rc      = chance(85U) ? KODE_OK : status_rc();
    g_aos.reset_rc      = chance(85U) ? KODE_OK : status_rc();
    g_aos.gnss_power_rc = chance(85U) ? KODE_OK : status_rc();
    g_aos.gnss_fix_rc   = chance(60U) ? KODE_OK : status_rc();
    g_aos.fix.valid     = chance(70U);
    g_aos.fix.lat       = 48.1173;
    g_aos.fix.lon       = 11.5166;
    chip_firmware(FCK_AOS_C5);
    chip_firmware(FCK_AOS_NRF);
    if (chance(40U)) {
        /* A module in good health: both firmwares 2.0, and every answer OK. */
        g_aos.present = g_aos.st.present = true;
        g_aos.start_rc = g_aos.stop_rc = g_aos.cancel_rc = g_aos.wait_rc = g_aos.command_rc = KODE_OK;
        g_aos.hold_rc = g_aos.power_rc = g_aos.reset_rc = g_aos.gnss_power_rc = g_aos.gnss_fix_rc = KODE_OK;
        g_aos.firmware_rc[FCK_AOS_C5] = g_aos.firmware_rc[FCK_AOS_NRF] = KODE_OK;
        g_aos.command_out = 0;
        fake_fck_aos_firmware(FCK_AOS_C5, C5_20);
        fake_fck_aos_firmware(FCK_AOS_NRF, NRF_20);
        s_firmware_kind[FCK_AOS_C5]  = AG_20;
        s_firmware_kind[FCK_AOS_NRF] = AG_20;
    }
    if (chance(15U)) {
        g_aos.cancel_after = 1U + below(4U);      /* an fck_core_cancel() lands while a long call runs */
    }
    g_aos.why = NULL;
}

/* ---- Arguments -------------------------------------------------------------------------- */

static fck_chip_t chip_arg(void)
{
    uint32_t const r = below(100U);
    if (!s_all_good && r >= 90U) {
        s_refusable = true;
        return (fck_chip_t)(2 + (int)below(200U));
    }
    return (r < 50U) ? FCK_C5 : FCK_NRF;
}

static uint32_t ms_short(void)
{
    static const uint32_t v[] = { 0U, 5U, 20U, 40U, 1000U, 60000U };
    return PICK(v);
}

static size_t cap_arg(arg_t k, size_t good)
{
    static const size_t bad[] = { 0U, 1U };
    return (k == A_BAD) ? PICK(bad) : good;
}

static fck_wifi_network_t s_net[FCK_WIFI_NETWORK_MAX];
static fck_wifi_channel_t s_chan[FCK_WIFI_CHANNELS_CAP_MAX];
static fck_ble_device_t   s_ble[FCK_BLE_DEVICE_MAX];
static fck_wifi_frame_t   s_wifi_frames[FCK_WIFI_FRAME_MAX];
static int8_t             s_dbm8[256];
static fck_radio_frame_t  s_frames[FCK_RADIO_FRAMES_MAX];

/* ---- The calls --------------------------------------------------------------------------- */

typedef kode_status_t (*call_fn)(void);

typedef struct {
    const char *name;
    call_fn     fn;
    uint32_t    allowed;
} call_t;

#define B(c) (1U << (c))
/* What an owner can answer a request with: the rest, a pool with no room (nothing was done). */
#define M_STD (B(KODE_OK) | B(KODE_DENIED) | B(KODE_TIMEOUT) | B(KODE_IO_ERROR) | B(KODE_NO_RESOURCES))
#define M_PURE (B(KODE_OK) | B(KODE_DENIED))

/* A result that was not written: the sentinel a failed call must leave alone. */
#define SENTINEL 0x5A

static kode_status_t c_init(void)
{
    return fck_core_begin();
}

static kode_status_t c_end(void)
{
    return fck_core_end();
}

static kode_status_t c_error(void)
{
    TEST_ASSERT_NOT_NULL(fck_error());
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

static kode_status_t c_status(void)
{
    fck_status_t st;
    memset(&st, SENTINEL, sizeof(st));
    if (kind() == A_NULL) {
        return fck_core_status(NULL);
    }
    kode_status_t const rc = fck_core_status(&st);
    CHECKF(rc == KODE_OK, "fck_core_status() did not answer OK (%d)", (int)rc);
    CHECKF(st.generation == g_aos.st.generation, "the generation is not the owner's (%u)", (unsigned)st.generation);
    return rc;
}

static kode_status_t c_header_position(void)
{
    uint8_t const r = fck_header_position((uint8_t)below(24U));
    CHECKF(r <= 14U, "a position J3 does not have (%u)", (unsigned)r);
    return KODE_OK;
}

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
    static const char *const verbs[] = { NULL, "info", "scan", "nope", "" };
    static char        longer[400];
    arg_t const        k = kind();
    const char        *line;
    if (k == A_NULL) {
        line = NULL;
    } else if (k == A_BAD && chance(50U)) {
        (void)memset(longer, 'x', sizeof(longer) - 1U);
        longer[sizeof(longer) - 1U] = '\0';
        line = longer;
    } else {
        line = PICK(lines);
    }
    /* A line the gate may refuse or take is not one the facade must refuse before the owner: only
     * a NULL line or a chip that is none is. */
    int32_t rcv = 7;
    return fck_core_command(chip_arg(), line, PICK(verbs), chance(50U) ? line_fn : NULL, NULL, ms_short(), OPT(&rcv));
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
    memset(&a, SENTINEL, sizeof(a));
    arg_t const k = kind();
    return fck_core_firmware(chip_arg(), (k == A_NULL) ? NULL : &a);
}

static fck_band_t band_arg(arg_t k)
{
    return (k == A_BAD) ? (fck_band_t)(3 + (int)below(100U)) : (fck_band_t)below(3U);
}

static kode_status_t c_wifi_scan(void)
{
    arg_t const k = kind();
    size_t      n = 5U;
    size_t const cap = (k == A_BAD && chance(50U)) ? SIZE_MAX : cap_arg(k, FCK_WIFI_NETWORK_MAX);
    return fck_core_wifi_scan(band_arg(k), (k == A_NULL) ? NULL : s_net, cap, OPT(&n));
}

static kode_status_t c_wifi_channels(void)
{
    arg_t const k = kind();
    size_t      n = 5U;
    static const uint16_t dwell[] = { 20U, 100U, 500U };
    size_t const cap = (k == A_BAD && chance(40U)) ? FCK_WIFI_CHANNELS_CAP_MAX + 1U
                                                    : ((k == A_BAD && chance(40U)) ? SIZE_MAX : cap_arg(k, FCK_WIFI_CHANNELS_CAP_MAX));
    return fck_core_wifi_channels(band_arg(k), (k == A_BAD && chance(30U)) ? 0U : PICK(dwell), (k == A_NULL) ? NULL : s_chan, cap,
                             OPT(&n));
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
    size_t      n = 5U;
    static const uint8_t good[] = { 1U, 2U, 5U, 60U };
    static const uint8_t bad[]  = { 0U, 61U, 255U };
    size_t const cap = (k == A_BAD && chance(50U)) ? SIZE_MAX : cap_arg(k, FCK_BLE_DEVICE_MAX);
    return fck_core_ble_scan((k == A_BAD && chance(50U)) ? PICK(bad) : PICK(good), (k == A_NULL) ? NULL : s_ble, cap, OPT(&n));
}

static kode_status_t c_gnss_begin(void)
{
    return fck_core_gnss_begin();
}

static kode_status_t c_gnss_read(void)
{
    fck_gnss_fix_t fix;
    memset(&fix, SENTINEL, sizeof(fix));
    return fck_core_gnss_read((kind() == A_NULL) ? NULL : &fix, ms_short());
}

static kode_status_t c_gnss_end(void)
{
    return fck_core_gnss_end();
}

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
    size_t      n = 3U;
    uint16_t    first = 2402U;
    uint16_t    last  = (uint16_t)(2402U + below(30U));
    if (k == A_BAD) {
        first = PICK(((const uint16_t[]){ 0U, 2399U, 2501U, 2450U }));
        last  = PICK(((const uint16_t[]){ 0U, 2400U, 2501U, 65535U }));
    }
    return fck_core_radio_spectrum(first, last, (k == A_BAD && chance(30U)) ? 0U : 100U,
                              (uint16_t)((k == A_BAD && chance(30U)) ? 0U : 1U + below(2U)), (k == A_NULL) ? NULL : s_dbm8,
                              cap_arg(k, sizeof(s_dbm8)), OPT(&n));
}

static kode_status_t c_radio_sniff(void)
{
    arg_t const k = kind();
    size_t      n = 3U;
    fck_radio_sniff_statistics_t st;
    static const uint32_t ms[] = { 200U, 500U, 1000U };
    return fck_core_radio_sniff(mhz_arg(k), (k == A_BAD && chance(30U)) ? 0xFFFFFFFFU : PICK(ms), (int8_t)-90,
                           (k == A_NULL) ? NULL : s_frames, cap_arg(k, FCK_RADIO_FRAMES_MAX), OPT(&n), OPT(&st));
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
    return fck_core_radio_echo_stop(OPT(&st));
}

#define CALL(fn, mask) { #fn, c_##fn, (mask) }

static const call_t k_calls[] = {
    CALL(init, B(KODE_OK) | B(KODE_EXISTS) | B(KODE_DENIED) | B(KODE_NO_RESOURCES) | B(KODE_ERROR)),
    CALL(end, B(KODE_OK) | B(KODE_DENIED) | B(KODE_NO_RESOURCES)),
    CALL(error, M_PURE),
    CALL(cancel, M_PURE),
    CALL(is_present, M_PURE),
    CALL(wait_present, B(KODE_OK) | B(KODE_TIMEOUT) | B(KODE_DENIED)),
    CALL(status, M_PURE),
    CALL(header_position, M_PURE),
    CALL(command, M_STD),
    CALL(reset, M_STD),
    CALL(power, M_STD),
    CALL(firmware, M_STD),
    CALL(wifi_scan, M_STD),
    CALL(wifi_channels, M_STD),
    CALL(wifi_frames, M_STD),
    CALL(ble_scan, M_STD),
    CALL(gnss_begin, M_STD),
    CALL(gnss_read, M_STD),
    CALL(gnss_end, M_STD),
    CALL(radio_spectrum, M_STD),
    CALL(radio_sniff, M_STD),
    CALL(radio_sniff_tune, M_STD),
    CALL(radio_ping, M_STD),
    CALL(radio_echo_start, M_STD),
    CALL(radio_echo_stop, M_STD),
};

#define CALLS_N (sizeof(k_calls) / sizeof(k_calls[0]))

/* ---- What the gate let through ---------------------------------------------------------- */

static void check_line(const char *what)
{
    if (g_aos.commands == 0U) {
        return;
    }
    const char *const l = g_aos.cmd_line;
    bool const        nrf = (g_aos.cmd_chip == FCK_AOS_NRF);
    firmware_kind_t const kind_ = s_firmware_kind[nrf ? FCK_AOS_NRF : FCK_AOS_C5];
    if (kind_ == AG_OWN) {
        return;     /* another firmware's: as written, and its programmer's */
    }
    if (nrf) {
        CHECKF(strncmp(l, "rf cw", 5) != 0, "%s: `rf cw` reached the nRF's firmware", what);
        if (strncmp(l, "rf power ", 9) == 0) {
            CHECKF(atoi(l + 9) <= FCK_RADIO_TRANSMIT_DBM_MAX, "%s: `%s` went to the nRF", what, l);
        }
    } else if (strncmp(l, "scan", 4) == 0) {
        CHECKF(kind_ == AG_20, "%s: `scan` went to a C5 whose firmware does not keep the radio limits", what);
    }
}

/* ---- The case --------------------------------------------------------------------------- */

static unsigned s_stats[64][9];

static void run_fuzz(uint32_t seed, unsigned iterations)
{
    s_seed       = seed;
    s_rng        = seed;
    s_trace_n    = 0U;
    s_generation = 0U;
    (void)memset(s_stats, 0, sizeof(s_stats));
    new_world();

    for (s_iter = 0U; s_iter < iterations; s_iter++) {
        if (chance(12U)) {
            s_now = "world";
            new_world();
            trace("world", 0);
            continue;
        }
        const call_t *const c = &k_calls[below((uint32_t)CALLS_N)];
        s_all_good  = chance(60U);
        s_refusable = false;
        s_now       = c->name;
        g_aos.why   = NULL;
        unsigned const owner_calls = g_aos.n_calls;
        unsigned const commands    = g_aos.commands;
        kode_status_t const rc     = c->fn();
        s_now                      = "";
        trace(c->name, (int)rc);
        if ((unsigned)rc < 9U) {
            s_stats[c - k_calls][rc]++;
        }
        CHECKF((unsigned)rc < 32U && ((c->allowed & B(rc)) != 0U), "%s answered %d, which its @return does not list", c->name, (int)rc);
        CHECKF(fck_error() != NULL, "%s: fck_error() answered NULL", c->name);
        /* The facade's own refusals say why, and never bother the owner. */
        if (s_refusable && !s_all_good && rc == KODE_DENIED && g_aos.n_calls == owner_calls) {
            CHECKF(fck_error()[0] != '\0', "%s refused an argument and said nothing in fck_error()", c->name);
        }
        if (g_aos.commands != commands) {
            check_line(c->name);
        }
        /* The way out of a cancel that landed while the call ran: the next call is not touched by it. */
        g_aos.cancel_after = 0U;
    }
    (void)printf("ANY-FACADE seed %08x: %u iterations\n", (unsigned)seed, iterations);
    for (size_t i = 0U; i < CALLS_N; i++) {
        (void)printf("ANY-STATS %-20s %5u %5u %5u %5u %5u %5u %5u\n", k_calls[i].name, s_stats[i][0], s_stats[i][1],
                     s_stats[i][2], s_stats[i][3], s_stats[i][4], s_stats[i][5], s_stats[i][6]);
    }
}

TEST_CASE("any call in any state: good, NULL and out-of-range arguments in random order, the owner answering anything", "[any]")
{
    run_fuzz(0xFAC4DE01U, 6000U);
}

TEST_CASE("any call in any state: a second seed", "[any]")
{
    run_fuzz(0x0511BEEFU, 4000U);
}
