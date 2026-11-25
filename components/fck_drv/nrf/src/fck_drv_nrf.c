/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file fck_drv_nrf.c
 * @brief The nRF firmware's vocabulary. See fck_drv_nrf.h.
 *
 * Plain C, like fck_drv_link.c: no ESP-IDF and no FreeRTOS, so tests/host runs
 * it as it is (tools/check_layers.sh, rule 4). Every name and number below
 * is the firmware's, from firmware/fck-nrf (rf.c, main.c, prj.conf).
 */
#include "fck_drv_nrf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fck_bsp.h"

/* The firmware counts channels from here: `ch` = MHz - 2400. */
#define CH_BASE_MHZ     2400U

/* The arguments the firmware takes after a sub-command (FCK_ARGS_MAX). One
 * more and it answers rc 2; a line with more is never built. */
#define ARGS_SUB_MAX    8U

/* Read in rf.c: per channel of `rf spec`, the radio disabled,
 * retuned and ramped up (fast ramp-up, about 40 us) and the RSSI settled
 * (15 us), on top of the dwell. Per packet of `rf ping`, the transmission
 * (cut about 0.5 ms after it starts if the radio does not end it) and the wait for the echo (PING_RX_TIMEOUT_MS 20), on
 * top of the gap. `rf sniff` checks its deadline every 100 ms. */
#define SPEC_HOP_US     100U
#define PING_PKT_MS     25U
#define SNIFF_SLICE_MS  100U

/* The longest line each command prints, for the time the UART takes to
 * carry it: a #SPEC24 of n values, a #PING24, a #SNIFFTOP24 of 40 bytes. */
#define SPEC_HEAD_BYTES 48U
#define SPEC_VAL_BYTES  5U
#define PING_LINE_BYTES 64U
#define SNIFF_LINE_BYTES 120U

/* A version as one number, for comparing: 2.0 is 0x200. Only an image that
 * prints no caps= is told by one (fck_drv_nrf_is_unlimited()). */
#define VER(major, minor)  (((unsigned)(major) << 8) | (unsigned)(minor))

/* The words of caps= this file knows: the firmware's FCK_NRF_CAPS. */
static const struct {
    const char *word;
    uint16_t    bit;
} s_caps[] = {
    { "spec", FCK_DRV_NRF_CAP_SPEC },   { "off", FCK_DRV_NRF_CAP_OFF },
    { "power", FCK_DRV_NRF_CAP_POWER }, { "echo", FCK_DRV_NRF_CAP_ECHO },
    { "ping", FCK_DRV_NRF_CAP_PING },   { "sniff", FCK_DRV_NRF_CAP_SNIFF },
    { "scfg", FCK_DRV_NRF_CAP_SCFG },   { "saddr", FCK_DRV_NRF_CAP_SADDR },
    { "eu", FCK_DRV_NRF_CAP_EU },       { "done", FCK_DRV_NRF_CAP_DONE },
    { "wdt", FCK_DRV_NRF_CAP_WDT },     { "echoidle", FCK_DRV_NRF_CAP_ECHOIDLE },
    { "smp", FCK_DRV_NRF_CAP_SMP },
};

/* The firmware's sub-commands, cw included: it has one, and a builder must
 * know it to refuse it. In the order the shell lists them. */
static const char *const s_sub[] = {
    "spec", "cw", "off", "power", "echo", "ping", "sniff", "scfg", "saddr",
};

/* ---- Commands ---------------------------------------------------------------- */

/* The n-th space-separated word of @p s, from 0: where it starts, its length
 * in *len. NULL when there is none. */
static const char *word(const char *s, unsigned n, size_t *len)
{
    for (;;) {
        while (*s == ' ') {
            s++;
        }
        if (*s == '\0') {
            return NULL;
        }
        size_t l = strcspn(s, " ");
        if (n == 0U) {
            *len = l;
            return s;
        }
        n--;
        s += l;
    }
}

static bool word_is(const char *w, size_t len, const char *lit)
{
    return strlen(lit) == len && memcmp(w, lit, len) == 0;
}

/* A number written so plainly that this file and the firmware cannot read it
 * apart: an optional '-', then 1 to 6 decimal digits, no leading zero. The
 * firmware reads with strtol, base 0, and no limit on the length: "010" is 8
 * to it, "0x10" 16, and a word too long for a buffer here still a number
 * there. A word less plain than this, where it could change what the radio
 * does, is refused rather than guessed at. */
#define PLAIN_DIGITS_MAX 6U

static bool plain(const char *w, size_t len)
{
    size_t const i = (len > 0U && w[0] == '-') ? 1U : 0U;
    size_t const d = len - i;
    if (d == 0U || d > PLAIN_DIGITS_MAX || (w[i] == '0' && d > 1U)) {
        return false;
    }
    for (size_t k = i; k < len; k++) {
        if (w[k] < '0' || w[k] > '9') {
            return false;
        }
    }
    return true;
}

/* The n-th word, when it is a plain number. false when there is no such
 * word or it is not one. */
static bool arg(const char *text, unsigned n, long *out)
{
    size_t      len;
    const char *w = word(text, n, &len);
    if (w == NULL || !plain(w, len)) {
        return false;
    }
    char buf[PLAIN_DIGITS_MAX + 2U];
    memcpy(buf, w, len);
    buf[len] = '\0';
    *out     = strtol(buf, NULL, 10);
    return true;
}

/* Words @p first to @p last that are there are all plain numbers. */
static bool args_plain(const char *text, unsigned first, unsigned last)
{
    for (unsigned n = first; n <= last; n++) {
        size_t      len;
        const char *w = word(text, n, &len);
        if (w != NULL && !plain(w, len)) {
            return false;
        }
    }
    return true;
}

/* The n-th word, or @p def when there is none. false when it is there and
 * the firmware would refuse it: the firmware then answers at once with rc 2. */
