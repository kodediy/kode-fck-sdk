/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file fck_drv_ook.c
 * @brief fck_drv_ook: runs from edges, the channel's noise, and the codes of
 *        EV1527, PT2262, HT12E, KeeLoq and Somfy RTS.
 *
 * How it was made. The protocols' own numbers (what a bit, a sync, a preamble
 * or a header looks like, and how long) come from the public data sheets and
 * articles listed below. All the readers were written from those, and checked
 * in the tests against frames laid out from the same sources (ook_frames.h).
 *
 * The windows around those numbers are Kode's own tolerances, in two groups.
 *  - KeeLoq and Somfy RTS: derived here from the sheet and the article, and
 *    each says why it is that wide (the receiver's slicer skews a pulse, a
 *    capture loses its first pulses, one unit has to stay apart from the next).
 *    No answer of the previous implementation is a reference for these two.
 *    Some of the windows come out where its were (at least three preamble
 *    pairs and a header's low from 7 TE for KeeLoq; two pairs of hardware
 *    sync, a software sync from 5 half symbols and the half-symbol windows at
 *    the midpoints for Somfy RTS): each has its reason next to it, but they
 *    are not new figures.
 *  - EV1527, PT2262 and HT12E (and the encoder): the shape is the sheets', but
 *    the tolerances were held, as a regression check, to what the previous
 *    implementation of this file answered through its public header, on 693
 *    captures and 158 calls (tests/host/main/ook_gold.inc says how). They are
 *    not all derived afresh; the exception, by design, is a te under 100 us,
 *    which is not read or written any more (see GAP_US).
 *
 * fck_drv_ook_rolling() (the sending path's "does this look like a rolling
 * code" detector, at the end of the file) is a different thing: a heuristic of
 * Kode's own that has no data sheet (tests/host holds it to fixed answers on
 * 693 captures).
 *
 *  [EV1527]  Silvan Chip Electronics, "EV1527 OTP Encoder" (English data sheet,
 *            "Output format"; the Chinese data sheet of the same part has the
 *            Rosc table in 16 LCK per bit): a sync of 1 unit high and 31 low, a
 *            bit 0 of 1 unit high and 3 low, a bit 1 of 3 high and 1 low.
 *  [PT2262]  Princeton Technology, "PT2262 Remote Control Encoder", V3.9,
 *            January 2011, "RF Operation": one bit is 32 oscillator periods
 *            (alpha); the AD bits are "0" (4 alpha high, 12 low, twice), "1"
 *            (12 high, 4 low, twice) and "f" (4 high 12 low, then 12 high 4
 *            low); the sync is 4 alpha high and 124 low.
 *  [HT12E]   Holtek, "212 Series of Encoders", 15 March 1996: Fig.4 (a pilot
 *            of 12 bits, a start pulse of 1/3 bit), Fig.5 (a bit is three
 *            oscillator periods: "1" two low and one high, "0" one low and two
 *            high) and the oscillator chart (about 1 to 7 kHz, 3 kHz typical).
 *  [HCS301]  Microchip, "HCS301 KEELOQ Code Hopping Encoder", DS21143C: Fig.4-2
 *            and Fig.9-5 (66 bits, least significant first: 32 of hop, 28 of
 *            serial, four of button, VLOW, repeat), Fig.9-3 (a bit is three TE
 *            in all: logic "0" two TE high and one low, logic "1" one high
 *            and two low), Fig.9-4 (a preamble of 23 TE at 50 % duty, then a
 *            header of 10 TE low), Table 9-4 (TE 260, 400 and 660 us minimum,
 *            typical and maximum at the default rate; 130, 200 and 330 us at
 *            "1 out of 2"; 65, 100 and 165 us at "1 out of 4"; preamble 23 TE,
 *            header 10 TE, guard 39 TE) and Table 3-3 (the baud rate selected
 *            by BSL0 and BSL1: a basic pulse element of 400, 200 or 100 us).
 *  [RTS]     Pushstack, "Somfy RTS protocol" (pushstack.wordpress.com): half
 *            symbol 604 us, hardware sync of 2 pairs (7 in a repeat) of 4 half
 *            symbols high and low, software sync of 4550 us high and a half
 *            symbol low, 56 bits in Manchester (rising edge 1), the frame
 *            obfuscated byte over byte, a nibble checksum, a gap of 30 ms.
 *
 * Plain C: nothing here touches a register.
 */
#include "fck_drv_ook.h"

#include <string.h>

/** A low this long ends a frame and starts the next (3 ms). The longest a
 *  frame's own low can be is 31 te (EV1527, PT2262, [EV1527] sync), so the
 *  least te a frame can be told from the next at is 3 ms / 31, rounded up
 *  (FCK_DRV_OOK_TE_MIN_US). */
#define GAP_US 3000U

/** The longest run the readers do arithmetic on: 200 ms. A run of a frame is
 *  never longer (the longest, a Somfy software sync or a KeeLoq header, is
 *  under 10 ms); one that is, is a quiet, and counts as 200 ms. It keeps every
 *  sum and product in 32 bits, whatever a file or a capture carries. */
#define RUN_CAP_US 200000U

static uint32_t mag(int32_t v)
{
    return (v < 0) ? (0U - (uint32_t)v) : (uint32_t)v;
}

/** The size of a run, at most RUN_CAP_US. */
static uint32_t cmag(int32_t v)
{
    uint32_t const m = mag(v);
    return (m > RUN_CAP_US) ? RUN_CAP_US : m;
}

/* ---- Edges to runs ------------------------------------------------------------ */

