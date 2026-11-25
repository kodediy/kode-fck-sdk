/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file fck_drv_c5.c
 * @brief The C5 firmware's vocabulary. See fck_drv_c5.h.
 *
 * Plain C, like fck_drv_link.c: no ESP-IDF and no FreeRTOS, so tests/host runs
 * it as it is (tools/check_layers.sh, rule 4). Every name and number below
 * is the firmware's, from firmware/fck-c5 (fck_c5.c).
 */
#include "fck_drv_c5.h"

#include <stdio.h>
#include <string.h>

const char *const fck_drv_c5_async[] = { "GNSSU", "PPS", NULL };

/* The channels `chan` tries on each band: the firmware's candidate lists, 13 on
 * 2.4 GHz and 25 on 5 GHz. The Wi-Fi driver refuses some of them under the
 * firmware's country, at once, but the time is worked out for all of them. */
#define CHAN_TRIED_2G  13U
#define CHAN_TRIED_5G  25U

/* A number in a command, past every range the firmware takes: not worth more
 * digits, and the firmware answers it with rc 2 at once. */
#define ARG_NUM_MAX    1000000L

/* TEMPORARY: pre-production firmware (2.x). Remove when the production modules ship.
 * A version as one number, for comparing: 2.0 is 0x200. Only has_by_version()
 * compares one. */
#define VER(major, minor)  (((unsigned)(major) << 8) | (unsigned)(minor))

static const char *const s_band[] = { "all", "2g", "5g" };

/* The words of caps= this file knows: the firmware's FW_CAPS. */
static const struct {
    const char *word;
    uint16_t    bit;
} s_caps[] = {
    { "scan", FCK_DRV_C5_CAP_SCAN }, { "chan", FCK_DRV_C5_CAP_CHAN },
    { "ble", FCK_DRV_C5_CAP_BLE },   { "gnss", FCK_DRV_C5_CAP_GNSS },
    { "wp", FCK_DRV_C5_CAP_WP },     { "gtx", FCK_DRV_C5_CAP_GTX },
    { "health", FCK_DRV_C5_CAP_HEALTH }, { "bleadv", FCK_DRV_C5_CAP_BLEADV },
    { "frames", FCK_DRV_C5_CAP_FRAMES },
};

/* In fck_drv_c5_security_t's order: the firmware's auth_name(). */
static const char *const s_auth[] = {
    "open",
    "wep",
    "wpa_psk",
    "wpa2_psk",
    "wpa_wpa2_psk",
    "wpa2_ent",
    "wpa3_psk",
    "wpa2_wpa3_psk",
    "wapi_psk",
    "owe",
    "wpa3_ent_192",
    "dpp",
    "wpa3_ent",
    "wpa2_wpa3_ent",
    "wpa_ent",
    "unknown",
};

_Static_assert(sizeof(s_auth) / sizeof(s_auth[0]) == (size_t)FCK_DRV_C5_SECURITY_UNKNOWN + 1U,
               "one firmware name per fck_drv_c5_security_t");

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

/* "250", or "dwell_ms=250" when @p key is "dwell_ms": the firmware's own
 * num_arg(), which reads base 10 from a digit (strtol(t, &end, 10) in 1.5
 * and 2.0). So "010" is 10 there and here, and
 * "0x3c" no number at all. -1 when the word is neither. */
static long num_arg(const char *w, size_t len, const char *key)
{
    size_t kl = strlen(key);
    if (len > kl && memcmp(w, key, kl) == 0 && w[kl] == '=') {
        w   += kl + 1U;
        len -= kl + 1U;
    }
    if (len == 0U) {
        return -1;
    }
    long v = 0;
    for (size_t i = 0U; i < len; i++) {
        if (w[i] < '0' || w[i] > '9') {
            return -1;
        }
        v = v * 10 + (w[i] - '0');
        if (v > ARG_NUM_MAX) {
            return -1;
        }
    }
    return v;
}

/* `chan [2g|5g|all] [dwell_ms]`, in any order, the last band word winning:
 * the firmware's way of reading them. */
static uint32_t chan_ms(const char *text)
{
    uint32_t    tried = CHAN_TRIED_2G + CHAN_TRIED_5G;
    uint32_t    dwell = FCK_DRV_C5_DWELL_DEFAULT_MS;
    size_t      len;
    const char *w;
    for (unsigned i = 1U; (w = word(text, i, &len)) != NULL; i++) {
        long v;
        if (word_is(w, len, "all")) {
            tried = CHAN_TRIED_2G + CHAN_TRIED_5G;
        } else if (word_is(w, len, "2g")) {
            tried = CHAN_TRIED_2G;
        } else if (word_is(w, len, "5g")) {
            tried = CHAN_TRIED_5G;
        } else if ((v = num_arg(w, len, "dwell_ms")) >= 0) {
            dwell = (v > (long)FCK_DRV_C5_DWELL_MAX_MS) ? FCK_DRV_C5_DWELL_MAX_MS : (uint32_t)v;
        }
    }
    return tried * (dwell + FCK_DRV_C5_CHANNEL_HOP_MS) + FCK_DRV_C5_CHANNEL_SLACK_MS;
}

/* `frames ch=<n> [ms=<dwell>]`: its listening time and the UART's slack. Only a
 * keyed ms= is read: a bare number is no argument of `frames`. */
