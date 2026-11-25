/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file fck_drv_link.h
 * @brief The line protocol both of the module's firmwares speak, read.
 *
 * The C5 firmware (fck-c5) and the nRF firmware (fck-nrf) answer the
 * same way: one command per line, and back a run of lines of three kinds.
 *
 *     #AP ssid="Kode \"lab\"" bssid=aa:bb:cc:dd:ee:ff ch=6 rssi=-61   machine
 *     (scan: 5 GHz passive on 100-140)                                 human
 *     #DONE scan rc=0                                                  the end
 *
 * A machine line is `#TAG` and then space-separated `key=value`. A value
 * never holds a space unless it is quoted, and inside the quotes `\"`, `\\`
 * and `\xHH` stand for a quote, a backslash and any other byte, so an SSID
 * of 32 arbitrary bytes still fits on one line. Some tags carry free text
 * after the tag instead (`#GNSSU <sentence>`). Every command ends with
 * `#DONE <verb> rc=<n>`, rc 0 done, 1 failed, 2 not understood. The verb is
 * one word, and it is the firmware's to choose: the C5's `ble scan 5` ends with
 * `#DONE ble`, the nRF's `rf ping 10` with `#DONE rf_ping`. So the caller
 * says which verb ends its command, and the link never guesses it from the
 * line. A line that does not start with `#` is for humans and is never
 * parsed.
 *
 * Some lines answer no command. The boot banner (#C5INFO, #NRFINFO) arrives
 * on its own, and with the C5's GNSS relay on, #GNSSU and #PPS arrive at any
 * moment, between a command's lines included. So lines are routed by tag,
 * never by which command happens to be running: a tag named in
 * fck_drv_link_config_t.async always goes to on_async, whatever is in flight.
 *
 * The banner is also `info`'s answer, so it is named apart
 * (fck_drv_link_config_t.banner): every one goes to on_banner, whatever else it is
 * routed to. One that arrives in the middle of any other command, or while
 * the link waits for the #DONE of an abandoned one, says the chip restarted:
 * that command will never end, so the link ends it there
 * (fck_drv_link_restarted()) and hands the banner to on_async.
 *
 * This file reads; it does not write, wait or own a UART. The caller feeds
 * it the bytes its UART received and writes the command line itself, and
 * decides how long to wait. No ESP-IDF and no FreeRTOS on purpose: it is
 * the one piece the C5's and the nRF's clients share, and tests/host runs it
 * as it is. One link per chip, fed from one task.
 */
#ifndef FCK_DRV_LINK_H_
#define FCK_DRV_LINK_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "kode_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/** The longest line kept. The C5's is 320 bytes (an #AP with a fully
 *  escaped SSID); the nRF's, a #SPEC24 across all 101 MHz it listens to,
 *  545. A longer one is dropped whole and counted. */
#define FCK_DRV_LINK_LINE_MAX   640U
/** The longest tag and the longest verb, without '#' and the terminator. */
#define FCK_DRV_LINK_TAG_MAX    15U
/** How many tags can be routed as asynchronous. */
#define FCK_DRV_LINK_ASYNC_MAX  8U

/** @brief One machine line, valid only for the duration of the callback. */
typedef struct {
    const char *tag;    /**< Without the '#': "AP", "C5INFO", "GNSSU".     */
    const char *body;   /**< Everything after the tag and one space; "" if
                         *   the line is the tag alone.                    */
} fck_drv_link_message_t;

/** @brief A machine line handed to its reader. */
typedef void (*fck_drv_link_message_fn)(void *ctx, const fck_drv_link_message_t *msg);

/** @brief A human line handed to its reader, control bytes removed. */
typedef void (*fck_drv_link_text_fn)(void *ctx, const char *line);

/** @brief How a link routes what it reads. Copied by fck_drv_link_init(). */
typedef struct {
    /** Tags that are never a command's answer, NULL-terminated; may be NULL. */
    const char *const *async;
    /** Lines with one of those tags, and machine lines that arrive when no
     *  command is in flight (a boot banner). May be NULL. */
    fck_drv_link_message_fn    on_async;
    void              *async_ctx;
    /** Human lines. May be NULL. */
    fck_drv_link_text_fn   on_text;
    void              *text_ctx;
    /** The chip's banner tag ("C5INFO", "NRFINFO"), and the verb of the
     *  command it answers ("info"); both NULL for a chip without one. */
    const char        *banner;
    const char        *banner_verb;
    /** Every banner line, before anything else reads it. May be NULL. */
    fck_drv_link_message_fn    on_banner;
    void              *banner_ctx;
} fck_drv_link_config_t;

