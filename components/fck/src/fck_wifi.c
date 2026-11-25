/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file fck_wifi.c
 * @brief The C5's Wi-Fi and Bluetooth LE for an app: fck_core_wifi_scan(),
 *        fck_core_wifi_channels(), fck_core_wifi_frames() and fck_core_ble_scan()
 *        in fck.h.
 *
 * Each call is one of the firmware's commands (fck_drv_c5.h), judged by what the C5
 * said it runs and sent only while it still runs that (fck_aos_command_if_same_firmware()).
 * The firmware's lines are read in the owner's task as they arrive and kept in a
 * list of the call's own, so that the caller's array is written only when the
 * command ended well. Nothing is logged: a network's name or address, or a
 * device's, says who is near a person.
 */
#include "fck.h"

#include <stdlib.h>
#include <string.h>

#include "fck_drv_c5.h"
#include "fck_aos.h"
#include "fck_private.h"

/* The facade's types are the firmware's, in an app's names: the same order and
 * the same sizes, so that what the parsers fill is copied and not mapped. */
_Static_assert((int)FCK_BAND_ALL == (int)FCK_DRV_C5_BAND_ALL, "fck_band_t follows fck_drv_c5_band_t");
_Static_assert((int)FCK_BAND_2G == (int)FCK_DRV_C5_BAND_2G, "fck_band_t follows fck_drv_c5_band_t");
_Static_assert((int)FCK_BAND_5G == (int)FCK_DRV_C5_BAND_5G, "fck_band_t follows fck_drv_c5_band_t");
_Static_assert((int)FCK_WIFI_SECURITY_OPEN == (int)FCK_DRV_C5_SECURITY_OPEN, "auth follows the firmware");
_Static_assert((int)FCK_WIFI_SECURITY_WEP == (int)FCK_DRV_C5_SECURITY_WEP, "auth follows the firmware");
_Static_assert((int)FCK_WIFI_SECURITY_WPA_PSK == (int)FCK_DRV_C5_SECURITY_WPA_PSK, "auth follows the firmware");
_Static_assert((int)FCK_WIFI_SECURITY_WPA2_PSK == (int)FCK_DRV_C5_SECURITY_WPA2_PSK, "auth follows the firmware");
_Static_assert((int)FCK_WIFI_SECURITY_WPA_WPA2_PSK == (int)FCK_DRV_C5_SECURITY_WPA_WPA2_PSK,
               "auth follows the firmware");
_Static_assert((int)FCK_WIFI_SECURITY_WPA2_ENTERPRISE == (int)FCK_DRV_C5_SECURITY_WPA2_ENTERPRISE, "auth follows the firmware");
_Static_assert((int)FCK_WIFI_SECURITY_WPA3_PSK == (int)FCK_DRV_C5_SECURITY_WPA3_PSK, "auth follows the firmware");
_Static_assert((int)FCK_WIFI_SECURITY_WPA2_WPA3_PSK == (int)FCK_DRV_C5_SECURITY_WPA2_WPA3_PSK,
               "auth follows the firmware");
_Static_assert((int)FCK_WIFI_SECURITY_WAPI_PSK == (int)FCK_DRV_C5_SECURITY_WAPI_PSK, "auth follows the firmware");
_Static_assert((int)FCK_WIFI_SECURITY_OWE == (int)FCK_DRV_C5_SECURITY_OWE, "auth follows the firmware");
_Static_assert((int)FCK_WIFI_SECURITY_WPA3_ENTERPRISE_192 == (int)FCK_DRV_C5_SECURITY_WPA3_ENTERPRISE_192,
               "auth follows the firmware");
_Static_assert((int)FCK_WIFI_SECURITY_DPP == (int)FCK_DRV_C5_SECURITY_DPP, "auth follows the firmware");
_Static_assert((int)FCK_WIFI_SECURITY_WPA3_ENTERPRISE == (int)FCK_DRV_C5_SECURITY_WPA3_ENTERPRISE, "auth follows the firmware");
_Static_assert((int)FCK_WIFI_SECURITY_WPA2_WPA3_ENTERPRISE == (int)FCK_DRV_C5_SECURITY_WPA2_WPA3_ENTERPRISE,
               "auth follows the firmware");
