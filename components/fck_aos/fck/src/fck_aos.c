/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file fck_aos.c
 * @brief The module's owner: what AO_Fck's signals do. See fck_aos.h, and
 *        fck_ao.c for the Active Object itself.
 *
 * Everything here runs on one of two tasks, never both at once: the Active
 * Object's step, and a job on its worker. While a job runs the step leaves
 * the module alone - no UART read, no look, no expander - and the requests
 * that would touch it wait in s.deferred until the job's done event.
 *
 * Finding the module, telling it from another, the power-loss mark and the
 * antenna switch at rest follow the module's own firmware. The GNSS reset by
 * hand and the patience after it give the receiver the time it takes. The bus
 * lives on core 1 (fck_drv_tca6408a.h).
 */
#include "fck_aos.h"

#include "fck_ao.h"
#include "fck_plan.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "kode_sys.h"
#include "kode_log.h"

#include "fck_bsp.h"
#include "fck_drv_c5.h"
#include "fck_drv_cc1101.h"
#include "fck_drv_system.h"
#include "fck_drv_uart.h"

/* What the image should say: the firmwares' bytes survive a flash write only with the UART
 * interrupt in IRAM. J3's supply is switched by the Dot's C5 (CONFIG_KODEOS_RADIO), but the
 * SDK builds without it: J3 is then always powered (kode_sys_exp_power() answers KODE_DENIED)
 * and fck_end() cannot switch it off. The host tests are not the P4 and are left alone. */
#if defined(CONFIG_IDF_TARGET_ESP32P4)
#if !defined(CONFIG_UART_ISR_IN_IRAM)
#warning "CONFIG_UART_ISR_IN_IRAM=y is advised: without it the firmwares' bytes are lost while the flash is written"
#endif
#endif

#define TAG "fck"

#define LOOK_MS          300U   /* an empty header is looked at this often     */
#define FIND_MS          1000U  /* the bus is asked this long after the reset  */
#define FIND_GAP_MS      150U
#define FIND_TRIES       4U     /* probes per ask once the receiver is reset   */
#define ALIVE_MS         1000U
#define RELAY_WAIT_MS    7500U  /* the C5 has this long to say what it runs, to relay the GNSS again */
#define PATIENT_MS       600U
#define GNSS_CHECK_MS    (FCK_BSP_GNSS_SDA_BUSY_MS + 200U)
#define NRF_RESET_MS     20U
#define LOCK_MARGIN_MS   1000U
/* The longest any call waits: FCK_AOS_CALL_MS_MAX, in ticks well inside
 * reached()'s signed difference. A longer one is cut to it. */
#define CALL_MS_MAX      FCK_AOS_CALL_MS_MAX
/* What a caller waits for the owner's answer, past the request's own time
 * (a command's ms): behind a job of the worker (FCK_AOS_JOB_MS_MAX), for its
 * own job, and the grace for the owner's step. An answer that has not come by
 * then is a stuck owner, not a slow module: the caller gives up. */
#ifndef FCK_AOS_JOB_MS_MAX
#define FCK_AOS_JOB_MS_MAX 600000U
#endif
#ifndef FCK_AOS_ANSWER_GRACE_MS
#define FCK_AOS_ANSWER_GRACE_MS 2000U
#endif
/* How long fck_aos_start() waits for a start already under way. */
#define START_WAIT_MS    5000U
/* The longest command: what fck.h promises an app, under 256 bytes. */
/* What a restart waits for the module to park. The park itself is about
 * 100 ms, most of it the C5's 60 ms and the nRF's 20 ms in reset. */
#define PARK_WAIT_MS     250U
/* The command both firmwares answer with their banner, and how long after the
 * banner its #DONE may come: the firmwares print the two back to back, a few
 * milliseconds apart at their baud rates. */
#define INFO_VERB        "info"
#define INFO_GRACE_MS    300U
/* The tag a sentence of the GNSS receiver's comes with, and how often a caller
 * waiting for a fix looks whether one came. */
#define GNSS_TAG         "GNSSU"
#define FIX_POLL_MS      20U

#define BIT_PRESENT      (1U << 0)
/* Requests that wait for a job to end: more than the callers there are. */
#define DEFERRED_MAX     8U

#define SW0_MASK         ((uint8_t)FCK_BSP_EXPANDER_SW0_MASK)
#define SW0_REST         (FCK_BSP_LEG_SW0_REST != 0U)
#define P4_MASK          ((uint8_t)(1U << FCK_BSP_EXPANDER_HOST_WP_NRF))
#define P2_MASK          ((uint8_t)(1U << FCK_BSP_EXPANDER_GNSS_RST))
#define P6_MASK          ((uint8_t)(1U << FCK_BSP_EXPANDER_NRF_RST))

typedef enum {
    REQUEST_COMMAND,
    REQUEST_POWER,
    REQUEST_RESET,
    REQUEST_GNSS,
    REQUEST_RUN,
    REQUEST_LISTEN,
    REQUEST_FIRMWARE,
    REQUEST_STOP,
} request_kind_t;

/* What the worker is doing for the owner. */
typedef enum {
    JOB_NONE,
    JOB_LOOK,        /* look(): finding the module, setting it up            */
    JOB_GNSS_STOP,   /* gnss_hold(): the receiver to reset, the bus back      */
    JOB_RUN,         /* a caller's fck_aos_run_job()                          */
    JOB_RUN_WITHOUT_CHIPS, /* a caller's fck_aos_run_job_without_chips()      */
    JOB_LISTEN,      /* one slice of the sub-GHz listening, in the background */
} job_kind_t;

/* The sub-GHz listening, in the background: while it is in force and nothing else wants the
 * CC1101, the owner listens in slices on its worker and keeps the latest remote it heard. A slice
 * is short so that a request waits for it, at most, to run. */
#define LISTEN_SLICE_MS   200U
#define LISTEN_RETRY_MS   1000U   /* after a slice that could not run (no memory, no interrupt)  */
#define LISTEN_LOCK_MS    100U
#define LISTEN_RUNS       FCK_AOS_LISTEN_RUNS

/* The latest capture the listening kept; read by any task under s.lst_lock, written by the worker. */
typedef struct {
    uint32_t               seq;
    size_t                 n;
    fck_drv_cc1101_heard_t heard;
    int32_t                us[LISTEN_RUNS];
} listen_last_t;

/* What answered at 0x21: fck_plan.h's. */
typedef fck_plan_ident_t ident_t;
#define ID_FCK    FCK_PLAN_ID_FCK
#define ID_OTHER  FCK_PLAN_ID_OTHER
#define ID_UNSURE FCK_PLAN_ID_UNSURE

/* A request. The caller's is on its stack; what the owner is handed is a copy on the heap
 * (call()) with its own line and verb, and its reader behind reader_gate(), so that a caller
 * that gives up - a timeout, fck_cancel() - leaves nothing of its own for the owner to touch. */
struct fck_aos_request {
    request_kind_t    kind;
    fck_aos_chip_t    chip;
    const char       *line;
    const char       *verb;
    fck_drv_link_message_fn   on_reply;
    void             *ctx;
    uint32_t          ms;
    bool              flag;
    fck_aos_job_fn    job;
    void             *arg;
    bool              check;     /* sent only while the chip's banner is seq */
    uint32_t          seq;
    fck_aos_firmware_t  *firmware;     /* REQUEST_FIRMWARE's answer */
    kode_status_t     st;
    int32_t           rc;
    SemaphoreHandle_t done;
    bool              finished;  /* the owner answered (under s_mux)                  */
    bool              abandoned; /* the caller gave up, and the owner frees it (s_mux) */
    uint32_t          cancel_mark; /* s_cancel_gen when the call began: a job queued before a
                                    * fck_cancel() is not run                            */
    fck_drv_link_message_fn user_reply; /* the caller's reader, which on_reply gates          */
    void             *user_ctx;
    volatile bool     in_reader; /* the owner is inside user_reply                          */
    bool              internal;  /* the owner's own (the GNSS relay set again), or a command posted
                                  * with fck_aos_command_post(): nobody waits, the owner frees it
                                  * and notes how it went                                      */
    bool              relay_cmd; /* internal, and the GNSS relay's `gnss on`                    */
    const char       *why;       /* a non-OK answer's reason, a literal (fck_error())         */
    char              data[];    /* the heap copy's line and verb                           */
};

/* A command waiting for its link to be free, or in flight. */
typedef struct {
    fck_aos_request_t     *req;
    TickType_t deadline;
    bool       started;
    bool       heard;      /* the chip's banner came while it was in flight */
    TickType_t heard_at;
    bool       aborted;    /* the last command ran out; its #DONE is waited */
    TickType_t aborted_until; /* for until then, and given up on after   */
} flight_t;

static const uint8_t POSITIONS[FCK_BSP_POSITIONS] = {
    1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U, 9U, 10U, 11U, 12U, 13U, 14U,
};

/* With the C5's GNSS relay on, these arrive between any command's lines. */
static const char *const C5_ASYNC[] = { GNSS_TAG, "PPS", NULL };

/* What each chip greets with when it starts, and answers `info` with. */
static const char *const BANNER[FCK_AOS_CHIPS] = { "C5INFO", "NRFINFO" };

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

/* The GNSS relay set again after the module came back (the receiver is powered by
 * settings_restore(); the C5 is told `gnss on` when it has said what it runs). */
typedef enum {
    RELAY_NONE = 0,
    RELAY_WAIT,    /* the receiver runs; the C5's firmware is waited for        */
    RELAY_SENT,    /* `gnss on` is on its way                                */
    RELAY_OK,      /* it answered: told to the app at the next step          */
    RELAY_FAIL,    /* it did not (or the firmware is not the F*CK's): receiver off */
} relay_t;

static struct {
    bool               started;
    bool               starting;
    EventGroupHandle_t ev;
    SemaphoreHandle_t  chip_lock[FCK_AOS_CHIPS];   /* recursive: fck_aos_hold() */
    SemaphoreHandle_t  parked;
    StaticSemaphore_t  parked_buf;
    volatile bool      park_asked;
    bool               parked_for_good;   /* the P4 is restarting: nothing more */
    bool               stopped;           /* fck_aos_stop(): the module is let go; the
                                           * Active Object idles until a new start */
    volatile bool      leaving;           /* the app is going home, to sleep or to a
                                           * restart: any task sets it, and from then
                                           * on nothing new is started and a send in
                                           * the air is cut (fck_drv_cc1101_abort()) */

    fck_drv_pins_t     hw;
    fck_drv_tca6408a_t tca;
    int                uart_port[FCK_AOS_CHIPS]; /* kode_sys_exp_ctrl()'s, for good */
    int                uart[FCK_AOS_CHIPS];   /* the port while open, -1 closed */
    fck_drv_link_t         link[FCK_AOS_CHIPS];
    flight_t           flight[FCK_AOS_CHIPS];
    fck_drv_link_message_fn    listen_fn[FCK_AOS_CHIPS];
    void              *listen_ctx[FCK_AOS_CHIPS];
    fck_aos_firmware_t    firmware[FCK_AOS_CHIPS];  /* written by the owner under s_mux */

    fck_aos_status_t    state;      /* written by the owner under s_mux */
    bool               canary;
    uint32_t           misses;
    TickType_t         alive_at;
    bool               gnss_checking;
    TickType_t         gnss_check_at;

    /* The GNSS receiver's fix. The reader, and the sentences it has had
     * since the last fix, are the owner's task's alone; the fix itself is
     * copied under s_mux. */
    fck_drv_nmea_t         nmea;
    bool               nmea_gga;
    bool               nmea_rmc;
    bool               fix_have;
    fck_drv_nmea_fix_t     fix;
    uint32_t           fix_n;      /* fixes made since start */
    TickType_t         fix_at;     /* when the latest was completed: what its age counts from */

    /* What the looks saw; only the owner's task touches these. */
    bool               held;
    uint32_t           agreed;
    uint32_t           reask;
    bool               said;
    bool               troubled;
    bool               known;      /* an F*CK was identified, and the pair has
                                    * been held ever since: the same module */
    TickType_t         look_at;    /* the next look, with the header empty  */
    bool               ev_present; /* look() found the module: told when its job ends */
    uint32_t           ev_wrong;   /* look() ruled one out (FCK_AOS_SEEN_*), 0 for none */

    /* What the app set that the module forgets (F6). The wishes are kept across a module's
     * leaving to be set again when it is back; what is in force now is told apart. Under
     * s_mux: the worker sets the listening, the owner's task puts it out and in force. */
    bool               want_gnss;          /* the receiver was running when the module left     */
    bool               want_listen;        /* fck_aos_setting_listen(true, ...) and not off      */
    bool               listen_active;      /* ... and the module is there to listen with         */
    uint32_t           listen_hz;
    uint32_t           listen_gen;
    relay_t            relay;              /* only the owner's task                              */
    TickType_t         relay_until;
    bool               power_off[FCK_AOS_CHIPS];  /* the app switched the chip off (not the default)   */

    /* The worker's one job, and the requests that wait for its end. */
    job_kind_t         job;
    uint16_t           job_seq;
    fck_aos_request_t *job_request;
    fck_aos_job_context_t job_context;
    bool               job_chip_on[FCK_AOS_CHIPS];
    fck_aos_request_t *deferred[DEFERRED_MAX];
    size_t             deferred_n;
    /* The lines posted and not waited for (fck_aos_command_post()) that found a command in flight
     * on their chip: sent one at a time, in the order they came, as the chip is free. */
    fck_aos_request_t *posted[FCK_AOS_CHIPS][FCK_AOS_POSTED_MAX];
    size_t             posted_n[FCK_AOS_CHIPS];

    /* Resting (CONFIG_KODE_LOWPOWER): the module is put down for the board's rest and looked for
     * again on AWAKE. Unlike parked_for_good, it can be undone. */
    bool               rested;
    bool               rest_asked;     /* a rest began while a job ran: put down when it ends */
    /* J3's supply: asked at the owner's first tick, answered by EXP_POWER_SIG. */
    bool               power_todo;

    /* The listening in the background. The buffers are the heap's while the listening is in force. */
    fck_drv_cc1101_listen_t lst;
    uint32_t           lst_gen;        /* the listen_gen lst was started on                  */
    listen_last_t     *lst_last;       /* the latest capture, under lst_lock                 */
    int32_t           *lst_buf;        /* a slice's own runs, the worker's                   */
    SemaphoreHandle_t  lst_lock;
    TickType_t         lst_at;         /* the next slice not before                          */
    uint32_t           lst_seq;        /* captures kept; under lst_lock, read by the owner   */
} s = {
    .uart_port = { -1, -1 },
    .uart      = { -1, -1 },
};

static TickType_t ms_ticks(uint32_t ms)
{
    return pdMS_TO_TICKS(ms);
}

static bool reached(TickType_t when)
{
    return (int32_t)(xTaskGetTickCount() - when) >= 0;
}

/* ---- Why a call failed (fck_error()) ----------------------------------------------- */

/* The reason of each task's last failure, FCK_AOS_ERROR_TASKS tasks at most: a task is looked
 * up by its handle, one more takes the place of the oldest. The texts are literals, so a task reads
 * one long after it was set. A finished task's handle can be given to a new one, which would
 * read its last text: the table is cleared when the owner starts, and that is all. */
#define ERROR_TASKS FCK_AOS_ERROR_TASKS
static struct {
    TaskHandle_t  task;
    const char   *why;
    fck_aos_why_t cls;
    char          copy[FCK_AOS_ERROR_TEXT];   /* the one text that is composed: an Active Object's refusal */
} s_error[ERROR_TASKS];
static uint8_t s_error_next;

/* Whether @p text holds @p word. The texts are the SDK's own literals. */
static bool has(const char *text, const char *word)
{
    /* cppcheck-suppress misra-c2012-21.14 ; strstr() on the SDK's own literals, both terminated */
    return (text != NULL) && (strstr(text, word) != NULL);
}

/* The class of a failure, from its status and, for a refusal, from what its text says. The text
 * stays the authority (fck_error()); the class is what an answer to an Active Object carries. */
