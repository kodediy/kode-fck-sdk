/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_ook.c
 * @brief fck_drv_ook and fck_drv_subghz_scan: a remote's code read back from GDO0's edges,
 *        and its frequency found against each frequency's own noise.
 *
 * The frames are built from PT2262's timing (bit 0: te high, 3 te low; bit
 * 1: 3 te high, te low; sync: te high, 31 te low) with a PT2262 remote
 * as the main case: 315 MHz, PT2262, te 436 us; the key, 0x5155C3, is made up
 * (a real remote's code has no place in a public repository). What must never
 * happen: a key read from noise, a frame with no sync after it taken
 * whole, a peak on the edge of a sweep taken for a remote.
 */
#include <string.h>

#include "unity.h"

#include "fck_drv_ook.h"
#include "fck_drv_subghz_scan.h"

#define REMOTE_KEY 0x5155C3U
#define REMOTE_TE  436U

static int32_t s_runs[1024];
static size_t  s_n;

/* A small, fixed pseudo-random sequence: the same jitter every run. */
static uint32_t s_lcg = 1U;

static uint32_t rnd(void)
{
    s_lcg = s_lcg * 1103515245U + 12345U;
    return (s_lcg >> 8) & 0xFFFFU;
}

/* @p d with up to +/- @p pct percent of jitter. */
static int32_t jit(uint32_t d, uint32_t pct)
{
    if (pct == 0U) {
        return (int32_t)d;
    }
    int32_t const span = (int32_t)(d * pct / 100U);
    return (int32_t)d + (int32_t)(rnd() % (uint32_t)(2 * span + 1)) - span;
}

static void put(int32_t v)
{
    if (s_n < sizeof(s_runs) / sizeof(s_runs[0])) {
        s_runs[s_n++] = v;
    }
}

static void frames(uint32_t key, uint32_t te, unsigned count, uint32_t pct)
{
    for (unsigned f = 0U; f < count; f++) {
        for (int b = 23; b >= 0; b--) {
            bool const one = ((key >> b) & 1U) != 0U;
            put(jit(one ? 3U * te : te, pct));
            put(-jit(one ? te : 3U * te, pct));
        }
        put(jit(te, pct));
        put(-jit(31U * te, pct));
    }
}

static void reset_runs(void)
{
    s_n   = 0U;
    s_lcg = 1U;
}

/* ---- Edges to runs ---------------------------------------------------------- */

TEST_CASE("edges become signed runs in place, from the level at the start to the end", "[ook]")
{
    uint32_t e[] = {
        fck_drv_ook_edge(100, true),  fck_drv_ook_edge(400, false), fck_drv_ook_edge(500, true),
        fck_drv_ook_edge(500, true),  /* the same level again: not an edge      */
        fck_drv_ook_edge(900, false), fck_drv_ook_edge(5000, true), /* after the end */
    };
    size_t const n = fck_drv_ook_runs(e, sizeof(e) / sizeof(e[0]), false, 1000);
    int32_t      r[4];
    memcpy(r, e, sizeof(r));
    TEST_ASSERT_EQUAL_size_t(5, n);
    TEST_ASSERT_EQUAL_INT32(-100, r[0]);
    TEST_ASSERT_EQUAL_INT32(300, r[1]);
    TEST_ASSERT_EQUAL_INT32(-100, r[2]);
    TEST_ASSERT_EQUAL_INT32(400, r[3]);
    TEST_ASSERT_EQUAL_INT32(-100, ((int32_t *)e)[4]);

    /* Starting high, no edge at all: one run to the end. */
    uint32_t none[1] = { 0 };
    TEST_ASSERT_EQUAL_size_t(1, fck_drv_ook_runs(none, 0, true, 750));
    TEST_ASSERT_EQUAL_INT32(750, (int32_t)none[0]);
}

TEST_CASE("edges at the same microsecond leave no run of 0, and no time at all leaves no run", "[ook]")
{
    /* low from 0, high at 100, low at 100 again (a run of 0 between), high at 300, low at 600, end at 1000 */
    uint32_t e[] = { fck_drv_ook_edge(100, true), fck_drv_ook_edge(100, false), fck_drv_ook_edge(300, true),
                     fck_drv_ook_edge(600, false) };
    size_t const n = fck_drv_ook_runs(e, 4U, false, 1000U);
    int32_t      r[4];
    memcpy(r, e, sizeof(r));
    TEST_ASSERT_EQUAL_size_t(3U, n);
    TEST_ASSERT_EQUAL_INT32(-300, r[0]);   /* the low goes on over the 0-long high */
    TEST_ASSERT_EQUAL_INT32(300, r[1]);
    TEST_ASSERT_EQUAL_INT32(-400, r[2]);

    /* An edge at 0 against the level at the start: the first run starts at it. */
    uint32_t z[] = { fck_drv_ook_edge(0, true), fck_drv_ook_edge(500, false) };
    TEST_ASSERT_EQUAL_size_t(2U, fck_drv_ook_runs(z, 2U, false, 800U));
    TEST_ASSERT_EQUAL_INT32(500, (int32_t)z[0]);
    TEST_ASSERT_EQUAL_INT32(-300, (int32_t)z[1]);

    /* A capture that ends where it starts has no run. */
    uint32_t none[1] = { 0 };
    TEST_ASSERT_EQUAL_size_t(0U, fck_drv_ook_runs(none, 0U, true, 0U));
    for (size_t i = 0U; i < n; i++) {
        TEST_ASSERT_TRUE(r[i] != 0);
    }
}

TEST_CASE("a run under 70 us joins the runs around it", "[ook]")
{
    int32_t r[] = { 400, -30, 500, -1200, 50, -800, 20 };
    size_t const n = fck_drv_ook_deglitch(r, sizeof(r) / sizeof(r[0]), FCK_DRV_OOK_GLITCH_US);
    TEST_ASSERT_EQUAL_size_t(2, n);
    TEST_ASSERT_EQUAL_INT32(930, r[0]);
    TEST_ASSERT_EQUAL_INT32(-2070, r[1]);

    int32_t first[] = { 40, -400, 300 };   /* nothing before it to join */
    TEST_ASSERT_EQUAL_size_t(3, fck_drv_ook_deglitch(first, 3, FCK_DRV_OOK_GLITCH_US));
}

/* ---- The channel's noise ------------------------------------------------------ */

TEST_CASE("the noise starts at a low percentile and settles where 4 readings in 5 are above",
          "[ook]")
{
    int16_t v[32];
    for (int i = 0; i < 32; i++) {
        v[i] = (int16_t)(-200 + i);   /* -100 .. -84.5 dBm */
    }
    v[3] = -40;                        /* a key already pressed: high, not low */
    fck_drv_ook_floor_t f;
    fck_drv_ook_floor_start(&f, v, 32);
    TEST_ASSERT_EQUAL_INT16(-193, fck_drv_ook_floor_half(&f));

    /* A channel whose readings are -190 four times in five and -170 once
     * (each step sees one of them): close to -190, never at -170. */
    for (int k = 0; k < 16 * 5000; k++) {
        fck_drv_ook_floor_step(&f, ((k / 16) % 5 == 0) ? (int16_t)-170 : (int16_t)-190);
    }
    TEST_ASSERT_INT16_WITHIN(2, -190, fck_drv_ook_floor_half(&f));

    /* A carrier that stays is taken in at about 8 dB a second: 5000
     * readings, 200 us apart, is a second, and 16 half-dB is 8 dB. */
    fck_drv_ook_floor_t g = { .floor16 = -190 * 16 };
    for (int k = 0; k < 5000; k++) {
        fck_drv_ook_floor_step(&g, -100);
    }
    TEST_ASSERT_INT16_WITHIN(4, -190 + 19, fck_drv_ook_floor_half(&g));

    /* Bad reads are not the channel. */
    fck_drv_ook_floor_t b = g;
    for (int k = 0; k < 1000; k++) {
        fck_drv_ook_floor_step(&b, -300);
    }
    TEST_ASSERT_EQUAL_INT32(g.floor16, b.floor16);
}

/* ---- Codes ------------------------------------------------------------------- */

TEST_CASE("A PT2262 remote: PT2262 0x5155C3 at 436 us, from three clean frames", "[ook]")
{
    reset_runs();
    put(-8000);
    frames(REMOTE_KEY, REMOTE_TE, 3, 0);
    fck_drv_ook_code_t c[2];
    TEST_ASSERT_EQUAL_size_t(1, fck_drv_ook_decode(s_runs, s_n, c, 2));
    TEST_ASSERT_EQUAL(FCK_DRV_OOK_PT2262, c[0].proto);
    TEST_ASSERT_EQUAL_HEX32(REMOTE_KEY, c[0].key);
    TEST_ASSERT_EQUAL_UINT8(24, c[0].bits);
    TEST_ASSERT_EQUAL_UINT16(REMOTE_TE, c[0].te_us);
    TEST_ASSERT_EQUAL_UINT8(3, c[0].frames);
}

TEST_CASE("frames with 15 % jitter, noise before them and the last sync cut still decode",
          "[ook]")
{
    reset_runs();
    /* What GDO0 shows on a quiet channel before the remote: short runs. */
    for (int i = 0; i < 40; i++) {
        put((i & 1) ? -(int32_t)(100 + rnd() % 400) : (int32_t)(100 + rnd() % 400));
    }
    put(-6000);
    frames(REMOTE_KEY, REMOTE_TE, 4, 15);
    /* The capture closes 3 ms after the last power it heard: the last sync
     * is cut to 3 ms, short of 8 te. */
    s_runs[s_n - 1U] = -3000;
    fck_drv_ook_code_t c;
    TEST_ASSERT_EQUAL_size_t(1, fck_drv_ook_decode(s_runs, s_n, &c, 1));
    TEST_ASSERT_EQUAL_HEX32(REMOTE_KEY, c.key);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(3, c.frames, "a frame with its sync cut was taken whole");
    TEST_ASSERT_UINT16_WITHIN(25, REMOTE_TE, c.te_us);
}

TEST_CASE("a pair 10 makes a code EV1527, and so does a PT2262 with every pin tied", "[ook]")
{
    reset_runs();
    put(-8000);
    frames(0xA1B2C3U, 300, 2, 0);
    fck_drv_ook_code_t c;
    TEST_ASSERT_EQUAL_size_t(1, fck_drv_ook_decode(s_runs, s_n, &c, 1));
    TEST_ASSERT_EQUAL(FCK_DRV_OOK_EV1527, c.proto);
    TEST_ASSERT_EQUAL_HEX32(0xA1B2C3U, c.key);

    reset_runs();
    put(-8000);
    frames(0xF0F0F0U, 300, 2, 0);   /* trits 1 1 0 0 1 1 0 0 1 1 0 0, none open */
    TEST_ASSERT_EQUAL_size_t(1, fck_drv_ook_decode(s_runs, s_n, &c, 1));
    TEST_ASSERT_EQUAL(FCK_DRV_OOK_EV1527, c.proto);
}

TEST_CASE("two remotes in one capture: the key most frames carry comes first", "[ook]")
{
    reset_runs();
    put(-8000);
    frames(0x111111U, 350, 1, 0);
    frames(REMOTE_KEY, REMOTE_TE, 3, 0);
    fck_drv_ook_code_t c[3];
    TEST_ASSERT_EQUAL_size_t(2, fck_drv_ook_decode(s_runs, s_n, c, 3));
    TEST_ASSERT_EQUAL_HEX32(REMOTE_KEY, c[0].key);
    TEST_ASSERT_EQUAL_UINT8(3, c[0].frames);
    TEST_ASSERT_EQUAL_HEX32(0x111111U, c[1].key);
    TEST_ASSERT_EQUAL_UINT8(1, c[1].frames);
    TEST_ASSERT_EQUAL_size_t(1, fck_drv_ook_decode(s_runs, s_n, c, 1));
    TEST_ASSERT_EQUAL_HEX32(REMOTE_KEY, c[0].key);
}

TEST_CASE("what is not a frame is not read as one", "[ook]")
{
    fck_drv_ook_code_t c;

    /* No sync after the 24 bits. */
    reset_runs();
    put(-8000);
    frames(REMOTE_KEY, REMOTE_TE, 1, 0);
    s_runs[s_n - 1U] = -(int32_t)(3U * REMOTE_TE);
    TEST_ASSERT_EQUAL_size_t(0, fck_drv_ook_decode(s_runs, s_n, &c, 1));

    /* te too short and too long. */
    reset_runs();
    put(-8000);
    frames(REMOTE_KEY, 60, 2, 0);
    TEST_ASSERT_EQUAL_size_t(0, fck_drv_ook_decode(s_runs, s_n, &c, 1));
    reset_runs();
    put(-80000);
    frames(REMOTE_KEY, 1500, 2, 0);
    TEST_ASSERT_EQUAL_size_t(0, fck_drv_ook_decode(s_runs, s_n, &c, 1));

    /* Bits whose high and low are the same length: no long and short. */
    reset_runs();
    put(-8000);
    for (int b = 0; b < 24; b++) {
        put(2 * 400);
        put(-2 * 400);
    }
    put(400);
    put(-31 * 400);
    TEST_ASSERT_EQUAL_size_t(0, fck_drv_ook_decode(s_runs, s_n, &c, 1));

    /* Noise alone, a lot of it. */
    reset_runs();
    for (int i = 0; i < 1000; i++) {
        put((i & 1) ? -(int32_t)(80 + rnd() % 6000) : (int32_t)(80 + rnd() % 2000));
    }
    TEST_ASSERT_EQUAL_size_t_MESSAGE(0, fck_drv_ook_decode(s_runs, s_n, &c, 1),
                                     "a key was read from noise");
    TEST_ASSERT_EQUAL_size_t(0, fck_drv_ook_decode(s_runs, 10, &c, 1));
}

/* ---- Finding the frequency -------------------------------------------------- */

TEST_CASE("the list holds 38 remote frequencies, low to high, in the CC1101's bands", "[ook]")
{
    size_t                n;
    const uint32_t *const hz = fck_drv_subghz_scan_remotes(&n);
    TEST_ASSERT_EQUAL_size_t(38, n);
    for (size_t i = 0U; i < n; i++) {
        TEST_ASSERT_TRUE((hz[i] >= 300000000U && hz[i] <= 348000000U)
                         || (hz[i] >= 387000000U && hz[i] <= 464000000U)
                         || (hz[i] >= 779000000U && hz[i] <= 928000000U));
        if (i > 0U) {
            TEST_ASSERT_GREATER_THAN_UINT32(hz[i - 1U], hz[i]);
        }
    }
}

TEST_CASE("a frequency's noise is learned from it, and a rise is measured against it", "[ook]")
{
    int16_t noise = FCK_DRV_SUBGHZ_SCAN_NO_RECEIVE;
    TEST_ASSERT_EQUAL_INT16(0, fck_drv_subghz_scan_rise(&noise, FCK_DRV_SUBGHZ_SCAN_NO_RECEIVE));
    TEST_ASSERT_EQUAL_INT16(FCK_DRV_SUBGHZ_SCAN_NO_RECEIVE, noise);    /* nothing to learn from */
    TEST_ASSERT_EQUAL_INT16(0, fck_drv_subghz_scan_rise(&noise, -200));
    TEST_ASSERT_EQUAL_INT16(-200, noise);
    TEST_ASSERT_EQUAL_INT16(-10, fck_drv_subghz_scan_rise(&noise, -210));
    TEST_ASSERT_EQUAL_INT16(-205, noise);              /* halfway down */
    TEST_ASSERT_EQUAL_INT16(25, fck_drv_subghz_scan_rise(&noise, -180));
    TEST_ASSERT_EQUAL_INT16(-204, noise);              /* half a dB up only */
    TEST_ASSERT_EQUAL_INT16(0, fck_drv_subghz_scan_rise(&noise, FCK_DRV_SUBGHZ_SCAN_NO_RECEIVE));
    TEST_ASSERT_EQUAL_INT16(-204, noise);

    /* The crystal heard on 312 MHz (12 x 26 MHz): high, but always there. It is the
     * noise of that frequency, and does not rise. */
    int16_t h = FCK_DRV_SUBGHZ_SCAN_NO_RECEIVE;
    for (uint32_t s = 0U; s < 10U; s++) {
        int16_t const r = fck_drv_subghz_scan_rise(&h, -120);
        if (s >= FCK_DRV_SUBGHZ_SCAN_LEARN_SWEEPS) {
            TEST_ASSERT_LESS_THAN_INT16(FCK_DRV_SUBGHZ_SCAN_RISE_HALF, r);
        }
    }
}

TEST_CASE("a narrow sweep peaks where the parabola does, and never on its edges", "[ook]")
{
    int16_t v[FCK_DRV_SUBGHZ_SCAN_FINE_POINTS];
    for (size_t i = 0U; i < FCK_DRV_SUBGHZ_SCAN_FINE_POINTS; i++) {
        v[i] = -200;
    }
    uint32_t const first = 315000000U - FCK_DRV_SUBGHZ_SCAN_FINE_SPAN_HZ;
    uint32_t       hz    = 0U;
    int16_t        pk    = 0;

    /* Symmetric around point 15: exactly there. */
    v[14] = -170;
    v[15] = -150;
    v[16] = -170;
    TEST_ASSERT_TRUE(fck_drv_subghz_scan_peak(v, FCK_DRV_SUBGHZ_SCAN_FINE_POINTS, first, FCK_DRV_SUBGHZ_SCAN_FINE_STEP_HZ, &hz, &pk));
    TEST_ASSERT_EQUAL_UINT32(315000000U, hz);
    TEST_ASSERT_EQUAL_INT16(-150, pk);

    /* Leaning right: between 15 and 16, never past half a step. */
    v[16] = -155;
    TEST_ASSERT_TRUE(fck_drv_subghz_scan_peak(v, FCK_DRV_SUBGHZ_SCAN_FINE_POINTS, first, FCK_DRV_SUBGHZ_SCAN_FINE_STEP_HZ, &hz, &pk));
    TEST_ASSERT_GREATER_THAN_UINT32(315000000U, hz);
    TEST_ASSERT_LESS_OR_EQUAL_UINT32(315010000U, hz);
    TEST_ASSERT_EQUAL_UINT32(0, hz % 1000U);

    /* Not 6 dB over the sweep's median: refused, outputs left alone. */
    for (size_t i = 0U; i < FCK_DRV_SUBGHZ_SCAN_FINE_POINTS; i++) {
        v[i] = -200;
    }
    v[15] = -190;
    hz    = 7U;
    TEST_ASSERT_FALSE(fck_drv_subghz_scan_peak(v, FCK_DRV_SUBGHZ_SCAN_FINE_POINTS, first, FCK_DRV_SUBGHZ_SCAN_FINE_STEP_HZ, &hz, &pk));
    TEST_ASSERT_EQUAL_UINT32(7, hz);

    /* On the edge: the skirt of something outside the sweep. */
    v[15] = -200;
    v[1]  = -120;
    TEST_ASSERT_FALSE(fck_drv_subghz_scan_peak(v, FCK_DRV_SUBGHZ_SCAN_FINE_POINTS, first, FCK_DRV_SUBGHZ_SCAN_FINE_STEP_HZ, &hz, &pk));
    v[1]                          = -200;
    v[FCK_DRV_SUBGHZ_SCAN_FINE_POINTS - 2U] = -120;
    TEST_ASSERT_FALSE(fck_drv_subghz_scan_peak(v, FCK_DRV_SUBGHZ_SCAN_FINE_POINTS, first, FCK_DRV_SUBGHZ_SCAN_FINE_STEP_HZ, &hz, &pk));
    v[FCK_DRV_SUBGHZ_SCAN_FINE_POINTS - 2U] = -200;
    v[2]                          = -120;
    TEST_ASSERT_TRUE(fck_drv_subghz_scan_peak(v, FCK_DRV_SUBGHZ_SCAN_FINE_POINTS, first, FCK_DRV_SUBGHZ_SCAN_FINE_STEP_HZ, &hz, &pk));

    TEST_ASSERT_FALSE(fck_drv_subghz_scan_peak(v, 4, first, FCK_DRV_SUBGHZ_SCAN_FINE_STEP_HZ, &hz, &pk));
}

TEST_CASE("points that never reached RX are not the peak, the median or a neighbour", "[ook]")
{
    int16_t v[FCK_DRV_SUBGHZ_SCAN_FINE_POINTS];
    for (size_t i = 0U; i < FCK_DRV_SUBGHZ_SCAN_FINE_POINTS; i++) {
        v[i] = (i < 20U) ? FCK_DRV_SUBGHZ_SCAN_NO_RECEIVE : -200;
    }
    uint32_t const first = 433920000U - FCK_DRV_SUBGHZ_SCAN_FINE_SPAN_HZ;
    uint32_t       hz    = 0U;
    int16_t        pk    = 0;

    /* 11 points there, -200 but for one at -190: 5 dB over their median.
     * Counted with the missing ones, the median would be -300. */
    v[25] = -190;
    TEST_ASSERT_FALSE(fck_drv_subghz_scan_peak(v, FCK_DRV_SUBGHZ_SCAN_FINE_POINTS, first, FCK_DRV_SUBGHZ_SCAN_FINE_STEP_HZ, &hz, &pk));

    /* A peak beside a missing point stays on its own frequency. */
    v[25] = -150;
    v[24] = FCK_DRV_SUBGHZ_SCAN_NO_RECEIVE;
    v[26] = -160;
    TEST_ASSERT_TRUE(fck_drv_subghz_scan_peak(v, FCK_DRV_SUBGHZ_SCAN_FINE_POINTS, first, FCK_DRV_SUBGHZ_SCAN_FINE_STEP_HZ, &hz, &pk));
    TEST_ASSERT_EQUAL_UINT32(first + 25U * FCK_DRV_SUBGHZ_SCAN_FINE_STEP_HZ, hz);
    TEST_ASSERT_EQUAL_INT16(-150, pk);

    for (size_t i = 0U; i < FCK_DRV_SUBGHZ_SCAN_FINE_POINTS; i++) {
        v[i] = FCK_DRV_SUBGHZ_SCAN_NO_RECEIVE;
    }
    TEST_ASSERT_FALSE(fck_drv_subghz_scan_peak(v, FCK_DRV_SUBGHZ_SCAN_FINE_POINTS, first, FCK_DRV_SUBGHZ_SCAN_FINE_STEP_HZ, &hz, &pk));
}

TEST_CASE("a remote is found after a narrow peak and two rises in 1.5 s, or one of 25 dB", "[ook]")
{
    fck_drv_subghz_scan_lock_t l = { 0 };

    /* 25 dB at once, narrowed: found. */
    TEST_ASSERT_TRUE(fck_drv_subghz_scan_lock(&l, 433930000U, true, 50, 1000));
    TEST_ASSERT_EQUAL_UINT32(433930000U, l.hz);

    /* 15 dB: not yet; again 1 s later, 60 kHz off: found, at the mean. */
    l = (fck_drv_subghz_scan_lock_t){ 0 };
    TEST_ASSERT_FALSE(fck_drv_subghz_scan_lock(&l, 314980000U, true, 30, 0));
    TEST_ASSERT_TRUE(fck_drv_subghz_scan_lock(&l, 315040000U, true, 30, 1000000));
    TEST_ASSERT_EQUAL_UINT32(315010000U, l.hz);

    /* The second rise too late, or too far: a remote of its own. */
    l = (fck_drv_subghz_scan_lock_t){ 0 };
    TEST_ASSERT_FALSE(fck_drv_subghz_scan_lock(&l, 315000000U, true, 30, 0));
    TEST_ASSERT_FALSE(fck_drv_subghz_scan_lock(&l, 315000000U, true, 30, 1600000));
    TEST_ASSERT_EQUAL_UINT32(1, l.hits);
    TEST_ASSERT_FALSE(fck_drv_subghz_scan_lock(&l, 315200000U, true, 30, 2000000));
    TEST_ASSERT_EQUAL_UINT32(1, l.hits);

    /* Never narrowed: never found, however strong; the first peak then
     * finds it, where it peaked. */
    l = (fck_drv_subghz_scan_lock_t){ 0 };
    TEST_ASSERT_FALSE(fck_drv_subghz_scan_lock(&l, 868350000U, false, 80, 0));
    TEST_ASSERT_FALSE(fck_drv_subghz_scan_lock(&l, 868350000U, false, 80, 100000));
    TEST_ASSERT_TRUE(fck_drv_subghz_scan_lock(&l, 868310000U, true, 30, 200000));
    TEST_ASSERT_EQUAL_UINT32(868310000U, l.hz);
}

TEST_CASE("a peak within 40 kHz of a listed frequency is that frequency", "[ook]")
{
    TEST_ASSERT_EQUAL_UINT32(433920000U, fck_drv_subghz_scan_snap(433950000U));
    TEST_ASSERT_EQUAL_UINT32(433920000U, fck_drv_subghz_scan_snap(433880000U));
    TEST_ASSERT_EQUAL_UINT32(433700000U, fck_drv_subghz_scan_snap(433700000U));
    TEST_ASSERT_EQUAL_UINT32(315000000U, fck_drv_subghz_scan_snap(315040000U));
}
