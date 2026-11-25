/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file fck_drv_subghz_modem.c
 * @brief The CC1101's packet receiver, in numbers. See fck_drv_subghz_modem.h.
 *
 * Every exponent and mantissa is tried: 4096 pairs for the baud rate, 64 for
 * the deviation, 16 for the filter. A search that cannot miss is worth more
 * here than the logarithm that would skip it.
 */
#include "fck_drv_subghz_modem.h"

#include "fck_drv_subghz_scan.h"

#define XOSC_HZ 26000000U

/* RSSI's offset, in half-dB: 74 dB [S17.3], as fck_drv_cc1101 reads it. */
#define RSSI_OFFSET_HALF 148
/* The level receiver's rules (fck_drv_cc1101.c level_profile()): the IF at
 * half the filter plus two steps, and the front end of a wide filter. */
#define IF_STEP_HZ       25391U
#define IF_MIN           0x06U
#define IF_MAX           0x0EU
#define WIDE_HZ          325000U

static uint32_t modem_baud_of(uint32_t e, uint32_t m)
{
    uint64_t const mantissa = (uint64_t)256U + (uint64_t)m;
    return (uint32_t)(((mantissa << e) * (uint64_t)XOSC_HZ) >> 28);
}

static uint32_t modem_deviation_of(uint32_t e, uint32_t m)
{
    uint64_t const mantissa = (uint64_t)8U + (uint64_t)m;
    return (uint32_t)((((uint64_t)XOSC_HZ * mantissa) << e) >> 17);
}

static uint32_t modem_filter_of(uint32_t e, uint32_t m)
{
    return XOSC_HZ / ((8U * (4U + m)) << e);
}

static uint32_t modem_distance(uint32_t a, uint32_t b)
{
    return (a > b) ? a - b : b - a;
}

bool fck_drv_subghz_modem_baud(uint32_t baud, uint8_t *e, uint8_t *m, uint32_t *got)
{
    if (e == NULL || m == NULL || got == NULL || baud < FCK_DRV_SUBGHZ_MODEM_BAUD_MIN
        || baud > FCK_DRV_SUBGHZ_MODEM_BAUD_MAX) {
        return false;
    }
    uint32_t best = UINT32_MAX;
    for (uint32_t ee = 0U; ee < 16U; ee++) {
        for (uint32_t mm = 0U; mm < 256U; mm++) {
            uint32_t const d = modem_distance(modem_baud_of(ee, mm), baud);
            if (d < best) {
                best = d;
                *e   = (uint8_t)ee;
                *m   = (uint8_t)mm;
            }
        }
    }
    *got = modem_baud_of(*e, *m);
    return true;
}

bool fck_drv_subghz_modem_deviation(uint32_t hz, uint8_t *e, uint8_t *m, uint32_t *got)
{
    if (e == NULL || m == NULL || got == NULL || hz < FCK_DRV_SUBGHZ_MODEM_DEVIATION_MIN
        || hz > FCK_DRV_SUBGHZ_MODEM_DEVIATION_MAX) {
        return false;
    }
    uint32_t best = UINT32_MAX;
    for (uint32_t ee = 0U; ee < 8U; ee++) {
        for (uint32_t mm = 0U; mm < 8U; mm++) {
            uint32_t const d = modem_distance(modem_deviation_of(ee, mm), hz);
            if (d < best) {
                best = d;
                *e   = (uint8_t)ee;
                *m   = (uint8_t)mm;
            }
        }
    }
    *got = modem_deviation_of(*e, *m);
    return true;
}

bool fck_drv_subghz_modem_filter(uint32_t hz, uint8_t *e, uint8_t *m, uint32_t *got)
{
    if (e == NULL || m == NULL || got == NULL || hz > FCK_DRV_SUBGHZ_MODEM_FILTER_MAX) {
        return false;
    }
    /* From the narrowest up, E 3 and M 3, to the widest, E 0 and M 0: the
     * first that is not narrower than asked. The widest always is. */
    uint32_t const want = (hz < FCK_DRV_SUBGHZ_MODEM_FILTER_MIN) ? FCK_DRV_SUBGHZ_MODEM_FILTER_MIN : hz;
    uint32_t       k    = 15U;
    while (k > 0U && modem_filter_of(k / 4U, k % 4U) < want) {
        k--;
    }
    *e   = (uint8_t)(k / 4U);
    *m   = (uint8_t)(k % 4U);
    *got = modem_filter_of(k / 4U, k % 4U);
    return true;
}