size_t fck_drv_ook_runs(uint32_t *edges, size_t n, bool level0, uint32_t end_us)
{
    int32_t *out    = (int32_t *)edges;
    size_t   count  = 0U;
    bool     level  = level0;
    uint32_t start  = 0U;

    for (size_t i = 0U; i < n; i++) {
        uint32_t const us = edges[i] >> 1;
        bool const     lv = (edges[i] & 1U) != 0U;
        if (us > end_us) {
            break;
        }
        if (lv == level) {
            continue;
        }
        if (us > start) {
            out[count++] = level ? (int32_t)(us - start) : -(int32_t)(us - start);
            start        = us;
        } else if (count > 0U) {
            /* Two edges at the same microsecond: the run between them would be 0 long. It is not written, and
             * the run before it goes on, at its own level, to the run after. */
            count--;
            start = start - mag(out[count]);
        } else {
            /* The same, at the very start: nothing before it to join, so the first run starts here. */
        }
        level = lv;
    }
    if (end_us > start) {
        out[count++] = level ? (int32_t)(end_us - start) : -(int32_t)(end_us - start);
    }
    return count;
}

size_t fck_drv_ook_deglitch(int32_t *runs, size_t n, uint32_t min_us)
{
    if (n == 0U) {
        return 0U;
    }
    size_t out = 1U;   /* the first run is kept, whatever it is: nothing before it to join */
    for (size_t i = 1U; i < n;) {
        if (mag(runs[i]) < min_us) {
            uint32_t const joined = mag(runs[i]) + ((i + 1U < n) ? mag(runs[i + 1U]) : 0U);
            uint32_t const total  = mag(runs[out - 1U]) + joined;
            uint32_t const kept   = (total > 0x7FFFFFFFU) ? 0x7FFFFFFFU : total;
            runs[out - 1U]        = (runs[out - 1U] < 0) ? -(int32_t)kept : (int32_t)kept;
            i += 2U;
        } else {
            runs[out++] = runs[i++];
        }
    }
    return out;
}

/* ---- The channel's noise ------------------------------------------------------- */

#define FLOOR_PERIOD 16U

void fck_drv_ook_floor_start(fck_drv_ook_floor_t *f, int16_t *half, size_t n)
{
    if (f == NULL || half == NULL || n == 0U) {
        return;
    }
    if (n > 64U) {
        n = 64U;
    }
    for (size_t i = 0U; i < n; i++) {
        if (half[i] < FCK_DRV_OOK_BOGUS_HALF) {
            half[i] = FCK_DRV_OOK_BOGUS_HALF;
        }
    }
    /* Insertion sort: 64 readings at most. */
    for (size_t i = 1U; i < n; i++) {
        int16_t const v = half[i];
        size_t        j = i;
        while (j > 0U && half[j - 1U] > v) {
            half[j] = half[j - 1U];
            j--;
        }
        half[j] = v;
    }
    /* The 19th percentile (3/16): the 7th of 32. */
    f->floor16 = (int32_t)half[(n * 3U) / 16U] * 16;
    f->k       = 0U;
}

void fck_drv_ook_floor_step(fck_drv_ook_floor_t *f, int16_t half)
{
    if (f == NULL || half < FCK_DRV_OOK_BOGUS_HALF) {
        return;
    }
    if (++f->k < FLOOR_PERIOD) {
        return;
    }
    f->k = 0U;
    if ((int32_t)half * 16 > f->floor16) {
        f->floor16 += 1;
    } else {
        f->floor16 -= 4;
    }
}

/* ---- Tolerance windows ----------------------------------------------------------- */

/* A pulse that should be one te long: from te / 1.4 to te / 0.7. */
static bool is_short(uint32_t x, uint32_t te)
{
    return (7U * x >= 5U * te) && (7U * x <= 10U * te);
}

/* A pulse that should be two te long: from 0.7 to 1.4 of it. */
static bool is_long(uint32_t x, uint32_t te)
{
    return (5U * x >= 7U * te) && (5U * x <= 14U * te);
}

/* ---- Fixed codes: tables and small rules ------------------------------------------ */

bool fck_drv_ook_te_ok(fck_drv_ook_protocol_t proto, uint16_t te_us)
{
    switch (proto) {
    case FCK_DRV_OOK_EV1527:
    case FCK_DRV_OOK_PT2262:
        return te_us >= FCK_DRV_OOK_TE_MIN_US && te_us <= FCK_DRV_OOK_TE_MAX_US;
    case FCK_DRV_OOK_HT12E:
        return te_us >= 150U && te_us <= 449U;
    default:
        return false;
    }
}

bool fck_drv_ook_bits_ok(fck_drv_ook_protocol_t proto, uint8_t bits)
{
    switch (proto) {
    case FCK_DRV_OOK_EV1527:
    case FCK_DRV_OOK_PT2262:
        return bits == 24U;
    case FCK_DRV_OOK_HT12E:
        return bits == 12U;
    default:
        return false;
    }
}

bool fck_drv_ook_te_range(fck_drv_ook_protocol_t proto, uint16_t *min_us, uint16_t *max_us, uint16_t *typical_us)
{
    if (min_us == NULL || max_us == NULL || typical_us == NULL) {
        return false;
    }
    switch (proto) {
    case FCK_DRV_OOK_EV1527:
    case FCK_DRV_OOK_PT2262:
        *min_us     = FCK_DRV_OOK_TE_MIN_US;
        *max_us     = FCK_DRV_OOK_TE_MAX_US;
        *typical_us = 390U;   /* [EV1527] 330 kOhm at 5 V: a bit of 1.56 ms, four units */
        return true;
    case FCK_DRV_OOK_HT12E:
        *min_us     = 150U;
        *max_us     = 449U;
        *typical_us = 333U;   /* [HT12E] 3 kHz */
        return true;
    default:
        return false;
    }
}