static bool arg_or(const char *text, unsigned n, long def, long lo, long hi, long *out)
{
    size_t len;
    if (word(text, n, &len) == NULL) {
        *out = def;
        return true;
    }
    return arg(text, n, out) && *out >= lo && *out <= hi;
}

/* How long the UART takes to carry @p bytes, rounded up. */
static uint32_t wire_ms(uint32_t bytes)
{
    return (bytes * 10U * 1000U + FCK_BSP_NRF_BAUD - 1U) / FCK_BSP_NRF_BAUD;
}

/* `rf spec [ch_first] [ch_last] [dwell_us] [sweeps]`. */
static uint32_t spec_ms(const char *text)
{
    long ch0, ch1, dwell, sweeps;
    if (!arg_or(text, 2U, 0, 0, 100, &ch0) || !arg_or(text, 3U, 83, 0, 100, &ch1)
        || !arg_or(text, 4U, 200, FCK_DRV_NRF_DWELL_MIN_US, FCK_DRV_NRF_DWELL_MAX_US, &dwell)
        || !arg_or(text, 5U, 1, 1, FCK_DRV_NRF_SWEEPS_MAX, &sweeps) || ch1 < ch0) {
        return FCK_DRV_NRF_COMMAND_MS;
    }
    uint64_t n     = (uint64_t)(ch1 - ch0 + 1);
    uint64_t sweep = (n * ((uint64_t)dwell + SPEC_HOP_US) + 999U) / 1000U
                     + wire_ms(SPEC_HEAD_BYTES + (uint32_t)n * SPEC_VAL_BYTES);
    uint64_t ms    = (uint64_t)sweeps * sweep + FCK_DRV_NRF_SLACK_MS;
    return (ms > UINT32_MAX) ? UINT32_MAX : (uint32_t)ms;
}

/* `rf ping <n> [ch] [gap_ms]`. */
static uint32_t ping_ms(const char *text)
{
    long n, ch, gap;
    /* No count: the firmware refuses the line at once, as it does a count out of range. */
    if (!arg_or(text, 2U, -1, 1, FCK_DRV_NRF_PING_MAX, &n) || n < 1 || !arg_or(text, 3U, 40, 0, 80, &ch)
        || !arg_or(text, 4U, 10, 0, FCK_DRV_NRF_GAP_MAX_MS, &gap)) {
        return FCK_DRV_NRF_COMMAND_MS;
    }
    return (uint32_t)n * (PING_PKT_MS + (uint32_t)gap + wire_ms(PING_LINE_BYTES))
           + FCK_DRV_NRF_SLACK_MS;
}

/* `rf sniff <ch> [ms] [rssi_min]`: the listening, then the table. */
static uint32_t sniff_ms(const char *text)
{
    long ch, ms, rssi_min;
    if (!arg_or(text, 2U, -1, 0, 100, &ch)
        || !arg_or(text, 3U, 20000, FCK_DRV_NRF_SNIFF_MIN_MS, FCK_DRV_NRF_SNIFF_MAX_MS, &ms)
        || !arg_or(text, 4U, FCK_DRV_NRF_RSSI_MIN_DEFAULT, -127, 0, &rssi_min)) {
        return FCK_DRV_NRF_COMMAND_MS;
    }
    return (uint32_t)ms + SNIFF_SLICE_MS + wire_ms(FCK_DRV_NRF_FRAMES_MAX * SNIFF_LINE_BYTES)
           + FCK_DRV_NRF_SLACK_MS;
}

/* The channel a command would transmit on, against the module's band
 * (fck_drv_nrf.h). None is the firmware's 2440 MHz; one that is not a plain number
 * is refused, since what the firmware would make of it is not this file's to
 * guess. */
static bool tx_ch_ok(const char *text, unsigned n)
{
    size_t len;
    long   ch;
    if (word(text, n, &len) == NULL) {
        return true;
    }
    return arg(text, n, &ch) && ch >= (long)(FCK_DRV_NRF_TRANSMIT_MHZ_MIN - CH_BASE_MHZ)
           && ch <= (long)(FCK_DRV_NRF_TRANSMIT_MHZ_MAX - CH_BASE_MHZ);
}

/* A word that starts with '-' and is not a plain negative number reads as
 * an option to Zephyr's shell: `-h` or `--help` anywhere prints the help
 * and runs nothing, so no #DONE would ever come. */
static bool no_options(const char *text)
{
    size_t      len;
    const char *w;
    for (unsigned n = 0U; (w = word(text, n, &len)) != NULL; n++) {
        if (w[0] == '-' && !plain(w, len)) {
            return false;
        }
    }
    return true;
}

