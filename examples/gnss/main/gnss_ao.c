/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file gnss_ao.c
 * @brief The GNSS receiver from an Active Object, the extended layer (fck_sys.h): started once the
 *        module is found, a fix read every second, its quality logged.
 *
 * Wire: the F*CK module on J3, and the Dot on a USB-C cable, somewhere with a
 * view of the sky: a first fix indoors can take minutes, or never come.
 *
 * Try it: once the module is found the receiver is taken out of reset
 * (fck_sys_gnss_begin()); from then on, every second, the latest fix is read
 * (fck_sys_gnss_read()) and the log says its quality, how many satellites it
 * uses, its dilution and the UTC time it carries. The position itself is
 * never logged: it is the Dot's, not the cable's. Pull the module out and the
 * reads stop; put it back and the receiver is started again. Holding the
 * bottom button goes home to kodeOS, as in every app (CONFIG_KODEOS_HOME_HOLD):
 * the owner parks the receiver on the way.
 *
 * Nothing transmits: a GNSS receiver only listens.
 *
 * ---- Nothing waits ----------------------------------------------------------
 *
 * A step of an Active Object never waits, and no call of fck_sys.h does. The module
 * being found is news (FCK_MODULE_FOUND_SIG), starting the receiver is a request answered on
 * FCK_GNSS_BEGIN_RSP_SIG, and the fix is an immediate read of the latest one. The pause between
 * reads is a QTimeEvt. No worker, no task of the app's own.
 *
 * The queue: one slot for the request's answer, one for the timer, two the news leave free; the
 * default 16 of KQP_AO_QUEUE_LEN is far more than this needs (see fck_sys.h, "Queue depth").
 */
#include "gnss_ao.h"

#include "kode_log.h"
#include "fck_sys.h"

#define TAG "gnss"

/** The pause between reads. */
#define GNSS_PAUSE_MS       1000U

/** @brief This AO's private signals. Posted to itself, never published. */
enum GnssSignals {
    GNSS_PAUSE_SIG = MAX_SIG   /**< The pause before the next read. */
};

/** @brief The AO's own state, private to this file. */
typedef struct {
    QActive        super;
    QTimeEvt       pause;    /**< One-shot, armed after each read.    */
    fck_gnss_fix_t fix;      /**< Filled by fck_sys_gnss_read().      */
} Gnss;

static QEvtPtr     l_queue[KQP_AO_QUEUE_LEN];
static StackType_t l_stack[KQP_AO_STACK_BYTES / sizeof(StackType_t)];

static Gnss l_ao;
QActive * const AO_Gnss = &l_ao.super;

static QState Gnss_initial(Gnss * const me, void const * const par);
static QState Gnss_waiting(Gnss * const me, QEvt const * const e);
static QState Gnss_starting(Gnss * const me, QEvt const * const e);
static QState Gnss_reading(Gnss * const me, QEvt const * const e);

/* ---- What a read found, on the log ----------------------------------------- */

/**
 * @brief The fix's quality, never its place.
 * @param f The fix.
 */
static void show(fck_gnss_fix_t const * const f)
{
    if ( 0U == f->quality )
    {
        KLOG_I(TAG, "no fix yet: %u satellite(s) in use%s", (unsigned)f->sats,
               f->has_time ? ", the time is in" : "");
    }
    else
    {
        KLOG_I(TAG, "fix%s: quality %u, %u satellites, HDOP %.1f, %.1f kn%s", f->valid ? "" : " (not vouched for)",
               (unsigned)f->quality, (unsigned)f->sats, f->hdop, f->speed_kn,
               f->has_time ? "" : ", no time");
    }
    if ( f->has_time )
    {
        KLOG_I(TAG, "UTC %02u:%02u:%02u%s", (unsigned)f->hour, (unsigned)f->min, (unsigned)f->sec,
               f->has_date ? " (dated)" : "");
    }
}

/**
 * @brief Ask for the receiver to start, and say so if the SDK would not take the request.
 * @param me The AO.
 * @return true when the answer will come.
 */
static bool ask_start(Gnss * const me)
{
    kode_status_t const rc = fck_sys_gnss_begin(&me->super);
    if ( KODE_OK != rc )
    {
        KLOG_W(TAG, "the request was not taken (status %d)", (int)rc);
    }
    return KODE_OK == rc;
}

void Gnss_ctor(void)
{
    Gnss * const me = &l_ao;
    QActive_ctor(&me->super, Q_STATE_CAST(&Gnss_initial));
    QTimeEvt_ctorX(&me->pause, &me->super, (enum_t)GNSS_PAUSE_SIG, 0U);
}

void Gnss_start(void)
{
    /* Name the FreeRTOS task after the AO. Without this the port calls every
     * one of them "AO", which is fine until something asserts. */
    QActive_setAttr(AO_Gnss, TASK_NAME_ATTR, "gnss");
    QACTIVE_START(AO_Gnss, KODE_PRIO_USER_LOW,
                  l_queue, Q_DIM(l_queue),
                  l_stack, sizeof(l_stack),
                  (void *)0);
}

