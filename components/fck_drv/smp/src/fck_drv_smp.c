/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file fck_drv_smp.c
 * @brief SMP's packets, frames and CBOR. See fck_drv_smp.h.
 */
#include "fck_drv_smp.h"

#include <string.h>

#define OP_READ    0U
#define OP_WRITE   2U
#define GRP_OS     0U
#define GRP_IMG    1U
#define ID_ECHO    0U
#define ID_RESET   5U
#define ID_STATE   0U
#define ID_UPLOAD  1U
#define HDR        8U
#define ECHO_MAX   64U
#define DEPTH_MAX  8

/* ---- CRC and base64 ------------------------------------------------------------- */

static uint16_t crc16_xmodem(const uint8_t *p, size_t n)
{
    uint16_t c = 0U;
    for (size_t k = 0U; k < n; k++) {
        c ^= (uint16_t)((uint16_t)p[k] << 8);
        for (int i = 0; i < 8; i++) {
            c = (c & 0x8000U) ? (uint16_t)((c << 1) ^ 0x1021U) : (uint16_t)(c << 1);
        }
    }
    return c;
}

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static size_t b64_encode(const uint8_t *in, size_t n, uint8_t *out)
{
    size_t o = 0U;
    for (size_t i = 0U; i < n; i += 3U) {
        uint32_t v = (uint32_t)in[i] << 16;
        if (i + 1U < n) {
            v |= (uint32_t)in[i + 1U] << 8;
        }
        if (i + 2U < n) {
            v |= in[i + 2U];
        }
        out[o++] = (uint8_t)B64[(v >> 18) & 63U];
        out[o++] = (uint8_t)B64[(v >> 12) & 63U];
        out[o++] = (i + 1U < n) ? (uint8_t)B64[(v >> 6) & 63U] : (uint8_t)'=';
        out[o++] = (i + 2U < n) ? (uint8_t)B64[v & 63U] : (uint8_t)'=';
    }
    return o;
}

static int b64_value(uint8_t c)
{
    if (c >= 'A' && c <= 'Z') {
        return c - 'A';
    }
    if (c >= 'a' && c <= 'z') {
        return c - 'a' + 26;
    }
    if (c >= '0' && c <= '9') {
        return c - '0' + 52;
    }
    if (c == '+') {
        return 62;
    }
    if (c == '/') {
        return 63;
    }
    return -1;
}

/* Whole quads only, '=' only at the end. Returns false for anything else. */
static bool b64_decode(const uint8_t *in, size_t n, uint8_t *out, size_t cap, size_t *got)
{
    if ((n % 4U) != 0U) {
        return false;
    }
    size_t o = 0U;
    for (size_t i = 0U; i < n; i += 4U) {
        int    v[4];
        size_t pad = 0U;
        for (size_t k = 0U; k < 4U; k++) {
            if (in[i + k] == '=' && i + 4U == n && k >= 2U) {
                v[k] = 0;
                pad++;
            } else if (pad > 0U || (v[k] = b64_value(in[i + k])) < 0) {
                return false;
            }
        }
        uint32_t const w = ((uint32_t)v[0] << 18) | ((uint32_t)v[1] << 12)
                           | ((uint32_t)v[2] << 6) | (uint32_t)v[3];
        size_t const bytes = 3U - pad;
        if (o + bytes > cap) {
            return false;
        }
        out[o++] = (uint8_t)(w >> 16);
        if (bytes > 1U) {
            out[o++] = (uint8_t)(w >> 8);
        }
        if (bytes > 2U) {
            out[o++] = (uint8_t)w;
        }
    }
    *got = o;
    return true;
}

/* ---- A little CBOR out ------------------------------------------------------------ */

typedef struct {
    uint8_t *p;
    size_t   n;
    size_t   cap;
    bool     over;
} wr_t;

static void put(wr_t *w, uint8_t b)
{
    if (w->n < w->cap) {
        w->p[w->n++] = b;
    } else {
        w->over = true;
    }
}

