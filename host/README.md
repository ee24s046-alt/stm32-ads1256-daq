# `ads1256-host`

Host-side decoder, capture tool and noise analysis for the STM32F407 + ADS1256
data-acquisition system.

```bash
pip install -e .[dev]
```

## Commands

```bash
ads1256 capture --port /dev/ttyACM0 --seconds 300 --out run.npz --raw run.bin
ads1256 verify  run.npz
ads1256 noise   run.npz --channel 0 --plot noise.png
ads1256 plot    run.npz --labels "Pressure prox,Pressure dist,Flow,Diameter" \
                        --loop "Diameter,Pressure prox" --out traces.png
ads1256 monitor --port /dev/ttyACM0
ads1256 dsp --plot dsp.png
ads1256 selftest
```

Every command that takes `--port` also accepts `--simulate`, which substitutes a
synthetic device. That is not a toy: the simulator can inject sequence gaps and
bit corruption on demand, so the decoder's resync and loss-detection paths are
exercised by the same code that handles real data rather than by a test-only
branch. It is also what lets CI cover the decoder end to end.

## What the pieces do

| Module | Responsibility |
|---|---|
| `protocol.py` | frame encode/decode and `StreamDecoder`, the resumable framer |
| `stream.py` | byte sources — serial port, raw file replay, simulator |
| `analysis.py` | RMS, effective/noise-free bits, Welch PSD, timestamp continuity |
| `dsp.py` | reference polyphase decimator, Welch, THD+N - checked against the firmware |
| `cli.py` | the `ads1256` entry point |

`protocol.py` is a deliberate re-implementation of
[`firmware/lib/packet.c`](../firmware/lib/packet.c), not a binding to it. Both
are asserted against the same golden byte vector, which is the only arrangement
that catches an encoder and a decoder drifting apart.

## Notes on the analysis

Two different numbers get called "resolution" and conflating them is how
datasheet comparisons go wrong:

- `effective_bits = log2(FSR / rms)` — RMS-referred, what TI's noise tables
  report, so this is the one to compare against the datasheet.
- `noise_free_bits = log2(FSR / (6.6 · rms))` — peak-to-peak referred, using the
  conventional 6.6 σ crest factor. Always ~2.7 bits lower, and it is the number
  of bits that are actually stable sample to sample.

The PSD is a Welch estimate with a Hann window and 50 % overlap, implemented
directly rather than via SciPy — it is fifteen lines and the window
normalisation is the part that has to be right.
`test_psd_integrates_back_to_the_time_domain_rms` is a Parseval check, which is
what catches a wrong normaliser.

Captures should be saved with `--raw` as well as `--out`. If the decoder later
turns out to be wrong, a raw byte capture can be re-analysed; decoded samples
have already had the mistake baked in.

## The DSP reference

`dsp.py` reimplements, in Python, everything `firmware/lib/` computes on the
device: the polyphase decimator (bit-exact, including the Q15 rounding), Welch's
method with the periodic Hann window and the `sum(w**2)` normaliser, and THD+N.

It exists to be checked against, not to replace the firmware.
`tools/design_filter.py` emits the taps, the C header, this package's
`fir_coeffs.json` and a golden input/output vector; `tests/test_fir_decimate.c`
and `tests/test_dsp.py` assert the same expected output. Two independent
implementations agreeing bit for bit is what makes the DSP claims checkable.

`ads1256 dsp` prints the comparison and `--plot` draws it.

## Tests

```bash
python -m pytest -q      # 43 tests
```
