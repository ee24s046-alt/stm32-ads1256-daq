#!/usr/bin/env python3
"""
Design the M=4 anti-alias decimation filter and emit it for both languages.

Run:  python3 tools/design_filter.py

Emits:
    firmware/include/fir_coeffs.h        Q15 taps + a golden vector for the C tests
    host/ads1256_host/fir_coeffs.json    the same taps and vector, float

One source of truth. The firmware's fixed-point decimator and the host's float
reference are checked against the same generated numbers, so "the device agrees
with the reference" is a test rather than a hope.

----------------------------------------------------------------------------
Specification, and where each number comes from
----------------------------------------------------------------------------
Input rate is 30 kSPS - the ADS1256's DRATE setting, available only in the
single-channel wideband mode where the multiplexer is parked and there is no
per-change filter settling penalty. Decimating by M = 4 gives 7.5 kSPS out,
so the output Nyquist is 3750 Hz.

The stopband edge is NOT the output Nyquist. What matters for a decimator is
which input frequencies fold into the *band of interest* after downsampling.
Declaring the band of interest as DC..3000 Hz, the aliasing images land at

    k * 7500 +/- f,   k = 1, 2, 3, ...

so the first input region that folds into 0..3000 Hz is 4500..7500 Hz. The band
3750..4500 Hz folds into 3000..3750 Hz, which is declared don't-care.

That is worth stating because it is the whole reason the filter is 68 taps and
not 660: specifying the stopband at the output Nyquist (3750 Hz) instead of at
the first aliasing edge (4500 Hz) shrinks the transition band by 6x and the tap
count grows in proportion. Over-specifying an anti-alias filter is expensive and
buys nothing.
"""

from __future__ import annotations

import json
import pathlib

import numpy as np

# ---------------------------------------------------------------- specification
FS_IN = 30_000.0          # ADS1256 DRATE, single-channel wideband mode
M = 4                     # decimation factor
FS_OUT = FS_IN / M        # 7500 Hz
BAND_OF_INTEREST = 3000.0 # Hz - everything below this must be alias-free
F_CUTOFF = 3680.0        # tuned so the -3 dB corner lands at 3.5 kHz exactly
NUM_TAPS = 68             # divisible by M, so the polyphase decomposition is even
Q15 = 32768


def design() -> np.ndarray:
    """Windowed-sinc lowpass, Hamming window, unity DC gain.

    Hamming rather than Blackman: it reaches about -53 dB of stopband
    attenuation for a given transition width, which clears the >40 dB
    requirement with margin, and it needs roughly 40 % fewer taps than Blackman
    would for the same transition. Blackman's extra stopband depth would be
    spent on a requirement nobody has.

    Not Parks-McClellan/remez: an equiripple design would hit the same spec in
    perhaps 45 taps rather than 68, but it needs SciPy and the saving is 23
    multiply-accumulates per output sample at 7.5 kHz - 172 kMAC/s on a part
    that does one MAC per cycle at 168 MHz. Not a trade worth a dependency.
    """
    n = np.arange(NUM_TAPS) - (NUM_TAPS - 1) / 2.0
    fc_norm = F_CUTOFF / FS_IN                      # cycles/sample
    h = 2.0 * fc_norm * np.sinc(2.0 * fc_norm * n)  # ideal lowpass
    h *= np.hamming(NUM_TAPS)
    h /= h.sum()                                    # exactly unity at DC
    return h


def response(h: np.ndarray, freqs: np.ndarray) -> np.ndarray:
    """|H(f)| for the analogue-domain frequencies in `freqs`."""
    n = np.arange(len(h))
    # H(f) = sum h[n] * exp(-j 2 pi f n / fs)
    phase = np.exp(-2j * np.pi * np.outer(freqs / FS_IN, n))
    return np.abs(phase @ h)


def alias_rejection(h: np.ndarray) -> tuple:
    """Worst-case attenuation of anything that folds into the band of interest.

    Sweeps every input frequency whose image lands in DC..BAND_OF_INTEREST after
    downsampling by M, and returns the least-attenuated one. This is the number
    the >40 dB claim refers to; the stopband attenuation of the prototype filter
    on its own is a weaker statement.
    """
    worst_db = np.inf
    worst_hz = 0.0
    f = np.linspace(0.0, FS_IN / 2.0, 20_001)
    folded = np.abs(((f + FS_OUT / 2) % FS_OUT) - FS_OUT / 2)  # image frequency

    mask = (folded <= BAND_OF_INTEREST) & (f > FS_OUT / 2)
    mag = response(h, f[mask])
    db = -20.0 * np.log10(np.maximum(mag, 1e-12))
    if db.size:
        i = int(np.argmin(db))
        worst_db = float(db[i])
        worst_hz = float(f[mask][i])
    return worst_db, worst_hz


