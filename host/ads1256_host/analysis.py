"""Noise-floor analysis: RMS, effective bits, and a Welch PSD.

Conversions
-----------
The ADS1256's full-scale differential input is ``+/-2 * VREF / PGA``.  With the
2.5 V reference on this board and PGA = 1 that is +/-5 V, and a code of
+8388607 (2^23 - 1) corresponds to +5 V.  Everything below follows from those
two facts; if you change the reference, change ``vref`` and nothing else.

Bit counts
----------
Two different numbers get called "resolution" and conflating them is how
datasheet comparisons go wrong:

``effective_bits``
    ``log2(FSR / rms)`` - the RMS-referred figure.  This is what TI's noise
    tables report, so it is the one to compare against the datasheet.

``noise_free_bits``
    ``log2(FSR / (6.6 * rms))`` - the peak-to-peak-referred figure, using the
    conventional 6.6-sigma crest factor.  This is the number of bits that are
    actually stable sample to sample, and it is always ~2.7 bits lower.

A 24-bit converter running at 30 kSPS delivering ~17 noise-free bits is normal
and expected.  Quoting "24-bit" as if all 24 were usable is not.
"""

from __future__ import annotations

from dataclasses import dataclass
from typing import Optional, Tuple

import numpy as np

FULL_SCALE_CODE = 8388607.0


def code_to_volts(codes, vref: float = 2.5, pga: int = 1):
    """Convert raw/corrected ADC codes to volts."""
    full_scale_v = 2.0 * vref / pga
    return np.asarray(codes, dtype=np.float64) * (full_scale_v / FULL_SCALE_CODE)


@dataclass
class NoiseResult:
    n_samples: int
    fs_hz: float
    mean_v: float
    rms_v: float
    peak_to_peak_v: float
    effective_bits: float
    noise_free_bits: float
    psd_freq_hz: Optional[np.ndarray] = None
    psd_v_per_rthz: Optional[np.ndarray] = None

    def summary(self) -> str:
        return (
            f"n={self.n_samples}  fs={self.fs_hz:.1f} Hz\n"
            f"  mean            : {self.mean_v * 1e6:10.3f} uV\n"
            f"  RMS noise       : {self.rms_v * 1e6:10.3f} uV\n"
            f"  peak-to-peak    : {self.peak_to_peak_v * 1e6:10.3f} uV\n"
            f"  effective bits  : {self.effective_bits:10.2f} (RMS-referred)\n"
            f"  noise-free bits : {self.noise_free_bits:10.2f} (6.6 sigma)\n"
        )


def _welch_psd(x: np.ndarray, fs: float, nperseg: int) -> Tuple[np.ndarray, np.ndarray]:
    """Welch PSD with a Hann window and 50 % overlap, in V/sqrt(Hz).

    Implemented directly rather than pulling in SciPy: it is fifteen lines, and
    the window normalisation is the part that has to be right.  ``S2`` below is
    the sum of squared window samples, which is the correct normaliser for a
    *power spectral density* (as opposed to ``S1**2`` for peak amplitude).
    """
    nperseg = min(nperseg, len(x))
    if nperseg < 8:
        raise ValueError("not enough samples for a PSD")

    step = nperseg // 2
    window = np.hanning(nperseg)
    s2 = np.sum(window ** 2)

    segments = [x[i:i + nperseg] for i in range(0, len(x) - nperseg + 1, step)]
    acc = np.zeros(nperseg // 2 + 1)
    for seg in segments:
        spec = np.fft.rfft((seg - seg.mean()) * window)
        acc += (np.abs(spec) ** 2)
    acc /= len(segments)

    psd = acc / (fs * s2)
    psd[1:-1] *= 2.0                      # fold the negative frequencies in
    freqs = np.fft.rfftfreq(nperseg, 1.0 / fs)
    return freqs, np.sqrt(psd)


def analyse_noise(
    codes,
    fs_hz: float,
    vref: float = 2.5,
    pga: int = 1,
    psd: bool = True,
    nperseg: int = 4096,
) -> NoiseResult:
    """Characterise a shorted-input capture.

    ``codes`` should be a 1-D array for a single channel.  The mean is removed
    before the RMS is taken: what is being measured is noise, and any residual
    offset is a calibration question, reported separately as ``mean_v``.
    """
    volts = code_to_volts(codes, vref, pga)
    if volts.size < 2:
        raise ValueError("need at least 2 samples")

    mean = float(volts.mean())
    centred = volts - mean
    rms = float(np.sqrt(np.mean(centred ** 2)))
    ptp = float(centred.max() - centred.min())
    fsr = 2.0 * (2.0 * vref / pga)        # full scale is bipolar: -FS .. +FS

    eff_bits = float(np.log2(fsr / rms)) if rms > 0 else float("inf")
    nf_bits = float(np.log2(fsr / (6.6 * rms))) if rms > 0 else float("inf")

    freqs = density = None
    if psd and volts.size >= 8:
        freqs, density = _welch_psd(centred, fs_hz, nperseg)

    return NoiseResult(
        n_samples=int(volts.size),
        fs_hz=fs_hz,
        mean_v=mean,
        rms_v=rms,
        peak_to_peak_v=ptp,
        effective_bits=eff_bits,
        noise_free_bits=nf_bits,
        psd_freq_hz=freqs,
        psd_v_per_rthz=density,
    )


def timestamp_continuity(t_us: np.ndarray, expected_period_us: float,
                         tolerance_us: float = 5.0) -> dict:
    """Check the device timestamps for gaps.

    This is the independent evidence that nothing was lost: the sequence
    counter proves no *packet* went missing, and the timestamps prove no
    *conversion* went missing.  They can disagree - a scan dropped inside the
    firmware before packetisation shows up here and nowhere else.
    """
    t = np.asarray(t_us, dtype=np.int64)
    if t.size < 2:
        return {"gaps": 0, "jitter_rms_us": 0.0, "max_deviation_us": 0.0}

    deltas = np.diff(t)
    deltas = np.where(deltas < 0, deltas + (1 << 32), deltas)  # TIM2 wrap
    deviation = deltas - expected_period_us

    return {
        "n_intervals": int(deltas.size),
        "mean_period_us": float(deltas.mean()),
        "gaps": int(np.count_nonzero(np.abs(deviation) > tolerance_us)),
        "jitter_rms_us": float(np.sqrt(np.mean(deviation ** 2))),
        "max_deviation_us": float(np.abs(deviation).max()),
    }
