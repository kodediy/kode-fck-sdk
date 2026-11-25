/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kode.h
 * @brief kode-sdk as examples/console_actions's rows see it: the console's
 *        table types and its two calls, from kode_functions.h and
 *        kode_variables.h at the pinned release, and nothing else.
 *
 * The types are kode-sdk's, field for field: the example's target build,
 * which compiles its rows against the real headers, is what proves they
 * still match. fake_console.c keeps the rules kodeOS's registry keeps.
 */
#ifndef KODE_H_
#define KODE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "kode_defs.h"

#define KODE_ROW_NAME_MAX           15U
#define KODE_ROW_DESC_MAX           48U
#define KODE_VALUE_STR_MAX          32U
#define KODE_VARIABLES_MAX          32U
#define KODE_VARIABLE_TABLES        4U
#define KODE_FUNCTIONS_MAX          64U
#define KODE_FUNCTION_TABLES        4U
#define KODE_VALUE_ENUMS_MAX        6U
#define KODE_FUNCTION_ANSWER_MS_MAX 600000U

typedef enum {
    KODE_TYPE_NONE = 0,
    KODE_TYPE_INT,
    KODE_TYPE_BOOL,
    KODE_TYPE_ENUM,
    KODE_TYPE_STRING
} kode_type_t;

typedef struct {
    int32_t     i;
    bool        b;
    uint8_t     e;
    const char *s;
} kode_value_t;

typedef kode_status_t (*kode_function_run_fn)(const kode_value_t *arg);

typedef struct {
    const char           *name;
    const char           *desc;
    kode_type_t           arg;
    int32_t               min;
    int32_t               max;
    const char *const    *enums;
    uint8_t               n_enums;
    kode_function_run_fn  run;
    uint32_t              answer_ms;
} kode_function_t;

typedef kode_status_t (*kode_variable_get_fn)(char *buf, size_t cap);

typedef struct {
    const char           *name;
    const char           *desc;
    kode_variable_get_fn  get;
} kode_variable_t;

kode_status_t kode_functions_add(const kode_function_t *table, size_t n);
kode_status_t kode_variables_add(const kode_variable_t *table, size_t n);

#define KODE_FUNCTIONS_ADD(t) kode_functions_add((t), sizeof(t) / sizeof((t)[0]))
#define KODE_VARIABLES_ADD(t) kode_variables_add((t), sizeof(t) / sizeof((t)[0]))

#endif /* KODE_H_ */