static fck_aos_why_t why_class(kode_status_t st, const char *text)
{
    switch (st) {
    case KODE_OK:
        return FCK_AOS_WHY_NONE;
    case KODE_TIMEOUT:
        return has(text, "cancelled") ? FCK_AOS_WHY_CANCELLED : FCK_AOS_WHY_TIMEOUT;
    case KODE_IO_ERROR:
        return has(text, "cancelled") ? FCK_AOS_WHY_CANCELLED : FCK_AOS_WHY_IO;
    case KODE_NO_RESOURCES:
        return FCK_AOS_WHY_BUSY;
    default:
        break;
    }
    /* By the words of the reason, the most specific first: a text that says two things (an argument
     * and a limit) is classed by the first of them. */
    fck_aos_why_t cls = FCK_AOS_WHY_REFUSED;
    if (has(text, "cancelled")) {
        cls = FCK_AOS_WHY_CANCELLED;
    } else if (has(text, "fck_begin() has not run")) {
        cls = FCK_AOS_WHY_NOT_BEGUN;
    } else if (has(text, "leaving") || has(text, "resting")) {
        cls = FCK_AOS_WHY_LEAVING;
    } else if (has(text, "no F*CK") || has(text, "no module") || has(text, "module left")) {
        cls = FCK_AOS_WHY_NO_MODULE;
    } else if (has(text, "chip is off") || has(text, "held in reset") || has(text, "in flight")
               || has(text, "holds the chip")) {
        cls = FCK_AOS_WHY_CHIP_HELD;
    } else if (has(text, "has not said") || has(text, "holds the bus")
               || has(text, "out of reach")) {
        cls = FCK_AOS_WHY_CHIP_NOT_READY;
    } else if (has(text, "firmware before") || has(text, "firmware is older") || has(text, "too old")
               || has(text, "lacks the command")) {
        cls = FCK_AOS_WHY_FIRMWARE_TOO_OLD;
    } else if (has(text, "rolling code") || has(text, "carrier would") || has(text, "air time")
               || has(text, "radio limits") || has(text, "duty") || has(text, "EU limits")
               /* older module firmwares word the refusal this way */
               || has(text, "CE limits")) {
        cls = FCK_AOS_WHY_BEYOND_LIMITS;
    } else if (has(text, "no listening") || has(text, "not on") || has(text, "nothing has come")
               || has(text, "receiver is off")) {
        cls = FCK_AOS_WHY_WRONG_ORDER;
    } else if (has(text, "NULL") || has(text, "is 0") || has(text, "above") || has(text, "below")
               || has(text, "not an") || has(text, "out of range") || has(text, "is not")) {
        cls = FCK_AOS_WHY_BAD_ARGUMENT;
    } else {
        cls = FCK_AOS_WHY_REFUSED;
    }
    return cls;
}

static void error_put(const char *why, fck_aos_why_t cls)
{
    TaskHandle_t const me = xTaskGetCurrentTaskHandle();
    taskENTER_CRITICAL(&s_mux);
    unsigned slot = ERROR_TASKS;
    for (unsigned i = 0U; i < ERROR_TASKS; i++) {
        if (s_error[i].task == me) {
            slot = i;
            break;
        }
    }
    for (unsigned i = 0U; slot == ERROR_TASKS && i < ERROR_TASKS; i++) {
        slot = (s_error[i].task == NULL) ? i : ERROR_TASKS;
    }
    if (slot == ERROR_TASKS) {
        slot         = s_error_next;
        s_error_next = (uint8_t)((s_error_next + 1U) % ERROR_TASKS);
    }
    s_error[slot].task = me;
    s_error[slot].why  = (why != NULL) ? why : "";
    s_error[slot].cls  = cls;
    taskEXIT_CRITICAL(&s_mux);
}

void fck_aos_error_set(const char *why)
{
    error_put(why, why_class(KODE_DENIED, why));
}

void fck_aos_error_ok(void)
{
    error_put("", FCK_AOS_WHY_NONE);
}

/* The refusal of a call that blocks, made from an Active Object: the text names the call and the
 * Active Object, so it is composed, and kept in the task's own slot until its next failure. */
static void error_put_ao(const char *call_name, const char *ao_name)
{
    char text[FCK_AOS_ERROR_TEXT];
    (void)snprintf(text, sizeof(text),
                   "%s was called from an Active Object (%s); fck.h calls block - call fck_sys_*() instead",
                   (call_name != NULL) ? call_name : "an fck_*() call", (ao_name != NULL) ? ao_name : "?");
    error_put("", FCK_AOS_WHY_AO_CALLER);
    TaskHandle_t const me = xTaskGetCurrentTaskHandle();
    taskENTER_CRITICAL(&s_mux);
    for (unsigned i = 0U; i < ERROR_TASKS; i++) {
        if (s_error[i].task == me) {
            (void)memcpy(s_error[i].copy, text, sizeof(text));
            s_error[i].why = s_error[i].copy;
        }
    }
    taskEXIT_CRITICAL(&s_mux);
}

void fck_aos_error_set_for(kode_status_t st, const char *why)
{
    error_put(why, why_class(st, why));
}

fck_aos_why_t fck_aos_error_why(void)
{
    TaskHandle_t const me  = xTaskGetCurrentTaskHandle();
    fck_aos_why_t      cls = FCK_AOS_WHY_NONE;
    taskENTER_CRITICAL(&s_mux);
    for (unsigned i = 0U; i < ERROR_TASKS; i++) {
        if (s_error[i].task == me) {
            cls = s_error[i].cls;
        }
    }
    taskEXIT_CRITICAL(&s_mux);
    return cls;
}

const char *fck_aos_error(void)
{
    TaskHandle_t const me  = xTaskGetCurrentTaskHandle();
    const char        *why = "";
    taskENTER_CRITICAL(&s_mux);
    for (unsigned i = 0U; i < ERROR_TASKS; i++) {
        if (s_error[i].task == me) {
            why = s_error[i].why;
        }
    }
    taskEXIT_CRITICAL(&s_mux);
    return why;
}

static void error_clear(void)
{
    taskENTER_CRITICAL(&s_mux);
    (void)memset(s_error, 0, sizeof(s_error));
    s_error_next = 0U;
    taskEXIT_CRITICAL(&s_mux);
}

/* Records the reason for the calling task and hands the status back. */
static kode_status_t fail(kode_status_t st, const char *why)
{
    fck_aos_error_set_for(st, why);
    return st;
}

/* What a code says when nothing said more. */
static const char *why_of(kode_status_t st)
{
    switch (st) {
    case KODE_DENIED:
        return "refused: no module, the chip is off, or the call is not valid now";
    case KODE_NO_RESOURCES:
        return "no memory, or no room in the owner's queue";
    case KODE_TIMEOUT:
        return "no answer in the time given";
    case KODE_IO_ERROR:
        return "the module left, lost power or the chip restarted on the way";
    case KODE_EXISTS:
        return "the priority or the name is taken";
    default:
        return "the call failed";
    }
}

/* The reason the owner's own steps give for the request they are serving (chip_power(),
 * chip_reset()), taken by finish(). Only the owner's task writes it. */
static const char *s_why;

static void owner_why(const char *text)
{
    s_why = text;
}

/* ---- Cancelling ---------------------------------------------------------------------- */

/* How often a waiting caller looks at fck_cancel(): a wait is cut into steps of this. */
#define CANCEL_STEP_MS 20U

/* fck_cancel() counts. A call remembers the count when it begins and is cancelled if the
 * count has moved on: a call begun after it is not touched. */
static volatile uint32_t s_cancel_gen;
/* The count when the task that holds a chip (fck_aos_hold()) took it: every call it makes while
 * it holds is one operation, cancelled together. */
static uint32_t s_hold_mark[FCK_AOS_CHIPS];

uint32_t fck_aos_cancel_mark(void)
{
    return s_cancel_gen;
}

bool fck_aos_cancelled(uint32_t mark)
{
    return s_cancel_gen != mark;
}

kode_status_t fck_aos_cancel(void)
{
    if (!s.started) {
        return fail(KODE_DENIED, "fck_begin() has not run, or fck_end() was called");
    }
    taskENTER_CRITICAL(&s_mux);
    s_cancel_gen++;
    taskEXIT_CRITICAL(&s_mux);
    /* What runs on the worker is ended at its next step; the job after it clears this (run_job()). */
    fck_drv_cc1101_abort();
    return KODE_OK;
}

/* ---- Events -------------------------------------------------------------------------- */

#define EV_SLOW_MS 10U        /* a sink that takes longer than this is logged (one in 100) */

static uint32_t s_ev_slow;    /* sinks over EV_SLOW_MS, to say so once in a hundred */

/* Who is told, for good once a layer has begun: set before the first event that matters and
 * never taken back, so the owner reads it without a lock but for the pointers themselves. */
static fck_aos_sink_fn s_sink_fn[FCK_AOS_SINKS];
static void           *s_sink_ctx[FCK_AOS_SINKS];
static uint32_t        s_sink_dropped[FCK_AOS_SINKS];

void fck_aos_sink_set(fck_aos_sink_t which, fck_aos_sink_fn fn, void *ctx)
{
    if ((unsigned)which >= (unsigned)FCK_AOS_SINKS) {
        return;
    }
    taskENTER_CRITICAL(&s_mux);
    s_sink_fn[which]  = fn;
    s_sink_ctx[which] = ctx;
    taskEXIT_CRITICAL(&s_mux);
}

/* Tell every sink what happened, on the calling task. A sink is short and never blocks: it
 * queues the event or posts it to an Active Object, and says false when it had no room. */
void fck_aos_news(fck_aos_event_t ev, uint32_t arg)
{
    for (unsigned w = 0U; w < (unsigned)FCK_AOS_SINKS; w++) {
        taskENTER_CRITICAL(&s_mux);
        fck_aos_sink_fn const fn  = s_sink_fn[w];
        void *const           ctx = s_sink_ctx[w];
        taskEXIT_CRITICAL(&s_mux);
        if (fn == NULL) {
            continue;
        }
        uint32_t const t0    = kode_sys_time_ms();
        bool const     taken = fn(ev, arg, ctx);
        uint32_t const took  = kode_sys_time_ms() - t0;
        if (!taken) {
            taskENTER_CRITICAL(&s_mux);
            s_sink_dropped[w]++;
            taskEXIT_CRITICAL(&s_mux);
        }
        /* The task that told it waits for the sink: it only queues or posts. */
        if ((took > EV_SLOW_MS) && ((s_ev_slow++ % 100U) == 0U)) {
            KLOG_W(TAG, "the event sink %u took %u ms (event %u): it must only queue or post",
                   w, (unsigned)took, (unsigned)ev);
        }
    }
}

void fck_aos_dropped_add(fck_aos_sink_t which, uint32_t n)
{
    if ((unsigned)which >= (unsigned)FCK_AOS_SINKS) {
        return;
    }
    taskENTER_CRITICAL(&s_mux);
    s_sink_dropped[which] += n;
    taskEXIT_CRITICAL(&s_mux);
}

static void emit(fck_aos_event_t ev, uint32_t arg)
{
    fck_aos_news(ev, arg);
}

/* The reader of a command whose caller may give up: it is called only while the caller
 * waits, and the caller, leaving, waits for it to return (call()). */
static void reader_gate(void *ctx, const fck_drv_link_message_t *m)
{
    fck_aos_request_t *const r = (fck_aos_request_t *)ctx;
    taskENTER_CRITICAL(&s_mux);
    bool const live = !r->abandoned;
    if (live) {
        r->in_reader = true;
    }
    taskEXIT_CRITICAL(&s_mux);
    if (live) {
        r->user_reply(r->user_ctx, m);
        r->in_reader = false;
    }
}

/* The owner's answer. The request is on the heap, the caller's copy of what it
 * asked: one whose caller has given up is the owner's to free, and nothing of
 * it is touched after this call. */
static void finish(fck_aos_request_t *r, kode_status_t st)
{
    r->st = st;
    if (r->why == NULL) {
        r->why = s_why;
    }
    s_why = NULL;
    if (r->internal && !r->relay_cmd && r->kind == REQUEST_COMMAND) {
        /* Posted and not waited for: the log is the only place its end is told. */
        if (st != KODE_OK) {
            KLOG_W(TAG, "a posted `%s` failed: %s", r->verb, (r->why != NULL) ? r->why : "no reason noted");
        }
        if ((st == KODE_OK) && (r->rc != 0)) {
            KLOG_W(TAG, "a posted `%s` ended with rc=%d", r->verb, (int)r->rc);
        }
    }
    if (r->internal && r->relay_cmd && r->kind == REQUEST_COMMAND) {
        /* The GNSS relay's `gnss on`: the owner notes how it went at its next step. */
        s.relay = (st == KODE_OK && fck_drv_c5_status(r->rc) == KODE_OK) ? RELAY_OK : RELAY_FAIL;
    }
    taskENTER_CRITICAL(&s_mux);
    r->finished         = true;
    bool const orphaned = r->abandoned || r->internal;
    taskEXIT_CRITICAL(&s_mux);
    if (orphaned) {
        vSemaphoreDelete(r->done);
        /* cppcheck-suppress misra-c2012-21.3 ; the request call() allocated for a caller that gave up */
        free(r);
    } else {
        (void)xSemaphoreGive(r->done);
    }
}

static void finish_why(fck_aos_request_t *r, kode_status_t st, const char *text)
{
    r->why = text;
    finish(r, st);
}

static void firmware_copy(fck_aos_chip_t chip, fck_aos_firmware_t *out)
{
    taskENTER_CRITICAL(&s_mux);
    *out = s.firmware[chip];
    taskEXIT_CRITICAL(&s_mux);
}

/* REQUEST_FIRMWARE's end: what the chip said, if it did. */
static void finish_firmware(fck_aos_request_t *r)
{
    bool known = false;
    taskENTER_CRITICAL(&s_mux);
    known = s.firmware[r->chip].known;
    if (!r->abandoned && !r->internal) {
        *r->firmware = s.firmware[r->chip];     /* the caller's own memory: only while it waits */
    }
    taskEXIT_CRITICAL(&s_mux);
    finish_why(r, known ? KODE_OK : KODE_TIMEOUT, "the chip has not said what it runs");
}

/* A command that ended as commands do: its #DONE, or `info`'s banner. */
static void finish_done(fck_aos_request_t *r)
{
    if (r->kind == REQUEST_FIRMWARE) {
        finish_firmware(r);
    } else {
        finish(r, KODE_OK);
    }
}

static bool expander_usable(void)
{
    return fck_drv_tca6408a_is_open(&s.tca) && !s.gnss_checking && !s.state.gnss_holds_bus;
}

static void chip_on_set(fck_aos_chip_t chip, bool on)
{
    taskENTER_CRITICAL(&s_mux);
    s.state.chip_on[chip] = on && (s.uart[chip] >= 0);
    taskEXIT_CRITICAL(&s_mux);
}

/* ---- The GNSS receiver's fix -------------------------------------------------------- */

/* NOTHING HERE IS LOGGED, at any level: a position says where a person is, and
 * so does the sentence it came in. Only the owner's task calls these. */

/* No fix, and no sentence waiting for its pair. */
static void fix_reset(void)
{
    fck_drv_nmea_init(&s.nmea);
    s.nmea_gga = false;
    s.nmea_rmc = false;
    taskENTER_CRITICAL(&s_mux);
    s.fix_have = false;
    taskEXIT_CRITICAL(&s_mux);
}

/* One sentence the C5 relayed. A fix is a GGA and an RMC read since the
 * previous fix; the reader keeps the latest of each, field by field. */
static void fix_feed(const char *sentence)
{
    switch (fck_drv_nmea_feed(&s.nmea, sentence)) {
    case FCK_DRV_NMEA_GGA:
        s.nmea_gga = true;
        break;
    case FCK_DRV_NMEA_RMC:
        s.nmea_rmc = true;
        break;
    default:
        return;
    }
    if (s.nmea_gga && s.nmea_rmc) {
        s.nmea_gga = false;
        s.nmea_rmc = false;
        TickType_t const now = xTaskGetTickCount();
        taskENTER_CRITICAL(&s_mux);
        s.fix      = s.nmea.fix;
        s.fix_have = true;
        s.fix_n++;
        s.fix_at   = now;
        taskEXIT_CRITICAL(&s_mux);
    }
}