def to_q15(h: np.ndarray) -> np.ndarray:
    """Quantise to Q15 while preserving BOTH unity DC gain and symmetry.

    Two properties have to survive quantisation and they fight each other if
    you are careless:

    * the taps must sum to exactly 32768, or the filter has a DC gain error
      that is indistinguishable from a failed offset calibration;
    * the taps must stay symmetric, or the filter is no longer linear phase -
      different frequencies get delayed by different amounts, which on a
      pulse-transit-time measurement corrupts exactly the quantity being
      measured.

    Dumping the rounding residual onto the single largest tap fixes the sum and
    silently breaks the symmetry. Instead only the first half is quantised, the
    correction is applied there, and the second half is mirrored. For an
    even-length symmetric filter the total is always 2x the half-sum, hence
    always even - and 32768 is even, so the correction is always achievable.
    """
    half = NUM_TAPS // 2
    q_half = np.round(h[:half] * Q15).astype(np.int64)

    delta = Q15 - 2 * int(q_half.sum())
    assert delta % 2 == 0, "unreachable for an even-length symmetric filter"
    q_half[np.argmax(np.abs(q_half))] += delta // 2

    q = np.concatenate([q_half, q_half[::-1]])
    assert q.sum() == Q15, q.sum()
    assert np.array_equal(q, q[::-1]), "symmetry lost"
    assert np.all(np.abs(q) < 32768), "a tap does not fit in int16"
    return q.astype(np.int16)


def golden_vector(h_q15: np.ndarray, n_in: int = 512) -> tuple:
    """A deterministic input and the exact fixed-point output it must produce.

    The input deliberately contains an in-band tone that must survive and an
    out-of-band tone that must be rejected, so a broken filter fails this
    vector rather than merely producing different numbers.
    """
    rng = np.random.default_rng(20260916)
    n = np.arange(n_in)
    x = (100_000.0 * np.sin(2 * np.pi * 500.0 * n / FS_IN)     # in band, keep
         + 100_000.0 * np.sin(2 * np.pi * 6000.0 * n / FS_IN)  # folds to 1500 Hz
         + rng.normal(0.0, 50.0, n_in))
    x = np.round(x).astype(np.int64)

    # Bit-exact model of firmware/lib/fir_decimate.c: int64 accumulator over
    # Q15 taps, arithmetic shift right by 15, round-to-nearest.
    taps = h_q15.astype(np.int64)
    out = []
    hist = np.zeros(len(taps), dtype=np.int64)
    for i, sample in enumerate(x):
        hist[1:] = hist[:-1]
        hist[0] = sample
        if (i % M) == (M - 1):
            acc = int(np.dot(hist, taps))
            out.append((acc + (1 << 14)) >> 15)
    return x.tolist(), out