bool fck_drv_subghz_modem_registers(const fck_drv_subghz_modem_t *s, fck_drv_subghz_modem_registers_t *r)
{
    uint8_t  de;
    uint8_t  dm;
    uint8_t  ve;
    uint8_t  vm;
    uint8_t  fe;
    uint8_t  fm;
    uint32_t baud;
    uint32_t dev;
    uint32_t bw;
    if (s == NULL || r == NULL || s->length == 0U || s->length > FCK_DRV_SUBGHZ_MODEM_LENGTH_MAX
        || !fck_drv_subghz_modem_baud(s->baud, &de, &dm, &baud)
        || !fck_drv_subghz_modem_deviation(s->deviation_hz, &ve, &vm, &dev)
        || !fck_drv_subghz_modem_filter(s->filter_hz, &fe, &fm, &bw)) {
        return false;
    }
    uint32_t fif = bw / 2U / IF_STEP_HZ + 2U;
    fif          = (fif < IF_MIN) ? IF_MIN : (fif > IF_MAX) ? IF_MAX : fif;
    bool const wide = bw >= WIDE_HZ;
    *r = (fck_drv_subghz_modem_registers_t){
        .sync1    = (uint8_t)(s->sync_word >> 8),
        .sync0    = (uint8_t)(s->sync_word & 0xFFU),
        .pktlen   = s->length,
        /* WHITE_DATA, CRC_EN, LENGTH_CONFIG 00: fixed [S15.3]. */
        .pktctrl0 = (uint8_t)((s->whitening ? 0x40U : 0x00U) | (s->crc ? 0x04U : 0x00U)),
        .fsctrl1  = (uint8_t)fif,
        .mdmcfg4  = (uint8_t)((uint32_t)fe << 6 | (uint32_t)fm << 4 | de),
        .mdmcfg3  = dm,
        /* MOD_FORMAT 2-FSK (000) or GFSK (001), SYNC_MODE 30 of 32 (011). */
        .mdmcfg2  = (uint8_t)((s->gfsk ? 0x10U : 0x00U) | 0x03U),
        .deviatn  = (uint8_t)((uint32_t)ve << 4 | vm),
        /* SmartRF Studio's: the highest gain target for the slowest rates. */
        .agcctrl2 = (s->baud <= 2400U) ? 0x03U : 0x43U,
        .frend1   = wide ? 0xB6U : 0x56U,
        .test2    = wide ? 0x88U : 0x81U,
        .test1    = wide ? 0x31U : 0x35U,
    };
    return true;
}

bool fck_drv_subghz_modem_air_time_us(const fck_drv_subghz_modem_t *s, uint32_t *us)
{
    fck_drv_subghz_modem_registers_t r;
    uint8_t                          e;
    uint8_t                          m;
    uint32_t                         baud;
    if ((us == NULL) || !fck_drv_subghz_modem_registers(s, &r)
        || !fck_drv_subghz_modem_baud(s->baud, &e, &m, &baud)) {
        return false;
    }
    uint64_t const bytes = (uint64_t)FCK_DRV_SUBGHZ_MODEM_PREAMBLE_BYTES + FCK_DRV_SUBGHZ_MODEM_SYNC_BYTES
                           + s->length + (s->crc ? FCK_DRV_SUBGHZ_MODEM_CRC_BYTES : 0U);
    uint64_t const bits  = bytes * 8U;
    *us                  = (uint32_t)(((bits * 1000000U) + baud - 1U) / baud);
    return true;
}

bool fck_drv_subghz_modem_occupied_hz(const fck_drv_subghz_modem_t *s, uint32_t *hz)
{
    fck_drv_subghz_modem_registers_t r;
    uint8_t                          e;
    uint8_t                          m;
    uint32_t                         baud;
    uint32_t                         dev;
    if ((hz == NULL) || !fck_drv_subghz_modem_registers(s, &r) || !fck_drv_subghz_modem_baud(s->baud, &e, &m, &baud)
        || !fck_drv_subghz_modem_deviation(s->deviation_hz, &e, &m, &dev)) {
        return false;
    }
    *hz = (2U * dev) + baud;
    return true;
}

bool fck_drv_subghz_modem_may_transmit(const fck_drv_subghz_modem_t *s, uint32_t hz)
{
    uint32_t occupied;
    return fck_drv_subghz_modem_occupied_hz(s, &occupied) && fck_drv_subghz_scan_may_transmit_width(hz, occupied);
}

fck_drv_subghz_modem_status_t fck_drv_subghz_modem_status(uint8_t rssi, uint8_t crc_lqi, uint8_t freqest,
                                                          bool crc)
{
    return (fck_drv_subghz_modem_status_t){
        .half               = (int16_t)((int32_t)(int8_t)rssi - RSSI_OFFSET_HALF),
        .link_quality       = (uint8_t)(crc_lqi & 0x7FU),
        .crc_ok             = crc && (crc_lqi & 0x80U) != 0U,
        .frequency_error_hz = (int32_t)(((int64_t)(int8_t)freqest * (int64_t)XOSC_HZ) / 16384),
    };
}
