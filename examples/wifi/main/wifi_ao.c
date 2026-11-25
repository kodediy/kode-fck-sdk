/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file wifi_ao.c
 * @brief The C5 listening, from an Active Object, the extended layer (fck_sys.h): how busy each
 *        2.4 GHz Wi-Fi channel is, and which BLE devices are around.
 *
 * Wire: the F*CK module on J3, and the Dot on a USB-C cable.
 *
 * Try it: once the module is found, every ten seconds the log says how many
 * frames each channel carried while the C5 listened on it (fck_sys_wifi_channels())
 * and how many BLE devices advertised in five seconds (fck_sys_ble_scan()), with
 * how many of them carried a manufacturer's data (fck_ble_manufacturer_data()).
 * What belongs to somebody else - a network's name, a device's address, what
 * it advertised - is counted, never logged. Pull the module out and the rounds stop; put it back
 * and they start again. Holding the bottom button goes home to kodeOS, as in
 * every app (CONFIG_KODEOS_HOME_HOLD).
 *
 * NOTHING HERE TRANSMITS. fck_sys_wifi_scan() is left out on purpose: a scan
 * sends probe requests. The two calls used listen - the C5 counts what it
 * hears on a channel, and scans BLE passively, the way the module's radio limits
 * ask (c5/README.md).
 *
 * ---- Requests and answers ----------------------------------------------------
 *
 * A step of an Active Object never waits. Each call here is a request: it takes this AO, posts,
 * and returns at once, and the answer is an event of the AO's own (FCK_WIFI_CHANNELS_RSP_SIG,
 * FCK_BLE_SCAN_RSP_SIG). The channels are written by the SDK into a buffer the AO LENDS: it
 * stays in the AO, untouched, from the request to its answer. A BLE scan also posts one
 * FCK_BLE_SCAN_ENTRY_SIG per device, with what the device advertised: this example reads each
 * with one of fck.h's pure helpers, which never wait and so are an Active Object's to call, and
 * keeps a count and nothing of the device (the entries cost the queue FCK_SYS_ENTRY_MARGIN
 * slots and no more, see fck_sys.h, "Queue depth"). The two requests share one door (SCAN), so the
 * second is asked from the first one's answer, when the door is free again. The pause between
 * rounds is a QTimeEvt.
 */
#include "wifi_ao.h"

#include "kode_log.h"
#include "fck_sys.h"

#define TAG "wifi"

/** A round: how long the C5 listens on each channel, and for BLE. */
#define WIFI_DWELL_MS       200U
#define WIFI_BLE_SECONDS    5U

/** The pause between rounds. */
#define WIFI_PAUSE_MS       10000U

/** @brief This AO's private signals. Posted to itself, never published. */
enum WifiSignals {
    WIFI_PAUSE_SIG = MAX_SIG   /**< The pause before the next round. */
};

/** @brief The AO's own state, private to this file. */
typedef struct {
    QActive            super;
    QTimeEvt           pause;      /**< One-shot, armed after each round.    */
    fck_wifi_channel_t channels[FCK_WIFI_CHANNEL_2G_MAX]; /**< LENT to the channels request. */
    size_t             channel_count;
    uint32_t           ble_with_manufacturer; /**< Of this round's devices, those that carried one. */
} Wifi;

static QEvtPtr     l_queue[KQP_AO_QUEUE_LEN];
static StackType_t l_stack[KQP_AO_STACK_BYTES / sizeof(StackType_t)];

static Wifi l_ao;
QActive * const AO_Wifi = &l_ao.super;

static QState Wifi_initial(Wifi * const me, void const * const par);
static QState Wifi_waiting(Wifi * const me, QEvt const * const e);
static QState Wifi_listening(Wifi * const me, QEvt const * const e);
static QState Wifi_channels(Wifi * const me, QEvt const * const e);
static QState Wifi_ble(Wifi * const me, QEvt const * const e);
static QState Wifi_resting(Wifi * const me, QEvt const * const e);

/* ---- What a round found, on the log ---------------------------------------- */

/**
 * @brief The channels' counts and the strongest heard on each. Names and addresses stay where
 *        they are.
 * @param me The AO.
 */
static void show_channels(Wifi const * const me)
{
    for ( size_t i = 0U; i < me->channel_count; i++ )
    {
        fck_wifi_channel_t const * const c = &me->channels[i];
        if ( 0U == c->frames )
        {
            KLOG_I(TAG, "channel %2u: quiet", (unsigned)c->channel);
        }
        else
        {
            KLOG_I(TAG, "channel %2u: %lu frames in %u ms, strongest %d dBm",
                   (unsigned)c->channel, (unsigned long)c->frames, (unsigned)c->dwell_ms,
                   (int)c->rssi_max);
        }
    }
}

/**
 * @brief Ask for a round: the channels first. Says so on the log if the SDK would not take it.
 * @param me The AO.
 * @return true when the answer will come.
 */
static bool ask_channels(Wifi * const me)
{
    me->channel_count = 0U;
    kode_status_t const rc = fck_sys_wifi_channels(&me->super, FCK_BAND_2G, WIFI_DWELL_MS,
                                                   me->channels, Q_DIM(me->channels));
    if ( KODE_OK != rc )
    {
        KLOG_W(TAG, "the round was not taken (status %d)", (int)rc);
    }
    return KODE_OK == rc;
}

