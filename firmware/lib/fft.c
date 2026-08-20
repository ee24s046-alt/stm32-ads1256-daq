#include "fft.h"
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static float s_tw_re[FFT_N / 2U];
static float s_tw_im[FFT_N / 2U];
static float s_hann[FFT_N];
static float s_hann_power;
static uint8_t s_ready;

void fft_init(void)
{
    if (s_ready != 0U) {
        return;
    }

    for (uint32_t i = 0U; i < (FFT_N / 2U); i++) {
        const double a = (-2.0 * M_PI * (double)i) / (double)FFT_N;
        s_tw_re[i] = (float)cos(a);
        s_tw_im[i] = (float)sin(a);
    }

    /*
     * Periodic Hann (divisor N), not symmetric (divisor N-1). For spectral
     * analysis of a continuing signal the periodic form is correct; the
     * symmetric form leaves a small discontinuity between consecutive
     * overlapping segments and raises the apparent noise floor slightly.
     * NumPy's np.hanning() is the symmetric one, which is why the host
     * reference builds its window explicitly rather than calling it.
     */
    s_hann_power = 0.0f;
    for (uint32_t i = 0U; i < FFT_N; i++) {
        const double w = 0.5 - (0.5 * cos((2.0 * M_PI * (double)i) / (double)FFT_N));
        s_hann[i] = (float)w;
        s_hann_power += s_hann[i] * s_hann[i];
    }

    s_ready = 1U;
}

float fft_window_power(void)
{
    return s_hann_power;
}

void fft_apply_hann(float *x)
{
    for (uint32_t i = 0U; i < FFT_N; i++) {
        x[i] *= s_hann[i];
    }
}

void fft_forward(float *re, float *im)
{
    /* ---- bit-reversal permutation ------------------------------------
     * Computed by the incrementing-reversed-counter trick rather than with a
     * table: it is O(N) with no storage, and a 1024-entry table would cost
     * more RAM than the twiddles.
     */
    {
        uint32_t j = 0U;

        for (uint32_t i = 1U; i < FFT_N; i++) {
            uint32_t bit = FFT_N >> 1U;

            while ((j & bit) != 0U) {
                j ^= bit;
                bit >>= 1U;
            }
            j |= bit;

            if (i < j) {
                float t = re[i]; re[i] = re[j]; re[j] = t;
                t = im[i];       im[i] = im[j]; im[j] = t;
            }
        }
    }

    /* ---- Cooley-Tukey, decimation in time ---------------------------- */
    for (uint32_t len = 2U; len <= FFT_N; len <<= 1U) {
        const uint32_t half = len >> 1U;
        const uint32_t step = FFT_N / len;

        for (uint32_t i = 0U; i < FFT_N; i += len) {
            uint32_t tw = 0U;

            for (uint32_t k = 0U; k < half; k++) {
                const float wr = s_tw_re[tw];
                const float wi = s_tw_im[tw];
                const uint32_t a = i + k;
                const uint32_t b = a + half;

                const float xr = (re[b] * wr) - (im[b] * wi);
                const float xi = (re[b] * wi) + (im[b] * wr);

                re[b] = re[a] - xr;
                im[b] = im[a] - xi;
                re[a] += xr;
                im[a] += xi;

                tw += step;
            }
        }
    }
}
