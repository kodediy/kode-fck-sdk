/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file fck_ao.c
 * @brief AO_Fck: the module's owner as an Active Object of kode-sdk.
 *        See fck_ao.h.
 *
 * kode_sys.h's promise, taken at its word: an Active Object written outside
 * the SDK, on the same QP/C, the same pools and the same bus as kodeOS's own.
 * kode-sdk gives J3 no owner (its README: "the expansion header J3 has no
 * owner"), so a module's firmware is the module's owner. This is the F*CK's:
 * everything else asks it with an event, and it takes them one at a time,
 * each step to completion.
 *
 * ---- Its level: KODE_PRIO_USER_HIGH, and why ------------------------------
 *
 * kode-sdk leaves an app three levels and asks a reason for the high one -
 * "only if you can state why the drawing may be delayed". Two:
 *
 *  - An owner's step is short. A tick reads what the two UARTs hold and
 *    hands each line on; a request is a line written, or an expander pin.
 *    What waits - finding the module, the GNSS receiver let go of the bus,
 *    a chip's flash written - runs on the worker below, and a call that
 *    wants an answer waits on the caller's task, never on this one.
 *  - A round of rest (LOWPOWER_ROUND_SIG) gives every owner one
 *    run-to-completion step to leave its device safe: the CC1101 asleep, the
 *    GNSS receiver in reset and both chips' radios off.
 *
 * It leaves KODE_PRIO_USER_LOW, where kode-sdk tells an app to start its own
 * Active Object, to the app, and never takes level 3, which the standard
 * layer of kode-sdk starts its own at. An app that needs the high level for
 * itself moves this one with CONFIG_FCK_AO_PRIORITY. With
 * CONFIG_KODE_SCHEDULER KODE_PRIO_USER_HIGH does not exist: the owner then
 * refuses to start and says to set CONFIG_FCK_AO_PRIORITY. Two Active Objects
 * at one level stop the board at the second QACTIVE_START(), which is why
 * kqp_prio_taken() is asked first.
 *
 * ---- What the system tells it ----------------------------------------------
 *
 *  - EXP_POWER_SIG: a switch of J3's supply is over (published by AO_Radio).
 *  - HOME_SIG: the bottom button was held, and the board resets into kodeOS
 *    KODE_SYS_HOME_GRACE_MS later, whatever the app is doing.
 *  - With CONFIG_KODE_LOWPOWER, LOWPOWER_ROUND_SIG and LOWPOWER_STATE_SIG: the
 *    board is going to rest (the module is put down) and is awake again (the
 *    module is set up again and what the app had set is given back, without
 *    emitting). The owner does not join AO_LowPower's rounds (that is kaos.h's
 *    lowpower_join(), which an app does not include): it acts on the signals.
 *
 * HOME parks the module as a restart does (fck_aos.h, "Going home"), and from
 * then on nothing asked of the owner is done. A rest parks it the same way but
 * can be undone. esp_restart() is seen through ESP-IDF's restart hook
 * (fck_drv_system.h), which parks it too.
 *
 * ---- Its worker, and why it is not kode_worker.h's ---------------------------
 *
 * kode_worker.h's task has no core: it runs on whichever is free. The
 * module's I2C bus has to be created from a task on core 1 (fck_drv_tca6408a.h:
 * created on core 0, a write to an absent address right after a NAK'd probe
 * can come back as ESP_OK), and finding the module, and
 * getting the bus back from the GNSS receiver, create it. So the worker here
 * is a task of the owner's own, pinned to core 1, at KODE_PRIO_USER_LOW,
 * with its stack in internal RAM, that answers the AO the way kode_worker.h's
 * does: one done event per job, waiting for a block and for room as long as
 * it takes. The UARTs and the CC1101's SPI are opened from it too, so every
 * interrupt the module's drivers take is on core 1, as before the owner was
 * an Active Object.
 *
 * ---- What it takes from the system -----------------------------------------
 *
 *  - Per request, an event from kode-sdk's pools, back in the pool once
 *    served: taken only while the pool keeps AO_POOL_MARGIN blocks for the
 *    rest of the system. A dry pool is a refused request, never QP/C's
 *    assertion.
 *  - Its queue, AO_QUEUE_LENGTH deep, posted to only while AO_QUEUE_MARGIN
 *    slots stay free. The tick, EXP_POWER_SIG, the rest signals and HOME_SIG
 *    arrive without asking and with no margin: one of them finding the queue full would be
 *    the assertion, which is why the tick is one shot (below). The worker's
 *    answer waits for room on the worker's task.
 *  - Its own stack, AO_STACK_BYTES of internal RAM, kode-sdk's default for
 *    an AO; the worker's, WORKER_STACK_BYTES, also internal.
 *
 * ---- One tick in the queue at most -----------------------------------------
 *
 * The tick is a one-shot time event, armed again each time it is served, not
 * a periodic one. kode-sdk's QF tick is an esp_timer, and an esp_timer held
 * back fires every tick it missed back to back: a periodic time event would
 * then post one per FCK_AO_TICK_MS into a queue no one drains meanwhile, and
 * with AO_QUEUE_LENGTH 16 a hold of 160 ms is QP/C's assertion
 * (qf_port:130). A late tick
 * costs nothing here: fck_aos_on_tick() goes by the time that really passed,
 * its deadlines FreeRTOS ticks, not by FCK_AO_TICK_MS.
 *
 * kode-sdk's watch on every Active Object (CONFIG_KQP_AO_WATCH) times this
 * one's steps as it times kodeOS's. The longest is the nRF's reset, 20 ms
 * held (fck_aos.c), far from CONFIG_KQP_AO_STEP_MAX_MS.
 */