void Wifi_ctor(void)
{
    Wifi * const me = &l_ao;
    QActive_ctor(&me->super, Q_STATE_CAST(&Wifi_initial));
    QTimeEvt_ctorX(&me->pause, &me->super, (enum_t)WIFI_PAUSE_SIG, 0U);
    me->channel_count = 0U;
}

void Wifi_start(void)
{
    /* Name the FreeRTOS task after the AO. Without this the port calls every
     * one of them "AO", which is fine until something asserts. */
    QActive_setAttr(AO_Wifi, TASK_NAME_ATTR, "wifi");
    QACTIVE_START(AO_Wifi, KODE_PRIO_USER_LOW,
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
 * @return The transition to Wifi_waiting.
 */
static QState Wifi_initial(Wifi * const me, void const * const par)
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
    return Q_TRAN(&Wifi_waiting);
}

/**
 * @brief No module: the AO waits for the news.
 * @param me The AO.
 * @param e  The event.
 * @return Q_TRAN() once the module is found, Q_SUPER() otherwise.
 */
static QState Wifi_waiting(Wifi * const me, QEvt const * const e)
{
    QState status;

    switch ( e->sig )
    {
        case FCK_MODULE_FOUND_SIG: {
            KLOG_I(TAG, "found; the C5 listens");
            status = ask_channels(me) ? Q_TRAN(&Wifi_channels) : Q_HANDLED();
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
static QState Wifi_listening(Wifi * const me, QEvt const * const e)
{
    QState status;

    switch ( e->sig )
    {
        case FCK_MODULE_LOST_SIG: {
            (void)QTimeEvt_disarm(&me->pause);
            KLOG_W(TAG, "the module left J3");
            status = Q_TRAN(&Wifi_waiting);
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
 * @brief The channels are being counted; their answer starts the BLE listen.
 * @param me The AO.
 * @param e  The event.
 * @return Q_TRAN() to the BLE listen or to resting, Q_SUPER() otherwise.
 */
static QState Wifi_channels(Wifi * const me, QEvt const * const e)
{
    QState status;

    switch ( e->sig )
    {
        case FCK_WIFI_CHANNELS_RSP_SIG: {
            fck_sys_answer_t const * const a = Q_EVT_CAST(fck_sys_answer_t);
            if ( KODE_OK == a->rc )
            {
                me->channel_count = a->count;
                show_channels(me);
                /* The door of this kind is free again: ask the next from here. */
                if ( KODE_OK == fck_sys_ble_scan(&me->super, WIFI_BLE_SECONDS) )
                {
                    me->ble_with_manufacturer = 0U;
                    status = Q_TRAN(&Wifi_ble);
                    break;
                }
                KLOG_W(TAG, "the BLE listen was not taken");
            }
            else
            {
                KLOG_W(TAG, "the channels were refused: %s", fck_why_text(a->why));
            }
            (void)QTimeEvt_rearm(&me->pause, KQP_TICKS_PER_MS(WIFI_PAUSE_MS));
            status = Q_TRAN(&Wifi_resting);
            break;
        }
        default: {
            status = Q_SUPER(&Wifi_listening);
            break;
        }
    }

    return status;
}

/**
 * @brief The BLE listen runs; its answer closes the round.
 * @param me The AO.
 * @param e  The event.
 * @return Q_TRAN() to resting, Q_HANDLED() for the entries, Q_SUPER() otherwise.
 */
static QState Wifi_ble(Wifi * const me, QEvt const * const e)
{
    QState status;

    switch ( e->sig )
    {
        case FCK_BLE_SCAN_ENTRY_SIG: {
            /* One device, with what it advertised. Asked only whether it carried a manufacturer's
             * data - an app that tells devices apart would read whose, and the bytes - and
             * nothing about it is kept or logged. */
            fck_sys_ble_entry_t const * const d = Q_EVT_CAST(fck_sys_ble_entry_t);
            if ( fck_ble_manufacturer_data(&d->device, NULL, NULL, 0U, NULL) )
            {
                me->ble_with_manufacturer++;
            }
            status = Q_HANDLED();
            break;
        }
        case FCK_BLE_SCAN_RSP_SIG: {
            fck_sys_answer_t const * const a = Q_EVT_CAST(fck_sys_answer_t);
            if ( KODE_OK == a->rc )
            {
                KLOG_I(TAG, "BLE: %lu device(s) advertised in %u s, %lu with a manufacturer's data",
                       (unsigned long)a->count, (unsigned)WIFI_BLE_SECONDS,
                       (unsigned long)me->ble_with_manufacturer);
            }
            else
            {
                KLOG_W(TAG, "the BLE listen was refused: %s", fck_why_text(a->why));
            }
            (void)QTimeEvt_rearm(&me->pause, KQP_TICKS_PER_MS(WIFI_PAUSE_MS));
            status = Q_TRAN(&Wifi_resting);
            break;
        }
        default: {
            status = Q_SUPER(&Wifi_listening);
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
static QState Wifi_resting(Wifi * const me, QEvt const * const e)
{
    QState status;

    switch ( e->sig )
    {
        case WIFI_PAUSE_SIG: {
            if ( ask_channels(me) )
            {
                status = Q_TRAN(&Wifi_channels);
            }
            else
            {
                (void)QTimeEvt_rearm(&me->pause, KQP_TICKS_PER_MS(WIFI_PAUSE_MS));
                status = Q_HANDLED();
            }
            break;
        }
        default: {
            status = Q_SUPER(&Wifi_listening);
            break;
        }
    }

    return status;
}