static void head(wr_t *w, uint8_t major, uint32_t v)
{
    uint8_t const m = (uint8_t)(major << 5);
    if (v < 24U) {
        put(w, (uint8_t)(m | v));
    } else if (v <= 0xFFU) {
        put(w, (uint8_t)(m | 24U));
        put(w, (uint8_t)v);
    } else if (v <= 0xFFFFU) {
        put(w, (uint8_t)(m | 25U));
        put(w, (uint8_t)(v >> 8));
        put(w, (uint8_t)v);
    } else {
        put(w, (uint8_t)(m | 26U));
        for (int s = 24; s >= 0; s -= 8) {
            put(w, (uint8_t)(v >> s));
        }
    }
}

static void bytes(wr_t *w, uint8_t major, const uint8_t *b, size_t n)
{
    head(w, major, (uint32_t)n);
    for (size_t k = 0U; k < n; k++) {
        put(w, b[k]);
    }
}

static void text(wr_t *w, const char *s)
{
    bytes(w, 3U, (const uint8_t *)s, strlen(s));
}

static void boolean(wr_t *w, bool v)
{
    put(w, v ? 0xF5U : 0xF4U);
}

/* The header in front of what the writer @p w wrote at pkt + HDR. */
static kode_status_t seal(fck_drv_smp_request_t *r, const wr_t *w, uint8_t op, uint16_t grp,
                          uint8_t seq, uint8_t id)
{
    if (w->over) {
        r->len = 0U;
        return KODE_DENIED;
    }
    r->pkt[0] = op;
    r->pkt[1] = 0U;
    r->pkt[2] = (uint8_t)(w->n >> 8);
    r->pkt[3] = (uint8_t)w->n;
    r->pkt[4] = (uint8_t)(grp >> 8);
    r->pkt[5] = (uint8_t)grp;
    r->pkt[6] = seq;
    r->pkt[7] = id;
    r->len    = HDR + w->n;
    return KODE_OK;
}

static wr_t body(fck_drv_smp_request_t *r)
{
    wr_t w = { .p = r->pkt + HDR, .cap = sizeof(r->pkt) - HDR };
    return w;
}

kode_status_t fck_drv_smp_request_echo(fck_drv_smp_request_t *r, uint8_t seq, const char *s)
{
    if (r == NULL || s == NULL || strlen(s) > ECHO_MAX) {
        return KODE_DENIED;
    }
    wr_t w = body(r);
    head(&w, 5U, 1U);
    text(&w, "d");
    text(&w, s);
    return seal(r, &w, OP_WRITE, GRP_OS, seq, ID_ECHO);
}

kode_status_t fck_drv_smp_request_state(fck_drv_smp_request_t *r, uint8_t seq)
{
    if (r == NULL) {
        return KODE_DENIED;
    }
    wr_t w = body(r);
    head(&w, 5U, 0U);
    return seal(r, &w, OP_READ, GRP_IMG, seq, ID_STATE);
}

kode_status_t fck_drv_smp_request_upload(fck_drv_smp_request_t *r, uint8_t seq, const uint8_t *img, size_t len,
                                 size_t off, size_t *took)
{
    if (r == NULL || img == NULL || took == NULL || len == 0U || off >= len) {
        return KODE_DENIED;
    }
    size_t const k = (len - off < FCK_DRV_SMP_CHUNK) ? len - off : FCK_DRV_SMP_CHUNK;
    wr_t         w = body(r);
    head(&w, 5U, (off == 0U) ? 4U : 2U);
    if (off == 0U) {
        text(&w, "image");
        head(&w, 0U, 0U);
        text(&w, "len");
        head(&w, 0U, (uint32_t)len);
    }
    text(&w, "off");
    head(&w, 0U, (uint32_t)off);
    text(&w, "data");
    bytes(&w, 2U, img + off, k);
    kode_status_t const st = seal(r, &w, OP_WRITE, GRP_IMG, seq, ID_UPLOAD);
    if (st == KODE_OK) {
        *took = k;
    }
    return st;
}

kode_status_t fck_drv_smp_request_test(fck_drv_smp_request_t *r, uint8_t seq, const uint8_t *hash,
                               size_t hash_len)
{
    if (r == NULL || hash == NULL || hash_len == 0U || hash_len > FCK_DRV_SMP_HASH_MAX) {
        return KODE_DENIED;
    }
    wr_t w = body(r);
    head(&w, 5U, 2U);
    text(&w, "hash");
    bytes(&w, 2U, hash, hash_len);
    text(&w, "confirm");
    boolean(&w, false);
    return seal(r, &w, OP_WRITE, GRP_IMG, seq, ID_STATE);
}

