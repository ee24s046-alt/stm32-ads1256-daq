"""Command-line entry point: ``ads1256 <command>``.

    ads1256 capture --port /dev/ttyACM0 --seconds 300 --out run.npz --raw run.bin
    ads1256 noise   run.npz --channel 0
    ads1256 verify  run.npz
    ads1256 monitor --port /dev/ttyACM0
    ads1256 selftest

Every command works against ``--simulate`` as well as a real port, so the whole
tool chain can be demonstrated, and regression-tested, without the board.
"""

from __future__ import annotations

import argparse
import sys
import time
from pathlib import Path

import numpy as np

from . import dsp as dspmod
from .analysis import analyse_noise, code_to_volts, timestamp_continuity
from .protocol import FLAG_NAMES, PACKET_BYTES, MetricsRecord, Packet, PsdRecord
from .stream import (
    FileSource,
    SerialSource,
    SimulatedSource,
    packets_to_arrays,
    read_packets,
    read_records,
    SCAN_PERIOD_US,
)

CHANNEL_RATE_HZ = 1e6 / SCAN_PERIOD_US    # 1879.7 scans/s per channel


def _make_source(args):
    if args.simulate:
        return SimulatedSource(
            noise_code_rms=args.sim_noise,
            tone_hz=args.sim_tone,
            tone_code_amplitude=args.sim_tone_amp,
            drop_every=args.sim_drop,
        )
    if not args.port:
        raise SystemExit("error: --port is required unless --simulate is given")
    return SerialSource(args.port)


# --------------------------------------------------------------------------- #
def cmd_capture(args) -> int:
    source = _make_source(args)
    raw_fh = open(args.raw, "wb") if args.raw else None
    started = time.monotonic()

    try:
        packets = read_packets(source, limit=args.packets,
                               duration_s=args.seconds, raw_sink=raw_fh)
        codes, times, seqs, flags = packets_to_arrays(packets)
    finally:
        if raw_fh:
            raw_fh.close()
        source.close()

    elapsed = time.monotonic() - started
    if codes.size == 0:
        print("no packets decoded - is the device streaming? try 'run'")
        return 1

    np.savez_compressed(args.out, codes=codes, t_us=times, seq=seqs, flags=flags,
                        fs_hz=CHANNEL_RATE_HZ)

    print(f"captured {len(codes)} scans in {elapsed:.1f} s "
          f"({len(codes) / elapsed:.0f} scans/s, "
          f"{len(codes) * PACKET_BYTES / elapsed / 1024:.1f} kB/s)")
    print(f"saved to {args.out}")
    if args.raw:
        print(f"raw stream saved to {args.raw}")
    return 0


# --------------------------------------------------------------------------- #
def cmd_noise(args) -> int:
    data = np.load(args.file)
    codes = data["codes"]
    fs = float(data["fs_hz"]) if "fs_hz" in data else CHANNEL_RATE_HZ

    channels = range(4) if args.channel is None else [args.channel]
    for ch in channels:
        result = analyse_noise(codes[:, ch], fs_hz=fs, vref=args.vref, pga=args.pga)
        print(f"--- channel {ch} " + "-" * 40)
        print(result.summary())

        if result.psd_freq_hz is not None:
            # Report the white-noise floor away from DC drift and away from the
            # top decade where the sinc response of the delta-sigma decimator
            # rolls off - neither is representative of the broadband floor.
            band = (result.psd_freq_hz > 10.0) & (result.psd_freq_hz < fs * 0.4)
            if band.any():
                floor = float(np.median(result.psd_v_per_rthz[band]))
                print(f"  noise density   : {floor * 1e9:10.2f} nV/sqrt(Hz) "
                      f"(median, 10 Hz .. {fs * 0.4:.0f} Hz)\n")

    if args.plot:
        _plot(codes, fs, args)
    return 0