/** @brief Counters, for a log line or a test. They only grow. */
typedef struct {
    uint32_t lines;      /**< Lines completed, of every kind.               */
    uint32_t too_long;   /**< Lines longer than FCK_DRV_LINK_LINE_MAX, dropped.  */
    uint32_t stray_done; /**< #DONE lines that ended nothing.               */
    uint32_t stale;      /**< Lines of an abandoned command, dropped.        */
    uint32_t restarts;   /**< Banners that ended a command, or an abandoned
                          *   one: the chip restarted under it.             */
} fck_drv_link_statistics_t;

/** @brief A link. Its fields are private; it is a struct so it can be static. */
typedef struct {
    fck_drv_link_config_t   cfg;
    const char      *async[FCK_DRV_LINK_ASYNC_MAX + 1U];
    char             line[FCK_DRV_LINK_LINE_MAX + 1U];
    size_t           len;
    bool             overflow;
    uint8_t          esc;           /* where an ANSI escape is, if in one */
    char             banner[FCK_DRV_LINK_TAG_MAX + 1U];
    char             banner_verb[FCK_DRV_LINK_TAG_MAX + 1U];
    char             verb[FCK_DRV_LINK_TAG_MAX + 1U];
    char             stale[FCK_DRV_LINK_TAG_MAX + 1U];
    fck_drv_link_message_fn  on_reply;
    void            *reply_ctx;
    bool             pending;
    bool             done;
    bool             restarted;
    int32_t          rc;
    fck_drv_link_statistics_t stats;
} fck_drv_link_t;

/**
 * @brief Set a link up, empty and with no command in flight.
 * @param l   The link.
 * @param cfg How to route; NULL for no routing at all.
 * @return KODE_OK; KODE_DENIED when @p l is NULL, or more than
 *         FCK_DRV_LINK_ASYNC_MAX async tags are named, or one is longer than
 *         FCK_DRV_LINK_TAG_MAX, or a banner comes without its verb, or either
 *         is empty or longer than FCK_DRV_LINK_TAG_MAX.
 */
kode_status_t fck_drv_link_init(fck_drv_link_t *l, const fck_drv_link_config_t *cfg);

/**
 * @brief Forget everything: the half-read line, the command in flight, the
 *        stale verb. For after the chip behind the link was reset.
 */
void fck_drv_link_reset(fck_drv_link_t *l);

/**
 * @brief Say a command is going out, before writing it to the UART.
 *
 * From here until its #DONE, every machine line whose tag is not async goes
 * to @p on_reply.
 *
 * @param l        The link.
 * @param verb     The word the command's #DONE carries: "scan" for the C5's
 *                 `scan all`, "rf_ping" for the nRF's `rf ping 10`.
 * @param on_reply The command's lines; may be NULL.
 * @param ctx      Handed back to @p on_reply.
 * @return KODE_OK; KODE_DENIED when the link is not idle (a command in
 *         flight, or an abandoned one whose #DONE has not come: see
 *         fck_drv_link_abort()), or the verb is empty, holds a space, or is
 *         longer than FCK_DRV_LINK_TAG_MAX.
 */
kode_status_t fck_drv_link_begin(fck_drv_link_t *l, const char *verb,
                             fck_drv_link_message_fn on_reply, void *ctx);

/**
 * @brief Whether a command can begin: none in flight, none abandoned.
 */
bool fck_drv_link_idle(const fck_drv_link_t *l);

/**
 * @brief Read what the UART received: any number of bytes, any split.
 *
 * Carriage returns, ANSI escape sequences (the nRF's shell colours its
 * output) and other control bytes are removed. Callbacks run from here, in
 * the caller's context.
 */
void fck_drv_link_feed(fck_drv_link_t *l, const uint8_t *data, size_t n);

/**
 * @brief Whether the command in flight has ended.
 * @param l  The link.
 * @param rc Its rc when it has; -1 when the #DONE carried none, or when the
 *           chip restarted under it. May be NULL.
 * @return true once its #DONE was read, or a banner said the chip restarted
 *         (fck_drv_link_restarted()); false while it is in flight, or when no
 *         command was begun.
 */
bool fck_drv_link_done(const fck_drv_link_t *l, int32_t *rc);

/**
 * @brief Whether the command that ended did so because the chip restarted:
 *        its banner came, not its #DONE. What it did before is undone.
 */