kode_status_t fck_drv_smp_request_confirm(fck_drv_smp_request_t *r, uint8_t seq)
{
    if (r == NULL) {
        return KODE_DENIED;
    }
    wr_t w = body(r);
    head(&w, 5U, 1U);
    text(&w, "confirm");
    boolean(&w, true);
    return seal(r, &w, OP_WRITE, GRP_IMG, seq, ID_STATE);
}

kode_status_t fck_drv_smp_request_reset(fck_drv_smp_request_t *r, uint8_t seq)
{
    if (r == NULL) {
        return KODE_DENIED;
    }
    wr_t w = body(r);
    head(&w, 5U, 0U);
    return seal(r, &w, OP_WRITE, GRP_OS, seq, ID_RESET);
}

size_t fck_drv_smp_frame(const fck_drv_smp_request_t *r, uint8_t *out, size_t cap)
{
    if (r == NULL || out == NULL || r->len < HDR || r->len > FCK_DRV_SMP_PACKET_MAX) {
        return 0U;
    }
    uint8_t raw[FCK_DRV_SMP_PACKET_MAX + 4U];
    uint8_t b64[((FCK_DRV_SMP_PACKET_MAX + 4U + 2U) / 3U) * 4U];
    raw[0] = (uint8_t)((r->len + 2U) >> 8);
    raw[1] = (uint8_t)(r->len + 2U);
    memcpy(raw + 2, r->pkt, r->len);
    uint16_t const c = crc16_xmodem(r->pkt, r->len);
    raw[2U + r->len] = (uint8_t)(c >> 8);
    raw[3U + r->len] = (uint8_t)c;
    size_t const n = b64_encode(raw, r->len + 4U, b64);
    size_t       o = 0U;
    for (size_t off = 0U; off < n; off += FCK_DRV_SMP_BASE64_PER) {
        size_t const k = (n - off < FCK_DRV_SMP_BASE64_PER) ? n - off : FCK_DRV_SMP_BASE64_PER;
        if (o + k + 3U > cap) {
            return 0U;
        }
        out[o++] = (off == 0U) ? 0x06U : 0x04U;
        out[o++] = (off == 0U) ? 0x09U : 0x14U;
        memcpy(out + o, b64 + off, k);
        o += k;
        out[o++] = '\n';
    }
    return o;
}

/* ---- Reading the wire ------------------------------------------------------------- */

void fck_drv_smp_receive_init(fck_drv_smp_receive_t *rx)
{
    if (rx != NULL) {
        memset(rx, 0, sizeof(*rx));
    }
}

static bool starts(uint8_t a, uint8_t b)
{
    return (a == 0x06U && b == 0x09U) || (a == 0x04U && b == 0x14U);
}

/* One whole line that starts as a frame's piece does. */
static size_t rx_line(fck_drv_smp_receive_t *rx, uint8_t *out, size_t cap)
{
    if (rx->line[0] == 0x06U) {
        rx->in_pkt = true;
        rx->an     = 0U;
    } else if (!rx->in_pkt) {
        rx->dropped++;
        return 0U;
    }
    size_t k = rx->ln - 2U;
    while (k > 0U && rx->line[2U + k - 1U] == '\r') {
        k--;
    }
    size_t got = 0U;
    if (!b64_decode(rx->line + 2, k, rx->acc + rx->an, sizeof(rx->acc) - rx->an, &got)) {
        rx->dropped++;
        rx->in_pkt = false;
        return 0U;
    }
    rx->an += got;
    if (rx->an < 2U) {
        return 0U;
    }
    size_t const want = (((size_t)rx->acc[0] << 8) | rx->acc[1]) + 2U;
    if (want < HDR + 4U || want > sizeof(rx->acc)) {
        rx->dropped++;
        rx->in_pkt = false;
        return 0U;
    }
    if (rx->an < want) {
        return 0U;
    }
    rx->in_pkt = false;
    size_t const   pn  = want - 4U;
    uint16_t const crc = (uint16_t)(((uint16_t)rx->acc[want - 2U] << 8) | rx->acc[want - 1U]);
    if (rx->an != want || crc16_xmodem(rx->acc + 2, pn) != crc || pn > cap) {
        rx->dropped++;
        return 0U;
    }
    memcpy(out, rx->acc + 2, pn);
    return pn;
}

