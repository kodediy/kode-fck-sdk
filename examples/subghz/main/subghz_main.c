/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file subghz_main.c
 * @brief The CC1101 from a task, the standard layer (fck.h): the level at 433.92 MHz, the band
 *        swept, a remote found and its code decoded - or, from the console,
 *        a listen read by read, packets received, and the remote kept sent
 *        back.
 *
 * Wire: the F*CK module on J3, and the Dot on a USB-C cable. A 433 MHz remote
 * (a gate's, a doorbell's) to press during a round, if you have one.
 *
 * Try it: once the module is found, each round logs the level on 433.92 MHz
 * with the narrow filter (fck_subghz_level()), the peak of the 433 band
 * (fck_subghz_spectrum()), then listens three seconds for a remote
 * (fck_subghz_find()). One found, two seconds of edges are captured where it
 * sent (fck_subghz_capture()) and read as a fixed code - EV1527, PT2262,
 * HT12E (fck_subghz_decode()) - or,
 * listening, as a rolling one, KeeLoq or Somfy, which is read and never sent
 * (fck_subghz_decode_rolling()): the log says the protocol, the bits and the pulse,
 * not the key - that is the remote's. Pull the module out and the rounds
 * stop; put it back and they start again. Holding the bottom button goes home
 * to kodeOS, as in every app (CONFIG_KODEOS_HOME_HOLD).
 *
 * On the console (the USB-C cable), three functions; the name in quotes, the
 * argument bare:
 *
 *     function name="fck_sg_listen" arg=hop    rounds become listens
 *     function name="fck_sg_listen" arg=off    back to the rounds above
 *     function name="fck_sg_packets" arg=868   GFSK packets, 10 s
 *     function name="fck_sg_send" arg=clone    the remote kept, sent back
 *     function name="fck_sg_send" arg=packet   one GFSK packet, 433.92 MHz
 *
 *     fck_sg_listen   hop, 315, 433 or 868: each round is a listen of 2 s
 *                     (fck_subghz_listen_read()) - hopping, it stays where a
 *                     code is read, until 5 s after the last; what is heard
 *                     is decoded and written
 *                     as a Kode .ksub file, whose size the log says. off
 *                     stops it (fck_subghz_listen_stop()).
 *     fck_sg_packets  433 or 868: packets received for 10 s with fixed
 *                     settings (38.4 kbaud GFSK, sync 0xD391, 8 bytes, CRC):
 *                     how many, how many with a good CRC, and the strongest.
 *     fck_sg_send     clone or emulate: the remote kept (kept_t: the one
 *                     pressed by the Kode, not the band's background), on
 *                     the frequency it was heard - its runs as captured, up
 *                     to 5 times within 3 s of carrier (fck_subghz_send_raw()),
 *                     or the code read off it as 5 frames
 *                     (fck_subghz_send_code()). Refused with nothing heard
 *                     yet, with no code read for emulate, outside
 *                     433.05-434.79 and 868.0-868.6 MHz (45 kHz clear of
 *                     the edges), for a rolling code, and past the hour's
 *                     air time (fck_subghz_air_time_left()). packet
 *                     sends nothing that was heard: one 8-byte packet, 3
 *                     times, at 433.92 MHz with the same settings
 *                     (fck_subghz_packet_send()) - the ones fck_sg_packets
 *                     receives with, so a second Kode Dot
 *                     listening there hears it. Any send is refused over the
 *                     band's air time for the hour (fck_subghz_air_time_left()).
 *
 * A function answers `>ok` when its request is taken; one typed while
 * another waits is refused, on the log.
 *
 * ONLY fck_sg_send TRANSMITS, and only a remote you own: the rounds and the
 * listens only receive. Each band goes through its own antenna leg
 * (fck_bsp.h).
 *
 * >>> fck_sg_send REFUSES UNTIL fck_arm HAS BEEN GIVEN 1, FOR SUBGHZ_ARM_MS
 *     (60 s). ARM ONLY WITH A REMOTE OF YOUR OWN, AND AN ANTENNA OR A 50 OHM
 *     LOAD ON. fck_sg_stop DISARMS AT ONCE AND CUTS WHAT IS ON THE AIR
 *     (fck_cancel()); SO DOES THE MODULE LEAVING J3 (the event module_lost), A
 *     fck_arm 0 AND THE TIME RUNNING OUT. A send typed before and taken
 *     after any of them is dropped. fck_sg_armed says whether it is armed. <<<
 *
 *     function name="fck_arm" arg=1            allow the sends, for 60 s
 *     function name="fck_sg_stop"              disarm, cut what sends
 *     variable name="fck_sg_armed"             1 while armed
 *
 * ---- A task, and the console's rows -------------------------------------------
 *
 * app_main() is a task, so every call of fck.h is a plain call that waits and answers a bool, with
 * fck_error() saying why: the loop below is the whole program, a round after a round. A short press
 * of the bottom button ends the SDK and returns to kodeOS. The console's rows run on the console's
 * task, which must not wait behind a round: they hand a request to this one through a queue of one
 * and answer at once. This task serves it between two rounds. fck_sg_stop is the exception: it
 * disarms and calls fck_cancel() on the spot, which is safe from any task and cuts what is on the
 * air within a few milliseconds. The module leaving is told by the event module_lost, on a task of
 * the SDK's, which takes the arming back (a listener may call fck.h, but only this does).
 *
 * The Active Object side of the same module is fck_sys.h, in the other examples.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "kode.h"
#include "kode_log.h"
#include "kode_variables.h"   /* the console tables are the extended layer's headers; */
#include "kode_functions.h"   /* a task may include them for its rows. */
#include "fck.h"
#include "subghz_arm.h"

#define TAG "subghz"

/** The elements of an array. */
#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

/* What the launcher shows, and the category it files this app under. */
KODE_APP_MANIFEST("F*CK subghz", "example");

/** One wait for the module; refused, it is asked again. */
#define SUBGHZ_WAIT_MS        5000U

/** The remotes' frequency, and the band around it: 433.05 to 434.79 MHz. */
#define SUBGHZ_REMOTE_HZ      433920000U
#define SUBGHZ_FIRST_HZ       433050000U
#define SUBGHZ_STEP_HZ        50000U
#define SUBGHZ_POINTS         36U

/** How long to listen for a remote, and how long to capture it. */
#define SUBGHZ_FIND_MS        3000U
#define SUBGHZ_CAPTURE_MS     2000U

/** The pause between rounds. */
#define SUBGHZ_PAUSE_MS       5000U

/** A listen's round, its pause, and how long packets are received for. */
#define SUBGHZ_READ_MS        2000U
#define SUBGHZ_READ_PAUSE_MS  100U
#define SUBGHZ_PACKETS_MS     10000U

/** The edges a capture keeps, and the .ksub file they always fit in. */
#define SUBGHZ_EDGES          512U
#define SUBGHZ_FILE_BYTES     FCK_SUBGHZ_FILE_RAW_MAX(SUBGHZ_EDGES)
#define SUBGHZ_SEND_REPEATS   5U    /* times a raw clone is re-emitted, at most */
#define SUBGHZ_SEND_FRAMES    5U    /* frames an emulated code is sent as */
#define SUBGHZ_SEND_PACKETS   3U    /* times the packet is sent */
#define SUBGHZ_PACKET_HZ      433920000U
#define SUBGHZ_KEEP_DB        10    /* dB quieter than the one kept: background, not kept */
#define SUBGHZ_KEEP_MS        60000U /* after this, any capture replaces the one kept:
                                     * time to press a remote and type fck_sg_send twice */

/** @brief What packets are sent and received with: 38.4 kbaud GFSK,
 *         sync 0xD391, 8 bytes, CRC and whitening. */
static const fck_subghz_packet_settings_t FCKIT = {
    .modulation = FCK_SUBGHZ_GFSK, .baud = 38400U, .deviation_hz = 20630U, .filter_hz = 101000U,
    .sync_word = 0xD391U, .length = 8U, .crc = true, .whitening = true,
};

/** @brief The packet fck_sg_send arg=packet sends: "FCKit001". */
static const uint8_t PACKET_BYTES[8] = { 0x46U, 0x43U, 0x4BU, 0x69U, 0x74U, 0x30U, 0x30U, 0x31U };

/** @brief What the console asked for. */
typedef enum {
    REQUEST_LISTEN,     /**< fck_subghz_listen_start() or _stop().   */
    REQUEST_PACKETS,    /**< fck_subghz_packet_receive().            */
    REQUEST_SEND        /**< fck_subghz_send_raw() or _send_code().  */
} request_kind_t;

/** @brief A request, from the console's task to this one. */
typedef struct {
    request_kind_t kind;
    int32_t        arg;    /**< The enum's index.                         */
    uint32_t       cuts;   /**< subghz_arm_cuts() when its row posted it. */
} request_t;

/** @brief @p proto's name, for the log; "?" for one the SDK does not have. */
static char const *proto_name(fck_subghz_protocol_t const proto)
{
    fck_subghz_protocol_info_t info;
    return (fck_subghz_protocol_info(proto, &info)) ? info.name : "?";
}

/** @brief The remote kept for fck_sg_send: every round starts its capture
 *         afresh, so a send reads this, not the capture. A capture replaces
 *         it, code and runs together, when nothing is kept, when the one
 *         kept is SUBGHZ_KEEP_MS old, or when it is no more than
 *         SUBGHZ_KEEP_DB quieter than the loudest kept since and does not
 *         lose a code: a capture with no code never replaces one with a code
 *         but when it is louder. The reference is the loudest, not the last,
 *         so it does not step down 10 dB at a time. So the
 *         remote pressed by the Kode stays kept over the burst's tail and
 *         over whatever else sends on the band further off, and can be
 *         sent again and again. */
typedef struct {
    uint32_t          hz;           /**< Where the runs were heard; 0 for none yet. */
    int32_t           runs[SUBGHZ_EDGES]; /**< Its capture: signed us.       */
    size_t            run_count;
    fck_subghz_code_t code;         /**< The code read from it ...           */
    bool              has_code;     /**< ... when its capture had one.       */
    int16_t           dbm;          /**< The loudest kept since it was empty or old. */
    TickType_t        at;           /**< When it was kept.                   */
} kept_t;

/** @brief This example's own state, private to this file. */
typedef struct {
    int16_t           level_dbm;    /**< At SUBGHZ_REMOTE_HZ.               */
    int16_t           band[SUBGHZ_POINTS]; /**< The band, one point a step. */
    size_t            points;
    uint32_t          peak_hz;      /**< Where the band peaked.             */
    int16_t           peak_dbm;
    fck_subghz_hit_t  hit;          /**< A remote, if one sent.             */
    int32_t           edges[SUBGHZ_EDGES]; /**< Its capture: signed us.     */
    size_t            edge_count;
    fck_subghz_code_t codes[4];     /**< What the capture decoded to.       */
    size_t            code_count;
    bool              listening;    /**< Rounds are listens (fck_sg_listen). */
    uint32_t          listen_hz;    /**< FCK_SUBGHZ_HOP or a frequency.     */
    uint32_t          listen_after; /**< The sequence of the last capture read. */
    fck_subghz_heard_t heard;       /**< What a listen's round heard.       */
    char              file[SUBGHZ_FILE_BYTES]; /**< It, as a .ksub file.    */
    size_t            file_length;
    request_kind_t    kind;         /**< The request being served ...       */
    int32_t           arg;          /**< ... with this argument.            */
    uint32_t          cuts;         /**< ... posted at this arming's cuts.  */
    fck_subghz_packet_t packets[8]; /**< Filled by REQUEST_PACKETS.         */
    size_t            packet_count;
    kept_t            kept;         /**< What REQUEST_SEND sends.           */
    uint8_t           repeats;      /**< Times a clone was sent.            */
    bool              rolling;      /**< A listen read a rolling code ...   */
    fck_subghz_rolling_protocol_t rolling_proto; /**< ... of this one.      */
} Subghz;

static Subghz l_state;

/** @brief Whether the rows that transmit are allowed; see subghz_arm.h. */
static subghz_arm_t l_arm;

/** @brief The console's request, one at a time: a second is refused while one waits. */
static QueueHandle_t l_requests;

/**
 * @brief Keep the capture a round just decoded, heard on @p hz at @p dbm,
 *        for fck_sg_send - unless kept_t's rule keeps the one before.
 * @param me  The state.
 * @param hz  Where it was heard.
 * @param dbm How loud.
 */
static void keep(Subghz * const me, uint32_t const hz, int16_t const dbm)
{
    kept_t * const   k     = &me->kept;
    TickType_t const now   = xTaskGetTickCount();
    bool const       code  = (me->code_count > 0U);
    bool const       old   = ((now - k->at) >= pdMS_TO_TICKS(SUBGHZ_KEEP_MS));
    bool const       near  = (((int32_t)dbm + SUBGHZ_KEEP_DB) >= (int32_t)k->dbm);
    bool const       loses = (k->has_code && !code && (dbm <= k->dbm));
    bool const       fresh = ((0U == k->run_count) || old);
    if ( (0U == me->edge_count) || (!fresh && (!near || loses)) )
    {
        return;
    }
    k->hz        = hz;
    k->dbm       = (fresh || (dbm > k->dbm)) ? dbm : k->dbm;
    k->at        = now;
    k->run_count = me->edge_count;
    (void)memcpy(k->runs, me->edges, me->edge_count * sizeof(me->edges[0]));
    k->has_code  = code;
    if ( code )
    {
        k->code = me->codes[0];
    }
}

/**
 * @brief The log line for a call of fck.h that answered false: its sentence, from fck_error().
 * @param what The call.
 */
static void refused(char const * const what)
{
    KLOG_W(TAG, "%s: %s", what, fck_error());
}

/**
 * @brief One round: the level, the band, a remote found, captured, decoded.
 * @param me The state.
 * @return true when every call answered; false at the first that did not (logged).
 */
static bool round_once(Subghz * const me)
{
    me->points     = 0U;
    me->edge_count = 0U;
    me->code_count = 0U;
    me->hit.found  = false;

    if ( !fck_subghz_level(SUBGHZ_REMOTE_HZ, FCK_SUBGHZ_NARROW, &me->level_dbm) )
    {
        refused("level");
        return false;
    }
    if ( !fck_subghz_spectrum(SUBGHZ_FIRST_HZ, SUBGHZ_STEP_HZ, SUBGHZ_POINTS, FCK_SUBGHZ_WIDE,
                              me->band, ARRAY_LEN(me->band), &me->points) )
    {
        refused("spectrum");
        return false;
    }
    me->peak_hz  = SUBGHZ_FIRST_HZ;
    me->peak_dbm = FCK_SUBGHZ_NO_DBM;
    for ( size_t i = 0U; i < me->points; i++ )
    {
        if ( me->band[i] > me->peak_dbm )
        {
            me->peak_dbm = me->band[i];
            me->peak_hz  = SUBGHZ_FIRST_HZ + ((uint32_t)i * SUBGHZ_STEP_HZ);
        }
    }

    if ( !fck_subghz_find(SUBGHZ_FIND_MS, &me->hit) )
    {
        refused("find");
        return false;
    }
    if ( me->hit.found )
    {
        if ( !fck_subghz_capture(me->hit.hz, SUBGHZ_CAPTURE_MS, me->edges, ARRAY_LEN(me->edges),
                                 &me->edge_count) )
        {
            refused("capture");
            return false;
        }
        if ( !fck_subghz_decode(me->edges, me->edge_count, me->codes, ARRAY_LEN(me->codes),
                                &me->code_count) )
        {
            refused("decode");
            return false;
        }
        keep(me, me->hit.hz, me->hit.dbm);
    }
    return true;
}

/**
 * @brief A listen's round: one read of what is newer than the last, and what it heard decoded and
 *        written as a .ksub file - a code file for a code, a RAW one otherwise.
 * @param me The state.
 * @return true when every call answered; false at the first that did not (logged).
 */
static bool listen_once(Subghz * const me)
{
    me->edge_count  = 0U;
    me->code_count  = 0U;
    me->file_length = 0U;
    if ( !fck_subghz_listen_read(me->listen_after, SUBGHZ_READ_MS, me->edges, ARRAY_LEN(me->edges),
                                 &me->edge_count, &me->heard) )
    {
        refused("listen read");
        return false;
    }
    if ( !me->heard.heard )
    {
        return true;
    }
    me->listen_after = me->heard.sequence;
    if ( !fck_subghz_decode(me->edges, me->edge_count, me->codes, ARRAY_LEN(me->codes), &me->code_count) )
    {
        refused("decode");
        return false;
    }
    fck_subghz_rolling_t rolling;
    size_t               rolling_count = 0U;
    me->rolling = (0U == me->code_count)
                  && fck_subghz_decode_rolling(me->edges, me->edge_count, &rolling, 1U, &rolling_count)
                  && (rolling_count > 0U);
    me->rolling_proto = me->rolling ? rolling.proto : FCK_SUBGHZ_KEELOQ;
    keep(me, me->heard.hz, me->heard.dbm);
    bool const written = (me->code_count > 0U)
        ? fck_subghz_code_to_file(me->heard.hz, &me->codes[0], me->file, sizeof(me->file),
                                          &me->file_length)
        : fck_subghz_to_file(me->heard.hz, me->edges, me->edge_count, me->file,
                                sizeof(me->file), &me->file_length);
    if ( !written )
    {
        refused("ksub file");
    }
    return written;
}


/**
 * @brief How many times the remote kept fits in fck_subghz_send_raw()'s
 *        FCK_SUBGHZ_SEND_MS_MAX of carrier, SUBGHZ_SEND_REPEATS at most: a
 *        remote held down for 2 s is sent once or twice rather than refused.
 * @return 0 when once is already too long.
 */
static uint8_t clone_repeats(kept_t const * const k)
{
    uint64_t on_us = 0U;
    for ( size_t i = 0U; i < k->run_count; i++ )
    {
        if ( k->runs[i] > 0 )
        {
            on_us += (uint64_t)k->runs[i];
        }
    }
    uint64_t const cap_us = (uint64_t)FCK_SUBGHZ_SEND_MS_MAX * 1000U;
    uint64_t const fit    = (0U == on_us) ? SUBGHZ_SEND_REPEATS : (cap_us / on_us);
    return (fit < SUBGHZ_SEND_REPEATS) ? (uint8_t)fit : (uint8_t)SUBGHZ_SEND_REPEATS;
}

/**
 * @brief Serve the console's request: a listen started or stopped, packets received, or a send.
 *        The only calls here that transmit are the sends, and they go only if the arming still holds.
 * @param me The state.
 * @return true when it was done; false when refused (the sentence is logged by show_request()).
 */
static bool serve(Subghz * const me)
{
    /* The enums' order: fck_sg_listen off, hop, 315, 433, 868; fck_sg_packets 433, 868. */
    static const uint32_t LISTEN_HZ[]  = { 0U, FCK_SUBGHZ_HOP, 315000000U, 433920000U, 868350000U };
    static const uint32_t PACKETS_HZ[] = { 433920000U, 868350000U };
    bool ok = false;

    if ( REQUEST_LISTEN == me->kind )
    {
        if ( 0 == me->arg )
        {
            ok            = fck_subghz_listen_stop();
            me->listening = false;
        }
        else if ( (me->arg > 0) && ((size_t)me->arg < ARRAY_LEN(LISTEN_HZ)) )
        {
            me->listen_hz    = LISTEN_HZ[me->arg];
            me->listen_after = 0U;
            ok               = fck_subghz_listen_start(me->listen_hz);
            me->listening    = ok;
        }
    }
    else if ( (REQUEST_PACKETS == me->kind) && (me->arg >= 0) && ((size_t)me->arg < ARRAY_LEN(PACKETS_HZ)) )
    {
        me->packet_count = 0U;
        ok = fck_subghz_packet_receive(PACKETS_HZ[me->arg], &FCKIT, SUBGHZ_PACKETS_MS, me->packets,
                                       ARRAY_LEN(me->packets), &me->packet_count);
    }
    else if ( (REQUEST_SEND == me->kind) && !subghz_arm_holds(&l_arm, me->cuts, kode_uptime_ms()) )
    {
        /* Disarmed, run out or stopped between the row and here: not sent. */
        KLOG_W(TAG, "transmitting was disarmed, or ran out, before this send could go: dropped");
    }
    else if ( REQUEST_SEND == me->kind )
    {
        /* On the frequency the remote kept was heard, its runs (clone) or the code read off it
         * (emulate). Nothing heard yet, or outside a transmit band, send refuses; a packet is
         * the example's own. */
        kept_t const * const k = &me->kept;
        if ( 2 == me->arg )
        {
            ok = fck_subghz_packet_send(SUBGHZ_PACKET_HZ, &FCKIT, PACKET_BYTES, (uint8_t)ARRAY_LEN(PACKET_BYTES),
                                        SUBGHZ_SEND_PACKETS);
        }
        else if ( 0U == k->run_count )
        {
            KLOG_W(TAG, "nothing heard yet to send");
        }
        else if ( 0 == me->arg )
        {
            me->repeats = clone_repeats(k);
            ok = (me->repeats > 0U) && fck_subghz_send_raw(k->hz, k->runs, k->run_count, me->repeats);
        }
        else
        {
            ok = k->has_code && fck_subghz_send_code(k->hz, &k->code, SUBGHZ_SEND_FRAMES);
        }
    }
    else
    {
        /* Not a request this example knows. */
    }
    return ok;
}

/* ---- The console ------------------------------------------------------------ */

/**
 * @brief Hand a request to the task, from the console's.
 * @return KODE_OK once queued; KODE_NO_RESOURCES when one already waits.
 */
static kode_status_t request(request_kind_t const kind, int32_t const arg)
{
    request_t const r = { .kind = kind, .arg = arg, .cuts = subghz_arm_cuts(&l_arm) };
    return (pdTRUE == xQueueSend(l_requests, &r, 0U)) ? KODE_OK : KODE_NO_RESOURCES;
}

static kode_status_t do_listen(const kode_value_t *arg)
{
    return (NULL == arg) ? KODE_DENIED : request(REQUEST_LISTEN, (int32_t)arg->e);
}

static kode_status_t do_packets(const kode_value_t *arg)
{
    return (NULL == arg) ? KODE_DENIED : request(REQUEST_PACKETS, (int32_t)arg->e);
}

static kode_status_t do_send(const kode_value_t *arg)
{
    if ( NULL == arg )
    {
        return KODE_DENIED;
    }
    if ( !subghz_arm_ok(&l_arm, kode_uptime_ms()) )
    {
        KLOG_W(TAG, "fck_sg_send refused: fck_arm arg=1 first, with a remote of your own and an antenna on");
        return KODE_DENIED;
    }
    return request(REQUEST_SEND, (int32_t)arg->e);
}

static kode_status_t do_arm(const kode_value_t *arg)
{
    if ( NULL == arg )
    {
        return KODE_DENIED;
    }
    subghz_arm_set(&l_arm, 0 != arg->i, kode_uptime_ms());
    if ( 0 != arg->i )
    {
        KLOG_W(TAG, "transmitting is ARMED for %u s; fck_sg_stop disarms", (unsigned)(SUBGHZ_ARM_MS / 1000U));
    }
    else
    {
        KLOG_W(TAG, "transmitting is disarmed");
    }
    return KODE_OK;
}

/* The one row that does not queue: fck_cancel() returns at once, and a stop must not wait
 * behind a round. It disarms first, so that a send queued and not yet taken is
 * dropped, then cuts what waits or sends. */
/* The one row that does not queue: fck_cancel() returns at once, and a stop must not wait
 * behind a round. It disarms first, so that a send queued and not yet taken is dropped, then cuts
 * what waits or sends. */
static kode_status_t do_stop(const kode_value_t *arg)
{
    (void)arg;
    subghz_arm_cut(&l_arm);
    KLOG_W(TAG, "stop: disarmed, and what waits or sends is cut");
    return fck_cancel() ? KODE_OK : KODE_DENIED;
}

static const char * const LISTENS[] = { "off", "hop", "315", "433", "868" };
static const char * const BANDS[]   = { "433", "868" };
static const char * const SENDS[]   = { "clone", "emulate", "packet" };

static const kode_function_t FUNCTIONS[] = {
    { .name = "fck_arm", .arg = KODE_TYPE_INT, .min = 0, .max = 1, .run = do_arm,
      .desc = "1 allows fck_sg_send for 60 s; 0 refuses" },
    { .name = "fck_sg_listen", .arg = KODE_TYPE_ENUM, .enums = LISTENS, .n_enums = 5U, .run = do_listen,
      .desc = "Rounds become listens: off hop 315 433 868" },
    { .name = "fck_sg_packets", .arg = KODE_TYPE_ENUM, .enums = BANDS, .n_enums = 2U, .run = do_packets,
      .desc = "GFSK packets for 10 s: 433 or 868" },
    { .name = "fck_sg_send", .arg = KODE_TYPE_ENUM, .enums = SENDS, .n_enums = 3U, .run = do_send,
      .desc = "TRANSMITS the remote kept, or a packet; armed" },
    { .name = "fck_sg_stop", .run = do_stop,
      .desc = "Disarm and cut what sends, now" },
};

static kode_status_t get_armed(char *buf, size_t cap)
{
    (void)snprintf(buf, cap, "%u", subghz_arm_ok(&l_arm, kode_uptime_ms()) ? 1U : 0U);
    return KODE_OK;
}

static const kode_variable_t VARIABLES[] = {
    { .name = "fck_sg_armed", .get = get_armed, .desc = "1 while fck_arm allows fck_sg_send" },
};

/* ---- What a round found, on the log ---------------------------------------- */

/**
 * @brief The level, the band's peak, and the remote if one sent.
 * @param me The AO.
 */
static void show_round(Subghz const * const me)
{
    KLOG_I(TAG, "433.92 MHz: %d dBm; the band peaks at %lu.%03lu MHz, %d dBm",
           (int)me->level_dbm, (unsigned long)(me->peak_hz / 1000000U),
           (unsigned long)((me->peak_hz / 1000U) % 1000U), (int)me->peak_dbm);
    if ( !me->hit.found )
    {
        KLOG_I(TAG, "no remote sent in %u s", (unsigned)(SUBGHZ_FIND_MS / 1000U));
        return;
    }
    KLOG_I(TAG, "a remote at %lu.%03lu MHz, %d dBm over %d of noise: %u edges captured",
           (unsigned long)(me->hit.hz / 1000000U), (unsigned long)((me->hit.hz / 1000U) % 1000U),
           (int)me->hit.dbm, (int)me->hit.noise_dbm, (unsigned)me->edge_count);
    for ( size_t i = 0U; i < me->code_count; i++ )
    {
        fck_subghz_code_t const * const c = &me->codes[i];
        KLOG_I(TAG, "code %u: %s, %u bits, pulse %u us, %u frames (the key is the remote's)",
               (unsigned)(i + 1U), proto_name(c->proto),
               (unsigned)c->bits, (unsigned)c->te_us, (unsigned)c->repeats);
    }
    if ( 0U == me->code_count )
    {
        KLOG_I(TAG, "no fixed code in the capture");
    }
}

/**
 * @brief What a listen's round heard: where, how strongly, its code's shape
 *        (not the key - that is the remote's) and its .ksub file's size.
 * @param me The AO.
 */
static void show_listen(Subghz const * const me)
{
    if ( !me->heard.heard )
    {
        return;
    }
    KLOG_I(TAG, "heard at %lu.%03lu MHz, %d dBm over %d of noise: %u runs",
           (unsigned long)(me->heard.hz / 1000000U), (unsigned long)((me->heard.hz / 1000U) % 1000U),
           (int)me->heard.dbm, (int)me->heard.noise_dbm, (unsigned)me->edge_count);
    if ( me->code_count > 0U )
    {
        KLOG_I(TAG, "%s, %u bits, pulse %u us: a .ksub code file of %u bytes",
               proto_name(me->codes[0].proto),
               (unsigned)me->codes[0].bits, (unsigned)me->codes[0].te_us, (unsigned)me->file_length);
    }
    else if ( me->rolling )
    {
        KLOG_I(TAG, "a rolling code (%s): read, never sent - fck_sg_send clone refuses it",
               (FCK_SUBGHZ_SOMFY_RTS == me->rolling_proto) ? "Somfy" : "KeeLoq");
    }
    else
    {
        KLOG_I(TAG, "no code read: a .ksub RAW file of %u bytes", (unsigned)me->file_length);
    }
}

/**
 * @brief What a console request came to.
 * @param me The AO.
 * @param rc What the call answered.
 */
static void show_request(Subghz const * const me, kode_status_t const rc)
{
    char const * const name = (REQUEST_LISTEN == me->kind)    ? "fck_sg_listen"
                              : (REQUEST_PACKETS == me->kind) ? "fck_sg_packets"
                                                              : "fck_sg_send";

    if ( KODE_OK != rc )
    {
        KLOG_W(TAG, "%s: refused (%s)", name, fck_error());
        return;
    }
    if ( REQUEST_LISTEN == me->kind )
    {
        KLOG_I(TAG, "%s: %s", name, me->listening ? "each round listens" : "back to the rounds");
        return;
    }
    if ( (REQUEST_SEND == me->kind) && (2 == me->arg) )
    {
        KLOG_I(TAG, "%s: %u GFSK packet(s) sent at %lu.%03lu MHz", name, (unsigned)SUBGHZ_SEND_PACKETS,
               (unsigned long)(SUBGHZ_PACKET_HZ / 1000000U), (unsigned long)((SUBGHZ_PACKET_HZ / 1000U) % 1000U));
        return;
    }
    if ( REQUEST_SEND == me->kind )
    {
        bool const     clone = (0 == me->arg);
        uint32_t const hz    = me->kept.hz;
        KLOG_I(TAG, "%s: %s %u %s sent at %lu.%03lu MHz", name, clone ? "the capture," : "the code, as",
               clone ? (unsigned)me->repeats : (unsigned)SUBGHZ_SEND_FRAMES,
               clone ? "times," : "frames,", (unsigned long)(hz / 1000000U),
               (unsigned long)((hz / 1000U) % 1000U));
        return;
    }
    size_t  good     = 0U;
    int16_t strongest = FCK_SUBGHZ_NO_DBM;
    for ( size_t i = 0U; i < me->packet_count; i++ )
    {
        good += me->packets[i].crc_ok ? 1U : 0U;
        if ( me->packets[i].dbm > strongest )
        {
            strongest = me->packets[i].dbm;
        }
    }
    KLOG_I(TAG, "%s: %u packet(s) in %u s, %u with a good CRC, the strongest %d dBm", name,
           (unsigned)me->packet_count, (unsigned)(SUBGHZ_PACKETS_MS / 1000U), (unsigned)good,
           (int)strongest);
}

/**
 * @brief What the SDK tells, on a task of its own: the module leaving takes the arming back. Short,
 *        and it calls nothing that waits.
 * @param e The event.
 */
static void on_event(const fck_event_t *e)
{
    (void)e;
    subghz_arm_cut(&l_arm);
}

/**
 * @brief Wait up to @p ms, in slices, for the end: a request queued or the button pressed.
 * @param ms   How long.
 * @param quit Set when the bottom button was pressed.
 */
static void pause_for(uint32_t const ms, bool * const quit)
{
    for ( uint32_t t = 0U; (t < ms) && !*quit && (0U == uxQueueMessagesWaiting(l_requests)); t += 100U )
    {
        kode_delay(100U);
        *quit = kode_button_was_pressed("bottom");
    }
}

void app_main(void)
{
    Subghz * const me = &l_state;

    /* First, always: brings up logging, QF, the event pools and the core Active Objects. */
    kode_init();

    l_requests = xQueueCreate(1U, sizeof(request_t));
    if ( NULL == l_requests )
    {
        KLOG_E(TAG, "no queue for the console's requests; nothing starts");
        return;
    }

    /* A table with one bad row is refused whole, with a log line naming the
     * row; a name the app already uses answers KODE_EXISTS. */
    if ( KODE_FUNCTIONS_ADD(FUNCTIONS) != KODE_OK )
    {
        KLOG_E(TAG, "the console refused the functions");
    }
    if ( KODE_VARIABLES_ADD(VARIABLES) != KODE_OK )
    {
        KLOG_E(TAG, "the console refused the variables");
    }

    /* Before fck_begin(), so none is missed. */
    (void)fck_event_add_listener("module_lost", on_event);
    if ( !fck_begin() )
    {
        KLOG_E(TAG, "the SDK did not start: %s", fck_error());
        return;
    }
    KLOG_I(TAG, "up; looking for the F*CK on J3");

    /* The press count is shared and starts at boot: the first ask clears any press made before. */
    (void)kode_button_was_pressed("bottom");
    bool quit = false;

    while ( !quit )
    {
        if ( !fck_is_present() )
        {
            /* "Not yet" is not a failure; fck_error() says so. */
            if ( fck_wait_present(SUBGHZ_WAIT_MS) )
            {
                KLOG_I(TAG, "found; the CC1101 listens");
            }
            quit = kode_button_was_pressed("bottom");
            continue;
        }

        request_t r;
        if ( pdTRUE == xQueueReceive(l_requests, &r, 0U) )
        {
            me->kind = r.kind;
            me->arg  = r.arg;
            me->cuts = r.cuts;
            show_request(me, serve(me) ? KODE_OK : KODE_DENIED);
            continue;
        }

        bool const ok = me->listening ? listen_once(me) : round_once(me);
        if ( ok )
        {
            if ( me->listening )
            {
                show_listen(me);
            }
            else
            {
                show_round(me);
            }
        }
        pause_for(me->listening ? SUBGHZ_READ_PAUSE_MS : SUBGHZ_PAUSE_MS, &quit);
        quit = quit || kode_button_was_pressed("bottom");
    }

    subghz_arm_cut(&l_arm);
    (void)fck_end();
    KLOG_I(TAG, "SDK ended; leaving");
    kode_exit();
}