kode_status_t fck_drv_nrf_command(fck_drv_nrf_command_t *c, const char *text)
{
    if (c == NULL || text == NULL) {
        return KODE_DENIED;
    }
    size_t n = strlen(text);
    if (n == 0U || n > FCK_DRV_NRF_COMMAND_MAX) {
        return KODE_DENIED;
    }
    /* A line ending inside would be two commands, the second one nobody
     * waits for; a quote or a backslash would make the shell split the
     * words otherwise than here. */
    for (size_t i = 0U; i < n; i++) {
        unsigned char ch = (unsigned char)text[i];
        if (ch < 0x20U || ch > 0x7EU || ch == '"' || ch == '\'' || ch == '\\') {
            return KODE_DENIED;
        }
    }
    size_t      len;
    const char *w = word(text, 0U, &len);
    if (w == NULL || !no_options(text)) {
        return KODE_DENIED;
    }
    unsigned words = 0U;
    while (word(text, words, &len) != NULL) {
        words++;
    }

    char     verb[FCK_DRV_LINK_TAG_MAX + 1U];
    uint32_t ms    = FCK_DRV_NRF_COMMAND_MS;
    bool     emits = false;
    w = word(text, 0U, &len);
    if (word_is(w, len, "info")) {
        if (words - 1U > ARGS_SUB_MAX) {
            return KODE_DENIED;
        }
        (void)snprintf(verb, sizeof(verb), "info");
    } else if (word_is(w, len, "rf")) {
        const char *s = word(text, 1U, &len);
        int         k = -1;
        for (unsigned i = 0U; s != NULL && i < sizeof(s_sub) / sizeof(s_sub[0]); i++) {
            if (word_is(s, len, s_sub[i])) {
                k = (int)i;
            }
        }
        if (s == NULL) {
            /* `rf` alone: its usage, and #DONE rf rc=2. */
            (void)snprintf(verb, sizeof(verb), "rf");
        } else if (k < 0) {
            /* A sub-command this file does not know may be one an image
             * has that ends otherwise than the usage line, with a #DONE of
             * its own, or one the image does not have, and nothing would
             * know when it ended. */
            return KODE_DENIED;
        } else {
            const char *sub = s_sub[k];
            if (words - 2U > ARGS_SUB_MAX || strcmp(sub, "cw") == 0) {
                return KODE_DENIED;
            }
            (void)snprintf(verb, sizeof(verb), "rf_%s", sub);
            if (strcmp(sub, "spec") == 0) {
                ms = spec_ms(text);
            } else if (strcmp(sub, "ping") == 0) {
                if (!tx_ch_ok(text, 3U) || !args_plain(text, 2U, 4U)) {
                    return KODE_DENIED;
                }
                ms    = ping_ms(text);
                emits = true;
            } else if (strcmp(sub, "sniff") == 0) {
                ms = sniff_ms(text);
            } else if (strcmp(sub, "echo") == 0) {
                const char *o = word(text, 2U, &len);
                if (o != NULL && word_is(o, len, "on")) {
                    if (!tx_ch_ok(text, 3U)) {
                        return KODE_DENIED;
                    }
                    emits = true;
                }
            } else if (strcmp(sub, "power") == 0) {
                long dbm;
                if (word(text, 2U, &len) != NULL
                    && (!arg(text, 2U, &dbm) || dbm > FCK_DRV_NRF_TRANSMIT_DBM_MAX)) {
                    return KODE_DENIED;
                }
            }
        }
    } else {
        return KODE_DENIED;
    }

    memcpy(c->line, text, n);
    c->line[n]      = '\n';
    c->line[n + 1U] = '\0';
    c->len          = n + 1U;
    memcpy(c->verb, verb, strlen(verb) + 1U);
    c->ms    = ms;
    c->emits = emits;
    return KODE_OK;
}

kode_status_t fck_drv_nrf_command_info(fck_drv_nrf_command_t *c)
{
    return fck_drv_nrf_command(c, "info");
}

static bool rx_mhz_ok(uint16_t mhz)
{
    return mhz >= FCK_DRV_NRF_RECEIVE_MHZ_MIN && mhz <= FCK_DRV_NRF_RECEIVE_MHZ_MAX;
}

static bool tx_mhz_ok(uint16_t mhz)
{
    return mhz >= FCK_DRV_NRF_TRANSMIT_MHZ_MIN && mhz <= FCK_DRV_NRF_TRANSMIT_MHZ_MAX;
}

kode_status_t fck_drv_nrf_command_spectrum(fck_drv_nrf_command_t *c, uint16_t mhz_first, uint16_t mhz_last,
                               uint32_t dwell_us, uint16_t sweeps)
{
    if (!rx_mhz_ok(mhz_first) || !rx_mhz_ok(mhz_last) || mhz_last < mhz_first
        || dwell_us < FCK_DRV_NRF_DWELL_MIN_US || dwell_us > FCK_DRV_NRF_DWELL_MAX_US
        || sweeps < 1U || sweeps > FCK_DRV_NRF_SWEEPS_MAX) {
        return KODE_DENIED;
    }
    char t[48];
    (void)snprintf(t, sizeof(t), "rf spec %u %u %lu %u", (unsigned)(mhz_first - CH_BASE_MHZ),
                   (unsigned)(mhz_last - CH_BASE_MHZ), (unsigned long)dwell_us,
                   (unsigned)sweeps);
    return fck_drv_nrf_command(c, t);
}

kode_status_t fck_drv_nrf_command_off(fck_drv_nrf_command_t *c)
{
    return fck_drv_nrf_command(c, "rf off");
}

kode_status_t fck_drv_nrf_command_power(fck_drv_nrf_command_t *c, int8_t dbm)
{
    if (dbm < FCK_DRV_NRF_TRANSMIT_DBM_MIN || dbm > FCK_DRV_NRF_TRANSMIT_DBM_MAX) {
        return KODE_DENIED;
    }
    char t[24];
    (void)snprintf(t, sizeof(t), "rf power %d", (int)dbm);
    return fck_drv_nrf_command(c, t);
}

kode_status_t fck_drv_nrf_command_echo(fck_drv_nrf_command_t *c, bool on, uint16_t mhz)
{
    if (!on) {
        return fck_drv_nrf_command(c, "rf echo off");
    }
    if (!tx_mhz_ok(mhz)) {
        return KODE_DENIED;
    }
    char t[24];
    (void)snprintf(t, sizeof(t), "rf echo on %u", (unsigned)(mhz - CH_BASE_MHZ));
    return fck_drv_nrf_command(c, t);
}

kode_status_t fck_drv_nrf_command_echo_statistics(fck_drv_nrf_command_t *c)
{
    return fck_drv_nrf_command(c, "rf echo");
}

kode_status_t fck_drv_nrf_command_ping(fck_drv_nrf_command_t *c, uint16_t n, uint16_t mhz, uint16_t gap_ms)
{
    if (n < 1U || n > FCK_DRV_NRF_PING_MAX || !tx_mhz_ok(mhz) || gap_ms > FCK_DRV_NRF_GAP_MAX_MS) {
        return KODE_DENIED;
    }
    char t[32];
    (void)snprintf(t, sizeof(t), "rf ping %u %u %u", (unsigned)n,
                   (unsigned)(mhz - CH_BASE_MHZ), (unsigned)gap_ms);
    return fck_drv_nrf_command(c, t);
}

