/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file fake_fck_aos.h
 * @brief An owner that writes down what it is asked and answers what it is
 *        told to: the facade's checks and translations, seen from below.
 *
 * fck.c, fck_wifi.c, fck_radio.c and fck_gnss.c are the real facade; this
 * stands in for fck_aos.c. A case sets what the owner should answer (the
 * firmware each chip announces, the lines a command is answered with, its rc, a
 * fix) and calls a facade function; it reads back the calls the owner was
 * handed, in order, and the one command - its line, verb and wait - that the
 * facade sent, to see that the facade mapped the call right and refused what
 * it should before ever reaching the owner.
 */
#ifndef FAKE_FCK_AOS_H_
#define FAKE_FCK_AOS_H_

#include <stdbool.h>
#include <stdint.h>

#include "fck_aos.h"

/** What the owner was asked, in the order it was asked (the last CALLS_MAX). */
typedef enum {
    CALL_START = 1,
    CALL_STOP,
    CALL_CANCEL,
    CALL_WAIT_PRESENT,
    CALL_FIRMWARE,
    CALL_COMMAND,
    CALL_HOLD,
    CALL_RELEASE,
    CALL_POWER,
    CALL_RESET,
    CALL_GNSS_POWER,
    CALL_GNSS_FIX,
} fake_call_t;

#define FAKE_CALLS_MAX   16U
#define FAKE_LINES_MAX   8U
#define FAKE_TEXT_MAX    160U

typedef struct {
    const char *tag;
    const char *body;
} fake_reply_t;

typedef struct {
    /* What the owner answers. */
    fck_aos_status_t st;            /* fck_aos_status()                         */
    bool             present;       /* fck_aos_present()                        */
    kode_status_t    start_rc;
    kode_status_t    stop_rc;
    kode_status_t    cancel_rc;
    const char      *why;           /* fck_aos_error_set()'s last                 */
    uint32_t         cancel_gen;    /* the count fck_aos_cancel_mark() reads      */
    unsigned         cancel_after;  /* a fck_cancel() lands after this many commands; 0 never */
    kode_status_t    wait_rc;
    kode_status_t    firmware_rc[FCK_AOS_CHIPS];
    fck_aos_firmware_t  firmware[FCK_AOS_CHIPS];   /* what each chip announced        */
    kode_status_t    command_rc;    /* the command's own status                  */
    int32_t          command_out;   /* the firmware's #DONE rc                      */
    fake_reply_t     replies[FAKE_LINES_MAX]; /* played to the reader, tag NULL ends */
    kode_status_t    hold_rc;
    kode_status_t    power_rc;
    kode_status_t    reset_rc;
    kode_status_t    gnss_power_rc;
    kode_status_t    gnss_fix_rc;
    fck_drv_nmea_fix_t fix;
    uint32_t         fix_age_ms;    /* what the owner says of the fix's age       */
    uint8_t          pos_ret;

    /* What it was asked. */
    fake_call_t      calls[FAKE_CALLS_MAX];
    unsigned         n_calls;
    unsigned         commands;      /* fck_aos_command_if_same_firmware()           */
    fck_aos_chip_t   cmd_chip;
    uint32_t         cmd_seq;
    char             cmd_line[FAKE_TEXT_MAX];
    char             cmd_verb[FAKE_TEXT_MAX];
    uint32_t         cmd_ms;
    uint32_t         last_ms;       /* a wait's or a hold's time                 */
    fck_aos_chip_t   last_chip;     /* power's, reset's, hold's                  */
    bool             last_on;       /* power's, gnss_power's                     */
    unsigned         gnss_powers;
    bool             gnss_power_ons[4];
    uint32_t         fix_ms;

    unsigned         posts;         /* fck_aos_command_post()                    */
    char             post_line[FAKE_TEXT_MAX];
    kode_status_t    post_rc;
} fake_fck_aos_t;

extern fake_fck_aos_t g_aos;

/** @brief Nothing asked, everything answered OK, an F*CK present, no firmware known. */
void fake_fck_aos_reset(void);

/** @brief Chip @p chip announces @p body (what follows #C5INFO or #NRFINFO). */
void fake_fck_aos_firmware(fck_aos_chip_t chip, const char *body);

/** @brief The calls so far contain @p c. */
bool fake_fck_aos_called(fake_call_t c);

#endif /* FAKE_FCK_AOS_H_ */
