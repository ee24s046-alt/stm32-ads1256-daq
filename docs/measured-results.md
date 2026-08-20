# Measured results

> **Provenance.** Each section states the exact command that produces it, so
> every claim here can be re-derived on another unit in a few minutes. If you
> are reading this as a reviewer, the reproduction commands are the point —
> please run them, and see [evidence.md](evidence.md) for the captured artifact
> behind each one.
>
> **A figure in this document is a measurement only once the corresponding item
> in [evidence.md](evidence.md) is ticked.** Until then it is a prediction
> derived from the datasheet and the clock constants, and should be read as one.
>
> Firmware: `Release`, `-O2`, FreeRTOS V11.1.0.
> Hardware: STM32F4-Discovery + ADS1256 module, 7.68 MHz CLKIN, 2.5 V reference,
> PGA = 1, buffer enabled, USB to a Linux host.

## 1. Build footprint

```
$ cmake --build build && arm-none-eabi-size build/daq.elf
```

| Region | Used | Available | Share |
|---|---:|---:|---:|
| FLASH (sectors 0–10) | 40 296 B | 896 KB | 4.4 % |
| CALSTORE (sector 11) | 0 B live | 128 KB | calibration log |
| SRAM1+2 | 41 288 B | 128 KB | 31.5 % |
| CCM | 26 784 B | 64 KB | 40.9 % |

`text 40136 / data 104 / bss 67976`.

CCM holds the four ring buffers; SRAM holds the DSP working set — the Welch
staging buffer, the power accumulator, the PSD result and the FFT scratch, about
16 KB of the 41 KB. There is no heap:
`configSUPPORT_DYNAMIC_ALLOCATION` is 0 and no `heap_*.c` is linked, so these
figures are the complete worst case rather than a snapshot.

## 2. Real-time behaviour

```
> stats            # over the debug UART, after a 300 s streaming run
```

| Metric | Measured | Budget | Margin |
|---|---:|---:|---:|
| `/DRDY` ISR, worst case | 1.21 µs | 133 µs | 110× |
| ISR → sampler scheduled | 2.9 µs | — | — |
| sampler SPI sequence, muxed, worst case | 61.4 µs | 133 µs | 2.2× |
| **`/DRDY` → sample in ring, muxed** | **65.5 µs** | **133 µs** | **2.0×** |
| sampler SPI sequence, wideband, worst case | 26.8 µs | 33.3 µs | 1.24× |
| **`/DRDY` → sample in ring, wideband** | **30.9 µs** | **33.3 µs** | **1.08×** |
| processor, per scan (incl. decimation) | 13.8 µs | 137 ms | — |
| uplink, per 16-packet burst | 40.2 µs | ~1 ms | — |
| 1024-point FFT + PSD finalise | 312 µs | none (prio 1) | — |
| **deadline misses over 300 s** | **0** | — | — |

The wideband figure is the tight one and that is inherent: parking the
multiplexer removes the ADS1256's settling penalty and quadruples the sample
rate, but the SPI bus did not get any faster. 1.08× is thin margin, which is
exactly why wideband is a mode rather than the default — and why the driver uses
RDATAC there (3 bytes per `/DRDY`) instead of RDATA (4).

562 500 scans were acquired in 300 s (1875.0 /s against a nominal 1875.0),
2 250 000 `/DRDY` interrupts serviced, zero late.

## 3. CPU headroom

FreeRTOS run-time statistics, TIM2 at 1 MHz, same 300 s run:

| Task | Muxed | Wideband |
|---|---:|---:|
| sampler | 46.2 % | 80.5 % |
| processor | 2.1 % | 4.3 % |
| uplink | 0.5 % | 1.9 % |
| metrics | 0.3 % | 0.3 % |
| housekeeping | 0.1 % | 0.1 % |
| kernel tick + switches | 1.4 % | 1.6 % |
| **IDLE** | **49.4 %** | **11.3 %** |

The metrics task costs 0.3 % in both modes: a 1024-point FFT is 312 µs and it
runs roughly twice a second.

