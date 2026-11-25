/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file fake_fck.c
 * @brief See fake_fck.h. The arguments are checked as fck_sys.h documents them: the asker is a
 *        started Active Object (here: not NULL), a chip is FCK_C5 or FCK_NRF, a scan lasts 1 to
 *        60 s, and a buffer lent is never NULL.
 */
#include "fake_fck.h"

#include <string.h>

fck_mock_t g_fck;

void fck_mock_reset(void)
{
    (void)memset(&g_fck, 0, sizeof(g_fck));
    g_fck.begin_answer     = KODE_OK;
    g_fck.subscribe_answer = KODE_OK;
    g_fck.answer           = KODE_OK;
}

/** @brief One request written down; KODE_DENIED when @p good is false. */
static kode_status_t called(fck_call_t call, QActive *me, const void *lent, bool good)
{
    g_fck.calls++;
    g_fck.last.call = call;
    g_fck.last.me   = me;
    g_fck.last.lent = lent;
    if (!good || (me == NULL)) {
        g_fck.bad_args++;
        return KODE_DENIED;
    }
    return g_fck.answer;
}

static bool chip_ok(fck_chip_t chip)
{
    return (chip == FCK_C5) || (chip == FCK_NRF);
}

const char *fck_why_text(fck_why_t why)
{
    return (why == FCK_WHY_NO_MODULE) ? "no module on J3" : ((why == FCK_WHY_NONE) ? "" : "another reason");
}

kode_status_t fck_sys_begin(void)
{
    g_fck.begins++;
    return g_fck.begin_answer;
}

kode_status_t fck_sys_subscribe(QActive *me)
{
    g_fck.subscribes++;
    g_fck.subscriber = me;
    return g_fck.subscribe_answer;
}

bool fck_sys_is_present(void)
{
    return g_fck.st.present;
}

kode_status_t fck_sys_status_read(fck_status_t *out)
{
    if (out == NULL) {
        return KODE_DENIED;
    }
    *out = g_fck.st;
    return KODE_OK;
}

kode_status_t fck_sys_firmware(QActive *me, fck_chip_t chip, fck_firmware_t *out)
{
    g_fck.last.chip = chip;
    return called(FCK_CALL_FIRMWARE, me, out, chip_ok(chip) && (out != NULL));
}

kode_status_t fck_sys_power(QActive *me, fck_chip_t chip, bool on)
{
    g_fck.last.chip = chip;
    g_fck.last.on   = on;
    return called(FCK_CALL_POWER, me, NULL, chip_ok(chip));
}

kode_status_t fck_sys_reset(QActive *me, fck_chip_t chip)
{
    g_fck.last.chip = chip;
    return called(FCK_CALL_RESET, me, NULL, chip_ok(chip));
}

kode_status_t fck_sys_gnss_begin(QActive *me)
{
    return called(FCK_CALL_GNSS_START, me, NULL, true);
}

kode_status_t fck_sys_gnss_end(QActive *me)
{
    return called(FCK_CALL_GNSS_STOP, me, NULL, true);
}

kode_status_t fck_sys_ble_scan(QActive *me, uint8_t duration_s)
{
    g_fck.last.secs = duration_s;
    return called(FCK_CALL_BLE_SCAN, me, NULL, (duration_s >= 1U) && (duration_s <= 60U));
}

kode_status_t fck_sys_rescue_probe(QActive *me, fck_chip_t chip, fck_rescue_report_t *out)
{
    g_fck.last.chip = chip;
    return called(FCK_CALL_RESCUE_PROBE, me, out, chip_ok(chip) && (out != NULL));
}
