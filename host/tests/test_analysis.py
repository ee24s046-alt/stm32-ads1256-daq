"""Analysis tests: the maths has to be right or the headline numbers are fiction."""

import numpy as np
import pytest

from ads1256_host.analysis import (
    analyse_noise,
    code_to_volts,
    timestamp_continuity,
)
from ads1256_host.stream import SCAN_PERIOD_US, SimulatedSource, packets_to_arrays, read_packets

FS = 1e6 / SCAN_PERIOD_US


def test_code_to_volts_endpoints():
    # +/- full scale is +/- 2 * VREF / PGA.
    assert code_to_volts(8388607) == pytest.approx(5.0, rel=1e-9)
    assert code_to_volts(-8388607) == pytest.approx(-5.0, rel=1e-9)
    assert code_to_volts(0) == 0.0
    assert code_to_volts(8388607, vref=2.5, pga=64) == pytest.approx(5.0 / 64)


def test_rms_recovers_a_known_noise_level():
    rng = np.random.default_rng(1)
    codes = rng.normal(0.0, 100.0, 200_000)
    res = analyse_noise(codes, FS, psd=False)

    expected_v = code_to_volts(100.0)
    assert res.rms_v == pytest.approx(expected_v, rel=0.02)


def test_effective_and_noise_free_bits_differ_by_the_crest_factor():
    rng = np.random.default_rng(2)
    res = analyse_noise(rng.normal(0, 50.0, 100_000), FS, psd=False)
    assert res.effective_bits - res.noise_free_bits == pytest.approx(
        np.log2(6.6), abs=1e-9)


def test_mean_is_reported_but_excluded_from_rms():
    """A DC offset is a calibration fact, not noise; it must not inflate RMS."""
    rng = np.random.default_rng(3)
    noise = rng.normal(0.0, 20.0, 50_000)
    res_zero = analyse_noise(noise, FS, psd=False)
    res_offset = analyse_noise(noise + 5000.0, FS, psd=False)

    assert res_offset.rms_v == pytest.approx(res_zero.rms_v, rel=1e-9)
    assert res_offset.mean_v == pytest.approx(code_to_volts(5000.0), rel=1e-3)


def test_psd_integrates_back_to_the_time_domain_rms():
    """Parseval check - this is what catches a wrong window normalisation."""
    rng = np.random.default_rng(4)
    codes = rng.normal(0.0, 75.0, 262_144)
    res = analyse_noise(codes, FS, nperseg=8192)

    # np.trapz was removed in NumPy 2.0 in favour of np.trapezoid.
    trapezoid = getattr(np, "trapezoid", None) or np.trapz
    integrated = np.sqrt(trapezoid(res.psd_v_per_rthz ** 2, res.psd_freq_hz))
    assert integrated == pytest.approx(res.rms_v, rel=0.05)


def test_psd_finds_an_injected_tone():
    t = np.arange(65536) / FS
    codes = 10_000 * np.sin(2 * np.pi * 250.0 * t)
    res = analyse_noise(codes, FS, nperseg=16384)
    peak_hz = res.psd_freq_hz[np.argmax(res.psd_v_per_rthz)]
    assert peak_hz == pytest.approx(250.0, abs=FS / 16384)


def test_timestamp_continuity_flags_a_missing_scan():
    good = np.arange(0, 1000) * SCAN_PERIOD_US
    assert timestamp_continuity(good, SCAN_PERIOD_US)["gaps"] == 0

    gapped = np.delete(good, 500)
    assert timestamp_continuity(gapped, SCAN_PERIOD_US)["gaps"] == 1


def test_timestamp_continuity_survives_the_tim2_wrap():
    base = (1 << 32) - 5 * SCAN_PERIOD_US
    t = (base + np.arange(20) * SCAN_PERIOD_US) % (1 << 32)
    assert timestamp_continuity(t, SCAN_PERIOD_US)["gaps"] == 0


def test_end_to_end_against_the_simulator():
    src = SimulatedSource(noise_code_rms=14.6, seed=7)
    codes, t_us, seq, _ = packets_to_arrays(read_packets(src, limit=30_000))

    assert codes.shape == (30_000, 4)
    assert timestamp_continuity(t_us, SCAN_PERIOD_US)["gaps"] == 0

    res = analyse_noise(codes[:, 0], FS, psd=False)
    assert res.rms_v == pytest.approx(code_to_volts(14.6), rel=0.05)
    # 14.6 codes RMS on a 10 V span is ~20.1 effective bits.
    assert 19.8 < res.effective_bits < 20.4


def test_simulator_dropped_packets_are_detected_end_to_end():
    from ads1256_host.protocol import StreamDecoder

    src = SimulatedSource(drop_every=100, seed=9)
    dec = StreamDecoder()
    seen = 0
    while seen < 5000:
        for _ in dec.feed(src.read(4096)):
            seen += 1
    assert dec.dropped > 40