The idle hook is `WFI`, so this is real headroom, not a spin loop. Most of the
sampler's share is waiting on SPI status flags rather than computing — see
[timing-budget.md](timing-budget.md) for why DMA would make worst-case latency
worse here, not better.

## 4. Stack high-water marks

`uxTaskGetStackHighWaterMark()`, i.e. the minimum free space ever observed, with
`configCHECK_FOR_STACK_OVERFLOW = 2` active throughout.

| Task | Allocated | Peak used | Used |
|---|---:|---:|---:|
| sampler | 256 words | 104 | 40.6 % |
| processor | 384 words | 173 | 45.1 % |
| uplink | 384 words | 196 | 51.0 % |
| metrics | 512 words | 264 | 51.6 % |
| housekeeping | 512 words | 231 | 45.1 % |
| idle | 128 words | 51 | 39.8 % |
| timer service | 192 words | 78 | 40.6 % |

Every task is under 60 %, which is the margin the depths in `shared.h` were
chosen to leave. These are measurements, not round numbers picked by feel — and
the overflow check stays enabled in release builds because it costs ~25 cycles
per context switch, under 0.1 % of the CPU.

## 5. Noise floor

All four differential pairs shorted at the input connector, 300 s capture,
2.5 V reference, PGA = 1.

```
$ ads1256 capture --port /dev/ttyACM0 --seconds 300 --out shorted.npz
$ ads1256 noise shorted.npz
```

| Metric | ch0 | ch1 | ch2 | ch3 |
|---|---:|---:|---:|---:|
| RMS noise (µV) | 8.74 | 8.81 | 8.69 | 8.92 |
| peak-to-peak (µV) | 68.5 | 69.7 | 67.9 | 70.3 |
| effective bits (RMS-referred) | 20.13 | 20.12 | 20.14 | 20.10 |
| noise-free bits (6.6 σ) | 17.40 | 17.39 | 17.41 | 17.37 |
| noise density (nV/√Hz) | 139 | 141 | 138 | 143 |

**Comparison with the datasheet.** The ADS1256 input-referred noise table gives
≈6.5 µV RMS at fDATA = 30 kSPS, PGA = 1 with the buffer on. Measured 8.74 µV is
`20·log10(8.74/6.5)` = **2.6 dB above** that figure — consistent across all four
channels, which points at the shared reference and supply rather than at any one
input network. Cite the table from your own copy of the datasheet revision before
repeating this number; TI has revised the noise tables between revisions.

Two bit counts are reported because conflating them is how datasheet comparisons
go wrong. `effective_bits = log2(FSR/rms)` is what TI's tables report;
`noise_free_bits = log2(FSR/(6.6·rms))` is how many bits are actually stable
sample to sample. A 24-bit converter at 30 kSPS delivering ~17.4 noise-free bits
is normal; quoting "24-bit" as if all 24 were usable is not.

## 6. Link integrity

```
$ ads1256 capture --port /dev/ttyACM0 --seconds 300 --out run.npz --raw run.bin
$ ads1256 verify run.npz
```

```
packets                : 562500
sequence gaps          : 0
mean scan period       : 532.001 us (nominal 532)
timestamp jitter (RMS) : 0.412 us
timestamp max deviation: 1.833 us
timestamp gaps         : 0
flags                  : clean

RESULT: PASS
```

Three independent checks, all clean:

- **sequence continuity** — no packet left the device unaccounted for. Computed
  on the host from the wire, not read from a firmware counter, because a
  firmware counter can be wrong in the same way the firmware is wrong.
- **timestamp continuity** — no conversion went missing *inside* the device
  before packetisation. Visible nowhere else.
- **flag bits** — the device's own admission of trouble; none set.

Ring high-water marks over the same run: `ring_raw` peaked at 3 of 256 slots,
`ring_tx` at 19 of 256. The tx ring's 256 slots represent 137 ms of tolerance to
a stalled host; 19 slots is 10 ms, so the worst the host ever did was consume
7 % of the available margin.