fck_drv_ook_protocol_t fck_drv_ook_protocol_of(uint32_t key)
{
    bool open = false;
    for (unsigned pin = 0U; pin < 12U; pin++) {
        uint32_t const pair = (key >> (2U * pin)) & 3U;
        if (pair == 2U) {
            return FCK_DRV_OOK_EV1527;   /* no PT2262 sends "10" */
        }
        if (pair == 1U) {
            open = true;
        }
    }
    return open ? FCK_DRV_OOK_PT2262 : FCK_DRV_OOK_EV1527;
}

const char *fck_drv_ook_name(fck_drv_ook_protocol_t proto)
{
    switch (proto) {
    case FCK_DRV_OOK_EV1527:
        return "ev1527";
    case FCK_DRV_OOK_PT2262:
        return "pt2262";
    case FCK_DRV_OOK_HT12E:
        return "ht12e";
    default:
        return NULL;
    }
}

bool fck_drv_ook_from_name(const char *name, uint8_t bits, uint32_t key, fck_drv_ook_protocol_t *proto)
{
    if (name == NULL || proto == NULL) {
        return false;
    }
    static const fck_drv_ook_protocol_t ALL[] = { FCK_DRV_OOK_EV1527, FCK_DRV_OOK_PT2262, FCK_DRV_OOK_HT12E };
    for (size_t i = 0U; i < sizeof(ALL) / sizeof(ALL[0]); i++) {
        if (strcmp(name, fck_drv_ook_name(ALL[i])) != 0) {
            continue;
        }
        if (!fck_drv_ook_bits_ok(ALL[i], bits) || (key >> bits) != 0U) {
            return false;
        }
        if (ALL[i] == FCK_DRV_OOK_PT2262) {
            for (unsigned pin = 0U; pin < 12U; pin++) {
                if (((key >> (2U * pin)) & 3U) == 2U) {
                    return false;
                }
            }
        }
        *proto = ALL[i];
        return true;
    }
    return false;
}

/* ---- Fixed codes: the encoder ------------------------------------------------------ */

static size_t put_run(int32_t *runs, size_t cap, size_t n, int32_t v)
{
    if (n < cap) {
        runs[n] = v;
    }
    return n + 1U;
}

size_t fck_drv_ook_encode(fck_drv_ook_protocol_t proto, uint8_t bits, uint32_t key, uint16_t te_us,
                          size_t frames, int32_t *runs, size_t cap)
{
    if (runs == NULL || frames == 0U || !fck_drv_ook_bits_ok(proto, bits) || !fck_drv_ook_te_ok(proto, te_us)) {
        return 0U;
    }
    if (bits < 32U && (key >> bits) != 0U) {
        return 0U;
    }
    if (proto == FCK_DRV_OOK_PT2262) {
        for (unsigned pin = 0U; pin < 12U; pin++) {
            if (((key >> (2U * pin)) & 3U) == 2U) {
                return 0U;
            }
        }
    }
    int32_t const te       = (int32_t)te_us;
    size_t const  per      = 2U * bits + 2U;   /* a pair a bit, and the sync or the pilot */
    if (frames > cap / per) {   /* frames * per would not fit in cap; said without a product that can wrap */
        return 0U;
    }
    size_t n = 0U;
    for (size_t f = 0U; f < frames; f++) {
        if (proto == FCK_DRV_OOK_HT12E) {
            n = put_run(runs, cap, n, te);   /* the start pulse */
            for (unsigned b = 0U; b < bits; b++) {
                bool const one = ((key >> (bits - 1U - b)) & 1U) != 0U;
                n              = put_run(runs, cap, n, one ? -2 * te : -te);
                n              = put_run(runs, cap, n, one ? te : 2 * te);
            }
            n = put_run(runs, cap, n, -36 * te);   /* the pilot */
        } else {
            for (unsigned b = 0U; b < bits; b++) {
                bool const one = ((key >> (bits - 1U - b)) & 1U) != 0U;
                n              = put_run(runs, cap, n, one ? 3 * te : te);
                n              = put_run(runs, cap, n, one ? -te : -3 * te);
            }
            n = put_run(runs, cap, n, te);   /* the sync */
            n = put_run(runs, cap, n, -31 * te);
        }
    }
    return n;
}

/* ---- Fixed codes: the readers -------------------------------------------------------- */

typedef struct {
    fck_drv_ook_protocol_t proto;
    uint8_t                bits;
    uint32_t               key;
    uint32_t               te_sum;
    uint8_t                frames;
} fixed_slot_t;

static bool frame_starts_at(const int32_t *runs, size_t i)
{
    if (runs[i] <= 0) {
        return false;
    }
    return i == 0U || (runs[i - 1U] < 0 && cmag(runs[i - 1U]) >= GAP_US);
}

