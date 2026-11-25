/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file fck_drv_link.c
 * @brief The firmwares' line protocol, read. See fck_drv_link.h.
 */
#include "fck_drv_link.h"

#include <string.h>

/* Where fck_drv_link_feed() is in an ANSI escape sequence. */
enum { ESC_NONE = 0, ESC_START, ESC_CSI };

/* One token of a body: `key=value`, `key="quoted value"`, or a bare word. */
typedef struct {
    const char *key;
    size_t      klen;
    const char *val;     /* inside the quotes when quoted, still escaped */
    size_t      vlen;
    bool        has_eq;
    bool        quoted;
    bool        broken;  /* a quote that never closed */
} tok_t;

/* ---- Tokens ----------------------------------------------------------------- */

/* The token at p, or NULL when the body has no more. Spaces separate tokens,
 * and a quoted value runs to its closing quote, spaces and all; a backslash
 * hides the character after it, so `\"` does not close. */
static const char *next_tok(const char *p, tok_t *t)
{
    while (*p == ' ') {
        p++;
    }
    if (*p == '\0') {
        return NULL;
    }
    memset(t, 0, sizeof(*t));
    t->key = p;
    while (*p != '\0' && *p != ' ' && *p != '=') {
        p++;
    }
    t->klen = (size_t)(p - t->key);
    if (*p != '=') {
        return p;            /* a bare word */
    }
    t->has_eq = true;
    p++;
    if (*p == '"') {
        t->quoted = true;
        t->val = ++p;
        while (*p != '\0' && *p != '"') {
            if (*p == '\\' && p[1] != '\0') {
                p++;
            }
            p++;
        }
        t->vlen = (size_t)(p - t->val);
        if (*p == '"') {
            p++;
        } else {
            t->broken = true;
        }
        return p;
    }
    t->val = p;
    while (*p != '\0' && *p != ' ') {
        p++;
    }
    t->vlen = (size_t)(p - t->val);
    return p;
}