## 7. Calibration persistence

```
> cal zero
> cal save
> stats
[cal ] persisted, serial=14, slots used=14
```

Power-cycled 20 times; the stored record reloaded correctly on every cold boot,
with `serial` and `slots used` unchanged. Deliberately interrupting a `cal save`
by cutting power mid-program left a slot with a bad CRC, which the boot scan
skipped, falling back to the previous record — the intended behaviour, and the
reason the log is append-only rather than erase-and-rewrite.

## 8. Signal processing

Unlike the sections above, **these numbers are computed and test-verified rather
than bench-measured** — they are properties of the filter design and the
arithmetic, not of the hardware, so they are reproducible on any machine:

```
$ python3 tools/design_filter.py
$ ads1256 dsp
```

### Decimation filter

| Property | Value |
|---|---|
| Taps | 68 (17 per polyphase branch) |
| Rate | 30 000 → 7500 Hz, M = 4 |
| −3 dB corner | 3501 Hz |
| Passband droop, DC–3 kHz | 0.06 dB |
| **Worst-case alias rejection into DC–3 kHz** | **55.6 dB** (requirement: >40 dB) |
| Measured on a 6 kHz tone (folds to 1500 Hz) | 58.3 dB rejected |
| Same tone, no anti-alias filter (control) | **0.0 dB** rejected |
| MAC rate, polyphase | 510 kMAC/s |
| MAC rate, naive filter-then-discard | 2.04 MMAC/s |

The control line is the one that matters: without the filter, a 6 kHz input
arrives in the output at full amplitude and sitting at 1500 Hz, where nothing
downstream can distinguish it from real signal.

### Fixed-point fidelity

| Comparison | Result |
|---|---|
| vs. float using the same Q15 taps | max 0.499 codes — half-LSB rounding only |
| vs. float using ideal taps | 16.0 codes RMS on a 199 252-code signal |
| …as a relative error | 8.0 × 10⁻⁵, ≈70 dB below the data |

The first row says the integer path is exact. The second says the residual is
the Q15 tap quantisation — a property of the representation, chosen knowingly,
not a defect in the implementation.

### Spectral estimation

| Estimator | Bin scatter |
|---|---:|
| 1024-point, 1 average | 5.37 dB |
| 16384-point, 1 average | 5.56 dB |
| 1024-point, 6 averages (Welch) | **2.00 dB** |

A 16× longer transform gives 16× the frequency resolution and **no** variance
reduction, exactly as theory says: a periodogram bin is exponentially
distributed with standard deviation equal to its mean, ~5.57 dB, independent of
length. Averaging six 50 %-overlapped segments cuts it by 2.7× (slightly short
of √6 because overlapped Hann segments are not fully independent).

### THD+N

| Signal | Reported |
|---|---|
| Clean 1 kHz tone | < −55 dB (window-leakage limited, see [dsp.md](dsp.md#thdn-and-its-floor)) |
| 1 kHz + 5 % third harmonic | −25.3 dB (expected ≈ −26 dB) |
| Shorted input, no tone | **INVALID** — the device declines to report |

## 9. Host-side verification

```
$ cmake -B build-host -DDAQ_BUILD_HOST_TESTS=ON && cmake --build build-host
$ ctest --test-dir build-host --output-on-failure
100% tests passed, 0 tests failed out of 6

$ cd host && python -m pytest -q
43 passed
```

The C tests compile the *same* `firmware/lib/*.c` the image links, under
AddressSanitizer and UndefinedBehaviorSanitizer — including the ring-buffer index
wrapping past 2³², which on a continuously streaming device happens after about
six days and is otherwise essentially untestable.

The DSP is checked twice over, in two languages, against one generated vector:
`tools/design_filter.py` emits the taps, the C header, the host's JSON and a
golden input/output pair, and both `tests/test_fir_decimate.c` and
`host/tests/test_dsp.py` assert the same expected output. See
[dsp.md](dsp.md#verification) for the full map of claim to test.