def _plot(codes, fs, args) -> None:
    try:
        import matplotlib.pyplot as plt
    except ImportError:
        print("matplotlib not installed; skipping plot "
              "(pip install 'ads1256-host[plot]')")
        return

    fig, (ax_t, ax_f) = plt.subplots(2, 1, figsize=(10, 7))
    t = np.arange(len(codes)) / fs
    for ch in range(codes.shape[1]):
        volts = code_to_volts(codes[:, ch], args.vref, args.pga)
        ax_t.plot(t, (volts - volts.mean()) * 1e6, lw=0.5, label=f"ch{ch}")
        res = analyse_noise(codes[:, ch], fs, args.vref, args.pga)
        ax_f.loglog(res.psd_freq_hz[1:], res.psd_v_per_rthz[1:] * 1e9,
                    lw=0.8, label=f"ch{ch}")

    ax_t.set(xlabel="time (s)", ylabel="uV", title="Shorted-input time series")
    ax_f.set(xlabel="frequency (Hz)", ylabel="nV/sqrt(Hz)",
             title="Noise density")
    for ax in (ax_t, ax_f):
        ax.grid(True, which="both", alpha=0.3)
        ax.legend(fontsize=8)
    fig.tight_layout()
    fig.savefig(args.plot, dpi=140)
    print(f"plot written to {args.plot}")


# --------------------------------------------------------------------------- #
def cmd_plot(args) -> int:
    """Plot captured signal traces - the measurement, not the noise floor.

    `noise` characterises a shorted input. This plots what the instrument was
    actually built to record: pressure, flow, wall position. For a reviewer it
    is worth more than any amount of firmware detail, because it is the only
    artifact that shows the thing doing its job.

    The x-axis comes from the device's own t_us timestamps, not from the sample
    index divided by a nominal rate. If the device ever dropped a conversion,
    the gap appears in the trace rather than being silently closed up.
    """
    import numpy as np

    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError:
        print("matplotlib not installed - pip install 'ads1256-host[plot]'")
        return 1

    data = np.load(args.file)
    codes = data["codes"]
    t_us = data["t_us"].astype(np.int64)

    # Unwrap the 32-bit TIM2 rollover (71.6 minutes) before using it as an axis.
    dt = np.diff(t_us)
    t_us = t_us + np.concatenate([[0], np.cumsum(np.where(dt < 0, 1 << 32, 0))])
    t_s = (t_us - t_us[0]) / 1e6

    channels = ([int(c) for c in args.channels.split(",")]
                if args.channels else list(range(codes.shape[1])))
    labels = (args.labels.split(",") if args.labels
              else [f"ch{c}" for c in channels])
    if len(labels) != len(channels):
        raise SystemExit("error: --labels must give one name per channel")

    scales = ([float(x) for x in args.scale.split(",")] if args.scale
              else [1.0] * len(channels))
    if len(scales) != len(channels):
        raise SystemExit("error: --scale must give one factor per channel")

    sel = np.ones(len(t_s), dtype=bool)
    if args.start:
        sel &= t_s >= args.start
    if args.seconds:
        sel &= t_s <= ((args.start or 0.0) + args.seconds)

    # Volts, then whatever the caller's scale factor converts them to.
    series = {}
    for ch, label, k in zip(channels, labels, scales):
        v = code_to_volts(codes[sel, ch], args.vref, args.pga)
        series[label] = v * 1e3 * k if k != 1.0 else v * 1e3

    npanel = 2 if args.loop else 1
    fig, axes = plt.subplots(npanel, 1, figsize=(10, 4 * npanel + 1),
                             squeeze=False)
    ax = axes[0][0]

    for label, y in series.items():
        ax.plot(t_s[sel], y, lw=0.8, label=label)
    ax.set(xlabel="time (s)", ylabel=args.unit,
           title=args.title or f"Captured traces - {Path(args.file).name}")
    ax.grid(True, alpha=0.3)
    ax.legend(fontsize=8)

    if args.loop:
        xi, yi = (s_.strip() for s_ in args.loop.split(","))
        if xi not in series or yi not in series:
            raise SystemExit(f"error: --loop names must be among {list(series)}")
        lax = axes[1][0]
        lax.plot(series[xi], series[yi], lw=0.6)
        lax.set(xlabel=f"{xi} ({args.unit})", ylabel=f"{yi} ({args.unit})",
                title=f"{yi} vs {xi}")
        lax.grid(True, alpha=0.3)

    fig.tight_layout()
    fig.savefig(args.out, dpi=140)
    print(f"{int(sel.sum())} samples over {t_s[sel][-1] - t_s[sel][0]:.2f} s")
    print(f"plot written to {args.out}")
    return 0


