/**
 * @file test_metrics.c
 * @brief RMS windows, THD+N, and the "do not report THD+N on noise" guard.
 */
#include "test_harness.h"
#include "metrics.h"
#include "packet.h"
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define FS 7500.0f

static metrics_t s_m;

static uint32_t s_rng = 24680U;

static float gauss(void)
{
    s_rng = (s_rng * 1664525U) + 1013904223U;
    const float u1 = (((float)(s_rng >> 8) / 16777216.0f) + 1e-7f);
    s_rng = (s_rng * 1664525U) + 1013904223U;
    const float u2 = ((float)(s_rng >> 8) / 16777216.0f);
    return sqrtf(-2.0f * logf(u1)) * cosf(2.0f * (float)M_PI * u2);
}

/* ---------------------------------------------------------------------- */
static void test_rms_window_length_and_value(void)
{
    TEST("RMS integrates over exactly 100 ms and recovers a known level");
    metrics_init(&s_m, FS);
    CHECK_EQ(s_m.rms_window, 750U);          /* 7500 Hz / 10 */

    /* A square wave of +/-1000 has an RMS of exactly 1000. */
    bool completed = false;
    for (uint32_t i = 0U; i < 750U; i++) {
        completed = metrics_push(&s_m, 0U, ((i % 2U) == 0U) ? 1000 : -1000);
    }
    CHECK(completed);
    CHECK(s_m.rms_valid[0]);
    CHECK_EQ(s_m.rms[0], 1000);

    /* ...and it must not fire again until another full window. */
    CHECK(!metrics_push(&s_m, 0U, 0));
}

static void test_rms_of_a_sine(void)
{
    TEST("RMS of a sine is amplitude / sqrt(2)");
    metrics_init(&s_m, FS);

    const double amp = 500000.0;
    for (uint32_t i = 0U; i < 750U; i++) {
        const double v = amp * sin(2.0 * M_PI * 300.0 * (double)i / (double)FS);
        (void)metrics_push(&s_m, 1U, (int32_t)lrint(v));
    }
    CHECK(s_m.rms_valid[1]);
    {
        const double expect = amp / sqrt(2.0);
        const double err = fabs((double)s_m.rms[1] - expect) / expect;
        CHECK(err < 0.02);
    }
}

static void test_channels_are_independent(void)
{
    TEST("each channel keeps its own RMS accumulator");
    metrics_init(&s_m, FS);

    for (uint32_t i = 0U; i < 750U; i++) {
        (void)metrics_push(&s_m, 0U, 100);
        (void)metrics_push(&s_m, 1U, 200);
        (void)metrics_push(&s_m, 2U, 400);
        (void)metrics_push(&s_m, 3U, 800);
    }
    CHECK_EQ(s_m.rms[0], 100);
    CHECK_EQ(s_m.rms[1], 200);
    CHECK_EQ(s_m.rms[2], 400);
    CHECK_EQ(s_m.rms[3], 800);
}

static void test_effective_bits(void)
{
    TEST("effective bits is log2(full-scale range / RMS)");
    /* 8.74 uV RMS on a +/-5 V span is 14.6 codes, which is 20.1 bits. */
    const float b = metrics_effective_bits(15);
    CHECK(b > 20.0f);
    CHECK(b < 20.2f);
    CHECK_EQ((int)metrics_effective_bits(0), 0);
}

/** Drive enough samples through channel 0 to complete one averaged PSD block. */
static uint32_t drive_psd(double tone_hz, double amp, double noise)
{
    /* 1024 for the first segment plus a hop for each subsequent one. */
    const uint32_t needed = FFT_N + ((WELCH_SEGMENTS - 1U) * WELCH_HOP) + 16U;

    for (uint32_t i = 0U; i < needed; i++) {
        double v = noise * (double)gauss();
        if (amp > 0.0) {
            v += amp * sin(2.0 * M_PI * tone_hz * (double)i / (double)FS);
        }
        (void)metrics_push(&s_m, 0U, (int32_t)lrint(v));
    }
    return needed;
}

static void test_psd_block_completes_and_finds_the_tone(void)
{
    TEST("a completed PSD block locates a 1 kHz tone to within a bin");
    metrics_init(&s_m, FS);
    s_rng = 24680U;

    (void)drive_psd(1000.0, 1000000.0, 20.0);

    CHECK(metrics_take_psd(&s_m));
    CHECK_EQ(s_m.psd_ready_channel, 0U);
    CHECK_EQ(s_m.psd_serial, 1U);

    /* Bin spacing is 7.32 Hz; parabolic interpolation should do far better. */
    printf("       fundamental reported at %.3f Hz\n",
           (double)s_m.fund_mhz[0] / 1000.0);
    CHECK(s_m.fund_mhz[0] > 995000U);
    CHECK(s_m.fund_mhz[0] < 1005000U);

    /* And the PSD must peak at the corresponding bin. */
    {
        uint32_t peak = 0U;
        float best = -1e30f;
        for (uint32_t k = 3U; k < DAQ_PSD_BINS; k++) {
            if (s_m.psd_dbfs[k] > best) {
                best = s_m.psd_dbfs[k];
                peak = k;
            }
        }
        const double hz = (double)peak * (double)FS / (double)FFT_N;
        CHECK(fabs(hz - 1000.0) < 10.0);
    }

    /* Taking it twice must not report a stale block as new. */
    CHECK(!metrics_take_psd(&s_m));
}