/* HT12E: the pilot, a start pulse, 12 pairs of a low and a high. Returns the runs taken. */
static size_t read_ht12e(const int32_t *runs, size_t n, size_t i, uint32_t *key, uint32_t *te_out)
{
    if (i == 0U) {
        return 0U;
    }
    uint32_t const pilot = cmag(runs[i - 1U]);
    uint32_t const start = cmag(runs[i]);
    uint32_t       low[12];
    uint32_t       high[12];
    size_t         pairs = 0U;
    size_t         j     = i + 1U;
    while (j + 1U < n && runs[j] < 0 && runs[j + 1U] > 0 && cmag(runs[j]) < GAP_US) {
        if (pairs == 12U) {
            return 0U;   /* more bits than the chip sends */
        }
        low[pairs]  = cmag(runs[j]);
        high[pairs] = cmag(runs[j + 1U]);
        pairs++;
        j += 2U;
    }
    if (pairs != 12U) {
        return 0U;
    }
    uint32_t total = 0U;
    for (size_t p = 0U; p < 12U; p++) {
        total += low[p] + high[p];
    }
    uint32_t const te = total / 36U;   /* 12 bits of 3 units */
    if (!fck_drv_ook_te_ok(FCK_DRV_OOK_HT12E, (uint16_t)(te > 65535U ? 65535U : te))) {
        return 0U;
    }
    if (pilot < 15U * te) {
        return 0U;
    }
    if (20U * start < 13U * te || 20U * start > 29U * te) {
        return 0U;
    }
    uint32_t k = 0U;
    for (size_t p = 0U; p < 12U; p++) {
        uint32_t const sum   = low[p] + high[p];
        uint32_t const big   = (low[p] > high[p]) ? low[p] : high[p];
        uint32_t const small = (low[p] > high[p]) ? high[p] : low[p];
        if (10U * sum < 22U * te || 10U * sum > 38U * te || 10U * big < 14U * small || 10U * big > 28U * small) {
            return 0U;
        }
        k = (k << 1) | ((low[p] > high[p]) ? 1U : 0U);
    }
    *key    = k;
    *te_out = te;
    return 1U + 24U;
}

/* EV1527 and PT2262: 24 pairs of a high and a low, then the sync. */
static size_t read_pwm24(const int32_t *runs, size_t n, size_t i, uint32_t *key, uint32_t *te_out)
{
    if (i + 2U * FCK_DRV_OOK_BITS + 1U >= n) {
        return 0U;
    }
    uint32_t total = 0U;
    for (size_t b = 0U; b < FCK_DRV_OOK_BITS; b++) {
        if (runs[i + 2U * b] <= 0 || runs[i + 2U * b + 1U] >= 0) {
            return 0U;
        }
        total += cmag(runs[i + 2U * b]) + cmag(runs[i + 2U * b + 1U]);
    }
    uint32_t const te = total / (4U * FCK_DRV_OOK_BITS);   /* a bit is 4 units */
    if (!fck_drv_ook_te_ok(FCK_DRV_OOK_PT2262, (uint16_t)(te > 65535U ? 65535U : te))) {
        return 0U;
    }
    uint32_t k = 0U;
    for (size_t b = 0U; b < FCK_DRV_OOK_BITS; b++) {
        uint32_t const h     = cmag(runs[i + 2U * b]);
        uint32_t const l     = cmag(runs[i + 2U * b + 1U]);
        uint32_t const sum   = h + l;
        uint32_t const big   = (h > l) ? h : l;
        uint32_t const small = (h > l) ? l : h;
        if (10U * sum < 25U * te || 10U * sum > 55U * te || 10U * big < 18U * small) {
            return 0U;
        }
        k = (k << 1) | ((h > l) ? 1U : 0U);
    }
    size_t const  s  = i + 2U * FCK_DRV_OOK_BITS;
    if (runs[s] <= 0 || runs[s + 1U] >= 0) {
        return 0U;
    }
    uint32_t const sh = cmag(runs[s]);
    uint32_t const sl = cmag(runs[s + 1U]);
    if (2U * sh < te || sh > 2U * te || sl < 8U * te || sl > 60U * te) {
        return 0U;
    }
    *key    = k;
    *te_out = te;
    return 2U * FCK_DRV_OOK_BITS + 2U;
}

static void add_fixed(fixed_slot_t *slots, size_t *count, fck_drv_ook_protocol_t proto, uint8_t bits, uint32_t key,
                      uint32_t te)
{
    for (size_t s = 0U; s < *count; s++) {
        if (slots[s].proto == proto && slots[s].bits == bits && slots[s].key == key) {
            slots[s].te_sum += te;
            slots[s].frames++;
            return;
        }
    }
    if (*count < FCK_DRV_OOK_FRAMES_MAX) {
        slots[*count] = (fixed_slot_t){ proto, bits, key, te, 1U };
        (*count)++;
    }
}

size_t fck_drv_ook_decode(const int32_t *runs, size_t n, fck_drv_ook_code_t *out, size_t cap)
{
    if (runs == NULL || out == NULL || cap == 0U) {
        return 0U;
    }
    fixed_slot_t slots[FCK_DRV_OOK_FRAMES_MAX];
    size_t       count = 0U;
    size_t       read  = 0U;
    for (size_t i = 0U; i < n && read < FCK_DRV_OOK_FRAMES_MAX;) {
        if (!frame_starts_at(runs, i)) {
            i++;
            continue;
        }
        uint32_t key = 0U;
        uint32_t te  = 0U;
        size_t   used = read_ht12e(runs, n, i, &key, &te);
        if (used != 0U) {
            add_fixed(slots, &count, FCK_DRV_OOK_HT12E, 12U, key, te);
        } else {
            used = read_pwm24(runs, n, i, &key, &te);
            if (used != 0U) {
                add_fixed(slots, &count, fck_drv_ook_protocol_of(key), 24U, key, te);
            }
        }
        if (used != 0U) {
            read++;
            i += used;
        } else {
            i++;
        }
    }
    /* The code most frames carry first; equal counts keep the order they were met in. */
    for (size_t a = 1U; a < count; a++) {
        fixed_slot_t const v = slots[a];
        size_t             b = a;
        while (b > 0U && slots[b - 1U].frames < v.frames) {
            slots[b] = slots[b - 1U];
            b--;
        }
        slots[b] = v;
    }
    size_t const w = (count < cap) ? count : cap;
    for (size_t s = 0U; s < w; s++) {
        out[s].proto  = slots[s].proto;
        out[s].bits   = slots[s].bits;
        out[s].key    = slots[s].key;
        out[s].te_us  = (uint16_t)(slots[s].te_sum / slots[s].frames);
        out[s].frames = slots[s].frames;
    }
    return w;
}