static uint32_t frames_ms(const char *text)
{
    uint32_t    ms = FCK_DRV_C5_FRAMES_MS_DEFAULT;
    size_t      len;
    const char *w;
    for (unsigned i = 1U; (w = word(text, i, &len)) != NULL; i++) {
        long v;
        if (len > 3U && memcmp(w, "ms=", 3U) == 0 && (v = num_arg(w, len, "ms")) >= 0) {
            ms = (v > (long)FCK_DRV_C5_FRAMES_MS_MAX) ? FCK_DRV_C5_FRAMES_MS_MAX : (uint32_t)v;
        }
    }
    return ms + FCK_DRV_C5_FRAMES_SLACK_MS;
}

/* `ble scan [secs]`. Anything else under `ble` is a usage line, at once. */
static uint32_t ble_ms(const char *text)
{
    size_t      len;
    const char *w = word(text, 1U, &len);
    if (w == NULL || !word_is(w, len, "scan")) {
        return FCK_DRV_C5_COMMAND_MS;
    }
    uint32_t secs = FCK_DRV_C5_BLE_SECONDS_DEFAULT;
    long     v;
    if ((w = word(text, 2U, &len)) != NULL && (v = num_arg(w, len, "secs")) >= 0) {
        secs = (v > (long)FCK_DRV_C5_BLE_SECONDS_MAX) ? FCK_DRV_C5_BLE_SECONDS_MAX : (uint32_t)v;
    }
    return secs * 1000U + FCK_DRV_C5_BLE_SLACK_MS;
}

kode_status_t fck_drv_c5_command(fck_drv_c5_command_t *c, const char *text)
{
    if (c == NULL || text == NULL) {
        return KODE_DENIED;
    }
    size_t n = strlen(text);
    if (n == 0U || n > FCK_DRV_C5_COMMAND_MAX) {
        return KODE_DENIED;
    }
    /* The firmware drops control bytes, but a line ending inside would be two
     * commands, the second one nobody waits for. */
    for (size_t i = 0U; i < n; i++) {
        unsigned char ch = (unsigned char)text[i];
        if (ch < 0x20U || ch > 0x7EU) {
            return KODE_DENIED;
        }
    }
    size_t      vlen;
    const char *verb = word(text, 0U, &vlen);
    if (verb == NULL || vlen > FCK_DRV_LINK_TAG_MAX) {
        return KODE_DENIED;
    }

    uint32_t ms    = FCK_DRV_C5_COMMAND_MS;
    bool     emits = false;
    if (word_is(verb, vlen, "scan")) {
        ms    = FCK_DRV_C5_SCAN_MS;
        emits = true;
    } else if (word_is(verb, vlen, "chan")) {
        ms = chan_ms(text);
    } else if (word_is(verb, vlen, "ble")) {
        ms = ble_ms(text);
    } else if (word_is(verb, vlen, "frames")) {
        ms = frames_ms(text);
    }

    memcpy(c->line, text, n);
    c->line[n]      = '\n';
    c->line[n + 1U] = '\0';
    c->len          = n + 1U;
    memcpy(c->verb, verb, vlen);
    c->verb[vlen] = '\0';
    c->ms         = ms;
    c->emits      = emits;
    return KODE_OK;
}

kode_status_t fck_drv_c5_command_info(fck_drv_c5_command_t *c)
{
    return fck_drv_c5_command(c, "info");
}

kode_status_t fck_drv_c5_command_health(fck_drv_c5_command_t *c)
{
    return fck_drv_c5_command(c, "health");
}

static bool band_ok(fck_drv_c5_band_t b)
{
    return (unsigned)b < sizeof(s_band) / sizeof(s_band[0]);
}

kode_status_t fck_drv_c5_command_scan(fck_drv_c5_command_t *c, fck_drv_c5_band_t band)
{
    if (!band_ok(band)) {
        return KODE_DENIED;
    }
    char t[16];
    (void)snprintf(t, sizeof(t), "scan %s", s_band[band]);
    return fck_drv_c5_command(c, t);
}

kode_status_t fck_drv_c5_command_channel(fck_drv_c5_command_t *c, fck_drv_c5_band_t band,
                              uint16_t dwell_ms)
{
    if (!band_ok(band) || dwell_ms < FCK_DRV_C5_DWELL_MIN_MS
        || dwell_ms > FCK_DRV_C5_DWELL_MAX_MS) {
        return KODE_DENIED;
    }
    char t[24];
    (void)snprintf(t, sizeof(t), "chan %s %u", s_band[band], (unsigned)dwell_ms);
    return fck_drv_c5_command(c, t);
}

kode_status_t fck_drv_c5_command_ble_scan(fck_drv_c5_command_t *c, uint8_t secs)
{
    if (secs < FCK_DRV_C5_BLE_SECONDS_MIN || secs > FCK_DRV_C5_BLE_SECONDS_MAX) {
        return KODE_DENIED;
    }
    char t[16];
    (void)snprintf(t, sizeof(t), "ble scan %u", (unsigned)secs);
    return fck_drv_c5_command(c, t);
}

kode_status_t fck_drv_c5_command_frames(fck_drv_c5_command_t *c, uint8_t channel, uint16_t ms)
{
    if (channel < 1U || channel > FCK_DRV_C5_FRAMES_CHANNEL_MAX || ms < FCK_DRV_C5_FRAMES_MS_MIN
        || ms > FCK_DRV_C5_FRAMES_MS_MAX) {
        return KODE_DENIED;
    }
    char t[32];
    (void)snprintf(t, sizeof(t), "frames ch=%u ms=%u", (unsigned)channel, (unsigned)ms);
    return fck_drv_c5_command(c, t);
}

