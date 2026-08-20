/**
 * @file    fft.h
 * @brief   In-place radix-2 FFT, single precision, and a Hann window.
 *
 * Float rather than Q15, deliberately - and this is the opposite of the choice
 * made for the decimator and the calibration path, so it is worth saying why.
 *
 * A Q15 FFT has to rescale by 1/2 at every one of the log2(1024) = 10 stages to
 * avoid overflow, which costs 10 bits of dynamic range. On a noise floor being
 * characterised at 20 effective bits, that is most of the measurement. Block
 * floating point recovers some of it at the cost of considerable complexity.
 * The M4F's single-precision FPU has 24 bits of mantissa and no such problem.
 *
 * The objection to float elsewhere in this firmware is that touching the FPU
 * makes FreeRTOS lazily stack 33 extra words on preemption, adding jitter. That
 * objection does not apply here: the metrics task is the only FPU user in the
 * system and it sits at the lowest priority, off the acquisition deadline path
 * entirely. Nothing with a deadline is ever preempted while holding FPU state.
 *
 * A full complex transform is used on real input with the imaginary part zeroed,
 * rather than the real-input split-radix trick that would halve the work. The
 * transform costs about 0.06 % of the CPU at the rate it actually runs (one per
 * 500 ms); the split-radix version would save 0.03 % and roughly double the
 * amount of code that has to be right.
 */
#ifndef DAQ_FFT_H
#define DAQ_FFT_H

#include <stdint.h>

#define FFT_N       1024U
#define FFT_BINS    ((FFT_N / 2U) + 1U)   /* DC .. Nyquist inclusive */

/**
 * @brief Build the twiddle and window tables. Call once before fft_forward().
 * Idempotent.
 */
void fft_init(void);

/**
 * @brief In-place forward complex FFT of FFT_N points, natural order in and out.
 *
 * Unnormalised: the sum of the input appears in bin 0, so a DC input of
 * amplitude A gives |X[0]| = N*A. The PSD scaling in metrics.c accounts for it.
 */
void fft_forward(float *re, float *im);

/** @brief Apply the Hann window in place to FFT_N samples. */
void fft_apply_hann(float *x);

/**
 * @brief Sum of the squared Hann window - the correct PSD normaliser.
 *
 * For a power spectral *density* the normaliser is sum(w[n]^2), not
 * (sum w[n])^2. Using the latter - which is right for peak amplitude - gives a
 * density that is wrong by 1.76 dB for a Hann window, and it is wrong
 * consistently, so it looks like a real measurement.
 */
float fft_window_power(void);

#endif /* DAQ_FFT_H */