/* ---- The links ------------------------------------------------------------------ */

static void on_async(void *ctx, const fck_drv_link_message_t *m)
{
    fck_aos_chip_t const chip = (fck_aos_chip_t)(uintptr_t)ctx;
    /* Read first, and handed on after: whoever listens gets the line too. A
     * receiver the owner did not start is not read, and a late sentence of
     * one it has stopped is dropped. */
    if (chip == FCK_AOS_C5 && s.state.gnss_running && strcmp(m->tag, GNSS_TAG) == 0) {
        fix_feed(m->body);
    }
    taskENTER_CRITICAL(&s_mux);
    fck_drv_link_message_fn const fn  = s.listen_fn[chip];
    void *const           arg = s.listen_ctx[chip];
    taskEXIT_CRITICAL(&s_mux);
    if (fn != NULL) {
        fn(arg, m);
    }
}

/* A chip's banner: what it runs, kept until the chip is reset, powered,
 * handed to a job or set up again. Each one moves seq on, so a line judged
 * by the last is not sent to what may now be another image - but for the
 * answer to an `info` that says the same image again: nothing changed, and
 * a caller that judged before it need not judge again. Only the owner's
 * task writes s.firmware, so reading it here takes no lock. */
static void on_banner(void *ctx, const fck_drv_link_message_t *m)
{
    fck_aos_chip_t const chip = (fck_aos_chip_t)(uintptr_t)ctx;
    size_t const         n    = strlen(m->body);
    flight_t *const      f    = &s.flight[chip];
    bool const           answer
        = f->req != NULL && f->started && strcmp(f->req->verb, INFO_VERB) == 0;
    bool const same = answer && s.firmware[chip].known && n < sizeof(s.firmware[chip].body)
                      && fck_drv_link_same_banner(s.firmware[chip].body, m->body);
    taskENTER_CRITICAL(&s_mux);
    fck_aos_firmware_t *const a = &s.firmware[chip];
    a->known = true;
    if (!same) {
        a->seq++;
    }
    if (n < sizeof(a->body)) {
        memcpy(a->body, m->body, n + 1U);
    } else {
        a->body[0] = '\0';     /* not kept half: nothing is read from it */
    }
    taskEXIT_CRITICAL(&s_mux);
    if (f->req != NULL && f->started && !f->heard) {
        f->heard    = true;
        f->heard_at = xTaskGetTickCount();
    }
}

static void firmware_forget(fck_aos_chip_t chip)
{
    taskENTER_CRITICAL(&s_mux);
    s.firmware[chip].known   = false;
    s.firmware[chip].seq++;
    s.firmware[chip].body[0] = '\0';
    taskEXIT_CRITICAL(&s_mux);
}

static void links_init(void)
{
    for (int chip = 0; chip < FCK_AOS_CHIPS; chip++) {
        void *const          ctx = (void *)(uintptr_t)chip;
        const fck_drv_link_config_t cfg = {
            .async       = (chip == FCK_AOS_C5) ? C5_ASYNC : NULL,
            .on_async    = on_async,
            .async_ctx   = ctx,
            .banner      = BANNER[chip],
            .banner_verb = INFO_VERB,
            .on_banner   = on_banner,
            .banner_ctx  = ctx,
        };
        (void)fck_drv_link_init(&s.link[chip], &cfg);
        firmware_forget((fck_aos_chip_t)chip);
    }
}

/* The chip behind the link was reset, powered or handed to a job: what it
 * was saying, and what it said it runs, are gone with it. */
static void link_drop(fck_aos_chip_t chip)
{
    fck_drv_link_reset(&s.link[chip]);
    s.flight[chip].aborted = false;
    fck_drv_uart_flush(s.uart[chip]);
    firmware_forget(chip);
    if (chip == FCK_AOS_C5) {
        /* Its relay was off from the reset, so no fix will follow the last. */
        fix_reset();
    }
}

static void pump_chip(fck_aos_chip_t chip)
{
    uint8_t buf[128];
    size_t  n;
    while ((n = fck_drv_uart_read(s.uart[chip], buf, sizeof(buf))) > 0U) {
        fck_drv_link_feed(&s.link[chip], buf, n);
    }
}

static void pump(void)
{
    for (int chip = 0; chip < FCK_AOS_CHIPS; chip++) {
        pump_chip((fck_aos_chip_t)chip);
    }
}

/* When a command accepted now on @p chip runs out if it is never sent: its own time, after
 * the tail of a command that ran out, which try_start() waits for (and gives up on at
 * aborted_until). Once the line is written, try_start() counts its time again from then:
 * a frames listen cancelled at 5 s must not eat the 250 ms of the next one. */
static TickType_t accept_deadline(fck_aos_chip_t chip, uint32_t ms)
{
    flight_t const *const f    = &s.flight[chip];
    TickType_t const      now  = xTaskGetTickCount();
    TickType_t            tail = 0U;
    if (f->aborted && !reached(f->aborted_until)) {
        tail = f->aborted_until - now;
    }
    return now + tail + ms_ticks(ms);
}

/* A command waits here while its link is still busy with the tail of one
 * that timed out, whose late lines fck_drv_link_abort() drops. The chip's
 * banner, if it restarted, ends the wait; otherwise the firmware is given as
 * long again as the command had, and then its #DONE is no longer waited
 * for: one that never comes - a command that ends otherwise than its verb
 * says - must not shut the chip off for good. fck_aos_reset() is the way
 * out of a firmware that hung. */
static void try_start(fck_aos_chip_t chip)
{
    flight_t *const f = &s.flight[chip];
    if (f->req == NULL || f->started) {
        return;
    }
    fck_aos_request_t *const                 r    = f->req;
    if (r->abandoned) {
        f->req = NULL;     /* its caller is gone with the line and the reader it lent */
        finish_why(r, KODE_TIMEOUT, "the caller gave up");
        return;
    }
    fck_aos_firmware_t const *const a    = &s.firmware[chip];
    bool                         idle = fck_drv_link_idle(&s.link[chip]);
    if (!idle && f->aborted && reached(f->aborted_until)) {
        KLOG_W(TAG, "the %s never closed a command that ran out: no longer waited for",
               (chip == FCK_AOS_C5) ? "C5" : "nRF");
        fck_drv_link_forget(&s.link[chip]);
        idle = true;
    }
    if (idle) {
        f->aborted = false;
    }
    if (idle) {
        /* What arrived before the command is not its answer. */
        pump_chip(chip);
    }
    if (r->kind == REQUEST_FIRMWARE && a->known) {
        f->req = NULL;     /* it said by itself: nothing to ask */
        finish_firmware(r);
        return;
    }
    if (!idle) {
        return;
    }
    /* Judged by what the chip runs: sent only if that is still what it runs. */
    if (r->check && (!a->known || a->seq != r->seq)) {
        f->req = NULL;
        finish_why(r, KODE_DENIED, "the chip no longer runs the firmware the line was judged by");
        return;
    }
    if (fck_drv_link_begin(&s.link[chip], r->verb, r->on_reply, r->ctx) != KODE_OK) {
        f->req = NULL;
        finish_why(r, KODE_DENIED, "the chip's link was busy with an earlier command");
        return;
    }
    /* An empty line first: whatever the chip kept of a line that never ended - the Dot reset half
     * way through writing one - ends there, under its own verb, instead of being glued to this
     * command. Both firmwares say nothing to an empty line.
     * fck_drv_c5's builders end their lines already; a caller's own may not. */
    size_t const n     = strlen(r->line);
    bool const   ended = (n > 0U) && (r->line[n - 1U] == '\n');
    if (fck_drv_uart_write(s.uart[chip], "\n", 1U) != KODE_OK
        || fck_drv_uart_write(s.uart[chip], r->line, n) != KODE_OK
        || (!ended && fck_drv_uart_write(s.uart[chip], "\r\n", 2U) != KODE_OK)) {
        /* Never sent, so no #DONE of it will come to wait for. */
        fck_drv_link_reset(&s.link[chip]);
        f->req = NULL;
        finish_why(r, KODE_IO_ERROR, "the line could not be written to the chip's UART");
        return;
    }
    f->started  = true;
    f->heard    = false;
    f->deadline = xTaskGetTickCount() + ms_ticks(r->ms); /* its time counts from the line written */
}

/* The request in flight on @p chip, if its caller has given up (a timeout past every
 * bound, or fck_cancel()): nothing of it may be touched any more - its reader and the
 * memory it points to are gone - so the line is let go of. The chip goes on with the
 * command it has until its #DONE, which is waited for as long as the command had. */
static void drop_abandoned(fck_aos_chip_t chip)
{
    flight_t *const           f = &s.flight[chip];
    fck_aos_request_t *const  r = f->req;
    if (r == NULL || !r->abandoned) {
        return;
    }
    fck_drv_link_t *const link = &s.link[chip];
    if (f->started) {
        if (strcmp(r->verb, INFO_VERB) == 0) {
            fck_drv_link_forget(link);
        } else {
            fck_drv_link_abort(link);
            f->aborted       = true;
            f->aborted_until = xTaskGetTickCount() + ms_ticks(r->ms);
        }
    }
    f->req = NULL;
    finish_why(r, KODE_TIMEOUT, "the caller gave up");
}

static void handle(fck_aos_request_t *r);

/* A line posted and not waited for whose chip has a command in flight waits its turn here. One a
 * caller waits for does not: it is told the chip is busy. */
static bool posted_wait(fck_aos_request_t *r)
{
    if (!r->internal || r->relay_cmd || s.posted_n[r->chip] >= FCK_AOS_POSTED_MAX) {
        return false;
    }
    s.posted[r->chip][s.posted_n[r->chip]] = r;
    s.posted_n[r->chip]++;
    return true;
}

/* The chip is free: the posted line that waited longest is judged again as one arriving now - the
 * chip may be off since, or the app leaving - and sent. */
static void posted_next(fck_aos_chip_t chip)
{
    if (s.flight[chip].req != NULL || s.posted_n[chip] == 0U) {
        return;
    }
    fck_aos_request_t *const r = s.posted[chip][0];
    s.posted_n[chip]--;
    memmove(&s.posted[chip][0], &s.posted[chip][1], s.posted_n[chip] * sizeof(s.posted[chip][0]));
    handle(r);
}

/* `info` costs a firmware nothing, and its late answer is only a banner, so
 * one that ran out is forgotten rather than abandoned: an `info` written
 * while the chip was starting is never heard, and its #DONE never comes. */
static void flights(void)
{
    for (int chip = 0; chip < FCK_AOS_CHIPS; chip++) {
        flight_t *const f = &s.flight[chip];
        posted_next((fck_aos_chip_t)chip);
        if (f->req == NULL) {
            continue;
        }
        try_start((fck_aos_chip_t)chip);
        if (f->req == NULL) {
            continue;
        }
        fck_aos_request_t *const      r    = f->req;
        fck_drv_link_t *const link = &s.link[chip];
        if (r->abandoned) {
            drop_abandoned((fck_aos_chip_t)chip);
            continue;
        }
        bool const        info = (strcmp(r->verb, INFO_VERB) == 0);
        int32_t           rc   = -1;
        if (f->started && fck_drv_link_done(link, &rc)) {
            f->req = NULL;
            if (fck_drv_link_restarted(link)) {
                KLOG_W(TAG, "the %s restarted in the middle of `%s`",
                       (chip == FCK_AOS_C5) ? "C5" : "nRF", r->verb);
                finish_why(r, KODE_IO_ERROR, "the chip restarted in the middle of the command");
            } else {
                r->rc = rc;
                finish_done(r);
            }
        } else if (f->started && info && f->heard
                   && reached(f->heard_at + ms_ticks(INFO_GRACE_MS))) {
            /* The banner came and no #DONE after it: the chip was starting,
             * and the banner was its own, not the answer to an `info` it
             * never heard. */
            fck_drv_link_forget(link);
            f->req = NULL;
            finish_done(r);
        } else if (reached(f->deadline)) {
            if (f->started && info) {
                fck_drv_link_forget(link);
            } else if (f->started) {
                fck_drv_link_abort(link);
                f->aborted       = true;
                f->aborted_until = xTaskGetTickCount() + ms_ticks(r->ms);
            }
            f->req = NULL;
            if (r->kind == REQUEST_FIRMWARE) {
                finish_firmware(r);
            } else {
                finish_why(r, KODE_TIMEOUT, "the chip did not answer the command in the time given");
            }
        }
    }
}

static void flights_fail(kode_status_t st)
{
    for (int chip = 0; chip < FCK_AOS_CHIPS; chip++) {
        if (s.flight[chip].req != NULL) {
            fck_aos_request_t *const r = s.flight[chip].req;
            s.flight[chip].req = NULL;
            finish_why(r, st, "the module left J3 or lost power on the way");
        }
        while (s.posted_n[chip] > 0U) {
            s.posted_n[chip]--;
            finish_why(s.posted[chip][s.posted_n[chip]], st, "the module left J3 or lost power on the way");
        }
    }
}

/* ---- Finding the module, and losing it --------------------------------------------- */

static void j3_set(uint8_t v)
{
    taskENTER_CRITICAL(&s_mux);
    s.state.j3_supply = v;
    taskEXIT_CRITICAL(&s_mux);
}

static void state_set_present(bool present)
{
    fix_reset();
    taskENTER_CRITICAL(&s_mux);
    s.state.present = present;
    if (present) {
        s.state.generation++;
    }
    s.state.gnss_running   = false;
    s.state.gnss_holds_bus = false;
    taskEXIT_CRITICAL(&s_mux);
    if (present) {
        (void)xEventGroupSetBits(s.ev, BIT_PRESENT);
    } else {
        (void)xEventGroupClearBits(s.ev, BIT_PRESENT);
    }
}

/* The positions are named after the chip's pins: the P4 transmits on the
 * chip's RX and listens on its TX. */
static int uart_open(fck_aos_chip_t chip, uint8_t chip_rx, uint8_t chip_tx, uint32_t baud)
{
    int const port = s.uart_port[chip];
    return (fck_drv_uart_open(port, fck_drv_pins_gpio(&s.hw, chip_rx),
                              fck_drv_pins_gpio(&s.hw, chip_tx), baud) == KODE_OK) ? port : -1;
}

/* The receiver to reset through the expander; by hand when its firmware has
 * taken SDA and the expander cannot be reached. */
static kode_status_t gnss_hold(void)
{
    if (fck_drv_tca6408a_is_open(&s.tca) && fck_drv_tca6408a_mask(&s.tca, P2_MASK, true, false) == KODE_OK) {
        return KODE_OK;
    }
    fck_drv_tca6408a_blind_gnss_reset(&s.tca, &s.hw);
    return fck_drv_tca6408a_mask_patient(&s.tca, &s.hw, P2_MASK, true, false, PATIENT_MS);
}

/* Something answered at 0x21: is it the expander? One read, of INPUT, and no
 * register written: the register's number is the only byte the chip is sent.
 * A register-less chip (a PCF8574) takes that byte for its port and drives
 * every pin low, so it reads 0x00 back; the expander reads its pins, and five
 * of them are held high by the module's resistors (FCK_BSP_EXPANDER_PULLUPS_MASK).
 * A chip that keeps registers as the expander does cannot be told from it by
 * anything on the bus, and passes. */
static ident_t identify(void)
{
    uint8_t v = 0U;
    if (fck_drv_tca6408a_read(&s.tca, FCK_BSP_EXPANDER_REGISTER_INPUT, &v) != KODE_OK) {
        return ID_UNSURE;
    }
    return fck_plan_identify(v);
}

/* The power-loss mark, written to POLARITY and read back (Kode's test firmware
 * checks POLARITY the same way, with 0xA5). On the expander POLARITY
 * only inverts how inputs read: no pin moves. The first write the chip is
 * given, and so the proof that it keeps a register at all. */
static ident_t mark_canary(void)
{
    uint8_t const mark = (uint8_t)FCK_BSP_EXPANDER_CANARY_BIT;
    uint8_t       v    = 0U;
    if (fck_drv_tca6408a_write(&s.tca, FCK_BSP_EXPANDER_REGISTER_POLARITY, mark) != KODE_OK
        || fck_drv_tca6408a_read(&s.tca, FCK_BSP_EXPANDER_REGISTER_POLARITY, &v) != KODE_OK) {
        return ID_UNSURE;
    }
    return fck_plan_canary_kept(mark, v);
}