kode_status_t fck_drv_c5_command_gnss(fck_drv_c5_command_t *c, bool on)
{
    return fck_drv_c5_command(c, on ? "gnss on" : "gnss off");
}

kode_status_t fck_drv_c5_command_gnss_transmit(fck_drv_c5_command_t *c, const char *body)
{
    if (body == NULL) {
        return KODE_DENIED;
    }
    if (body[0] == '$') {
        body++;
    }
    size_t n = strlen(body);
    /* The firmware refuses the same characters (rc 2); POLCFGPRT it would send,
     * and the receiver would leave the 115200 the firmware listens at. */
    if (n == 0U || n > FCK_DRV_C5_GNSS_TRANSMIT_MAX || strpbrk(body, " $*\"\\") != NULL
        || strncmp(body, "POLCFGPRT", 9U) == 0) {
        return KODE_DENIED;
    }
    char t[FCK_DRV_C5_GNSS_TRANSMIT_MAX + 8U];
    (void)snprintf(t, sizeof(t), "gtx -n %s", body);
    return fck_drv_c5_command(c, t);
}

kode_status_t fck_drv_c5_status(int32_t rc)
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

/* A count: a whole number, not negative. */
static bool count(const fck_drv_link_message_t *m, const char *key, uint32_t *out)
{
    int32_t v;
    if (!fck_drv_link_message_integer(m, key, &v) || v < 0) {
        return false;
    }
    *out = (uint32_t)v;
    return true;
}

/* A flag, printed as 0 or 1. */
static bool flag(const fck_drv_link_message_t *m, const char *key, bool *out)
{
    int32_t v;
    if (!fck_drv_link_message_integer(m, key, &v) || (v != 0 && v != 1)) {
        return false;
    }
    *out = (v == 1);
    return true;
}

/* A C5 uptime in ms, which outgrows int32_t after 24.8 days: up to 18
 * digits, never negative. */
static bool uptime_ms(const fck_drv_link_message_t *m, const char *key, int64_t *out)
{
    char   s[20];
    size_t n = 0U;
    if (!fck_drv_link_message_text(m, key, s, sizeof(s), &n) || n == 0U || n > 18U) {
        return false;
    }
    int64_t v = 0;
    for (size_t i = 0U; i < n; i++) {
        if (s[i] < '0' || s[i] > '9') {
            return false;
        }
        v = v * 10 + (s[i] - '0');
    }
    *out = v;
    return true;
}

/* ch= and band=, which #AP and #CHAN both carry. */
static bool channel(const fck_drv_link_message_t *m, uint8_t *ch, fck_drv_c5_band_t *band)
{
    int32_t v;
    char    b[4];
    if (!fck_drv_link_message_integer(m, "ch", &v) || v < 1 || v > 255
        || !fck_drv_link_message_text(m, "band", b, sizeof(b), NULL)) {
        return false;
    }
    if (strcmp(b, "2g") == 0) {
        *band = FCK_DRV_C5_BAND_2G;
    } else if (strcmp(b, "5g") == 0) {
        *band = FCK_DRV_C5_BAND_5G;
    } else {
        return false;
    }
    *ch = (uint8_t)v;
    return true;
}

/* "2.0", "1.5" or "2.0.1" into its numbers; false for anything else. */
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

bool fck_drv_c5_parse_info(const fck_drv_link_message_t *m, fck_drv_c5_info_t *out)
{
    if (!is_tag(m, "C5INFO") || out == NULL) {
        return false;
    }
    fck_drv_c5_info_t i;
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
    (void)fck_drv_link_message_text(m, "chip", i.chip, sizeof(i.chip), NULL);
    (void)fck_drv_link_message_text(m, "rev", i.rev, sizeof(i.rev), NULL);
    (void)fck_drv_link_message_mac(m, "mac", i.mac);
    (void)fck_drv_link_message_text(m, "reg", i.reg, sizeof(i.reg), NULL);
    (void)fck_drv_link_message_text(m, "lim", i.lim, sizeof(i.lim), NULL);
    (void)fck_drv_link_message_unsigned(m, "uptime_ms", &i.uptime_ms);
    (void)fck_drv_link_message_text(m, "rst", i.rst, sizeof(i.rst), NULL);
    /* The list is read where it is, word by word: one that grows past any
     * buffer here still says what it said before. */
    i.has_caps = fck_drv_link_message_has(m, "caps");
    for (size_t k = 0U; i.has_caps && k < sizeof(s_caps) / sizeof(s_caps[0]); k++) {
        if (fck_drv_link_message_listed(m, "caps", s_caps[k].word)) {
            i.caps |= s_caps[k].bit;
        }
    }
    *out = i;
    return true;
}

bool fck_drv_c5_parse_health(const fck_drv_link_message_t *m, fck_drv_c5_health_t *out)
{
    if (!is_tag(m, "C5HEALTH") || out == NULL) {
        return false;
    }
    fck_drv_c5_health_t h;
    memset(&h, 0, sizeof(h));
    if (!fck_drv_link_message_unsigned(m, "uptime_ms", &h.uptime_ms)
        || !fck_drv_link_message_unsigned(m, "heap", &h.heap)
        || !fck_drv_link_message_unsigned(m, "heap_min", &h.heap_min)) {
        return false;
    }
    (void)fck_drv_link_message_text(m, "rst", h.rst, sizeof(h.rst), NULL);
    *out = h;
    return true;
}

