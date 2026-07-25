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

from .analysis import analyse_noise, code_to_volts, timestamp_continuity
from .protocol import FLAG_NAMES, PACKET_BYTES
from .stream import (
    FileSource,
    SerialSource,
    SimulatedSource,
    packets_to_arrays,
    read_packets,
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
        for pkt in read_packets(source):
            count += 1
            now = time.monotonic()
            if now - last >= 1.0:
                volts = code_to_volts(np.array(pkt.codes), args.vref, args.pga)
                cells = "  ".join(f"ch{c}:{v * 1e3:+9.4f} mV"
                                  for c, v in enumerate(volts))
                fl = ",".join(pkt.flag_names) or "-"
                print(f"{count:9d} scans  seq={pkt.seq:5d}  {cells}  [{fl}]")
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

    v = sub.add_parser("verify", help="prove a capture lost nothing")
    v.add_argument("file")
    v.add_argument("--tolerance", type=float, default=5.0,
                   help="timestamp deviation tolerance in us")
    v.set_defaults(func=cmd_verify)

    m = sub.add_parser("monitor", help="live one-line-per-second readout")
    add_source_opts(m)
    m.set_defaults(func=cmd_monitor)

    s = sub.add_parser("selftest", help="run the decode path against the simulator")
    s.set_defaults(func=cmd_selftest)

    return p


def main(argv=None) -> int:
    args = build_parser().parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