kode_status_t fck_drv_nrf_command_sniff(fck_drv_nrf_command_t *c, uint16_t mhz, uint32_t ms, int8_t rssi_min)
{
    if (!rx_mhz_ok(mhz) || ms < FCK_DRV_NRF_SNIFF_MIN_MS || ms > FCK_DRV_NRF_SNIFF_MAX_MS
        || rssi_min < -127 || rssi_min > 0) {
        return KODE_DENIED;
    }
    char t[40];
    (void)snprintf(t, sizeof(t), "rf sniff %u %lu %d", (unsigned)(mhz - CH_BASE_MHZ),
                   (unsigned long)ms, (int)rssi_min);
    return fck_drv_nrf_command(c, t);
}

kode_status_t fck_drv_nrf_command_sniff_config(fck_drv_nrf_command_t *c, const fck_drv_nrf_sniff_config_t *phy)
{
    if (phy == NULL || (phy->rate_mbps != 1U && phy->rate_mbps != 2U)
        || (phy->crc_bytes != 0U && phy->crc_bytes != 2U) || phy->len < 1U
        || phy->len > FCK_DRV_NRF_FRAME_MAX) {
        return KODE_DENIED;
    }
    char t[32];
    (void)snprintf(t, sizeof(t), "rf scfg %u %u %u", (unsigned)phy->rate_mbps,
                   (unsigned)phy->crc_bytes, (unsigned)phy->len);
    return fck_drv_nrf_command(c, t);
}

kode_status_t fck_drv_nrf_command_sniff_address(fck_drv_nrf_command_t *c, const fck_drv_nrf_sniff_address_t *addr)
{
    if (addr == NULL || addr->balen < 2U || addr->balen > 4U) {
        return KODE_DENIED;
    }
    /* The firmware reads both with strtoul(, 16): no 0x needed. */
    char t[40];
    (void)snprintf(t, sizeof(t), "rf saddr %u %08lX %02X", (unsigned)addr->balen,
                   (unsigned long)addr->base, (unsigned)addr->prefix);
    return fck_drv_nrf_command(c, t);
}

kode_status_t fck_drv_nrf_status(int32_t rc)
{
    switch (rc) {
    case 0:
        return KODE_OK;
    case 1:
        return KODE_IO_ERROR;
    case 2:
        return KODE_DENIED;
    default:
        /* No rc, or one this SDK does not know: the firmware said it did not do it. */
        return KODE_IO_ERROR;
    }
}

/* ---- Lines ------------------------------------------------------------------- */

static bool is_tag(const fck_drv_link_message_t *m, const char *tag)
{
    return m != NULL && m->tag != NULL && m->body != NULL && strcmp(m->tag, tag) == 0;
}

static int8_t dbm(int32_t v)
{
    if (v < INT8_MIN) {
        return INT8_MIN;
    }
    return (int8_t)((v > INT8_MAX) ? INT8_MAX : v);
}

/* A whole number from @p lo to @p hi. */
static bool range(const fck_drv_link_message_t *m, const char *key, int32_t lo, int32_t hi, int32_t *out)
{
    int32_t v;
    if (!fck_drv_link_message_integer(m, key, &v) || v < lo || v > hi) {
        return false;
    }
    *out = v;
    return true;
}

/* A flag, printed as 0 or 1. */
static bool flag(const fck_drv_link_message_t *m, const char *key, bool *out)
{
    int32_t v;
    if (!range(m, key, 0, 1, &v)) {
        return false;
    }
    *out = (v == 1);
    return true;
}

/* A channel the firmware printed (ch=, 0 to 100), in MHz. */
static bool mhz_of_ch(const fck_drv_link_message_t *m, uint16_t *out)
{
    int32_t ch;
    if (!range(m, "ch", 0, 100, &ch)) {
        return false;
    }
    *out = (uint16_t)(CH_BASE_MHZ + (uint32_t)ch);
    return true;
}

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

/* Hex digits with no 0x, as the firmware prints a node id (%02X) or a DEVICEID
 * (%08X%08X): exactly @p digits of them. fck_drv_link_message_integer() would read "15" as
 * fifteen. */
static bool hex(const fck_drv_link_message_t *m, const char *key, size_t digits, uint64_t *out)
{
    char   s[20];
    size_t n = 0U;
    if (!fck_drv_link_message_text(m, key, s, sizeof(s), &n) || n != digits) {
        return false;
    }
    uint64_t v = 0U;
    for (size_t i = 0U; i < n; i++) {
        int d = hex_digit(s[i]);
        if (d < 0) {
            return false;
        }
        v = (v << 4) | (uint64_t)d;
    }
    *out = v;
    return true;
}

static bool node(const fck_drv_link_message_t *m, const char *key, uint8_t *out)
{
    uint64_t v;
    if (!hex(m, key, 2U, &v)) {
        return false;
    }
    *out = (uint8_t)v;
    return true;
}

/* "-41.5", "0.0": the firmware's fmt_mean(), one decimal. In tenths. */
static bool tenths(const fck_drv_link_message_t *m, const char *key, int16_t *out)
{
    char   s[12];
    size_t n = 0U;
    if (!fck_drv_link_message_text(m, key, s, sizeof(s), &n) || n < 3U) {
        return false;
    }
    size_t i   = (s[0] == '-') ? 1U : 0U;
    long   acc = 0;
    bool   dot = false;
    size_t whole = 0U;
    for (; i < n; i++) {
        if (s[i] == '.' && !dot && whole > 0U && i == n - 2U) {
            dot = true;
        } else if (s[i] >= '0' && s[i] <= '9') {
            acc = acc * 10 + (s[i] - '0');
            whole += dot ? 0U : 1U;
            if (acc > 32767) {
                return false;
            }
        } else {
            return false;
        }
    }
    if (!dot) {
        return false;
    }
    *out = (int16_t)((s[0] == '-') ? -acc : acc);
    return true;
}