size_t fck_drv_smp_receive_byte(fck_drv_smp_receive_t *rx, uint8_t b, uint8_t *out, size_t cap)
{
    if (rx == NULL || out == NULL) {
        return 0U;
    }
    uint8_t const prev = rx->prev;
    rx->prev           = b;
    if (starts(prev, b)) {
        /* A piece starts here, whatever came before it on the line: the
         * firmware's shell may have left its prompt. Neither byte is base64's,
         * nor text the firmware writes. */
        rx->line[0] = prev;
        rx->line[1] = b;
        rx->ln      = 2U;
        rx->over    = false;
        return 0U;
    }
    if (b != '\n') {
        if (rx->ln < sizeof(rx->line)) {
            rx->line[rx->ln++] = b;
        } else {
            rx->over = true;
        }
        return 0U;
    }
    size_t n = 0U;
    if (!rx->over && rx->ln >= 2U && starts(rx->line[0], rx->line[1])) {
        n = rx_line(rx, out, cap);
    }
    rx->ln   = 0U;
    rx->over = false;
    return n;
}

/* ---- The text between the packets ------------------------------------------------ */

#define TEXT_PROMPT "uart:~$ "

void fck_drv_smp_text_init(fck_drv_smp_text_t *t)
{
    if (t != NULL) {
        (void)memset(t, 0, sizeof(*t));
    }
}

static bool text_starts(const char *p, const char *word)
{
    return strncmp(p, word, strlen(word)) == 0;
}

/* MCUboot's name anywhere in @p p, in either case. */
static bool text_names_mcuboot(const char *p)
{
    static const char lower[] = "mcuboot";
    static const char upper[] = "MCUBOOT";
    for (size_t i = 0U; p[i] != '\0'; i++) {
        size_t k = 0U;
        while ((lower[k] != '\0') && ((p[i + k] == lower[k]) || (p[i + k] == upper[k]))) {
            k++;
        }
        if (lower[k] == '\0') {
            return true;
        }
    }
    return false;
}

static fck_drv_smp_line_t text_line(fck_drv_smp_text_t *t, char *out, size_t cap)
{
    bool const frame = t->frame;
    size_t     end   = t->n;
    size_t     at    = 0U;
    t->line[end] = '\0';
    t->n         = 0U;
    t->esc       = 0U;
    t->frame     = false;
    while (t->line[at] == ' ') {
        at++;
    }
    while (text_starts(&t->line[at], TEXT_PROMPT)) {
        at += sizeof(TEXT_PROMPT) - 1U;
    }
    while ((end > at) && (t->line[end - 1U] == ' ')) {
        end--;
    }
    t->line[end] = '\0';
    const char *const p = &t->line[at];
    if (frame || (end == at) || text_starts(p, "*** Using")) {
        return FCK_DRV_SMP_LINE_NONE;
    }
    if (text_names_mcuboot(p)) {
        return FCK_DRV_SMP_LINE_BOOTLOADER;
    }
    if (text_starts(p, "*** Booting")) {
        return FCK_DRV_SMP_LINE_BANNER;
    }
    if ((out != NULL) && (cap > 0U)) {
        (void)strncpy(out, p, cap - 1U);
        out[cap - 1U] = '\0';
    }
    return FCK_DRV_SMP_LINE_TEXT;
}

fck_drv_smp_line_t fck_drv_smp_text_byte(fck_drv_smp_text_t *t, uint8_t b, char *out, size_t cap)
{
    if (t == NULL) {
        return FCK_DRV_SMP_LINE_NONE;
    }
    uint8_t const prev = t->prev;
    t->prev            = b;
    if (starts(prev, b)) {
        t->frame = true;
    }
    if (b == (uint8_t)'\n') {
        return text_line(t, out, cap);
    }
    if (t->esc == 1U) {
        t->esc = (b == (uint8_t)'[') ? 2U : 0U;
    } else if (t->esc == 2U) {
        t->esc = ((b >= 0x40U) && (b <= 0x7EU)) ? 0U : 2U;
    } else if (b == 0x1BU) {
        t->esc = 1U;
    } else if ((b >= 0x20U) && (b <= 0x7EU) && (t->n < FCK_DRV_SMP_TEXT_MAX)) {
        t->line[t->n] = (char)b;
        t->n++;
    } else {
        /* '\r', a stray byte, or past what is kept: dropped. */
    }
    return FCK_DRV_SMP_LINE_NONE;
}

