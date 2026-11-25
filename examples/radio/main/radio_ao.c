/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file radio_ao.c
 * @brief The nRF listening, from an Active Object, the extended layer (fck_sys.h): the 2.4 GHz
 *        band's level megahertz by megahertz, then the frames heard where it was busiest.
 *
 * Wire: the F*CK module on J3, and the Dot on a USB-C cable.
 *
 * Try it: once the module is found, every ten seconds the log says where the
 * band was busiest and at what level (fck_sys_radio_spectrum(), 2400 to 2483 MHz),
 * then how many frames the nRF heard there in two seconds and how many of
 * them were different (fck_sys_radio_sniff()). A frame's bytes belong to whoever
 * sent it and are not logged. Pull the module out and the rounds stop; put it
 * back and they start again. Holding the bottom button goes home to kodeOS,
 * as in every app (CONFIG_KODEOS_HOME_HOLD).
 *
 * NOTHING HERE TRANSMITS. fck_sys_radio_ping() and fck_sys_radio_echo_start() are
 * left out: both send. The two calls used listen.
 *
 * ---- Requests and answers ----------------------------------------------------
 *
 * A step of an Active Object never waits. Each call here is a request: it takes this AO, posts,
 * and returns at once, and the answer is an event of the AO's own (FCK_RADIO_SPECTRUM_RSP_SIG,
 * FCK_RADIO_SNIFF_RSP_SIG). What a request fills is LENT: it lives in the AO, untouched, from
 * the request to its answer, and is read once the answer is there. Both requests share one door
 * (RADIO), so the sniff is asked from the spectrum's answer, when the door is free again. The
 * pause between rounds is a QTimeEvt.
 */
#include "radio_ao.h"

#include "kode_log.h"
#include "fck_sys.h"

#define TAG "radio"

/** The spectrum: the whole band, how long on each MHz, how many sweeps. */
#define RADIO_FIRST_MHZ      2400U
#define RADIO_LAST_MHZ       2483U
#define RADIO_DWELL_US       200U
#define RADIO_SWEEPS         4U

/** The sniff: how long, and the weakest frame kept. */
#define RADIO_SNIFF_MS       2000U
#define RADIO_SNIFF_RSSI_MIN (-90)

/** The pause between rounds. */
#define RADIO_PAUSE_MS       10000U

/** @brief This AO's private signals. Posted to itself, never published. */
enum RadioSignals {
    RADIO_PAUSE_SIG = MAX_SIG   /**< The pause before the next round. */
};

/** @brief The AO's own state, private to this file. */
typedef struct {
    QActive           super;
    QTimeEvt          pause;       /**< One-shot, armed after each round.   */
    int8_t            dbm[RADIO_LAST_MHZ - RADIO_FIRST_MHZ + 1U]; /**< LENT to the spectrum, by MHz. */
    size_t            points;      /**< Of dbm, filled.                     */
    uint16_t          busiest_mhz; /**< Where the band peaked.              */
    int8_t            busiest_dbm;
    fck_radio_frame_t frames[16];  /**< LENT to the sniff.                  */
    fck_radio_sniff_statistics_t stats;  /**< LENT to the sniff.            */
} FckRadio;

static QEvtPtr     l_queue[KQP_AO_QUEUE_LEN];
static StackType_t l_stack[KQP_AO_STACK_BYTES / sizeof(StackType_t)];

static FckRadio l_ao;
QActive * const AO_FckRadio = &l_ao.super;

static QState FckRadio_initial(FckRadio * const me, void const * const par);
static QState FckRadio_waiting(FckRadio * const me, QEvt const * const e);
static QState FckRadio_listening(FckRadio * const me, QEvt const * const e);
static QState FckRadio_spectrum(FckRadio * const me, QEvt const * const e);
static QState FckRadio_sniffing(FckRadio * const me, QEvt const * const e);
static QState FckRadio_resting(FckRadio * const me, QEvt const * const e);

/**
 * @brief Ask for a round: the band swept first. Says so on the log if the SDK would not take it.
 * @param me The AO.
 * @return true when the answer will come.
 */
