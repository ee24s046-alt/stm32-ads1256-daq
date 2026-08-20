# 4-Channel 24-Bit Precision Data Acquisition System

A calibrated, RTOS-driven data-acquisition front end built around a delta-sigma
ADC, streaming live to a PC over USB with characterised noise performance.

STM32F407 (Cortex-M4) · TI ADS1256 · FreeRTOS · **no vendor HAL, no CMSIS pack,
no dynamic allocation** · register-level throughout.

[![firmware](https://img.shields.io/badge/firmware-40.3%20KB%20flash%20%2F%2041.3%20KB%20RAM-blue)]()
[![tests](https://img.shields.io/badge/tests-6%20CTest%20%2B%2043%20pytest-brightgreen)]()
[![license](https://img.shields.io/badge/license-MIT-lightgrey)]()

---

## What this is

Reading a 24-bit converter is easy. Reading one *on time, every time,* while a
USB host stalls whenever it likes, and being able to prove afterwards that
nothing was lost — that is the actual problem, and it is what this repository is
about.

| | |
|---|---|
| **Hardware** | STM32F407VGT6 @ 168 MHz · TI ADS1256 24-bit Σ-Δ · 4 differential channels |
| **Conversion rate** | 30 kSPS DRATE → 7500 settled conversions/s → **1875 four-channel scans/s** |
| **Wideband mode** | multiplexer parked → no settling penalty → **30 kSPS single channel**, decimated on-device to 7.5 kSPS |
| **On-device DSP** | M=4 polyphase anti-alias decimator · 1024-pt Welch PSD · live RMS / THD+N, streamed alongside the samples |
| **Real-time deadline** | 133 µs per `/DRDY`, **zero misses over 300 s** (2.25 M interrupts) |
| **Architecture** | 3-task FreeRTOS pipeline, sampler (4) → processor (3) → uplink (2), two lock-free SPSC rings |
| **Interface** | USB CDC virtual COM port, custom 24-byte binary framing with CRC-16 and resync |
| **Calibration** | offset + gain computed on-device, persisted to an append-only flash log, auto-loaded on cold boot |
| **Reliability** | fully static allocation (no `heap_*.c` linked), stack high-water < 60 % on every task, fault handler decoding CFSR/HFSR/MMFAR/BFAR over UART |

## Headline measurements

Full detail, with reproduction commands, in
**[docs/measured-results.md](docs/measured-results.md)**.

| Measurement | Result |
|---|---|
| `/DRDY` → sample in ring, worst case | **65.5 µs** against a 133 µs deadline (2.0× margin) |
| Deadline misses over 300 s | **0** of 2 250 000 |
| CPU idle, streaming | **49.7 %**, with the idle task in `WFI` |
| Noise floor, all inputs shorted | **8.74 µV RMS** — 2.6 dB above the ADS1256 datasheet figure |
| Effective resolution | 20.13 bits RMS-referred, **17.40 noise-free bits** (6.6 σ) |
| Dropped USB packets over 300 s | **0** of 562 500, verified from the wire by sequence *and* timestamp continuity |
| Stack high-water, worst task | 51.0 % of allocated |
| Alias rejection, measured | **55.6 dB** into DC–3 kHz, against a >40 dB requirement — with a no-filter control at 0.0 dB |
| Polyphase vs filter-then-discard | **4× fewer MACs**, bit-identical output (asserted, not assumed) |
| PSD variance, 6 averages | **5.37 dB → 2.00 dB** scatter; a 16× longer FFT gives 5.56 dB, i.e. no improvement |
| Flash / RAM | 40.3 KB / 41.3 KB + 26.8 KB CCM |

## Evidence

Those are measurements, so each one has an artifact behind it rather than a
claim in prose. [`docs/evidence.md`](docs/evidence.md) lists every artifact, the
exact command or instrument setup that produces it, and where it lives:

| Artifact | Backs |
|---|---|
| [`docs/evidence/stats-300s.txt`](docs/evidence/) | deadline misses, CPU split, stack high-water, ring watermarks |
| [`docs/img/la-drdy-to-cs.png`](docs/img/) | logic-analyser capture of `/DRDY` → `/CS`, with cursors |
| [`docs/img/la-scan-window.png`](docs/img/) | one full 133 µs window, SPI decoder enabled |
| [`docs/img/noise-shorted.png`](docs/img/) | time series and noise density, all four channels |
| [`captures/reference/`](captures/reference/) | a 10 s raw capture, so a reader can re-derive the numbers |
| [`docs/evidence/fault-example.txt`](docs/evidence/) | the fault handler's decoded output for a deliberate fault |

The reference capture is the one worth noting: it means you do not have to take
any of the above on trust.

```bash
ads1256 verify captures/reference/shorted-10s.npz
ads1256 noise  captures/reference/shorted-10s.npz
```

## Quick start

```bash
# firmware
cmake -B build -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi-gcc.cmake
cmake --build build
openocd -f interface/stlink.cfg -f target/stm32f4x.cfg \
        -c "program build/daq.elf verify reset exit"

# host tooling
pip install -e host[dev]
ads1256 monitor --port /dev/ttyACM0

# verify and demonstrate the on-device DSP chain (no hardware needed)
ads1256 dsp --plot dsp.png

# a 5-minute acceptance run
ads1256 capture --port /dev/ttyACM0 --seconds 300 --out run.npz --raw run.bin
ads1256 verify run.npz      # → RESULT: PASS
ads1256 noise  run.npz      # inputs shorted
```

No hardware to hand? Everything on the host side runs against a built-in
simulated device:

```bash
ads1256 selftest
ads1256 capture --simulate --packets 20000 --out sim.npz && ads1256 noise sim.npz
```

## Verification

Nothing in this repository is asserted without something checking it.

```bash
# the portable core, compiled natively, under ASan + UBSan
cmake -B build-host -DDAQ_BUILD_HOST_TESTS=ON
cmake --build build-host && ctest --test-dir build-host --output-on-failure

# host tooling
cd host && python -m pytest -q
```

The C tests link the **same `firmware/lib/*.c` files the image links** — not a
host reimplementation — so the lock-free ring and the wire codec under test are
the ones that ship. They cover the cases that are otherwise untestable on target:
the ring's free-running index wrapping past 2³² (which happens after about six
days of continuous streaming), bulk drain across the wrap boundary, exhaustive
single-bit corruption of every wire frame, and framing against a payload that
deliberately contains the sync word.

The firmware also builds with `-Wall -Wextra -Wconversion -Wsign-conversion
-Wshadow -Wcast-align -Wmissing-prototypes` **clean**, and `configASSERT` stays
enabled in release builds.

## Engineering decisions worth reading

Each of these is the answer to a specific failure, written up where the code
lives.

| Decision | Where |
|---|---|
| Why the `/DRDY` ISR does almost nothing, and the SPI work runs in a task | [architecture.md](docs/architecture.md#why-the-isr-does-almost-nothing) |
| Why 133 µs is the real deadline, and 33 µs is the number most ADS1256 projects get wrong | [timing-budget.md](docs/timing-budget.md#where-133-µs-comes-from) |
| Why a hand-written SPSC ring instead of `xQueueSendFromISR` | [`spsc_ring.h`](firmware/include/spsc_ring.h) |
| Why the rings live in CCM, where DMA cannot reach them | [`shared.c`](firmware/rtos/shared.c) |
| Why static allocation is *enforced by the linker*, not merely preferred | [`FreeRTOSConfig.h`](firmware/config/FreeRTOSConfig.h) |
| Why framing is sync **and** version **and** CRC — never sync alone | [protocol.md](docs/protocol.md#framing) |
| Why the calibration store is an append-only log rather than erase-and-rewrite | [calibration.md](docs/calibration.md#why-a-log-rather-than-erase-and-rewrite) |
| Why blocking SPI beats DMA for an 18 µs transfer | [`spi.h`](firmware/include/spi.h) |
| Why the fault handler prints MMFAR/BFAR only when their VALID bits are set | [`fault_handler.c`](firmware/core/fault_handler.c) |
| Why the USB IN endpoint carries binary only, never text | [`main.c`](firmware/core/main.c) |
| Why calibration is Q16.16 fixed point rather than `float` | [calibration.md](docs/calibration.md#why-fixed-point-rather-than-float) |
| Why the anti-alias stopband is at 4.5 kHz and not the output Nyquist — 68 taps instead of 660 | [dsp.md](docs/dsp.md#where-the-stopband-edge-comes-from) |
| Why a longer FFT does *not* reduce PSD variance, and averaging does | [dsp.md](docs/dsp.md#averaging-is-what-reduces-variance-fft-length-does-not) |
| Why the FIR is fixed point and the FFT is float — opposite choices, opposite reasons | [dsp.md](docs/dsp.md#float-fft-fixed-point-filter) |
| Why THD+N has a −65 dB floor, and why ±2 bins would make it −33 dB | [dsp.md](docs/dsp.md#thdn-and-its-floor) |
| Why the decimator is load-bearing rather than decorative | [`task_processor.c`](firmware/rtos/task_processor.c) |

## Repository layout

```
firmware/
  include/        public headers - the design rationale lives in these
  lib/            portable core: SPSC ring, packet codec, CRC, calibration,
                  polyphase FIR, FFT, Welch/THD+N metrics
                  (compiled into BOTH the image and the native unit tests)
  core/           startup path, clocks, faults, RTOS hooks, main
  drivers/        register-level SPI, GPIO, UART, flash, ADS1256
  usb/            OTG_FS device stack + CDC-ACM, written to the registers
  rtos/           the three pipeline tasks and their shared state
  config/         FreeRTOSConfig.h
  linker/         memory map, incl. the carved-out calibration sector
  startup/        vector table and reset handler
tests/            native unit tests over firmware/lib, ASan + UBSan
host/             Python package: decoder, capture, noise analysis, CLI, pytest
hardware/         interconnect diagram, schematic and design notes
captures/         reference capture, committed so the numbers are reproducible
tools/            design_filter.py - generates the FIR coefficients for BOTH
                  languages plus the golden vector both test against
                  collect-evidence.sh - regenerates the host-side artifacts
docs/             architecture, protocol, timing, calibration, results, bring-up
  img/            logic-analyser captures, noise plot, bench photograph
  evidence/       raw terminal output behind every measured claim
cmake/            arm-none-eabi toolchain file
```

## Documentation

- **[architecture.md](docs/architecture.md)** — the pipeline, and why it is three tasks
- **[timing-budget.md](docs/timing-budget.md)** — every deadline, derived then measured
- **[dsp.md](docs/dsp.md)** — multirate processing, spectral estimation, and how both are verified
- **[protocol.md](docs/protocol.md)** — wire format, framing, loss detection
- **[calibration.md](docs/calibration.md)** — correction model and the flash log
- **[measured-results.md](docs/measured-results.md)** — bench numbers and how to reproduce them
- **[evidence.md](docs/evidence.md)** — the artifact behind each number, and how to capture it
- **[bringup.md](docs/bringup.md)** — flashing, first light, failure modes in order
- **[hardware.md](docs/hardware.md)** — pin map, clock tree, analogue notes
- **[hardware/](hardware/)** — interconnect, schematic and front-end design notes

## Third-party

FreeRTOS Kernel V11.1.0, fetched at configure time (MIT). Nothing else — no HAL,
no CMSIS pack, no vendor middleware. The Cortex-M4 core registers and the STM32F4
peripheral map used here are hand-written from ARMv7-M ARM and RM0090, in
[`cortex_m4.h`](firmware/include/cortex_m4.h) and
[`stm32f407.h`](firmware/include/stm32f407.h).

## License

MIT — see [LICENSE](LICENSE).
