/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file fck_drv_nmea.c
 * @brief The GNSS receiver's sentences, read into a fix. See fck_drv_nmea.h.
 *
 * Plain C, like fck_drv_link.c (tools/check_layers.sh, rule 4). Numbers are
 * read digit by digit rather than with strtod(): a field that is not a
 * number must refuse the sentence, not read as half of one, and the
 * minutes of a coordinate are kept as an integer until the last division.
 */
#include "fck_drv_nmea.h"

#include <math.h>
#include <string.h>

/* Powers of ten up to the most digits a field is read with. */
static const int64_t k_pow10[] = {
    1LL,          10LL,          100LL,          1000LL,
    10000LL,      100000LL,      1000000LL,      10000000LL,
    100000000LL,  1000000000LL,  10000000000LL,  100000000000LL,
    1000000000000LL,
};
#define FRAC_MAX  12U   /* digits after the point */

/* ---- Fields ---------------------------------------------------------------- */

/* The n-th comma-separated field of a checked sentence, from 0, the address
 * ("GNGGA"): where it starts and its length. NULL past the last one. */
static const char *field(const char *s, unsigned n, size_t *len)
{
    const char *p = s + 1;
    for (;;) {
        size_t l = strcspn(p, ",*");
        if (n == 0U) {
            *len = l;
            return p;
        }
        if (p[l] != ',') {
            return NULL;
        }
        p += l + 1U;
        n--;
    }
}

/* Digits with at most one point: all of them as one integer, and how many
 * came after the point. A '-' first only when @p sign. */
static bool decimal(const char *f, size_t len, bool sign, int64_t *mant, unsigned *frac)
{
    size_t   i      = 0U;
    bool     neg    = false;
    bool     point  = false;
    bool     digits = false;
    int64_t  m      = 0;
    unsigned d      = 0U;
    if (sign && len > 0U && f[0] == '-') {
        neg = true;
        i   = 1U;
    }
    for (; i < len; i++) {
        char c = f[i];
        if (c == '.' && !point) {
            point = true;
            continue;
        }
        if (c < '0' || c > '9' || m > (INT64_MAX - 9) / 10) {
            return false;
        }
        m      = m * 10 + (c - '0');
        digits = true;
        if (point) {
            d++;
        }
    }
    if (!digits || d > FRAC_MAX) {
        return false;
    }
    *mant = neg ? -m : m;
    *frac = d;
    return true;
}

/* A number that may be empty, and NAN then. false when it is there and is
 * not one. */
static bool number(const char *f, size_t len, bool sign, double *out)
{
    if (len == 0U) {
        *out = NAN;
        return true;
    }
    int64_t  m;
    unsigned d;
    if (!decimal(f, len, sign, &m, &d)) {
        return false;
    }
    *out = (double)m / (double)k_pow10[d];
    return true;
}

/* A whole number up to @p max that may be empty, and 0 then. */
static bool whole(const char *f, size_t len, uint32_t max, uint32_t *out)
{
    uint32_t v = 0U;
    if (len > 9U) {
        return false;
    }
    for (size_t i = 0U; i < len; i++) {
        if (f[i] < '0' || f[i] > '9') {
            return false;
        }
        v = v * 10U + (uint32_t)(f[i] - '0');
    }
    if (v > max) {
        return false;
    }
    *out = v;
    return true;
}

/* Two digits at @p f, up to @p max. */
static bool two(const char *f, unsigned max, uint8_t *out)
{
    if (f[0] < '0' || f[0] > '9' || f[1] < '0' || f[1] > '9') {
        return false;
    }
    unsigned v = (unsigned)(f[0] - '0') * 10U + (unsigned)(f[1] - '0');
    if (v > max) {
        return false;
    }
    *out = (uint8_t)v;
    return true;
}

/* ddmm.mmmm (lat) or dddmm.mmmm (lon) and its hemisphere, into degrees.
 * Both empty: NAN, no position. */
static bool coord(const char *f, size_t len, const char *h, size_t hlen, bool lon,
                  double *out)
{
    if (len == 0U && hlen == 0U) {
        *out = NAN;
        return true;
    }
    int64_t  m;
    unsigned d;
    if (hlen != 1U || !decimal(f, len, false, &m, &d)) {
        return false;
    }
    char hemi = h[0];
    if (lon ? (hemi != 'E' && hemi != 'W') : (hemi != 'N' && hemi != 'S')) {
        return false;
    }
    int64_t unit = k_pow10[d];
    int64_t deg  = m / (100 * unit);
    int64_t mins = m - deg * 100 * unit;          /* minutes, times unit */
    if (mins >= 60 * unit || deg > (lon ? 180 : 90)
        || (deg == (lon ? 180 : 90) && mins != 0)) {
        return false;
    }
    double v = (double)deg + (double)mins / (60.0 * (double)unit);
    *out     = (hemi == 'S' || hemi == 'W') ? -v : v;
    return true;
}

/* hhmmss, with any fraction of a second after it. Empty: no time. */
static bool utc_time(const char *f, size_t len, fck_drv_nmea_fix_t *x)
{
    if (len == 0U) {
        x->has_time = false;
        return true;
    }
    uint8_t h;
    uint8_t mi;
    uint8_t s;
    if (len < 6U || !two(f, 23U, &h) || !two(f + 2, 59U, &mi) || !two(f + 4, 60U, &s)) {
        return false;
    }
    unsigned ms = 0U;
    if (len > 6U) {
        if (f[6] != '.' || len == 7U) {
            return false;
        }
        unsigned scale = 100U;
        for (size_t i = 7U; i < len; i++) {
            if (f[i] < '0' || f[i] > '9') {
                return false;
            }
            ms += (unsigned)(f[i] - '0') * scale;   /* past the third digit, scale is 0 */
            scale /= 10U;
        }
    }
    x->has_time = true;
    x->hour     = h;
    x->min      = mi;
    x->sec      = s;
    x->ms       = (uint16_t)ms;
    return true;
}

