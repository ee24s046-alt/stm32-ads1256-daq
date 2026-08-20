# Architecture

## The shape of the problem

A 24-bit delta-sigma converter is only as good as the timing around it. The ADS1256
asserts `/DRDY` when a conversion is ready and expects to be read before the next one
completes; miss that window and you either lose the sample or, worse, read a torn one.
At the same time the samples have to cross a USB link whose host can stall for an
entire 1 ms frame whenever it feels like it.

Those two requirements pull in opposite directions. The whole architecture is the
answer to holding both at once.

```
                 ADS1256                     STM32F407 @ 168 MHz
              ┌───────────┐
   AIN0-7 ───▶│  Σ-Δ ADC  │
              │  24-bit   │── /DRDY ──▶ EXTI3 ISR        prio 5 (NVIC)
              │  30 kSPS  │◀── SPI1 ──┐    │  latch TIM2, notify      1.2 µs
              └───────────┘           │    ▼
                                      │  ┌─────────────────────────┐
                                      └──│ sampler      prio 4     │  61 µs / DRDY
                                         │ RDATA, mux advance      │
                                         └────────────┬────────────┘
                                                      │ ring_raw (SPSC, CCM)
                                                      │ 256 × 20 B
                                         ┌────────────▼────────────┐
                                         │ processor    prio 3     │  11 µs / scan
                                         │ calibrate, encode, CRC  │
                                         └────────────┬────────────┘
                                                      │ ring_tx (SPSC, CCM)
                                                      │ 256 × 24 B
                                         ┌────────────▼────────────┐
                                         │ uplink       prio 2     │  40 µs / burst
                                         │ batch 16 pkt → bulk IN  │
                                         └────────────┬────────────┘
                                                      │ USB OTG_FS, CDC-ACM
                                                      ▼
                                                    host
```

Two service tasks sit alongside the three-stage data path, both at priority 1 -
deliberately the lowest things in the system above idle, because neither may
ever delay acquisition:

- `housekeeping` owns the heartbeat LED, the command parser and the statistics
  dump.