#include "fck_ao.h"

#include <stddef.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "kode_sys.h"
#include "kode_log.h"

#include "sdkconfig.h"

#define TAG "fck"

#if defined(CONFIG_FCK_AO_PRIORITY) && (CONFIG_FCK_AO_PRIORITY > 0)
#define AO_PRIORITY      ((uint8_t)CONFIG_FCK_AO_PRIORITY)
#elif defined(KODE_PRIO_USER_HIGH)
#define AO_PRIORITY      ((uint8_t)KODE_PRIO_USER_HIGH)
#else
/* CONFIG_KODE_SCHEDULER takes the level: 0 is "none", and the start says why. */
#define AO_PRIORITY      0U
#endif
/* The level the standard layer of kode-sdk starts its own Active Object at. */
#define AO_PRIORITY_TAKEN_BY_KODE 3U

#define AO_QUEUE_LENGTH     KQP_AO_QUEUE_LEN
#define AO_QUEUE_MARGIN     4U  /* left after a request: a tick, a sleep, a job's answer, and one */
#define AO_POOL_MARGIN      8U  /* blocks a request leaves the system */
#define AO_STACK_BYTES      KQP_AO_STACK_BYTES
#define WORKER_STACK_BYTES  6144U   /* the owner's task had as much, and ran these jobs */
#define WORKER_PRIORITY     ((UBaseType_t)KODE_PRIO_USER_LOW)
#define WORKER_CORE         1       /* fck_drv_tca6408a.h */
#define WORKER_DEPTH        1U      /* one job waiting besides the one running */

/* The owner's private signals sit in the F*CK's own range (fck_sys.h): MAX_SIG +
 * CONFIG_FCK_SIG_OFFSET, the first 90 for fck_sys.h's, these after them. */
#ifndef CONFIG_FCK_SIG_OFFSET
#define CONFIG_FCK_SIG_OFFSET 1300
#endif
#define FCK_OWNER_SIG_FIRST ((enum_t)(MAX_SIG + CONFIG_FCK_SIG_OFFSET + 90))

/** @brief The owner's private signals: posted, never published (kode_sys.h). */
enum FckSignals {
    FCK_REQUEST_SIG = FCK_OWNER_SIG_FIRST, /**< A request (FckRequestEvt).   */
    FCK_TICK_SIG,              /**< The time event, FCK_AO_TICK_MS after the last was served. */
    FCK_WORK_DONE_SIG,         /**< The worker's answer (FckWorkDoneEvt). */
};

