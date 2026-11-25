/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file fck_drv_subghz_file.c
 * @brief fck_drv_subghz_file: Kode's .ksub files, written and read.
 *
 * The format is Kode's own (docs/ksub-format.md): nothing here follows
 * another program's file, so there is no number in this file to source.
 */
#include "fck_drv_subghz_file.h"

#include <string.h>

#define HEADER_LINE "kode-subghz 1"

/* ---- Writing ----------------------------------------------------------------------- */

typedef struct {
    char  *text;
    size_t cap;
    size_t length;
    bool   ok;
} writer_t;

static void put_char(writer_t *w, char c)
{
    if (!w->ok) {
        return;
    }
    if (w->length + 1U >= w->cap) {   /* room for the NUL as well */
        w->ok = false;
        return;
    }
    w->text[w->length++] = c;
}

static void put_text(writer_t *w, const char *s)
{
    while (*s != '\0') {
        put_char(w, *s++);
    }
}

static void put_unsigned(writer_t *w, uint32_t v)
{
    char   digits[10];
    size_t count = 0U;
    do {
        digits[count++] = (char)('0' + (v % 10U));
        v /= 10U;
    } while (v != 0U);
    while (count > 0U) {
        put_char(w, digits[--count]);
    }
}

static void put_signed(writer_t *w, int32_t v)
{
    if (v < 0) {
        put_char(w, '-');
        put_unsigned(w, 0U - (uint32_t)v);
    } else {
        put_unsigned(w, (uint32_t)v);
    }
}

static void put_hex(writer_t *w, uint32_t v, unsigned digits)
{
    /* A writer that already failed writes nothing, so nothing is shifted either: a caller's bad bit count
     * (more than 32) would ask for a ninth digit, and a shift of 32 is not defined for 32 bits. */
    if (!w->ok || digits > 8U) {
        w->ok = false;
        return;
    }
    for (unsigned i = digits; i > 0U; i--) {
        put_char(w, "0123456789abcdef"[(v >> (4U * (i - 1U))) & 0xFU]);
    }
}

static void writer_start(writer_t *w, char *text, size_t cap)
{
    w->text   = text;
    w->cap    = cap;
    w->length = 0U;
    w->ok     = (text != NULL) && (cap > 0U);
}

/* The NUL, or on a failure an empty string; the length written, or 0. */
static size_t writer_end(writer_t *w)
{
    if (w->text == NULL || w->cap == 0U) {
        return 0U;
    }
    if (!w->ok) {
        w->text[0] = '\0';
        return 0U;
    }
    w->text[w->length] = '\0';
    return w->length;
}

static void put_top(writer_t *w, const char *kind, uint32_t hz)
{
    put_text(w, HEADER_LINE "\nkind: ");
    put_text(w, kind);
    put_text(w, "\nfrequency: ");
    put_unsigned(w, hz);
    put_text(w, "\nmodulation: ook\n");
}

size_t fck_drv_subghz_file_write_raw(uint32_t hz, const int32_t *runs, size_t n, char *text, size_t cap)
{
    writer_t w;
    writer_start(&w, text, cap);
    if (runs == NULL || n == 0U || hz == 0U || ((uint64_t)n >> 32U) != 0U) {
        w.ok = false;
    }
    for (size_t i = 0U; w.ok && i < n; i++) {
        if (runs[i] == 0) {
            w.ok = false;
        }
    }
    put_top(&w, "raw", hz);
    put_text(&w, "pulses: ");
    put_unsigned(&w, (uint32_t)n);
    put_char(&w, '\n');
    for (size_t i = 0U; w.ok && i < n; i++) {
        if (i % FCK_DRV_SUBGHZ_FILE_RUNS_A_LINE == 0U) {
            put_text(&w, "raw:");
        }
        put_char(&w, ' ');
        put_signed(&w, runs[i]);
        if ((i + 1U) % FCK_DRV_SUBGHZ_FILE_RUNS_A_LINE == 0U || i + 1U == n) {
            put_char(&w, '\n');
        }
    }
    return writer_end(&w);
}

static bool name_char_ok(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
}

static bool name_ok(const char *name, size_t length)
{
    if (length == 0U || length >= FCK_DRV_SUBGHZ_FILE_NAME_MAX) {
        return false;
    }
    for (size_t i = 0U; i < length; i++) {
        if (!name_char_ok(name[i])) {
            return false;
        }
    }
    return true;
}

size_t fck_drv_subghz_file_write_code(uint32_t hz, const char *protocol, uint8_t bits, uint32_t code,
                                      uint16_t pulse_us, char *text, size_t cap)
{
    writer_t w;
    writer_start(&w, text, cap);
    size_t name_length = 0U;
    while (protocol != NULL && name_length < FCK_DRV_SUBGHZ_FILE_NAME_MAX && protocol[name_length] != '\0') {
        name_length++;
    }
    if (protocol == NULL || !name_ok(protocol, name_length) || hz == 0U
        || bits == 0U || bits > FCK_DRV_SUBGHZ_FILE_CODE_BITS || (bits < 32U && (code >> bits) != 0U)) {
        w.ok = false;
    }
    put_top(&w, "code", hz);
    put_text(&w, "protocol: ");
    if (protocol != NULL) {
        put_text(&w, protocol);
    }
    put_text(&w, "\nbits: ");
    put_unsigned(&w, bits);
    put_text(&w, "\ncode: ");
    put_hex(&w, code, (bits + 3U) / 4U);
    put_char(&w, '\n');
    if (pulse_us != 0U) {
        put_text(&w, "pulse: ");
        put_unsigned(&w, pulse_us);
        put_char(&w, '\n');
    }
    return writer_end(&w);
}