bool fck_drv_c5_is_official(const char *fw)
{
    return fw != NULL && (strcmp(fw, FCK_DRV_C5_OFFICIAL_FIRMWARE) == 0
                          || strcmp(fw, FCK_DRV_C5_OFFICIAL_FIRMWARE_OLD) == 0);
}

bool fck_drv_c5_is_compatible(const fck_drv_c5_info_t *info)
{
    /* TEMPORARY: pre-production firmware (2.x). Remove when the production modules ship.
     * One of the first modules' prints no caps= and is known by its name. */
    return info != NULL && (info->has_caps || fck_drv_c5_is_official(info->fw));
}

/* TEMPORARY: pre-production firmware (2.x). Remove when the production modules ship. */
bool fck_drv_c5_is_unlimited(const fck_drv_c5_info_t *info)
{
    if (info == NULL || info->has_caps || !fck_drv_c5_is_official(info->fw)) {
        return false;
    }
    return !(info->has_version && VER(info->ver_major, info->ver_minor) >= VER(2, 0)
             && strcmp(info->reg, "EU") == 0 && strcmp(info->lim, "ok") == 0);
}

/* TEMPORARY: pre-production firmware (2.x). Remove when the production modules ship.
 * What a firmware that prints no caps= can be asked, by its version: the
 * thresholds of the line that ended at 2.4. The relay was in every one of
 * them, so it is asked of one that printed no version too. */
static bool has_by_version(const fck_drv_c5_info_t *info, fck_drv_c5_feature_t f)
{
    if (f == FCK_DRV_C5_GNSS_RELAY) {
        return true;
    }
    if (!info->has_version) {
        return false;
    }
    unsigned have = VER(info->ver_major, info->ver_minor);
    switch (f) {
    case FCK_DRV_C5_SCAN:
        return have >= VER(1, 0);
    case FCK_DRV_C5_CHANNEL:
    case FCK_DRV_C5_BLE_SCAN:
        return have >= VER(1, 1);
    case FCK_DRV_C5_WAKE_PIN:
        return have >= VER(1, 2);
    case FCK_DRV_C5_GNSS_TRANSMIT:
        return have >= VER(1, 4);
    case FCK_DRV_C5_GNSS_REPEAT:
        return have >= VER(1, 5);
    case FCK_DRV_C5_BLE_ADVERTISEMENT:
        /* 2.2 printed them too, but two lines of that firmware were numbered 2.2 and 2.3, and only one
         * had them: a version cannot tell which. From 2.4 there was one line. A #BLE is read by its
         * keys whatever this says (fck_drv_c5_parse_ble()). */
    case FCK_DRV_C5_FRAMES:
        return have >= VER(2, 4);
    default:
        return false;     /* `health` among them: none of that line has it */
    }
}

bool fck_drv_c5_has(const fck_drv_c5_info_t *info, fck_drv_c5_feature_t f)
{
    if (!fck_drv_c5_is_compatible(info)) {
        return false;
    }
    if (!info->has_caps) {
        /* TEMPORARY: pre-production firmware (2.x). Remove when the production modules ship. */
        return has_by_version(info, f);
    }
    switch (f) {
    case FCK_DRV_C5_SCAN:
        return (info->caps & FCK_DRV_C5_CAP_SCAN) != 0U;
    case FCK_DRV_C5_CHANNEL:
        return (info->caps & FCK_DRV_C5_CAP_CHAN) != 0U;
    case FCK_DRV_C5_BLE_SCAN:
        return (info->caps & FCK_DRV_C5_CAP_BLE) != 0U;
    case FCK_DRV_C5_BLE_ADVERTISEMENT:
        return (info->caps & FCK_DRV_C5_CAP_BLEADV) != 0U;
    case FCK_DRV_C5_FRAMES:
        return (info->caps & FCK_DRV_C5_CAP_FRAMES) != 0U;
    case FCK_DRV_C5_WAKE_PIN:
        return (info->caps & FCK_DRV_C5_CAP_WP) != 0U;
    case FCK_DRV_C5_GNSS_TRANSMIT:
        return (info->caps & FCK_DRV_C5_CAP_GTX) != 0U;
    case FCK_DRV_C5_GNSS_RELAY:
    case FCK_DRV_C5_GNSS_REPEAT:
        return (info->caps & FCK_DRV_C5_CAP_GNSS) != 0U;
    case FCK_DRV_C5_HEALTH:
        return (info->caps & FCK_DRV_C5_CAP_HEALTH) != 0U;
    default:
        return false;
    }
}

/* A `gtx` that would move the receiver to another baud rate: the firmware does
 * not follow it, and the module would lose the receiver. fck_drv_c5_command_gnss_transmit()
 * refuses the body; so must a line written by hand, with `-n` or without,
 * and with the '$' the firmware drops. */
static bool gtx_moves_baud(const char *text)
{
    size_t      len;
    const char *b = word(text, 1U, &len);
    if (b != NULL && word_is(b, len, "-n")) {
        b = word(text, 2U, &len);
    }
    if (b != NULL && len > 0U && *b == '$') {
        b++;
        len--;
    }
    return b != NULL && len >= 9U && memcmp(b, "POLCFGPRT", 9U) == 0;
}

