/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file fck_sys.c
 * @brief The extended layer: fck_sys.h, for Active Objects.
 *
 * A request is carried out by a task of this SDK (an "executor") through the core, the same
 * calls fck.h makes for a task; what the core answers is posted to the asking Active Object. So
 * an Active Object never waits, and the owner has one way in for both layers.
 *
 *   - The door table (asker x kind) lives here, as kode-sdk's does: a request takes its door
 *     when it is accepted and gives it back just before its answer is posted.
 *   - FCK_SYS_EXECUTORS tasks take requests from one queue. A request that was queued before
 *     fck_sys_cancel() is answered as cancelled without running; one that runs is cut by the
 *     core's own cancel marks.
 *   - The answer is posted with no margin, as kode-sdk's are. Nothing here asserts: a pool
 *     that is dry or a queue that is full is waited for up to a second, and then the answer is
 *     lost, logged and counted (fck_status().answers_dropped), with the door open all the same.
 *   - The news are the owner's events, posted to every subscriber with a margin.
 *   - fck_sys_end() runs on a task of its own: the owner cannot be stopped from an Active
 *     Object, and the executors may be busy with a long listening that the end itself cuts.
 */
#include "fck_sys.h"

#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "kode_log.h"

#include "fck_aos.h"
#include "fck_core.h"

#define TAG "fck_sys"

/* The classes of failure are the owner's, number for number. */
_Static_assert((int)FCK_WHY_NONE == (int)FCK_AOS_WHY_NONE, "why numbers");
_Static_assert((int)FCK_WHY_NOT_BEGUN == (int)FCK_AOS_WHY_NOT_BEGUN, "why numbers");
_Static_assert((int)FCK_WHY_LEAVING == (int)FCK_AOS_WHY_LEAVING, "why numbers");
_Static_assert((int)FCK_WHY_NO_MODULE == (int)FCK_AOS_WHY_NO_MODULE, "why numbers");
_Static_assert((int)FCK_WHY_CHIP_HELD == (int)FCK_AOS_WHY_CHIP_HELD, "why numbers");
_Static_assert((int)FCK_WHY_CHIP_NOT_READY == (int)FCK_AOS_WHY_CHIP_NOT_READY, "why numbers");
_Static_assert((int)FCK_WHY_BAD_ARGUMENT == (int)FCK_AOS_WHY_BAD_ARGUMENT, "why numbers");
_Static_assert((int)FCK_WHY_BEYOND_LIMITS == (int)FCK_AOS_WHY_BEYOND_LIMITS, "why numbers");
_Static_assert((int)FCK_WHY_FIRMWARE_TOO_OLD == (int)FCK_AOS_WHY_FIRMWARE_TOO_OLD, "why numbers");
_Static_assert((int)FCK_WHY_WRONG_ORDER == (int)FCK_AOS_WHY_WRONG_ORDER, "why numbers");
_Static_assert((int)FCK_WHY_CANCELLED == (int)FCK_AOS_WHY_CANCELLED, "why numbers");
_Static_assert((int)FCK_WHY_BUSY == (int)FCK_AOS_WHY_BUSY, "why numbers");
_Static_assert((int)FCK_WHY_TIMEOUT == (int)FCK_AOS_WHY_TIMEOUT, "why numbers");
_Static_assert((int)FCK_WHY_IO == (int)FCK_AOS_WHY_IO, "why numbers");
_Static_assert((int)FCK_WHY_REFUSED == (int)FCK_AOS_WHY_REFUSED, "why numbers");
_Static_assert((int)FCK_WHY_AO_CALLER == (int)FCK_AOS_WHY_AO_CALLER, "why numbers");

/** The tasks that carry requests out. Two: a long listening does not hold up a short command. */
#define EXECUTORS          2U
#define EXECUTOR_STACK     8192U
/* The executors are workers of the SDK, not the app's code: they take a request off the
 * queue, wait on the core's call and post the answer. They sit at 2, the level kode-sdk's
 * map (kqp_config.h) gives a worker task (AO_Net's shares it: "legal because a
 * worker is not an Active Object"): above the app's main task, so a request is taken and
 * its answer posted without waiting for it, and below every owner Active Object. */
#define EXECUTOR_PRIORITY  (tskIDLE_PRIORITY + 2U)
#define END_STACK          4096U
#define POLL_MS            1000U
/** How long an answer waits for an event block or for room in the asker's queue. */
#define ANSWER_TRIES       100U
#define ANSWER_STEP_MS     10U
/** The pool margin the entries and the news leave free for everybody else's events. */
#define POOL_MARGIN        2U
/** A command's line and verb, copied. */
#define COMMAND_LINE_MAX           256U
#define COMMAND_VERB_MAX           32U

/* ---- Why --------------------------------------------------------------------------- */

const char *fck_why_text(fck_why_t why)
{
    switch (why) {
    case FCK_WHY_NONE:
        return "";
    case FCK_WHY_NOT_BEGUN:
        return "fck_begin() / fck_sys_begin() has not run, or the end has";
    case FCK_WHY_LEAVING:
        return "the way home, a rest or a restart has begun";
    case FCK_WHY_NO_MODULE:
        return "no F*CK is on J3, it is powered off, or it was just pulled out";
    case FCK_WHY_CHIP_HELD:
        return "the chip is held in reset or kept by a flash";
    case FCK_WHY_CHIP_NOT_READY:
        return "the chip has not said what it runs, or the GNSS receiver holds the bus";
    case FCK_WHY_BAD_ARGUMENT:
        return "an argument is NULL, 0 or out of range";
    case FCK_WHY_BEYOND_LIMITS:
        return "beyond the module's radio limits, or a rolling code, or a line the gate refuses";
    case FCK_WHY_FIRMWARE_TOO_OLD:
        return "the chip's firmware is too old for the command";
    case FCK_WHY_WRONG_ORDER:
        return "asked out of order: a read before its start, a stop with nothing running";
    case FCK_WHY_CANCELLED:
        return "cancelled: by fck_cancel(), or a send cut by the way home, a rest or the end";
    case FCK_WHY_BUSY:
        return "busy: a chip another task keeps, or no memory or room for the call";
    case FCK_WHY_TIMEOUT:
        return "a wait ran out";
    case FCK_WHY_IO:
        return "the module or a bus failed under the call, or the firmware said it did not do it";
    case FCK_WHY_REFUSED:
        return "refused for a reason the log says";
    case FCK_WHY_AO_CALLER:
        return "a call of fck.h that blocks was made from an Active Object: call fck_sys_*() instead";
    default:
        return "";
    }
}

