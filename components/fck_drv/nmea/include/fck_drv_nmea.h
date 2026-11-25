/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file fck_drv_nmea.h
 * @brief The GNSS receiver's sentences, read into a fix.
 *
 * The module's ME31GR02 talks NMEA 0183 on a UART that reaches the C5 only.
 * With the C5 firmware's relay on (fck_drv_c5_command_gnss()), every sentence arrives
 * as the body of a #GNSSU line, checksum and all, exactly as the receiver
 * sent it:
 *
 *     #GNSSU $GNGGA,202634.00,4807.0380,N,01131.0000,E,1,04,6.2,109.418,M,-3.399,M,,*hh
 *     #GNSSU $GNRMC,202634.00,A,4807.0380,N,01131.0000,E,2.80,95.05,290926,,,A,V*hh
 *
 * This file reads the two that make a fix: GGA (position, height, quality,
 * satellites used, HDOP) and RMC (validity, speed, course, date). Each
 * sentence updates the fields it carries and leaves the others as they
 * were, so after one of each the fix is whole. Any talker is taken (GN for
 * the combined solution, GP, GL, GA, GB, BD), and every other sentence is
 * counted and left alone: GST, GSV, GSA, and the receiver's proprietary
 * $PO... lines.
 *
 * A position is private. Nothing here prints one, and nothing that uses
 * this file should log one either.
 *
 * Plain C: no ESP-IDF and no FreeRTOS, so tests/host runs it as it is. It
 * keeps no time of its own; whoever feeds it knows when a sentence came.
 */
#ifndef FCK_DRV_NMEA_H_
#define FCK_DRV_NMEA_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief What one sentence was. */
typedef enum {
    FCK_DRV_NMEA_BAD = 0,  /**< Not a sentence: no '$', no checksum, a checksum
                        *   that does not match, a byte out of place, or a
                        *   GGA or RMC whose fields do not read.           */
    FCK_DRV_NMEA_OTHER,    /**< A sound sentence this file does not read.     */
    FCK_DRV_NMEA_GGA,      /**< Read into the fix.                            */
    FCK_DRV_NMEA_RMC,      /**< Read into the fix.                            */
} fck_drv_nmea_kind_t;

/**
 * @brief A fix, as the latest GGA and RMC tell it.
 *
 * A number a sentence left empty reads as NAN (math.h), or as 0 for the
 * whole ones, so a value is never made up. Without a fix the receiver
 * still sends both sentences, with no position and quality 0; RMC usually
 * keeps the date and the time once it has them.
 */
typedef struct {
    uint8_t  quality;    /**< GGA's fix quality: 0 none, 1 GNSS, 2 differential,
                          *   4 RTK fixed, 5 RTK float, 6 estimated.         */
    uint8_t  sats;       /**< GGA: the satellites used in the fix.          */
    bool     valid;      /**< RMC: status A, a fix the receiver vouches for. */
    double   lat;        /**< Degrees, north positive. NAN without one.     */
    double   lon;        /**< Degrees, east positive. NAN without one.      */
    double   alt_m;      /**< GGA: above mean sea level. NAN without one.   */
    double   hdop;       /**< GGA: the horizontal dilution of precision.
                          *   The ME31GR02 says 100.00 with no fix.         */
    double   speed_kn;   /**< RMC: over the ground, in knots.               */
    double   course_deg; /**< RMC: over the ground, from true north.        */
    bool     has_time;   /**< hour, min, sec and ms hold a UTC time.        */
    uint8_t  hour;
    uint8_t  min;
    uint8_t  sec;        /**< Up to 60, for a leap second.                  */
    uint16_t ms;
    bool     has_date;   /**< day, month and year hold a UTC date (RMC).    */
    uint8_t  day;
    uint8_t  month;
    uint16_t year;       /**< 2000 to 2099: RMC carries two digits.         */
} fck_drv_nmea_fix_t;

/** @brief A reader: the fix so far, and what it has been fed. */
typedef struct {
    fck_drv_nmea_fix_t fix;
    uint32_t       gga;     /**< GGA sentences read.                        */
    uint32_t       rmc;     /**< RMC sentences read.                        */
    uint32_t       other;   /**< Sound sentences of any other kind.        */
    uint32_t       bad;     /**< Sentences refused.                         */
} fck_drv_nmea_t;

/** @brief Start empty: no fix, every number NAN or 0, every count 0. */
void fck_drv_nmea_init(fck_drv_nmea_t *n);

/**
 * @brief Whether @p s is a sentence: '$', printable ASCII, '*' and two hex
 *        digits that are the XOR of every byte between them, and nothing
 *        after. A line ending is allowed and ignored.
 */
bool fck_drv_nmea_checksum_ok(const char *s);

/**
 * @brief Read one sentence: the body of a #GNSSU line.
 *
 * A GGA or RMC updates @p n's fix only when the whole sentence reads; a
 * sentence that does not leaves the fix as it was and is counted bad.
 *
 * @return What @p s was.
 */
fck_drv_nmea_kind_t fck_drv_nmea_feed(fck_drv_nmea_t *n, const char *s);

#ifdef __cplusplus
}
#endif

#endif /* FCK_DRV_NMEA_H_ */
