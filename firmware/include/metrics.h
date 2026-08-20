/**
 * @file    metrics.h
 * @brief   On-device live metrics: RMS, averaged PSD (Welch), THD+N.
 *
 * Why averaging, and not just a longer FFT
 * ----------------------------------------
 * This is the part that is most often got wrong, so it is worth being explicit.
 *
 * A single periodogram is a *terribly* noisy estimator. Each bin is the squared
 * magnitude of one complex Gaussian sample, so its standard deviation equals
 * its mean - about 5.6 dB of scatter - no matter how long the transform is.
 * Taking a 16384-point FFT instead of a 1024-point one buys 16x finer frequency
 * resolution and *exactly zero* reduction in variance. The estimate still
 * scatters by 5.6 dB; there are simply more bins doing it.
 *
 * Variance comes down only by averaging independent estimates. Welch's method
 * splits the record into overlapping segments, windows each, and averages their
 * periodograms. With K averages the variance falls as 1/K. This implementation
 * uses K = 6 segments of 1024 points at 50 % overlap, which cuts the scatter
 * from ~5.6 dB to ~2.0 dB. (50 % overlapped Hann segments are not fully
 * independent - the effective K is about 0.9 x 6 - which is why the improvement
 * is a little short of sqrt(6).)
 *
 * `ads1256 dsp --compare` draws exactly this: one periodogram against the
 * averaged estimate, same data, same resolution.
 *
 * Rates
 * -----
 * RMS  : 100 ms windows, all channels, continuously.
 * PSD  : 1024-point, 50 % overlap, 6 segments -> one block per ~478 ms.
 * THD+N: computed from the same averaged spectrum, so it inherits the averaging.
 *
 * THD+N and its floor
 * -------------------
 * THD+N is computed from the same averaged spectrum: everything outside a band
 * around the fundamental, divided by everything inside it.
 *
 * The honest limitation, stated because it bounds what the number means: a tone
 * that does not land exactly on a bin leaks through the Hann window's sidelobes,
 * and that leakage is counted as distortion. With a +/-8 bin fundamental band
 * the leakage floor is around -65 dB, so a reported THD+N near that figure is
 * measuring the window, not the signal chain. Getting below it needs coherent
 * sampling (a source locked to fs/N) or a Blackman-Harris window for the THD
 * path specifically. Hann is kept here because the PSD is the primary product
 * and Hann is the right window for a noise density.
 *
 * The PSD runs on one channel at a time, round-robin, because four simultaneous
 * Welch accumulators would cost 4 x (4 KB staging + 2 KB accumulator) = 24 KB of
 * SRAM to produce a result nobody reads four times a second. Each channel is
 * updated every ~1.9 s instead. RMS, which is cheap, does run on all four
 * continuously.
 */
#ifndef DAQ_METRICS_H
#define DAQ_METRICS_H

#include "daq_types.h"
#include "fft.h"

/** Averages per PSD block. See the variance discussion above. */
#define WELCH_SEGMENTS   6U
#define WELCH_HOP        (FFT_N / 2U)          /* 50 % overlap */

/** RMS integration window, as a fraction of the sample rate (100 ms). */
#define RMS_WINDOW_DIVISOR 10U

/** Sentinel for "not enough signal to compute a meaningful THD+N". */
#define THDN_INVALID     INT32_MIN

/**
 * Half-width, in bins, of the band treated as "the fundamental".
 *
 * Not the 3-bin Hann main lobe: a tone at an arbitrary frequency sits between
 * bins and its energy spreads into the sidelobe skirt. Counting that skirt as
 * distortion puts a hard floor of about -33 dB on every THD+N reading, which
 * is what a +/-2 bin window actually produces. Widening to +/-8 lowers the
 * floor to roughly -65 dB at the cost of blinding the measurement to any
 * distortion product within 59 Hz of the fundamental - and there is no such
 * thing as a harmonic that close.
 */
#define THDN_FUND_HALF_BINS 8U

typedef struct {
    /* ---- RMS, all channels, always running ---------------------------- */
    int64_t  sumsq[DAQ_NUM_CHANNELS];
    uint32_t n_rms[DAQ_NUM_CHANNELS];
    int32_t  rms[DAQ_NUM_CHANNELS];        /**< last completed window, codes  */
    bool     rms_valid[DAQ_NUM_CHANNELS];
    uint32_t rms_window;                   /**< samples per window            */

    /* ---- Welch, one channel at a time --------------------------------- */
    uint32_t psd_channel;
    float    stage[FFT_N];                 /**< segment staging, 50 % overlap */
    uint32_t fill;
    float    accum[FFT_BINS];              /**< running sum of periodograms   */
    uint32_t segments;

    /* ---- Results ------------------------------------------------------ */
    float    psd_dbfs[FFT_BINS];           /**< dBFS/Hz, last completed block */
    uint32_t psd_ready_channel;
    uint32_t psd_serial;                   /**< increments per completed block*/
    bool     psd_valid;
    int32_t  thdn_mdb[DAQ_NUM_CHANNELS];   /**< milli-dB, or THDN_INVALID     */
    uint32_t fund_mhz[DAQ_NUM_CHANNELS];   /**< milli-Hz, 0 if none found     */

    float    fs_hz;
} metrics_t;

/**
 * @brief Initialise for a given sample rate.
 *
 * @param fs_hz rate of the stream being fed in - 7500 Hz in wideband mode
 *              (post-decimation), 1875 Hz per channel in 4-channel mux mode.
 */
void metrics_init(metrics_t *m, float fs_hz);

/** @brief Discard partial state without forgetting the sample rate. */
void metrics_reset(metrics_t *m);

/**
 * @brief Feed one sample for one channel.
 * @return true if a 100 ms RMS window completed for @p channel on this call.
 */
bool metrics_push(metrics_t *m, uint32_t channel, int32_t sample);

/**
 * @brief true if a new averaged PSD block is available since last checked.
 *
 * Clears the flag. The block is in @c psd_dbfs and applies to channel
 * @c psd_ready_channel.
 */
bool metrics_take_psd(metrics_t *m);

/**
 * @brief Convert an RMS in codes to effective bits, for the UART report.
 * @return log2(full-scale range / rms), or 0 if rms is zero.
 */
float metrics_effective_bits(int32_t rms_codes);

#endif /* DAQ_METRICS_H */