/* Whether @p text, which fck_drv_c5_command() took, is in the firmware's vocabulary:
 * seven words every one of them shares, `ble` only as `ble scan`, and
 * `health` for one that says it has it. (TEMPORARY, pre-production: what 1.6
 * added - `ble raw`, `sniff`, `pcap`, `deauth`, `probe` - is out.) */
static bool in_vocabulary(const fck_drv_c5_info_t *info, const char *text)
{
    static const char *const verbs[] = { "info", "scan", "chan", "gnss", "wp" };
    size_t      len;
    const char *w = word(text, 0U, &len);
    if (w == NULL) {
        return false;
    }
    if (word_is(w, len, "ble")) {
        const char *s = word(text, 1U, &len);
        return s != NULL && word_is(s, len, "scan");
    }
    if (word_is(w, len, "gtx")) {
        return !gtx_moves_baud(text);
    }
    if (word_is(w, len, "health")) {
        return fck_drv_c5_has(info, FCK_DRV_C5_HEALTH);
    }
    for (size_t i = 0U; i < sizeof(verbs) / sizeof(verbs[0]); i++) {
        if (word_is(w, len, verbs[i])) {
            return true;
        }
    }
    return false;
}

fck_drv_c5_gate_t fck_drv_c5_gate(const fck_drv_c5_info_t *info, const char *text, const char *verb,
                          fck_drv_c5_command_t *c)
{
    if (info == NULL || text == NULL || c == NULL) {
        return FCK_DRV_C5_GATE_REFUSE;
    }
    if (!fck_drv_c5_is_official(info->fw)) {
        return (verb != NULL) ? FCK_DRV_C5_GATE_AS_IS : FCK_DRV_C5_GATE_REFUSE;
    }
    fck_drv_c5_command_t k;
    if (fck_drv_c5_command(&k, text) != KODE_OK || !in_vocabulary(info, text)
        || (k.emits && fck_drv_c5_is_unlimited(info))
        || (verb != NULL && strcmp(verb, k.verb) != 0)) {
        return FCK_DRV_C5_GATE_REFUSE;
    }
    *c = k;
    return FCK_DRV_C5_GATE_OFFICIAL;
}

const char *fck_drv_c5_security_name(fck_drv_c5_security_t a)
{
    return ((unsigned)a < sizeof(s_auth) / sizeof(s_auth[0])) ? s_auth[a] : "unknown";
}

static fck_drv_c5_security_t auth_of(const fck_drv_link_message_t *m)
{
    char a[16];
    if (fck_drv_link_message_text(m, "auth", a, sizeof(a), NULL)) {
        for (unsigned i = 0U; i < (unsigned)FCK_DRV_C5_SECURITY_UNKNOWN; i++) {
            if (strcmp(a, s_auth[i]) == 0) {
                return (fck_drv_c5_security_t)i;
            }
        }
    }
    return FCK_DRV_C5_SECURITY_UNKNOWN;
}

bool fck_drv_c5_parse_network(const fck_drv_link_message_t *m, fck_drv_c5_network_t *out)
{
    if (!is_tag(m, "AP") || out == NULL) {
        return false;
    }
    fck_drv_c5_network_t ap;
    memset(&ap, 0, sizeof(ap));
    size_t  len = 0U;
    int32_t rssi;
    if (!fck_drv_link_message_text(m, "ssid", ap.ssid, sizeof(ap.ssid), &len)
        || !fck_drv_link_message_mac(m, "bssid", ap.bssid) || !channel(m, &ap.ch, &ap.band)
        || !fck_drv_link_message_integer(m, "rssi", &rssi)) {
        return false;
    }
    ap.ssid_len = (uint8_t)len;
    ap.rssi     = dbm(rssi);
    ap.auth     = auth_of(m);
    *out = ap;
    return true;
}

bool fck_drv_c5_parse_channel(const fck_drv_link_message_t *m, fck_drv_c5_channel_t *out)
{
    if (!is_tag(m, "CHAN") || out == NULL) {
        return false;
    }
    fck_drv_c5_channel_t c;
    memset(&c, 0, sizeof(c));
    int32_t max;
    int32_t avg;
    int32_t dwell;
    if (!channel(m, &c.ch, &c.band) || !count(m, "frames", &c.frames)
        || !count(m, "mgmt", &c.mgmt) || !count(m, "data", &c.data)
        || !count(m, "ctrl", &c.ctrl) || !fck_drv_link_message_integer(m, "rssi_max", &max)
        || !fck_drv_link_message_integer(m, "rssi_avg", &avg)) {
        return false;
    }
    c.rssi_max = dbm(max);
    c.rssi_avg = dbm(avg);
    if (fck_drv_link_message_integer(m, "dwell_ms", &dwell) && dwell >= 0 && dwell <= (int32_t)UINT16_MAX) {
        c.dwell_ms = (uint16_t)dwell;
    }
    *out = c;
    return true;
}