/* ---- Rolling codes: KeeLoq ----------------------------------------------------------- */

/* What the data sheet gives ([HCS301]): a bit is three TE in all, a "1" one TE high and two low, a "0" two high and
 * one low (Fig.9-3); a preamble of 23 TE at 50 % duty, then a header of 10 TE low (Fig.9-4); 66 bits, 32 of hop, 28 of
 * serial, 4 of button, VLOW and repeat (Fig.9-5); a guard of 39 TE (Table 9-4). TE is 260 to 660 us at the default
 * rate (400 us typical), 130 to 330 us at "1 out of 2" and 65 to 165 us at "1 out of 4" (Table 9-4; Table 3-3 selects
 * the rate: 400, 200 or 100 us).
 *
 * What is chosen here, and why:
 *  - TE from 100 to 660 us, read off the capture's preamble. 660 us is the table's largest. 100 us is the typical of
 *    the fastest rate; the table's 65 us minimum is under what FCK_DRV_OOK_GLITCH_US merges away, and a guard of 39
 *    TE is under GAP_US below 77 us, so a frame that fast could not be told from the next.
 *  - Inside one frame TE does not change (the table's spread is between parts, voltage and temperature, derived from
 *    one oscillator), so what remains is the receiver's slicer skewing a pulse: it moves time from the high to the
 *    low of the same bit or the other way. A pulse of one TE is read as one from 0.71 to 1.43 TE (is_short: 1 / 1.4
 *    to 1 / 0.7) and one of two TE from 1.4 to 2.8 TE (is_long: 0.7 to 1.4 of it). The two meet at 1.4 and share
 *    the sliver up to 1.43; only the last bit is read from one pulse alone, and there the short one is tried first.
 *  - A bit's high and low together are 2.2 to 3.75 TE: the sum does not move with skew, so this is the error of the
 *    TE read from the preamble (about 25 %, the same as a pulse, and not more as it is the mean of seven pulses or more). The
 *    longer over the shorter is 1.4 to 2.75: 2 is the datasheet's; 1.4 is a skew of a quarter of a TE toward the 1:1
 *    of a pair that has no bit in it, and 2.75 a skew of a fifth of a TE the other way, toward 3:1, which is no bit.
 *  - The header's low is 7 to 13 TE (the data sheet's 10, 30 % either way, wide enough for a header cut at its start
 *    or stretched by the slicer, and apart from the 3 TE of a bit's longest run). Its high is the last pulse of the
 *    preamble, and is read as any pulse of one TE is (is_short).
 *  - At least three preamble pairs (the data sheet's 12 pulses are not all in a capture that starts late), and
 *    64 to 66 bits: a capture can lose VLOW and repeat to the guard, never the 64 bits of the code itself. */
#define KEELOQ_TE_MIN_US 100U
#define KEELOQ_TE_MAX_US 660U

/* A bit's high and low together. See above. */
static bool keeloq_pair_ok(uint32_t h, uint32_t l, uint32_t te)
{
    uint32_t const big   = (h > l) ? h : l;
    uint32_t const small = (h > l) ? l : h;
    uint32_t const sum   = h + l;
    return 5U * sum >= 11U * te && 4U * sum <= 15U * te && 5U * big >= 7U * small && 4U * big <= 11U * small;
}

/* One frame of an HCS301: the preamble pairs, the header, 64 to 66 bits. Returns the runs taken. */
static size_t read_keeloq(const int32_t *runs, size_t n, size_t i, fck_drv_ook_rolling_t *c)
{
    uint32_t sum   = 0U;
    size_t   pairs = 0U;
    size_t   j     = i;
    while (j + 1U < n && runs[j] > 0 && runs[j + 1U] < 0) {
        uint32_t const h = cmag(runs[j]);
        uint32_t const l = cmag(runs[j + 1U]);
        if (l >= 4U * h) {
            break;   /* the header: a high of te and a low of about 10 te */
        }
        sum += h + l;
        pairs++;
        j += 2U;
    }
    if (pairs < 3U || j + 1U >= n || runs[j] <= 0 || runs[j + 1U] >= 0) {
        return 0U;
    }
    uint32_t const hh = cmag(runs[j]);
    uint32_t const hl = cmag(runs[j + 1U]);
    uint32_t const te = (sum + hh) / (2U * (uint32_t)pairs + 1U);
    if (te < KEELOQ_TE_MIN_US || te > KEELOQ_TE_MAX_US) {
        return 0U;
    }
    for (size_t p = 0U; p < 2U * pairs; p++) {
        if (!is_short(cmag(runs[i + p]), te)) {
            return 0U;
        }
    }
    if (!is_short(hh, te) || hl < 7U * te || hl > 13U * te) {
        return 0U;
    }
    /* The bits, to the guard (a low of 3 ms) or the end of the capture. */
    size_t   k    = j + 2U;
    unsigned bits = 0U;
    uint64_t word = 0U;
    bool     vlow = false;
    while (k < n) {
        if (runs[k] <= 0) {
            return 0U;
        }
        uint32_t const h = cmag(runs[k]);
        bool const     has_low = (k + 1U < n);
        uint32_t const l       = has_low ? cmag(runs[k + 1U]) : 0U;
        bool const     guard   = !has_low || l >= GAP_US;
        bool           one;
        if (!guard) {
            if (!keeloq_pair_ok(h, l, te)) {
                return 0U;
            }
            one = h < l;   /* a "1" is the short high and the long low */
        } else if (is_short(h, te)) {
            one = true;
        } else if (is_long(h, te)) {
            one = false;
        } else {
            return 0U;
        }
        if (bits >= 66U) {
            return 0U;
        }
        if (bits < 64U) {
            word |= (uint64_t)(one ? 1U : 0U) << bits;
        } else if (bits == 64U) {
            vlow = one;
        }
        bits++;
        k += 2U;
        if (guard) {
            break;
        }
    }
    if (bits < 64U) {
        return 0U;
    }
    uint64_t first_highest = 0U;
    for (unsigned b = 0U; b < 64U; b++) {
        first_highest |= ((word >> b) & 1U) << (63U - b);
    }
    memset(c, 0, sizeof(*c));
    c->proto       = FCK_DRV_OOK_KEELOQ;
    c->bits        = (uint8_t)bits;
    c->data        = first_highest;
    c->hop         = (uint32_t)(word & 0xFFFFFFFFU);
    c->serial      = (uint32_t)((word >> 32) & 0x0FFFFFFFU);
    c->button      = (uint8_t)((word >> 60) & 0xFU);
    c->battery_low = (bits > 64U) && vlow;
    c->te_us       = (uint16_t)te;
    return (k < n) ? k - i : n - i;
}