/** @brief A request: the pointer only, since its caller waits for the answer. */
typedef struct {
    QEvt               super;
    fck_aos_request_t *request;
} FckRequestEvt;

/** @brief A job ended on the worker. */
typedef struct {
    QEvt          super;
    uint16_t      seq;
    kode_status_t status;
    uint32_t      value;
} FckWorkDoneEvt;

typedef struct {
    QActive  super;
    QTimeEvt tick;
} Fck;

/** @brief A job waiting for the worker. */
typedef struct {
    fck_ao_job_t fn;
    void        *arg;
    uint16_t     seq;
} FckJob;

static QEvtPtr       l_queue[AO_QUEUE_LENGTH];
static StackType_t   l_stack[AO_STACK_BYTES / sizeof(StackType_t)];
static Fck           l_ao;
static bool          s_ao_started;

static QueueHandle_t l_jobs;
static TaskHandle_t  l_worker;
static uint16_t      l_job_seq;

static QActive * const AO_Fck = &l_ao.super;

_Static_assert((MAX_SIG + 1300 + 99) <= 0xFFFF, "the F*CK's signals fit in QSignal");

static QState Fck_initial(Fck * const me, void const * const par);
static QState Fck_running(Fck * const me, QEvt const * const e);

/**
 * @brief The worker: one job at a time, and one done event for each.
 *
 * The answer is never dropped: a job that ran must be heard of, or the owner
 * would wait for it for ever. So it waits for a block and for room, as
 * kode_worker.h's does, and takes no other job meanwhile.
 */
