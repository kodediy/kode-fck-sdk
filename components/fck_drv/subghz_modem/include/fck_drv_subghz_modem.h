/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file fck_drv_subghz_modem.h
 * @brief The CC1101's packet receiver, in numbers: the registers a 2-FSK or
 *        GFSK packet with a sync word is received with, from its baud rate,
 *        deviation and filter, and what the chip appends to each packet.
 *
 * The formulas are the datasheet's (SWRS061I, 26 MHz crystal):
 *   baud      = (256 + DRATE_M) * 2^DRATE_E * fXOSC / 2^28           [S12]
 *   deviation = fXOSC / 2^17 * (8 + DEVIATION_M) * 2^DEVIATION_E     [S16.1]
 *   filter    = fXOSC / (8 * (4 + CHANBW_M) * 2^CHANBW_E)            [S13]
 * Each is the nearest the chip has; the filter, the narrowest that is not
 * narrower than asked. What is not computed is fixed: the block
 * fck_drv_cc1101 writes first, 30 of 32 sync bits, 4
 * bytes of preamble, fixed length, status appended, no address, and the
 * IF, AGC and front end chosen from the filter as its level receiver does.
 *
 * The same numbers say what a packet sent with them takes on air and how
 * much spectrum it occupies (Carson's rule), for the transmitter's checks.
 *
 * Plain C, like fck_drv_ook. Nothing here transmits, or could.
 */
#ifndef FCK_DRV_SUBGHZ_MODEM_H_
#define FCK_DRV_SUBGHZ_MODEM_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** What one packet may carry: the FIFO's 64 bytes, less the two appended. */
#define FCK_DRV_SUBGHZ_MODEM_LENGTH_MAX     61U
#define FCK_DRV_SUBGHZ_MODEM_BAUD_MIN       600U
#define FCK_DRV_SUBGHZ_MODEM_BAUD_MAX       250000U
#define FCK_DRV_SUBGHZ_MODEM_DEVIATION_MIN  1586U     /* E 0, M 0 */
#define FCK_DRV_SUBGHZ_MODEM_DEVIATION_MAX  380859U   /* E 7, M 7 */
#define FCK_DRV_SUBGHZ_MODEM_FILTER_MIN     58035U    /* E 3, M 3 */
#define FCK_DRV_SUBGHZ_MODEM_FILTER_MAX     812500U   /* E 0, M 0 */

/** @brief A packet receiver. */
typedef struct {
    bool     gfsk;           /**< GFSK; 2-FSK otherwise.                    */
    uint32_t baud;
    uint32_t deviation_hz;
    uint32_t filter_hz;      /**< The channel filter's width.                */
    uint16_t sync_word;      /**< Sent highest byte first: SYNC1, SYNC0.     */
    uint8_t  length;         /**< Fixed, 1 to FCK_DRV_SUBGHZ_MODEM_LENGTH_MAX. */
    bool     crc;            /**< CRC-16 over the payload.                   */
    bool     whitening;      /**< PN9 data whitening.                        */
} fck_drv_subghz_modem_t;

/** @brief The registers that change with the receiver, over the base block. */
typedef struct {
    uint8_t sync1;
    uint8_t sync0;
    uint8_t pktlen;
    uint8_t pktctrl0;
    uint8_t fsctrl1;
    uint8_t mdmcfg4;
    uint8_t mdmcfg3;
    uint8_t mdmcfg2;
    uint8_t deviatn;
    uint8_t agcctrl2;
    uint8_t frend1;
    uint8_t test2;
    uint8_t test1;
} fck_drv_subghz_modem_registers_t;

/** @brief DRATE_E and DRATE_M nearest @p baud, and the baud they give.
 *  @return false outside FCK_DRV_SUBGHZ_MODEM_BAUD_MIN to _MAX. */
bool fck_drv_subghz_modem_baud(uint32_t baud, uint8_t *e, uint8_t *m, uint32_t *got);

/** @brief DEVIATION_E and _M nearest @p hz, and the deviation they give.
 *  @return false outside FCK_DRV_SUBGHZ_MODEM_DEVIATION_MIN to _MAX. */
bool fck_drv_subghz_modem_deviation(uint32_t hz, uint8_t *e, uint8_t *m, uint32_t *got);

/** @brief CHANBW_E and _M of the narrowest filter not narrower than @p hz,
 *         and its width; FCK_DRV_SUBGHZ_MODEM_FILTER_MIN for less.
 *  @return false over FCK_DRV_SUBGHZ_MODEM_FILTER_MAX. */
bool fck_drv_subghz_modem_filter(uint32_t hz, uint8_t *e, uint8_t *m, uint32_t *got);

/** @brief The registers for @p s. @return false, @p r untouched, for a value
 *         out of range or a length of 0 or over the maximum. */
bool fck_drv_subghz_modem_registers(const fck_drv_subghz_modem_t *s, fck_drv_subghz_modem_registers_t *r);

/** What the chip puts on air around a packet's bytes, from the base block:
 *  4 bytes of preamble (MDMCFG1 NUM_PREAMBLE 4) and the sync word twice, which
 *  is how SYNC_MODE 30 of 32 bits sends it, and 2 bytes of CRC-16 when on. */
#define FCK_DRV_SUBGHZ_MODEM_PREAMBLE_BYTES 4U
#define FCK_DRV_SUBGHZ_MODEM_SYNC_BYTES     4U
#define FCK_DRV_SUBGHZ_MODEM_CRC_BYTES      2U

/** @brief How long one packet of @p s is on air, in microseconds, at the
 *         baud rate the chip actually gets, rounded up.
 *  @return false as fck_drv_subghz_modem_registers() refuses @p s. */
bool fck_drv_subghz_modem_air_time_us(const fck_drv_subghz_modem_t *s, uint32_t *us);

/** @brief The spectrum @p s occupies, by Carson's rule: twice the deviation
 *         plus the baud rate, both as the chip actually gets them.
 *  @return false as fck_drv_subghz_modem_registers() refuses @p s. */
bool fck_drv_subghz_modem_occupied_hz(const fck_drv_subghz_modem_t *s, uint32_t *hz);

/** @brief Whether a packet of @p s may be sent centred on @p hz: all of its
 *         occupied bandwidth inside one of the two sub-bands
 *         (fck_drv_subghz_scan_may_transmit_width()). */
bool fck_drv_subghz_modem_may_transmit(const fck_drv_subghz_modem_t *s, uint32_t hz);

/** @brief What the chip appended to a packet, and its FREQEST read after it. */
typedef struct {
    int16_t half;              /**< RSSI at the sync word, in half-dB.          */
    uint8_t link_quality;      /**< LQI, 0 to 127: lower is cleaner.            */
    bool    crc_ok;            /**< false too when CRC is off.                  */
    int32_t frequency_error_hz;   /**< The carrier's offset from the tuning.  */
} fck_drv_subghz_modem_status_t;

/** @brief Read the two status bytes (RSSI, then CRC_OK and LQI) and FREQEST
 *         (one step is fXOSC / 2^14, 1587 Hz) [S17.3, S14.1]. */
fck_drv_subghz_modem_status_t fck_drv_subghz_modem_status(uint8_t rssi, uint8_t crc_lqi, uint8_t freqest,
                                                          bool crc);

#ifdef __cplusplus
}
#endif

#endif /* FCK_DRV_SUBGHZ_MODEM_H_ */