/* One hexadecimal digit of a #BLE's data=: its value, or false for anything else. */
static bool ble_data_digit(char c, uint8_t *value)
{
    bool ok = true;
    if ((c >= '0') && (c <= '9')) {
        *value = (uint8_t)((uint8_t)c - (uint8_t)'0');
    } else if ((c >= 'a') && (c <= 'f')) {
        *value = (uint8_t)(((uint8_t)c - (uint8_t)'a') + 10U);
    } else if ((c >= 'A') && (c <= 'F')) {
        *value = (uint8_t)(((uint8_t)c - (uint8_t)'A') + 10U);
    } else {
        ok = false;
    }
    return ok;
}

/* A #BLE's data=: the advertisement's bytes, two digits each. None when the
 * key is missing, and none when its value is not whole bytes or is more than
 * one advertisement holds: half of what a device sent would read as another
 * device's advertisement. */
static uint8_t ble_data(const fck_drv_link_message_t *m, uint8_t out[FCK_DRV_C5_BLE_DATA_MAX])
{
    char   text[(2U * FCK_DRV_C5_BLE_DATA_MAX) + 1U];
    size_t len = 0U;
    if ((!fck_drv_link_message_text(m, "data", text, sizeof(text), &len)) || ((len % 2U) != 0U)) {
        return 0U;
    }
    size_t const byte_count = len / 2U;
    uint8_t      bytes[FCK_DRV_C5_BLE_DATA_MAX] = { 0 };
    for (size_t i = 0U; i < byte_count; i++) {
        uint8_t hi = 0U;
        uint8_t lo = 0U;
        if ((!ble_data_digit(text[2U * i], &hi)) || (!ble_data_digit(text[(2U * i) + 1U], &lo))) {
            return 0U;
        }
        bytes[i] = (uint8_t)((uint8_t)(hi << 4) | lo);
    }
    (void)memcpy(out, bytes, byte_count);
    return (uint8_t)byte_count;
}

bool fck_drv_c5_parse_ble(const fck_drv_link_message_t *m, fck_drv_c5_ble_t *out)
{
    if (!is_tag(m, "BLE") || out == NULL) {
        return false;
    }
    fck_drv_c5_ble_t b;
    memset(&b, 0, sizeof(b));
    int32_t rssi;
    int32_t adv;
    char    type[4];
    size_t  len = 0U;
    if (!fck_drv_link_message_mac(m, "addr", b.addr) || !fck_drv_link_message_integer(m, "rssi", &rssi)) {
        return false;
    }
    b.rssi   = dbm(rssi);
    b.random = fck_drv_link_message_text(m, "type", type, sizeof(type), NULL) && strcmp(type, "rnd") == 0;
    if (fck_drv_link_message_text(m, "name", b.name, sizeof(b.name), &len)) {
        b.name_len = (uint8_t)len;
    }
    if (fck_drv_link_message_integer(m, "adv", &adv) && adv >= 0) {
        b.adv = (adv > (int32_t)UINT16_MAX) ? UINT16_MAX : (uint16_t)adv;
    }
    /* What 2.2 added. */
    int32_t evt;
    b.event = FCK_DRV_C5_BLE_EVENT_UNKNOWN;
    if (fck_drv_link_message_integer(m, "evt", &evt) && (evt >= 0) && (evt < (int32_t)FCK_DRV_C5_BLE_EVENT_UNKNOWN)) {
        b.event = (fck_drv_c5_ble_event_t)evt;
    }
    (void)fck_drv_link_message_unsigned(m, "first", &b.first_ms);
    (void)fck_drv_link_message_unsigned(m, "last", &b.last_ms);
    b.data_len = ble_data(m, b.data);
    *out = b;
    return true;
}

/* One character of base64's standard alphabet: its six bits, or false. */
static bool base64_digit(char c, uint8_t *value)
{
    bool ok = true;
    if ((c >= 'A') && (c <= 'Z')) {
        *value = (uint8_t)((uint8_t)c - (uint8_t)'A');
    } else if ((c >= 'a') && (c <= 'z')) {
        *value = (uint8_t)(((uint8_t)c - (uint8_t)'a') + 26U);
    } else if ((c >= '0') && (c <= '9')) {
        *value = (uint8_t)(((uint8_t)c - (uint8_t)'0') + 52U);
    } else if (c == '+') {
        *value = 62U;
    } else if (c == '/') {
        *value = 63U;
    } else {
        ok = false;
    }
    return ok;
}

/* Base64 of the standard alphabet with `=` padding, whole: groups of four characters, padding only
 * at the end of the last one (one or two `=`), and the bits the padding leaves over all zero, as
 * any encoder writes them. Up to @p cap bytes into @p out, their count in *n; false for anything
 * else, including more bytes than @p cap. */
static bool base64_decode(const char *text, size_t length, uint8_t *out, size_t cap, size_t *n)
{
    if ((length == 0U) || ((length % 4U) != 0U)) {
        return false;
    }
    size_t count = 0U;
    for (size_t at = 0U; at < length; at += 4U) {
        bool const last = (at + 4U == length);
        size_t     pads = 0U;
        uint8_t    v[4] = { 0U, 0U, 0U, 0U };
        for (size_t i = 0U; i < 4U; i++) {
            char const c = text[at + i];
            if ((c == '=') && last && (i >= 2U)) {
                pads++;
            } else if ((pads != 0U) || (!base64_digit(c, &v[i]))) {
                return false;       /* a character that is none, or one after the padding */
            }
        }
        size_t const bytes = 3U - pads;
        if ((count + bytes) > cap) {
            return false;
        }
        uint32_t const bits = ((uint32_t)v[0] << 18) | ((uint32_t)v[1] << 12) | ((uint32_t)v[2] << 6) | v[3];
        /* The bits of the last character(s) that no byte takes. */
        if (((pads == 1U) && ((bits & 0xFFU) != 0U)) || ((pads == 2U) && ((bits & 0xFFFFU) != 0U))) {
            return false;
        }
        for (size_t i = 0U; i < bytes; i++) {
            out[count++] = (uint8_t)(bits >> (16U - (8U * i)));
        }
    }
    *n = count;
    return true;
}

