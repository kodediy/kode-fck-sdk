/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file fck_drv_subghz_scan.h
 * @brief How the CC1101 finds a remote whose frequency is not known: the
 *        frequencies remotes use, each one's noise learned sweep by sweep,
 *        and the peak a narrow sweep around a rise lands on.
 *
 * An absolute level would not do: twelve times the CC1101's own 26 MHz
 * crystal is exactly 312 MHz, where the chip hears it, and the bands carry whatever the city
 * around them sends. So each frequency is measured against its own past,
 * and a remote is what rises 10 dB above it.
 *
 * Plain C, like fck_drv_link.
 */
#ifndef FCK_DRV_SUBGHZ_SCAN_H_
#define FCK_DRV_SUBGHZ_SCAN_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "fck_bsp.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Sweeps that only learn the noise, before any can find. */
#define FCK_DRV_SUBGHZ_SCAN_LEARN_SWEEPS  3U
/** A rise, in half-dB over a frequency's noise, worth a narrow look (10
 *  dB), and one strong enough to trust on its own (25 dB). */
#define FCK_DRV_SUBGHZ_SCAN_RISE_HALF     20
#define FCK_DRV_SUBGHZ_SCAN_STRONG_HALF   50
/** Two rises at most this far apart, and this close in time, are the same
 *  remote pressed twice: trusted. */
#define FCK_DRV_SUBGHZ_SCAN_SAME_HZ       150000U
#define FCK_DRV_SUBGHZ_SCAN_SAME_US       1500000U
/** The narrow sweep: +/- 300 kHz in 20 kHz steps, twice, the peak kept. */
#define FCK_DRV_SUBGHZ_SCAN_FINE_SPAN_HZ  300000U
#define FCK_DRV_SUBGHZ_SCAN_FINE_STEP_HZ  20000U
#define FCK_DRV_SUBGHZ_SCAN_FINE_POINTS   31U
#define FCK_DRV_SUBGHZ_SCAN_FINE_LAPS     2U
/** The peak must stand this far over the narrow sweep's median (6 dB). */
#define FCK_DRV_SUBGHZ_SCAN_FINE_OVER_HALF 12
/** A peak this close to a frequency of the list is that frequency. */
#define FCK_DRV_SUBGHZ_SCAN_SNAP_HZ       40000U

/** A reading that is not there, in half-dB: a point the receiver never
 *  reached RX on, or a noise not learned yet. Below any real reading. */
#define FCK_DRV_SUBGHZ_SCAN_NO_RECEIVE         (-300)

/**
 * @brief The antenna leg @p hz is listened on, by fck_bsp.h's table: the
 *        top one up to FCK_BSP_LEG_TOP_MAX_HZ, the middle one up to
 *        FCK_BSP_LEG_MIDDLE_MAX_HZ, the bottom one above. Whether the CC1101
 *        tunes @p hz at all is fck_drv_cc1101_tunes()'s to say.
 */
fck_bsp_leg_t fck_drv_subghz_scan_leg(uint32_t hz);

/**
 * @brief Whether @p hz's leg can be had: with the module's expander, any;
 *        without it - a GNSS receiver holding the bus - only the legs whose
 *        SW0 is the level it rests at, FCK_BSP_LEG_SW0_REST (433's, and 868
 *        and 915's).
 */
bool fck_drv_subghz_scan_leg_open(uint32_t hz, bool expander);

/** What a transmission keeps clear of a sub-band's edges, beyond its own
 *  width: the carrier lands up to +/-20 ppm off with the 26 MHz crystal's
 *  tolerance - 8.7 kHz at 433.92 MHz, 17.4 kHz at 868.35 - plus half a
 *  synthesizer step. */
#define FCK_DRV_SUBGHZ_SCAN_TX_GUARD_HZ    20000U
/** The width an OOK remote's signal is taken to occupy: keyed at a te of
 *  100 us or more, its sidebands stay well within it. */
#define FCK_DRV_SUBGHZ_SCAN_OOK_WIDTH_HZ   50000U

/**
 * @brief Whether the module may send an OOK remote's signal on @p hz:
 *        fck_drv_subghz_scan_may_transmit_width() with
 *        FCK_DRV_SUBGHZ_SCAN_OOK_WIDTH_HZ, so inside one of the two European
 *        SRD bands it sends in (fck_bsp.h, 433.05-434.79 and 868.0-868.6 MHz)
 *        and 45 kHz clear of their edges. Listening is legal wherever
 *        fck_drv_cc1101_tunes() is; sending only here. A remote of another
 *        region - 315 MHz, 915 MHz - is received, never sent.
 */
bool fck_drv_subghz_scan_may_transmit(uint32_t hz);

/**
 * @brief Whether a transmission centred on @p hz that occupies @p occupied_hz
 *        (Carson's rule: 2 * deviation + baud) stays inside one of the two
 *        sub-bands with FCK_DRV_SUBGHZ_SCAN_TX_GUARD_HZ to spare: the whole of
 *        [@p hz - @p occupied_hz / 2 - guard, @p hz + @p occupied_hz / 2 +
 *        guard], half rounded up, between a band's two edges, edges
 *        included.
 */