/**
 * @brief The initial transition: subscribe to the news, then start the SDK.
 *
 * Runs in the thread that called QACTIVE_START(), the one that called
 * kode_init(). The subscription comes first, so the module being found is never missed.
 *
 * @param me  The AO.
 * @param par Unused.
 * @return The transition to Gnss_waiting.
 */
static QState Gnss_initial(Gnss * const me, void const * const par)
{
    (void)par;

    if ( KODE_OK != fck_sys_subscribe(&me->super) )
    {
        KLOG_E(TAG, "no subscription; the module will not be heard of");
    }
    kode_status_t const rc = fck_sys_begin();
    if ( KODE_OK != rc )
    {
        KLOG_E(TAG, "the SDK did not start (status %d)", (int)rc);
    }

    KLOG_I(TAG, "up; looking for the F*CK on J3");
    return Q_TRAN(&Gnss_waiting);
}

/**
 * @brief From waiting: ask for the start, or arm the pause to ask again when the SDK does not take it.
 * @param me The AO.
 * @return Q_TRAN() to Gnss_starting() when it was taken, Q_HANDLED() otherwise.
 */
static QState ask_start_or_pause(Gnss * const me)
{
    QState status;

    if ( ask_start(me) )
    {
        (void)QTimeEvt_disarm(&me->pause);
        status = Q_TRAN(&Gnss_starting);
    }
    else
    {
        (void)QTimeEvt_rearm(&me->pause, KQP_TICKS_PER_MS(GNSS_PAUSE_MS));
        status = Q_HANDLED();
    }

    return status;
}

/**
 * @brief No module, or the start not taken yet: the AO waits for the news, and for the pause to ask
 *        again.
 * @param me The AO.
 * @param e  The event.
 * @return Q_TRAN() once the start is taken, Q_HANDLED() for a pause, Q_SUPER() otherwise.
 */
static QState Gnss_waiting(Gnss * const me, QEvt const * const e)
{
    QState status;

    switch ( e->sig )
    {
        case FCK_MODULE_FOUND_SIG: {
            KLOG_I(TAG, "found; starting the receiver");
            status = ask_start_or_pause(me);
            break;
        }
        case GNSS_PAUSE_SIG: {
            /* The start the SDK did not take is asked again, while the module is there. */
            status = fck_sys_is_present() ? ask_start_or_pause(me) : Q_HANDLED();
            break;
        }
        default: {
            status = Q_SUPER(&QHsm_top);
            break;
        }
    }

    return status;
}

/**
 * @brief The receiver comes out of reset; the AO waits for the owner's word.
 * @param me The AO.
 * @param e  The event.
 * @return Q_TRAN() to reading or back to waiting, Q_SUPER() otherwise.
 */
static QState Gnss_starting(Gnss * const me, QEvt const * const e)
{
    QState status;

    switch ( e->sig )
    {
        case FCK_GNSS_BEGIN_RSP_SIG: {
            fck_sys_answer_t const * const a = Q_EVT_CAST(fck_sys_answer_t);
            if ( KODE_OK == a->rc )
            {
                KLOG_I(TAG, "the receiver runs; reading every second");
                (void)QTimeEvt_rearm(&me->pause, KQP_TICKS_PER_MS(GNSS_PAUSE_MS));
                status = Q_TRAN(&Gnss_reading);
            }
            else
            {
                /* The class of the failure, as a sentence; the receiver is asked again
                 * when the module is found again. */
                KLOG_W(TAG, "the receiver did not start: %s", fck_why_text(a->why));
                status = Q_TRAN(&Gnss_waiting);
            }
            break;
        }
        case FCK_MODULE_LOST_SIG: {
            KLOG_W(TAG, "the module left J3");
            status = Q_TRAN(&Gnss_waiting);
            break;
        }
        default: {
            status = Q_SUPER(&QHsm_top);
            break;
        }
    }

    return status;
}

/**
 * @brief The receiver runs: a read, a pause, a read.
 * @param me The AO.
 * @param e  The event.
 * @return Q_TRAN() back to waiting when the module leaves, Q_SUPER() otherwise.
 */
static QState Gnss_reading(Gnss * const me, QEvt const * const e)
{
    QState status = Q_HANDLED();

    switch ( e->sig )
    {
        case GNSS_PAUSE_SIG: {
            /* Immediate: a copy of the latest fix, never a wait. DENIED is "nothing yet". */
            if ( KODE_OK == fck_sys_gnss_read(&me->fix) )
            {
                show(&me->fix);
            }
            else
            {
                KLOG_I(TAG, "no sentence yet");
            }
            (void)QTimeEvt_rearm(&me->pause, KQP_TICKS_PER_MS(GNSS_PAUSE_MS));
            break;
        }
        case FCK_MODULE_LOST_SIG: {
            (void)QTimeEvt_disarm(&me->pause);
            KLOG_W(TAG, "the module left J3");
            status = Q_TRAN(&Gnss_waiting);
            break;
        }
        default: {
            status = Q_SUPER(&QHsm_top);
            break;
        }
    }

    return status;
}
