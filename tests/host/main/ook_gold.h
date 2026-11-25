/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file ook_gold.h
 * @brief Test support: the shapes of the golden vectors of fck_drv_ook.
 *
 * ook_gold.inc is data: what the implementation this one replaced answered when
 * asked, through its public header only, for each capture of ook_scenarios.h
 * and for each call of the encoder below (ook_gold.inc says which answers, and
 * which are not kept). The new code is held to the same answers for the fixed
 * codes and the sending path's detector, but for the deliberate differences the
 * test lists (te under 100 us is no longer read or written). Captures that the
 * previous one read as a protocol this one does not have (no public timing
 * exists for it) carry skip_fixed.
 */
#ifndef OOK_GOLD_H_
#define OOK_GOLD_H_

#include <stddef.h>
#include <stdint.h>

typedef struct {
    int      proto;
    unsigned bits;
    uint32_t key;
    unsigned te_us;
    size_t   frames;
    size_t   cap;
    size_t   runs;   /* what the encoder returns                       */
    uint32_t hash;   /* FNV-1a of the runs, as int32, little endian    */
} gold_encode_t;

typedef struct {
    int      proto;
    unsigned bits;
    uint32_t key;
    unsigned te_us;
    unsigned frames;
} gold_fixed_t;

typedef struct {
    int          skip_fixed;
    size_t       fixed_n;
    gold_fixed_t fixed[3];
    int          shaped;
} gold_scenario_t;

#endif /* OOK_GOLD_H_ */
