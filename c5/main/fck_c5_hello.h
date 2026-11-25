/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file fck_c5_hello.h
 * @brief The line this firmware greets the Kode with, at boot and for `info`.
 *
 * The official firmware's #C5INFO, field for field, so the Kode's SDK reads it
 * the same way (fck_c5_parse_info()): fw= says which firmware it is, ver= its
 * version as digits and dots, reg= and lim= what its radio keeps. The SDK's
 * host tests print a greeting with this format and read it back, so this
 * header stays plain C.
 *
 * Arguments, in order: name, version, chip revision (major, minor), the six
 * bytes of the base MAC, region ("EU"), limits ("ok", or the first one the
 * Wi-Fi driver refused).
 */
#ifndef FCK_C5_HELLO_H_
#define FCK_C5_HELLO_H_

#define FCK_C5_HELLO_FMT                                                     \
    "#C5INFO fw=%s ver=%s chip=esp32c5 rev=%d.%d "                          \
    "mac=%02x:%02x:%02x:%02x:%02x:%02x reg=%s lim=%s caps="

#endif /* FCK_C5_HELLO_H_ */