/* ddmmyy. Empty: no date. */
static bool utc_date(const char *f, size_t len, fck_drv_nmea_fix_t *x)
{
    if (len == 0U) {
        x->has_date = false;
        return true;
    }
    uint8_t d;
    uint8_t mo;
    uint8_t y;
    if (len != 6U || !two(f, 31U, &d) || !two(f + 2, 12U, &mo) || !two(f + 4, 99U, &y)
        || d == 0U || mo == 0U) {
        return false;
    }
    x->has_date = true;
    x->day      = d;
    x->month    = mo;
    x->year     = (uint16_t)(2000U + y);
    return true;
}

/* The fields of one sentence, up to the n-th, all present. */
typedef struct {
    const char *p[10];
    size_t      len[10];
} fields_t;

static bool split(const char *s, unsigned n, fields_t *f)
{
    for (unsigned i = 0U; i <= n; i++) {
        if ((f->p[i] = field(s, i, &f->len[i])) == NULL) {
            return false;
        }
    }
    return true;
}

/* ---- The two sentences ----------------------------------------------------- */

/* $--GGA,time,lat,N,lon,E,quality,sats,hdop,alt,M,... */
static bool gga(const char *s, fck_drv_nmea_fix_t *fix)
{
    fields_t       f;
    fck_drv_nmea_fix_t x = *fix;
    uint32_t       q;
    uint32_t       n;
    if (!split(s, 9U, &f) || !utc_time(f.p[1], f.len[1], &x)
        || !coord(f.p[2], f.len[2], f.p[3], f.len[3], false, &x.lat)
        || !coord(f.p[4], f.len[4], f.p[5], f.len[5], true, &x.lon)
        || !whole(f.p[6], f.len[6], 9U, &q) || !whole(f.p[7], f.len[7], 99U, &n)
        || !number(f.p[8], f.len[8], false, &x.hdop)
        || !number(f.p[9], f.len[9], true, &x.alt_m)) {
        return false;
    }
    /* Half a position is no position. */
    if (isnan(x.lat) != isnan(x.lon)) {
        return false;
    }
    x.quality = (uint8_t)q;
    x.sats    = (uint8_t)n;
    *fix      = x;
    return true;
}

/* $--RMC,time,status,lat,N,lon,E,speed,course,date,... */
static bool rmc(const char *s, fck_drv_nmea_fix_t *fix)
{
    fields_t       f;
    fck_drv_nmea_fix_t x = *fix;
    if (!split(s, 9U, &f) || !utc_time(f.p[1], f.len[1], &x) || f.len[2] > 1U
        || !coord(f.p[3], f.len[3], f.p[4], f.len[4], false, &x.lat)
        || !coord(f.p[5], f.len[5], f.p[6], f.len[6], true, &x.lon)
        || !number(f.p[7], f.len[7], false, &x.speed_kn)
        || !number(f.p[8], f.len[8], false, &x.course_deg)
        || !utc_date(f.p[9], f.len[9], &x)) {
        return false;
    }
    if (isnan(x.lat) != isnan(x.lon)) {
        return false;
    }
    x.valid = (f.len[2] == 1U && f.p[2][0] == 'A');
    *fix    = x;
    return true;
}

/* ---- The reader ------------------------------------------------------------ */

void fck_drv_nmea_init(fck_drv_nmea_t *n)
{
    if (n == NULL) {
        return;
    }
    memset(n, 0, sizeof(*n));
    n->fix.lat        = NAN;
    n->fix.lon        = NAN;
    n->fix.alt_m      = NAN;
    n->fix.hdop       = NAN;
    n->fix.speed_kn   = NAN;
    n->fix.course_deg = NAN;
}

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    return -1;
}

bool fck_drv_nmea_checksum_ok(const char *s)
{
    if (s == NULL || s[0] != '$') {
        return false;
    }
    uint8_t x = 0U;
    size_t  i = 1U;
    for (; s[i] != '\0' && s[i] != '*'; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x20U || c > 0x7EU || c == '$') {
            return false;
        }
        x ^= c;
    }
    if (s[i] != '*') {
        return false;
    }
    int hi = hex_digit(s[i + 1U]);
    int lo = (hi >= 0) ? hex_digit(s[i + 2U]) : -1;
    if (hi < 0 || lo < 0) {
        return false;
    }
    const char *end = s + i + 3U;
    while (*end == '\r' || *end == '\n') {
        end++;
    }
    return *end == '\0' && ((hi << 4) | lo) == x;
}

fck_drv_nmea_kind_t fck_drv_nmea_feed(fck_drv_nmea_t *n, const char *s)
{
    if (n == NULL) {
        return FCK_DRV_NMEA_BAD;
    }
    if (!fck_drv_nmea_checksum_ok(s)) {
        n->bad++;
        return FCK_DRV_NMEA_BAD;
    }
    size_t      len;
    const char *addr = field(s, 0U, &len);
    if (len == 5U && memcmp(addr + 2, "GGA", 3U) == 0) {
        if (!gga(s, &n->fix)) {
            n->bad++;
            return FCK_DRV_NMEA_BAD;
        }
        n->gga++;
        return FCK_DRV_NMEA_GGA;
    }
    if (len == 5U && memcmp(addr + 2, "RMC", 3U) == 0) {
        if (!rmc(s, &n->fix)) {
            n->bad++;
            return FCK_DRV_NMEA_BAD;
        }
        n->rmc++;
        return FCK_DRV_NMEA_RMC;
    }
    n->other++;
    return FCK_DRV_NMEA_OTHER;
}
