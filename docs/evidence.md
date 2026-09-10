# Evidence checklist

Every headline claim in this repository is a measurement, so every headline
claim needs an artifact behind it. This document lists what those artifacts are,
exactly how to capture each one, and where it goes.

**Status key:** `[x]` captured and committed · `[ ]` not yet captured.

Item 9 is already done: the DSP demonstration is a property of the filter design
and the arithmetic, so it needs no hardware and is committed. Everything else
needs the bench.

| # | Backs the claim | Artifact | Status |
|---|---|---|---|
| 1 | deadline misses, CPU idle, stack high-water, ring watermarks | `docs/evidence/stats-300s.txt` | `[ ]` |
| 2 | `/DRDY` → sample latency, 133 µs margin | `docs/img/la-drdy-to-cs.png` | `[ ]` |
| 3 | full channel-cycling sequence fits the window | `docs/img/la-scan-window.png` | `[ ]` |
| 4 | 8.7 µV RMS, 20.1 effective bits, noise density | `docs/img/noise-shorted.png` | `[ ]` |
| 5 | zero dropped packets, reproducible by a reviewer | `captures/reference/shorted-10s.npz` | `[ ]` |
| 6 | the instrument physically exists | `docs/img/bench.jpg` | `[ ]` |
| 7 | the fault handler actually diagnoses | `docs/evidence/fault-example.txt` | `[ ]` |
| 8 | calibration survives power cycles | `docs/evidence/cal-persistence.txt` | `[ ]` |
| 9 | the DSP chain, verified without hardware | `docs/img/dsp-demo.png` | **`[x]`** |
| 10 | wideband mode holds its 33.3 µs deadline | `docs/img/la-wideband-window.png` | `[ ]` |

Until an item is `[x]`, the corresponding numbers in
[measured-results.md](measured-results.md) are predictions, not measurements,
and that document says so at the top. Replace them as you go.

---

## 1. Statistics dump

The cheapest artifact here and the most valuable per minute spent. It carries
the deadline-miss count, the CPU split, every stack high-water mark and both
ring watermarks in one block.

```bash
# terminal on the ST-LINK VCP, 921600 8N1
picocom -b 921600 /dev/ttyACM1 | tee docs/evidence/stats-300s.txt

# let it stream for 300 s with the host capturing, then:
> stats
```

Paste the block verbatim into `measured-results.md` sections 2–4, replacing the
tables. Do not round the numbers — an odd figure like `61.4 µs` reads as
measured; `60 µs` reads as estimated.

## 2 & 3. Logic analyser captures

The single most convincing artifact for a real-time claim. Any 8-channel USB
analyser (Saleae, or a $10 clone running sigrok/PulseView) is sufficient —
SCLK is only 1.3125 MHz.

**Setup**

| Channel | Signal | Pin |
|---:|---|---|
| 0 | `/DRDY` | PC3 |
| 1 | `/CS` | PA4 |
| 2 | `SCLK` | PA5 |
| 3 | `MOSI` | PA7 |
| 4 | `MISO` | PA6 |

- Sample rate ≥ 16 MS/s (≈12× oversampling on SCLK)
- Trigger: falling edge on `/DRDY`
- Capture window: 1 ms, so several 133 µs periods are visible

**Capture 2 — `la-drdy-to-cs.png`.** Zoom to a single `/DRDY` edge. Place
cursor A on the `/DRDY` falling edge and cursor B on the first `/CS` falling
edge. That interval is ISR entry + ISR body + notification + context switch into
the sampler task — the ~4.1 µs claimed in
[timing-budget.md](timing-budget.md). Screenshot **with the cursor readout
visible in frame**; a trace without a measurement annotation proves nothing.

**Capture 3 — `la-scan-window.png`.** Zoom out to show two consecutive `/DRDY`
edges, i.e. one full 133 µs period. Cursors on `/CS` falling to `/CS` rising
gives the sampler service time; the gap to the next `/DRDY` is your margin.
Enable the analyser's SPI decoder so the `RDATA` / data / `WREG MUX` / `SYNC` /
`WAKEUP` bytes are labelled — that turns the screenshot into documentation of
the protocol as well as the timing.

If you only have a scope: two channels (`/DRDY`, `/CS`) with cursors is enough
for capture 2, and is still worth far more than no capture.

## 4. Noise plot

```bash
# short all four differential pairs AT THE INPUT CONNECTOR, not at the ADC pins
ads1256 capture --port /dev/ttyACM0 --seconds 300 --out shorted.npz
ads1256 noise shorted.npz --plot docs/img/noise-shorted.png
```

Produces the time series in µV and the noise density in nV/√Hz for all four
channels. Requires `pip install 'ads1256-host[plot]'`.

Shorting at the connector rather than at the chip is the difference between
characterising the converter and characterising the instrument. The instrument
is what you built.

## 5. Reference capture

```bash
ads1256 capture --port /dev/ttyACM0 --packets 20000 \
                --out captures/reference/shorted-10s.npz \
                --raw captures/reference/shorted-10s.bin
```