/* ---- A little CBOR in ------------------------------------------------------------- */

typedef enum {
    MAP_TOP,     /* the answer's own map                     */
    MAP_ERR,     /* "err": SMP version 2's error             */
    MAP_IMAGE,   /* one entry of "images"                    */
    MAP_OTHER,   /* anything else: read past                 */
} map_t;

typedef struct {
    const uint8_t *p;
    size_t         n;
    size_t         i;
    bool           bad;
} rd_t;

/* The initial byte and its argument. An indefinite length is *indef. */
static bool rd_head(rd_t *r, uint8_t *major, uint64_t *arg, bool *indef)
{
    if (r->bad || r->i >= r->n) {
        r->bad = true;
        return false;
    }
    uint8_t const ib = r->p[r->i++];
    uint8_t const ai = ib & 31U;
    *major = ib >> 5;
    *indef = false;
    *arg   = ai;
    if (ai < 24U) {
        return true;
    }
    if (ai == 31U) {
        *indef = true;
        return true;
    }
    size_t const k = (ai == 24U) ? 1U : (ai == 25U) ? 2U : (ai == 26U) ? 4U : (ai == 27U) ? 8U : 0U;
    if (k == 0U || r->i + k > r->n) {
        r->bad = true;
        return false;
    }
    uint64_t v = 0U;
    for (size_t j = 0U; j < k; j++) {
        v = (v << 8) | r->p[r->i++];
    }
    *arg = v;
    return true;
}

static bool at_break(const rd_t *r)
{
    return r->i < r->n && r->p[r->i] == 0xFFU;
}

static bool is_key(const char *key, const char *name)
{
    return key != NULL && strcmp(key, name) == 0;
}

static void keep_text(char *dst, size_t cap, const uint8_t *s, size_t n)
{
    size_t const k = (n < cap - 1U) ? n : cap - 1U;
    memcpy(dst, s, k);
    dst[k] = '\0';
}

static void rd_entries(rd_t *r, fck_drv_smp_response_t *o, map_t kind, int img, int depth, uint64_t v,
                       bool indef);