_Static_assert((int)FCK_WIFI_SECURITY_WPA_ENTERPRISE == (int)FCK_DRV_C5_SECURITY_WPA_ENTERPRISE, "auth follows the firmware");
_Static_assert((int)FCK_WIFI_SECURITY_UNKNOWN == (int)FCK_DRV_C5_SECURITY_UNKNOWN, "auth follows the firmware");
_Static_assert(FCK_WIFI_NETWORK_MAX == FCK_DRV_C5_NETWORK_MAX, "what the firmware lists");
_Static_assert(FCK_BLE_DEVICE_MAX == FCK_DRV_C5_BLE_MAX, "what the firmware lists");
_Static_assert(FCK_BLE_DATA_MAX == FCK_DRV_C5_BLE_DATA_MAX, "what one advertisement carries");
_Static_assert((int)FCK_BLE_ADVERTISING_CONNECTABLE == (int)FCK_DRV_C5_BLE_EVENT_CONNECTABLE,
               "how it advertised follows the firmware");
_Static_assert((int)FCK_BLE_ADVERTISING_DIRECTED == (int)FCK_DRV_C5_BLE_EVENT_DIRECTED,
               "how it advertised follows the firmware");
_Static_assert((int)FCK_BLE_ADVERTISING_SCANNABLE == (int)FCK_DRV_C5_BLE_EVENT_SCANNABLE,
               "how it advertised follows the firmware");
_Static_assert((int)FCK_BLE_ADVERTISING_NOT_CONNECTABLE == (int)FCK_DRV_C5_BLE_EVENT_NOT_CONNECTABLE,
               "how it advertised follows the firmware");
_Static_assert((int)FCK_BLE_ADVERTISING_UNKNOWN == (int)FCK_DRV_C5_BLE_EVENT_UNKNOWN,
               "how it advertised follows the firmware");
_Static_assert(FCK_WIFI_CHANNEL_2G_MAX == FCK_DRV_C5_CHANNEL_2G_MAX, "what the firmware lists");
_Static_assert(FCK_WIFI_CHANNEL_5G_MAX == FCK_DRV_C5_CHANNEL_5G_MAX, "what the firmware lists");
_Static_assert(FCK_WIFI_FRAME_HEAD_MAX == FCK_DRV_C5_FRAME_HEAD_MAX, "what a frame's head has room for");
_Static_assert(FCK_WIFI_FRAME_HEAD_MAX >= FCK_DRV_C5_FRAME_HEAD_SENT_MAX, "the most the firmware sends");
_Static_assert(FCK_WIFI_RSSI_NONE == FCK_DRV_C5_RSSI_NONE, "what the firmware says for nothing");

/* One command, as the owner runs it; its rc as the caller's status. @p failed,
 * when given, says the #DONE came with rc 1: the firmware ran the command and
 * says it failed, which is not the UART failing under it. */
static kode_status_t run(uint32_t seq, const fck_drv_c5_command_t *c, fck_drv_link_message_fn fn, void *ctx,
                         bool *failed)
{
    int32_t             rc = -1;
    kode_status_t const st = fck_aos_command_if_same_firmware(FCK_AOS_C5, seq, c->line, c->verb, fn, ctx,
                                                c->ms, &rc);
    if (failed != NULL) {
        *failed = (st == KODE_OK) && (rc == 1);
    }
    return (st == KODE_OK) ? fck_drv_c5_status(rc) : st;
}

/* ---- Networks ------------------------------------------------------------------------ */

typedef struct {
    fck_wifi_network_t *list;
    size_t         keep;   /* what list holds */
    size_t         n;
} aps_t;