/* SW0 driven to the level it rests at, and read back into @p input; @p at_rest
 * says whether the pin reads it. The status is the bus's. @p patient asks
 * again over PATIENT_MS, for a bus a firmware has just let go. */
static kode_status_t sw0_rest(bool patient, uint8_t *input, bool *at_rest)
{
    uint8_t const mask = SW0_MASK;
    kode_status_t st   = patient
        ? fck_drv_tca6408a_mask_patient(&s.tca, &s.hw, mask, true, SW0_REST, PATIENT_MS)
        : fck_drv_tca6408a_mask(&s.tca, mask, true, SW0_REST);
    if (st == KODE_OK) {
        st = fck_drv_tca6408a_read(&s.tca, FCK_BSP_EXPANDER_REGISTER_INPUT, input);
    }
    uint8_t const got  = *input & mask;
    uint8_t const want = SW0_REST ? mask : 0U;
    *at_rest           = (st == KODE_OK) && (got == want);
    return st;
}

/* The expander is identified, and keeps the power-loss mark: everything else,
 * in the order fck_aos.h gives. */
static void setup(void)
{
    if (gnss_hold() != KODE_OK) {
        KLOG_W(TAG, "the GNSS receiver's reset (P2) was not confirmed: it may be running");
    }
    uint8_t input   = 0U;
    bool    at_rest = false;
    if ((sw0_rest(true, &input, &at_rest) != KODE_OK) || !at_rest) {
        KLOG_W(TAG, "the antenna switch's control pin (P3) does not read at rest "
                    "(input 0x%02X): it may float", (unsigned)input);
    }
    /* This SDK writes SW0 to P3 alone, and P4, the nRF's wake line, goes
     * back to R703. */
    if (fck_drv_tca6408a_mask(&s.tca, P4_MASK, false, true) != KODE_OK) {
        KLOG_W(TAG, "the nRF's wake line (P4) could not be let go");
    }
    /* A firmware before this one may have left the nRF held in reset, and
     * the expander kept it through every restart since: let it run. */
    if (fck_drv_tca6408a_mask(&s.tca, P6_MASK, false, true) != KODE_OK) {
        KLOG_W(TAG, "the nRF's reset (P6) could not be let go");
    }
    /* look() left the power-loss mark in POLARITY, and read it back. */
    s.canary = true;

    uint8_t             part = 0xFFU;
    uint8_t             ver  = 0xFFU;
    kode_status_t const cc   = fck_drv_cc1101_sleep(&s.hw, &part, &ver);
    if (cc == KODE_OK) {
        KLOG_I(TAG, "CC1101 found (PARTNUM 0x%02X, VERSION 0x%02X), reset and powered down",
               (unsigned)part, (unsigned)ver);
    } else {
        KLOG_E(TAG, "the CC1101 did not answer (%d): PARTNUM 0x%02X, VERSION 0x%02X",
               (int)cc, (unsigned)part, (unsigned)ver);
    }

    s.uart[FCK_AOS_C5]  = uart_open(FCK_AOS_C5, FCK_BSP_POSITION_C5_RX, FCK_BSP_POSITION_C5_TX,
                                    FCK_BSP_C5_BAUD);
    s.uart[FCK_AOS_NRF] = uart_open(FCK_AOS_NRF, FCK_BSP_POSITION_NRF_RX, FCK_BSP_POSITION_NRF_TX,
                                    FCK_BSP_NRF_BAUD);
    for (int chip = 0; chip < FCK_AOS_CHIPS; chip++) {
        if (s.uart[chip] < 0) {
            KLOG_E(TAG, "the %s's UART%d could not be opened: it cannot be reached",
                   (chip == FCK_AOS_C5) ? "C5" : "nRF", s.uart_port[chip]);
        }
    }
    links_init();

    taskENTER_CRITICAL(&s_mux);
    s.state.cc1101_ok      = (cc == KODE_OK);
    s.state.cc1101_part    = part;
    s.state.cc1101_version = ver;
    taskEXIT_CRITICAL(&s_mux);
    chip_on_set(FCK_AOS_C5, true);
    chip_on_set(FCK_AOS_NRF, true);

    s.misses        = 0U;
    s.alive_at      = xTaskGetTickCount();
    s.gnss_checking = false;
    state_set_present(true);
    s.ev_present = true;   /* told on the owner's task, when this job ends */
    KLOG_I(TAG, "F*CK found: expander at 0x%02X on GPIO%d (SCL) and GPIO%d (SDA), "
                "GNSS held in reset, C5 on UART%d, nRF on UART%d",
           (unsigned)FCK_BSP_EXPANDER_ADDRESS, fck_drv_pins_gpio(&s.hw, FCK_BSP_POSITION_I2C_SCL),
           fck_drv_pins_gpio(&s.hw, FCK_BSP_POSITION_I2C_SDA), s.uart[FCK_AOS_C5], s.uart[FCK_AOS_NRF]);
}

/* The module let go of: the requests in flight failed, the UARTs and the bus
 * closed, the pins at rest, and what was known of it forgotten. */
static void release_module(void)
{
    flights_fail(KODE_IO_ERROR);
    s.relay = RELAY_NONE;
    for (int chip = 0; chip < FCK_AOS_CHIPS; chip++) {
        fck_drv_uart_close(s.uart[chip]);
        s.uart[chip] = -1;
        fck_drv_link_reset(&s.link[chip]);
        firmware_forget((fck_aos_chip_t)chip);
    }
    (void)fck_drv_tca6408a_close(&s.tca);
    fck_drv_tca6408a_forget(&s.tca);
    fck_drv_pins_rest(&s.hw);
    s.canary        = false;
    s.power_off[FCK_AOS_C5]  = false;
    s.power_off[FCK_AOS_NRF] = false;
    s.gnss_checking = false;
    s.held          = false;
    s.agreed        = 0U;
    s.said          = false;
    s.troubled      = false;
    taskENTER_CRITICAL(&s_mux);
    s.state.chip_on[FCK_AOS_C5]  = false;
    s.state.chip_on[FCK_AOS_NRF] = false;
    s.state.cc1101_ok            = false;
    taskEXIT_CRITICAL(&s_mux);
    state_set_present(false);
}

static kode_status_t gnss_start(void);

/* What the app had set, as the mask the events speak. The owner's task, under no job. */
static uint32_t settings_in_force(void)
{
    uint32_t mask = 0U;
    taskENTER_CRITICAL(&s_mux);
    if (s.state.gnss_running) {
        mask |= FCK_AOS_SETTING_GNSS;
    }
    if (s.listen_active) {
        mask |= FCK_AOS_SETTING_LISTEN;
    }
    if (s.power_off[FCK_AOS_C5]) {
        mask |= FCK_AOS_SETTING_C5_POWER;
    }
    if (s.power_off[FCK_AOS_NRF]) {
        mask |= FCK_AOS_SETTING_NRF_POWER;
    }
    taskEXIT_CRITICAL(&s_mux);
    return mask;
}

/* The settings go with the module: the receiver runs no more and the listening is out of
 * force, and a chip the app switched off is on again. What can be set
 * again without emitting (the receiver, the listening) is remembered for when the module is. */
static void settings_lose(void)
{
    taskENTER_CRITICAL(&s_mux);
    s.want_gnss = s.want_gnss || s.state.gnss_running;
    s.listen_active = false;
    for (int chip = 0; chip < FCK_AOS_CHIPS; chip++) {
        s.power_off[chip] = false;
    }
    taskEXIT_CRITICAL(&s_mux);
}

static void leave(const char *why)
{
    KLOG_W(TAG, "%s", why);
    uint32_t const lost = settings_in_force();
    settings_lose();
    release_module();
    emit(FCK_AOS_EVENT_GONE, 0U);
    if (lost != 0U) {
        emit(FCK_AOS_EVENT_SETTING_LOST, lost);
    }
}

/* The module is back and told so: the receiver and the listening are set again - they do not
 * emit - and the app told which. A receiver that does not start is not tried again; the app
 * has been told it was lost, and can ask for it. */
static void settings_restore(void)
{
    uint32_t restored = 0U;
    if (s.want_gnss) {
        s.want_gnss = false;
        if (gnss_start() == KODE_OK) {
            /* The receiver runs; the C5 relays it once it has said what it runs, and the app
             * is told then (relay_tick()). */
            s.relay       = RELAY_WAIT;
            s.relay_until = xTaskGetTickCount() + ms_ticks(RELAY_WAIT_MS);
        }
    }
    taskENTER_CRITICAL(&s_mux);
    if (s.want_listen && !s.listen_active) {
        s.listen_active = true;
        s.listen_gen++;
        restored |= FCK_AOS_SETTING_LISTEN;
    }
    taskEXIT_CRITICAL(&s_mux);
    if (restored != 0U) {
        emit(FCK_AOS_EVENT_SETTING_RESTORED, restored);
    }
}

static void listen_forget(void);

void fck_aos_setting_listen(bool on, uint32_t hz)
{
    taskENTER_CRITICAL(&s_mux);
    s.want_listen = on;
    if (on) {
        s.listen_active = true;
        s.listen_hz     = hz;
        s.listen_gen++;
    } else {
        s.listen_active = false;
    }
    taskEXIT_CRITICAL(&s_mux);
    /* What an earlier listening heard is not this one's: turned on or off, it is forgotten (the
     * count goes on, so that a reader's "after" never meets a count it has seen). */
    listen_forget();
}

uint32_t fck_aos_listen_seq(void)
{
    taskENTER_CRITICAL(&s_mux);
    uint32_t const seq = s.lst_seq;
    taskEXIT_CRITICAL(&s_mux);
    return seq;
}

static bool refuse_ao(const char *call_name);

/* A capture above @p after, copied under the lock. */
static bool listen_take(uint32_t after, int32_t *us, size_t cap, size_t *n, fck_drv_cc1101_heard_t *heard,
                        uint32_t *seq)
{
    bool took = false;
    if ((s.lst_lock != NULL) && (xSemaphoreTake(s.lst_lock, ms_ticks(LISTEN_LOCK_MS)) == pdTRUE)) {
        if ((s.lst_last != NULL) && (s.lst_last->seq != 0U) && (s.lst_last->seq > after)) {
            size_t const k = (s.lst_last->n < cap) ? s.lst_last->n : cap;
            if (k > 0U) {
                memcpy(us, s.lst_last->us, k * sizeof(us[0]));
            }
            *n     = k;
            *heard = s.lst_last->heard;
            *seq   = s.lst_last->seq;
            took   = true;
        }
        (void)xSemaphoreGive(s.lst_lock);
    }
    return took;
}

kode_status_t fck_aos_listen_last(uint32_t after, uint32_t ms, int32_t *us, size_t cap, size_t *n,
                                  fck_drv_cc1101_heard_t *heard, uint32_t *seq)
{
    if (n == NULL || heard == NULL || seq == NULL || (us == NULL && cap > 0U)) {
        return fail(KODE_DENIED, "n, heard or seq is NULL, or us is NULL with cap above 0");
    }
    if (!s.started) {
        return fail(KODE_DENIED, "fck_begin() has not run, or fck_end() was called");
    }
    if (ms > CALL_MS_MAX) {
        return fail(KODE_DENIED, "ms is above FCK_AOS_CALL_MS_MAX");
    }
    if (ms > 0U && refuse_ao("fck_subghz_listen_read()")) {
        return KODE_DENIED;
    }
    if (ms > 0U && fck_ao_is_worker_task()) {
        return fail(KODE_DENIED, "called from a job of the owner's worker: it would wait for itself");
    }
    TickType_t const until = xTaskGetTickCount() + ms_ticks(ms);
    TickType_t const poll  = (ms_ticks(FIX_POLL_MS) > 0U) ? ms_ticks(FIX_POLL_MS) : 1U;
    uint32_t const   mark  = fck_aos_cancel_mark();
    for (;;) {
        if (!fck_aos_setting_listening(NULL, NULL)) {
            return fail(KODE_DENIED, "no listening: fck_subghz_listen_start() has not run, or the module is gone");
        }
        if (listen_take(after, us, cap, n, heard, seq)) {
            return KODE_OK;
        }
        if (ms == 0U || reached(until)) {
            return fail(KODE_TIMEOUT, "no remote was heard in the time given");
        }
        if (fck_aos_cancelled(mark)) {
            return fail(KODE_DENIED, "cancelled by fck_cancel()");
        }
        vTaskDelay(poll);
    }
}

bool fck_aos_setting_listening(uint32_t *hz, uint32_t *generation)
{
    taskENTER_CRITICAL(&s_mux);
    bool const on = s.listen_active && s.state.present;
    if (on) {
        if (hz != NULL) {
            *hz = s.listen_hz;
        }
        if (generation != NULL) {
            *generation = s.listen_gen;
        }
    }
    taskEXIT_CRITICAL(&s_mux);
    return on;
}

static void look(void)
{
    if (!fck_drv_pins_pair_held(&s.hw)) {
        /* An empty header, or a module not built the way this one is. The
         * module identified before, if any, is gone with it. */
        s.held     = false;
        s.agreed   = 0U;
        s.reask    = 0U;
        s.said     = false;
        s.troubled = false;
        s.known    = false;
        return;
    }
    if (!s.held) {
        s.held   = true;
        s.agreed = 0U;
        s.reask  = 0U;
    }
    /* A module that is not an F*CK, or whose bus misbehaves, is asked again
     * only now and then. */
    if (fck_plan_look_waits(s.agreed, s.troubled, s.reask)) {
        s.reask--;
        return;
    }

    /* One probe first: an address and a stop, nothing written. */
    ident_t  id     = ID_UNSURE;
    uint32_t nacked = 0U;
    uint32_t failed = 0U;
    fck_drv_tca6408a_forget(&s.tca);
    if (fck_drv_tca6408a_open(&s.tca, &s.hw) == KODE_OK && fck_drv_tca6408a_probe(&s.tca, 1U, &nacked, &failed)) {
        id = identify();
    }
    /* No clean NAK and no answer that held. Nothing is known of what holds the
     * bus, and nothing is driven for it: an F*CK whose receiver's firmware has
     * taken SDA (about 420 ms after power-up) looks like a Mesh module's, and
     * like any other module that holds the pair. The exception is the module
     * identified before, the pair held ever since: it is that F*CK. Its
     * receiver goes to reset by hand, and the bus is asked for up to a second,
     * since it takes up to 500 ms to come back. A clean NAK, or a chip that
     * is not the expander, is never driven. */
    if (id == ID_UNSURE && s.known && fck_drv_tca6408a_is_open(&s.tca) && nacked == 0U) {
        TickType_t const until = xTaskGetTickCount() + ms_ticks(FIND_MS);
        do {
            fck_drv_tca6408a_blind_gnss_reset(&s.tca, &s.hw);
            if (fck_drv_tca6408a_probe(&s.tca, FIND_TRIES, &nacked, &failed)) {
                id = identify();
            }
            if (id == ID_UNSURE) {
                vTaskDelay(ms_ticks(FIND_GAP_MS));
            }
        } while (id == ID_UNSURE && !reached(until) && !s.park_asked);
    }

    if (id == ID_FCK) {
        /* It read like the expander. The first register it is written is the
         * power-loss mark, and a chip that does not keep it is not one; if
         * the write fails, nothing else is written either. */
        id = mark_canary();
    }
    if (id == ID_FCK) {
        s.agreed   = 0U;
        s.said     = false;
        s.troubled = false;
        setup();
        s.known = true;
        return;
    }
    bool const open = fck_drv_tca6408a_is_open(&s.tca);
    (void)fck_drv_tca6408a_close(&s.tca);
    fck_drv_pins_rest(&s.hw);
    if (id == ID_OTHER) {
        s.known = false;
    }
    if (!open || (id == ID_UNSURE && nacked == 0U)) {
        /* The pair is held and the bus misbehaves - a module still powering
         * up looks like this - or there was no bus to ask with. Nothing is
         * ruled out on it. */
        if (!s.troubled) {
            s.troubled = true;
            if (open && s.known) {
                KLOG_W(TAG, "the F*CK's bus does not come back (%lu errors), the GNSS "
                            "receiver's reset by hand included", (unsigned long)failed);
            } else if (open) {
                KLOG_W(TAG, "the header holds the I2C pair but the bus does not answer "
                            "cleanly (%lu errors): nothing is written or driven until a "
                            "F*CK is identified. One whose GNSS receiver already holds SDA "
                            "is not found until it is unplugged and plugged in again",
                       (unsigned long)failed);
            } else {
                KLOG_E(TAG, "no I2C controller is free for J3: the F*CK cannot be looked for");
            }
        }
        s.reask = FCK_PLAN_REASK_LOOKS;
        return;
    }
    s.agreed++;
    s.reask = FCK_PLAN_REASK_LOOKS;
    if (fck_plan_ruled_out(s.agreed) && !s.said) {
        s.said = true;
        s.ev_wrong = (id == ID_OTHER) ? FCK_AOS_SEEN_OTHER_CHIP : FCK_AOS_SEEN_HOLDS_PAIR;
        if (id == ID_OTHER) {
            KLOG_I(TAG, "a chip answers at 0x%02X but is not the F*CK's expander",
                   (unsigned)FCK_BSP_EXPANDER_ADDRESS);
        } else {
            KLOG_I(TAG, "something holds the I2C pair and nothing answers at 0x%02X: "
                        "not an F*CK", (unsigned)FCK_BSP_EXPANDER_ADDRESS);
        }
    }
}

