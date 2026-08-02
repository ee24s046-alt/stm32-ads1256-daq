# Timing budget

Every number here is derived from a datasheet or clock constant, then measured on
the target with `DWT->CYCCNT` (168 MHz, 5.95 ns resolution). Derivation first,
measurement second — a measurement without a prediction to check it against is
just a number.

## Where 133 µs comes from

This is the number the entire real-time design is built on, and it is the one
most ADS1256 projects get wrong.

The converter's `DRATE` register is set to **30 kSPS**. That is the raw
conversion rate, and it is tempting to divide it by four channels and claim
7.5 kSPS each with a 33 µs service window.

That is not what a multiplexed ADS1256 delivers. The decimation filter is a
5th-order sinc. After a MUX change followed by `SYNC`/`WAKEUP`, the filter is
restarted, and the first *fully settled* result is not available until it has
flushed. The datasheet's settling-time table gives **0.13 ms at
fDATA = 30 kSPS** — exactly four conversion periods.

So:

```
  DRATE                            30 000 conversions/s
  settling periods per result             4
  ─────────────────────────────────────────────────────
  settled conversions delivered     7 500 /s   →  /DRDY every 133 µs
  channels                                4
  ─────────────────────────────────────────────────────
  complete 4-channel scans          1 875 /s   →  one scan every 532 µs
```

**133 µs is the hard deadline** — the interval between `/DRDY` edges, and the
window in which the sampler must complete one channel's SPI transaction. The
firmware checks this on every scan (`DAQ_DRDY_PERIOD_US` in `daq_types.h`) and
sets `DAQ_STATUS_DRDY_LATE` in the packet if it is ever exceeded.

Reading unsettled conversions instead — which is what running at a nominal
33 µs window would require — does not fail loudly. It shows up much later as
inter-channel crosstalk, at which point it is indistinguishable from an analogue
layout problem.

## SPI transaction cost

SCLK is constrained to ≤ fCLKIN/4 = 7.68 MHz / 4 = **1.92 MHz**. From an 84 MHz
APB2 the available prescalers give 2.625 MHz (÷32, out of spec) or **1.3125 MHz**
(÷64, used). Note that the ÷32 option *mostly works*, because it violates a setup
time rather than a hard limit — which is worse than failing, and is why the driver
verifies every register by read-back.

Per `/DRDY`, the driver runs TI's documented channel-cycling sequence, all inside
one chip-select window:

| Step | Bytes | Time @ 1.3125 MHz |
|---|---:|---:|
| `RDATA` command | 1 | 6.1 µs |
| t6 settling delay (50 tCLKIN) | — | 6.5 µs |
| conversion result | 3 | 18.3 µs |
| `WREG MUX` (next channel) | 3 | 18.3 µs |
| t11 delay | — | 0.5 µs |
| `SYNC` | 1 | 6.1 µs |
| t11 delay | — | 0.5 µs |
| `WAKEUP` | 1 | 6.1 µs |
| **total** | **9** | **62.4 µs predicted** |

Keeping all of this inside a single CS window rather than three saves about
1.5 µs of GPIO transitions and, more usefully, removes two places where a glitch
could desynchronise the command stream.

## Budget

| Stage | Predicted | Measured (max) | Deadline | Utilisation |
|---|---:|---:|---:|---:|
| `/DRDY` → ISR entry | ~0.2 µs | 0.31 µs | — | — |
| EXTI3 ISR body | ~1.0 µs | 1.21 µs | 133 µs | 0.9 % |
| ISR → sampler running | ~2 µs | 2.9 µs | — | 2.2 % |
| sampler SPI sequence | 62.4 µs | 61.4 µs | 133 µs | **46.2 %** |
| processor, per scan | ~10 µs | 11.4 µs | 137 ms | 2.1 % |
| uplink, per 16-pkt burst | ~35 µs | 40.2 µs | ~1 ms | 0.5 % |

Worst-case path from `/DRDY` to the sample being in `ring_raw`:
1.21 + 2.9 + 61.4 = **65.5 µs**, i.e. **49 % of the 133 µs deadline**. The
remaining 51 % is the margin that absorbs a coincident USB interrupt, a tick
interrupt and a context switch.

## CPU load

From FreeRTOS run-time statistics (TIM2, 1 MHz), 300-second run while streaming:

| Task | Share |
|---|---:|
| sampler | 46.2 % |
| processor | 2.1 % |
| uplink | 0.5 % |
| housekeeping | 0.1 % |
| timer service | < 0.1 % |
| kernel tick + switches | 1.4 % |
| **IDLE** | **49.7 %** |

The idle task executes `WFI`, so that 49.7 % is genuine headroom rather than a
spin loop — which is the only reason the figure means anything.

Almost all of the sampler's share is spinning on SPI status flags, not
computation. That is a deliberate trade: see [`spi.h`](../firmware/include/spi.h)
for why DMA would *increase* worst-case latency here rather than reduce it.

## Interrupt priorities

The STM32F4 implements the upper 4 bits of the 8-bit NVIC priority field, so the
numbers below are shifted left by 4 in the register.

| Source | Priority | Rationale |
|---|---:|---|
| EXTI3 (`/DRDY`) | 5 | most urgent level from which a `FromISR()` call is legal |
| OTG_FS | 6 | a USB frame is 1 ms and the FIFOs absorb latency; a missed `/DRDY` is a lost sample |
| SysTick / PendSV | 15 | kernel, lowest |

`configMAX_SYSCALL_INTERRUPT_PRIORITY` is 5. An ISR one step more urgent than
that may not call kernel APIs, and `vTaskNotifyGiveFromISR` from such a context
would corrupt the scheduler — `configASSERT` catches it, and is left enabled in
release builds for exactly this reason.

## Reproducing these numbers

```
# on the device, over the debug UART
stats

# on the host
ads1256 capture --port /dev/ttyACM0 --seconds 300 --out run.npz --raw run.bin
ads1256 verify run.npz
```

`stats` reports `isr latency max`, `sampler max` against its budget, both ring
high-water marks and every task's stack high-water mark. `verify` independently
confirms from the wire that nothing was lost.
