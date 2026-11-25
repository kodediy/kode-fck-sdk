/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file subghz_arm.c
 * @brief The arming of subghz_main.c's transmitting rows: see subghz_arm.h.
 */
#include "subghz_arm.h"

#include <stddef.h>

void subghz_arm_set(subghz_arm_t *a, bool on, uint32_t now_ms)
{
    if (a == NULL) {
        return;
    }
    if (!on) {
        a->on = false;
        a->cuts++;
        return;
    }
    a->until_ms = now_ms + SUBGHZ_ARM_MS;
    a->on       = true;
}

void subghz_arm_cut(subghz_arm_t *a)
{
    if (a == NULL) {
        return;
    }
    a->on = false;
    a->cuts++;
}

bool subghz_arm_ok(const subghz_arm_t *a, uint32_t now_ms)
{
    return (a != NULL) && a->on && ((int32_t)(now_ms - a->until_ms) < 0);
}

uint32_t subghz_arm_cuts(const subghz_arm_t *a)
{
    return (a != NULL) ? a->cuts : 0U;
}

bool subghz_arm_holds(const subghz_arm_t *a, uint32_t cuts, uint32_t now_ms)
{
    return subghz_arm_ok(a, now_ms) && (a->cuts == cuts);
}