static bool ask_spectrum(FckRadio * const me)
{
    me->points = 0U;
    kode_status_t const rc = fck_sys_radio_spectrum(&me->super, RADIO_FIRST_MHZ, RADIO_LAST_MHZ,
                                                    RADIO_DWELL_US, RADIO_SWEEPS, me->dbm,
                                                    Q_DIM(me->dbm));
    if ( KODE_OK != rc )
    {
        KLOG_W(TAG, "the round was not taken (status %d)", (int)rc);
    }
    return KODE_OK == rc;
}

/**
 * @brief Find the busiest MHz of the sweep just answered.
 * @param me The AO, with dbm and points filled.
 */
static void find_busiest(FckRadio * const me)
{
    me->busiest_mhz = RADIO_FIRST_MHZ;
    me->busiest_dbm = FCK_RADIO_QUIET_DBM;
    for ( size_t i = 0U; i < me->points; i++ )
    {
        if ( me->dbm[i] > me->busiest_dbm )
        {
            me->busiest_dbm = me->dbm[i];
            me->busiest_mhz = (uint16_t)(RADIO_FIRST_MHZ + i);
        }
    }
}

void FckRadio_ctor(void)
{
    FckRadio * const me = &l_ao;
    QActive_ctor(&me->super, Q_STATE_CAST(&FckRadio_initial));
    QTimeEvt_ctorX(&me->pause, &me->super, (enum_t)RADIO_PAUSE_SIG, 0U);
    me->points = 0U;
}

void FckRadio_start(void)
{
    /* Name the FreeRTOS task after the AO. Without this the port calls every
     * one of them "AO", which is fine until something asserts. */
    QActive_setAttr(AO_FckRadio, TASK_NAME_ATTR, "radio");
    QACTIVE_START(AO_FckRadio, KODE_PRIO_USER_LOW,
                  l_queue, Q_DIM(l_queue),
                  l_stack, sizeof(l_stack),
                  (void *)0);
}

/**
 * @brief The initial transition: subscribe to the news, then start the SDK.
 *
 * Runs in the thread that called QACTIVE_START(), the one that called kode_init(). The
 * subscription comes first, so the module being found is never missed.
 *
 * @param me  The AO.
 * @param par Unused.
 * @return The transition to FckRadio_waiting.
 */
static QState FckRadio_initial(FckRadio * const me, void const * const par)
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
    return Q_TRAN(&FckRadio_waiting);
}

/**
 * @brief From waiting: ask for a round, or arm the pause to ask again when the SDK does not take it.
 * @param me The AO.
 * @return Q_TRAN() to FckRadio_spectrum() when it was taken, Q_HANDLED() otherwise.
 */
static QState ask_spectrum_or_pause(FckRadio * const me)
{
    QState status;

    if ( ask_spectrum(me) )
    {
        (void)QTimeEvt_disarm(&me->pause);
        status = Q_TRAN(&FckRadio_spectrum);
    }
    else
    {
        (void)QTimeEvt_rearm(&me->pause, KQP_TICKS_PER_MS(RADIO_PAUSE_MS));
        status = Q_HANDLED();
    }

    return status;
}

/**
 * @brief No module, or a round not taken yet: the AO waits for the news, and for the pause to ask
 *        again.
 * @param me The AO.
 * @param e  The event.
 * @return Q_TRAN() once a round is taken, Q_HANDLED() for a pause, Q_SUPER() otherwise.
 */