bool fck_drv_c5_parse_frame(const fck_drv_link_message_t *m, fck_drv_c5_frame_t *out)
{
    if (!is_tag(m, "F") || out == NULL) {
        return false;
    }
    /* The most a record of 70 bytes takes is 96 characters; a body longer than that is no record. */
    size_t const length = strlen(m->body);
    uint8_t      record[72];
    size_t       n = 0U;
    if ((length > 96U) || !base64_decode(m->body, length, record, sizeof(record), &n)
        || (n < FCK_DRV_C5_FRAME_HEADER_LEN)
        || (n > (FCK_DRV_C5_FRAME_HEADER_LEN + FCK_DRV_C5_FRAME_HEAD_SENT_MAX))) {
        return false;
    }
    fck_drv_c5_frame_t f;
    (void)memset(&f, 0, sizeof(f));
    f.fc[0]   = record[0];
    f.fc[1]   = record[1];
    f.channel = record[2];
    f.rssi    = (int8_t)record[3];
    f.len     = (uint16_t)((uint16_t)record[4] | (uint16_t)((uint16_t)record[5] << 8));
    (void)memcpy(f.addr1, &record[6], sizeof(f.addr1));
    (void)memcpy(f.addr2, &record[12], sizeof(f.addr2));
    (void)memcpy(f.addr3, &record[18], sizeof(f.addr3));
    f.head_len = (uint8_t)(n - FCK_DRV_C5_FRAME_HEADER_LEN);
    (void)memcpy(f.head, &record[FCK_DRV_C5_FRAME_HEADER_LEN], f.head_len);
    *out = f;
    return true;
}

bool fck_drv_c5_parse_frames_summary(const fck_drv_link_message_t *m, fck_drv_c5_frames_summary_t *out)
{
    if (!is_tag(m, "FRAMES") || out == NULL) {
        return false;
    }
    fck_drv_c5_frames_summary_t s;
    (void)memset(&s, 0, sizeof(s));
    int32_t ch;
    if (!fck_drv_link_message_integer(m, "ch", &ch) || ch < 1 || ch > 255 || !count(m, "ms", &s.ms)
        || !count(m, "heard", &s.heard) || !count(m, "mgmt", &s.mgmt) || !count(m, "data", &s.data)
        || !count(m, "ctrl", &s.ctrl) || !count(m, "beacon", &s.beacon)
        || !count(m, "probe_req", &s.probe_request) || !count(m, "probe_resp", &s.probe_response)
        || !count(m, "deauth", &s.deauth) || !count(m, "disassoc", &s.disassoc)
        || !count(m, "sent", &s.sent) || !count(m, "dropped", &s.dropped)) {
        return false;
    }
    s.ch = (uint8_t)ch;
    *out = s;
    return true;
}

bool fck_drv_c5_parse_summary(const fck_drv_link_message_t *m, fck_drv_c5_summary_t *out)
{
    if (!(is_tag(m, "SCANDONE") || is_tag(m, "CHANDONE") || is_tag(m, "BLEDONE"))
        || out == NULL) {
        return false;
    }
    fck_drv_c5_summary_t s;
    if (!count(m, "n", &s.n) || !count(m, "ms", &s.ms)) {
        return false;
    }
    *out = s;
    return true;
}

bool fck_drv_c5_parse_wake_pin(const fck_drv_link_message_t *m, fck_drv_c5_wake_pin_t *out)
{
    if (!is_tag(m, "WP") || out == NULL) {
        return false;
    }
    fck_drv_c5_wake_pin_t w = { false, false, false };
    if (!flag(m, "host_wp_c5", &w.host_wp_c5) || !flag(m, "c5_wp_host", &w.c5_wp_host)) {
        return false;
    }
    (void)flag(m, "driven", &w.driven);
    *out = w;
    return true;
}

bool fck_drv_c5_parse_pps(const fck_drv_link_message_t *m, fck_drv_c5_pps_t *out)
{
    if (!is_tag(m, "PPS") || out == NULL) {
        return false;
    }
    fck_drv_c5_pps_t p;
    if (!count(m, "n", &p.n) || !uptime_ms(m, "t_ms", &p.t_ms)) {
        return false;
    }
    *out = p;
    return true;
}

bool fck_drv_c5_parse_gnss_transmit(const fck_drv_link_message_t *m, char *sent, size_t cap)
{
    /* "$" + the longest body + 1.3's "*CS", and the terminator. */
    char   s[FCK_DRV_C5_GNSS_TRANSMIT_MAX + 8U];
    size_t n = 0U;
    if (!is_tag(m, "GTX") || sent == NULL
        || !fck_drv_link_message_text(m, "sent", s, sizeof(s), &n) || n + 1U > cap) {
        return false;
    }
    memcpy(sent, s, n + 1U);
    return true;
}