static void test_thdn_of_a_clean_tone_is_low(void)
{
    TEST("THD+N of a clean tone is strongly negative");
    metrics_init(&s_m, FS);
    s_rng = 13579U;

    (void)drive_psd(1000.0, 1000000.0, 30.0);
    CHECK(metrics_take_psd(&s_m));

    CHECK(s_m.thdn_mdb[0] != THDN_INVALID);
    printf("       clean tone THD+N = %.2f dB\n",
           (double)s_m.thdn_mdb[0] / 1000.0);
    /* The bound is the Hann leakage floor (see THDN_FUND_HALF_BINS), not the
       synthesised signal's own purity - the tone here is ~87 dB above its
       noise, so what is being measured is the window. */
    CHECK(s_m.thdn_mdb[0] < -55000);          /* better than -55 dB */
}

static void test_thdn_degrades_with_distortion(void)
{
    TEST("adding a harmonic makes THD+N worse by roughly the expected amount");
    metrics_init(&s_m, FS);
    s_rng = 11111U;

    /* Fundamental plus a third harmonic at -26 dB => THD+N near -26 dB. */
    const uint32_t needed = FFT_N + ((WELCH_SEGMENTS - 1U) * WELCH_HOP) + 16U;
    for (uint32_t i = 0U; i < needed; i++) {
        const double t = (double)i / (double)FS;
        const double v = (1000000.0 * sin(2.0 * M_PI * 1000.0 * t))
                       + (50000.0 * sin(2.0 * M_PI * 3000.0 * t));
        (void)metrics_push(&s_m, 0U, (int32_t)lrint(v));
    }
    CHECK(metrics_take_psd(&s_m));

    printf("       with 5%% third harmonic THD+N = %.2f dB\n",
           (double)s_m.thdn_mdb[0] / 1000.0);
    CHECK(s_m.thdn_mdb[0] > -30000);
    CHECK(s_m.thdn_mdb[0] < -20000);
}

static void test_thdn_refuses_to_report_on_pure_noise(void)
{
    TEST("THD+N is reported INVALID when there is no tone - not a made-up number");
    metrics_init(&s_m, FS);
    s_rng = 55555U;

    (void)drive_psd(0.0, 0.0, 1000.0);        /* noise only */
    CHECK(metrics_take_psd(&s_m));

    /*
     * On a shorted input the largest bin is just the luckiest noise bin.
     * Dividing noise by noise yields a confident-looking figure that means
     * nothing, and it would appear on a live readout as though the instrument
     * had measured something.
     */
    CHECK_EQ(s_m.thdn_mdb[0], THDN_INVALID);
    CHECK_EQ(s_m.fund_mhz[0], 0U);
}

static void test_psd_rotates_across_channels(void)
{
    TEST("PSD analysis round-robins so every channel is eventually covered");
    metrics_init(&s_m, FS);
    s_rng = 99999U;

    const uint32_t needed = FFT_N + ((WELCH_SEGMENTS - 1U) * WELCH_HOP) + 16U;

    for (uint32_t block = 0U; block < DAQ_NUM_CHANNELS; block++) {
        const uint32_t ch = s_m.psd_channel;
        for (uint32_t i = 0U; i < needed; i++) {
            /* Feed every channel; only the selected one is analysed. */
            for (uint32_t c = 0U; c < DAQ_NUM_CHANNELS; c++) {
                (void)metrics_push(&s_m, c, (int32_t)lrintf(1000.0f * gauss()));
            }
        }
        CHECK(metrics_take_psd(&s_m));
        CHECK_EQ(s_m.psd_ready_channel, ch);
    }
    CHECK_EQ(s_m.psd_serial, DAQ_NUM_CHANNELS);
}

int main(void)
{
    test_rms_window_length_and_value();
    test_rms_of_a_sine();
    test_channels_are_independent();
    test_effective_bits();
    test_psd_block_completes_and_finds_the_tone();
    test_thdn_of_a_clean_tone_is_low();
    test_thdn_degrades_with_distortion();
    test_thdn_refuses_to_report_on_pure_noise();
    test_psd_rotates_across_channels();
    TEST_SUMMARY();
}