static void ap_copy(fck_wifi_network_t *o, const fck_drv_c5_network_t *a)
{
    _Static_assert(sizeof(o->ssid) == sizeof(a->ssid), "the SSID's room");
    memcpy(o->ssid, a->ssid, sizeof(o->ssid));
    o->ssid_len = a->ssid_len;
    memcpy(o->bssid, a->bssid, sizeof(o->bssid));
    o->channel = a->ch;
    o->band    = (fck_band_t)a->band;
    o->rssi    = a->rssi;
    o->auth    = (fck_wifi_security_t)a->auth;
}

/* In the owner's task, once per #AP: kept strongest first, and when the list
 * is full the weakest goes. Equal strengths stay in the firmware's order. */
static void on_ap(void *ctx, const fck_drv_link_message_t *m)
{
    aps_t *const x = ctx;
    fck_drv_c5_network_t  a;
    if (!fck_drv_c5_parse_network(m, &a)) {
        return;
    }
    size_t at = x->n;
    while (at > 0U && x->list[at - 1U].rssi < a.rssi) {
        at--;
    }
    if (at >= x->keep) {
        return;
    }
    size_t const last = (x->n < x->keep) ? x->n : x->keep - 1U;
    memmove(&x->list[at + 1U], &x->list[at], (last - at) * sizeof(x->list[0]));
    ap_copy(&x->list[at], &a);
    if (x->n < x->keep) {
        x->n++;
    }
}

kode_status_t fck_core_wifi_scan(fck_band_t band, fck_wifi_network_t *out, size_t cap, size_t *n)
{
    fck_drv_c5_command_t c;
    if (out == NULL || cap == 0U || (unsigned)band > (unsigned)FCK_BAND_5G
        || fck_drv_c5_command_scan(&c, (fck_drv_c5_band_t)band) != KODE_OK) {
        return fck_fail(KODE_DENIED, "out is NULL, cap is 0, or band is not an fck_band_t");
    }
    fck_drv_c5_info_t info;
    uint32_t      seq = 0U;
    kode_status_t st  = fck_firmware_c5(&info, &seq);
    if (st != KODE_OK) {
        return st;
    }
    if (!fck_drv_c5_has(&info, FCK_DRV_C5_SCAN)) {
        return fck_fail(KODE_DENIED, "the C5's firmware lacks the command: no scan in what it says it does");
    }
    /* `scan` is the one command that makes the C5 transmit: probe requests.
     * The limits are the firmware's own, and it refuses the command itself
     * (rc 1) while one is not in force.
     * TEMPORARY: pre-production firmware (2.x). Remove when the production modules ship.
     * One of the first modules' that keeps none - before 2.0 it sends them
     * on both bands with no limit - is not asked. */
    if (c.emits && fck_drv_c5_is_unlimited(&info)) {
        return fck_fail(KODE_DENIED, "the C5's firmware is one of the first modules' and keeps no limits itself: its scan is not asked for");
    }
    aps_t x = { .keep = (cap < FCK_WIFI_NETWORK_MAX) ? cap : FCK_WIFI_NETWORK_MAX };
    x.list  = malloc(x.keep * sizeof(x.list[0]));
    if (x.list == NULL) {
        return fck_fail(KODE_NO_RESOURCES, "no memory for the list");
    }
    bool failed = false;
    st = run(seq, &c, on_ap, &x, &failed);
    if (failed) {
        /* rc 1: its own refusal with lim= not "ok", or a scan its driver
         * did not end in time and it stopped. Nothing is handed over. */
        st = fck_fail(KODE_IO_ERROR, "the C5's firmware said the scan failed: a limit its driver refused, or a scan that did not end in time");
    }
    if (st == KODE_OK) {
        memcpy(out, x.list, x.n * sizeof(out[0]));
        if (n != NULL) {
            *n = x.n;
        }
    }
    free(x.list);
    return st;
}

/* ---- Channels ------------------------------------------------------------------------ */

typedef struct {
    fck_wifi_channel_t *list;
    size_t           keep;
    size_t           n;
    bool             more;   /* the firmware listed more than list holds */
} chans_t;