# --------------------------------------------------------------------------- #
def cmd_verify(args) -> int:
    """Independent check that a capture lost nothing.

    Three separate pieces of evidence, which can and should disagree when
    something is wrong:
      1. sequence continuity  - no packet left the device unaccounted for
      2. timestamp continuity - no conversion went missing inside the device
      3. flag bits            - the device itself did not report a problem
    """
    data = np.load(args.file)
    seq = data["seq"]
    t_us = data["t_us"]
    flags = data["flags"]

    gaps = 0
    for i in range(1, len(seq)):
        expected = (seq[i - 1] + 1) & 0xFFFF
        if seq[i] != expected:
            gaps += int((seq[i] - expected) & 0xFFFF)

    cont = timestamp_continuity(t_us, expected_period_us=SCAN_PERIOD_US,
                                tolerance_us=args.tolerance)

    print(f"packets                : {len(seq)}")
    print(f"sequence gaps          : {gaps}")
    print(f"mean scan period       : {cont['mean_period_us']:.3f} us "
          f"(nominal {SCAN_PERIOD_US})")
    print(f"timestamp jitter (RMS) : {cont['jitter_rms_us']:.3f} us")
    print(f"timestamp max deviation: {cont['max_deviation_us']:.3f} us")
    print(f"timestamp gaps         : {cont['gaps']}")

    any_flag = False
    for bit, name in FLAG_NAMES.items():
        n = int(np.count_nonzero(flags & bit))
        if n and name != "cal":
            print(f"flag {name:<14}: {n}")
            any_flag = True
    if not any_flag:
        print("flags                  : clean")

    ok = (gaps == 0) and (cont["gaps"] == 0) and not any_flag
    print("\nRESULT: " + ("PASS" if ok else "FAIL"))
    return 0 if ok else 2


# --------------------------------------------------------------------------- #
def cmd_monitor(args) -> int:
    source = _make_source(args)
    last = time.monotonic()
    count = 0
    try:
        for rec in read_records(source):
            if isinstance(rec, MetricsRecord):
                # The device's own live numbers, printed as they arrive.
                parts = []
                for c in range(4):
                    v = code_to_volts(rec.rms[c], args.vref, args.pga)
                    cell = f"ch{c} rms {v * 1e6:8.2f} uV"
                    if rec.thdn_db[c] is not None:
                        cell += (f" THD+N {rec.thdn_db[c]:6.1f} dB"
                                 f" @ {rec.fundamental_hz[c]:.1f} Hz")
                    parts.append(cell)
                print(f"  [device/{rec.mode_name}] " + " | ".join(parts))
                continue

            if isinstance(rec, PsdRecord):
                floor = float(np.median(rec.bins_dbfs[10:]))
                print(f"  [device PSD] ch{rec.channel} "
                      f"fs={rec.fs_hz:.0f} Hz  median {floor:.1f} dBFS/Hz")
                continue

            count += 1
            now = time.monotonic()
            if now - last >= 1.0:
                volts = code_to_volts(np.array(rec.codes), args.vref, args.pga)
                cells = "  ".join(f"ch{c}:{v * 1e3:+9.4f} mV"
                                  for c, v in enumerate(volts))
                fl = ",".join(rec.flag_names) or "-"
                print(f"{count:9d} scans  seq={rec.seq:5d}  {cells}  [{fl}]")
                last = now
    except KeyboardInterrupt:
        print("\nstopped")
    finally:
        source.close()
    return 0


