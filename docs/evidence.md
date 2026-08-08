# Evidence checklist

Every headline claim in this repository is a measurement, so every headline
claim needs an artifact behind it. This document lists what those artifacts are,
exactly how to capture each one, and where it goes.

**Status key:** `[x]` captured and committed · `[ ]` not yet captured.

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

## Helper

`tools/collect-evidence.sh` runs the host-side items (1, 4, 5) and puts the
files in the right places. The logic analyser captures and the photograph are
manual by nature.

```bash
./tools/collect-evidence.sh /dev/ttyACM0
```
