"""Host-side DSP tests, and the cross-language checks against the firmware.

``test_golden_vector_matches_the_firmware`` is the important one: the same input
and the same expected output are asserted here and in ``tests/test_fir_decimate.c``.
Two independent implementations - one fixed-point C, one Python - agreeing bit
for bit on a generated vector is what turns "the DSP on the device is correct"
from a claim into a check.
"""

import json
import pathlib

import numpy as np
import pytest

from ads1256_host import dsp
from ads1256_host.protocol import (
    MetricsRecord,
    Packet,
    PsdRecord,
    StreamDecoder,
    decode_metrics,
    decode_psd,
    encode_metrics,
    encode_packet,
    encode_psd,
)

COEFFS = json.loads(
    (pathlib.Path(dsp.__file__).with_name("fir_coeffs.json")).read_text()
)


# --------------------------------------------------------------------------- #
# The filter
# --------------------------------------------------------------------------- #
def test_golden_vector_matches_the_firmware():
    """The same vector tests/test_fir_decimate.c asserts, in Python."""
    out = dsp.decimate_fixed(COEFFS["golden_in"])
    assert out.tolist() == COEFFS["golden_out"]


def test_taps_are_symmetric_and_unity_dc_gain():
    taps = dsp.TAPS_Q15
    # Unity DC gain, exactly - not approximately. A 0.3 % gain error here is
    # indistinguishable from a failed offset calibration downstream.
    assert taps.sum() == 32768
    # Symmetry is what makes the filter linear phase. Without it, different
    # frequencies are delayed differently, which on a pulse-transit-time
    # measurement corrupts exactly the quantity being measured.
    assert np.array_equal(taps, taps[::-1])


def test_fixed_point_arithmetic_is_exact_to_half_an_lsb():
    """Isolates the integer path from the tap quantisation."""
    rng = np.random.default_rng(3)
    x = np.round(rng.normal(0, 300_000, 4096)).astype(np.int64)
    skip = dsp.NUM_TAPS

    fixed = dsp.decimate_fixed(x)[skip:]
    same_taps = np.convolve(x.astype(float), dsp.TAPS_Q15 / 32768.0)[: len(x)][
        dsp.DECIMATION - 1:: dsp.DECIMATION
    ][skip:]

    assert np.abs(fixed - same_taps).max() <= 0.5


def test_dc_passes_at_unity():
    x = np.full(4096, 1_000_000, dtype=np.int64)
    out = dsp.decimate_fixed(x)[dsp.NUM_TAPS:]
    assert np.all(np.abs(out - 1_000_000) <= 1)


def test_passband_tone_survives():
    n = np.arange(16384)
    amp = 1_000_000.0
    x = np.round(amp * np.sin(2 * np.pi * 1000.0 * n / dsp.FS_IN_HZ))
    out = dsp.decimate_fixed(x)[dsp.NUM_TAPS:]
    loss_db = -20 * np.log10(dsp.rms(out) / (amp / np.sqrt(2)))
    assert abs(loss_db) < 0.5


def test_alias_rejection_exceeds_40_db():
    """6 kHz folds to 1500 Hz after M=4 - squarely inside the band of interest."""
    n = np.arange(16384)
    amp = 1_000_000.0
    x = np.round(amp * np.sin(2 * np.pi * 6000.0 * n / dsp.FS_IN_HZ))

    filtered = dsp.decimate_fixed(x)[dsp.NUM_TAPS:]
    rejection = -20 * np.log10(dsp.rms(filtered) / (amp / np.sqrt(2)) + 1e-12)
    assert rejection > 40.0


def test_naive_downsampling_fails_the_same_case():
    """The control that gives the previous test meaning."""
    n = np.arange(16384)
    amp = 1_000_000.0
    x = np.round(amp * np.sin(2 * np.pi * 6000.0 * n / dsp.FS_IN_HZ))

    naive = dsp.decimate_naive(x)[dsp.NUM_TAPS:]
    rejection = -20 * np.log10(dsp.rms(naive) / (amp / np.sqrt(2)))
    assert rejection < 1.0          # passes essentially untouched