static void alive(void)
{
    bool ok = false;
    if (s.state.gnss_holds_bus) {
        ok = fck_drv_pins_scl_held(&s.hw);
    } else {
        uint8_t input = 0U;
        ok = (fck_drv_tca6408a_read(&s.tca, FCK_BSP_EXPANDER_REGISTER_INPUT, &input) == KODE_OK);
        uint8_t pol = 0U;
        if (ok && s.canary
            && fck_drv_tca6408a_read(&s.tca, FCK_BSP_EXPANDER_REGISTER_POLARITY, &pol) == KODE_OK
            && fck_plan_power_lost(pol)) {
            /* Everything on the module restarted: it is set up again where it
             * stands, as if it had just been plugged in. */
            leave("the F*CK lost power for a moment: it is set up again");
            s.look_at = xTaskGetTickCount();   /* looked for again at the next tick */
            return;
        }
    }
    s.misses = ok ? 0U : s.misses + 1U;
    if (fck_plan_gone(s.misses)) {
        leave("the F*CK no longer answers: the module has left the header");
    }
}

/* ---- The GNSS receiver ------------------------------------------------------------- */

static void gnss_state(bool running, bool holds_bus)
{
    taskENTER_CRITICAL(&s_mux);
    s.state.gnss_running   = running;
    s.state.gnss_holds_bus = holds_bus;
    taskEXIT_CRITICAL(&s_mux);
}

static kode_status_t gnss_start(void)
{
    if (s.state.gnss_running) {
        return KODE_OK;
    }
    if (!expander_usable()) {
        return KODE_IO_ERROR;
    }
    /* A firmware that holds the bus leaves the expander out of reach until
     * it stops: SW0 is put at rest first, and read back, so that a listen
     * meanwhile goes through the leg fck_drv_subghz_scan_leg_open() says.
     * A pin that does not read it is said, as setup() says it; the GNSS
     * receiver, which does not use it, runs all the same. */
    uint8_t input   = 0U;
    bool    at_rest = false;
    if (sw0_rest(false, &input, &at_rest) != KODE_OK) {
        return KODE_IO_ERROR;
    }
    if (!at_rest) {
        KLOG_W(TAG, "the antenna switch's control pin (P3) does not read at rest (input 0x%02X): "
                    "a listen while the GNSS receiver runs may go through the wrong leg",
               (unsigned)input);
    }
    kode_status_t const st = fck_drv_tca6408a_mask(&s.tca, P2_MASK, true, true);
    if (st != KODE_OK) {
        return st;
    }
    fix_reset();
    gnss_state(true, false);
    /* Whether its firmware takes the bus is seen once it has had time to. */
    s.gnss_checking = true;
    s.gnss_check_at = xTaskGetTickCount() + ms_ticks(GNSS_CHECK_MS);
    return KODE_OK;
}

/* After gnss_hold() on the worker. Through the expander or by hand, P2 went
 * low: the receiver is in reset. When the expander still does not answer
 * after it, alive() finds out whether the module is there at all. */
static void gnss_stopped(void)
{
    s.gnss_checking = false;
    fix_reset();
    gnss_state(false, false);
}

static void gnss_check(void)
{
    s.gnss_checking = false;
    if (fck_drv_tca6408a_probe(&s.tca, FIND_TRIES, NULL, NULL)) {
        return;
    }
    (void)fck_drv_tca6408a_close(&s.tca);
    gnss_state(true, true);
    KLOG_I(TAG, "the GNSS receiver's firmware holds SDA: the expander is out of reach "
                "until it is put back in reset");
}

/* ---- Going home -------------------------------------------------------------------- */

/* kodeOS does not know the module, and a restart changes nothing on it
 * (fck_drv_system.h): what costs energy, or transmits, and nobody asked for goes
 * down before the app leaves. Quick, so the restart is not held up: the
 * receiver's reset by hand if the bus is not there, and no waiting for the
 * bus to come back.
 *
 * Both chips come back as from power-on, their radios off: the nRF through
 * its reset, now - or, with the expander out of reach, told `rf off`, which
 * stops its echo responder once any command it is running ends - and the C5
 * when kodeOS lets go of the EN held low here (kode_init() floats J3, and
 * R502 pulls EN up). Neither firmware transmits at boot. */
/* The first thing a way home does, from whatever task it runs on: the CC1101's
 * send stops at once (SIDLE and GDO0 low, within 10 ms) instead of when the
 * worker's job is over, and nothing new is started. A job the worker is in
 * is not waited for to start doing this: that wait is what let a send of up
 * to 3 s outlast kodeOS's 200 ms of grace. */
static void leave_the_air(void)
{
    s.leaving = true;
    fck_drv_cc1101_abort();
}

/* Everything a way home does to the module but the C5's park (which is for good): the GNSS
 * receiver in reset, the CC1101 asleep, the nRF reset. A rest does only this, and holds the C5
 * (below); the P4's restart is what a way home lets the C5 go with. */
static void park_module(void)
{
    if (s.state.gnss_running
        && (!expander_usable() || fck_drv_tca6408a_mask(&s.tca, P2_MASK, true, false) != KODE_OK)) {
        fck_drv_tca6408a_blind_gnss_reset(&s.tca, &s.hw);
    }
    if (s.state.cc1101_ok) {
        (void)fck_drv_cc1101_sleep(&s.hw, NULL, NULL);
    }
    if (expander_usable() && fck_drv_tca6408a_mask(&s.tca, P6_MASK, true, false) == KODE_OK) {
        vTaskDelay(ms_ticks(NRF_RESET_MS));
        (void)fck_drv_tca6408a_mask(&s.tca, P6_MASK, false, true);
    } else if (s.uart[FCK_AOS_NRF] >= 0) {
        (void)fck_drv_uart_write(s.uart[FCK_AOS_NRF], "\nrf off\n", 8U);
    }
}

static void park(void)
{
    if (!s.state.present || s.parked_for_good) {
        return;
    }
    park_module();
    fck_drv_pins_c5_park(&s.hw);
    s.parked_for_good = true;
}

/* Run by esp_restart() in the task that asked for it, before the scheduler
 * stops. The owner parks the module at its next tick; a restart asked from
 * a callback, or from a job, is already where the module is, and parks it
 * in place. */
static void on_restart(void)
{
    if (!s.started) {
        return;
    }
    leave_the_air();
    if (!s.state.present) {
        return;
    }
    if (fck_ao_is_own_task() || fck_ao_is_worker_task()) {
        park();
        return;
    }
    /* The owner at the asker's level for it (AO_Boot's, from kodeOS's ways
     * home), so that what runs above the owner cannot hold the park past
     * PARK_WAIT_MS. */
    fck_ao_lift_to_caller();
    s.park_asked = true;
    if (xSemaphoreTake(s.parked, ms_ticks(PARK_WAIT_MS)) != pdTRUE) {
        /* Only the C5's two lines, which are plain GPIOs and nobody else's:
         * a job writing it through its ROM would otherwise leave it awake in
         * its loader, drawing current, until something resets it. */
        fck_drv_pins_c5_park(&s.hw);
        KLOG_W(TAG, "the F*CK was not parked in %u ms (a job or a long ask in the way): "
                    "the C5 is held in reset, the rest goes home as it is",
               (unsigned)PARK_WAIT_MS);
    }
}

static void rest_do(void);

static void park_if_asked(void)
{
    if (s.park_asked) {
        park();
        s.park_asked = false;
        (void)xSemaphoreGive(s.parked);
    }
    if (s.rest_asked) {
        s.rest_asked = false;
        rest_do();
    }
}

static void listen_free(void);

/* fck_aos_stop() on the owner's task: the module is put down as for a way home
 * - the GNSS receiver in reset, the CC1101 asleep, the nRF reset, the C5 held -
 * and let go of, and nothing more is looked for until fck_aos_start(). */
static void stop_module(void)
{
    s.leaving = true;
    fck_drv_cc1101_abort();
    park();
    release_module();
    s.parked_for_good = false;
    s.park_asked      = false;
    s.known           = false;
    s.ev_present      = false;
    s.ev_wrong        = 0U;
    s.want_gnss       = false;
    taskENTER_CRITICAL(&s_mux);
    s.want_listen   = false;
    s.listen_active = false;
    taskEXIT_CRITICAL(&s_mux);
    s.rested          = false;
    s.rest_asked      = false;
    s.power_todo      = false;
    s.stopped         = true;
    listen_free();
    KLOG_I(TAG, "stopped: the module is let go of");
}

/* ---- Requests ---------------------------------------------------------------------- */

static kode_status_t chip_power(fck_aos_chip_t chip, bool on)
{
    kode_status_t st = KODE_OK;
    if (chip == FCK_AOS_C5) {
        if (on) {
            fck_drv_pins_c5_run(&s.hw);
        } else {
            fck_drv_pins_c5_reset(&s.hw, false, true);
        }
    } else {
        if (!expander_usable()) {
            owner_why("the module's expander cannot be used now (no F*CK, or the GNSS receiver holds the bus)");
            return KODE_DENIED;
        }
        /* Let go, R701 holds the nRF out of reset; driven low, it is in it. */
        st = on ? fck_drv_tca6408a_mask(&s.tca, P6_MASK, false, true)
                : fck_drv_tca6408a_mask(&s.tca, P6_MASK, true, false);
    }
    if (st == KODE_OK) {
        link_drop(chip);
        chip_on_set(chip, on);
        s.power_off[chip] = !on;
    }
    return st;
}

static kode_status_t chip_reset(fck_aos_chip_t chip)
{
    kode_status_t st = KODE_OK;
    if (chip == FCK_AOS_C5) {
        fck_drv_pins_c5_reset(&s.hw, false, false);
    } else {
        if (!expander_usable()) {
            owner_why("the module's expander cannot be used now (no F*CK, or the GNSS receiver holds the bus)");
            return KODE_DENIED;
        }
        st = fck_drv_tca6408a_mask(&s.tca, P6_MASK, true, false);
        vTaskDelay(ms_ticks(NRF_RESET_MS));
        kode_status_t const up = fck_drv_tca6408a_mask(&s.tca, P6_MASK, false, true);
        if (st == KODE_OK) {
            st = up;
        }
    }
    link_drop(chip);
    if (st == KODE_OK) {
        chip_on_set(chip, true);
    }
    return st;
}

/* ---- The worker's jobs -------------------------------------------------------------- */

/* Each runs on the worker, pinned to core 1 (fck_ao.c), with the module to
 * itself: the step does not touch it until the done event. */

static kode_status_t look_job(void *arg, uint32_t *value)
{
    (void)arg;
    (void)value;
    look();
    return KODE_OK;
}

static kode_status_t gnss_stop_job(void *arg, uint32_t *value)
{
    (void)arg;
    (void)value;
    return gnss_hold();
}

static kode_status_t run_job(void *arg, uint32_t *value)
{
    (void)value;
    fck_aos_request_t *const r = (fck_aos_request_t *)arg;
    if (r->abandoned) {
        return KODE_TIMEOUT;     /* its caller is gone, and its argument with it */
    }
    /* A fck_cancel() ended what ran before this; this one is cancelled only by a later one. The
     * flag is cleared first and the count read after, so a cancel at any moment in between is
     * seen by one or the other. Leaving is never cleared. */
    if (!s.leaving) {
        fck_drv_cc1101_abort_clear();
    }
    if (fck_aos_cancelled(r->cancel_mark)) {
        r->why = "cancelled by fck_cancel()";
        return KODE_DENIED;      /* cancelled while it waited for the worker */
    }
    if (s.leaving) {
        fck_drv_cc1101_abort();
        r->why = "the app is leaving (HOME, sleep or a restart)";
        return KODE_DENIED;
    }
    return r->job(r->arg, &s.job_context);
}

/* Hand @p fn to the worker; @p r, if any, is answered when it ends. */
static void job_start(job_kind_t kind, fck_ao_job_t fn, fck_aos_request_t *r)
{
    s.job         = kind;
    s.job_request = r;
    if (fck_ao_submit_work(fn, r, &s.job_seq) != KODE_OK) {
        s.job         = JOB_NONE;
        s.job_request = NULL;
        if (r != NULL) {
            finish_why(r, KODE_NO_RESOURCES, "the owner's worker could not take the job");
        }
    }
}

static void handle(fck_aos_request_t *r)
{
    if (r->abandoned) {
        finish_why(r, KODE_TIMEOUT, "the caller gave up");
        return;
    }
    if (r->kind == REQUEST_LISTEN) {
        /* Here, between two callbacks: once it returns, the old one is
         * never called again, and its ctx may go. */
        taskENTER_CRITICAL(&s_mux);
        s.listen_fn[r->chip]  = r->on_reply;
        s.listen_ctx[r->chip] = r->ctx;
        taskEXIT_CRITICAL(&s_mux);
        finish(r, KODE_OK);
        return;
    }
    if (s.job != JOB_NONE) {
        /* The module is the job's: this waits for its end, in its turn. */
        if (s.deferred_n < DEFERRED_MAX) {
            s.deferred[s.deferred_n++] = r;
        } else {
            finish_why(r, KODE_NO_RESOURCES, "too many requests wait for the job that runs");
        }
        return;
    }
    if (r->kind == REQUEST_STOP) {
        stop_module();
        finish(r, KODE_OK);
        return;
    }
    if (r->kind == REQUEST_GNSS && !r->flag) {
        s.want_gnss = false;     /* stopped while the module is gone: nothing to set again */
    }
    if (!s.state.present || s.parked_for_good || s.leaving || s.stopped || s.rested) {
        finish_why(r, KODE_DENIED,
                   (s.leaving || s.parked_for_good || s.rested || s.stopped)
                       ? "the app is leaving (a way home or a restart) or the board is resting: nothing new is started"
                       : "no F*CK is on J3");
        return;
    }
    switch (r->kind) {
    case REQUEST_COMMAND:
    case REQUEST_FIRMWARE:
        drop_abandoned(r->chip);     /* a call that was let go of does not hold the line */
        if (s.state.chip_on[r->chip] && s.flight[r->chip].req != NULL && posted_wait(r)) {
            return;
        }
        if (!s.state.chip_on[r->chip] || s.flight[r->chip].req != NULL) {
            finish_why(r, KODE_DENIED, "the chip is off, or another command is in flight on it");
            return;
        }
        s.flight[r->chip].req      = r;
        s.flight[r->chip].started  = false;
        s.flight[r->chip].heard    = false;
        s.flight[r->chip].deadline = accept_deadline(r->chip, r->ms);
        try_start(r->chip);
        return;
    case REQUEST_POWER:
        finish(r, chip_power(r->chip, r->flag));
        return;
    case REQUEST_RESET:
        finish(r, chip_reset(r->chip));
        return;
    case REQUEST_GNSS:
        if (r->flag) {
            finish(r, gnss_start());
        } else if (!s.state.gnss_running) {
            finish(r, KODE_OK);
        } else {
            /* Up to PATIENT_MS, and the bus made again: the worker's. */
            job_start(JOB_GNSS_STOP, gnss_stop_job, r);
        }
        return;
    case REQUEST_RUN:
        s.job_chip_on[FCK_AOS_C5]  = s.state.chip_on[FCK_AOS_C5];
        s.job_chip_on[FCK_AOS_NRF] = s.state.chip_on[FCK_AOS_NRF];
        /* A job without the chips is not handed their UARTs: the owner goes
         * on reading them while it runs. */
        s.job_context = (fck_aos_job_context_t){
            .hw      = &s.hw,
            .tca     = expander_usable() ? &s.tca : NULL,
            .uart    = { r->flag ? -1 : s.uart[FCK_AOS_C5], r->flag ? -1 : s.uart[FCK_AOS_NRF] },
            .chip_on = s.job_chip_on,
        };
        job_start(r->flag ? JOB_RUN_WITHOUT_CHIPS : JOB_RUN, run_job, r);
        return;
    default:
        finish_why(r, KODE_DENIED, "the owner does not know this request");
        return;
    }
}

