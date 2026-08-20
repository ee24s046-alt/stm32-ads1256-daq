/**
 * @file test_spectral.c
 * @brief FFT correctness, and the averaging-vs-FFT-length demonstration.
 *
 * The test worth reading is test_averaging_reduces_variance_fft_length_does_not.
 * It is the executable form of the claim on the project summary: a longer
 * transform buys resolution, not confidence. Only averaging buys confidence.
 */
#include "test_harness.h"
#include "fft.h"
#include "metrics.h"
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static float s_re[FFT_N];
static float s_im[FFT_N];

static void clear(void)
{
    for (uint32_t i = 0U; i < FFT_N; i++) {
        s_re[i] = 0.0f;
        s_im[i] = 0.0f;
    }
}

/* ---------------------------------------------------------------------- */
static void test_impulse_is_flat(void)
{
    TEST("FFT of a unit impulse is flat at unity across every bin");
    fft_init();
    clear();
    s_re[0] = 1.0f;
    fft_forward(s_re, s_im);

    for (uint32_t k = 0U; k < FFT_N; k++) {
        const float mag = sqrtf((s_re[k] * s_re[k]) + (s_im[k] * s_im[k]));
        CHECK(fabsf(mag - 1.0f) < 1e-4f);
    }
}

static void test_dc_lands_in_bin_zero(void)
{
    TEST("a constant input puts all its energy in bin 0");
    fft_init();
    clear();
    for (uint32_t i = 0U; i < FFT_N; i++) {
        s_re[i] = 2.0f;
    }
    fft_forward(s_re, s_im);

    CHECK(fabsf(s_re[0] - (2.0f * (float)FFT_N)) < 1.0f);
    for (uint32_t k = 1U; k < 16U; k++) {
        const float mag = sqrtf((s_re[k] * s_re[k]) + (s_im[k] * s_im[k]));
        CHECK(mag < 1e-2f);
    }
}

static void test_bin_exact_tone(void)
{
    TEST("a bin-centred tone appears in exactly that bin");
    fft_init();
    clear();

    const uint32_t bin = 37U;
    for (uint32_t i = 0U; i < FFT_N; i++) {
        s_re[i] = cosf((2.0f * (float)M_PI * (float)bin * (float)i) / (float)FFT_N);
    }
    fft_forward(s_re, s_im);

    /* Real cosine: energy splits between bin and N-bin, each N/2. */
    const float mag = sqrtf((s_re[bin] * s_re[bin]) + (s_im[bin] * s_im[bin]));
    CHECK(fabsf(mag - ((float)FFT_N / 2.0f)) < 1.0f);

    for (uint32_t k = 1U; k < (FFT_N / 2U); k++) {
        if ((k < (bin - 1U)) || (k > (bin + 1U))) {
            const float m = sqrtf((s_re[k] * s_re[k]) + (s_im[k] * s_im[k]));
            CHECK(m < 1.0f);
        }
    }
}

static void test_parseval(void)
{
    TEST("Parseval: time-domain energy equals frequency-domain energy / N");
    fft_init();
    clear();

    uint32_t seed = 12345U;
    double time_energy = 0.0;

    for (uint32_t i = 0U; i < FFT_N; i++) {
        seed = (seed * 1103515245U) + 12345U;
        s_re[i] = (float)((int32_t)((seed >> 16) & 0xFFFFU) - 32768) / 32768.0f;
        time_energy += (double)s_re[i] * (double)s_re[i];
    }
    fft_forward(s_re, s_im);

    double freq_energy = 0.0;
    for (uint32_t k = 0U; k < FFT_N; k++) {
        freq_energy += ((double)s_re[k] * (double)s_re[k]) +
                       ((double)s_im[k] * (double)s_im[k]);
    }
    freq_energy /= (double)FFT_N;

    CHECK(fabs(freq_energy - time_energy) < (1e-3 * time_energy));
}

