/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file fake_kode.h
 * @brief What tests/host_ao fakes under the owner's Active Object: klog's
 *        kode_log(), printed and counted, and khal's task identity.
 *
 * Nothing else is: kqp_self.c is kode-sdk's own, and fck_ao.c's worker is a FreeRTOS task of its own, and the
 * host port runs it as the board does.
 */
#ifndef FAKE_KODE_H_
#define FAKE_KODE_H_

#ifdef __cplusplus
extern "C" {
#endif

/** @brief How many times kode_log() was called at warning level or above. */
extern unsigned g_log_warnings;

#ifdef __cplusplus
}
#endif

#endif /* FAKE_KODE_H_ */