static QState FckRadio_waiting(FckRadio * const me, QEvt const * const e)
{
    QState status;

    switch ( e->sig )
    {
        case FCK_MODULE_FOUND_SIG: {
            KLOG_I(TAG, "found; the nRF listens");
            status = ask_spectrum_or_pause(me);
            break;
        }
        case RADIO_PAUSE_SIG: {
            /* A round the SDK did not take is asked again, while the module is there. */
            status = fck_sys_is_present() ? ask_spectrum_or_pause(me) : Q_HANDLED();
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
 * @brief The module is there. If it leaves, whatever was asked is answered by the SDK and the AO
 *        goes back to waiting; the pause is stopped.
 * @param me The AO.
 * @param e  The event.
 * @return Q_TRAN() back to waiting, Q_SUPER() otherwise.
 */
static QState FckRadio_listening(FckRadio * const me, QEvt const * const e)
{
    QState status;

    switch ( e->sig )
    {
        case FCK_MODULE_LOST_SIG: {
            (void)QTimeEvt_disarm(&me->pause);
            KLOG_W(TAG, "the module left J3");
            status = Q_TRAN(&FckRadio_waiting);
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
 * @brief The band is being swept; its answer starts the sniff on the busiest MHz.
 * @param me The AO.
 * @param e  The event.
 * @return Q_TRAN() to the sniff or to resting, Q_SUPER() otherwise.
 */
static QState FckRadio_spectrum(FckRadio * const me, QEvt const * const e)
{
    QState status;

    switch ( e->sig )
    {
        case FCK_RADIO_SPECTRUM_RSP_SIG: {
            fck_sys_answer_t const * const a = Q_EVT_CAST(fck_sys_answer_t);
            if ( KODE_OK == a->rc )
            {
                me->points = a->count;
                find_busiest(me);
                KLOG_I(TAG, "busiest at %u MHz, %d dBm over %u points", (unsigned)me->busiest_mhz,
                       (int)me->busiest_dbm, (unsigned)me->points);
                /* The door of this kind is free again: ask the next from here. */
                if ( KODE_OK == fck_sys_radio_sniff(&me->super, me->busiest_mhz, RADIO_SNIFF_MS,
                                                    RADIO_SNIFF_RSSI_MIN, me->frames,
                                                    Q_DIM(me->frames), &me->stats) )
                {
                    status = Q_TRAN(&FckRadio_sniffing);
                    break;
                }
                KLOG_W(TAG, "the sniff was not taken");
            }
            else
            {
                KLOG_W(TAG, "the sweep was refused: %s", fck_why_text(a->why));
            }
            (void)QTimeEvt_rearm(&me->pause, KQP_TICKS_PER_MS(RADIO_PAUSE_MS));
            status = Q_TRAN(&FckRadio_resting);
            break;
        }
        default: {
            status = Q_SUPER(&FckRadio_listening);
            break;
        }
    }

    return status;
}

/**
 * @brief The nRF listens on one MHz; its answer closes the round.
 * @param me The AO.
 * @param e  The event.
 * @return Q_TRAN() to resting, Q_SUPER() otherwise.
 */
static QState FckRadio_sniffing(FckRadio * const me, QEvt const * const e)
{
    QState status;

    switch ( e->sig )
    {
        case FCK_RADIO_SNIFF_RSP_SIG: {
            fck_sys_answer_t const * const a = Q_EVT_CAST(fck_sys_answer_t);
            if ( KODE_OK == a->rc )
            {
                KLOG_I(TAG, "heard there in %lu ms: %lu frames, %u different, %lu too weak",
                       (unsigned long)me->stats.ms, (unsigned long)me->stats.heard,
                       (unsigned)me->stats.distinct, (unsigned long)me->stats.weak);
            }
            else
            {
                KLOG_W(TAG, "the sniff was refused: %s", fck_why_text(a->why));
            }
            (void)QTimeEvt_rearm(&me->pause, KQP_TICKS_PER_MS(RADIO_PAUSE_MS));
            status = Q_TRAN(&FckRadio_resting);
            break;
        }
        default: {
            status = Q_SUPER(&FckRadio_listening);
            break;
        }
    }

    return status;
}

/**
 * @brief The pause between rounds.
 * @param me The AO.
 * @param e  The event.
 * @return Q_TRAN() to a new round, Q_HANDLED() if it could not be asked (tried again after
 *         the next pause), Q_SUPER() otherwise.
 */
static QState FckRadio_resting(FckRadio * const me, QEvt const * const e)
{
    QState status;

    switch ( e->sig )
    {
        case RADIO_PAUSE_SIG: {
            if ( ask_spectrum(me) )
            {
                status = Q_TRAN(&FckRadio_spectrum);
            }
            else
            {
                (void)QTimeEvt_rearm(&me->pause, KQP_TICKS_PER_MS(RADIO_PAUSE_MS));
                status = Q_HANDLED();
            }
            break;
        }
        default: {
            status = Q_SUPER(&FckRadio_listening);
            break;
        }
    }

    return status;
}
