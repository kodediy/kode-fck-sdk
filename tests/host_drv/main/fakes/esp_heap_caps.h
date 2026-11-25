/* SPDX-License-Identifier: GPL-3.0-or-later */
/** @file esp_heap_caps.h @brief heap_caps_malloc(), for the host: it is malloc(). */
#ifndef FAKE_ESP_HEAP_CAPS_H_
#define FAKE_ESP_HEAP_CAPS_H_

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#define MALLOC_CAP_INTERNAL 1U
#define MALLOC_CAP_8BIT     2U

static inline void *heap_caps_malloc(size_t size, uint32_t caps)
{
    (void)caps;
    return malloc(size);
}

static inline void heap_caps_free(void *ptr)
{
    free(ptr);
}

#endif /* FAKE_ESP_HEAP_CAPS_H_ */