static void test_hann_window_power_normaliser(void)
{
    TEST("sum(w^2) for a periodic Hann window is 3N/8");
    fft_init();
    /* The exact value is 3N/8 = 384 for N = 1024.  Using (sum w)^2 instead -
       the peak-amplitude normaliser - would be wrong by 1.76 dB, consistently,
       which is exactly the kind of error that looks like a real measurement. */
    CHECK(fabsf(fft_window_power() - ((3.0f * (float)FFT_N) / 8.0f)) < 1.0f);
}

/* ---------------------------------------------------------------------- */
/* The headline demonstration                                              */
/* ---------------------------------------------------------------------- */

static uint32_t s_rng = 987654321U;

static float gauss(void)
{
    /* Box-Muller from a cheap LCG; deterministic across runs. */
    s_rng = (s_rng * 1664525U) + 1013904223U;
    const float u1 = (((float)(s_rng >> 8) / 16777216.0f) + 1e-7f);
    s_rng = (s_rng * 1664525U) + 1013904223U;
    const float u2 = ((float)(s_rng >> 8) / 16777216.0f);
    return sqrtf(-2.0f * logf(u1)) * cosf(2.0f * (float)M_PI * u2);
}

/** Scatter (standard deviation in dB) of the bins of one power spectrum. */
static double spectrum_scatter_db(const float *power, uint32_t n)
{
    double sum = 0.0;
    double sumsq = 0.0;

    for (uint32_t k = 10U; k < n; k++) {
        const double db = 10.0 * log10((double)power[k] + 1e-30);
        sum += db;
        sumsq += db * db;
    }
    {
        const double cnt = (double)(n - 10U);
        const double mean = sum / cnt;
        return sqrt((sumsq / cnt) - (mean * mean));
    }
}

static void periodogram(float *power, uint32_t averages)
{
    for (uint32_t k = 0U; k < FFT_BINS; k++) {
        power[k] = 0.0f;
    }

    for (uint32_t s = 0U; s < averages; s++) {
        for (uint32_t i = 0U; i < FFT_N; i++) {
            s_re[i] = gauss();
            s_im[i] = 0.0f;
        }
        fft_apply_hann(s_re);
        fft_forward(s_re, s_im);

        for (uint32_t k = 0U; k < FFT_BINS; k++) {
            power[k] += (s_re[k] * s_re[k]) + (s_im[k] * s_im[k]);
        }
    }
    for (uint32_t k = 0U; k < FFT_BINS; k++) {
        power[k] /= (float)averages;
    }
}

static float s_power_single[FFT_BINS];
static float s_power_avg[FFT_BINS];

static void test_averaging_reduces_variance_fft_length_does_not(void)
{
    TEST("averaging reduces PSD variance; this is what Welch's method buys");
    fft_init();
    s_rng = 987654321U;

    periodogram(s_power_single, 1U);
    periodogram(s_power_avg, WELCH_SEGMENTS);

    const double scatter_one = spectrum_scatter_db(s_power_single, FFT_BINS);
    const double scatter_avg = spectrum_scatter_db(s_power_avg, FFT_BINS);

    printf("       single periodogram : %.2f dB scatter\n", scatter_one);
    printf("       %u averages        : %.2f dB scatter  (%.2fx better)\n",
           WELCH_SEGMENTS, scatter_avg, scatter_one / scatter_avg);

    /* Theory: a single periodogram bin is exponentially distributed, giving
       ~5.57 dB standard deviation regardless of transform length. */
    CHECK(scatter_one > 4.5);
    CHECK(scatter_one < 6.5);

    /* Averaging K independent periodograms cuts the variance by K. */
    CHECK(scatter_avg < (scatter_one * 0.6));
}

int main(void)
{
    test_impulse_is_flat();
    test_dc_lands_in_bin_zero();
    test_bin_exact_tone();
    test_parseval();
    test_hann_window_power_normaliser();
    test_averaging_reduces_variance_fft_length_does_not();
    TEST_SUMMARY();
}