/* ---- What a request is ---------------------------------------------------------------- */

typedef enum {
    K_COMMAND = 0, K_RESET, K_POWER, K_FIRMWARE, K_FLASH, K_RESCUE,
    K_WIFI_SCAN, K_WIFI_CHANNELS, K_BLE_SCAN, K_GNSS_BEGIN, K_GNSS_END,
    K_RADIO_SPECTRUM, K_RADIO_SNIFF, K_RADIO_PING, K_ECHO_START, K_ECHO_STOP,
    K_LEVEL, K_SPECTRUM, K_FIND, K_CAPTURE, K_PACKET_RECEIVE,
    K_SEND_RAW, K_SEND_CODE, K_PACKET_SEND,
    K_COUNT
} kind_t;

/* Doors: one per kind of request and asker. */
typedef enum {
    D_COMMAND = 0, D_FLASH, D_SCAN, D_GNSS, D_RADIO, D_LISTEN, D_SEND, D_COUNT
} door_t;

static const struct {
    QSignal signal;
    door_t  door;
} KIND[K_COUNT] = {
    [K_COMMAND]        = { FCK_COMMAND_RSP_SIG,               D_COMMAND },
    [K_RESET]          = { FCK_RESET_RSP_SIG,                 D_COMMAND },
    [K_POWER]          = { FCK_POWER_RSP_SIG,                 D_COMMAND },
    [K_FIRMWARE]          = { FCK_FIRMWARE_RSP_SIG,                 D_COMMAND },
    [K_FLASH]          = { FCK_FLASH_RSP_SIG,                 D_FLASH },
    [K_RESCUE]         = { FCK_RESCUE_PROBE_RSP_SIG,          D_FLASH },
    [K_WIFI_SCAN]      = { FCK_WIFI_SCAN_RSP_SIG,             D_SCAN },
    [K_WIFI_CHANNELS]  = { FCK_WIFI_CHANNELS_RSP_SIG,         D_SCAN },
    [K_BLE_SCAN]       = { FCK_BLE_SCAN_RSP_SIG,              D_SCAN },
    [K_GNSS_BEGIN]     = { FCK_GNSS_BEGIN_RSP_SIG,            D_GNSS },
    [K_GNSS_END]       = { FCK_GNSS_END_RSP_SIG,              D_GNSS },
    [K_RADIO_SPECTRUM] = { FCK_RADIO_SPECTRUM_RSP_SIG,        D_RADIO },
    [K_RADIO_SNIFF]    = { FCK_RADIO_SNIFF_RSP_SIG,           D_RADIO },
    [K_RADIO_PING]     = { FCK_RADIO_PING_RSP_SIG,            D_RADIO },
    [K_ECHO_START]     = { FCK_RADIO_ECHO_START_RSP_SIG,      D_RADIO },
    [K_ECHO_STOP]      = { FCK_RADIO_ECHO_STOP_RSP_SIG,       D_RADIO },
    [K_LEVEL]          = { FCK_SUBGHZ_LEVEL_RSP_SIG,          D_LISTEN },
    [K_SPECTRUM]       = { FCK_SUBGHZ_SPECTRUM_RSP_SIG,       D_LISTEN },
    [K_FIND]           = { FCK_SUBGHZ_FIND_RSP_SIG,           D_LISTEN },
    [K_CAPTURE]        = { FCK_SUBGHZ_CAPTURE_RSP_SIG,        D_LISTEN },
    [K_PACKET_RECEIVE] = { FCK_SUBGHZ_PACKET_RECEIVE_RSP_SIG, D_LISTEN },
    [K_SEND_RAW]       = { FCK_SUBGHZ_SEND_RAW_RSP_SIG,       D_SEND },
    [K_SEND_CODE]      = { FCK_SUBGHZ_SEND_CODE_RSP_SIG,      D_SEND },
    [K_PACKET_SEND]    = { FCK_SUBGHZ_PACKET_SEND_RSP_SIG,    D_SEND },
};

typedef struct {
    kind_t   kind;
    QActive *me;
    uint32_t mark;          /* fck_aos_cancel_mark() when it was accepted */
    union {
        struct { fck_chip_t chip; char line[COMMAND_LINE_MAX]; char verb[COMMAND_VERB_MAX]; bool has_verb; uint32_t ms;
                 char *reply; size_t cap; } command;
        struct { fck_chip_t chip; bool on; fck_firmware_t *out; } chip;
        struct { fck_chip_t chip; const fck_image_t *img; size_t n; fck_flash_report_t *rep; } flash;
        struct { fck_chip_t chip; fck_rescue_report_t *out; } rescue;
        struct { fck_band_t band; uint16_t dwell_ms; fck_wifi_channel_t *out; size_t cap; } wifi;
        struct { uint8_t duration_s; } ble;
        struct { uint16_t first_mhz; uint16_t last_mhz; uint32_t dwell_us; uint16_t sweeps; int8_t *dbm; size_t cap; } rspec;
        struct { uint16_t mhz; uint32_t ms; int8_t rssi_min; fck_radio_frame_t *out; size_t cap;
                 fck_radio_sniff_statistics_t *stats; } sniff;
        struct { uint16_t count; uint16_t mhz; uint16_t gap_ms; int8_t dbm; fck_radio_ping_statistics_t *stats; } ping;
        struct { uint16_t mhz; int8_t dbm; fck_radio_echo_statistics_t *stats; } echo;
        struct { uint32_t hz; uint32_t step_hz; uint16_t count; uint32_t ms; fck_subghz_bandwidth_t bw;
                 int16_t *dbm; size_t cap; fck_subghz_hit_t *hit; int32_t *us; } sub;
        struct { uint32_t hz; fck_subghz_packet_settings_t settings; uint32_t ms; fck_subghz_packet_t *packets;
                 size_t cap; } rx;
        struct { uint32_t hz; const int32_t *us; size_t n; uint8_t repeats; } raw;
        struct { uint32_t hz; fck_subghz_code_t code; uint8_t frames; } code;
        struct { uint32_t hz; fck_subghz_packet_settings_t settings; uint8_t bytes[FCK_SUBGHZ_PACKET_MAX];
                 uint8_t length; uint8_t repeats; } packet;
    } a;
} req_t;

/* ---- State ------------------------------------------------------------------------------ */

static portMUX_TYPE   s_mux = portMUX_INITIALIZER_UNLOCKED;
static QueueHandle_t  s_queue;
static bool           s_tasks_started;
static bool           s_sink_set;
static SemaphoreHandle_t s_end_sem;
static StaticSemaphore_t s_end_sem_buf;
static bool           s_end_started;
static bool           s_end_pending;

