"""Host-side reference implementations of everything the device computes.

This module exists to be *checked against*, not to replace the firmware. Every
function here has a counterpart in ``firmware/lib/`` and the pair is verified
against the same generated vector in ``fir_coeffs.json``. When the fixed-point
decimator on the device and the float reference here agree bit for bit on a
known input, "the DSP on the device is correct" stops being an assertion.

Three things are reimplemented:

``decimate_fixed``
    Bit-exact model of ``fir_decimate.c`` - Q15 taps, int64 accumulator,
    round-to-nearest shift. Must match the device exactly.

``decimate_float``
    The same filter in floating point. Must match ``decimate_fixed`` to within
    the quantisation of the taps, which is what tells you the fixed-point
    choice costs nothing meaningful.

``welch``/``thd_n``
    The spectral estimates, matching ``metrics.c`` including the periodic Hann
    window and the ``sum(w**2)`` normaliser.
"""

from __future__ import annotations

import json
import pathlib
from dataclasses import dataclass
from typing import Optional, Tuple

import numpy as np

_COEFF_PATH = pathlib.Path(__file__).with_name("fir_coeffs.json")

with _COEFF_PATH.open() as _fh:
    _COEFFS = json.load(_fh)

TAPS_Q15 = np.asarray(_COEFFS["taps_q15"], dtype=np.int64)
TAPS_FLOAT = np.asarray(_COEFFS["taps_float"], dtype=np.float64)
DECIMATION = int(_COEFFS["decimation"])
FS_IN_HZ = float(_COEFFS["fs_in_hz"])
FS_OUT_HZ = float(_COEFFS["fs_out_hz"])
NUM_TAPS = int(_COEFFS["num_taps"])
ALIAS_REJECTION_DB = float(_COEFFS["alias_rejection_db"])
PASSBAND_DROOP_DB = float(_COEFFS["passband_droop_db"])
MINUS3DB_HZ = float(_COEFFS["minus3db_hz"])

FFT_N = 1024
WELCH_SEGMENTS = 6


# --------------------------------------------------------------------------- #
# Decimation
# --------------------------------------------------------------------------- #
def decimate_fixed(x) -> np.ndarray:
    """Bit-exact model of the firmware's polyphase decimator.

    Int64 accumulation over Q15 taps, then ``(acc + 16384) >> 15``. NumPy's
    ``>>`` on a signed integer floors exactly as C's does on GCC, so this
    reproduces the device's output including its rounding.
    """
    x = np.asarray(x, dtype=np.int64)
    n_out = len(x) // DECIMATION
    out = np.empty(n_out, dtype=np.int64)

    # Build the convolution with explicit zero history so the startup transient
    # matches the device, which begins with zeroed delay lines.
    padded = np.concatenate([np.zeros(NUM_TAPS - 1, dtype=np.int64), x])
    for m in range(n_out):
        end = (m * DECIMATION) + DECIMATION - 1 + NUM_TAPS - 1
        window = padded[end - NUM_TAPS + 1: end + 1][::-1]
        acc = int(np.dot(window, TAPS_Q15))
        out[m] = (acc + (1 << 14)) >> 15
    return out


def decimate_float(x) -> np.ndarray:
    """The same filter in float64 - the 'what it should be' reference."""
    x = np.asarray(x, dtype=np.float64)
    filtered = np.convolve(x, TAPS_FLOAT)[: len(x)]
    return filtered[DECIMATION - 1:: DECIMATION]


def decimate_naive(x) -> np.ndarray:
    """No filter at all: keep every Mth sample.

    Here so the alias-rejection comparison has a control. This is what a
    decimator without an anti-alias filter does, and the difference between its
    output and ``decimate_fixed``'s on an out-of-band tone is the entire point
    of the filter.
    """
    return np.asarray(x)[DECIMATION - 1:: DECIMATION]


def frequency_response(freqs_hz, fs_hz: float = FS_IN_HZ) -> np.ndarray:
    """|H(f)| of the decimation filter, for plotting and for alias checks."""
    f = np.asarray(freqs_hz, dtype=np.float64)
    n = np.arange(NUM_TAPS)
    phase = np.exp(-2j * np.pi * np.outer(f / fs_hz, n))
    return np.abs(phase @ (TAPS_Q15 / 32768.0))