def test_frequency_response_meets_the_design_spec():
    """Every input that folds into DC..3 kHz must be >40 dB down."""
    f = np.linspace(0, dsp.FS_IN_HZ / 2, 20001)
    folded = np.abs(((f + dsp.FS_OUT_HZ / 2) % dsp.FS_OUT_HZ) - dsp.FS_OUT_HZ / 2)
    mask = (folded <= 3000.0) & (f > dsp.FS_OUT_HZ / 2)

    worst_db = -20 * np.log10(dsp.frequency_response(f[mask]).max())
    assert worst_db > 40.0
    # ...and it must agree with what the design script recorded.
    assert worst_db == pytest.approx(dsp.ALIAS_REJECTION_DB, abs=0.5)


# --------------------------------------------------------------------------- #
# Spectral estimation
# --------------------------------------------------------------------------- #
def test_welch_normaliser_satisfies_parseval():
    """Catches a wrong window normaliser, the classic 1.76 dB error."""
    rng = np.random.default_rng(5)
    x = rng.normal(0, 1000.0, 1 << 17)
    f, p = dsp.welch(x, dsp.FS_OUT_HZ, nperseg=1024)

    trapezoid = getattr(np, "trapezoid", None) or np.trapz
    integrated = np.sqrt(trapezoid(p, f))
    assert integrated == pytest.approx(dsp.rms(x), rel=0.05)


def test_longer_fft_does_not_reduce_variance():
    """The claim on the project summary, as an executable assertion."""
    rng = np.random.default_rng(6)
    x = rng.normal(0, 1000.0, 1 << 17)

    _, p_short = dsp.periodogram(x, dsp.FS_OUT_HZ, nperseg=1024)
    _, p_long = dsp.periodogram(x, dsp.FS_OUT_HZ, nperseg=16384)

    s_short = dsp.spectrum_scatter_db(p_short)
    s_long = dsp.spectrum_scatter_db(p_long)

    # Both land near the theoretical 5.57 dB for an exponentially distributed
    # periodogram bin, regardless of transform length.
    assert 4.5 < s_short < 6.5
    assert 4.5 < s_long < 6.5
    assert abs(s_short - s_long) < 1.0


def test_averaging_does_reduce_variance():
    rng = np.random.default_rng(7)
    x = rng.normal(0, 1000.0, 1 << 17)

    _, p_one = dsp.periodogram(x, dsp.FS_OUT_HZ, nperseg=1024)
    _, p_avg = dsp.welch(x, dsp.FS_OUT_HZ, nperseg=1024, segments=dsp.WELCH_SEGMENTS)

    s_one = dsp.spectrum_scatter_db(p_one)
    s_avg = dsp.spectrum_scatter_db(p_avg)
    assert s_avg < 0.6 * s_one


def test_periodic_hann_is_not_numpy_hanning():
    """A guard against someone 'simplifying' this to np.hanning()."""
    w = dsp.hann_periodic(1024)
    assert w[0] == pytest.approx(0.0)
    assert not np.allclose(w, np.hanning(1024))
    # sum(w^2) for a periodic Hann is exactly 3N/8 - the PSD normaliser.
    assert np.sum(w ** 2) == pytest.approx(3 * 1024 / 8)


def test_thdn_of_a_clean_tone_and_of_noise():
    fs = dsp.FS_OUT_HZ
    n = np.arange(1 << 14)

    clean = 1_000_000 * np.sin(2 * np.pi * 1000.0 * n / fs)
    f, p = dsp.welch(clean, fs, nperseg=1024, segments=dsp.WELCH_SEGMENTS)
    res = dsp.thd_n(p, f)
    assert res.valid
    assert res.fundamental_hz == pytest.approx(1000.0, abs=5.0)
    assert res.thdn_db < -55.0          # window-leakage limited, see metrics.h

    rng = np.random.default_rng(8)
    noise = rng.normal(0, 1000.0, 1 << 14)
    f, p = dsp.welch(noise, fs, nperseg=1024, segments=dsp.WELCH_SEGMENTS)
    # No tone: the device refuses to invent a number, and so does the reference.
    assert not dsp.thd_n(p, f).valid


def test_thdn_tracks_added_distortion():
    fs = dsp.FS_OUT_HZ
    n = np.arange(1 << 14)
    sig = (1_000_000 * np.sin(2 * np.pi * 1000.0 * n / fs)
           + 50_000 * np.sin(2 * np.pi * 3000.0 * n / fs))   # 5 % third harmonic
    f, p = dsp.welch(sig, fs, nperseg=1024, segments=dsp.WELCH_SEGMENTS)
    res = dsp.thd_n(p, f)
    assert res.valid
    assert -30.0 < res.thdn_db < -20.0