About 10 s and ~500 KB. Commit both. The `.npz` lets a reviewer run

```bash
ads1256 verify captures/reference/shorted-10s.npz
ads1256 noise  captures/reference/shorted-10s.npz
```

and reproduce your headline numbers from your own data in ten seconds. The raw
`.bin` matters too: if the decoder later turns out to be wrong, raw bytes can be
re-analysed, whereas decoded samples have the mistake baked in.

Very few student repositories let a reader check the claims. This one should.

## 6. Bench photograph

`docs/img/bench.jpg`. Discovery board, ADS1256 module, the shorting links on the
input connector, the logic analyser clipped on, terminal visible if convenient.
One photo, well lit, in focus. It answers the question a repository alone cannot:
did this thing exist.

## 7. Fault-handler demonstration

Worth ninety seconds because it proves a feature that otherwise only exists as a
claim. Temporarily add to `main()`:

```c
volatile uint32_t *bad = (uint32_t *)0xF0000000;
*bad = 1;                      /* precise bus fault */
```

Capture the UART output into `docs/evidence/fault-example.txt`, then revert the
change. The output shows the decoded `BUS: precise data bus error`, a valid
`BFAR` holding `0xF0000000`, and the faulting PC — which is exactly the
diagnosis the handler exists to produce.

An `INVSTATE` example (branch to an address with bit 0 clear) is a good second
one if you want to show two.

## 8. Calibration persistence

```
> cal zero
> cal save
[cal ] persisted, serial=1, slots used=1
```

Power-cycle, then on reboot:

```
[cal ] loaded from flash, serial=1, slots used=1
```

Capture both boot banners into `docs/evidence/cal-persistence.txt`. Repeat the
save a few times so `serial` and `slots used` both increment — that demonstrates
the append-only log rather than just "a value was stored".

If you are feeling thorough: cut power *during* a `cal save` and show the next
boot falling back to the previous record. That is the behaviour the whole
log design exists for, and almost nobody demonstrates it.

---

## 9. DSP demonstration

The only artifact here that needs no hardware at all - it is a property of the
filter design and the arithmetic:

```bash
ads1256 dsp --plot docs/img/dsp-demo.png
```

Three panels: the filter response with the aliasing bands shaded and the
-40 dB requirement marked; a 6 kHz tone decimated with and without the
anti-alias filter; and a single periodogram against the 6-average Welch estimate
on the same data at the same resolution. Also capture the console output to
`docs/evidence/dsp.txt` - the numbers are the substance and the plot is the
illustration.

## 10. Wideband-mode timing

Same logic-analyser setup as captures 2 and 3, but with the device in
`mode wide`. This is the harder case and therefore the more interesting capture:
the `/DRDY` period is 33.3 us rather than 133 us and the sampler uses 80 % of it.

Put cursors on `/DRDY` falling to `/CS` rising and show that the 3-byte RDATAC
read completes before the next edge. If you capture only one timing screenshot,
capture this one - the muxed mode has 2x margin and nobody doubts it; the
wideband mode is where the design is actually being claimed to work.

## 11. Real signal traces

The most convincing artifact on this list, and the one most easily forgotten.
Every other item proves the firmware is correct; this one proves the instrument
measures the thing it was built to measure.

```bash
ads1256 capture --port /dev/ttyACM0 --seconds 20 --out run.npz
ads1256 plot run.npz --out docs/img/traces-pressure-flow.png \
    --channels 0,1,2 \
    --labels "Pressure prox,Pressure dist,Flow" \
    --seconds 6
```

Add `--loop "Diameter,Pressure prox"` for a pressure-diameter loop once the
wall-tracking channel is in the capture - that single plot is the scientific
output the whole instrument exists to produce.

`--scale` applies a per-channel factor after the mV conversion, so pass your
transducer sensitivities and `--unit mmHg` to plot in physical units rather than
volts.

The x-axis is built from the device's own `t_us` timestamps, with the TIM2
rollover unwrapped - not from the sample index over a nominal rate. If a
conversion was ever dropped, the gap shows in the trace rather than being
silently closed up.

## Packaging it for someone who will not clone the repo

`tools/make_evidence_pdf.py` builds `docs/evidence/project-evidence.pdf` - three
pages covering the system, the verification that needs no hardware, the coverage
and static-analysis results, and the DSP plot.

```bash
python3 tools/make_evidence_pdf.py
```

It reads bench figures from `docs/evidence/bench.json`, which ships with
`"measured": false`. While that is false the bench section is **omitted
entirely** rather than printed with placeholders: a reviewer who finds a TODO in
a document titled "evidence" reads the whole thing as a draft. Fill the file in
after a bench run and re-run the script.

## Helper

`tools/collect-evidence.sh` runs the host-side items (1, 4, 5) and puts the
files in the right places. The logic analyser captures and the photograph are
manual by nature.

```bash
./tools/collect-evidence.sh /dev/ttyACM0
```
