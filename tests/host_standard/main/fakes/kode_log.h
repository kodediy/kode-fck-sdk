/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kode_log.h
 * @brief kode-sdk's log, counted: the suite asks how many warnings the layer said (g_log_warnings).
 */
#ifndef KODE_LOG_H_
#define KODE_LOG_H_

void fake_log_write(char level, const char *tag, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

#define KLOG_I(tag, ...) fake_log_write('I', (tag), __VA_ARGS__)
#define KLOG_W(tag, ...) fake_log_write('W', (tag), __VA_ARGS__)
#define KLOG_E(tag, ...) fake_log_write('E', (tag), __VA_ARGS__)

#endif /* KODE_LOG_H_ */