/* ---- Rolling codes: Somfy RTS ---------------------------------------------------------- */

/* What the article gives ([RTS]): a half symbol of 604 us; a hardware sync of 4 half symbols high and 4 low, twice in
 * the first frame of a press and seven times in a repeat; a software sync of 4550 us high (7.5 half symbols) and one
 * half symbol low; 56 bits of two half symbols each, Manchester (a rising edge is a 1); a gap of 30 ms. An optional
 * wake-up pulse of 9415 us and 89 ms of quiet comes before the first frame, and is no frame's start (it is a high
 * followed by a gap, and a frame needs two pairs of the hardware sync). The article says its figures are of passive
 * observation, with about 15 to 20 % of tolerance in a comment of its readers.
 *
 * What is chosen here, and why:
 *  - The half symbol from 480 to 730 us: 604 us less and more 20 % (the article's tolerance) rounded outward.
 *  - A hardware sync pulse is 2.5 to 5.5 half symbols (4 is the article's): 2.5 is under 3, the middle of the way to
 *    the 2 of the longest data run, and 5.5 is under the middle of the way to the sync's 7.5.
 *  - The software sync's high is 5 to 10 half symbols, 7.5 and the same 33 %, and apart from the hardware sync's 4.
 *  - A data run is one half symbol from 0.5 to 1.5 of the measured one and two from 1.5 to 2.5: the middles between
 *    0, 1, 2 and 3, as a slicer's skew allows.
 *  - The key's upper nibble must be 0xA, as the article says of the remote it measured. One of its readers wrote that
 *    some remotes do not keep it; a frame with another nibble is not read here, so as not to read noise as a remote. */
#define SOMFY_TE_MIN_US 480U
#define SOMFY_TE_MAX_US 730U
#define SOMFY_BITS      56U

/* Units of a half symbol in a run: 1 or 2, 0 when it is neither. */
static unsigned half_units(uint32_t x, uint32_t te)
{
    if (2U * x >= te && 2U * x < 3U * te) {
        return 1U;
    }
    if (2U * x >= 3U * te && 2U * x < 5U * te) {
        return 2U;
    }
    return 0U;
}