/* "2.0", "1.0" or "2.0.1" into its numbers; false for anything else. */
static bool version(const char *s, uint8_t out[3])
{
    uint8_t  v[3]  = { 0U, 0U, 0U };
    unsigned part  = 0U;
    unsigned acc   = 0U;
    bool     digit = false;
    for (;; s++) {
        if (*s >= '0' && *s <= '9') {
            acc   = acc * 10U + (unsigned)(*s - '0');
            digit = true;
            if (acc > 255U) {
                return false;
            }
        } else if ((*s == '.' || *s == '\0') && digit && part < 3U) {
            v[part++] = (uint8_t)acc;
            if (*s == '\0') {
                break;
            }
            acc   = 0U;
            digit = false;
        } else {
            return false;
        }
    }
    if (part < 2U) {
        return false;
    }
    memcpy(out, v, sizeof(v));
    return true;
}

bool fck_drv_nrf_parse_info(const fck_drv_link_message_t *m, fck_drv_nrf_info_t *out)
{
    if (!is_tag(m, "NRFINFO") || out == NULL) {
        return false;
    }
    fck_drv_nrf_info_t i;
    memset(&i, 0, sizeof(i));
    if (!fck_drv_link_message_text(m, "fw", i.fw, sizeof(i.fw), NULL) || i.fw[0] == '\0') {
        return false;
    }
    uint8_t v[3];
    if (fck_drv_link_message_text(m, "ver", i.ver, sizeof(i.ver), NULL) && version(i.ver, v)) {
        i.has_version = true;
        i.ver_major   = v[0];
        i.ver_minor   = v[1];
        i.ver_patch   = v[2];
    }
    if (hex(m, "devid", 16U, &i.devid)) {
        i.node = (uint8_t)(i.devid & 0xFFU);
    }
    (void)fck_drv_link_message_unsigned(m, "reset", &i.reset);
    (void)fck_drv_link_message_unsigned(m, "uptime_ms", &i.uptime_ms);
    i.has_map = fck_drv_link_message_unsigned(m, "map", &i.map);
    /* The list is read where it is, word by word: at 66 characters it is
     * already longer than a buffer one would give it, and it grows. */
    i.has_caps = fck_drv_link_message_has(m, "caps");
    for (size_t k = 0U; i.has_caps && k < sizeof(s_caps) / sizeof(s_caps[0]); k++) {
        if (fck_drv_link_message_listed(m, "caps", s_caps[k].word)) {
            i.caps |= s_caps[k].bit;
        }
    }
    *out = i;
    return true;
}

bool fck_drv_nrf_is_official(const char *fw)
{
    return fw != NULL && (strcmp(fw, FCK_DRV_NRF_OFFICIAL_FIRMWARE) == 0
                          || strcmp(fw, FCK_DRV_NRF_OFFICIAL_FIRMWARE_OLD) == 0);
}

bool fck_drv_nrf_is_compatible(const fck_drv_nrf_info_t *info)
{
    /* An official image that prints no caps= is known by its name. */
    return info != NULL && (info->has_caps || fck_drv_nrf_is_official(info->fw));
}

/* An official image without caps= keeps the limits itself from 2.0. */
bool fck_drv_nrf_is_unlimited(const fck_drv_nrf_info_t *info)
{
    if (info == NULL || info->has_caps || !fck_drv_nrf_is_official(info->fw)) {
        return false;
    }
    return !(info->has_version && VER(info->ver_major, info->ver_minor) >= VER(2, 0));
}

/* What an official image that prints no caps= can be asked: the four radio
 * commands, whatever its version, so they are asked of one that printed no
 * version too. */
static bool has_by_version(const fck_drv_nrf_info_t *info, fck_drv_nrf_feature_t f)
{
    (void)info;
    switch (f) {
    case FCK_DRV_NRF_SPECTRUM:
    case FCK_DRV_NRF_PING:
    case FCK_DRV_NRF_ECHO:
    case FCK_DRV_NRF_SNIFF:
        return true;
    default:
        return false;
    }
}

static bool caps_all(const fck_drv_nrf_info_t *info, uint16_t bits)
{
    return (info->caps & bits) == bits;
}

bool fck_drv_nrf_has(const fck_drv_nrf_info_t *info, fck_drv_nrf_feature_t f)
{
    if (!fck_drv_nrf_is_compatible(info)) {
        return false;
    }
    if (!info->has_caps) {
        return has_by_version(info, f);
    }
    switch (f) {
    case FCK_DRV_NRF_SPECTRUM:
        return caps_all(info, FCK_DRV_NRF_CAP_SPEC);
    case FCK_DRV_NRF_PING:
        return caps_all(info, FCK_DRV_NRF_CAP_PING | FCK_DRV_NRF_CAP_POWER);
    case FCK_DRV_NRF_ECHO:
        return caps_all(info, FCK_DRV_NRF_CAP_ECHO | FCK_DRV_NRF_CAP_POWER | FCK_DRV_NRF_CAP_OFF);
    case FCK_DRV_NRF_SNIFF:
        return caps_all(info, FCK_DRV_NRF_CAP_SNIFF | FCK_DRV_NRF_CAP_SCFG | FCK_DRV_NRF_CAP_SADDR);
    default:
        return false;
    }
}