static void on_chan(void *ctx, const fck_drv_link_message_t *m)
{
    chans_t *const x = ctx;
    fck_drv_c5_channel_t  c;
    if (!fck_drv_c5_parse_channel(m, &c)) {
        return;
    }
    if (x->n >= x->keep) {
        x->more = true;
        return;
    }
    x->list[x->n++] = (fck_wifi_channel_t){
        .channel  = c.ch,
        .band     = (fck_band_t)c.band,
        .frames   = c.frames,
        .mgmt     = c.mgmt,
        .data     = c.data,
        .ctrl     = c.ctrl,
        .rssi_max = c.rssi_max,
        .rssi_avg = c.rssi_avg,
        .dwell_ms = c.dwell_ms,
    };
}

/* The most channels `chan` can list for a band. */
static size_t chan_room(fck_band_t band)
{
    switch (band) {
    case FCK_BAND_2G:
        return FCK_WIFI_CHANNEL_2G_MAX;
    case FCK_BAND_5G:
        return FCK_WIFI_CHANNEL_5G_MAX;
    default:
        return FCK_WIFI_CHANNEL_MAX;
    }
}

kode_status_t fck_core_wifi_channels(fck_band_t band, uint16_t dwell_ms, fck_wifi_channel_t *out,
                                size_t cap, size_t *n)
{
    fck_drv_c5_command_t c;
    if (out == NULL || (unsigned)band > (unsigned)FCK_BAND_5G || cap < chan_room(band)
        || cap > FCK_WIFI_CHANNELS_CAP_MAX
        || fck_drv_c5_command_channel(&c, (fck_drv_c5_band_t)band, dwell_ms) != KODE_OK) {
        return fck_fail(KODE_DENIED, "out is NULL, band is not an fck_band_t, cap is below the band's channels or above FCK_WIFI_CHANNELS_CAP_MAX, or dwell_ms is out of range");
    }
    fck_drv_c5_info_t info;
    uint32_t      seq = 0U;
    kode_status_t st  = fck_firmware_c5(&info, &seq);
    if (st != KODE_OK) {
        return st;
    }
    if (!fck_drv_c5_has(&info, FCK_DRV_C5_CHANNEL)) {
        return fck_fail(KODE_DENIED, "the C5's firmware lacks the command: no channel survey in what it says it does");
    }
    chans_t x = { .keep = cap };
    x.list    = calloc(x.keep, sizeof(x.list[0]));     /* cap is bounded above, and calloc checks the product anyway */
    if (x.list == NULL) {
        return fck_fail(KODE_NO_RESOURCES, "no memory for the list");
    }
    st = run(seq, &c, on_chan, &x, NULL);
    if (st == KODE_OK && x.more) {
        st = fck_fail(KODE_NO_RESOURCES, "the band has more channels than cap holds");
    }
    if (st == KODE_OK) {
        memcpy(out, x.list, x.n * sizeof(out[0]));
        if (n != NULL) {
            *n = x.n;
        }
    }
    free(x.list);
    return st;
}

/* ---- Frames -------------------------------------------------------------------------- */

typedef struct {
    fck_wifi_frame_t *list;
    size_t            keep;   /* what list holds */
    size_t            n;
    bool              done;   /* the firmware's #FRAMES came */
    fck_drv_c5_frames_summary_t sum;
} frames_t;

/* In the owner's task, once per #F, and once for #FRAMES. The frames are kept in the order they
 * came, and the first keep of them: a later one is only counted, in the summary's sent. */
