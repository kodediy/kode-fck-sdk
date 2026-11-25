/* SPDX-License-Identifier: GPL-3.0-or-later */
/** @file esp_timer.h @brief esp_timer_get_time(), for the host: real time plus what the model has skipped. */
#ifndef FAKE_ESP_TIMER_H_
#define FAKE_ESP_TIMER_H_

#include <stdint.h>

int64_t esp_timer_get_time(void);

#endif /* FAKE_ESP_TIMER_H_ */