# --------------------------------------------------------------------------- #
# Spectral estimation
# --------------------------------------------------------------------------- #
def hann_periodic(n: int) -> np.ndarray:
    """Periodic Hann, divisor N.

    NumPy's ``np.hanning`` is the *symmetric* window (divisor N-1), which is
    correct for filter design and wrong for spectral analysis of a continuing
    signal: the symmetric form leaves a discontinuity between consecutive
    overlapping segments and raises the apparent noise floor slightly. The
    firmware builds the periodic form, so this does too.
    """
    return 0.5 - 0.5 * np.cos(2.0 * np.pi * np.arange(n) / n)


def welch(
    x,
    fs_hz: float,
    nperseg: int = FFT_N,
    segments: Optional[int] = None,
) -> Tuple[np.ndarray, np.ndarray]:
    """Averaged periodogram, matching ``metrics.c``.

    Returns ``(freqs_hz, psd)`` where psd is one-sided power per Hz in the
    squared units of the input.

    The normaliser is ``fs * sum(w**2)``. Using ``(sum w)**2`` instead - the
    right choice for reading a peak amplitude off the spectrum - makes the
    density wrong by 1.76 dB for a Hann window, consistently and invisibly.
    """
    x = np.asarray(x, dtype=np.float64)
    hop = nperseg // 2
    window = hann_periodic(nperseg)
    s2 = np.sum(window ** 2)

    starts = list(range(0, len(x) - nperseg + 1, hop))
    if segments is not None:
        starts = starts[:segments]
    if not starts:
        raise ValueError(f"need at least {nperseg} samples, got {len(x)}")

    acc = np.zeros(nperseg // 2 + 1)
    for s in starts:
        seg = x[s: s + nperseg]
        spec = np.fft.rfft((seg - seg.mean()) * window)
        acc += np.abs(spec) ** 2
    acc /= len(starts)

    psd = acc / (fs_hz * s2)
    psd[1:-1] *= 2.0
    return np.fft.rfftfreq(nperseg, 1.0 / fs_hz), psd


def periodogram(x, fs_hz: float, nperseg: int = FFT_N):
    """A single, unaveraged periodogram - the thing Welch's method improves on."""
    return welch(x, fs_hz, nperseg=nperseg, segments=1)


@dataclass
class ThdResult:
    thdn_db: float
    fundamental_hz: float
    valid: bool


def thd_n(
    psd: np.ndarray,
    freqs: np.ndarray,
    fund_half_bins: int = 8,
    dc_bins: int = 3,
) -> ThdResult:
    """THD+N from an averaged spectrum, matching the firmware's method.

    ``fund_half_bins`` defaults to 8 for the same reason it does on the device:
    a tone that is not bin-centred leaks into the Hann sidelobe skirt, and
    counting that skirt as distortion puts a hard floor near -33 dB on every
    reading. See ``firmware/include/metrics.h``.
    """
    power = np.asarray(psd, dtype=np.float64).copy()
    power[:dc_bins] = 0.0

    peak = int(np.argmax(power))
    total = float(power.sum())

    lo = peak - fund_half_bins
    hi = peak + fund_half_bins + 1
    if (lo < dc_bins) or (hi > len(power)) or (total <= 0.0):
        return ThdResult(float("nan"), 0.0, False)

    fund = float(power[lo:hi].sum())
    if not (0.5 * total < fund < total):
        # No dominant tone: the "peak" is just the luckiest noise bin, and
        # dividing noise by noise produces a confident number that means
        # nothing.
        return ThdResult(float("nan"), 0.0, False)

    ratio = (total - fund) / fund

    # Three-point parabolic interpolation on the log magnitude.
    a, b, c = (np.log10(power[peak - 1] + 1e-300),
               np.log10(power[peak] + 1e-300),
               np.log10(power[peak + 1] + 1e-300))
    denom = a - 2 * b + c
    delta = 0.5 * (a - c) / denom if abs(denom) > 1e-30 else 0.0
    if abs(delta) > 1.0:
        delta = 0.0
    df = freqs[1] - freqs[0]

    return ThdResult(10.0 * np.log10(ratio), float((peak + delta) * df), True)


def rms(x) -> float:
    """RMS with the mean removed, matching the device's 100 ms windows."""
    x = np.asarray(x, dtype=np.float64)
    return float(np.sqrt(np.mean((x - x.mean()) ** 2)))


def spectrum_scatter_db(psd: np.ndarray, skip: int = 10) -> float:
    """Standard deviation, in dB, of the bins of a spectrum.

    The figure of merit for an estimator's variance. A single periodogram of
    white noise scatters by ~5.6 dB no matter how long the transform is;
    averaging K of them reduces it by roughly sqrt(K).
    """
    db = 10.0 * np.log10(np.asarray(psd)[skip:] + 1e-300)
    return float(np.std(db))