static void on_frame(void *ctx, const fck_drv_link_message_t *m)
{
    frames_t *const x = ctx;
    fck_drv_c5_frame_t f;
    if (fck_drv_c5_parse_frame(m, &f)) {
        if (x->n < x->keep) {
            fck_wifi_frame_t *const o = &x->list[x->n++];
            (void)memset(o, 0, sizeof(*o));
            (void)memcpy(o->fc, f.fc, sizeof(o->fc));
            o->channel = f.channel;
            o->rssi    = f.rssi;
            o->len     = f.len;
            (void)memcpy(o->addr1, f.addr1, sizeof(o->addr1));
            (void)memcpy(o->addr2, f.addr2, sizeof(o->addr2));
            (void)memcpy(o->addr3, f.addr3, sizeof(o->addr3));
            o->head_len = f.head_len;
            (void)memcpy(o->head, f.head, f.head_len);
        }
    } else if (fck_drv_c5_parse_frames_summary(m, &x->sum)) {
        x->done = true;
    }
}

kode_status_t fck_core_wifi_frames(uint8_t channel, uint16_t ms, fck_wifi_frame_t *out, size_t cap,
                                   size_t *n, fck_wifi_frames_statistics_t *stats)
{
    fck_drv_c5_command_t c;
    if (out == NULL || cap == 0U || fck_drv_c5_command_frames(&c, channel, ms) != KODE_OK) {
        return fck_fail(KODE_DENIED, "out is NULL, cap is 0, channel is not 1 to 177, or ms is not 20 to 5000");
    }
    fck_drv_c5_info_t info;
    uint32_t          seq = 0U;
    kode_status_t     st  = fck_firmware_c5(&info, &seq);
    if (st != KODE_OK) {
        return st;
    }
    /* It only listens, so one of the first modules' firmwares, which keeps no limits itself, is
     * asked too: it just has no `frames`. */
    if (!fck_drv_c5_has(&info, FCK_DRV_C5_FRAMES)) {
        return fck_fail(KODE_DENIED, "the C5's firmware lacks the command: no frame listener in what it says it does");
    }
    frames_t x = { .keep = (cap < FCK_WIFI_FRAME_MAX) ? cap : FCK_WIFI_FRAME_MAX };
    x.list     = malloc(x.keep * sizeof(x.list[0]));
    if (x.list == NULL) {
        return fck_fail(KODE_NO_RESOURCES, "no memory for the list");
    }
    /* As run(), but what the firmware's rc means is said in words: an app tells a firmware that does not
     * know `frames` (or refused its arguments) from a listen that failed. */
    int32_t rc = -1;
    st         = fck_aos_command_if_same_firmware(FCK_AOS_C5, seq, c.line, c.verb, on_frame, &x, c.ms, &rc);
    if (st == KODE_OK) {
        st = fck_drv_c5_status(rc);
        if (rc == 2) {
            st = fck_fail(st, "the C5's firmware answered rc 2 to `frames`: it does not know the command, or took the channel or the time for bad");
        } else if (rc == 1) {
            st = fck_fail(st, "the C5's firmware answered rc 1 to `frames`: it could not listen on that channel");
        } else if (st != KODE_OK) {
            st = fck_fail(st, "the C5's firmware ended `frames` with an rc this SDK does not know");
        }
    }
    if (st == KODE_OK && !x.done) {
        st = fck_fail(KODE_IO_ERROR, "the C5's firmware ended `frames` without its summary");
    }
    if (st == KODE_OK) {
        (void)memcpy(out, x.list, x.n * sizeof(out[0]));
        if (n != NULL) {
            *n = x.n;
        }
        if (stats != NULL) {
            *stats = (fck_wifi_frames_statistics_t){
                .channel        = x.sum.ch,
                .ms             = x.sum.ms,
                .heard          = x.sum.heard,
                .mgmt           = x.sum.mgmt,
                .data           = x.sum.data,
                .ctrl           = x.sum.ctrl,
                .beacon         = x.sum.beacon,
                .probe_request  = x.sum.probe_request,
                .probe_response = x.sum.probe_response,
                .deauth         = x.sum.deauth,
                .disassoc       = x.sum.disassoc,
                .sent           = x.sum.sent,
                .dropped        = x.sum.dropped,
            };
        }
    }
    free(x.list);
    return st;
}

/* ---- Bluetooth LE -------------------------------------------------------------------- */

