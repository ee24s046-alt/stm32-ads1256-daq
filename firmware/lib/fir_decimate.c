#include "fir_decimate.h"
#include <string.h>

/*
 * Rounding: add half an LSB before the arithmetic right shift rather than
 * truncating. A bare >>15 floors, which biases every output sample by -0.5 LSB
 * - a DC offset introduced by the filter itself, on a signal chain whose whole
 * purpose is that DC offsets have been calibrated out.
 *
 * The shift is on int64_t and GCC/Clang define >> on signed types as an
 * arithmetic shift. The golden vector in fir_coeffs.h is generated with Python's
 * >>, which floors identically, so the two agree bit for bit.
 */
static inline int32_t q15_round_shift(int64_t acc)
{
    return (int32_t)((acc + (int64_t)(1 << 14)) >> 15);
}

void fir_decim_reset(fir_decim_t *f)
{
    (void)memset(f, 0, sizeof(*f));
}

bool fir_decim_ready(const fir_decim_t *f)
{
    return f->primed >= FIR_NUM_TAPS;
}

bool fir_decim_push(fir_decim_t *f, int32_t sample, int32_t *out)
{
    /*
     * Branch selection. Writing the convolution index as i = pM + k gives
     *
     *     y[m] = sum_k sum_p  h[pM + k] * x[(m - p)M + (M - 1 - k)]
     *
     * so the input whose position within the group is r = n mod M belongs to
     * branch k = (M - 1) - r. Getting this reversal wrong still produces
     * plausible-looking filtered output - just with the branches permuted,
     * which shows up as a fractional-sample delay and a mangled phase
     * response rather than as anything obviously broken.
     */
    const uint32_t branch = (FIR_DECIM_M - 1U) - f->phase;

    f->hist[branch][f->idx] = sample;

    if (f->primed < FIR_NUM_TAPS) {
        f->primed++;
    }

    f->phase++;
    if (f->phase < FIR_DECIM_M) {
        return false;                     /* group not complete, no output yet */
    }
    f->phase = 0U;

    {
        int64_t acc = 0;

        for (uint32_t k = 0U; k < FIR_DECIM_M; k++) {
            const int32_t *line = f->hist[k];
            uint32_t j = f->idx;

            for (uint32_t p = 0U; p < FIR_TAPS_PER_PHASE; p++) {
                acc += (int64_t)line[j] * (int64_t)k_fir_taps_q15[(p * FIR_DECIM_M) + k];
                j = (j == 0U) ? (FIR_TAPS_PER_PHASE - 1U) : (j - 1U);
            }
        }

        f->mac_count += FIR_NUM_TAPS;
        f->idx = (f->idx + 1U) % FIR_TAPS_PER_PHASE;
        *out = q15_round_shift(acc);
    }
    return true;
}

void fir_decim_reference(const int32_t *in, uint32_t n_in,
                         int32_t *out, uint32_t *n_out)
{
    uint32_t produced = 0U;

    /*
     * The wasteful version, on purpose: full convolution at every input sample,
     * then keep one in M. This is what the polyphase implementation must match
     * exactly, and writing it the obvious way is the point - a clever reference
     * cannot be trusted to arbitrate a disagreement.
     */
    for (uint32_t n = 0U; n < n_in; n++) {
        int64_t acc = 0;

        for (uint32_t t = 0U; t < FIR_NUM_TAPS; t++) {
            const int64_t x = (n >= t) ? (int64_t)in[n - t] : 0;
            acc += x * (int64_t)k_fir_taps_q15[t];
        }

        if ((n % FIR_DECIM_M) == (FIR_DECIM_M - 1U)) {
            out[produced] = q15_round_shift(acc);
            produced++;
        }
    }
    *n_out = produced;
}