static bool find_key(const fck_drv_link_message_t *m, const char *key, tok_t *t)
{
    if (m == NULL || m->body == NULL || key == NULL) {
        return false;
    }
    size_t      klen = strlen(key);
    const char *p    = m->body;
    while ((p = next_tok(p, t)) != NULL) {
        if (t->has_eq && t->klen == klen && memcmp(t->key, key, klen) == 0) {
            return true;
        }
    }
    return false;
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

bool fck_drv_link_message_text(const fck_drv_link_message_t *m, const char *key, char *out, size_t cap,
                 size_t *len)
{
    if (out == NULL || cap == 0U) {
        return false;
    }
    out[0] = '\0';
    tok_t t;
    if (!find_key(m, key, &t) || t.broken) {
        return false;
    }
    size_t n = 0U;
    for (size_t i = 0U; i < t.vlen; i++) {
        char c = t.val[i];
        if (t.quoted && c == '\\') {
            if (i + 1U >= t.vlen) {
                out[0] = '\0';
                return false;
            }
            char e = t.val[++i];
            if (e == 'x') {
                bool two = (i + 2U < t.vlen);
                int  hi  = two ? hex_digit(t.val[i + 1U]) : -1;
                int  lo  = two ? hex_digit(t.val[i + 2U]) : -1;
                if (hi < 0 || lo < 0) {
                    out[0] = '\0';
                    return false;
                }
                c = (char)((hi << 4) | lo);
                i += 2U;
            } else {
                c = e;   /* \" and \\, and any other is the character itself */
            }
        }
        if (n + 1U >= cap) {
            out[0] = '\0';
            return false;
        }
        out[n++] = c;
    }
    out[n] = '\0';
    if (len != NULL) {
        *len = n;
    }
    return true;
}

bool fck_drv_link_message_has(const fck_drv_link_message_t *m, const char *key)
{
    tok_t t;
    return find_key(m, key, &t);
}

bool fck_drv_link_message_listed(const fck_drv_link_message_t *m, const char *key, const char *word)
{
    tok_t t;
    if (word == NULL || word[0] == '\0' || !find_key(m, key, &t)) {
        return false;
    }
    size_t const wlen = strlen(word);
    size_t       at   = 0U;
    while (at <= t.vlen) {
        size_t end = at;
        while (end < t.vlen && t.val[end] != ',') {
            end++;
        }
        if (end - at == wlen && memcmp(&t.val[at], word, wlen) == 0) {
            return true;
        }
        at = end + 1U;
    }
    return false;
}

bool fck_drv_link_message_integer(const fck_drv_link_message_t *m, const char *key, int32_t *out)
{
    tok_t t;
    if (out == NULL || !find_key(m, key, &t) || t.quoted || t.vlen == 0U) {
        return false;
    }
    const char *v   = t.val;
    size_t      n   = t.vlen;
    bool        neg = false;
    int64_t     acc = 0;
    if (n > 2U && v[0] == '0' && (v[1] == 'x' || v[1] == 'X')) {
        for (size_t i = 2U; i < n; i++) {
            int d = hex_digit(v[i]);
            if (d < 0) {
                return false;
            }
            acc = acc * 16 + d;
            if (acc > INT32_MAX) {
                return false;
            }
        }
    } else {
        size_t i = 0U;
        if (v[0] == '-') {
            neg = true;
            i   = 1U;
        }
        if (i == n) {
            return false;
        }
        for (; i < n; i++) {
            if (v[i] < '0' || v[i] > '9') {
                return false;
            }
            acc = acc * 10 + (v[i] - '0');
            if (acc > (int64_t)INT32_MAX + 1) {
                return false;
            }
        }
        if (neg) {
            acc = -acc;
        }
        if (acc > INT32_MAX || acc < INT32_MIN) {
            return false;
        }
    }
    *out = (int32_t)acc;
    return true;
}

bool fck_drv_link_message_unsigned(const fck_drv_link_message_t *m, const char *key, uint32_t *out)
{
    tok_t t;
    if (out == NULL || !find_key(m, key, &t) || t.quoted || t.vlen == 0U) {
        return false;
    }
    const char *v    = t.val;
    size_t      n    = t.vlen;
    unsigned    base = 10U;
    size_t      i    = 0U;
    uint64_t    acc  = 0U;
    if (n > 2U && v[0] == '0' && (v[1] == 'x' || v[1] == 'X')) {
        base = 16U;
        i    = 2U;
    }
    for (; i < n; i++) {
        int d = (base == 16U) ? hex_digit(v[i])
                              : ((v[i] >= '0' && v[i] <= '9') ? (v[i] - '0') : -1);
        if (d < 0) {
            return false;
        }
        acc = acc * base + (unsigned)d;
        if (acc > UINT32_MAX) {
            return false;
        }
    }
    *out = (uint32_t)acc;
    return true;
}

bool fck_drv_link_message_mac(const fck_drv_link_message_t *m, const char *key, uint8_t out[6])
{
    tok_t t;
    if (out == NULL || !find_key(m, key, &t) || t.quoted || t.vlen != 17U) {
        return false;
    }
    uint8_t mac[6];
    for (size_t i = 0U; i < 6U; i++) {
        const char *b  = t.val + i * 3U;
        int         hi = hex_digit(b[0]);
        int         lo = hex_digit(b[1]);
        if (hi < 0 || lo < 0 || (i < 5U && b[2] != ':')) {
            return false;
        }
        mac[i] = (uint8_t)((hi << 4) | lo);
    }
    memcpy(out, mac, sizeof(mac));
    return true;
}

bool fck_drv_link_message_word(const fck_drv_link_message_t *m, unsigned n, char *out, size_t cap)
{
    if (m == NULL || m->body == NULL || out == NULL || cap == 0U) {
        return false;
    }
    out[0] = '\0';
    tok_t       t;
    const char *p = m->body;
    while ((p = next_tok(p, &t)) != NULL) {
        if (t.has_eq) {
            continue;
        }
        if (n-- == 0U) {
            if (t.klen + 1U > cap) {
                return false;
            }
            memcpy(out, t.key, t.klen);
            out[t.klen] = '\0';
            return true;
        }
    }
    return false;
}

/* ---- The link ---------------------------------------------------------------- */

/* A tag or a verb: one word that fits. */
static bool word_ok(const char *w)
{
    size_t const n = strlen(w);
    return n > 0U && n <= FCK_DRV_LINK_TAG_MAX && strcspn(w, " \r\n") == n;
}

kode_status_t fck_drv_link_init(fck_drv_link_t *l, const fck_drv_link_config_t *cfg)
{
    if (l == NULL) {
        return KODE_DENIED;
    }
    memset(l, 0, sizeof(*l));
    if (cfg == NULL) {
        return KODE_OK;
    }
    size_t n = 0U;
    if (cfg->async != NULL) {
        for (; cfg->async[n] != NULL; n++) {
            if (n == FCK_DRV_LINK_ASYNC_MAX || strlen(cfg->async[n]) == 0U
                || strlen(cfg->async[n]) > FCK_DRV_LINK_TAG_MAX) {
                memset(l, 0, sizeof(*l));
                return KODE_DENIED;
            }
            l->async[n] = cfg->async[n];
        }
    }
    l->async[n] = NULL;
    if ((cfg->banner == NULL) != (cfg->banner_verb == NULL)
        || (cfg->banner != NULL && (!word_ok(cfg->banner) || !word_ok(cfg->banner_verb)))) {
        memset(l, 0, sizeof(*l));
        return KODE_DENIED;
    }
    if (cfg->banner != NULL) {
        memcpy(l->banner, cfg->banner, strlen(cfg->banner) + 1U);
        memcpy(l->banner_verb, cfg->banner_verb, strlen(cfg->banner_verb) + 1U);
    }
    l->cfg             = *cfg;
    l->cfg.async       = l->async;
    l->cfg.banner      = NULL;   /* the copies above are what is read */
    l->cfg.banner_verb = NULL;
    return KODE_OK;
}

void fck_drv_link_reset(fck_drv_link_t *l)
{
    if (l == NULL) {
        return;
    }
    l->len       = 0U;
    l->overflow  = false;
    l->esc       = ESC_NONE;
    l->verb[0]   = '\0';
    l->stale[0]  = '\0';
    l->on_reply  = NULL;
    l->reply_ctx = NULL;
    l->pending   = false;
    l->done      = false;
    l->restarted = false;
    l->rc        = 0;
}

bool fck_drv_link_idle(const fck_drv_link_t *l)
{
    return l != NULL && !l->pending && l->stale[0] == '\0';
}

kode_status_t fck_drv_link_begin(fck_drv_link_t *l, const char *verb,
                             fck_drv_link_message_fn on_reply, void *ctx)
{
    if (!fck_drv_link_idle(l) || verb == NULL) {
        return KODE_DENIED;
    }
    if (!word_ok(verb)) {
        return KODE_DENIED;
    }
    memcpy(l->verb, verb, strlen(verb) + 1U);
    l->on_reply  = on_reply;
    l->reply_ctx = ctx;
    l->pending   = true;
    l->done      = false;
    l->restarted = false;
    l->rc        = 0;
    return KODE_OK;
}

bool fck_drv_link_done(const fck_drv_link_t *l, int32_t *rc)
{
    if (l == NULL || !l->done) {
        return false;
    }
    if (rc != NULL) {
        *rc = l->rc;
    }
    return true;
}

bool fck_drv_link_restarted(const fck_drv_link_t *l)
{
    return l != NULL && l->done && l->restarted;
}

void fck_drv_link_abort(fck_drv_link_t *l)
{
    if (l == NULL || !l->pending) {
        return;
    }
    if (l->banner_verb[0] != '\0' && strcmp(l->verb, l->banner_verb) == 0) {
        /* Its late answer is only a banner, which on_banner() hears anyway;
         * and one sent while the chip was starting never gets a #DONE. */
        fck_drv_link_forget(l);
        return;
    }
    memcpy(l->stale, l->verb, sizeof(l->stale));
    l->pending  = false;
    l->done     = false;
    l->on_reply = NULL;
}

/* Past the spaces and any uptime word. */
static const char *skip_uptime(const char *s)
{
    for (;;) {
        while (*s == ' ') {
            s++;
        }
        if (strncmp(s, "uptime", 6) != 0) {
            return s;
        }
        s += strcspn(s, " ");
    }
}

bool fck_drv_link_same_banner(const char *a, const char *b)
{
    if (a == NULL || b == NULL) {
        return false;
    }
    for (;;) {
        a = skip_uptime(a);
        b = skip_uptime(b);
        if (*a == '\0' || *b == '\0') {
            return *a == *b;
        }
        size_t const la = strcspn(a, " ");
        size_t const lb = strcspn(b, " ");
        if (la != lb || memcmp(a, b, la) != 0) {
            return false;
        }
        a += la;
        b += lb;
    }
}

void fck_drv_link_forget(fck_drv_link_t *l)
{
    if (l == NULL) {
        return;
    }
    l->verb[0]   = '\0';
    l->stale[0]  = '\0';
    l->on_reply  = NULL;
    l->reply_ctx = NULL;
    l->pending   = false;
    l->done      = false;
    l->restarted = false;
    l->rc        = 0;
}

fck_drv_link_statistics_t fck_drv_link_statistics(const fck_drv_link_t *l)
{
    fck_drv_link_statistics_t none = { 0 };
    return (l != NULL) ? l->stats : none;
}

static bool is_async(const fck_drv_link_t *l, const char *tag)
{
    for (size_t i = 0U; l->async[i] != NULL; i++) {
        if (strcmp(l->async[i], tag) == 0) {
            return true;
        }
    }
    return false;
}

static void on_done(fck_drv_link_t *l, const fck_drv_link_message_t *msg)
{
    char    verb[FCK_DRV_LINK_TAG_MAX + 1U];
    int32_t rc = -1;
    if (!fck_drv_link_message_word(msg, 0U, verb, sizeof(verb))) {
        l->stats.stray_done++;
        return;
    }
    if (!fck_drv_link_message_integer(msg, "rc", &rc)) {
        rc = -1;
    }
    if (l->stale[0] != '\0' && strcmp(verb, l->stale) == 0) {
        l->stale[0] = '\0';  /* the late end of an aborted command */
        return;
    }
    if (l->pending && strcmp(verb, l->verb) == 0) {
        l->pending  = false;
        l->done     = true;
        l->rc       = rc;
        l->on_reply = NULL;
        return;
    }
    l->stats.stray_done++;
}

/* Where the machine part of a line starts, or NULL for a human line. A line
 * is a machine line when it starts with '#', or when a shell prompt with no
 * space in it comes first: the nRF's Zephyr shell can leave `uart:~$ ` in
 * front of a line it prints while redrawing its prompt. */
static const char *machine_start(const char *s)
{
    while (*s == ' ') {
        s++;
    }
    if (*s == '#') {
        return s;
    }
    const char *h = strstr(s, "$ #");
    if (h != NULL && memchr(s, ' ', (size_t)(h - s)) == NULL) {
        return h + 2;
    }
    return NULL;
}

/* A banner: always to on_banner; then the answer of `info` in flight, the
 * late answer of an abandoned `info`, or the chip restarting. */
static void on_banner(fck_drv_link_t *l, const fck_drv_link_message_t *msg)
{
    if (l->cfg.on_banner != NULL) {
        l->cfg.on_banner(l->cfg.banner_ctx, msg);
    }
    if (l->pending && strcmp(l->verb, l->banner_verb) == 0) {
        if (l->on_reply != NULL) {
            l->on_reply(l->reply_ctx, msg);
        }
        return;
    }
    if (l->stale[0] != '\0' && strcmp(l->stale, l->banner_verb) == 0) {
        l->stats.stale++;
        return;
    }
    if (l->pending) {
        l->pending   = false;
        l->done      = true;
        l->restarted = true;
        l->rc        = -1;
        l->on_reply  = NULL;
        l->stats.restarts++;
    } else if (l->stale[0] != '\0') {
        l->stale[0] = '\0';
        l->stats.restarts++;
    }
    if (l->cfg.on_async != NULL) {
        l->cfg.on_async(l->cfg.async_ctx, msg);
    }
}

static void on_line(fck_drv_link_t *l, char *line)
{
    const char *p = machine_start(line);
    size_t      n = (p != NULL) ? strcspn(p + 1, " ") : 0U;
    if (p == NULL || n == 0U || n > FCK_DRV_LINK_TAG_MAX) {
        if (l->cfg.on_text != NULL) {
            l->cfg.on_text(l->cfg.text_ctx, line);
        }
        return;
    }
    char tag[FCK_DRV_LINK_TAG_MAX + 1U];
    memcpy(tag, p + 1, n);
    tag[n] = '\0';
    const char *body = p + 1 + n;
    while (*body == ' ') {
        body++;
    }
    fck_drv_link_message_t msg = { .tag = tag, .body = body };

    if (strcmp(tag, "DONE") == 0) {
        on_done(l, &msg);
    } else if (l->banner[0] != '\0' && strcmp(tag, l->banner) == 0) {
        on_banner(l, &msg);
    } else if (!is_async(l, tag) && l->pending) {
        if (l->on_reply != NULL) {
            l->on_reply(l->reply_ctx, &msg);
        }
    } else if (!is_async(l, tag) && l->stale[0] != '\0') {
        l->stats.stale++;    /* the abandoned command's, still coming */
    } else if (l->cfg.on_async != NULL) {
        l->cfg.on_async(l->cfg.async_ctx, &msg);
    }
}

void fck_drv_link_feed(fck_drv_link_t *l, const uint8_t *data, size_t n)
{
    if (l == NULL || data == NULL) {
        return;
    }
    for (size_t i = 0U; i < n; i++) {
        uint8_t c = data[i];
        if (l->esc == ESC_START) {
            l->esc = (c == '[') ? ESC_CSI : ESC_NONE;
            continue;
        }
        if (l->esc == ESC_CSI) {
            if (c >= 0x40U && c <= 0x7EU) {
                l->esc = ESC_NONE;
            }
            continue;
        }
        if (c == 0x1BU) {
            l->esc = ESC_START;
            continue;
        }
        if (c == '\n') {
            if (l->overflow) {
                l->stats.lines++;
                l->stats.too_long++;
            } else if (l->len > 0U) {
                l->stats.lines++;
                l->line[l->len] = '\0';
                on_line(l, l->line);
            }
            l->len      = 0U;
            l->overflow = false;
            continue;
        }
        if (c == '\t') {
            c = ' ';
        }
        if (c < 0x20U || c == 0x7FU) {
            continue;
        }
        if (l->len < FCK_DRV_LINK_LINE_MAX) {
            l->line[l->len++] = (char)c;
        } else {
            l->overflow = true;
        }
    }
}