typedef struct {
    fck_ble_device_t *list;
    size_t         keep;
    size_t         n;
} devs_t;

/* As on_ap(): the firmware sorts too, but the order the call promises is not
 * left to what runs on the C5. */
static void on_ble(void *ctx, const fck_drv_link_message_t *m)
{
    devs_t *const x = ctx;
    fck_drv_c5_ble_t  b;
    if (!fck_drv_c5_parse_ble(m, &b)) {
        return;
    }
    size_t at = x->n;
    while (at > 0U && x->list[at - 1U].rssi < b.rssi) {
        at--;
    }
    if (at >= x->keep) {
        return;
    }
    size_t const last = (x->n < x->keep) ? x->n : x->keep - 1U;
    memmove(&x->list[at + 1U], &x->list[at], (last - at) * sizeof(x->list[0]));
    fck_ble_device_t *const o = &x->list[at];
    _Static_assert(sizeof(o->name) == sizeof(b.name), "the name's room");
    memcpy(o->addr, b.addr, sizeof(o->addr));
    o->random = b.random;
    o->rssi   = b.rssi;
    memcpy(o->name, b.name, sizeof(o->name));
    o->name_len = b.name_len;
    o->adv      = b.adv;
    _Static_assert(sizeof(o->data) == sizeof(b.data), "the advertisement's room");
    o->advertising = (fck_ble_advertising_t)b.event;
    o->first_ms    = b.first_ms;
    o->last_ms     = b.last_ms;
    (void)memcpy(o->data, b.data, sizeof(o->data));
    o->data_len = b.data_len;
    if (x->n < x->keep) {
        x->n++;
    }
}

kode_status_t fck_core_ble_scan(uint8_t duration_s, fck_ble_device_t *out, size_t cap, size_t *n)
{
    fck_drv_c5_command_t c;
    if (out == NULL || cap == 0U || fck_drv_c5_command_ble_scan(&c, duration_s) != KODE_OK) {
        return fck_fail(KODE_DENIED, "out is NULL, cap is 0, or duration_s is out of range");
    }
    fck_drv_c5_info_t info;
    uint32_t      seq = 0U;
    kode_status_t st  = fck_firmware_c5(&info, &seq);
    if (st != KODE_OK) {
        return st;
    }
    if (!fck_drv_c5_has(&info, FCK_DRV_C5_BLE_SCAN)) {
        return fck_fail(KODE_DENIED, "the C5's firmware lacks the command: no BLE scan in what it says it does");
    }
    devs_t x = { .keep = (cap < FCK_BLE_DEVICE_MAX) ? cap : FCK_BLE_DEVICE_MAX };
    x.list   = malloc(x.keep * sizeof(x.list[0]));
    if (x.list == NULL) {
        return fck_fail(KODE_NO_RESOURCES, "no memory for the list");
    }
    bool failed = false;
    st = run(seq, &c, on_ble, &x, &failed);
    /* rc 1: the firmware could not start the scan, or cut it short, and
     * what it heard until then is still listed. The call fails, and hands
     * over what there is. */
    if (st == KODE_OK || failed) {
        memcpy(out, x.list, x.n * sizeof(out[0]));
        if (n != NULL) {
            *n = x.n;
        }
    }
    free(x.list);
    if (failed) {
        st = fck_fail(KODE_IO_ERROR, "the C5's firmware said the BLE scan failed or was cut short: out holds what it heard until then");
    }
    return st;
}

/* What a Bluetooth LE advertisement carries, read from a fck_ble_device_t. Plain C: no radio, no
 * module and no error slot, so both layers and an app that kept a device call the same four. */

#define AD_SERVICES_16_INCOMPLETE 0x02U
#define AD_SERVICES_16_COMPLETE   0x03U
#define AD_SERVICE_DATA_16        0x16U
#define AD_APPEARANCE             0x19U
#define AD_MANUFACTURER_DATA      0xFFU