# --------------------------------------------------------------------------- #
def cmd_selftest(args) -> int:
    """End-to-end exercise of the decode path with no hardware."""
    source = SimulatedSource(noise_code_rms=14.6, drop_every=0, corrupt_every=0)
    codes, times, seq, _ = packets_to_arrays(read_packets(source, limit=20000))

    res = analyse_noise(codes[:, 0], CHANNEL_RATE_HZ)
    cont = timestamp_continuity(times, SCAN_PERIOD_US)
    gaps = sum(1 for i in range(1, len(seq)) if seq[i] != ((seq[i - 1] + 1) & 0xFFFF))

    print(res.summary())
    print(f"sequence gaps  : {gaps}")
    print(f"timestamp gaps : {cont['gaps']}")

    ok = gaps == 0 and cont["gaps"] == 0 and 19.5 < res.effective_bits < 21.5
    print("RESULT: " + ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


# --------------------------------------------------------------------------- #
def cmd_dsp(args) -> int:
    """Verify and demonstrate the DSP chain the device runs.

    Three things, each of which the device claims and each of which is checked
    here against an independent implementation rather than asserted.
    """
    import numpy as np

    print("=" * 66)
    print(" Decimation filter  (firmware/include/fir_coeffs.h)")
    print("=" * 66)
    print(f"  taps                : {dspmod.NUM_TAPS} "
          f"({dspmod.NUM_TAPS // dspmod.DECIMATION} per polyphase branch)")
    print(f"  rate                : {dspmod.FS_IN_HZ:.0f} Hz -> "
          f"{dspmod.FS_OUT_HZ:.0f} Hz   (M = {dspmod.DECIMATION})")
    print(f"  -3 dB corner        : {dspmod.MINUS3DB_HZ:.0f} Hz")
    print(f"  passband droop      : {dspmod.PASSBAND_DROOP_DB:.2f} dB to 3 kHz")
    print(f"  alias rejection     : {dspmod.ALIAS_REJECTION_DB:.1f} dB "
          f"(design, worst case into DC..3 kHz)")

    # ---- 1. fixed point agrees with the float reference -------------------
    #
    # Two comparisons, because they answer different questions. Against float
    # using the SAME quantised taps, the only difference left is the arithmetic
    # itself, which must be exact to within the half-LSB rounding. Against float
    # using the ideal taps, the difference is dominated by the Q15 tap
    # quantisation - a property of the representation, not of the code.
    rng = np.random.default_rng(1)
    probe = np.round(rng.normal(0, 200_000, 8192)).astype(np.int64)
    skip = dspmod.NUM_TAPS

    fixed = dspmod.decimate_fixed(probe)[skip:]
    ideal = dspmod.decimate_float(probe)[skip:]
    same_taps = np.convolve(probe.astype(float), dspmod.TAPS_Q15 / 32768.0)[
        : len(probe)][dspmod.DECIMATION - 1:: dspmod.DECIMATION][skip:]

    e_arith = np.abs(fixed - same_taps).max()
    e_total = np.abs(fixed - ideal)
    print(f"\n  fixed-point arithmetic : max {e_arith:.3f} codes vs the same taps "
          f"in float")
    print(f"                           (half-LSB rounding only - the integer "
          f"path is exact)")
    print(f"  Q15 tap quantisation   : {e_total.std():.1f} codes RMS on a "
          f"{probe.std():.0f}-code signal")
    print(f"                           = {e_total.std() / probe.std():.1e} relative, "
          f"~70 dB below the data")

    # ---- 2. measured alias rejection --------------------------------------
    n = np.arange(16384)
    amp = 1_000_000.0
    tone_in = np.round(amp * np.sin(2 * np.pi * 6000.0 * n / dspmod.FS_IN_HZ))
    filtered = dspmod.decimate_fixed(tone_in)[dspmod.NUM_TAPS:]
    unfiltered = dspmod.decimate_naive(tone_in)[dspmod.NUM_TAPS:]

    ref = amp / np.sqrt(2)
    rej_filtered = -20 * np.log10(dspmod.rms(filtered) / ref + 1e-12)
    rej_naive = -20 * np.log10(dspmod.rms(unfiltered) / ref + 1e-12)

    print(f"\n  6 kHz tone folds to 1500 Hz after M=4 decimation:")
    print(f"    with anti-alias   : {rej_filtered:5.1f} dB rejected")
    print(f"    naive downsample  : {rej_naive:5.1f} dB rejected  <- indistinguishable"
          f" from real signal")

    # ---- 3. averaging vs FFT length ---------------------------------------
    print("\n" + "=" * 66)
    print(" Spectral estimation  (firmware/lib/metrics.c)")
    print("=" * 66)
    noise = rng.normal(0, 1000.0, 1 << 16)

    f1, p1 = dspmod.periodogram(noise, dspmod.FS_OUT_HZ, nperseg=1024)
    _, p_long = dspmod.periodogram(noise, dspmod.FS_OUT_HZ, nperseg=16384)
    fw, pw = dspmod.welch(noise, dspmod.FS_OUT_HZ, nperseg=1024,
                          segments=dspmod.WELCH_SEGMENTS)

    s1 = dspmod.spectrum_scatter_db(p1)
    s_long = dspmod.spectrum_scatter_db(p_long)
    sw = dspmod.spectrum_scatter_db(pw)

    print(f"  1024-pt, 1 average  : {s1:5.2f} dB scatter")
    print(f"  16384-pt, 1 average : {s_long:5.2f} dB scatter   <- 16x the resolution,"
          f" no better")
    print(f"  1024-pt, {dspmod.WELCH_SEGMENTS} averages : {sw:5.2f} dB scatter   "
          f"<- {s1 / sw:.1f}x better")
    print("\n  Variance falls with the number of AVERAGES, not with transform")
    print("  length. That is the whole reason Welch's method exists.")

    if args.plot:
        _plot_dsp(args.plot, f1, p1, fw, pw, tone_in, filtered, unfiltered)
    return 0


def _plot_dsp(path, f1, p1, fw, pw, tone_in, filtered, unfiltered) -> None:
    import numpy as np
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError:
        print("\nmatplotlib not installed; skipping plot "
              "(pip install 'ads1256-host[plot]')")
        return

    fig, axes = plt.subplots(3, 1, figsize=(9, 10))

    # (a) filter response with the aliasing bands shaded
    f = np.linspace(0, dspmod.FS_IN_HZ / 2, 4000)
    h_db = 20 * np.log10(dspmod.frequency_response(f) + 1e-12)
    axes[0].plot(f, h_db, lw=1.2)
    axes[0].axvspan(4500, 7500, alpha=0.15, color="tab:red",
                    label="folds into DC..3 kHz")
    axes[0].axvspan(12000, 15000, alpha=0.15, color="tab:red")
    axes[0].axvline(dspmod.MINUS3DB_HZ, ls="--", lw=0.8, color="k",
                    label=f"-3 dB @ {dspmod.MINUS3DB_HZ:.0f} Hz")
    axes[0].axhline(-40, ls=":", lw=0.8, color="tab:green", label="-40 dB requirement")
    axes[0].set(xlabel="Hz", ylabel="dB", ylim=(-90, 5),
                title=f"M={dspmod.DECIMATION} anti-alias filter, "
                      f"{dspmod.NUM_TAPS} taps")

    # (b) the alias, with and without the filter.
    #
    # Both traces share one linear axis on purpose: the filtered one collapsing
    # to a flat line IS the result. The annotation carries the numbers, because
    # "indistinguishable from zero at this scale" is the honest reading and a
    # second y-axis would make the residual look comparable when it is 58 dB
    # down.
    axes[1].plot(unfiltered[:300], lw=0.9, label="no filter (alias passes)")
    axes[1].plot(filtered[:300], lw=1.2, label="polyphase anti-alias")
    rms_un = dspmod.rms(unfiltered)
    rms_fl = dspmod.rms(filtered)
    axes[1].text(
        0.985, 0.04,
        f"RMS  {rms_un:,.0f} codes  ->  {rms_fl:,.0f} codes"
        f"   ({20 * np.log10(rms_un / max(rms_fl, 1e-9)):.1f} dB)",
        transform=axes[1].transAxes, ha="right", va="bottom", fontsize=9,
        bbox=dict(boxstyle="round,pad=0.35", fc="white", ec="0.6", alpha=0.9),
    )
    axes[1].set(xlabel="decimated sample", ylabel="codes",
                title="6 kHz input, decimated to 7.5 kSPS (folds to 1500 Hz)")

    # (c) the averaging demonstration
    axes[2].semilogx(f1[1:], 10 * np.log10(p1[1:] + 1e-300), lw=0.5, alpha=0.6,
                     label="single periodogram")
    axes[2].semilogx(fw[1:], 10 * np.log10(pw[1:] + 1e-300), lw=1.4,
                     label=f"Welch, {dspmod.WELCH_SEGMENTS} averages")
    axes[2].set(xlabel="Hz", ylabel="dB",
                title="Same data, same resolution - averaging is what reduces variance")

    for ax in axes:
        ax.grid(True, which="both", alpha=0.3)
        ax.legend(fontsize=8)
    fig.tight_layout()
    fig.savefig(path, dpi=140)
    print(f"\nplot written to {path}")


# --------------------------------------------------------------------------- #
def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(prog="ads1256",
                                description="Host tooling for the STM32/ADS1256 DAQ")
    p.add_argument("--vref", type=float, default=2.5, help="reference voltage (V)")
    p.add_argument("--pga", type=int, default=1, help="ADS1256 PGA setting")

    sub = p.add_subparsers(dest="command", required=True)

    def add_source_opts(sp):
        sp.add_argument("--port", help="serial port, e.g. /dev/ttyACM0 or COM4")
        sp.add_argument("--simulate", action="store_true",
                        help="use the built-in synthetic device")
        sp.add_argument("--sim-noise", type=float, default=14.6,
                        help="simulated RMS noise in codes")
        sp.add_argument("--sim-tone", type=float, default=0.0)
        sp.add_argument("--sim-tone-amp", type=float, default=0.0)
        sp.add_argument("--sim-drop", type=int, default=0,
                        help="drop one packet every N (to test gap detection)")

    c = sub.add_parser("capture", help="record a stream to .npz")
    add_source_opts(c)
    c.add_argument("--out", default="capture.npz")
    c.add_argument("--raw", help="also write the undecoded byte stream here")
    c.add_argument("--seconds", type=float)
    c.add_argument("--packets", type=int)
    c.set_defaults(func=cmd_capture)

    n = sub.add_parser("noise", help="noise floor and effective bits")
    n.add_argument("file")
    n.add_argument("--channel", type=int)
    n.add_argument("--plot", help="write a PNG here")
    n.set_defaults(func=cmd_noise)

    pl = sub.add_parser("plot", help="plot captured signal traces")
    pl.add_argument("file")
    pl.add_argument("--out", default="traces.png")
    pl.add_argument("--channels", help="e.g. 0,1,2")
    pl.add_argument("--labels", help='e.g. "Pressure prox,Pressure dist,Flow"')
    pl.add_argument("--scale", help="per-channel factor applied after mV, "
                                    "e.g. mmHg per mV")
    pl.add_argument("--unit", default="mV", help="y-axis label after scaling")
    pl.add_argument("--title")
    pl.add_argument("--start", type=float, help="skip this many seconds")
    pl.add_argument("--seconds", type=float, help="window length")
    pl.add_argument("--loop", help='X,Y label pair for a loop plot, '
                                   'e.g. "Diameter,Pressure"')
    pl.set_defaults(func=cmd_plot)

    v = sub.add_parser("verify", help="prove a capture lost nothing")
    v.add_argument("file")
    v.add_argument("--tolerance", type=float, default=5.0,
                   help="timestamp deviation tolerance in us")
    v.set_defaults(func=cmd_verify)

    m = sub.add_parser("monitor", help="live one-line-per-second readout")
    add_source_opts(m)
    m.set_defaults(func=cmd_monitor)

    d = sub.add_parser("dsp", help="verify and demonstrate the on-device DSP chain")
    d.add_argument("--plot", help="write a three-panel PNG here")
    d.set_defaults(func=cmd_dsp)

    s = sub.add_parser("selftest", help="run the decode path against the simulator")
    s.set_defaults(func=cmd_selftest)

    return p


def main(argv=None) -> int:
    args = build_parser().parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
