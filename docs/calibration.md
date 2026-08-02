# Calibration and persistence

## Two calibrations, not one

They are complementary and both are required. Using only one is the usual mistake.

**`SELFCAL`** runs once at init and populates the ADS1256's own OFC/FSC registers.
It corrects the *converter* — its internal offset and gain. It knows nothing about
anything outside the chip.

**Two-point host calibration** corrects the *signal path*: the anti-alias filter,
the reference divider, the connector, the PCB. This is what the record in
`calibration.h` holds and what `flash_store.c` persists.

## The correction model

Per channel:

```
corrected = ((raw - offset) * gain_q16 + 32768) >> 16
```

- `offset` — the mean code with the inputs shorted, in ADC codes
- `gain_q16` — Q16.16 fixed point, 65536 == unity
- the `+ 32768` rounds to nearest; a bare shift floors, which biases every sample
  by −0.5 LSB and reintroduces exactly the DC error the offset term just removed

### Why fixed point rather than float

The F407 has a single-precision FPU, so `float` would work. It would also mean
FreeRTOS lazily stacks the 33-word FPU context on every preemption of the
processor task, adding jitter to a path whose whole job is to be boring.

Q16.16 covers gains from 0.0000153× to 32767× with a resolution of **15.3 ppm**.
The ADS1256's own gain-error spec is ±0.1 %, so the representation is roughly 65×
finer than the thing it is correcting — nowhere near the dominant error term.
`test_two_point_fit` asserts the realised error is within 32 codes on a
4.19 M-code reading, which is that 15.3 ppm bound and not a fudge factor.

### Saturation, not wraparound

`daq_cal_apply` clamps to ±(2²³−1). The intermediate is `int64_t`, which is not
optional: `(raw − offset)` reaches ~2²⁴ and `gain_q16` is ~2¹⁶, so the product
needs 40 bits. On a 32-bit multiply this truncates silently and the error only
appears near full scale — exactly where a calibration bug is hardest to notice.

Saturating rather than wrapping matters too. A clipped reading is recoverable
information ("this channel railed", and `FLAG_OVERRANGE` says so). A wrapped one
is a sign flip that looks like real signal and will quietly corrupt any
downstream analysis.

### Guard band on the fit

`daq_cal_from_two_point` refuses any fit implying a gain outside 0.5×–2.0×.
In practice that means the reference was not actually connected. Refusing at
calibration time is cheaper than shipping a unit whose gain is silently 100× off.

## Running a calibration

```
# 1. Short all four differential pairs at the input connector.
cal zero          # averages 4096 scans (2.2 s), stores the means as offsets

# 2. Apply a known reference to each channel, then (optionally) run the
#    two-point fit via the host.

# 3. Persist.
cal save
```

4096 is a power of two so the division is a shift, and it is long enough that the
residual uncertainty in the averaged offset is ~64× below the single-sample
noise — the calibration is not itself a noise source.

## The flash log

### Why a log rather than erase-and-rewrite

An STM32F4 sector erase takes 1–2 s and leaves the sector reading `0xFF` for that
entire window. Lose power during it and the previous calibration is gone, with no
indication that anything was lost — the unit simply cold-boots uncalibrated.

Instead, sector 11 (128 KB, carved out as its own `CALSTORE` region in the linker
script) holds **2048 × 64-byte slots**, written in order:

- A save programs the next blank slot. ~100 µs, **no erase**.
- On boot, the scan walks backwards from the first blank slot; the newest record
  whose CRC-32 validates wins.
- The sector is erased only when it fills — after 2048 saves — and the erase is
  immediately followed by rewriting the record that was current.

### Power-fail behaviour

A save interrupted mid-program leaves a slot with a bad CRC. The boot scan skips
it and falls back to the previous valid slot. The only value lost is the one
being written, which is the correct outcome.

### Record layout (64 bytes)

| Offset | Size | Field |
|---:|---:|---|
| 0x00 | 4 | `magic` = `0x31434441` ("ADC1") |
| 0x04 | 2 | `version` |
| 0x06 | 2 | `num_channels` |
| 0x08 | 4 | `serial` — monotonic write counter |
| 0x0C | 4 | `uptime_s` at calibration |
| 0x10 | 16 | `offset[4]`, int32 codes |
| 0x20 | 16 | `gain_q16[4]`, int32 Q16.16 |
| 0x30 | 12 | reserved |
| 0x3C | 4 | `crc32` over bytes 0x00…0x3B |

`_Static_assert(sizeof(daq_cal_t) == 64)` and
`_Static_assert(131072 % sizeof(daq_cal_t) == 0)` tie the record to the log
geometry at compile time, so a field added later cannot silently misalign the
slots.

`magic` must not be `0xFFFFFFFF`, because that is what an erased slot reads as
and it would otherwise look like a valid record.

### Flash constraints honoured

- Programming can only clear bits (1 → 0), never set them — hence append-only.
- x32 programming requires 4-byte alignment; 64-byte slots at a sector-aligned
  base satisfy this by construction.
- **The ART data cache must be invalidated after an erase.** The F407's caches
  sit in front of the flash array and do not observe the erase; without the
  `DCRST` pulse in `flash_erase_cal_sector()`, reads of the just-erased sector
  return pre-erase content for as long as the line stays resident — which makes
  the "newest valid slot" scan find a record that no longer physically exists.
- Every write is verified by reading it back **through the normal accessor**, not
  the write pointer, which also proves the CRC and the scan agree with intent.

### Linker enforcement

```ld
FLASH    (rx) : ORIGIN = 0x08000000, LENGTH = 896K   /* sectors 0..10 */
CALSTORE (r)  : ORIGIN = 0x080E0000, LENGTH = 128K   /* sector 11     */
```

`CALSTORE` is a separate region, so the linker cannot place code where the
runtime writer erases. The region's base and size are exported as symbols and
consumed by `flash_store.c`, so the address cannot drift out of agreement with
the map.

## Endurance

STM32F4 flash is specified for 10 000 erase cycles per sector. With 2048 slots
per erase, that is roughly **20 million calibration saves** before the sector
wears out — which is not a constraint anyone will reach, and was not the point:
the log exists for power-fail safety, and the endurance is a free consequence.
