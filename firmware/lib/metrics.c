#include "metrics.h"
#include <math.h>
#include <string.h>

/*
 * FFT scratch, shared. Only the metrics task ever calls into here and it is
 * single-threaded by construction, so one buffer pair suffices; making these
 * per-instance would cost 8 KB to no purpose.
 */
static float s_re[FFT_N];
static float s_im[FFT_N];

/** Full-scale code magnitude, as a float, for the dBFS reference. */
#define FS_CODES 8388607.0f

void metrics_init(metrics_t *m, float fs_hz)
{
    (void)memset(m, 0, sizeof(*m));
    fft_init();

    m->fs_hz = fs_hz;
    m->rms_window = (uint32_t)(fs_hz / (float)RMS_WINDOW_DIVISOR);
    if (m->rms_window == 0U) {
        m->rms_window = 1U;
    }
    for (uint32_t c = 0U; c < DAQ_NUM_CHANNELS; c++) {
        m->thdn_mdb[c] = THDN_INVALID;
    }
}

void metrics_reset(metrics_t *m)
{
    const float fs = m->fs_hz;
    metrics_init(m, fs);
}

float metrics_effective_bits(int32_t rms_codes)
{
    if (rms_codes <= 0) {
        return 0.0f;
    }
    /* Full-scale *range* is bipolar, hence the factor of two. */
    return log2f((2.0f * FS_CODES) / (float)rms_codes);
}

/* ------------------------------------------------------------------------ */
/* Welch                                                                     */
/* ------------------------------------------------------------------------ */

static void accumulate_segment(metrics_t *m)
{
    /* Copy the staged segment into the FFT scratch, remove its mean, window. */
    float mean = 0.0f;

    for (uint32_t i = 0U; i < FFT_N; i++) {
        mean += m->stage[i];
    }
    mean /= (float)FFT_N;

    for (uint32_t i = 0U; i < FFT_N; i++) {
        s_re[i] = m->stage[i] - mean;
        s_im[i] = 0.0f;
    }

    /*
     * Detrending by mean removal before windowing, not after. A DC offset
     * multiplied by the window becomes a scaled copy of the window's own
     * spectrum smeared across the low bins, which is indistinguishable from
     * real low-frequency content and will quietly dominate a noise-floor
     * measurement at the exact frequencies the measurement cares about.
     */
    fft_apply_hann(s_re);
    fft_forward(s_re, s_im);

    for (uint32_t k = 0U; k < FFT_BINS; k++) {
        const float p = (s_re[k] * s_re[k]) + (s_im[k] * s_im[k]);
        m->accum[k] += p;
    }
    m->segments++;
}