/* One value, under @p key of a map of @p kind. */
static void rd_value(rd_t *r, fck_drv_smp_response_t *o, map_t kind, int img, const char *key, int depth)
{
    uint8_t  major;
    uint64_t v;
    bool     indef;
    if (depth > DEPTH_MAX || !rd_head(r, &major, &v, &indef)) {
        r->bad = true;
        return;
    }
    switch (major) {
    case 0U:
    case 1U: {
        if (indef) {
            r->bad = true;
            return;
        }
        if (v > INT32_MAX) {
            return;   /* no key this file keeps is that large */
        }
        int32_t const s = (major == 0U) ? (int32_t)v : -1 - (int32_t)v;
        if ((kind == MAP_TOP || kind == MAP_ERR) && is_key(key, "rc")) {
            o->has_rc = true;
            o->rc     = s;
        } else if (kind == MAP_TOP && is_key(key, "off") && major == 0U) {
            o->has_off = true;
            o->off     = (uint32_t)v;
        } else if (kind == MAP_IMAGE && is_key(key, "slot") && s >= 0 && s <= INT8_MAX) {
            o->image[img].slot = (int8_t)s;
        }
        return;
    }
    case 2U:
    case 3U: {
        if (indef || v > r->n - r->i) {
            r->bad = true;   /* chunked strings: no server here writes them */
            return;
        }
        const uint8_t *const s = r->p + r->i;
        r->i += (size_t)v;
        if (major == 2U && kind == MAP_IMAGE && is_key(key, "hash") && v <= FCK_DRV_SMP_HASH_MAX) {
            memcpy(o->image[img].hash, s, (size_t)v);
            o->image[img].hash_len = (uint8_t)v;
        } else if (major == 3U && kind == MAP_IMAGE && is_key(key, "version")) {
            keep_text(o->image[img].version, sizeof(o->image[img].version), s, (size_t)v);
        } else if (major == 3U && kind == MAP_TOP && is_key(key, "r")) {
            keep_text(o->echo, sizeof(o->echo), s, (size_t)v);
        }
        return;
    }
    case 4U: {
        bool const images = (kind == MAP_TOP && is_key(key, "images"));
        for (uint64_t j = 0U; !r->bad && (indef ? !at_break(r) : j < v); j++) {
            if (r->i >= r->n) {
                r->bad = true;
                return;
            }
            if (images && (r->p[r->i] >> 5) == 5U && o->images < FCK_DRV_SMP_IMAGES_MAX) {
                uint8_t  m;
                uint64_t mv;
                bool     mi;
                (void)rd_head(r, &m, &mv, &mi);
                int const e      = o->images++;
                o->image[e].slot = -1;
                rd_entries(r, o, MAP_IMAGE, e, depth + 1, mv, mi);
            } else {
                rd_value(r, o, MAP_OTHER, -1, NULL, depth + 1);
            }
        }
        if (indef && !r->bad) {
            r->i++;   /* the break */
        }
        return;
    }
    case 5U:
        rd_entries(r, o, (kind == MAP_TOP && is_key(key, "err")) ? MAP_ERR : MAP_OTHER, img,
                   depth + 1, v, indef);
        return;
    case 6U:
        rd_value(r, o, kind, img, key, depth + 1);   /* a tag: what it tags */
        return;
    default:   /* 7: false, true, null, and floats */
        if (indef) {
            r->bad = true;   /* a break where a value should be */
            return;
        }
        if (kind == MAP_IMAGE && (v == 20U || v == 21U)) {
            bool const b = (v == 21U);
            if (is_key(key, "active")) {
                o->image[img].active = b;
            } else if (is_key(key, "confirmed")) {
                o->image[img].confirmed = b;
            } else if (is_key(key, "pending")) {
                o->image[img].pending = b;
            } else if (is_key(key, "bootable")) {
                o->image[img].bootable = b;
            }
        }
        return;
    }
}

/* A map's entries, its head read. The answer's own (depth 0) may end with
 * the data and no break: MCUboot's serial recovery writes it so. */
static void rd_entries(rd_t *r, fck_drv_smp_response_t *o, map_t kind, int img, int depth, uint64_t v,
                       bool indef)
{
    if (depth > DEPTH_MAX) {
        r->bad = true;
        return;
    }
    for (uint64_t j = 0U; !r->bad && (indef ? !at_break(r) : j < v); j++) {
        if (r->i >= r->n) {
            if (!(indef && depth == 0)) {
                r->bad = true;
            }
            return;
        }
        char          key[16] = "";
        uint8_t const ib      = r->p[r->i];
        if ((ib >> 5) == 3U && (ib & 31U) != 31U) {
            uint8_t  km;
            uint64_t kl;
            bool     ki;
            if (!rd_head(r, &km, &kl, &ki) || kl > r->n - r->i) {
                r->bad = true;
                return;
            }
            if (kl < sizeof(key)) {   /* longer than any key kept: not one of them */
                keep_text(key, sizeof(key), r->p + r->i, (size_t)kl);
            }
            r->i += (size_t)kl;
        } else {
            rd_value(r, o, MAP_OTHER, -1, NULL, depth + 1);
        }
        rd_value(r, o, kind, img, (key[0] != '\0') ? key : NULL, depth + 1);
    }
    if (indef && !r->bad && r->i < r->n) {
        r->i++;   /* the break */
    }
}

bool fck_drv_smp_parse(const uint8_t *pkt, size_t n, fck_drv_smp_response_t *out)
{
    if (pkt == NULL || out == NULL || n < HDR
        || (((size_t)pkt[2] << 8) | pkt[3]) != n - HDR) {
        return false;
    }
    fck_drv_smp_response_t o;
    memset(&o, 0, sizeof(o));
    o.op    = pkt[0];
    o.group = (uint16_t)(((uint16_t)pkt[4] << 8) | pkt[5]);
    o.seq   = pkt[6];
    o.id    = pkt[7];
    if (n > HDR) {
        rd_t     r = { .p = pkt + HDR, .n = n - HDR };
        uint8_t  major;
        uint64_t v;
        bool     indef;
        if (!rd_head(&r, &major, &v, &indef) || major != 5U) {
            return false;
        }
        rd_entries(&r, &o, MAP_TOP, -1, 0, v, indef);
        if (r.bad) {
            return false;
        }
    }
    *out = o;
    return true;
}