/* ---- Reading ----------------------------------------------------------------------- */

static bool is_blank(char c)
{
    return c == ' ' || c == '\t';
}

static char lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

/* A span of the text, never read past its end. */
typedef struct {
    const char *s;
    size_t      n;
} span_t;

static span_t trim(span_t v)
{
    while (v.n > 0U && is_blank(v.s[0])) {
        v.s++;
        v.n--;
    }
    while (v.n > 0U && is_blank(v.s[v.n - 1U])) {
        v.n--;
    }
    return v;
}

static bool span_is(span_t v, const char *word)
{
    size_t const n = strlen(word);
    if (v.n != n) {
        return false;
    }
    for (size_t i = 0U; i < n; i++) {
        if (lower(v.s[i]) != word[i]) {
            return false;
        }
    }
    return true;
}

/* A decimal number of at most @p max, no sign, no blank in it. */
static bool parse_unsigned(span_t v, uint32_t max, uint32_t *out)
{
    if (v.n == 0U) {
        return false;
    }
    uint64_t acc = 0U;
    for (size_t i = 0U; i < v.n; i++) {
        if (v.s[i] < '0' || v.s[i] > '9') {
            return false;
        }
        acc = acc * 10U + (uint64_t)(v.s[i] - '0');
        if (acc > max) {
            return false;
        }
    }
    *out = (uint32_t)acc;
    return true;
}

/* A run: a decimal number that is not 0, with a minus for a low. */
static bool parse_run(span_t v, int32_t *out)
{
    bool negative = false;
    if (v.n > 0U && v.s[0] == '-') {
        negative = true;
        v.s++;
        v.n--;
    }
    uint32_t magnitude = 0U;
    if (!parse_unsigned(v, negative ? 2147483648U : 2147483647U, &magnitude) || magnitude == 0U) {
        return false;
    }
    *out = negative ? (int32_t)(0U - magnitude) : (int32_t)magnitude;
    return true;
}