static struct {
    QActive *me;
    uint16_t open;      /* bit per door */
} s_asker[FCK_SYS_ASKERS_MAX];

static QActive *s_subscriber[FCK_SUBSCRIBERS_MAX];

/* ---- Doors --------------------------------------------------------------------------------- */

/* KODE_OK door taken; KODE_DENIED door busy; KODE_NO_RESOURCES no room for another asker. */
static kode_status_t door_take(QActive *me, door_t door)
{
    kode_status_t st = KODE_NO_RESOURCES;
    taskENTER_CRITICAL(&s_mux);
    int free_slot = -1;
    for (size_t i = 0U; i < FCK_SYS_ASKERS_MAX; i++) {
        if (s_asker[i].me == me) {
            st = ((s_asker[i].open & (1U << (unsigned)door)) != 0U) ? KODE_DENIED : KODE_OK;
            if (st == KODE_OK) {
                s_asker[i].open |= (uint16_t)(1U << (unsigned)door);
            }
            free_slot = -2;
            break;
        }
        if (s_asker[i].me == NULL && free_slot == -1) {
            free_slot = (int)i;
        }
    }
    if (free_slot >= 0) {
        s_asker[free_slot].me   = me;
        s_asker[free_slot].open = (uint16_t)(1U << (unsigned)door);
        st = KODE_OK;
    }
    taskEXIT_CRITICAL(&s_mux);
    return st;
}

static void door_give(QActive *me, door_t door)
{
    taskENTER_CRITICAL(&s_mux);
    for (size_t i = 0U; i < FCK_SYS_ASKERS_MAX; i++) {
        if (s_asker[i].me == me) {
            s_asker[i].open &= (uint16_t)~(1U << (unsigned)door);
            if (s_asker[i].open == 0U) {
                s_asker[i].me = NULL;
            }
            break;
        }
    }
    taskEXIT_CRITICAL(&s_mux);
}

/* ---- Posting: the answer, the entries, the news ----------------------------------------------- */