bool fck_drv_smp_answers(const fck_drv_smp_response_t *rsp, const fck_drv_smp_request_t *req)
{
    return rsp != NULL && req != NULL && req->len >= HDR && rsp->op == req->pkt[0] + 1U
           && rsp->group == (uint16_t)(((uint16_t)req->pkt[4] << 8) | req->pkt[5])
           && rsp->seq == req->pkt[6] && rsp->id == req->pkt[7];
}

const fck_drv_smp_image_t *fck_drv_smp_slot(const fck_drv_smp_response_t *rsp, int slot)
{
    if (rsp == NULL) {
        return NULL;
    }
    for (uint8_t k = 0U; k < rsp->images && k < FCK_DRV_SMP_IMAGES_MAX; k++) {
        if (rsp->image[k].slot == slot) {
            return &rsp->image[k];
        }
    }
    return NULL;
}

/* ---- The image ---------------------------------------------------------------- */

static uint32_t le(const uint8_t *p, size_t n)
{
    uint32_t v = 0U;
    for (size_t k = n; k > 0U; k--) {
        v = (v << 8) | p[k - 1U];
    }
    return v;
}

/* The first TLV of the unprotected area whose type @p want takes, with a
 * value of 1 to @p max bytes. MCUboot's header: IMAGE_MAGIC 0x96f3b83d, then
 * hdr_size at 8 and img_size at 12; the TLV areas follow the image, the
 * protected one (0x6908) first, each counting its own 4-byte head. */
static bool tlv_find(const uint8_t *img, size_t len, bool (*want)(uint32_t type), size_t max,
                     const uint8_t **val, size_t *val_len)
{
    if (img == NULL || len < 32U || le(img, 4U) != 0x96F3B83DU) {
        return false;
    }
    size_t o = (size_t)le(img + 8, 2U) + (size_t)le(img + 12, 4U);
    while (o + 4U <= len) {
        uint32_t const magic = le(img + o, 2U);
        size_t const   end   = o + le(img + o + 2U, 2U);
        if ((magic != 0x6907U && magic != 0x6908U) || end > len || end < o + 4U) {
            return false;
        }
        for (size_t t = o + 4U; t + 4U <= end;) {
            uint32_t const type = le(img + t, 2U);
            size_t const   l    = le(img + t + 2U, 2U);
            if (t + 4U + l > end) {
                return false;
            }
            if (magic == 0x6907U && want(type) && l > 0U && l <= max) {
                *val     = img + t + 4U;
                *val_len = l;
                return true;
            }
            t += 4U + l;
        }
        o = end;
    }
    return false;
}

/* IMAGE_TLV_SHA256, _SHA384 and _SHA512. */
static bool is_hash(uint32_t type)
{
    return type == 0x10U || type == 0x11U || type == 0x12U;
}

/* IMAGE_TLV_RSA2048_PSS, _ECDSA224, _ECDSA_SIG, _RSA3072_PSS and _ED25519. */
static bool is_signature(uint32_t type)
{
    return type >= 0x20U && type <= 0x24U;
}

static bool is_ed25519(uint32_t type)
{
    return type == 0x24U;
}

bool fck_drv_smp_image_hash(const uint8_t *img, size_t len, const uint8_t **hash, size_t *hash_len)
{
    if (hash == NULL || hash_len == NULL) {
        return false;
    }
    return tlv_find(img, len, is_hash, FCK_DRV_SMP_HASH_MAX, hash, hash_len);
}

