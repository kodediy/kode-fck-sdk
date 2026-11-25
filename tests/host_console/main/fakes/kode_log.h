/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kode_log.h
 * @brief klog's header as examples/console_actions sees it: its levels and
 *        its three macros over kode_log(), which fake_kode.c keeps.
 *
 * The format attribute is the real header's, so the example's format strings
 * are checked against their arguments here too.
 */
#ifndef KODE_LOG_H_
#define KODE_LOG_H_

typedef enum {
    KODE_LOG_DEBUG = 0,
    KODE_LOG_INFO,
    KODE_LOG_NOTICE,
    KODE_LOG_WARN,
    KODE_LOG_ERROR,
    KODE_LOG_OFF
} kode_log_lvl_t;

void kode_log(kode_log_lvl_t lvl, const char *tag, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

#define KLOG_I(tag, ...) kode_log(KODE_LOG_INFO, (tag), __VA_ARGS__)
#define KLOG_W(tag, ...) kode_log(KODE_LOG_WARN, (tag), __VA_ARGS__)
#define KLOG_E(tag, ...) kode_log(KODE_LOG_ERROR, (tag), __VA_ARGS__)

#endif /* KODE_LOG_H_ */