static void answer(const req_t *r, kode_status_t rc, fck_why_t why, uint32_t count, int32_t value,
                   uint32_t flags)
{
    QSignal const signal = KIND[r->kind].signal;
    /* Free again before the answer, so that the handler may ask again. */
    door_give(r->me, KIND[r->kind].door);
    for (unsigned i = 0U; i < ANSWER_TRIES; i++) {
        fck_sys_answer_t *const e = Q_NEW_X(fck_sys_answer_t, 0U, signal);
        if (e != NULL) {
            e->rc    = rc;
            e->why   = why;
            e->count = count;
            e->value = value;
            e->flags = flags;
            /* Not posted, QActive_post_() hands the block back to its pool. */
            if (QACTIVE_POST_X(r->me, &e->super, 0U, (void *)0)) {
                return;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(ANSWER_STEP_MS) > 0U ? pdMS_TO_TICKS(ANSWER_STEP_MS) : 1U);
    }
    KLOG_E(TAG, "an answer (signal %u) found no event block or no room in the asker's queue for 1 s: lost. "
                "The queue must hold one slot per door open (fck_sys.h, queue depth)", (unsigned)signal);
    fck_aos_dropped_add(FCK_AOS_SINK_ANSWERS, 1U);
}

/* One entry of a scan, with a margin: a full queue loses it, never the answer. */
static bool post_wifi_entry(QActive *me, uint16_t index, const fck_wifi_network_t *n)
{
    fck_sys_wifi_entry_t *const e = Q_NEW_X(fck_sys_wifi_entry_t, POOL_MARGIN, FCK_WIFI_SCAN_ENTRY_SIG);
    if (e == NULL) {
        return false;
    }
    e->index   = index;
    e->network = *n;
    return QACTIVE_POST_X(me, &e->super, FCK_SYS_ENTRY_MARGIN, (void *)0);
}

static bool post_ble_entry(QActive *me, uint16_t index, const fck_ble_device_t *d)
{
    fck_sys_ble_entry_t *const e = Q_NEW_X(fck_sys_ble_entry_t, POOL_MARGIN, FCK_BLE_SCAN_ENTRY_SIG);
    if (e == NULL) {
        return false;
    }
    e->index  = index;
    e->device = *d;
    return QACTIVE_POST_X(me, &e->super, FCK_SYS_ENTRY_MARGIN, (void *)0);
}

/* The owner's events, as signals. Called on the owner's task (or the worker's): it never waits. */
static bool news_sink(fck_aos_event_t ev, uint32_t arg, void *ctx)
{
    (void)ctx;
    QSignal signal = 0U;
    int32_t value  = (int32_t)arg;
    switch (ev) {
    case FCK_AOS_EVENT_PRESENT:
        signal = FCK_MODULE_FOUND_SIG;
        value  = 1;
        break;
    case FCK_AOS_EVENT_GONE:
        signal = FCK_MODULE_LOST_SIG;
        value  = 0;
        break;
    case FCK_AOS_EVENT_WRONG_MODULE:
        signal = FCK_WRONG_MODULE_SIG;
        break;
    case FCK_AOS_EVENT_SETTING_LOST:
        signal = FCK_SETTING_LOST_SIG;
        break;
    case FCK_AOS_EVENT_SETTING_RESTORED:
        signal = FCK_SETTING_RESTORED_SIG;
        break;
    case FCK_AOS_EVENT_PACKET_RECEIVED:
        signal = FCK_PACKET_RECEIVED_SIG;
        break;
    case FCK_AOS_EVENT_SEND_CANCELLED:
        signal = FCK_SEND_CANCELLED_SIG;
        value  = (arg == FCK_AOS_CANCEL_CALL) ? FCK_CANCEL_CALL : FCK_CANCEL_LEAVING;
        break;
    default:
        return true;
    }
    QActive *to[FCK_SUBSCRIBERS_MAX];
    taskENTER_CRITICAL(&s_mux);
    (void)memcpy(to, s_subscriber, sizeof(to));
    taskEXIT_CRITICAL(&s_mux);
    bool all = true;
    for (size_t i = 0U; i < FCK_SUBSCRIBERS_MAX; i++) {
        if (to[i] == NULL) {
            continue;
        }
        fck_sys_news_t *const e = Q_NEW_X(fck_sys_news_t, POOL_MARGIN, signal);
        if (e == NULL) {
            all = false;
            continue;
        }
        e->value = value;
        if (!QACTIVE_POST_X(to[i], &e->super, FCK_SYS_NEWS_MARGIN, (void *)0)) {
            all = false;
        }
    }
    return all;
}

/* ---- The executors ------------------------------------------------------------------------------ */

/* A command's reply: its tagged lines, "TAG body\n", into the buffer lent. */
typedef struct {
    char  *buf;
    size_t cap;
    size_t n;
    bool   truncated;
} reply_t;

static void reply_put(reply_t *r, const char *text)
{
    size_t const len = strlen(text);
    size_t const room = (r->cap > r->n + 1U) ? (r->cap - r->n - 1U) : 0U;
    size_t const k    = (len < room) ? len : room;
    (void)memcpy(&r->buf[r->n], text, k);
    r->n += k;
    r->buf[r->n] = '\0';
    if (k < len) {
        r->truncated = true;
    }
}

static void on_reply_line(void *ctx, const fck_line_t *line)
{
    reply_t *const r = (reply_t *)ctx;
    reply_put(r, line->tag);
    reply_put(r, " ");
    reply_put(r, line->body);
    reply_put(r, "\n");
}

static void exec_scan_entries(req_t *r, kode_status_t *st, uint32_t *count, int32_t *value)
{
    if (r->kind == K_WIFI_SCAN) {
        fck_wifi_network_t *const list = malloc(FCK_WIFI_NETWORK_MAX * sizeof(list[0]));
        if (list == NULL) {
            *st = KODE_NO_RESOURCES;
            fck_aos_error_set_for(*st, "no memory for the scan's list");
            return;
        }
        size_t n = 0U;
        *st = fck_core_wifi_scan(r->a.wifi.band, list, FCK_WIFI_NETWORK_MAX, &n);
        if (*st == KODE_OK) {
            int32_t lost = 0;
            for (size_t i = 0U; i < n; i++) {
                if (!post_wifi_entry(r->me, (uint16_t)i, &list[i])) {
                    lost++;
                }
            }
            *count = (uint32_t)n;
            *value = lost;
        }
        free(list);
    } else {
        fck_ble_device_t *const list = malloc(FCK_BLE_DEVICE_MAX * sizeof(list[0]));
        if (list == NULL) {
            *st = KODE_NO_RESOURCES;
            fck_aos_error_set_for(*st, "no memory for the scan's list");
            return;
        }
        size_t n = 0U;
        *st = fck_core_ble_scan(r->a.ble.duration_s, list, FCK_BLE_DEVICE_MAX, &n);
        if (*st == KODE_OK) {
            int32_t lost = 0;
            for (size_t i = 0U; i < n; i++) {
                if (!post_ble_entry(r->me, (uint16_t)i, &list[i])) {
                    lost++;
                }
            }
            *count = (uint32_t)n;
            *value = lost;
        }
        free(list);
    }
}

static kode_status_t exec_run(req_t *r, uint32_t *count, int32_t *value, uint32_t *flags)
{
    kode_status_t st = KODE_ERROR;
    size_t        n  = 0U;
    switch (r->kind) {
    case K_COMMAND: {
        reply_t reply = { .buf = r->a.command.reply, .cap = r->a.command.cap };
        if (reply.buf != NULL && reply.cap > 0U) {
            reply.buf[0] = '\0';
        }
        int32_t rc = -1;
        st = fck_core_command(r->a.command.chip, r->a.command.line,
                              r->a.command.has_verb ? r->a.command.verb : NULL,
                              (reply.buf != NULL && reply.cap > 0U) ? on_reply_line : NULL, &reply,
                              r->a.command.ms, &rc);
        *value = rc;
        *count = (uint32_t)reply.n;
        if (reply.truncated) {
            *flags |= FCK_SYS_ANSWER_TRUNCATED;
        }
        break;
    }
    case K_RESET:
        st = fck_core_reset(r->a.chip.chip);
        break;
    case K_POWER:
        st = fck_core_power(r->a.chip.chip, r->a.chip.on);
        break;
    case K_FIRMWARE:
        st = fck_core_firmware(r->a.chip.chip, r->a.chip.out);
        break;
    case K_FLASH:
        st = fck_core_flash(r->a.flash.chip, r->a.flash.img, r->a.flash.n, r->a.flash.rep);
        break;
    case K_RESCUE:
        st = fck_core_rescue_probe(r->a.rescue.chip, r->a.rescue.out);
        break;
    case K_WIFI_SCAN:
    case K_BLE_SCAN:
        exec_scan_entries(r, &st, count, value);
        break;
    case K_WIFI_CHANNELS:
        st = fck_core_wifi_channels(r->a.wifi.band, r->a.wifi.dwell_ms, r->a.wifi.out, r->a.wifi.cap, &n);
        *count = (uint32_t)n;
        break;
    case K_GNSS_BEGIN:
        st = fck_core_gnss_begin();
        break;
    case K_GNSS_END:
        st = fck_core_gnss_end();
        break;
    case K_RADIO_SPECTRUM:
        st = fck_core_radio_spectrum(r->a.rspec.first_mhz, r->a.rspec.last_mhz, r->a.rspec.dwell_us,
                                     r->a.rspec.sweeps, r->a.rspec.dbm, r->a.rspec.cap, &n);
        *count = (uint32_t)n;
        break;
    case K_RADIO_SNIFF:
        st = fck_core_radio_sniff(r->a.sniff.mhz, r->a.sniff.ms, r->a.sniff.rssi_min, r->a.sniff.out,
                                  r->a.sniff.cap, &n, r->a.sniff.stats);
        *count = (uint32_t)n;
        break;
    case K_RADIO_PING:
        st = fck_core_radio_ping(r->a.ping.count, r->a.ping.mhz, r->a.ping.gap_ms, r->a.ping.dbm,
                                 r->a.ping.stats);
        break;
    case K_ECHO_START:
        st = fck_core_radio_echo_start(r->a.echo.mhz, r->a.echo.dbm);
        break;
    case K_ECHO_STOP:
        st = fck_core_radio_echo_stop(r->a.echo.stats);
        break;
    case K_LEVEL:
        st = fck_core_subghz_level(r->a.sub.hz, r->a.sub.bw, r->a.sub.dbm);
        break;
    case K_SPECTRUM:
        st = fck_core_subghz_spectrum(r->a.sub.hz, r->a.sub.step_hz, r->a.sub.count, r->a.sub.bw,
                                      r->a.sub.dbm, r->a.sub.cap, &n);
        *count = (uint32_t)n;
        break;
    case K_FIND:
        st = fck_core_subghz_find(r->a.sub.ms, r->a.sub.hit);
        break;
    case K_CAPTURE:
        st = fck_core_subghz_capture(r->a.sub.hz, r->a.sub.ms, r->a.sub.us, r->a.sub.cap, &n);
        *count = (uint32_t)n;
        break;
    case K_PACKET_RECEIVE:
        st = fck_core_subghz_packet_receive(r->a.rx.hz, &r->a.rx.settings, r->a.rx.ms, r->a.rx.packets,
                                            r->a.rx.cap, &n);
        *count = (uint32_t)n;
        break;
    case K_SEND_RAW:
        st = fck_core_subghz_send_raw(r->a.raw.hz, r->a.raw.us, r->a.raw.n, r->a.raw.repeats);
        break;
    case K_SEND_CODE:
        st = fck_core_subghz_send_code(r->a.code.hz, &r->a.code.code, r->a.code.frames);
        break;
    case K_PACKET_SEND:
        st = fck_core_subghz_packet_send(r->a.packet.hz, &r->a.packet.settings, r->a.packet.bytes,
                                         r->a.packet.length, r->a.packet.repeats);
        break;
    default:
        break;
    }
    return st;
}

static void execute(req_t *r)
{
    uint32_t count = 0U;
    int32_t  value = 0;
    uint32_t flags = 0U;
    fck_aos_error_ok();
    kode_status_t st;
    if (fck_aos_cancelled(r->mark)) {
        /* fck_sys_cancel() came after it was accepted and before it ran: it never starts. */
        st = KODE_DENIED;
        fck_aos_error_set_for(st, "cancelled by fck_cancel()");
    } else {
        st = exec_run(r, &count, &value, &flags);
    }
    fck_why_t why = FCK_WHY_NONE;
    if (st != KODE_OK) {
        why = (fck_why_t)fck_aos_error_why();
        if (why == FCK_WHY_NONE) {
            why = FCK_WHY_REFUSED;
        }
    }
    answer(r, st, why, count, value, flags);
}

static void executor_task(void *arg)
{
    (void)arg;
    for (;;) {
        req_t *r = NULL;
        if (xQueueReceive(s_queue, &r, pdMS_TO_TICKS(POLL_MS)) != pdTRUE || r == NULL) {
            continue;
        }
        execute(r);
        free(r);
    }
}

static void end_task(void *arg)
{
    (void)arg;
    for (;;) {
        if (xSemaphoreTake(s_end_sem, pdMS_TO_TICKS(POLL_MS)) != pdTRUE) {
            continue;
        }
        bool const was = fck_aos_present();
        kode_status_t const st = fck_core_end();
        if (st == KODE_OK && was) {
            fck_aos_news(FCK_AOS_EVENT_GONE, 0U);
        }
        taskENTER_CRITICAL(&s_mux);
        s_end_pending = false;
        taskEXIT_CRITICAL(&s_mux);
    }
}

/* The queue and the executors, made once. */
static kode_status_t tasks_ready(void)
{
    taskENTER_CRITICAL(&s_mux);
    bool const first = !s_tasks_started;
    s_tasks_started  = true;
    taskEXIT_CRITICAL(&s_mux);
    if (first) {
        QueueHandle_t const q = xQueueCreate(FCK_SYS_QUEUE, sizeof(req_t *));
        bool ok = (q != NULL);
        if (ok) {
            s_queue = q;
            for (unsigned i = 0U; ok && i < EXECUTORS; i++) {
                ok = (xTaskCreate(executor_task, "fck_sys", EXECUTOR_STACK, NULL, EXECUTOR_PRIORITY, NULL) == pdPASS);
            }
        }
        if (!ok) {
            KLOG_E(TAG, "no memory for the requests' task and queue");
            taskENTER_CRITICAL(&s_mux);
            s_tasks_started = false;    /* the executors that did start keep serving s_queue; a retry makes more */
            taskEXIT_CRITICAL(&s_mux);
        }
    }
    return (s_queue != NULL) ? KODE_OK : KODE_NO_RESOURCES;
}

/* ---- Accepting a request ------------------------------------------------------------------------- */

/* The one test of an asking Active Object, as kode-sdk's doors make it (kaos_asker_started()): a
 * NULL one, one not started (priority 0) or one out of range is refused with KODE_DENIED, where
 * posting to it would reach QP's assert. */
static bool asker_started(QActive const *who)
{
    return (who != NULL) && (who->prio != 0U) && (who->prio <= QF_MAX_ACTIVE);
}

static bool began(void)
{
    fck_aos_status_t s;
    fck_aos_status(&s);
    return s.started;
}

/* A request in hand: me checked, begun, its door taken, its memory allocated. */
static req_t *request_new(kind_t kind, QActive *me, kode_status_t *st)
{
    if (!asker_started(me)) {
        *st = KODE_DENIED;
        fck_aos_error_set_for(*st, "me is NULL or not a started Active Object: a task asks through fck.h");
        return NULL;
    }
    if (!began()) {
        *st = KODE_DENIED;
        fck_aos_error_set_for(*st, "fck_begin() has not run, or fck_end() was called");
        return NULL;
    }
    if (tasks_ready() != KODE_OK) {
        *st = KODE_NO_RESOURCES;
        fck_aos_error_set_for(*st, "no memory for the requests' task and queue");
        return NULL;
    }
    *st = door_take(me, KIND[kind].door);
    if (*st != KODE_OK) {
        fck_aos_error_set_for(*st, (*st == KODE_DENIED) ? "the door of this kind of request is busy"
                                                         : "FCK_SYS_ASKERS_MAX Active Objects hold a door already");
        return NULL;
    }
    /* cppcheck-suppress misra-c2012-21.3 ; the request, freed by the executor that answers it */
    req_t *const r = calloc(1U, sizeof(*r));
    if (r == NULL) {
        door_give(me, KIND[kind].door);
        *st = KODE_NO_RESOURCES;
        fck_aos_error_set_for(*st, "no memory for the request");
        return NULL;
    }
    r->kind = kind;
    r->me   = me;
    r->mark = fck_aos_cancel_mark();
    return r;
}

static kode_status_t request_send(req_t *r)
{
    if (xQueueSend(s_queue, &r, 0) != pdTRUE) {
        door_give(r->me, KIND[r->kind].door);
        /* cppcheck-suppress misra-c2012-21.3 ; the request allocated by request_new() */
        free(r);
        fck_aos_error_set_for(KODE_NO_RESOURCES, "the SDK's queue of requests is full");
        return KODE_NO_RESOURCES;
    }
    return KODE_OK;
}

static kode_status_t deny(const char *why)
{
    fck_aos_error_set_for(KODE_DENIED, why);
    return KODE_DENIED;
}

static bool chip_ok(fck_chip_t chip)
{
    return chip == FCK_C5 || chip == FCK_NRF;
}

/* ---- Start and end -------------------------------------------------------------------------------- */

kode_status_t fck_sys_begin(void)
{
    taskENTER_CRITICAL(&s_mux);
    bool const ending = s_end_pending;
    taskEXIT_CRITICAL(&s_mux);
    if (ending) {
        return deny("fck_sys_end() is still under way: ask again when FCK_MODULE_LOST_SIG or a moment has passed");
    }
    kode_status_t const tasks = tasks_ready();
    if (tasks != KODE_OK) {
        fck_aos_error_set_for(tasks, "no memory for the requests' task and queue");
        return tasks;
    }
    if (!s_sink_set) {
        fck_aos_sink_set(FCK_AOS_SINK_NEWS, news_sink, NULL);
        s_sink_set = true;
    }
    kode_status_t const st = fck_core_begin();
    if (st == KODE_OK) {
        fck_aos_error_ok();
    }
    return st;
}

kode_status_t fck_sys_end(void)
{
    if (!began()) {
        return KODE_OK;
    }
    kode_status_t st = KODE_OK;
    taskENTER_CRITICAL(&s_mux);
    bool const pending = s_end_pending;
    bool const make    = !s_end_started;
    s_end_pending      = true;
    s_end_started      = true;
    taskEXIT_CRITICAL(&s_mux);
    if (pending) {
        return KODE_OK;
    }
    if (make) {
        s_end_sem = xSemaphoreCreateBinaryStatic(&s_end_sem_buf);
        if (xTaskCreate(end_task, "fck_sys_end", END_STACK, NULL, EXECUTOR_PRIORITY, NULL) != pdPASS) {
            st = KODE_NO_RESOURCES;
            taskENTER_CRITICAL(&s_mux);
            s_end_started = false;
            s_end_pending = false;
            taskEXIT_CRITICAL(&s_mux);
            fck_aos_error_set_for(st, "no memory for the end's task");
            return st;
        }
    }
    /* What is on the air is cut at once; the task finishes the end. */
    (void)fck_core_cancel();
    (void)xSemaphoreGive(s_end_sem);
    return KODE_OK;
}

/* ---- Commands -------------------------------------------------------------------------------------- */

kode_status_t fck_sys_cancel(void)
{
    return fck_core_cancel();
}

kode_status_t fck_sys_subghz_listen_start(uint32_t hz)
{
    return fck_core_subghz_listen_start(hz);
}

kode_status_t fck_sys_subghz_listen_stop(void)
{
    return fck_core_subghz_listen_stop();
}

kode_status_t fck_sys_radio_sniff_tune(const fck_radio_sniff_config_t *cfg)
{
    return fck_core_radio_sniff_tune_post(cfg);
}

/* ---- Requests ----------------------------------------------------------------------------------------- */

kode_status_t fck_sys_command(QActive *me, fck_chip_t chip, const char *line, const char *verb,
                              uint32_t ms, char *reply, size_t reply_cap)
{
    if (!chip_ok(chip) || line == NULL || strnlen(line, COMMAND_LINE_MAX) >= COMMAND_LINE_MAX
        || (verb != NULL && strnlen(verb, COMMAND_VERB_MAX) >= COMMAND_VERB_MAX) || (reply == NULL && reply_cap > 0U)) {
        return deny("chip is not FCK_C5 or FCK_NRF, line is NULL or 256 bytes or more, verb is 32 or more, or reply is NULL with a cap");
    }
    kode_status_t st;
    req_t *const  r = request_new(K_COMMAND, me, &st);
    if (r == NULL) {
        return st;
    }
    r->a.command.chip = chip;
    (void)strncpy(r->a.command.line, line, COMMAND_LINE_MAX - 1U);
    if (verb != NULL) {
        (void)strncpy(r->a.command.verb, verb, COMMAND_VERB_MAX - 1U);
        r->a.command.has_verb = true;
    }
    r->a.command.ms    = ms;
    r->a.command.reply = reply;
    r->a.command.cap   = reply_cap;
    return request_send(r);
}

kode_status_t fck_sys_reset(QActive *me, fck_chip_t chip)
{
    if (!chip_ok(chip)) {
        return deny("chip is not FCK_C5 or FCK_NRF");
    }
    kode_status_t st;
    req_t *const  r = request_new(K_RESET, me, &st);
    if (r == NULL) {
        return st;
    }
    r->a.chip.chip = chip;
    return request_send(r);
}

kode_status_t fck_sys_power(QActive *me, fck_chip_t chip, bool on)
{
    if (!chip_ok(chip)) {
        return deny("chip is not FCK_C5 or FCK_NRF");
    }
    kode_status_t st;
    req_t *const  r = request_new(K_POWER, me, &st);
    if (r == NULL) {
        return st;
    }
    r->a.chip.chip = chip;
    r->a.chip.on   = on;
    return request_send(r);
}

kode_status_t fck_sys_firmware(QActive *me, fck_chip_t chip, fck_firmware_t *out)
{
    if (!chip_ok(chip) || out == NULL) {
        return deny("chip is not FCK_C5 or FCK_NRF, or out is NULL");
    }
    kode_status_t st;
    req_t *const  r = request_new(K_FIRMWARE, me, &st);
    if (r == NULL) {
        return st;
    }
    r->a.chip.chip = chip;
    r->a.chip.out  = out;
    return request_send(r);
}

kode_status_t fck_sys_flash(QActive *me, fck_chip_t chip, const fck_image_t *img, size_t n,
                            fck_flash_report_t *rep)
{
    if (!chip_ok(chip) || img == NULL || n == 0U) {
        return deny("chip is not FCK_C5 or FCK_NRF, img is NULL, or n is 0");
    }
    kode_status_t st;
    req_t *const  r = request_new(K_FLASH, me, &st);
    if (r == NULL) {
        return st;
    }
    r->a.flash.chip = chip;
    r->a.flash.img  = img;
    r->a.flash.n    = n;
    r->a.flash.rep  = rep;
    return request_send(r);
}

kode_status_t fck_sys_rescue_probe(QActive *me, fck_chip_t chip, fck_rescue_report_t *out)
{
    if (!chip_ok(chip) || out == NULL) {
        return deny("chip is not FCK_C5 or FCK_NRF, or out is NULL");
    }
    kode_status_t st;
    req_t *const  r = request_new(K_RESCUE, me, &st);
    if (r == NULL) {
        return st;
    }
    r->a.rescue.chip = chip;
    r->a.rescue.out  = out;
    return request_send(r);
}

kode_status_t fck_sys_wifi_scan(QActive *me, fck_band_t band)
{
    kode_status_t st;
    req_t *const  r = request_new(K_WIFI_SCAN, me, &st);
    if (r == NULL) {
        return st;
    }
    r->a.wifi.band = band;
    return request_send(r);
}

kode_status_t fck_sys_wifi_channels(QActive *me, fck_band_t band, uint16_t dwell_ms,
                                    fck_wifi_channel_t *out, size_t cap)
{
    if (out == NULL || cap == 0U) {
        return deny("out is NULL, or cap is 0");
    }
    kode_status_t st;
    req_t *const  r = request_new(K_WIFI_CHANNELS, me, &st);
    if (r == NULL) {
        return st;
    }
    r->a.wifi.band     = band;
    r->a.wifi.dwell_ms = dwell_ms;
    r->a.wifi.out      = out;
    r->a.wifi.cap      = cap;
    return request_send(r);
}

kode_status_t fck_sys_ble_scan(QActive *me, uint8_t duration_s)
{
    kode_status_t st;
    req_t *const  r = request_new(K_BLE_SCAN, me, &st);
    if (r == NULL) {
        return st;
    }
    r->a.ble.duration_s = duration_s;
    return request_send(r);
}

kode_status_t fck_sys_gnss_begin(QActive *me)
{
    kode_status_t st;
    req_t *const  r = request_new(K_GNSS_BEGIN, me, &st);
    return (r == NULL) ? st : request_send(r);
}

kode_status_t fck_sys_gnss_end(QActive *me)
{
    kode_status_t st;
    req_t *const  r = request_new(K_GNSS_END, me, &st);
    return (r == NULL) ? st : request_send(r);
}

kode_status_t fck_sys_radio_spectrum(QActive *me, uint16_t first_mhz, uint16_t last_mhz,
                                     uint32_t dwell_us, uint16_t sweeps, int8_t *dbm, size_t cap)
{
    if (dbm == NULL || cap == 0U) {
        return deny("dbm is NULL, or cap is 0");
    }
    kode_status_t st;
    req_t *const  r = request_new(K_RADIO_SPECTRUM, me, &st);
    if (r == NULL) {
        return st;
    }
    r->a.rspec.first_mhz = first_mhz;
    r->a.rspec.last_mhz  = last_mhz;
    r->a.rspec.dwell_us  = dwell_us;
    r->a.rspec.sweeps    = sweeps;
    r->a.rspec.dbm       = dbm;
    r->a.rspec.cap       = cap;
    return request_send(r);
}

kode_status_t fck_sys_radio_sniff(QActive *me, uint16_t mhz, uint32_t ms, int8_t rssi_min,
                                  fck_radio_frame_t *out, size_t cap, fck_radio_sniff_statistics_t *stats)
{
    if (out == NULL || cap == 0U) {
        return deny("out is NULL, or cap is 0");
    }
    kode_status_t st;
    req_t *const  r = request_new(K_RADIO_SNIFF, me, &st);
    if (r == NULL) {
        return st;
    }
    r->a.sniff.mhz      = mhz;
    r->a.sniff.ms       = ms;
    r->a.sniff.rssi_min = rssi_min;
    r->a.sniff.out      = out;
    r->a.sniff.cap      = cap;
    r->a.sniff.stats    = stats;
    return request_send(r);
}

kode_status_t fck_sys_radio_ping(QActive *me, uint16_t count, uint16_t mhz, uint16_t gap_ms, int8_t dbm,
                                 fck_radio_ping_statistics_t *stats)
{
    if (stats == NULL) {
        return deny("stats is NULL");
    }
    kode_status_t st;
    req_t *const  r = request_new(K_RADIO_PING, me, &st);
    if (r == NULL) {
        return st;
    }
    r->a.ping.count  = count;
    r->a.ping.mhz    = mhz;
    r->a.ping.gap_ms = gap_ms;
    r->a.ping.dbm    = dbm;
    r->a.ping.stats  = stats;
    return request_send(r);
}

kode_status_t fck_sys_radio_echo_start(QActive *me, uint16_t mhz, int8_t dbm)
{
    kode_status_t st;
    req_t *const  r = request_new(K_ECHO_START, me, &st);
    if (r == NULL) {
        return st;
    }
    r->a.echo.mhz = mhz;
    r->a.echo.dbm = dbm;
    return request_send(r);
}

kode_status_t fck_sys_radio_echo_stop(QActive *me, fck_radio_echo_statistics_t *stats)
{
    kode_status_t st;
    req_t *const  r = request_new(K_ECHO_STOP, me, &st);
    if (r == NULL) {
        return st;
    }
    r->a.echo.stats = stats;
    return request_send(r);
}

kode_status_t fck_sys_subghz_level(QActive *me, uint32_t hz, fck_subghz_bandwidth_t bw, int16_t *dbm)
{
    if (dbm == NULL) {
        return deny("dbm is NULL");
    }
    kode_status_t st;
    req_t *const  r = request_new(K_LEVEL, me, &st);
    if (r == NULL) {
        return st;
    }
    r->a.sub.hz  = hz;
    r->a.sub.bw  = bw;
    r->a.sub.dbm = dbm;
    return request_send(r);
}

kode_status_t fck_sys_subghz_spectrum(QActive *me, uint32_t first_hz, uint32_t step_hz, uint16_t count,
                                      fck_subghz_bandwidth_t bw, int16_t *dbm, size_t cap)
{
    if (dbm == NULL || cap == 0U) {
        return deny("dbm is NULL, or cap is 0");
    }
    kode_status_t st;
    req_t *const  r = request_new(K_SPECTRUM, me, &st);
    if (r == NULL) {
        return st;
    }
    r->a.sub.hz      = first_hz;
    r->a.sub.step_hz = step_hz;
    r->a.sub.count   = count;
    r->a.sub.bw      = bw;
    r->a.sub.dbm     = dbm;
    r->a.sub.cap     = cap;
    return request_send(r);
}

kode_status_t fck_sys_subghz_find(QActive *me, uint32_t ms, fck_subghz_hit_t *hit)
{
    if (hit == NULL) {
        return deny("hit is NULL");
    }
    kode_status_t st;
    req_t *const  r = request_new(K_FIND, me, &st);
    if (r == NULL) {
        return st;
    }
    r->a.sub.ms  = ms;
    r->a.sub.hit = hit;
    return request_send(r);
}

kode_status_t fck_sys_subghz_capture(QActive *me, uint32_t hz, uint32_t ms, int32_t *us, size_t cap)
{
    if (us == NULL || cap == 0U) {
        return deny("us is NULL, or cap is 0");
    }
    kode_status_t st;
    req_t *const  r = request_new(K_CAPTURE, me, &st);
    if (r == NULL) {
        return st;
    }
    r->a.sub.hz  = hz;
    r->a.sub.ms  = ms;
    r->a.sub.us  = us;
    r->a.sub.cap = cap;
    return request_send(r);
}

kode_status_t fck_sys_subghz_packet_receive(QActive *me, uint32_t hz,
                                            const fck_subghz_packet_settings_t *settings, uint32_t ms,
                                            fck_subghz_packet_t *packets, size_t cap)
{
    if (settings == NULL || packets == NULL || cap == 0U) {
        return deny("settings or packets is NULL, or cap is 0");
    }
    kode_status_t st;
    req_t *const  r = request_new(K_PACKET_RECEIVE, me, &st);
    if (r == NULL) {
        return st;
    }
    r->a.rx.hz       = hz;
    r->a.rx.settings = *settings;
    r->a.rx.ms       = ms;
    r->a.rx.packets  = packets;
    r->a.rx.cap      = cap;
    return request_send(r);
}

kode_status_t fck_sys_subghz_send_raw(QActive *me, uint32_t hz, const int32_t *us, size_t n, uint8_t repeats)
{
    if (us == NULL || n == 0U) {
        return deny("us is NULL, or n is 0");
    }
    kode_status_t st;
    req_t *const  r = request_new(K_SEND_RAW, me, &st);
    if (r == NULL) {
        return st;
    }
    r->a.raw.hz      = hz;
    r->a.raw.us      = us;
    r->a.raw.n       = n;
    r->a.raw.repeats = repeats;
    return request_send(r);
}

kode_status_t fck_sys_subghz_send_code(QActive *me, uint32_t hz, const fck_subghz_code_t *code, uint8_t frames)
{
    if (code == NULL) {
        return deny("code is NULL");
    }
    kode_status_t st;
    req_t *const  r = request_new(K_SEND_CODE, me, &st);
    if (r == NULL) {
        return st;
    }
    r->a.code.hz     = hz;
    r->a.code.code   = *code;
    r->a.code.frames = frames;
    return request_send(r);
}

kode_status_t fck_sys_subghz_packet_send(QActive *me, uint32_t hz, const fck_subghz_packet_settings_t *settings,
                                         const uint8_t *bytes, uint8_t length, uint8_t repeats)
{
    if (settings == NULL || bytes == NULL || length == 0U || length > FCK_SUBGHZ_PACKET_MAX) {
        return deny("settings or bytes is NULL, or length is 0 or above FCK_SUBGHZ_PACKET_MAX");
    }
    kode_status_t st;
    req_t *const  r = request_new(K_PACKET_SEND, me, &st);
    if (r == NULL) {
        return st;
    }
    r->a.packet.hz       = hz;
    r->a.packet.settings = *settings;
    (void)memcpy(r->a.packet.bytes, bytes, length);
    r->a.packet.length   = length;
    r->a.packet.repeats  = repeats;
    return request_send(r);
}

/* ---- Immediate ------------------------------------------------------------------------------------------- */

bool fck_sys_is_present(void)
{
    return fck_is_present();
}

kode_status_t fck_sys_status_read(fck_status_t *out)
{
    return fck_core_status(out);
}

uint8_t fck_sys_header_position(uint8_t pos)
{
    return fck_header_position(pos);
}

kode_status_t fck_sys_gnss_read(fck_gnss_fix_t *out)
{
    return fck_core_gnss_read(out, 0U);
}

kode_status_t fck_sys_subghz_listen_read(uint32_t after, int32_t *us, size_t cap, size_t *n,
                                         fck_subghz_heard_t *heard)
{
    return fck_core_subghz_listen_read(after, 0U, us, cap, n, heard);
}

uint32_t fck_sys_subghz_listen_hz(void)
{
    return fck_subghz_listen_hz();
}

kode_status_t fck_sys_subghz_frequencies(uint32_t *hz, size_t cap, size_t *n)
{
    return fck_core_subghz_frequencies(hz, cap, n);
}

bool fck_sys_subghz_may_transmit(uint32_t hz)
{
    return fck_subghz_may_transmit(hz);
}

bool fck_sys_subghz_is_rolling(const int32_t *us, size_t n)
{
    return fck_subghz_is_rolling(us, n);
}

kode_status_t fck_sys_subghz_air_time_left(uint32_t hz, uint32_t *ms)
{
    return fck_core_subghz_air_time_left(hz, ms);
}

kode_status_t fck_sys_subghz_protocol_info(fck_subghz_protocol_t proto, fck_subghz_protocol_info_t *info)
{
    return fck_core_subghz_protocol_info(proto, info);
}

kode_status_t fck_sys_subscribe(QActive *me)
{
    if (!asker_started(me)) {
        return deny("me is NULL or not a started Active Object");
    }
    kode_status_t st = KODE_NO_RESOURCES;
    taskENTER_CRITICAL(&s_mux);
    int free_slot = -1;
    for (size_t i = 0U; i < FCK_SUBSCRIBERS_MAX; i++) {
        if (s_subscriber[i] == me) {
            st        = KODE_OK;
            free_slot = -2;
            break;
        }
        if (s_subscriber[i] == NULL && free_slot == -1) {
            free_slot = (int)i;
        }
    }
    if (free_slot >= 0) {
        s_subscriber[free_slot] = me;
        st = KODE_OK;
    }
    taskEXIT_CRITICAL(&s_mux);
    if (st != KODE_OK) {
        fck_aos_error_set_for(st, "FCK_SUBSCRIBERS_MAX Active Objects are subscribed already");
    } else if (!s_sink_set) {
        fck_aos_sink_set(FCK_AOS_SINK_NEWS, news_sink, NULL);
        s_sink_set = true;
    }
    return st;
}

kode_status_t fck_sys_unsubscribe(QActive *me)
{
    if (me == NULL) {
        return deny("me is NULL");
    }
    taskENTER_CRITICAL(&s_mux);
    for (size_t i = 0U; i < FCK_SUBSCRIBERS_MAX; i++) {
        if (s_subscriber[i] == me) {
            s_subscriber[i] = NULL;
        }
    }
    taskEXIT_CRITICAL(&s_mux);
    return KODE_OK;
}