static void worker_task(void *unused)
{
    (void)unused;
    for (;;) {
        FckJob job;
        if (xQueueReceive(l_jobs, &job, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        uint32_t            value  = 0U;
        kode_status_t const status = job.fn(job.arg, &value);
        bool                posted = false;
        while (!posted) {
            FckWorkDoneEvt *const e = Q_NEW_X(FckWorkDoneEvt, 0U, FCK_WORK_DONE_SIG);
            if (e != NULL) {
                e->seq    = job.seq;
                e->status = status;
                e->value  = value;
                /* Not posted, QActive_post_() hands the block back to its pool. */
                posted = QACTIVE_POST_X(AO_Fck, &e->super, 0U, (void *)0);
            }
            if (!posted) {
                vTaskDelay(1);
            }
        }
    }
}

/**
 * @brief The Active Object made and its worker started, all but the AO's task.
 * @return KODE_OK; KODE_NO_RESOURCES as fck_ao_start().
 */
static kode_status_t Fck_ctor(void)
{
    /* A pool too narrow would be QF_newX_()'s assertion at the first request. */
    size_t const biggest = (sizeof(FckWorkDoneEvt) > sizeof(FckRequestEvt))
                           ? sizeof(FckWorkDoneEvt) : sizeof(FckRequestEvt);
    if (biggest > (size_t)QF_poolGetMaxBlockSize()) {
        KLOG_E(TAG, "an event (%u bytes) fits no event pool (%u at most)",
               (unsigned)biggest, (unsigned)QF_poolGetMaxBlockSize());
        return KODE_NO_RESOURCES;
    }
    Fck * const me = &l_ao;
    QActive_ctor(&me->super, Q_STATE_CAST(&Fck_initial));
    QTimeEvt_ctorX(&me->tick, &me->super, (enum_t)FCK_TICK_SIG, 0U);

    /* Before the start: the AO's first tick may already want the worker. */
    l_jobs = xQueueCreate(WORKER_DEPTH, sizeof(FckJob));
    if ((l_jobs == NULL)
        || (xTaskCreatePinnedToCore(worker_task, "fck_work", WORKER_STACK_BYTES, NULL,
                                    WORKER_PRIORITY, &l_worker, WORKER_CORE) != pdPASS)) {
        KLOG_E(TAG, "the owner's worker could not start");
        if (l_jobs != NULL) {
            vQueueDelete(l_jobs);
            l_jobs = NULL;
        }
        return KODE_NO_RESOURCES;
    }
    return KODE_OK;
}

kode_status_t fck_ao_start(void)
{
    if (s_ao_started) {
        return KODE_OK;
    }
    /* Before anything is made: two Active Objects at one level stop the board
     * inside QACTIVE_START(). A level that is taken is said, and refused. */
    if (AO_PRIORITY == 0U) {
        KLOG_E(TAG, "KODE_PRIO_USER_HIGH does not exist with CONFIG_KODE_SCHEDULER: set CONFIG_FCK_AO_PRIORITY "
                    "to a level nothing in this image holds");
        return KODE_DENIED;
    }
    if (AO_PRIORITY == AO_PRIORITY_TAKEN_BY_KODE) {
        KLOG_E(TAG, "priority 3 is the one kode-sdk's standard layer starts its own Active Object at: "
                    "CONFIG_FCK_AO_PRIORITY moves this one");
        return KODE_DENIED;
    }
    if ((AO_PRIORITY < 1U) || (AO_PRIORITY > (uint8_t)QF_MAX_ACTIVE)) {
        KLOG_E(TAG, "CONFIG_FCK_AO_PRIORITY %u is not a QP/C level (1 to %u)", (unsigned)AO_PRIORITY,
               (unsigned)QF_MAX_ACTIVE);
        return KODE_DENIED;
    }
    const char *holder = "?";
    if (kqp_prio_taken(AO_PRIORITY, &holder)) {
        KLOG_E(TAG, "priority %u is taken by %s: CONFIG_FCK_AO_PRIORITY moves this one",
               (unsigned)AO_PRIORITY, holder);
        return KODE_EXISTS;
    }
    kode_status_t const rc = Fck_ctor();
    if (rc != KODE_OK) {
        return rc;
    }

    /* Said before, not after: a level already taken stops the board inside
     * QACTIVE_START(), and this is then the line above it. */
    KLOG_I(TAG, "the owner starts at priority %u (CONFIG_FCK_AO_PRIORITY moves it)",
           (unsigned)AO_PRIORITY);
    /* Named, so a full queue or an assertion names this owner and not "AO". */
    QActive_setAttr(AO_Fck, TASK_NAME_ATTR, "fck");
    QACTIVE_START(AO_Fck, AO_PRIORITY,
                  l_queue, Q_DIM(l_queue),
                  l_stack, sizeof(l_stack),
                  (void *)0);
    s_ao_started = true;
    return KODE_OK;
}

kode_status_t fck_ao_post(fck_aos_request_t *request)
{
    if (!s_ao_started || (request == NULL)) {
        return KODE_DENIED;
    }
    FckRequestEvt *const e = Q_NEW_X(FckRequestEvt, AO_POOL_MARGIN, FCK_REQUEST_SIG);
    if (e == NULL) {
        return KODE_NO_RESOURCES;
    }
    e->request = request;
    /* Not posted, QActive_post_() hands the block back to its pool. */
    return QACTIVE_POST_X(AO_Fck, &e->super, AO_QUEUE_MARGIN, (void *)0)
           ? KODE_OK : KODE_NO_RESOURCES;
}

bool fck_ao_is_own_task(void)
{
    return s_ao_started && (xTaskGetCurrentTaskHandle() == l_ao.super.thread);
}

bool fck_ao_is_any_ao_task(const char **name)
{
    return kqp_current_is_ao(name);
}

bool fck_ao_kode_is_up(void)
{
    return kqp_prio_taken((uint8_t)KODE_PRIO_BOOT, NULL);
}

bool fck_ao_is_worker_task(void)
{
    return (l_worker != NULL) && (xTaskGetCurrentTaskHandle() == l_worker);
}

void fck_ao_lift_to_caller(void)
{
    if (!s_ao_started) {
        return;
    }
    UBaseType_t const mine = uxTaskPriorityGet(NULL);
    if (mine > uxTaskPriorityGet(l_ao.super.thread)) {
        vTaskPrioritySet(l_ao.super.thread, mine);
    }
}

kode_status_t fck_ao_submit_work(fck_ao_job_t fn, void *arg, uint16_t *seq)
{
    if ((fn == NULL) || (l_jobs == NULL)) {
        return KODE_DENIED;
    }
    l_job_seq++;
    FckJob const job = { .fn = fn, .arg = arg, .seq = l_job_seq };
    if (xQueueSend(l_jobs, &job, 0) != pdTRUE) {
        return KODE_NO_RESOURCES;
    }
    if (seq != NULL) {
        *seq = job.seq;
    }
    return KODE_OK;
}

/**
 * @brief The initial transition: the system's two signals for an owner, then run.
 * @param me  The AO.
 * @param par Unused.
 * @return The transition to Fck_running.
 */
static QState Fck_initial(Fck * const me, void const * const par)
{
    (void)par;
    /* EXP_POWER_SIG before the first kode_sys_exp_power(): the answer must find a subscriber. */
    QActive_subscribe(&me->super, EXP_POWER_SIG);
    QActive_subscribe(&me->super, HOME_SIG);
#if CONFIG_KODE_LOWPOWER
    QActive_subscribe(&me->super, LOWPOWER_ROUND_SIG);
    QActive_subscribe(&me->super, LOWPOWER_STATE_SIG);
#endif
    return Q_TRAN(&Fck_running);
}

/**
 * @brief The one state: requests as they come, a tick FCK_AO_TICK_MS after the last.
 * @param me The AO.
 * @param e  The event.
 * @return Q_HANDLED() for what it handles, Q_SUPER() otherwise.
 */
static QState Fck_running(Fck * const me, QEvt const * const e)
{
    QState status;

    switch (e->sig) {
    case Q_ENTRY_SIG:
        /* One shot, armed again as each is served: never more than one in the
         * queue (the file's head, "One tick in the queue at most"). */
        QTimeEvt_armX(&me->tick, KQP_TICKS_PER_MS(FCK_AO_TICK_MS), 0U);
        status = Q_HANDLED();
        break;
    case Q_EXIT_SIG:
        (void)QTimeEvt_disarm(&me->tick);
        status = Q_HANDLED();
        break;
    case FCK_REQUEST_SIG:
        fck_aos_on_request(((FckRequestEvt const *)e)->request);
        status = Q_HANDLED();
        break;
    case FCK_TICK_SIG:
        QTimeEvt_armX(&me->tick, KQP_TICKS_PER_MS(FCK_AO_TICK_MS), 0U);
        fck_aos_on_tick();
        status = Q_HANDLED();
        break;
    case FCK_WORK_DONE_SIG: {
        FckWorkDoneEvt const * const d = (FckWorkDoneEvt const *)e;
        fck_aos_on_work_done(d->seq, d->status, d->value);
        status = Q_HANDLED();
        break;
    }
    case EXP_POWER_SIG: {
        kode_sys_exp_power_done_t const * const d = (kode_sys_exp_power_done_t const *)e;
        fck_aos_on_exp_power(d->on, d->rc);
        status = Q_HANDLED();
        break;
    }
#if CONFIG_KODE_LOWPOWER
    case LOWPOWER_ROUND_SIG:
        /* A round for ASLEEP or OFF: the module is put down. The owner is not in the round
         * (lowpower_join() is kaos.h's), so it answers nothing and cannot refuse. */
        if ((((LowPowerRoundEvt const *)e)->target == (uint8_t)KODE_LP_ASLEEP)
            || (((LowPowerRoundEvt const *)e)->target == (uint8_t)KODE_LP_OFF)) {
            fck_aos_on_rest(((LowPowerRoundEvt const *)e)->target == (uint8_t)KODE_LP_OFF);
        }
        status = Q_HANDLED();
        break;
    case LOWPOWER_STATE_SIG:
        /* AWAKE repeats (a round abandoned comes with prev AWAKE): the restore is idempotent.
         * Nothing is armed on ASLEEP, and RADIO is the radio owners' alone. */
        if (((LowPowerStateEvt const *)e)->state == (uint8_t)KODE_LP_AWAKE) {
            fck_aos_on_wake();
        }
        status = Q_HANDLED();
        break;
#endif
    case HOME_SIG:
        fck_aos_on_home();
        status = Q_HANDLED();
        break;
    default:
        status = Q_SUPER(&QHsm_top);
        break;
    }
    return status;
}