/** One field of an advertisement: its type and the bytes after it. */
typedef struct {
    uint8_t        type;
    const uint8_t *bytes;
    size_t         len;
} ad_field_t;

/**
 * The field at @p *at, and @p *at moved past it. False where the advertisement ends, and where it
 * stops making sense - a length of 0, or one that runs past the end - so what a device sent wrong
 * is never read beyond its own bytes.
 */
static bool ad_next(const fck_ble_device_t *device, size_t *at, ad_field_t *field)
{
    const size_t end = device->data_len;
    if ((end > FCK_BLE_DATA_MAX) || (*at >= end)) {
        return false;
    }
    const size_t len = device->data[*at];
    if ((len == 0U) || (len > ((end - *at) - 1U))) {
        return false;
    }
    field->type  = device->data[*at + 1U];
    field->bytes = &device->data[*at + 2U];
    field->len   = len - 1U;
    *at += 1U + len;
    return true;
}

static uint16_t le16(const uint8_t *bytes)
{
    return (uint16_t)((uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8));
}

/** @p len bytes, or the @p cap first, into @p out; how many into @p n. */
static void ad_give(const uint8_t *bytes, size_t len, uint8_t *out, size_t cap, size_t *n)
{
    const size_t give = (len < cap) ? len : cap;
    if (give > 0U) {
        (void)memcpy(out, bytes, give);
    }
    if (n != NULL) {
        *n = give;
    }
}

bool fck_ble_manufacturer_data(const fck_ble_device_t *device, uint16_t *company, uint8_t *out, size_t cap,
                               size_t *n)
{
    if ((device == NULL) || ((out == NULL) && (cap > 0U))) {
        return false;
    }
    size_t     at = 0U;
    ad_field_t f;
    while (ad_next(device, &at, &f)) {
        if ((f.type == AD_MANUFACTURER_DATA) && (f.len >= 2U)) {
            if (company != NULL) {
                *company = le16(f.bytes);
            }
            ad_give(&f.bytes[2], f.len - 2U, out, cap, n);
            return true;
        }
    }
    return false;
}

bool fck_ble_service_data(const fck_ble_device_t *device, uint16_t uuid, uint8_t *out, size_t cap, size_t *n)
{
    if ((device == NULL) || ((out == NULL) && (cap > 0U))) {
        return false;
    }
    size_t     at = 0U;
    ad_field_t f;
    while (ad_next(device, &at, &f)) {
        if ((f.type == AD_SERVICE_DATA_16) && (f.len >= 2U) && (le16(f.bytes) == uuid)) {
            ad_give(&f.bytes[2], f.len - 2U, out, cap, n);
            return true;
        }
    }
    return false;
}

bool fck_ble_services(const fck_ble_device_t *device, uint16_t *out, size_t cap, size_t *n)
{
    if ((device == NULL) || ((out == NULL) && (cap > 0U))) {
        return false;
    }
    size_t     at    = 0U;
    size_t     given = 0U;
    bool       any   = false;
    ad_field_t f;
    while (ad_next(device, &at, &f)) {
        if ((f.type != AD_SERVICES_16_INCOMPLETE) && (f.type != AD_SERVICES_16_COMPLETE)) {
            continue;
        }
        /* Whole UUIDs only: an odd byte left at the end of a list is not one. */
        for (size_t i = 0U; (i + 2U) <= f.len; i += 2U) {
            any = true;
            if (given < cap) {
                out[given] = le16(&f.bytes[i]);
                given++;
            }
        }
    }
    if (any && (n != NULL)) {
        *n = given;
    }
    return any;
}

bool fck_ble_appearance(const fck_ble_device_t *device, uint16_t *appearance)
{
    if (device == NULL) {
        return false;
    }
    size_t     at = 0U;
    ad_field_t f;
    while (ad_next(device, &at, &f)) {
        if ((f.type == AD_APPEARANCE) && (f.len == 2U)) {
            if (appearance != NULL) {
                *appearance = le16(f.bytes);
            }
            return true;
        }
    }
    return false;
}