static size_t read_somfy(const int32_t *runs, size_t n, size_t i, fck_drv_ook_rolling_t *c)
{
    if (i + 1U >= n || runs[i] <= 0 || runs[i + 1U] >= 0) {
        return 0U;
    }
    /* The hardware sync: pairs of 4 half symbols high and 4 low. */
    uint32_t const te0   = (cmag(runs[i]) + cmag(runs[i + 1U])) / 8U;
    uint32_t       sum   = 0U;
    size_t         pairs = 0U;
    size_t         j     = i;
    while (j + 1U < n && runs[j] > 0 && runs[j + 1U] < 0) {
        uint32_t const h = cmag(runs[j]);
        uint32_t const l = cmag(runs[j + 1U]);
        if (2U * h < 5U * te0 || 2U * h > 11U * te0 || 2U * l < 5U * te0 || 2U * l > 11U * te0) {
            break;
        }
        sum += h + l;
        pairs++;
        j += 2U;
    }
    if (pairs < 2U || j + 1U >= n) {
        return 0U;
    }
    uint32_t const te = sum / (8U * (uint32_t)pairs);
    if (te < SOMFY_TE_MIN_US || te > SOMFY_TE_MAX_US) {
        return 0U;
    }
    if (runs[j] <= 0 || cmag(runs[j]) < 5U * te || cmag(runs[j]) > 10U * te) {
        return 0U;
    }
    /* The software sync is followed by a half symbol low, and then the data in half symbols. */
    uint8_t  level[2U * SOMFY_BITS + 1U];
    unsigned halves = 0U;
    size_t   k      = j + 1U;
    bool     ends_in_gap = false;
    while (halves < 2U * SOMFY_BITS + 1U) {
        if (k >= n) {
            return 0U;
        }
        bool const     high = runs[k] > 0;
        uint32_t const x    = cmag(runs[k]);
        unsigned       u    = half_units(x, te);
        if (u == 0U) {
            /* The last half symbol low can have the gap to the next frame in it. */
            if (!high && halves == 2U * SOMFY_BITS && x >= GAP_US) {
                u           = 1U;
                ends_in_gap = true;
            } else {
                return 0U;
            }
        }
        if (halves + u > 2U * SOMFY_BITS + 1U) {
            return 0U;
        }
        for (unsigned h = 0U; h < u; h++) {
            level[halves++] = high ? 1U : 0U;
        }
        k++;
    }
    if (!ends_in_gap && k < n && !(runs[k] < 0 && cmag(runs[k]) >= GAP_US)) {
        return 0U;   /* more half symbols than 56 bits have */
    }
    if (level[0] != 0U) {
        return 0U;   /* the software sync's low */
    }
    uint64_t v = 0U;
    for (unsigned b = 0U; b < SOMFY_BITS; b++) {
        uint8_t const a = level[1U + 2U * b];
        uint8_t const z = level[2U + 2U * b];
        if (a == z) {
            return 0U;
        }
        v = (v << 1) | ((a == 0U) ? 1U : 0U);   /* a low and then a high is a 1 */
    }
    uint64_t const d = v ^ (v >> 8);
    if (((d >> 52) & 0xFU) != 0xAU) {
        return 0U;   /* [RTS] the key's upper nibble is always 0xA on the remote the article measured */
    }
    uint8_t        cks = 0U;
    for (unsigned nib = 0U; nib < 14U; nib++) {
        if (nib != 10U) {
            cks ^= (uint8_t)((d >> (4U * nib)) & 0xFU);
        }
    }
    if ((uint8_t)((d >> 40) & 0xFU) != cks) {
        return 0U;
    }
    memset(c, 0, sizeof(*c));
    c->proto        = FCK_DRV_OOK_SOMFY_RTS;
    c->bits         = (uint8_t)SOMFY_BITS;
    c->data         = v;
    c->serial       = (uint32_t)(d & 0xFFFFFFU);
    c->button       = (uint8_t)((d >> 44) & 0xFU);
    c->counter      = (uint16_t)((d >> 24) & 0xFFFFU);
    c->counter_read = true;
    c->te_us        = (uint16_t)te;
    return k - i;
}

typedef struct {
    fck_drv_ook_rolling_t code;
    uint32_t              te_sum;
} rolling_slot_t;

static void add_rolling(rolling_slot_t *slots, size_t *count, const fck_drv_ook_rolling_t *c)
{
    for (size_t s = 0U; s < *count; s++) {
        fck_drv_ook_rolling_t *o = &slots[s].code;
        if (o->proto == c->proto && o->bits == c->bits && o->data == c->data && o->battery_low == c->battery_low) {
            slots[s].te_sum += c->te_us;
            o->frames++;
            return;
        }
    }
    if (*count < FCK_DRV_OOK_FRAMES_MAX) {
        slots[*count].code        = *c;
        slots[*count].code.frames = 1U;
        slots[*count].te_sum      = c->te_us;
        (*count)++;
    }
}

size_t fck_drv_ook_decode_rolling(const int32_t *runs, size_t n, fck_drv_ook_rolling_t *out, size_t cap)
{
    if (runs == NULL || out == NULL || cap == 0U) {
        return 0U;
    }
    rolling_slot_t slots[FCK_DRV_OOK_FRAMES_MAX];
    size_t         count = 0U;
    size_t         read  = 0U;
    for (size_t i = 0U; i < n && read < FCK_DRV_OOK_FRAMES_MAX;) {
        if (!frame_starts_at(runs, i)) {
            i++;
            continue;
        }
        fck_drv_ook_rolling_t c;
        size_t                used = read_keeloq(runs, n, i, &c);
        if (used == 0U) {
            used = read_somfy(runs, n, i, &c);
        }
        if (used != 0U) {
            add_rolling(slots, &count, &c);
            read++;
            i += used;   /* a frame counts once: the next one is looked for after it */
        } else {
            i++;
        }
    }
    for (size_t a = 1U; a < count; a++) {
        rolling_slot_t const v = slots[a];
        size_t               b = a;
        while (b > 0U && slots[b - 1U].code.frames < v.code.frames) {
            slots[b] = slots[b - 1U];
            b--;
        }
        slots[b] = v;
    }
    size_t const w = (count < cap) ? count : cap;
    for (size_t s = 0U; s < w; s++) {
        out[s]       = slots[s].code;
        out[s].te_us = (uint16_t)(slots[s].te_sum / slots[s].code.frames);
    }
    return w;
}

/* ---- The sending path's detector ---------------------------------------------------------- */

static bool even_pair(uint32_t h, uint32_t l, uint32_t lo, uint32_t hi, uint32_t pct)
{
    uint32_t const big   = (h > l) ? h : l;
    uint32_t const small = (h > l) ? l : h;
    return h >= lo && h <= hi && l >= lo && l <= hi && 100U * big <= (100U + pct) * small;
}