void fck_drv_nrf_spectrum_peak(const fck_drv_nrf_spectrum_t *sweep, uint16_t first_mhz, int8_t *dbm, size_t n)
{
    if (sweep == NULL || dbm == NULL) {
        return;
    }
    for (size_t k = 0U; k < sweep->n; k++) {
        long const i = (long)sweep->f0_mhz + (long)k - (long)first_mhz;
        if (i >= 0 && (size_t)i < n && sweep->dbm[k] > dbm[i]) {
            dbm[i] = sweep->dbm[k];
        }
    }
}

fck_drv_nrf_gate_t fck_drv_nrf_gate(const fck_drv_nrf_info_t *info, const char *text, const char *verb,
                            fck_drv_nrf_command_t *c)
{
    if (info == NULL || text == NULL || c == NULL) {
        return FCK_DRV_NRF_GATE_REFUSE;
    }
    if (!fck_drv_nrf_is_official(info->fw)) {
        return (verb != NULL) ? FCK_DRV_NRF_GATE_AS_IS : FCK_DRV_NRF_GATE_REFUSE;
    }
    fck_drv_nrf_command_t k;
    if (fck_drv_nrf_command(&k, text) != KODE_OK
        || (k.emits && fck_drv_nrf_is_unlimited(info))
        || (verb != NULL && strcmp(verb, k.verb) != 0)) {
        return FCK_DRV_NRF_GATE_REFUSE;
    }
    *c = k;
    return FCK_DRV_NRF_GATE_OFFICIAL;
}

bool fck_drv_nrf_parse_error(const fck_drv_link_message_t *m, fck_drv_nrf_error_t *out)
{
    if (!is_tag(m, "ERR") || out == NULL) {
        return false;
    }
    fck_drv_nrf_error_t e;
    memset(&e, 0, sizeof(e));
    if ((!fck_drv_link_message_text(m, "cmd", e.cmd, sizeof(e.cmd), NULL)
         && !fck_drv_link_message_text(m, "what", e.cmd, sizeof(e.cmd), NULL))
        || e.cmd[0] == '\0') {
        return false;
    }
    (void)fck_drv_link_message_text(m, "why", e.why, sizeof(e.why), NULL);
    if (!fck_drv_link_message_integer(m, "err", &e.code) && !fck_drv_link_message_integer(m, "rc", &e.code)
        && !fck_drv_link_message_integer(m, "points", &e.code)) {
        e.code = 0;
    }
    *out = e;
    return true;
}

bool fck_drv_nrf_parse_spectrum(const fck_drv_link_message_t *m, fck_drv_nrf_spectrum_t *out)
{
    if (!is_tag(m, "SPEC24") || out == NULL) {
        return false;
    }
    int32_t f0;
    int32_t n;
    int32_t sweep;
    /* Up to 101 values of "-127,": the longest value any firmware line has. */
    char    v[FCK_DRV_NRF_SPECTRUM_MAX * 5U + 1U];
    if (!range(m, "f0", (int32_t)FCK_DRV_NRF_RECEIVE_MHZ_MIN, (int32_t)FCK_DRV_NRF_RECEIVE_MHZ_MAX, &f0)
        || !range(m, "n", 1, (int32_t)FCK_DRV_NRF_SPECTRUM_MAX, &n)
        || f0 + n - 1 > (int32_t)FCK_DRV_NRF_RECEIVE_MHZ_MAX
        || !range(m, "sweep", 0, (int32_t)FCK_DRV_NRF_SWEEPS_MAX - 1, &sweep)
        || !fck_drv_link_message_text(m, "v", v, sizeof(v), NULL)) {
        return false;
    }
    fck_drv_nrf_spectrum_t s;
    memset(&s, 0, sizeof(s));
    const char *p = v;
    for (int32_t i = 0; i < n; i++) {
        bool neg = (*p == '-');
        p += neg ? 1 : 0;
        if (*p < '0' || *p > '9') {
            return false;
        }
        int32_t x = 0;
        while (*p >= '0' && *p <= '9') {
            x = x * 10 + (*p - '0');
            if (x > 128) {
                return false;
            }
            p++;
        }
        s.dbm[i] = dbm(neg ? -x : x);
        if (*p != ((i == n - 1) ? '\0' : ',')) {
            return false;
        }
        p += (*p == ',') ? 1 : 0;
    }
    s.f0_mhz = (uint16_t)f0;
    s.n      = (uint8_t)n;
    s.sweep  = (uint16_t)sweep;
    *out = s;
    return true;
}

bool fck_drv_nrf_parse_power(const fck_drv_link_message_t *m, fck_drv_nrf_power_t *out)
{
    if (!is_tag(m, "POWER24") || out == NULL) {
        return false;
    }
    int32_t req;
    int32_t d;
    if (!range(m, "req", -100, 100, &req) || !range(m, "dbm", -128, 127, &d)) {
        return false;
    }
    out->req = (int16_t)req;
    out->dbm = (int8_t)d;
    return true;
}

bool fck_drv_nrf_parse_echo(const fck_drv_link_message_t *m, fck_drv_nrf_echo_t *out)
{
    if (!is_tag(m, "ECHO24") || out == NULL) {
        return false;
    }
    fck_drv_nrf_echo_t e;
    memset(&e, 0, sizeof(e));
    int32_t d;
    if (!flag(m, "on", &e.on)
        || (e.on && (!mhz_of_ch(m, &e.mhz) || !range(m, "dbm", -128, 127, &d)
                     || !node(m, "node", &e.node)))) {
        return false;
    }
    if (e.on) {
        int32_t idle;
        e.dbm = (int8_t)d;
        if (range(m, "idle_s", 0, UINT16_MAX, &idle)) {
            e.idle_s = (uint16_t)idle;
        }
    }
    *out = e;
    return true;
}