bool fck_drv_link_restarted(const fck_drv_link_t *l);

/**
 * @brief Give up on the command in flight: its time ran out.
 *
 * The firmwares carry out one command at a time, so the firmware may still be in
 * the middle of it, and every line it still sends is that command's. Until
 * its #DONE arrives those lines are dropped (counted in stale), async tags
 * still reach on_async, and fck_drv_link_begin() refuses: a new command would
 * otherwise be handed the old one's lines - false counts in a ping, APs of
 * the wrong band in a scan. The caller waits for fck_drv_link_idle(), or resets
 * the chip and calls fck_drv_link_reset(). A banner, the chip restarting, also
 * ends the wait.
 *
 * The banner's own verb (fck_drv_link_config_t.banner_verb) is forgotten instead,
 * as fck_drv_link_forget(): its late answer is only a banner, and one sent
 * while the chip was starting would never get its #DONE.
 */
void fck_drv_link_abort(fck_drv_link_t *l);

/**
 * @brief Whether two banners' bodies say the same image: word for word,
 *        but any word that starts with "uptime", the one thing a banner
 *        says that changes while the same image runs.
 */
bool fck_drv_link_same_banner(const char *a, const char *b);

/**
 * @brief Forget the command in flight, or the abandoned one, as if it had
 *        never been begun: for one the chip never heard, written while it
 *        was starting. Its late lines, if any come, are read as unsolicited
 *        and its #DONE as a stray. The half-read line is kept.
 */
void fck_drv_link_forget(fck_drv_link_t *l);

/** @brief The counters. */
fck_drv_link_statistics_t fck_drv_link_statistics(const fck_drv_link_t *l);

/**
 * @brief A value by its key, unquoted and unescaped.
 *
 * Keys a caller does not ask for are never looked at, so a firmware that adds
 * a key does not break an older SDK.
 *
 * @param m   The line.
 * @param key The key, without '='.
 * @param out Where the value goes, NUL-terminated. It may hold NUL bytes
 *            (`\x00` in an SSID): use @p len, not strlen().
 * @param cap The size of @p out, the terminator included.
 * @param len The value's length in bytes; may be NULL.
 * @return true when the key is there and its value fits; false otherwise,
 *         with @p out empty.
 */
bool fck_drv_link_message_text(const fck_drv_link_message_t *m, const char *key, char *out, size_t cap,
                 size_t *len);

/**
 * @brief A whole number by its key: decimal, or hexadecimal after `0x`.
 * @return true when the key is there and all of its value is a number that
 *         fits; false otherwise, with @p out untouched.
 */
bool fck_drv_link_message_integer(const fck_drv_link_message_t *m, const char *key, int32_t *out);

/**
 * @brief fck_drv_link_message_integer() for what is never negative and may pass 2^31: a
 *        frequency in hertz (2.48 GHz), a time in milliseconds.
 * @return true when the key is there and all of its value is a number from
 *         0 to 2^32 - 1; false otherwise, with @p out untouched.
 */
bool fck_drv_link_message_unsigned(const fck_drv_link_message_t *m, const char *key, uint32_t *out);

/**
 * @brief A MAC address by its key, `aa:bb:cc:dd:ee:ff`.
 * @return true when the key is there and holds six hex bytes.
 */
bool fck_drv_link_message_mac(const fck_drv_link_message_t *m, const char *key, uint8_t out[6]);

/**
 * @brief Whether the line carries @p key at all, whatever its value: a
 *        firmware that prints `caps=` is told from one that does not.
 */
bool fck_drv_link_message_has(const fck_drv_link_message_t *m, const char *key);

/**
 * @brief Whether @p key's value, a list of words with commas between them
 *        (`caps=scan,chan,ble`), holds @p word, to the letter: `echo` is
 *        not in `echoidle`. The value is read where it is, so a list of any
 *        length is looked through whole.
 * @return false when the key is not there, @p word is empty or NULL, or the
 *         list does not hold it.
 */
bool fck_drv_link_message_listed(const fck_drv_link_message_t *m, const char *key, const char *word);

/**
 * @brief The n-th word of the body that is not `key=value`, from 0.
 *
 * `#DONE ble rc=0` has one, `ble`.
 *
 * @return true when there is one and it fits in @p out.
 */
bool fck_drv_link_message_word(const fck_drv_link_message_t *m, unsigned n, char *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* FCK_DRV_LINK_H_ */