/* ---- What AO_Fck's signals do (fck_ao.h) -------------------------------------------- */

void fck_aos_on_request(fck_aos_request_t *request)
{
    handle(request);
}

/* One of the owner's own commands to the C5, on its line: nobody waits for it, and finish() frees
 * it. false when there was no memory for it. */
static bool own_command(request_kind_t kind, const char *line, const char *verb, uint32_t ms)
{
    size_t const line_n = strlen(line) + 1U;
    size_t const verb_n = strlen(verb) + 1U;
    /* cppcheck-suppress misra-c2012-21.3 ; the owner's own request, freed by finish() */
    fck_aos_request_t *const q = calloc(1U, sizeof(*q) + line_n + verb_n);
    if (q == NULL) {
        return false;
    }
    memcpy(q->data, line, line_n);
    memcpy(&q->data[line_n], verb, verb_n);
    q->kind     = kind;
    q->chip     = FCK_AOS_C5;
    q->relay_cmd = true;
    q->line     = q->data;
    q->verb     = &q->data[line_n];
    q->ms       = ms;
    q->st       = KODE_ERROR;
    q->rc       = -1;
    q->internal = true;
    q->done     = xSemaphoreCreateBinary();
    if (q->done == NULL) {
        /* cppcheck-suppress misra-c2012-21.3 ; as above */
        free(q);
        return false;
    }
    s.flight[FCK_AOS_C5].req      = q;
    s.flight[FCK_AOS_C5].started  = false;
    s.flight[FCK_AOS_C5].heard    = false;
    s.flight[FCK_AOS_C5].deadline = accept_deadline(FCK_AOS_C5, ms);
    try_start(FCK_AOS_C5);
    return true;
}

/* The GNSS relay set again, a step at a time. The C5 started again with the module: it is asked
 * what it runs (the app would ask the same), and only the F*CK's own firmware, one that says it has
 * the relay and takes a `gnss on` whatever the relay's state (FCK_DRV_C5_GNSS_REPEAT: "gnss" in
 * its caps=), is told it. A receiver nobody hears costs energy for nothing:
 * if the relay cannot be set, it is put in reset and the app, told it was lost, is not told it
 * came back. */
static void relay_tick(void)
{
    switch (s.relay) {
    case RELAY_WAIT:
        if (!s.state.gnss_running) {
            s.relay = RELAY_NONE;     /* the app stopped it meanwhile */
        } else if (reached(s.relay_until)) {
            s.relay = RELAY_FAIL;
        } else if (s.state.chip_on[FCK_AOS_C5] && s.flight[FCK_AOS_C5].req == NULL) {
            fck_aos_firmware_t a;
            firmware_copy(FCK_AOS_C5, &a);
            if (!a.known) {
                (void)own_command(REQUEST_FIRMWARE, INFO_VERB, INFO_VERB, FCK_DRV_C5_COMMAND_MS);
                break;
            }
            fck_drv_link_message_t const m = { .tag = "C5INFO", .body = a.body };
            fck_drv_c5_info_t            info;
            fck_drv_c5_command_t         c;
            if (!fck_drv_c5_parse_info(&m, &info) || !fck_drv_c5_is_compatible(&info)
                || !fck_drv_c5_has(&info, FCK_DRV_C5_GNSS_REPEAT) || fck_drv_c5_command_gnss(&c, true) != KODE_OK
                || !own_command(REQUEST_COMMAND, c.line, c.verb, c.ms)) {
                s.relay = RELAY_FAIL;
            } else if (s.relay == RELAY_WAIT) {
                s.relay = RELAY_SENT;
            }
        }
        break;
    case RELAY_OK:
        s.relay = RELAY_NONE;
        emit(FCK_AOS_EVENT_SETTING_RESTORED, FCK_AOS_SETTING_GNSS);
        break;
    case RELAY_FAIL:
        s.relay = RELAY_NONE;
        if (s.state.gnss_running && s.job == JOB_NONE) {
            job_start(JOB_GNSS_STOP, gnss_stop_job, NULL);
        }
        break;
    case RELAY_NONE:
    case RELAY_SENT:
    default:
        break;
    }
}

/* J3's supply: asked once the owner runs and is subscribed to EXP_POWER_SIG, and asked again at
 * the next tick when the queue of the Dot's radio owner had no room. The owner does not wait for
 * the answer: it goes on looking for the module, which may be powered already (R72). */
static void power_tick(void)
{
    if (!s.power_todo) {
        return;
    }
    kode_status_t const st = kode_sys_exp_power(true);
    switch (st) {
    case KODE_OK:
        s.power_todo = false;
        j3_set(FCK_AOS_J3_ASKED);
        break;
    case KODE_NO_RESOURCES:
        break;      /* the next tick */
    case KODE_DENIED:
        s.power_todo = false;
        j3_set(FCK_AOS_J3_ALWAYS);
        KLOG_I(TAG, "no radio in this image: J3 is always powered, and fck_end() cannot switch it off");
        break;
    default:
        s.power_todo = false;
        j3_set(FCK_AOS_J3_FAILED);
        KLOG_E(TAG, "J3's supply could not be asked for (%d)", (int)st);
        break;
    }
}

void fck_aos_on_exp_power(bool on, uint8_t rc)
{
    if (!on) {
        if (s.stopped) {
            j3_set(FCK_AOS_J3_OFF);
        }
        return;
    }
    if (!s.started && !s.starting) {
        return;
    }
    switch ((kode_status_t)rc) {
    case KODE_OK:
        j3_set(FCK_AOS_J3_ON);
        break;
    case KODE_DENIED:
        /* The Dot's C5 link did not come up: the wish stays and the next link carries it out. The
         * module may be powered all the same (R72 holds the rail on when the C5 lets IO5 go). */
        j3_set(FCK_AOS_J3_NO_LINK);
        KLOG_W(TAG, "J3's supply: the link to the Dot's C5 is not up; the wish stays and the next link carries it out");
        break;
    default:
        j3_set(FCK_AOS_J3_FAILED);
        KLOG_E(TAG, "J3's supply: the Dot's C5 refused (%d); the wish was dropped", (int)rc);
        break;
    }
}

/* ---- Resting ------------------------------------------------------------------------ */

/* The board rests: the module is put down as for a way home, told to the app as it leaving, and
 * the C5 held where it costs least. Undone by fck_aos_on_wake(). */
static void rest_do(void)
{
    if (s.state.present) {
        park_module();
        leave("the board rests: the module is put down");
        fck_drv_pins_c5_reset(&s.hw, false, true);
    }
    s.rested = true;
}

void fck_aos_on_rest(bool off)
{
    (void)off;      /* an OFF round is a rest as an ASLEEP one: the chips are put in their lowest consumption */
    if (!s.started || s.stopped || s.parked_for_good || s.rested || s.rest_asked) {
        return;
    }
    leave_the_air();
    if (s.job != JOB_NONE) {
        s.rest_asked = true;     /* when the job ends */
        return;
    }
    rest_do();
}

void fck_aos_on_wake(void)
{
    if (s.rest_asked) {
        /* The round was abandoned before the job in the way ended. */
        s.rest_asked = false;
    }
    if (!s.rested && !s.leaving) {
        return;
    }
    if (s.parked_for_good || s.park_asked || s.stopped) {
        return;     /* a way home, done or waiting for the job in its way, or fck_end(): not a rest */
    }
    s.rested = false;
    s.leaving = false;
    fck_drv_cc1101_abort_clear();
    /* The C5 goes: the pull-up brings it out of reset, and the module is looked for as one plugged in. */
    fck_drv_pins_rest(&s.hw);
    s.look_at = xTaskGetTickCount();
}

/* ---- The sub-GHz listening, in the background ----------------------------------------- */

/* The capture kept is dropped; the memory stays while the listening is in force. */
static void listen_forget(void)
{
    if ((s.lst_lock != NULL) && (xSemaphoreTake(s.lst_lock, ms_ticks(LISTEN_LOCK_MS)) == pdTRUE)) {
        if (s.lst_last != NULL) {
            s.lst_last->seq = 0U;
            s.lst_last->n   = 0U;
        }
        (void)xSemaphoreGive(s.lst_lock);
    }
}

static void listen_free(void)
{
    if (s.lst_lock != NULL) {
        (void)xSemaphoreTake(s.lst_lock, ms_ticks(LISTEN_LOCK_MS));
    }
    /* cppcheck-suppress misra-c2012-21.3 ; the buffers listen_tick() allocated */
    free(s.lst_last);
    s.lst_last = NULL;
    /* cppcheck-suppress misra-c2012-21.3 ; as above */
    free(s.lst_buf);
    s.lst_buf = NULL;
    if (s.lst_lock != NULL) {
        (void)xSemaphoreGive(s.lst_lock);
    }
}

static kode_status_t listen_job(void *arg, uint32_t *value)
{
    (void)arg;
    if (!s.leaving) {
        fck_drv_cc1101_abort_clear();
    }
    uint32_t hz  = 0U;
    uint32_t gen = 0U;
    if (s.leaving || !fck_aos_setting_listening(&hz, &gen) || (s.lst_buf == NULL)) {
        return KODE_DENIED;
    }
    if (gen != s.lst_gen) {
        /* Set again, or by another call: the hop state starts over. */
        fck_drv_cc1101_listen_start(&s.lst, hz);
        s.lst_gen = gen;
    }
    size_t                 n = 0U;
    fck_drv_cc1101_heard_t h = { .heard = false };
    kode_status_t const    st = fck_drv_cc1101_listen(s.job_context.hw, s.job_context.tca, &s.lst,
                                                      LISTEN_SLICE_MS, s.lst_buf, LISTEN_RUNS, &n, &h);
    if ((st == KODE_OK) && h.heard && (n > 0U) && (s.lst_lock != NULL)
        && (xSemaphoreTake(s.lst_lock, ms_ticks(LISTEN_LOCK_MS)) == pdTRUE)) {
        uint32_t now_gen = 0U;
        /* Kept only if it is still the listening it was heard for: one stopped or set again
         * meanwhile has forgotten what came before. */
        if ((s.lst_last != NULL) && fck_aos_setting_listening(NULL, &now_gen) && (now_gen == gen)) {
            s.lst_last->seq   = ++s.lst_seq;
            s.lst_last->n     = n;
            s.lst_last->heard = h;
            memcpy(s.lst_last->us, s.lst_buf, n * sizeof(s.lst_buf[0]));
            *value = s.lst_seq;     /* the capture's count: what the news carries, and a reader asks "after" */
        }
        (void)xSemaphoreGive(s.lst_lock);
    }
    return st;
}

/* Start a slice when the listening is in force and nothing else wants the CC1101. */
static void listen_tick(void)
{
    bool const wanted = s.listen_active && s.state.present && s.state.cc1101_ok;
    if (!wanted || s.leaving || s.rested) {
        if (!s.want_listen && (s.lst_last != NULL)) {
            listen_free();      /* turned off: the memory goes back */
        }
        return;
    }
    if (s.job != JOB_NONE || s.deferred_n > 0U || !reached(s.lst_at)) {
        return;
    }
    if (s.lst_lock == NULL) {
        s.lst_lock = xSemaphoreCreateMutex();
    }
    if (s.lst_last == NULL) {
        /* cppcheck-suppress misra-c2012-21.3 ; held while the listening is, freed by listen_free() */
        s.lst_last = calloc(1U, sizeof(*s.lst_last));
    }
    if (s.lst_buf == NULL) {
        /* cppcheck-suppress misra-c2012-21.3 ; as above */
        s.lst_buf = malloc(LISTEN_RUNS * sizeof(s.lst_buf[0]));
    }
    if ((s.lst_lock == NULL) || (s.lst_last == NULL) || (s.lst_buf == NULL)) {
        s.lst_at = xTaskGetTickCount() + ms_ticks(LISTEN_RETRY_MS);
        KLOG_W(TAG, "no memory for the listening's buffers: tried again in %u ms", (unsigned)LISTEN_RETRY_MS);
        return;
    }
    s.job_context = (fck_aos_job_context_t){
        .hw      = &s.hw,
        .tca     = expander_usable() ? &s.tca : NULL,
        .uart    = { -1, -1 },
        .chip_on = s.job_chip_on,
    };
    job_start(JOB_LISTEN, listen_job, NULL);
}

void fck_aos_on_tick(void)
{
    if (s.stopped) {
        return;
    }
    power_tick();
    if (s.rested) {
        return;
    }
    if (s.job != JOB_NONE) {
        if ((s.job == JOB_RUN_WITHOUT_CHIPS) || (s.job == JOB_LISTEN)) {
            /* The job leaves both chips alone: what they say - the GNSS the
             * C5 relays among it - is read on as it comes. */
            pump();
        }
        return;
    }
    park_if_asked();
    if (!s.state.present) {
        if (!s.parked_for_good && reached(s.look_at)) {
            s.look_at = xTaskGetTickCount() + ms_ticks(LOOK_MS);
            job_start(JOB_LOOK, look_job, NULL);
        }
        return;
    }
    pump();
    flights();
    relay_tick();
    if (s.gnss_checking && reached(s.gnss_check_at)) {
        gnss_check();
    }
    if (!s.gnss_checking && reached(s.alive_at + ms_ticks(ALIVE_MS))) {
        s.alive_at = xTaskGetTickCount();
        alive();
    }
    if (s.state.present && (s.job == JOB_NONE)) {
        listen_tick();
    }
}

void fck_aos_on_work_done(uint16_t seq, kode_status_t status, uint32_t value)
{
    if (s.job == JOB_NONE || seq != s.job_seq) {
        return;
    }
    job_kind_t const         kind = s.job;
    fck_aos_request_t *const r    = s.job_request;
    s.job         = JOB_NONE;
    s.job_request = NULL;
    switch (kind) {
    case JOB_GNSS_STOP:
        gnss_stopped();
        if (r != NULL) {
            finish(r, status);
        }
        break;
    case JOB_RUN:
        link_drop(FCK_AOS_C5);
        link_drop(FCK_AOS_NRF);
        /* The lines say nothing back: the job's own word is all there is. */
        chip_on_set(FCK_AOS_C5, s.job_chip_on[FCK_AOS_C5]);
        chip_on_set(FCK_AOS_NRF, s.job_chip_on[FCK_AOS_NRF]);
        finish(r, status);
        break;
    case JOB_RUN_WITHOUT_CHIPS:
        /* Neither chip was the job's: what each said it runs, its lines and
         * the GNSS fix all stand. */
        finish(r, status);
        break;
    case JOB_LISTEN:
        /* A slice: nothing waits for it. A remote heard is told; a slice that could not run
         * waits before the next. */
        if (value > 0U) {
            emit(FCK_AOS_EVENT_PACKET_RECEIVED, value);
        }
        if ((status != KODE_OK) && (status != KODE_DENIED)) {
            s.lst_at = xTaskGetTickCount() + ms_ticks(LISTEN_RETRY_MS);
        }
        break;
    case JOB_LOOK:
    default:
        /* look() said what it found: s.state.present, and what it flagged
         * for the app, told here because the app's callback runs on this task. */
        if (s.ev_present) {
            s.ev_present = false;
            emit(FCK_AOS_EVENT_PRESENT, 0U);
            settings_restore();
        }
        if (s.ev_wrong != 0U) {
            uint32_t const seen = s.ev_wrong;
            s.ev_wrong          = 0U;
            emit(FCK_AOS_EVENT_WRONG_MODULE, seen);
        }
        break;
    }
    park_if_asked();
    /* What waited for the module, in the order it came. A request may start
     * another job, and the rest go back to wait for that one. */
    fck_aos_request_t *waiting[DEFERRED_MAX];
    size_t const       n = s.deferred_n;
    memcpy(waiting, s.deferred, n * sizeof(waiting[0]));
    s.deferred_n = 0U;
    for (size_t i = 0U; i < n; i++) {
        handle(waiting[i]);
    }
}