# --------------------------------------------------------------------------- #
# The new wire records
# --------------------------------------------------------------------------- #
def test_metrics_record_roundtrip():
    rec = MetricsRecord(
        seq=1234, t_us=0xDEADBEEF, mode=1,
        rms=(10, -20, 30, 40),
        thdn_db=(-62.345, None, -12.0, None),
        fundamental_hz=(1000.5, None, 250.25, None),
        psd_serial=77,
    )
    out = decode_metrics(encode_metrics(rec))
    assert out is not None
    assert out.seq == rec.seq and out.t_us == rec.t_us and out.mode == 1
    assert out.rms == rec.rms
    assert out.thdn_db[0] == pytest.approx(-62.345, abs=1e-3)
    assert out.thdn_db[1] is None          # INT32_MIN sentinel survives
    assert out.fundamental_hz[2] == pytest.approx(250.25, abs=1e-3)
    assert out.psd_serial == 77


def test_psd_record_roundtrip():
    rng = np.random.default_rng(9)
    bins = rng.uniform(-180, -120, 513)
    rec = PsdRecord(seq=5, t_us=1000, channel=2, fs_hz=7500.0, bins_dbfs=bins)

    wire = encode_psd(rec)
    assert len(wire) == 1044               # header + 513 int16 + CRC
    out = decode_psd(wire)
    assert out is not None
    assert out.channel == 2
    assert out.fs_hz == pytest.approx(7500.0)
    # 0.01 dB quantisation on the wire.
    assert np.allclose(out.bins_dbfs, bins, atol=0.01)


def test_corrupt_metrics_and_psd_records_are_rejected():
    rec = MetricsRecord(seq=1, t_us=2, mode=0, rms=(1, 2, 3, 4),
                        thdn_db=(None,) * 4, fundamental_hz=(None,) * 4,
                        psd_serial=0)
    wire = bytearray(encode_metrics(rec))
    wire[20] ^= 0x01
    assert decode_metrics(bytes(wire)) is None

    psd = PsdRecord(seq=1, t_us=2, channel=0, fs_hz=7500.0,
                    bins_dbfs=np.full(513, -150.0))
    wire = bytearray(encode_psd(psd))
    wire[500] ^= 0x80
    assert decode_psd(bytes(wire)) is None


def test_decoder_frames_a_mixed_record_stream():
    """Samples, metrics and 1044-byte PSD blocks interleaved, chunked arbitrarily."""
    rng = np.random.default_rng(10)
    stream = bytearray()
    expected_samples = 0

    for i in range(40):
        stream += encode_packet(Packet(seq=i, t_us=i * 532, flags=1,
                                       codes=(i, -i, 2 * i, -2 * i)))
        expected_samples += 1
        if i % 10 == 9:
            stream += encode_metrics(MetricsRecord(
                seq=i, t_us=i * 532, mode=0, rms=(14,) * 4,
                thdn_db=(None,) * 4, fundamental_hz=(None,) * 4, psd_serial=i))
        if i % 17 == 16:
            stream += encode_psd(PsdRecord(
                seq=i, t_us=i * 532, channel=i % 4, fs_hz=1875.0,
                bins_dbfs=rng.uniform(-170, -140, 513)))

    for chunk in (1, 7, 24, 64, 997, 4096):
        dec = StreamDecoder()
        samples = metrics = psds = 0
        for off in range(0, len(stream), chunk):
            for rec in dec.feed(bytes(stream[off: off + chunk])):
                if isinstance(rec, Packet):
                    samples += 1
                elif isinstance(rec, MetricsRecord):
                    metrics += 1
                else:
                    psds += 1

        assert samples == expected_samples, chunk
        assert metrics == 4, chunk
        assert psds == 2, chunk
        assert dec.dropped == 0, chunk
        assert dec.bad_crc == 0, chunk
        assert dec.resyncs == 0, chunk


def test_partial_psd_block_is_not_mistaken_for_corruption():
    """A 1044-byte record spans many serial reads; waiting is not a resync."""
    psd = PsdRecord(seq=1, t_us=0, channel=0, fs_hz=7500.0,
                    bins_dbfs=np.full(513, -150.0))
    wire = encode_psd(psd)

    dec = StreamDecoder()
    assert list(dec.feed(wire[:500])) == []      # incomplete - no output yet
    assert dec.resyncs == 0
    assert dec.bad_crc == 0

    out = list(dec.feed(wire[500:]))
    assert len(out) == 1 and isinstance(out[0], PsdRecord)