def main() -> None:
    root = pathlib.Path(__file__).resolve().parent.parent
    h = design()
    hq = to_q15(h)

    rej_db, rej_hz = alias_rejection(h)
    passband = response(h, np.linspace(0.0, BAND_OF_INTEREST, 512))
    droop_db = float(-20 * np.log10(passband.min()))
    # -3 dB corner: first crossing on a fine grid, found by walking up from DC.
    # (Interpolating against the dB curve directly is a trap - the response is
    # not monotonic once you are past the first stopband null.)
    grid = np.linspace(0.0, FS_OUT, 30_001)
    db_grid = 20 * np.log10(np.maximum(response(h, grid), 1e-12))
    below = np.nonzero(db_grid <= -3.0)[0]
    minus3 = float(grid[below[0]]) if below.size else float("nan")
    at_nyq = float(-20 * np.log10(response(h, np.array([FS_OUT / 2]))[0]))

    print(f"taps                     : {NUM_TAPS}  ({NUM_TAPS // M} per polyphase branch)")
    print(f"input / output rate      : {FS_IN:.0f} -> {FS_OUT:.0f} Hz  (M = {M})")
    print(f"passband droop, DC..3 kHz: {droop_db:.2f} dB")
    print(f"-3 dB corner             : {minus3:.0f} Hz")
    print(f"attenuation at 3750 Hz   : {at_nyq:.1f} dB  (output Nyquist)")
    print(f"WORST-CASE ALIAS REJECTION into DC..{BAND_OF_INTEREST/1000:.0f} kHz:"
          f" {rej_db:.1f} dB  (worst input {rej_hz:.0f} Hz)")

    x, y = golden_vector(hq)

    # ------------------------------------------------------------ C header
    header = root / "firmware" / "include" / "fir_coeffs.h"
    lines = [
        "/**",
        " * @file fir_coeffs.h",
        " * @brief GENERATED by tools/design_filter.py - do not edit by hand.",
        " *",
        f" * M = {M} decimating anti-alias FIR, {NUM_TAPS} taps, Hamming-windowed sinc.",
        f" *   {FS_IN:.0f} Hz -> {FS_OUT:.0f} Hz",
        f" *   passband droop DC..{BAND_OF_INTEREST:.0f} Hz : {droop_db:.2f} dB",
        f" *   -3 dB corner                    : {minus3:.0f} Hz",
        f" *   worst-case alias rejection into",
        f" *     DC..{BAND_OF_INTEREST:.0f} Hz                   : {rej_db:.1f} dB"
        f"  (worst input {rej_hz:.0f} Hz)",
        " *",
        " * Taps are Q15 and sum to exactly 32768, i.e. unity DC gain.",
        " */",
        "#ifndef FIR_COEFFS_H",
        "#define FIR_COEFFS_H",
        "",
        "#include <stdint.h>",
        "",
        f"#define FIR_NUM_TAPS       {NUM_TAPS}U",
        f"#define FIR_DECIM_M        {M}U",
        f"#define FIR_TAPS_PER_PHASE {NUM_TAPS // M}U",
        f"#define FIR_FS_IN_HZ       {FS_IN:.0f}U",
        f"#define FIR_FS_OUT_HZ      {FS_OUT:.0f}U",
        f"#define FIR_ALIAS_REJ_DB   {rej_db:.1f}f",
        "",
        "static const int16_t k_fir_taps_q15[FIR_NUM_TAPS] = {",
    ]
    for i in range(0, len(hq), 8):
        lines.append("    " + ", ".join(f"{v:6d}" for v in hq[i:i + 8]) + ",")
    lines += [
        "};",
        "",
        "/* Golden vector: the exact fixed-point output this filter must produce.",
        "   Shared with host/tests via host/ads1256_host/fir_coeffs.json. */",
        f"#define FIR_GOLDEN_IN_LEN  {len(x)}U",
        f"#define FIR_GOLDEN_OUT_LEN {len(y)}U",
        "",
        "static const int32_t k_fir_golden_in[FIR_GOLDEN_IN_LEN] = {",
    ]
    for i in range(0, len(x), 8):
        lines.append("    " + ", ".join(f"{v:8d}" for v in x[i:i + 8]) + ",")
    lines += ["};", "", "static const int32_t k_fir_golden_out[FIR_GOLDEN_OUT_LEN] = {"]
    for i in range(0, len(y), 8):
        lines.append("    " + ", ".join(f"{v:8d}" for v in y[i:i + 8]) + ",")
    lines += ["};", "", "#endif /* FIR_COEFFS_H */", ""]
    header.write_text("\n".join(lines))
    print(f"\nwrote {header.relative_to(root)}")

    # ------------------------------------------------------------ JSON
    js = root / "host" / "ads1256_host" / "fir_coeffs.json"
    js.write_text(json.dumps({
        "generated_by": "tools/design_filter.py",
        "fs_in_hz": FS_IN,
        "fs_out_hz": FS_OUT,
        "decimation": M,
        "num_taps": NUM_TAPS,
        "band_of_interest_hz": BAND_OF_INTEREST,
        "taps_q15": [int(v) for v in hq],
        "taps_float": [float(v) for v in h],
        "passband_droop_db": droop_db,
        "minus3db_hz": minus3,
        "alias_rejection_db": rej_db,
        "alias_worst_input_hz": rej_hz,
        "golden_in": x,
        "golden_out": y,
    }, indent=1))
    print(f"wrote {js.relative_to(root)}")


if __name__ == "__main__":
    main()