bool fck_drv_nrf_parse_echo_statistics(const fck_drv_link_message_t *m, fck_drv_nrf_echo_statistics_t *out)
{
    if (!is_tag(m, "ECHOSTAT24") || out == NULL) {
        return false;
    }
    fck_drv_nrf_echo_statistics_t e;
    memset(&e, 0, sizeof(e));
    if (!flag(m, "on", &e.on) || !mhz_of_ch(m, &e.mhz) || !fck_drv_link_message_unsigned(m, "rx", &e.rx)
        || !fck_drv_link_message_unsigned(m, "crcbad", &e.crcbad) || !fck_drv_link_message_unsigned(m, "replied", &e.replied)
        || !fck_drv_link_message_unsigned(m, "foreign", &e.foreign)) {
        return false;
    }
    (void)fck_drv_link_message_unsigned(m, "txslow", &e.txslow);
    (void)flag(m, "idle", &e.idle);
    *out = e;
    return true;
}

bool fck_drv_nrf_parse_ping_start(const fck_drv_link_message_t *m, fck_drv_nrf_ping_start_t *out)
{
    if (!is_tag(m, "PINGSTART24") || out == NULL) {
        return false;
    }
    fck_drv_nrf_ping_start_t p;
    memset(&p, 0, sizeof(p));
    int32_t n;
    int32_t d;
    int32_t to;
    if (!range(m, "n", 1, (int32_t)FCK_DRV_NRF_PING_MAX, &n) || !mhz_of_ch(m, &p.mhz)
        || !range(m, "dbm", -128, 127, &d) || !node(m, "node", &p.node)) {
        return false;
    }
    p.n   = (uint16_t)n;
    p.dbm = (int8_t)d;
    if (range(m, "timeout_ms", 0, UINT16_MAX, &to)) {
        p.timeout_ms = (uint16_t)to;
    }
    *out = p;
    return true;
}

bool fck_drv_nrf_parse_ping(const fck_drv_link_message_t *m, fck_drv_nrf_ping_t *out)
{
    if (!is_tag(m, "PING24") || out == NULL) {
        return false;
    }
    fck_drv_nrf_ping_t p;
    memset(&p, 0, sizeof(p));
    p.rssi_fwd = FCK_DRV_NRF_RSSI_NONE;
    p.rssi_rev = FCK_DRV_NRF_RSSI_NONE;
    int32_t seq;
    int32_t fwd;
    int32_t rev;
    if (!range(m, "seq", 1, (int32_t)FCK_DRV_NRF_PING_MAX, &seq) || !flag(m, "ok", &p.ok)) {
        return false;
    }
    p.seq = (uint16_t)seq;
    if (p.ok) {
        if (!range(m, "rssi_fwd", -127, 0, &fwd) || !range(m, "rssi_rev", -127, 0, &rev)) {
            return false;
        }
        p.why      = FCK_DRV_NRF_PING_OK;
        p.rssi_fwd = (int8_t)fwd;
        p.rssi_rev = (int8_t)rev;
    } else {
        static const char *const why[] = { "txfail", "timeout", "crc", "mismatch" };
        char w[12];
        p.why = FCK_DRV_NRF_PING_OTHER;
        if (fck_drv_link_message_text(m, "why", w, sizeof(w), NULL)) {
            for (unsigned i = 0U; i < sizeof(why) / sizeof(why[0]); i++) {
                if (strcmp(w, why[i]) == 0) {
                    p.why = (fck_drv_nrf_ping_why_t)(FCK_DRV_NRF_PING_TRANSMIT_FAILED + i);
                }
            }
        }
        if (range(m, "rssi_rev", -127, 0, &rev)) {
            p.rssi_rev = (int8_t)rev;
        }
    }
    *out = p;
    return true;
}

bool fck_drv_nrf_parse_ping_done(const fck_drv_link_message_t *m, fck_drv_nrf_ping_done_t *out)
{
    if (!is_tag(m, "PINGDONE24") || out == NULL) {
        return false;
    }
    fck_drv_nrf_ping_done_t d;
    memset(&d, 0, sizeof(d));
    int32_t sent;
    int32_t ok;
    if (!mhz_of_ch(m, &d.mhz) || !range(m, "sent", 1, (int32_t)FCK_DRV_NRF_PING_MAX, &sent)
        || !range(m, "ok", 0, sent, &ok) || !tenths(m, "fwd_mean", &d.fwd_mean_x10)
        || !tenths(m, "rev_mean", &d.rev_mean_x10)) {
        return false;
    }
    d.sent = (uint16_t)sent;
    d.ok   = (uint16_t)ok;

    /* The rest only describes; one that is missing stays 0. */
    struct {
        const char *key;
        int8_t     *to;
    } const rssi[] = {
        { "fwd_min", &d.fwd_min }, { "fwd_max", &d.fwd_max }, { "rev_min", &d.rev_min },
        { "rev_max", &d.rev_max }, { "dbm", &d.dbm },         { "peer_dbm", &d.peer_dbm },
    };
    for (unsigned i = 0U; i < sizeof(rssi) / sizeof(rssi[0]); i++) {
        int32_t v;
        if (range(m, rssi[i].key, -128, 127, &v)) {
            *rssi[i].to = (int8_t)v;
        }
    }
    struct {
        const char *key;
        uint16_t   *to;
    } const cnt[] = {
        { "timeout", &d.timeout }, { "crcbad", &d.crcbad },
        { "mismatch", &d.mismatch }, { "txfail", &d.txfail },
    };
    for (unsigned i = 0U; i < sizeof(cnt) / sizeof(cnt[0]); i++) {
        int32_t v;
        if (range(m, cnt[i].key, 0, UINT16_MAX, &v)) {
            *cnt[i].to = (uint16_t)v;
        }
    }
    (void)node(m, "peer", &d.peer);
    (void)fck_drv_link_message_unsigned(m, "txslow", &d.txslow);
    *out = d;
    return true;
}

/* "1M" or "2M". */
static bool rate(const fck_drv_link_message_t *m, uint8_t *out)
{
    char r[4];
    if (!fck_drv_link_message_text(m, "rate", r, sizeof(r), NULL) || (strcmp(r, "1M") != 0
                                                        && strcmp(r, "2M") != 0)) {
        return false;
    }
    *out = (uint8_t)(r[0] - '0');
    return true;
}

