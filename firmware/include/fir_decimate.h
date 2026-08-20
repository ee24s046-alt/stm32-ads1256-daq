/**
 * @file    fir_decimate.h
 * @brief   Polyphase decimating FIR, fixed point.
 *
 * Why polyphase, concretely
 * -------------------------
 * The naive implementation filters every input sample and then throws M-1 out
 * of every M results away.  That costs N multiply-accumulates per *input*
 * sample: 68 x 30000 = 2.04 MMAC/s here.
 *
 * The polyphase decomposition computes only the samples that survive.  Writing
 * the tap index as i = pM + k splits the prototype filter into M branches of
 * N/M taps, each running on a subsequence of the input at the *output* rate.
 * The cost becomes N MACs per *output* sample: 68 x 7500 = 510 kMAC/s.
 *
 * Exactly 4x fewer operations, for bit-identical output.  `test_fir_decimate.c`
 * asserts that identity directly against a naive reference rather than taking
 * it on trust, because "it is mathematically the same" is precisely the kind of
 * claim that is quietly false after an off-by-one in the branch indexing.
 *
 * The second benefit does not show up in the MAC count: each branch owns a
 * short contiguous delay line of N/M entries, so nothing is shifted through a
 * 68-deep history on every sample.
 *
 * Arithmetic
 * ----------
 * Taps are Q15 and sum to exactly 32768, so the DC gain is exactly unity - not
 * approximately.  A decimator with 0.3 % DC gain error produces a small offset
 * that looks indistinguishable from a failed offset calibration.
 *
 * The accumulator is int64_t and that is not optional.  Input codes reach 2^23
 * and the tap sum is 2^15, so the worst-case accumulation needs 38 bits before
 * the shift.  In int32 this overflows silently, and only on large signals -
 * i.e. it passes every bench test done on a shorted input.  On Cortex-M4 the
 * int64 MAC is a single SMLAL instruction, so it is also free.
 */
#ifndef FIR_DECIMATE_H
#define FIR_DECIMATE_H

#include <stdint.h>
#include <stdbool.h>
#include "fir_coeffs.h"

typedef struct {
    /* One delay line per polyphase branch, each FIR_TAPS_PER_PHASE deep. */
    int32_t  hist[FIR_DECIM_M][FIR_TAPS_PER_PHASE];
    uint32_t idx;        /**< circular write position, advances once per output */
    uint32_t phase;      /**< which branch the next input belongs to            */
    uint32_t primed;     /**< inputs seen, capped - see fir_decim_ready()        */
    uint32_t mac_count;  /**< instrumentation: MACs performed since reset        */
} fir_decim_t;

/** @brief Zero the delay lines and counters. */
void fir_decim_reset(fir_decim_t *f);

/**
 * @brief Feed one input sample.
 *
 * @param out receives a decimated sample when one is produced
 * @return true on every Mth call, when @p out has been written
 */
bool fir_decim_push(fir_decim_t *f, int32_t sample, int32_t *out);

/**
 * @brief false until the delay lines have filled.
 *
 * The first FIR_NUM_TAPS-1 inputs produce outputs contaminated by the zeroed
 * initial state.  Callers that care about a clean start - the noise-floor
 * measurement does - should discard outputs until this returns true.
 */
bool fir_decim_ready(const fir_decim_t *f);

/**
 * @brief Naive filter-then-discard reference, for tests only.
 *
 * Deliberately written the obvious, wasteful way. Its only job is to be
 * transparently correct so the polyphase implementation can be checked against
 * it. Not compiled into the firmware image's hot path.
 */
void fir_decim_reference(const int32_t *in, uint32_t n_in,
                         int32_t *out, uint32_t *n_out);

#endif /* FIR_DECIMATE_H */