static void finalise_psd(metrics_t *m)
{
    const uint32_t ch = m->psd_channel;
    const float norm = m->fs_hz * fft_window_power() * (float)m->segments;

    uint32_t peak = 0U;
    float peak_p = 0.0f;
    float total_p = 0.0f;

    /*
     * Bins 0..2 are excluded from every power sum below. The Hann window has a
     * 3-bin-wide main lobe, so whatever residual DC survives mean removal lands
     * there; counting it would inflate both the noise total and any THD+N
     * figure computed from it.
     */
    for (uint32_t k = 3U; k < FFT_BINS; k++) {
        const float p = m->accum[k];
        total_p += p;
        if (p > peak_p) {
            peak_p = p;
            peak = k;
        }
    }

    /* ---- one-sided PSD in dBFS/Hz -------------------------------------- */
    for (uint32_t k = 0U; k < FFT_BINS; k++) {
        float s = m->accum[k] / norm;

        /* Fold the negative-frequency half into everything but DC and Nyquist. */
        if ((k != 0U) && (k != (FFT_BINS - 1U))) {
            s *= 2.0f;
        }
        s /= (FS_CODES * FS_CODES);           /* referenced to full scale */

        m->psd_dbfs[k] = (s > 0.0f) ? (10.0f * log10f(s)) : -300.0f;
    }

    /* ---- fundamental and THD+N ----------------------------------------- */
    m->fund_mhz[ch] = 0U;
    m->thdn_mdb[ch] = THDN_INVALID;

    if ((peak > THDN_FUND_HALF_BINS) &&
        (peak < (FFT_BINS - THDN_FUND_HALF_BINS - 1U)) &&
        (total_p > 0.0f)) {
        /* See THDN_FUND_HALF_BINS: this band is the window's leakage skirt,
           not just its main lobe. */
        float fund_p = 0.0f;
        for (uint32_t k = peak - THDN_FUND_HALF_BINS;
             k <= (peak + THDN_FUND_HALF_BINS); k++) {
            fund_p += m->accum[k];
        }

        /*
         * Only report THD+N when there is actually a tone. On a shorted input
         * the "peak" is just the largest noise bin, and dividing noise by noise
         * produces a confident-looking number that means nothing. Requiring the
         * fundamental to hold most of the total power is the cheap test for
         * "somebody applied a signal".
         */
        if ((fund_p > (0.5f * total_p)) && (fund_p < total_p)) {
            const float rest = total_p - fund_p;
            const float ratio = rest / fund_p;
            m->thdn_mdb[ch] = (int32_t)lrintf(10.0f * log10f(ratio) * 1000.0f);

            /*
             * Three-point parabolic interpolation on the log-magnitude puts the
             * fundamental between bins. Bin spacing here is 7.32 Hz; without
             * interpolation a 1 kHz tone reports as 998.5 Hz and looks like a
             * clock error rather than a quantised estimate.
             */
            {
                const float a = log10f(m->accum[peak - 1U] + 1e-30f);
                const float b = log10f(m->accum[peak] + 1e-30f);
                const float c = log10f(m->accum[peak + 1U] + 1e-30f);
                const float denom = (a - (2.0f * b)) + c;
                float delta = 0.0f;

                if (fabsf(denom) > 1e-20f) {
                    delta = (0.5f * (a - c)) / denom;
                    if ((delta > 1.0f) || (delta < -1.0f)) {
                        delta = 0.0f;
                    }
                }
                {
                    const float hz = ((float)peak + delta) * m->fs_hz / (float)FFT_N;
                    m->fund_mhz[ch] = (uint32_t)lrintf(hz * 1000.0f);
                }
            }
        }
    }

    m->psd_ready_channel = ch;
    m->psd_serial++;
    m->psd_valid = true;

    /* Rotate to the next channel and start a fresh average. */
    (void)memset(m->accum, 0, sizeof(m->accum));
    m->segments = 0U;
    m->fill = 0U;
    m->psd_channel = (ch + 1U) % DAQ_NUM_CHANNELS;
}

/* ------------------------------------------------------------------------ */
bool metrics_push(metrics_t *m, uint32_t channel, int32_t sample)
{
    bool rms_done = false;

    if (channel >= DAQ_NUM_CHANNELS) {
        return false;
    }

    /* ---- RMS, every channel ------------------------------------------- */
    m->sumsq[channel] += (int64_t)sample * (int64_t)sample;
    m->n_rms[channel]++;

    if (m->n_rms[channel] >= m->rms_window) {
        const double mean = (double)m->sumsq[channel] / (double)m->n_rms[channel];
        m->rms[channel] = (int32_t)lrint(sqrt(mean));
        m->rms_valid[channel] = true;
        m->sumsq[channel] = 0;
        m->n_rms[channel] = 0U;
        rms_done = true;
    }

    /* ---- Welch, only the channel currently under analysis -------------- */
    if (channel == m->psd_channel) {
        m->stage[m->fill] = (float)sample;
        m->fill++;

        if (m->fill >= FFT_N) {
            accumulate_segment(m);

            if (m->segments >= WELCH_SEGMENTS) {
                finalise_psd(m);
            } else {
                /* Slide by the hop, keeping the overlap. */
                (void)memmove(&m->stage[0], &m->stage[WELCH_HOP],
                              (FFT_N - WELCH_HOP) * sizeof(float));
                m->fill = FFT_N - WELCH_HOP;
            }
        }
    }

    return rms_done;
}

bool metrics_take_psd(metrics_t *m)
{
    if (!m->psd_valid) {
        return false;
    }
    m->psd_valid = false;
    return true;
}