static bool scfg(const fck_drv_link_message_t *m, fck_drv_nrf_sniff_config_t *out)
{
    int32_t crc;
    int32_t len;
    if (!rate(m, &out->rate_mbps) || !range(m, "crc", 0, 2, &crc) || crc == 1
        || !range(m, "len", 1, (int32_t)FCK_DRV_NRF_FRAME_MAX, &len)) {
        return false;
    }
    out->crc_bytes = (uint8_t)crc;
    out->len       = (uint8_t)len;
    return true;
}

static bool saddr(const fck_drv_link_message_t *m, fck_drv_nrf_sniff_address_t *out)
{
    int32_t  balen;
    uint32_t prefix;
    if (!range(m, "balen", 2, 4, &balen) || !fck_drv_link_message_unsigned(m, "base", &out->base)
        || !fck_drv_link_message_unsigned(m, "prefix", &prefix) || prefix > 0xFFU) {
        return false;
    }
    out->balen  = (uint8_t)balen;
    out->prefix = (uint8_t)prefix;
    return true;
}

bool fck_drv_nrf_parse_sniff_start(const fck_drv_link_message_t *m, fck_drv_nrf_sniff_start_t *out)
{
    if (!is_tag(m, "SNIFFSTART24") || out == NULL) {
        return false;
    }
    fck_drv_nrf_sniff_start_t s;
    memset(&s, 0, sizeof(s));
    int32_t rmin;
    if (!mhz_of_ch(m, &s.mhz) || !scfg(m, &s.phy) || !saddr(m, &s.addr)
        || !range(m, "rssi_min", -127, 0, &rmin) || !fck_drv_link_message_unsigned(m, "ms", &s.ms)) {
        return false;
    }
    s.rssi_min = (int8_t)rmin;
    *out = s;
    return true;
}

bool fck_drv_nrf_parse_frame(const fck_drv_link_message_t *m, fck_drv_nrf_frame_t *out)
{
    bool top = is_tag(m, "SNIFFTOP24");
    if ((!top && !is_tag(m, "SNIFF24")) || out == NULL) {
        return false;
    }
    fck_drv_nrf_frame_t f;
    memset(&f, 0, sizeof(f));
    int32_t i;
    int32_t rssi;
    int32_t cnt = 1;
    int32_t len;
    char    h[FCK_DRV_NRF_FRAME_MAX * 2U + 1U];
    size_t  hl = 0U;
    if (!range(m, "i", 0, (int32_t)FCK_DRV_NRF_FRAMES_MAX - 1, &i)
        || !range(m, "rssi", -127, 0, &rssi) || (top && !range(m, "cnt", 1, UINT16_MAX, &cnt))
        || !fck_drv_link_message_text(m, "data", h, sizeof(h), &hl) || hl == 0U || (hl % 2U) != 0U) {
        return false;
    }
    f.len = (uint8_t)(hl / 2U);
    if (fck_drv_link_message_integer(m, "len", &len) && len != (int32_t)f.len) {
        return false;
    }
    for (size_t k = 0U; k < f.len; k++) {
        int hi = hex_digit(h[2U * k]);
        int lo = hex_digit(h[2U * k + 1U]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        f.data[k] = (uint8_t)((hi << 4) | lo);
    }
    f.top  = top;
    f.i    = (uint8_t)i;
    f.cnt  = (uint16_t)cnt;
    f.rssi = (int8_t)rssi;
    *out = f;
    return true;
}

bool fck_drv_nrf_frames_add(fck_drv_nrf_frames_t *t, const fck_drv_nrf_frame_t *fr)
{
    if (t == NULL || fr == NULL) {
        return false;
    }
    if (fr->top && !t->top) {
        t->top = true;
        t->n   = 0U;
    } else if (!fr->top && t->top) {
        return false;
    }
    if (t->n >= FCK_DRV_NRF_FRAMES_MAX) {
        return false;
    }
    for (uint8_t k = 0U; !fr->top && k < t->n; k++) {
        if (t->f[k].i == fr->i) {
            return false;
        }
    }
    t->f[t->n++] = *fr;
    return true;
}

bool fck_drv_nrf_parse_sniff_done(const fck_drv_link_message_t *m, fck_drv_nrf_sniff_done_t *out)
{
    if (!is_tag(m, "SNIFFDONE24") || out == NULL) {
        return false;
    }
    fck_drv_nrf_sniff_done_t d;
    memset(&d, 0, sizeof(d));
    int32_t uniq;
    if (!mhz_of_ch(m, &d.mhz) || !fck_drv_link_message_unsigned(m, "rx", &d.rx)
        || !fck_drv_link_message_unsigned(m, "gated", &d.gated)
        || !range(m, "uniq", 0, (int32_t)FCK_DRV_NRF_FRAMES_MAX, &uniq)
        || !fck_drv_link_message_unsigned(m, "ms", &d.ms)) {
        return false;
    }
    d.uniq = (uint8_t)uniq;
    *out = d;
    return true;
}

bool fck_drv_nrf_parse_sniff_config(const fck_drv_link_message_t *m, fck_drv_nrf_sniff_config_t *out)
{
    fck_drv_nrf_sniff_config_t s;
    if (!is_tag(m, "SCFG24") || out == NULL || !scfg(m, &s)) {
        return false;
    }
    *out = s;
    return true;
}

bool fck_drv_nrf_parse_sniff_address(const fck_drv_link_message_t *m, fck_drv_nrf_sniff_address_t *out)
{
    fck_drv_nrf_sniff_address_t s;
    if (!is_tag(m, "SADDR24") || out == NULL || !saddr(m, &s)) {
        return false;
    }
    *out = s;
    return true;
}
