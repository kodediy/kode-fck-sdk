/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file fake_compat.h
 * @brief Included before every file of the suite (-include): the host port of
 *        FreeRTOS has one core, and the owner's task asks for core 1.
 */
#ifndef SIM_COMPAT_H_
#define SIM_COMPAT_H_

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define xTaskCreatePinnedToCore(fn, name, stack, arg, prio, handle, core) \
    (xTaskCreatePinnedToCore)((fn), (name), (stack), (arg), (prio), (handle), 0)

#endif /* SIM_COMPAT_H_ */