- `metrics` owns the on-device DSP: RMS, the averaged PSD and THD+N. It is fed a
  third ring by the processor and emits its own records into a fourth, which the
  uplink drains alongside the sample packets. It is also the only FPU user in
  the firmware - see [dsp.md](dsp.md#float-fft-fixed-point-filter) for why that
  isolation matters.

In `DAQ_MODE_WIDEBAND` the processor additionally runs an M=4 polyphase
anti-alias decimator, which is what takes the 30 kSPS single-channel stream down
to a 180 kB/s link rate. See [dsp.md](dsp.md).

## Why three tasks and not one

The single-task version is genuinely tempting — one loop that waits on `/DRDY`,
reads, converts and writes to USB. It fails for a specific reason: the USB write
can block for up to a full USB frame, and during that block the next `/DRDY`
arrives and is lost. Not "might be" — will be, as soon as the host is busy.

Splitting the pipeline at two ring buffers converts that failure mode into a
capacity question. A host stall no longer costs samples; it costs ring occupancy.
The `ring_tx` buffer holds 256 packets, which at 1875 scans/s is **137 ms** of
host misbehaviour absorbed before anything is lost. That is the design's actual
tolerance, it is a number rather than a hope, and the high-water mark reported by
the `stats` command says how much of it has ever been used.

## Why the ISR does almost nothing

The `/DRDY` handler latches `TIM2->CNT` and gives a task notification. That is all.
Measured worst case: **1.21 µs**.

The alternative — doing the SPI transaction in the ISR — puts ~61 µs of
mostly-spinning inside an interrupt context, during which every interrupt at or
below `configMAX_SYSCALL_INTERRUPT_PRIORITY` is masked, *including the USB core's*.
It works on the bench and drops USB packets under load. The whole point of the
measured results is that this design does not.

Two details in the ISR are load-bearing:

- **The EXTI pending bit is cleared first.** Clearing it last leaves a window in
  which a second edge arriving during the handler is cleared along with the first
  and the sample is silently lost — a bug that only appears under load.
- **The timestamp is taken here, not in the processor task.** Every packet's
  `t_us` therefore reflects when the conversion finished, not when software got
  round to it. For a pulse-transit-time measurement, jitter in that field is
  indistinguishable from jitter in the physiology.

## Why priorities are 4 / 3 / 2

Rate-monotonic: shortest deadline gets the highest priority.

| Task | Deadline | Period | Priority |
|---|---|---|---|
| sampler | 133 µs (next `/DRDY`) | 133 µs | 4 |
| processor | 137 ms (ring fills) | 1 ms poll | 3 |
| uplink | ~1 ms (USB frame) | 1 ms poll | 2 |
| housekeeping | none | 100 ms | 1 |

This is not "1, 2, 3 because there are three tasks". Swap the processor and the
uplink and the system still runs — until the host stalls, at which point the
uplink's retries start preempting the calibration work that is trying to drain
the ring the stall is filling.

## Why a custom ring and not `xQueueSendFromISR`

`xQueueSend` takes the scheduler's critical section for the duration of the copy
and the list walk. With a 133 µs hard deadline, every microsecond of unrelated
interrupt latency is margin spent.

A correct SPSC ring needs no critical section at all: the producer owns `head`,
the consumer owns `tail`, and correctness comes from two release/acquire fences
rather than from masking interrupts. See [`spsc_ring.h`](../firmware/include/spsc_ring.h)
for the contract and `tests/test_spsc_ring.c` for the cases that matter —
particularly the free-running index wrapping past 2³², which on a device
streaming continuously happens after about 6 days.

## Why the rings live in CCM

The STM32F407's 64 KB of core-coupled memory is zero-wait-state from the core and
**unreachable by DMA and by the USB core**. Putting the rings there means sampler
accesses never contend with USB FIFO traffic on the SRAM1 bus matrix.

The second benefit is negative-space: it becomes structurally impossible to hand
a ring address to a DMA engine by mistake, because that would bus-fault
immediately rather than transferring garbage.

## Why static allocation is enforced, not merely preferred

`configSUPPORT_DYNAMIC_ALLOCATION` is `0` and no `heap_*.c` is compiled into the
image. This is not a style preference:

- Out-of-memory becomes a **link-time** error rather than a runtime one.
- Worst-case RAM is the linker map, not an empirical observation.
- Fragmentation cannot exist, so there is no allocation-order-dependent failure
  that shows up after eight hours of running.

If someone later adds a `pvPortMalloc()`, the build breaks. That is the
enforcement mechanism, and it is the reason the claim "fully static" is
verifiable rather than asserted.

## Memory map

| Region | Size | Contents |
|---|---|---|
| FLASH sectors 0–10 | 896 KB | code, 23.1 KB used |
| FLASH sector 11 | 128 KB | calibration log (`CALSTORE`, carved out in the linker script) |
| SRAM1+2 | 128 KB | `.data` + `.bss` + MSP stack, 11.0 KB used |
| CCM | 64 KB | ring buffer storage, 12.0 KB used |

Sector 11 is a **separate `MEMORY` region**, not part of `FLASH`. The linker
therefore physically cannot place code where the runtime calibration writer
erases. See [calibration.md](calibration.md).

## Failure behaviour

| Failure | Detection | Response |
|---|---|---|
| HSE dead | bounded wait in `SystemInit` | fall back to HSI, report over UART, USB will not enumerate |
| ADS1256 absent | `STATUS.ID != 3` | reported at boot, error LED, no silent zeros |
| SCLK too fast | register read-back mismatch | reported at boot |
| `/DRDY` stops | 100 ms timeout in the sampler | error LED, `deadline_misses` counter |
| ring overflow | `spsc_push` returns false | counted; flag bit set in the affected packet |
| host stalls | `usb_cdc_tx_ready()` false | packets stay in the ring; `usb_retries` counter |
| stack overflow | FreeRTOS check mode 2 | halt, task name over UART, error LED |
| bus/usage fault | own vectors, CFSR decode | halt with a diagnosis over UART, RAM preserved for a debugger |

Nothing in this table auto-resets. A device that faults once a minute and resets
looks healthy from the outside; halting preserves the evidence.

## Related documents

- [protocol.md](protocol.md) — the wire format and why framing is not just a sync word
- [timing-budget.md](timing-budget.md) — the derivation of every deadline
- [calibration.md](calibration.md) — the two-point model and the flash log
- [measured-results.md](measured-results.md) — what the bench actually reports
- [bringup.md](bringup.md) — flashing, first light, and the failure modes in order
- [hardware.md](hardware.md) — pin map and analogue front-end notes