bool fck_drv_smp_image_suits(const uint8_t *img, size_t len, fck_drv_smp_loader_t loader)
{
    const uint8_t *v;
    size_t         n;
    if (!tlv_find(img, len, is_hash, FCK_DRV_SMP_HASH_MAX, &v, &n)) {
        return false;
    }
    if (loader == FCK_DRV_SMP_LOADER_HASH) {
        /* It checks the SHA-256 and nothing else: a signature beside it
         * would be for another loader's hash. */
        return n == 32U && !tlv_find(img, len, is_signature, 1024U, &v, &n);
    }
    /* A loader that answers no echo boots only an ED25519 signature. */
    return loader == FCK_DRV_SMP_LOADER_SIGNED && n == 64U && tlv_find(img, len, is_ed25519, 1024U, &v, &n);
}

/* The header's two sizes, checked against the bytes given: false for what is
 * not an MCUboot image, or one cut short before its code ends. */
static bool image_code(const uint8_t *img, size_t len, size_t *code, size_t *code_len)
{
    if ((img == NULL) || (len < 32U) || (le(img, 4U) != 0x96F3B83DU)) {
        return false;
    }
    size_t const header = le(&img[8], 2U);
    size_t const length = le(&img[12], 4U);
    if ((header < 32U) || (header > len) || (length > (len - header))) {
        return false;
    }
    *code     = header;
    *code_len = length;
    return true;
}

fck_drv_smp_image_check_t fck_drv_smp_image_check(const uint8_t *img, size_t len, uint32_t slot,
                                                  uint32_t image_max, uint32_t ram,
                                                  uint32_t ram_size)
{
    if ((img == NULL) || (len < 32U) || (le(img, 4U) != 0x96F3B83DU)) {
        return FCK_DRV_SMP_IMAGE_NOT_MCUBOOT;
    }
    size_t code;
    size_t code_len;
    if ((!image_code(img, len, &code, &code_len)) || (code_len < 16U)) {
        return FCK_DRV_SMP_IMAGE_SIZES;
    }
    if (len > image_max) {
        return FCK_DRV_SMP_IMAGE_LARGER;
    }
    /* The stack grows down from its first value: anywhere above the RAM's
     * start, up to its very end, on a word boundary. */
    uint32_t const sp = le(&img[code], 4U);
    if ((sp <= ram) || ((sp - ram) > ram_size) || ((sp & 3U) != 0U)) {
        return FCK_DRV_SMP_IMAGE_STACK;
    }
    /* The reset handler, the NMI and the hard fault, Thumb addresses inside
     * the code as it lies in the slot. Only the reset handler must be there;
     * the other two may be left at 0. */
    uint32_t const start = slot + (uint32_t)code;
    for (size_t k = 1U; k <= 3U; k++) {
        uint32_t const v        = le(&img[code + (4U * k)], 4U);
        uint32_t const at       = v & ~1U;
        bool const     left_out = (v == 0U) && (k != 1U);
        bool const     outside  = ((v & 1U) == 0U) || (at < start) || ((at - start) >= (uint32_t)code_len);
        if (!left_out && outside) {
            return FCK_DRV_SMP_IMAGE_ADDRESS;
        }
    }
    return FCK_DRV_SMP_IMAGE_FITS;
}

bool fck_drv_smp_image_map(const uint8_t *img, size_t len, uint32_t *address)
{
    /* 'F' 'C' 'K' 'M' 'A' 'P' 0x00 0x01, then the address, little endian:
     * firmware/fck-nrf, fck_nrf.h. Looked for at every byte: where the
     * linker puts it is its own business. */
    static const uint8_t MAGIC[8] = { 0x46U, 0x43U, 0x4BU, 0x4DU, 0x41U, 0x50U, 0x00U, 0x01U };
    size_t code;
    size_t code_len;
    if (address == NULL || !image_code(img, len, &code, &code_len)) {
        return false;
    }
    for (size_t k = 0U; (k + sizeof(MAGIC) + 4U) <= code_len; k++) {
        if (memcmp(&img[code + k], MAGIC, sizeof(MAGIC)) == 0) {
            *address = le(&img[code + k + sizeof(MAGIC)], 4U);
            return true;
        }
    }
    return false;
}

bool fck_drv_smp_image_holds(const uint8_t *img, size_t len, uint32_t word)
{
    size_t code;
    size_t code_len;
    if (!image_code(img, len, &code, &code_len)) {
        return false;
    }
    for (size_t k = 0U; (k + 4U) <= code_len; k += 4U) {
        if (le(&img[code + k], 4U) == word) {
            return true;
        }
    }
    return false;
}