/* The system is going away - the battery cut, or kodeOS in a moment - and
 * the module with it: parked, as for a restart. A job in the way parks it
 * when it ends; a restart before that holds the C5 in reset (on_restart()). */
static void going_away(void)
{
    leave_the_air();
    if (s.job != JOB_NONE) {
        s.park_asked = true;
        return;
    }
    park();
}

void fck_aos_on_home(void)
{
    going_away();
}

/* ---- The callers' side --------------------------------------------------------------- */

/* Calls from an Active Object that were refused, to say so once in a hundred. */
static uint32_t s_ao_refused;

/* A call that blocks, made from the task of an Active Object: its step would
 * wait for the owner (or for a chip) and the queue behind it would not move. */
bool fck_aos_refuse_ao(const char *call_name)
{
    const char *ao = NULL;
    if (!fck_ao_is_any_ao_task(&ao)) {
        return false;
    }
    error_put_ao(call_name, ao);
    if ((s_ao_refused++ % 100U) == 0U) {
        KLOG_E(TAG, "%s blocks and was called from the Active Object %s: refused (KODE_DENIED). "
                    "Call fck_sys_*() from an Active Object, and the fck_*() calls that block "
                    "from a task of the app's own", call_name, (ao != NULL) ? ao : "?");
    }
    return true;
}

static bool refuse_ao(const char *call_name)
{
    return fck_aos_refuse_ao(call_name);
}

/* A chip's lock, waited for in steps so that fck_cancel() is heard.
 * @return KODE_OK held; KODE_TIMEOUT after @p ms; KODE_DENIED cancelled. */
static kode_status_t lock_take(fck_aos_chip_t chip, uint32_t ms, uint32_t mark)
{
    uint32_t left = ms;
    for (;;) {
        uint32_t const step = (left < CANCEL_STEP_MS) ? left : CANCEL_STEP_MS;
        if (xSemaphoreTakeRecursive(s.chip_lock[chip], ms_ticks(step)) == pdTRUE) {
            return KODE_OK;
        }
        if (fck_aos_cancelled(mark)) {
            return fail(KODE_DENIED, "cancelled by fck_cancel()");
        }
        if (left <= step) {
            return fail(KODE_TIMEOUT, "another task holds the chip, for longer than the call waits");
        }
        left -= step;
    }
}

/* @p r is the caller's, on its stack; what the owner is handed is a copy on the
 * heap, so that a caller that gives up (the owner stuck, past every bound
 * above) can leave without the owner writing to its stack afterwards. */
static kode_status_t call(fck_aos_request_t *r, bool lock_c5, bool lock_nrf, uint32_t lock_ms)
{
    if (!s.started) {
        return fail(KODE_DENIED, "fck_begin() has not run, or fck_end() was called");
    }
    if (refuse_ao("an fck_*() call")) {
        return KODE_DENIED;
    }
    if (fck_ao_is_worker_task()) {
        return fail(KODE_DENIED, "called from a job of the owner's worker: it would wait for itself");     /* from a job: it would wait for itself */
    }
    bool const locks[FCK_AOS_CHIPS] = { lock_c5, lock_nrf };
    bool       held[FCK_AOS_CHIPS]  = { false, false };
    uint32_t   mark                 = fck_aos_cancel_mark();
    for (int chip = 0; chip < FCK_AOS_CHIPS; chip++) {
        if (locks[chip] && (xSemaphoreGetMutexHolder(s.chip_lock[chip]) == xTaskGetCurrentTaskHandle())) {
            mark = s_hold_mark[chip];     /* part of an operation that began at its hold */
            break;
        }
    }
    if (fck_aos_cancelled(mark)) {
        return fail(KODE_DENIED, "cancelled by fck_cancel()");     /* the operation this call belongs to was cancelled */
    }
    kode_status_t st = KODE_OK;
    for (int chip = 0; chip < FCK_AOS_CHIPS && st == KODE_OK; chip++) {
        if (locks[chip]) {
            st = lock_take((fck_aos_chip_t)chip, lock_ms, mark);
            held[chip] = (st == KODE_OK);
        }
    }
    if (st == KODE_OK) {
        /* cppcheck-suppress misra-c2012-21.3 ; one request for the call, freed by whoever ends it last */
        size_t const line_n = (r->line != NULL) ? (strlen(r->line) + 1U) : 0U;
        size_t const verb_n = (r->verb != NULL) ? (strlen(r->verb) + 1U) : 0U;
        /* cppcheck-suppress misra-c2012-21.3 ; as above */
        fck_aos_request_t *const q = calloc(1U, sizeof(*q) + line_n + verb_n);
        if (q == NULL) {
            st = fail(KODE_NO_RESOURCES, "no memory for the request");
        } else {
            *q       = *r;
            if (line_n > 0U) {
                memcpy(q->data, r->line, line_n);
                q->line = q->data;
            }
            if (verb_n > 0U) {
                memcpy(&q->data[line_n], r->verb, verb_n);
                q->verb = &q->data[line_n];
            }
            if (r->kind != REQUEST_LISTEN && r->on_reply != NULL) {
                q->user_reply = r->on_reply;
                q->user_ctx   = r->ctx;
                q->on_reply   = reader_gate;
                q->ctx        = q;
            }
            q->done  = xSemaphoreCreateBinary();
            q->st    = KODE_ERROR;
            q->rc    = -1;
            q->finished  = false;
            q->abandoned = false;
            q->cancel_mark = mark;
            if (q->done == NULL) {
                /* cppcheck-suppress misra-c2012-21.3 ; the request allocated above */
                free(q);
                st = fail(KODE_NO_RESOURCES, "no memory for the request");
            } else {
                st = fck_ao_post(q);
                if (st != KODE_OK) {
                    vSemaphoreDelete(q->done);
                    /* cppcheck-suppress misra-c2012-21.3 ; the request allocated above */
                    free(q);
                    fck_aos_error_set_for(st, "the owner's queue had no room for the request");
                } else {
                    uint32_t const wait_ms = r->ms + (2U * FCK_AOS_JOB_MS_MAX) + FCK_AOS_ANSWER_GRACE_MS;
                    uint32_t       left    = wait_ms;
                    bool           answered = false;
                    bool           cut      = false;
                    for (;;) {
                        uint32_t const step = (left < CANCEL_STEP_MS) ? left : CANCEL_STEP_MS;
                        if (xSemaphoreTake(q->done, ms_ticks(step)) == pdTRUE) {
                            answered = true;
                            break;
                        }
                        /* A job lends the caller's memory to the worker: its caller stays until
                         * the job ends, which the cancel makes soon (run_job()). A command is
                         * let go at once, and the owner drops it. */
                        if (fck_aos_cancelled(mark) && (q->kind != REQUEST_RUN)) {
                            cut = true;
                            break;
                        }
                        if (left <= step) {
                            break;
                        }
                        left -= step;
                    }
                    if (!answered) {
                        taskENTER_CRITICAL(&s_mux);
                        answered = q->finished;
                        if (!answered) {
                            q->abandoned = true;
                        }
                        taskEXIT_CRITICAL(&s_mux);
                        if (answered) {
                            /* The owner finished it after the last wait gave up, and finish()
                             * gives done once out of s_mux: that give is taken before done is
                             * deleted and q freed under it. */
                            (void)xSemaphoreTake(q->done, portMAX_DELAY);
                        }
                        /* The reader may be running, in the owner's step: it is let finish,
                         * since it writes to this stack. No new one starts. */
                        for (unsigned w = 0U; !answered && q->in_reader && w < 200U; w++) {
                            vTaskDelay(1);
                        }
                    }
                    if (answered) {
                        st    = q->st;
                        r->rc = q->rc;
                        if (st != KODE_OK) {
                            fck_aos_error_set_for(st, (q->why != NULL) ? q->why : why_of(st));
                        }
                        vSemaphoreDelete(q->done);
                        /* cppcheck-suppress misra-c2012-21.3 ; the request allocated above */
                        free(q);
                    } else if (cut) {
                        KLOG_I(TAG, "the call was cancelled (fck_cancel())");
                        st = fail(KODE_DENIED, "cancelled by fck_cancel()");
                    } else {
                        KLOG_E(TAG, "the owner did not answer in %u ms: the call gives up "
                                    "(KODE_TIMEOUT)", (unsigned)wait_ms);
                        st = fail(KODE_TIMEOUT, "the owner did not answer: it is stuck, or the chip's lines are");
                    }
                }
            }
        }
    }
    for (int chip = FCK_AOS_CHIPS - 1; chip >= 0; chip--) {
        if (held[chip]) {
            (void)xSemaphoreGiveRecursive(s.chip_lock[chip]);
        }
    }
    return st;
}

static bool chip_ok(fck_aos_chip_t chip)
{
    return (int)chip >= 0 && chip < FCK_AOS_CHIPS;
}

/* The controllers the module's drivers run on, from kode-sdk's books and
 * under the module's tag, so that kode_sys_exp_return() gives them back with
 * the positions: I2C1 for the expander, SPI3 for the CC1101, and two UARTs,
 * the lowest free, one per chip. */
static const char *s_controller_missing = "";

static kode_status_t controllers_take(void)
{
    static const struct {
        kode_sys_exp_bus_t bus;
        const char    *name;
    } WANTED[] = {
        { KODE_SYS_EXP_I2C,  "I2C"  },
        { KODE_SYS_EXP_SPI,  "SPI"  },
        { KODE_SYS_EXP_UART, "UART" },
        { KODE_SYS_EXP_UART, "UART" },
    };
    uint8_t id[4] = { 0U, 0U, 0U, 0U };
    for (size_t i = 0U; i < 4U; i++) {
        kode_status_t const st = kode_sys_exp_ctrl(WANTED[i].bus, FCK_BSP_LOAN_TAG, &id[i]);
        if (st != KODE_OK) {
            s_controller_missing = WANTED[i].name;
            return st;
        }
    }
    s.hw.i2c_port             = (int)id[0];
    s.hw.spi_host             = (int)id[1];
    s.uart_port[FCK_AOS_C5]   = (int)id[2];
    s.uart_port[FCK_AOS_NRF]  = (int)id[3];
    return KODE_OK;
}

static bool s_hook_set;

kode_status_t fck_aos_start(void)
{
    if (!fck_ao_kode_is_up()) {
        return fail(KODE_DENIED, "kode_init() has not run: it starts the Active Objects the module's owner relies on");
    }
    taskENTER_CRITICAL(&s_mux);
    bool const busy = s.started || s.starting;
    if (!busy) {
        s.starting = true;
    }
    taskEXIT_CRITICAL(&s_mux);
    if (busy) {
        if (!s.started && fck_ao_is_any_ao_task(NULL)) {
            return fail(KODE_DENIED, "a start is under way in another task, and an Active Object does not wait for it");
        }
        TickType_t const until = xTaskGetTickCount() + ms_ticks(START_WAIT_MS);
        while (!s.started && s.starting && !reached(until)) {
            vTaskDelay(1);
        }
        return s.started ? KODE_OK
                         : fail(KODE_DENIED, "another task is starting the F*CK, and did not finish in 5 s");
    }
    error_clear();

    kode_status_t st = kode_sys_exp_lend(POSITIONS, FCK_BSP_POSITIONS, FCK_BSP_LOAN_TAG, s.hw.gpio);
    if (st != KODE_OK) {
        KLOG_E(TAG, "J3 is not free for the F*CK (%d): %s", (int)st,
               (st == KODE_EXISTS) ? "the console took it - `ps` says who"
                                   : "another loan in this image holds its positions");
        s.starting = false;
        return fail(st, (st == KODE_EXISTS) ? "J3 is not free: the console took it by hand (`exp`)"
                                            : "J3 is not free: another loan in this image holds some of its positions");
    }
    st = controllers_take();
    if (st != KODE_OK) {
        KLOG_E(TAG, "kode-sdk has no %s controller left for the F*CK (%d)",
               s_controller_missing, (int)st);
        (void)kode_sys_exp_return(FCK_BSP_LOAN_TAG);
        s.starting = false;
        return fail((st == KODE_EXISTS) ? KODE_EXISTS : KODE_NO_RESOURCES,
                    "kode-sdk has no I2C, SPI or UART controller left for the F*CK");
    }
    /* Made once: a start that failed further down left them for the next. */
    if (s.ev == NULL) {
        s.ev = xEventGroupCreate();
    }
    if (s.parked == NULL) {
        s.parked = xSemaphoreCreateBinaryStatic(&s.parked_buf);
    }
    for (int chip = 0; chip < FCK_AOS_CHIPS; chip++) {
        if (s.chip_lock[chip] == NULL) {
            s.chip_lock[chip] = xSemaphoreCreateRecursiveMutex();
        }
    }
    fck_drv_tca6408a_forget(&s.tca);
    fck_drv_pins_rest(&s.hw);
    s.look_at = xTaskGetTickCount();
    /* J3's supply is not promised either way at the start of an app: a C5 that let go of IO5 leaves
     * R72 holding the rail on, one that drove it off leaves it off. The owner asks for it on at its
     * first tick, once it is subscribed to EXP_POWER_SIG, and does not wait for the answer: the looks,
     * every LOOK_MS, find the module whenever it is up. */
    s.power_todo = true;
    j3_set(FCK_AOS_J3_UNKNOWN);
    kode_status_t const ao = (s.ev == NULL || s.chip_lock[0] == NULL || s.chip_lock[1] == NULL)
                                 ? KODE_NO_RESOURCES : fck_ao_start();
    if (ao != KODE_OK) {
        s.power_todo = false;
        (void)kode_sys_exp_return(FCK_BSP_LOAN_TAG);
        s.starting = false;
        return fail((ao == KODE_EXISTS) ? KODE_EXISTS : KODE_NO_RESOURCES,
                    (ao == KODE_EXISTS) ? "CONFIG_FCK_AO_PRIORITY is taken by another Active Object"
                                        : "the owner's Active Object or its worker could not start");
    }
    /* Once: ESP-IDF has five places for these, and none gives one back. */
    if (!s_hook_set) {
        if (fck_drv_system_on_restart(on_restart) != KODE_OK) {
            KLOG_W(TAG, "no place left for a restart hook: going home leaves the module "
                        "as it is (GNSS and CC1101 included)");
        }
        s_hook_set = true;
    }
    /* Only now is the owner let loose: after fck_end() it idles with the module let go of, and a
     * start that fails above (the supply, the Active Object) must leave it so - cleared before, an
     * idle owner would find the module and drive J3's pins with the loan given back. */
    fck_drv_cc1101_abort_clear();
    s.leaving         = false;
    s.parked_for_good = false;
    s.park_asked      = false;
    s.rested          = false;
    s.rest_asked      = false;
    s.stopped         = false;
    s.started  = true;
    s.starting = false;
    return KODE_OK;
}

/* J3's supply asked off, on the caller's task: a command that finds the radio owner's queue short
 * of room is asked again, 10 ms apart, for up to a second. With no radio in the image it answers
 * KODE_DENIED and J3 stays powered, which the status says. */
static void j3_power_off(void)
{
    for (unsigned i = 0U; i < 100U; i++) {
        kode_status_t const st = kode_sys_exp_power(false);
        if (st == KODE_OK) {
            j3_set(FCK_AOS_J3_OFF);
            return;
        }
        if (st != KODE_NO_RESOURCES) {
            j3_set(FCK_AOS_J3_ALWAYS);
            return;
        }
        vTaskDelay(ms_ticks(10U));
    }
    KLOG_W(TAG, "J3's supply could not be asked to switch off: the radio owner's queue had no room for 1 s");
}