bool fck_drv_c5_parse_gnss_transmit_loop(const fck_drv_link_message_t *m, bool *ok, uint32_t *got)
{
    bool     k;
    uint32_t g;
    if (!is_tag(m, "GTXLOOP") || ok == NULL || got == NULL || !flag(m, "ok", &k)
        || !count(m, "got", &g)) {
        return false;
    }
    *ok  = k;
    *got = g;
    return true;
}

/* ---- What the C5 prints from a reset on --------------------------------------- */

static bool starts_with(const char *s, const char *prefix)
{
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

/* "I (35) boot: ESP-IDF v6.1 2nd stage bootloader", at any of ESP-IDF's five
 * levels: where its tag starts; NULL for a line of another kind. */
static const char *log_tag(const char *s)
{
    if ((s[0] == '\0') || (strchr("EWIDV", s[0]) == NULL) || (s[1] != ' ') || (s[2] != '(')) {
        return NULL;
    }
    const char *const close = strchr(s, ')');
    if ((close == NULL) || (close[1] != ' ')) {
        return NULL;
    }
    return &close[2];
}

static bool bootloader_tag(const char *tag)
{
    /* The tags ESP-IDF's second-stage bootloader logs with. An image's own log
     * lines are the image's. */
    static const char *const BOOTLOADER_TAGS[] = {
        "boot:", "boot.", "esp_image:", "qio_mode:", "flash_encrypt:", "secure_boot:",
    };

    for (size_t i = 0U; i < (sizeof(BOOTLOADER_TAGS) / sizeof(BOOTLOADER_TAGS[0])); i++) {
        if (starts_with(tag, BOOTLOADER_TAGS[i])) {
            return true;
        }
    }
    return false;
}

/* @p s into @p dst, trailing blanks left out, cut to FCK_DRV_C5_BOOT_LINE_MAX. */
static void keep_line(char *dst, const char *s)
{
    size_t n = strlen(s);
    while ((n > 0U) && (s[n - 1U] == ' ')) {
        n--;
    }
    if (n > FCK_DRV_C5_BOOT_LINE_MAX) {
        n = FCK_DRV_C5_BOOT_LINE_MAX;
    }
    (void)memcpy(dst, s, n);
    dst[n] = '\0';
}

static void boot_line(fck_drv_c5_boot_t *b, const char *line)
{
    /* How the ROM's lines start, its first left out: the ESP32-C5's on
     * a KDFCK001 rev 1.0, and what Espressif's ROMs print when there is no
     * image to start, when one is damaged, or when they wait for a download. */
    static const char *const ROM_LINES[] = {
        "Build:", "rst:", "SPI mode:", "SPIWP:", "load:", "entry 0x", "invalid header",
        "Saved PC:", "csum err", "waiting for download", "ets ",
    };

    const char *s = line;
    while (*s == ' ') {
        s++;
    }
    if (*s == '\0') {
        return;
    }
    if (starts_with(s, "ESP-ROM:")) {
        b->restarts++;
        return;
    }
    for (size_t i = 0U; i < (sizeof(ROM_LINES) / sizeof(ROM_LINES[0])); i++) {
        if (starts_with(s, ROM_LINES[i])) {
            b->entry = b->entry || starts_with(s, "entry 0x");
            if (starts_with(s, "invalid header")) {
                b->no_image++;
            }
            return;
        }
    }
    const char *const tag = log_tag(s);
    if (tag == NULL) {
        keep_line(b->line, s);
    } else if (bootloader_tag(tag)) {
        b->bootloader = true;
    } else if (b->log_line[0] == '\0') {
        keep_line(b->log_line, s);
    } else {
        /* The image's later log lines: its first is kept. */
    }
}

void fck_drv_c5_boot_init(fck_drv_c5_boot_t *b)
{
    if (b != NULL) {
        (void)memset(b, 0, sizeof(*b));
    }
}

bool fck_drv_c5_boot_done(const fck_drv_c5_boot_t *b)
{
    return (b == NULL) || (b->line[0] != '\0') || (b->restarts >= FCK_DRV_C5_BOOT_LOOP)
           || (b->no_image >= FCK_DRV_C5_BOOT_LOOP);
}

const char *fck_drv_c5_boot_line(const fck_drv_c5_boot_t *b)
{
    if (b == NULL) {
        return "";
    }
    return (b->line[0] != '\0') ? b->line : b->log_line;
}

void fck_drv_c5_boot_feed(fck_drv_c5_boot_t *b, const uint8_t *data, size_t n)
{
    if (data == NULL) {
        return;
    }
    for (size_t i = 0U; (i < n) && !fck_drv_c5_boot_done(b); i++) {
        uint8_t const c = data[i];
        if (b->escape) {
            /* An escape sequence ends at its final byte: "\x1b[0;32m". */
            b->escape = (c < 0x40U) || (c > 0x7EU) || (c == (uint8_t)'[');
        } else if (c == 0x1BU) {
            b->escape = true;
        } else if (c == (uint8_t)'\n') {
            b->part[b->n] = '\0';
            boot_line(b, b->part);
            b->n = 0U;
        } else if ((c >= 0x20U) && (c <= 0x7EU) && ((b->n + 1U) < sizeof(b->part))) {
            b->part[b->n] = (char)c;
            b->n++;
        } else {
            /* '\r', noise from the reset, or what runs past part: left out. */
        }
    }
}