/* A header and a frame of KeeLoq's shape after the even pairs that start at runs[i]. */
static bool keeloq_shaped_from(const int32_t *runs, size_t n, size_t i)
{
    uint32_t sum   = 0U;
    size_t   pairs = 0U;
    size_t   j     = i;
    while (j + 1U < n && runs[j] > 0 && runs[j + 1U] < 0 &&
           even_pair(cmag(runs[j]), cmag(runs[j + 1U]), 150U, 900U, 30U)) {
        sum += cmag(runs[j]) + cmag(runs[j + 1U]);
        pairs++;
        j += 2U;
    }
    if (pairs < 2U || j + 1U >= n || runs[j] <= 0 || runs[j + 1U] >= 0) {
        return false;
    }
    uint32_t const te = sum / (2U * (uint32_t)pairs);
    uint32_t const hh = cmag(runs[j]);
    uint32_t const hl = cmag(runs[j + 1U]);
    if (5U * hh < 3U * te || 5U * hh > 7U * te || hl < 6U * te || hl > 16U * te) {
        return false;
    }
    /* The runs of a bit (3 te at most) from the header to the gap or the end: a damaged bit does not stop the
     * count, so a frame that lost or stretched some still has the shape. */
    unsigned counted = 0U;
    for (size_t k = j + 2U; k < n; k++) {
        if (runs[k] < 0 && cmag(runs[k]) >= GAP_US) {
            break;
        }
        if (cmag(runs[k]) <= 3U * te) {
            counted++;
        }
    }
    return counted >= 40U;
}

static bool somfy_shaped_from(const int32_t *runs, size_t n, size_t i)
{
    uint32_t sum   = 0U;
    size_t   pairs = 0U;
    size_t   j     = i;
    while (j + 1U < n && runs[j] > 0 && runs[j + 1U] < 0 &&
           even_pair(cmag(runs[j]), cmag(runs[j + 1U]), 1500U, 3600U, 35U)) {
        sum += cmag(runs[j]) + cmag(runs[j + 1U]);
        pairs++;
        j += 2U;
    }
    if (pairs < 1U || j >= n || runs[j] <= 0) {
        return false;
    }
    uint32_t const pulse = sum / (2U * (uint32_t)pairs);
    uint32_t const sync  = cmag(runs[j]);
    if (4U * sync < 5U * pulse || 5U * sync > 13U * pulse) {
        return false;
    }
    unsigned counted = 0U;
    for (size_t k = j + 1U; k < n; k++) {
        if (runs[k] < 0 && cmag(runs[k]) >= GAP_US) {
            break;
        }
        /* a half symbol is a quarter of the pulse; up to 2.6 times that counts */
        if (20U * cmag(runs[k]) <= 13U * pulse) {
            counted++;
        }
    }
    return counted >= 30U;
}

/* The pulse of a list of runs: the mean of those that are not above the mean. */
static uint32_t pwm_pulse(const int32_t *runs, size_t n)
{
    uint64_t all = 0U;
    for (size_t i = 0U; i < n; i++) {
        all += cmag(runs[i]);
    }
    uint32_t const mean    = (uint32_t)(all / n);
    uint64_t       under   = 0U;
    size_t         under_n = 0U;
    for (size_t i = 0U; i < n; i++) {
        if (cmag(runs[i]) <= mean) {
            under += cmag(runs[i]);
            under_n++;
        }
    }
    return (under_n == 0U) ? 0U : (uint32_t)(under / under_n);
}

#define PWM_RUNS_MIN           60U
#define PWM_SHORT_MIN_PERCENT  70U
#define PWM_FIT_PERCENT        90U

/* Whether a run is about one, two or three pulses: a short one is 0.7 to 1.35 te, a long one 1.55 to 3.75 te. What is
 * between them is neither. These limits, like the whole of this detector, are Kode's own and from no
 * published source (the checks of the detector in tests/host hold it to fixed answers on 693 captures). */
static bool pwm_fits(uint32_t x, uint32_t te)
{
    bool const is_short_run = 20U * x <= 27U * te && 100U * x >= PWM_SHORT_MIN_PERCENT * te;
    bool const is_long_run  = 20U * x >= 31U * te && 4U * x <= 15U * te;
    return is_short_run || is_long_run;
}

/* One stretch of runs between two gaps: is a run of more than 60 in it, none over six pulses, nine in ten of them
 * a short or a long one? The pulse is that of the whole stretch, the gaps' runs that are over six pulses cut it in
 * parts and each part is judged on its own. */
static bool pwm_stretch(const int32_t *runs, size_t n)
{
    if (n <= PWM_RUNS_MIN) {
        return false;
    }
    uint32_t const te = pwm_pulse(runs, n);
    if (te == 0U) {
        return false;
    }
    size_t a = 0U;
    while (a < n) {
        size_t b   = a;
        size_t fit = 0U;
        while (b < n && 100U * cmag(runs[b]) <= 600U * te) {
            fit += pwm_fits(cmag(runs[b]), te) ? 1U : 0U;
            b++;
        }
        if ((b - a) > PWM_RUNS_MIN && 100U * fit >= PWM_FIT_PERCENT * (b - a)) {
            return true;
        }
        a = (b == a) ? (a + 1U) : b;
    }
    return false;
}

/* A long PWM frame in any stretch of the capture without a run of GAP_US or more. */
static bool long_pwm(const int32_t *runs, size_t n)
{
    size_t s = 0U;
    while (s < n) {
        if (cmag(runs[s]) >= GAP_US) {
            s++;
            continue;
        }
        size_t e = s;
        while (e < n && cmag(runs[e]) < GAP_US) {
            e++;
        }
        if (pwm_stretch(&runs[s], e - s)) {
            return true;
        }
        s = e;
    }
    return false;
}

bool fck_drv_ook_rolling(const int32_t *runs, size_t n)
{
    if (runs == NULL || n == 0U) {
        return false;
    }
    fck_drv_ook_rolling_t read;
    if (fck_drv_ook_decode_rolling(runs, n, &read, 1U) != 0U) {
        return true;
    }
    for (size_t i = 0U; i < n; i++) {
        if (runs[i] > 0 && (keeloq_shaped_from(runs, n, i) || somfy_shaped_from(runs, n, i))) {
            return true;
        }
    }
    return long_pwm(runs, n);
}