kode_status_t fck_aos_stop(void)
{
    if (!s.started) {
        return s.starting ? KODE_DENIED : KODE_OK;
    }
    if (refuse_ao("fck_end()")) {
        return KODE_DENIED;     /* from an Active Object: it would wait for itself */
    }
    if (fck_ao_is_worker_task()) {
        return fail(KODE_DENIED, "called from a job of the owner's worker: it would wait for itself");
    }
    /* A send in the air is cut now, not when the request is served behind it. */
    fck_drv_cc1101_abort();
    fck_aos_request_t r = { .kind = REQUEST_STOP };
    kode_status_t st    = call(&r, false, false, 0U);
    if (st != KODE_OK) {
        if (!s.leaving) {
            fck_drv_cc1101_abort_clear();
        }
        return st;
    }
    s.started = false;
    j3_power_off();
    (void)kode_sys_exp_return(FCK_BSP_LOAN_TAG);
    return KODE_OK;
}

bool fck_aos_present(void)
{
    taskENTER_CRITICAL(&s_mux);
    bool const p = s.state.present;
    taskEXIT_CRITICAL(&s_mux);
    return p;
}

kode_status_t fck_aos_wait_present(uint32_t ms)
{
    if (!s.started) {
        return fail(KODE_DENIED, "fck_begin() has not run, or fck_end() was called");
    }
    if ((ms > 0U) && refuse_ao("fck_wait_present()")) {
        return KODE_DENIED;
    }
    if (ms > CALL_MS_MAX) {
        return fail(KODE_DENIED, "ms is above FCK_AOS_CALL_MS_MAX");
    }
    uint32_t const mark = fck_aos_cancel_mark();
    uint32_t       left = ms;
    for (;;) {
        uint32_t const step = (left < CANCEL_STEP_MS) ? left : CANCEL_STEP_MS;
        EventBits_t const bits = xEventGroupWaitBits(s.ev, BIT_PRESENT, pdFALSE, pdTRUE, ms_ticks(step));
        if ((bits & BIT_PRESENT) != 0U) {
            return KODE_OK;
        }
        if (fck_aos_cancelled(mark)) {
            return fail(KODE_DENIED, "cancelled by fck_cancel()");
        }
        if (left <= step) {
            return fail(KODE_TIMEOUT, "no F*CK was found in the time given");
        }
        left -= step;
    }
}

void fck_aos_status(fck_aos_status_t *out)
{
    if (out == NULL) {
        return;
    }
    taskENTER_CRITICAL(&s_mux);
    *out                = s.state;
    out->listening      = s.listen_active && s.state.present;
    out->started        = s.started;
    out->events_dropped = s_sink_dropped[FCK_AOS_SINK_EVENTS];
    out->news_dropped   = s_sink_dropped[FCK_AOS_SINK_NEWS];
    out->answers_dropped = s_sink_dropped[FCK_AOS_SINK_ANSWERS];
    taskEXIT_CRITICAL(&s_mux);
}

static kode_status_t command(fck_aos_chip_t chip, bool check, uint32_t seq, const char *line,
                             const char *verb, fck_drv_link_message_fn on_reply, void *ctx, uint32_t ms,
                             int32_t *rc)
{
    if (!chip_ok(chip) || line == NULL || !fck_plan_command_line_ok(line) || verb == NULL || ms == 0U
        || ms > CALL_MS_MAX) {
        return fail(KODE_DENIED, "the chip is not FCK_AOS_C5 or FCK_AOS_NRF, the line is not 1 to 255 printable characters, the verb is NULL, or ms is 0 or above FCK_AOS_CALL_MS_MAX");
    }
    fck_aos_request_t r = {
        .kind     = REQUEST_COMMAND,
        .chip     = chip,
        .line     = line,
        .verb     = verb,
        .on_reply = on_reply,
        .ctx      = ctx,
        .ms       = ms,
        .check    = check,
        .seq      = seq,
        .rc       = -1,
    };
    kode_status_t const st = call(&r, chip == FCK_AOS_C5, chip == FCK_AOS_NRF,
                                  ms + LOCK_MARGIN_MS);
    if (rc != NULL) {
        *rc = r.rc;
    }
    return st;
}

kode_status_t fck_aos_command(fck_aos_chip_t chip, const char *line, const char *verb,
                              fck_drv_link_message_fn on_reply, void *ctx, uint32_t ms,
                              int32_t *rc)
{
    return command(chip, false, 0U, line, verb, on_reply, ctx, ms, rc);
}

kode_status_t fck_aos_command_if_same_firmware(fck_aos_chip_t chip, uint32_t seq, const char *line,
                                 const char *verb, fck_drv_link_message_fn on_reply, void *ctx,
                                 uint32_t ms, int32_t *rc)
{
    return command(chip, true, seq, line, verb, on_reply, ctx, ms, rc);
}

kode_status_t fck_aos_firmware(fck_aos_chip_t chip, uint32_t ms, fck_aos_firmware_t *out)
{
    if (!chip_ok(chip) || out == NULL || !s.started) {
        return fail(KODE_DENIED, "the chip is not FCK_AOS_C5 or FCK_AOS_NRF, out is NULL, or fck_begin() has not run");
    }
    firmware_copy(chip, out);
    if (out->known) {
        return KODE_OK;
    }
    if (ms == 0U) {
        fck_aos_status_t st;
        fck_aos_status(&st);
        return (st.present && st.chip_on[chip]) ? KODE_TIMEOUT : KODE_DENIED;
    }
    if (ms > CALL_MS_MAX) {
        return fail(KODE_DENIED, "ms is above FCK_AOS_CALL_MS_MAX");
    }
    fck_aos_request_t r = {
        .kind  = REQUEST_FIRMWARE,
        .chip  = chip,
        .line  = INFO_VERB,
        .verb  = INFO_VERB,
        .ms    = ms,
        .firmware = out,
    };
    return call(&r, chip == FCK_AOS_C5, chip == FCK_AOS_NRF, ms + LOCK_MARGIN_MS);
}

kode_status_t fck_aos_hold(fck_aos_chip_t chip, uint32_t ms)
{
    if (!chip_ok(chip)) {
        return fail(KODE_DENIED, "the chip is not FCK_AOS_C5 or FCK_AOS_NRF");
    }
    if (!s.started) {
        return fail(KODE_DENIED, "fck_begin() has not run, or fck_end() was called");
    }
    if (refuse_ao("an fck_*() call")) {
        return KODE_DENIED;
    }
    if (fck_ao_is_worker_task()) {
        return fail(KODE_DENIED, "called from a job of the owner's worker: it would wait for itself");
    }
    if (ms > CALL_MS_MAX) {
        return fail(KODE_DENIED, "ms is above FCK_AOS_CALL_MS_MAX");
    }
    uint32_t const mark = fck_aos_cancel_mark();
    bool const     mine = (xSemaphoreGetMutexHolder(s.chip_lock[chip]) == xTaskGetCurrentTaskHandle());
    kode_status_t const st = lock_take(chip, ms, mark);
    if (st == KODE_OK && !mine) {
        s_hold_mark[chip] = mark;
    }
    return st;
}

kode_status_t fck_aos_release(fck_aos_chip_t chip)
{
    /* Allowed after fck_aos_stop() too: a task that held the chip while another ended the SDK still
     * gives it back. The locks are made at the first start and kept. */
    if (!chip_ok(chip) || s.chip_lock[chip] == NULL) {
        return fail(KODE_DENIED, "the chip is not FCK_AOS_C5 or FCK_AOS_NRF, or fck_begin() has not run");
    }
    /* Refused, by FreeRTOS, for a task that does not hold it. */
    return (xSemaphoreGiveRecursive(s.chip_lock[chip]) == pdTRUE)
               ? KODE_OK : fail(KODE_DENIED, "the calling task does not hold the chip");
}

kode_status_t fck_aos_listen(fck_aos_chip_t chip, fck_drv_link_message_fn fn, void *ctx)
{
    if (!chip_ok(chip)) {
        return fail(KODE_DENIED, "the chip is not FCK_AOS_C5 or FCK_AOS_NRF");
    }
    if (!s.started || fck_ao_is_own_task() || fck_ao_is_worker_task()) {
        /* No owner yet, so no callback running; the owner itself, between
         * two of them; or a job, while the UARTs are not read. */
        taskENTER_CRITICAL(&s_mux);
        s.listen_fn[chip]  = fn;
        s.listen_ctx[chip] = ctx;
        taskEXIT_CRITICAL(&s_mux);
        return KODE_OK;
    }
    fck_aos_request_t r = { .kind = REQUEST_LISTEN, .chip = chip, .on_reply = fn, .ctx = ctx };
    return call(&r, false, false, 0U);
}

kode_status_t fck_aos_power(fck_aos_chip_t chip, bool on)
{
    if (!chip_ok(chip)) {
        return fail(KODE_DENIED, "the chip is not FCK_AOS_C5 or FCK_AOS_NRF");
    }
    fck_aos_request_t r = { .kind = REQUEST_POWER, .chip = chip, .flag = on };
    return call(&r, chip == FCK_AOS_C5, chip == FCK_AOS_NRF, LOCK_MARGIN_MS);
}

kode_status_t fck_aos_reset(fck_aos_chip_t chip)
{
    if (!chip_ok(chip)) {
        return fail(KODE_DENIED, "the chip is not FCK_AOS_C5 or FCK_AOS_NRF");
    }
    fck_aos_request_t r = { .kind = REQUEST_RESET, .chip = chip };
    return call(&r, chip == FCK_AOS_C5, chip == FCK_AOS_NRF, LOCK_MARGIN_MS);
}

kode_status_t fck_aos_gnss_power(bool run)
{
    fck_aos_request_t r = { .kind = REQUEST_GNSS, .flag = run };
    return call(&r, false, false, 0U);
}

/* One look at the fix, under s_mux: what a caller decides on. The first look
 * notes the module and the fix it found, so that a later one tells a fix made
 * after the call from the one already there. */
static kode_status_t fix_peek(bool first, bool latest, uint32_t *gen, uint32_t *since,
                              fck_drv_nmea_fix_t *out, uint32_t *age_ms)
{
    kode_status_t st;
    TickType_t    at = 0U;
    taskENTER_CRITICAL(&s_mux);
    if (first) {
        *gen   = s.state.generation;
        *since = s.fix_n;
    }
    st = fck_plan_fix_verdict(first, latest, s.state.present, s.state.generation == *gen,
                              s.state.gnss_running, s.fix_have, s.fix_n != *since);
    if (st == KODE_OK) {
        *out = s.fix;
        at   = s.fix_at;
    }
    taskEXIT_CRITICAL(&s_mux);
    if ((st == KODE_OK) && (age_ms != NULL)) {
        /* The clock is read after the fix was taken, and the fix was stamped with a reading
         * made before it was stored: this one is never the earlier of the two. */
        *age_ms = fck_plan_fix_age_ms((uint32_t)xTaskGetTickCount(), (uint32_t)at,
                                      (uint32_t)configTICK_RATE_HZ);
    }
    return st;
}

kode_status_t fck_aos_gnss_fix(uint32_t ms, fck_drv_nmea_fix_t *out, uint32_t *age_ms)
{
    if (out == NULL || !s.started) {
        return fail(KODE_DENIED, "out is NULL, or fck_begin() has not run");
    }
    if (ms > 0U && refuse_ao("fck_gnss_read()")) {
        return KODE_DENIED;     /* from an Active Object: it would wait for itself */
    }
    if (ms > 0U && fck_ao_is_worker_task()) {
        return fail(KODE_DENIED, "called from a job of the owner's worker: it would wait for itself");
    }
    if (ms > CALL_MS_MAX) {
        return fail(KODE_DENIED, "ms is above FCK_AOS_CALL_MS_MAX");
    }
    TickType_t const until = xTaskGetTickCount() + ms_ticks(ms);
    TickType_t const poll  = (ms_ticks(FIX_POLL_MS) > 0U) ? ms_ticks(FIX_POLL_MS) : 1U;
    uint32_t         gen   = 0U;
    uint32_t         since = 0U;
    uint32_t const   mark  = fck_aos_cancel_mark();
    kode_status_t    st    = fix_peek(true, ms == 0U, &gen, &since, out, age_ms);
    while (st == KODE_TIMEOUT && ms > 0U && !reached(until)) {
        if (fck_aos_cancelled(mark)) {
            return fail(KODE_DENIED, "cancelled by fck_cancel()");
        }
        vTaskDelay(poll);
        st = fix_peek(false, false, &gen, &since, out, age_ms);
    }
    if (st != KODE_OK) {
        fck_aos_error_set((st == KODE_DENIED) ? "the receiver is off, or no module is there"
                          : (st == KODE_IO_ERROR) ? "the module left or lost power while the fix was waited for"
                                                  : "no fix in the time given");
    }
    return st;
}

static kode_status_t job_request(fck_aos_job_fn fn, void *arg, uint32_t ms, bool without_chips)
{
    if (fn == NULL) {
        return fail(KODE_DENIED, "the job is NULL");
    }
    fck_aos_request_t r = { .kind = REQUEST_RUN, .job = fn, .arg = arg, .flag = without_chips };
    return call(&r, true, true, ms);
}

kode_status_t fck_aos_run_job(fck_aos_job_fn fn, void *arg, uint32_t ms)
{
    return job_request(fn, arg, ms, false);
}

kode_status_t fck_aos_run_job_without_chips(fck_aos_job_fn fn, void *arg, uint32_t ms)
{
    return job_request(fn, arg, ms, true);
}

kode_status_t fck_aos_command_post(fck_aos_chip_t chip, const char *line, const char *verb, uint32_t ms)
{
    if (!chip_ok(chip) || line == NULL || !fck_plan_command_line_ok(line) || verb == NULL || ms == 0U
        || ms > CALL_MS_MAX) {
        return fail(KODE_DENIED, "the chip is not FCK_AOS_C5 or FCK_AOS_NRF, the line is not 1 to 255 printable characters, the verb is NULL, or ms is 0 or above FCK_AOS_CALL_MS_MAX");
    }
    if (!s.started) {
        return fail(KODE_DENIED, "fck_begin() has not run, or fck_end() was called");
    }
    if (!s.state.present) {
        return fail(KODE_DENIED, "no F*CK is on J3");
    }
    size_t const line_n = strlen(line) + 1U;
    size_t const verb_n = strlen(verb) + 1U;
    /* cppcheck-suppress misra-c2012-21.3 ; the owner's request, freed by finish() */
    fck_aos_request_t *const q = calloc(1U, sizeof(*q) + line_n + verb_n);
    if (q == NULL) {
        return fail(KODE_NO_RESOURCES, "no memory for the request");
    }
    memcpy(q->data, line, line_n);
    memcpy(&q->data[line_n], verb, verb_n);
    q->kind     = REQUEST_COMMAND;
    q->chip     = chip;
    q->line     = q->data;
    q->verb     = &q->data[line_n];
    q->ms       = ms;
    q->st       = KODE_ERROR;
    q->rc       = -1;
    q->internal = true;
    q->cancel_mark = fck_aos_cancel_mark();
    q->done     = xSemaphoreCreateBinary();
    if (q->done == NULL) {
        /* cppcheck-suppress misra-c2012-21.3 ; as above */
        free(q);
        return fail(KODE_NO_RESOURCES, "no memory for the request");
    }
    kode_status_t const st = fck_ao_post(q);
    if (st != KODE_OK) {
        vSemaphoreDelete(q->done);
        /* cppcheck-suppress misra-c2012-21.3 ; as above */
        free(q);
        return fail(st, "the owner's queue had no room for the request");
    }
    return KODE_OK;
}

uint8_t fck_aos_header_position(uint8_t pos)
{
    /* The module is held upright only: a position is itself. A position
     * outside J3's range is not the module's, present or not. */
    if ((pos < 1U) || (pos > (uint8_t)FCK_BSP_POSITIONS)) {
        return 0U;
    }
    return fck_aos_present() ? pos : 0U;
}