static bool parse_hex(span_t v, uint32_t *out)
{
    if (v.n == 0U || v.n > 8U) {
        return false;
    }
    uint32_t acc = 0U;
    for (size_t i = 0U; i < v.n; i++) {
        char const c = lower(v.s[i]);
        uint32_t   d;
        if (c >= '0' && c <= '9') {
            d = (uint32_t)(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            d = (uint32_t)(c - 'a') + 10U;
        } else {
            return false;
        }
        acc = (acc << 4) | d;
    }
    *out = acc;
    return true;
}

/* What the lines said so far. */
typedef struct {
    bool     has_kind, has_frequency, has_modulation, has_protocol, has_bits, has_code, has_pulse, has_pulses;
    bool     has_raw;
    fck_drv_subghz_file_kind_t kind;
    uint32_t frequency;
    char     protocol[FCK_DRV_SUBGHZ_FILE_NAME_MAX];
    uint32_t bits;
    uint32_t code;
    uint32_t pulse;
    uint32_t pulses;
    uint64_t counted;   /* runs on the raw lines, all of them */
    size_t   stored;    /* the first cap of them went to runs */
} reading_t;

static bool take_raw(span_t value, reading_t *r, int32_t *runs, size_t cap)
{
    size_t i = 0U;
    while (i < value.n) {
        while (i < value.n && is_blank(value.s[i])) {
            i++;
        }
        size_t const start = i;
        while (i < value.n && !is_blank(value.s[i])) {
            i++;
        }
        if (i == start) {
            break;
        }
        int32_t run;
        if (!parse_run((span_t){ value.s + start, i - start }, &run)) {
            return false;
        }
        if (r->counted >= r->pulses) {
            return false;   /* more runs than the file says it has */
        }
        if (r->stored < cap && runs != NULL) {
            runs[r->stored++] = run;
        }
        r->counted++;
    }
    return true;
}

/* One `key: value` line. */
static bool take_line(span_t line, reading_t *r, int32_t *runs, size_t cap)
{
    size_t colon = 0U;
    while (colon < line.n && line.s[colon] != ':') {
        colon++;
    }
    if (colon == line.n) {
        return false;   /* a line that is neither blank, a comment nor a key */
    }
    span_t const key   = trim((span_t){ line.s, colon });
    span_t const value = trim((span_t){ line.s + colon + 1U, line.n - colon - 1U });
    uint32_t     number = 0U;

    if (span_is(key, "kind")) {
        if (r->has_kind) {
            return false;
        }
        r->has_kind = true;
        if (span_is(value, "raw")) {
            r->kind = FCK_DRV_SUBGHZ_FILE_RAW;
        } else if (span_is(value, "code")) {
            r->kind = FCK_DRV_SUBGHZ_FILE_CODE;
        } else {
            return false;
        }
        return true;
    }
    if (span_is(key, "frequency")) {
        if (r->has_frequency || !parse_unsigned(value, 0xFFFFFFFFU, &number) || number == 0U) {
            return false;
        }
        r->has_frequency = true;
        r->frequency     = number;
        return true;
    }
    if (span_is(key, "modulation")) {
        if (r->has_modulation || !span_is(value, "ook")) {
            return false;
        }
        r->has_modulation = true;
        return true;
    }
    if (span_is(key, "protocol")) {
        if (r->has_protocol || !name_ok(value.s, value.n)) {
            return false;
        }
        r->has_protocol = true;
        memcpy(r->protocol, value.s, value.n);
        r->protocol[value.n] = '\0';
        return true;
    }
    if (span_is(key, "bits")) {
        if (r->has_bits || !parse_unsigned(value, FCK_DRV_SUBGHZ_FILE_CODE_BITS, &number) || number == 0U) {
            return false;
        }
        r->has_bits = true;
        r->bits     = number;
        return true;
    }
    if (span_is(key, "code")) {
        if (r->has_code || !parse_hex(value, &number)) {
            return false;
        }
        r->has_code = true;
        r->code     = number;
        return true;
    }
    if (span_is(key, "pulse")) {
        if (r->has_pulse || !parse_unsigned(value, 65535U, &number) || number == 0U) {
            return false;
        }
        r->has_pulse = true;
        r->pulse     = number;
        return true;
    }
    if (span_is(key, "pulses")) {
        if (r->has_pulses || !parse_unsigned(value, 0xFFFFFFFFU, &number) || number == 0U) {
            return false;
        }
        r->has_pulses = true;
        r->pulses     = number;
        return true;
    }
    if (span_is(key, "raw")) {
        if (!r->has_pulses) {
            return false;   /* the count comes first, so that no run is taken that it does not allow */
        }
        r->has_raw = true;
        return take_raw(value, r, runs, cap);
    }
    return true;   /* a key a later version adds */
}

/* One pass over the text: what it says into @p out, its first @p cap runs into
 * @p runs. false for a file refused, with some runs perhaps written. */
static bool read_pass(const char *text, size_t length, fck_drv_subghz_file_t *out, int32_t *runs, size_t cap)
{
    reading_t r;
    memset(&r, 0, sizeof(r));
    bool   first = true;
    size_t pos   = 0U;
    while (pos < length) {
        size_t end = pos;
        while (end < length && text[end] != '\n') {
            end++;
        }
        span_t line = { text + pos, end - pos };
        pos         = end + 1U;
        if (line.n > 0U && line.s[line.n - 1U] == '\r') {
            line.n--;
        }
        line = trim(line);
        if (line.n == 0U || line.s[0] == '#') {
            continue;
        }
        if (first) {
            first = false;
            if (!span_is(line, HEADER_LINE)) {
                return false;
            }
            continue;
        }
        if (!take_line(line, &r, runs, cap)) {
            return false;
        }
    }
    if (first || !r.has_kind || !r.has_frequency || !r.has_modulation) {
        return false;
    }
    fck_drv_subghz_file_t f;
    memset(&f, 0, sizeof(f));
    f.kind = r.kind;
    f.hz   = r.frequency;
    if (r.kind == FCK_DRV_SUBGHZ_FILE_RAW) {
        if (r.has_protocol || r.has_bits || r.has_code || r.has_pulse || !r.has_pulses || !r.has_raw
            || r.counted != r.pulses) {
            return false;
        }
        f.runs = r.stored;
        f.cut  = r.counted > (uint64_t)r.stored;
    } else {
        if (r.has_pulses || r.has_raw || !r.has_protocol || !r.has_bits || !r.has_code
            || (r.bits < 32U && (r.code >> r.bits) != 0U)) {
            return false;
        }
        memcpy(f.protocol, r.protocol, sizeof(f.protocol));
        f.bits     = (uint8_t)r.bits;
        f.code     = r.code;
        f.pulse_us = (uint16_t)r.pulse;
    }
    *out = f;
    return true;
}

bool fck_drv_subghz_file_read(const char *text, size_t length, fck_drv_subghz_file_t *out, int32_t *runs,
                              size_t cap)
{
    if (text == NULL || out == NULL || (runs == NULL && cap != 0U) || memchr(text, '\0', length) != NULL) {
        return false;
    }
    /* The whole file is checked first, writing no run, so that @p runs is
     * written only for a file that is taken; then its runs are read again
     * into @p runs. */
    fck_drv_subghz_file_t f;
    if (!read_pass(text, length, &f, NULL, 0U)) {
        return false;
    }
    if (f.kind == FCK_DRV_SUBGHZ_FILE_RAW && cap != 0U && !read_pass(text, length, &f, runs, cap)) {
        return false;   /* the same text: never */
    }
    *out = f;
    return true;
}