bool fck_drv_subghz_scan_may_transmit_width(uint32_t hz, uint32_t occupied_hz);

/**
 * The most one transmission - one call, all its repeats - may keep the
 * carrier on, in milliseconds. A real remote's 16 EV1527 frames at a te of
 * 1100 us keep it on for about 1.3 s (2.3 s with the gaps, which the hour's
 * budget counts), so 3 s lets any of them through and nothing like a runaway: the runs of a call are bounded one by one, not as a sum.
 * The hour's budget (fck_drv_subghz_duty.h) is separate, and smaller.
 */
#define FCK_DRV_SUBGHZ_SCAN_SEND_MS_MAX  3000U

/**
 * @brief How long a call puts the carrier on: the positive runs of @p runs
 *        (signed microseconds, positive the carrier on) summed, times
 *        @p repeats. Saturates at UINT64_MAX instead of wrapping.
 */
uint64_t fck_drv_subghz_scan_on_air_us(const int32_t *runs, size_t n, uint8_t repeats);

/**
 * @brief Whether @p on_air_us of carrier is within one call's cap,
 *        FCK_DRV_SUBGHZ_SCAN_SEND_MS_MAX: exactly the cap is, one
 *        microsecond more is not. For a packet, its air time times its
 *        repeats (the carrier is on for all of it).
 */
bool fck_drv_subghz_scan_send_fits(uint64_t on_air_us);

/** How many frequencies fck_drv_subghz_scan_remotes() gives. */
#define FCK_DRV_SUBGHZ_SCAN_REMOTES 38U

/** @brief The frequencies remotes use, in Hz, low to high: 38 of them, in
 *         the CC1101's three bands. */
const uint32_t *fck_drv_subghz_scan_remotes(size_t *n);

/* ---- Listening with no frequency: hopping ------------------------------------- */

/** The frequencies a listen with no frequency hops over, and how long it
 *  waits on each. */
#define FCK_DRV_SUBGHZ_SCAN_HOPS              6U
#define FCK_DRV_SUBGHZ_SCAN_HOP_MS            40U
/** A remote strong and near bleeds a few dB into the hops beside it on its
 *  leg (310, 315 and 318 MHz are 3 to 5 MHz apart): before staying where it
 *  was heard, the hops this close are each listened to for this long... */
#define FCK_DRV_SUBGHZ_SCAN_NEIGHBOUR_HZ      10000000U
#define FCK_DRV_SUBGHZ_SCAN_NEIGHBOUR_MS      25U
/** ...and the one this much louder (6 dB) is where it stays. */
#define FCK_DRV_SUBGHZ_SCAN_NEIGHBOUR_OVER_HALF 12
/** A capture no code reads is a remote from this many runs on - one frame
 *  of 24 bits and its sync are 50 -, and a click of noise below. */
#define FCK_DRV_SUBGHZ_SCAN_WORTH_RUNS        48U
/** Hopping, the listen stays on a hop only where it reads a code, and hops
 *  again this long after the last code read there. A capture with no code
 *  does not hold it: otherwise a device sending on 310 MHz every half
 *  second would hold a listen there for good, deaf to a remote on 433.92
 *  and on 868.35 MHz. */
#define FCK_DRV_SUBGHZ_SCAN_STAY_MS           5000U

/** @brief The hops, in the order they are listened to: 433.92, 315 and
 *         868.35 MHz first, the remotes' own, then 310, 318 and 390. */
const uint32_t *fck_drv_subghz_scan_hops(size_t *n);

/** @brief Whether @p b is a neighbour of @p a: another hop on the same leg,
 *         within FCK_DRV_SUBGHZ_SCAN_NEIGHBOUR_HZ. */
bool fck_drv_subghz_scan_neighbours(uint32_t a, uint32_t b);

/** @brief Whether @p half, a neighbour's loudest reading, is
 *         FCK_DRV_SUBGHZ_SCAN_NEIGHBOUR_OVER_HALF over @p best_half; a reading
 *         that is not there never is. */
bool fck_drv_subghz_scan_louder(int16_t half, int16_t best_half);

/** @brief Whether a capture of @p runs is a remote: one a code was read
 *         from, or one of FCK_DRV_SUBGHZ_SCAN_WORTH_RUNS runs or more. */
bool fck_drv_subghz_scan_worth(size_t runs, bool decoded);

/**
 * @brief Where a listen is held, read after read: fck_drv_cc1101_listen()
 *        keeps one, and the rule is here, without the radio.
 *
 * A listen started on a frequency stays there. One started hopping is held
 * on a hop only by a code read there, until FCK_DRV_SUBGHZ_SCAN_STAY_MS after
 * the last one, and goes back to hopping then - or as soon as the hop's leg
 * cannot be had (fck_drv_subghz_scan_leg_open()).
 */
typedef struct {
    uint32_t hz;       /**< Where it listens; 0: hopping.               */
    bool     hops;     /**< Started hopping: @c hz is held by a code.   */
    int64_t  until_us; /**< Started hopping: held on @c hz until then.  */
} fck_drv_subghz_scan_hold_t;

/** @brief A listen on @p hz, or hopping with 0. */
fck_drv_subghz_scan_hold_t fck_drv_subghz_scan_hold_start(uint32_t hz);

/** @brief Back to hopping when the hold on a hop is over at @p now_us, or its
 *         leg cannot be had with or without the @p expander. A listen started
 *         on a frequency is left alone. */
void fck_drv_subghz_scan_hold_check(fck_drv_subghz_scan_hold_t *hold, int64_t now_us, bool expander);

/** @brief When a read on @p hold's frequency ends: at @p end_us, or sooner
 *         when the hold on a hop is over first. */
int64_t fck_drv_subghz_scan_hold_end(const fck_drv_subghz_scan_hold_t *hold, int64_t end_us);

/** @brief A capture heard on @p hz at @p now_us: started hopping, a code read
 *         from it (@p decoded) holds the listen on @p hz until
 *         FCK_DRV_SUBGHZ_SCAN_STAY_MS later; without a code, nothing changes. */
void fck_drv_subghz_scan_hold_heard(fck_drv_subghz_scan_hold_t *hold, uint32_t hz, bool decoded,
                                    int64_t now_us);

/**
 * @brief One frequency's reading against its noise, and the noise learned
 *        from it: the first reading is the noise; after that a lower
 *        reading pulls it halfway down, and a higher one lets it creep up
 *        half a dB a sweep.
 * @param noise The frequency's noise, in half-dB, kept by the caller from
 *              FCK_DRV_SUBGHZ_SCAN_NO_RECEIVE.
 * @param half  The reading; FCK_DRV_SUBGHZ_SCAN_NO_RECEIVE leaves the noise as it was.
 * @return How far @p half stands above the noise it had; 0 for the first
 *         reading and for one that is not there.
 */
int16_t fck_drv_subghz_scan_rise(int16_t *noise, int16_t half);

/**
 * @brief Where a narrow sweep peaks.
 *
 * Refused when the peak does not stand FCK_DRV_SUBGHZ_SCAN_FINE_OVER_HALF over the
 * sweep's median, or lies on either of the sweep's two outermost points on
 * each side - the skirt of something outside it, not a remote. Otherwise
 * the parabola through the peak and its two neighbours, no further than
 * half a step from the peak, rounded to 1 kHz; the peak's own point when a
 * neighbour is not there. Points that are not there (FCK_DRV_SUBGHZ_SCAN_NO_RECEIVE) are
 * never the peak and do not count in the median.
 *
 * @param half      @p n readings, @p first_hz + i * @p step_hz; 5 <= n <= 64.
 * @param hz        Out: where it peaks.
 * @param peak_half Out: its reading.
 * @return false when refused; @p hz and @p peak_half are left alone.
 */
bool fck_drv_subghz_scan_peak(const int16_t *half, size_t n, uint32_t first_hz, uint32_t step_hz,
                   uint32_t *hz, int16_t *peak_half);

/** @brief @p hz, or the frequency of the list within FCK_DRV_SUBGHZ_SCAN_SNAP_HZ of
 *         it. */
uint32_t fck_drv_subghz_scan_snap(uint32_t hz);

/** @brief The rises taken for one remote, in a row. Zeroed to start. */
typedef struct {
    uint32_t hits;
    uint32_t fines;   /**< Of them, the ones a narrow sweep peaked for. */
    uint64_t sum;     /**< Of those peaks.                              */
    uint32_t hz;      /**< Their mean; the last rise's own before one.  */
    int64_t  t_us;
} fck_drv_subghz_scan_lock_t;

/**
 * @brief One more rise, and whether it finds the remote.
 *
 * A rise further than FCK_DRV_SUBGHZ_SCAN_SAME_HZ from the remote so far, or later than
 * FCK_DRV_SUBGHZ_SCAN_SAME_US after its last one, starts another. The remote is found
 * once a narrow sweep has peaked for it and it has risen twice, or once by
 * FCK_DRV_SUBGHZ_SCAN_STRONG_HALF.
 *
 * @param hz       Where the narrow sweep peaked; the list's frequency when
 *                 it did not.
 * @param narrowed Whether it peaked.
 * @param rise     How far the list's frequency rose over its noise.
 * @param t_us     When, in microseconds.
 * @return true when found; @p lock's hz is where.
 */
bool fck_drv_subghz_scan_lock(fck_drv_subghz_scan_lock_t *lock, uint32_t hz, bool narrowed, int16_t rise,
                   int64_t t_us);

#ifdef __cplusplus
}
#endif

#endif /* FCK_DRV_SUBGHZ_SCAN_H_ */
