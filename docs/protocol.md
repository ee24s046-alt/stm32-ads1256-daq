# Wire protocol

Version `0x01`. Implemented by [`firmware/lib/packet.c`](../firmware/lib/packet.c)
and, independently, by [`host/ads1256_host/protocol.py`](../host/ads1256_host/protocol.py).
Both are checked against the same golden vector — `tests/test_packet.c` on the
firmware side, `host/tests/test_protocol.py` on the host side. Two implementations
against one fixed byte sequence is what keeps an encoder and a decoder from
drifting apart.

## Direction and format separation

The USB bulk IN endpoint carries **only** 24-byte binary packets. Never text,
never status strings, never a log line.

This matters more than it looks. A receiver has to re-acquire framing after any
interruption, and it does that by scanning for a byte pattern. If human-readable
text can appear in the same stream, the resynchroniser has to distinguish text
from sample data — which it cannot do reliably, because 24-bit sample data
contains every byte value including printable ASCII.

So:

| Direction | Endpoint | Content |
|---|---|---|
| device → host | EP1 IN, bulk | binary packets, nothing else |
| host → device | EP1 OUT, bulk | ASCII commands, newline-terminated |
| device → bench | USART2, 921600 8N1 | all human-readable output |

Command responses go out on the debug UART. One direction, one format, no ambiguity.

## Record types

The device emits three kinds of record. All share the `0xA5` first sync byte;
the second byte identifies the type **and fixes the length**.

| Sync | Type | Bytes | Rate |
|---|---|---:|---|
| `A5 5A` | sample scan | 24 | 1875/s muxed, 7500/s wideband |
| `A5 5B` | live metrics | 64 | 10/s |
| `A5 5C` | averaged PSD block | 1044 | ~2/s, one channel round-robin |

The length is implied by the type rather than carried in a field. That is
deliberate: a corrupted stream cannot lie about a length, and a receiver that has
never heard of a record type still knows exactly how many bytes to skip.

A record always completes before the next one starts. A PSD block is 1044 bytes
and does not fit in the 384-byte endpoint FIFO, so the uplink sends it as three
writes — but it never interleaves sample packets into the middle of one, because
the host's framer would then be looking at a sample packet embedded inside a PSD
block and would reject both on CRC.

## Packet layout

24 bytes, little-endian throughout, 4-byte aligned.

| Offset | Size | Field | Notes |
|---:|---:|---|---|
| 0 | 2 | `sync` | `0xA5 0x5A` |
| 2 | 1 | `ver` | `0x01` |
| 3 | 1 | `flags` | see below |
| 4 | 2 | `seq` | uint16, free-running scan counter, wraps at 65535 |
| 6 | 4 | `t_us` | uint32, TIM2 microseconds at scan start |
| 10 | 12 | `ch[4]` | 4 × int24, two's complement |
| 22 | 2 | `crc16` | CRC-16/CCITT-FALSE over bytes 0…21 |

Aggregate rate: 1875 scans/s × 24 B = **45 kB/s**, about 0.4 % of USB full-speed
bulk bandwidth.

### Why 24 bytes

It is 4-byte aligned, so the firmware encoder never performs an unaligned store
(which would fault, since `UNALIGN_TRP` is enabled) and the host can view the
stream as a NumPy array without a copy. Padding to 32 would waste 33 % of the
link for nothing; packing to 22 would save 8 % and cost both of those properties.

### Flags

| Bit | Name | Meaning |
|---:|---|---|
| 0 | `CAL_APPLIED` | offset + gain correction was live for this scan |
| 1 | `OVERRANGE` | at least one channel hit 24-bit full scale |
| 2 | `DRDY_LATE` | the sampler exceeded its 133 µs deadline |
| 3 | `RAW_OVF` | stage-1 ring overflowed (processor fell behind) |
| 4 | `TX_OVF` | stage-2 ring overflowed (host fell behind) |

Bits 2–4 are the device's own admission that something went wrong. They are
reported by `ads1256 verify` alongside the two independent checks below, and all
three should agree.

### Timestamps

`t_us` is captured in the `/DRDY` ISR at the start of the scan, from a
free-running 32-bit TIM2 at 1 MHz. It wraps every 71.6 minutes; decoders must
handle that (`analysis.timestamp_continuity` does).

Taking the timestamp in the ISR rather than at packetisation time is the reason
the measured inter-scan jitter is sub-microsecond rather than sub-millisecond.
Scheduling latency in the processor task shows up as latency in *delivery*, never
as error in the *data*.

## Framing

**Framing is sync AND version AND CRC. Never sync alone.**

The bytes `0xA5 0x5A` occur naturally in 24-bit sample data with probability
2⁻¹⁶ per position — roughly once every 65 kB, so about once every 1.5 seconds at
45 kB/s. A receiver that locks on the sync word will lock onto sample data within
a couple of minutes and stay wrong.

The decoder therefore validates all three. `test_false_sync_inside_payload_is_rejected`
on both sides constructs a packet whose channel data deliberately *contains* the
sync word at offset 11 of every frame, and asserts that framing is never lost.

### Resynchronisation

`StreamDecoder` is resumable across arbitrary chunk boundaries — a serial port
splits reads wherever it likes, and `test_decoder_handles_arbitrary_chunking`
feeds it in sizes from 1 byte to 4 KB.

On loss of sync it advances to the next occurrence of the sync word rather than
one byte at a time, which on a large backlog of garbage is the difference between
a millisecond and a second. It also distinguishes two situations that look alike
and are not:

- **wrong position in the stream** → resync, expected once at attach
- **right position, bad CRC** → `bad_crc`, a genuine link fault

### CRC choice

CRC-16/CCITT-FALSE: polynomial `0x1021`, init `0xFFFF`, no reflection, no final
XOR. Check value for `"123456789"` is `0x29B1`, asserted on both sides.

The STM32F4's hardware CRC unit is deliberately **not** used. It implements only
the Ethernet polynomial with a fixed initial value, which is not CCITT-FALSE;
pretending otherwise is a classic source of "works on my host decoder" bugs. The
bitwise software implementation costs about 4 % of one core at this data rate,
which is affordable and auditable.

Over 22 bytes, CRC-16 detects all single-, double- and triple-bit errors and all
burst errors up to 16 bits. `test_every_single_bit_flip_is_caught` exhaustively
verifies the single-bit case over all 176 bit positions.

## Loss detection

Three independent pieces of evidence, which can and should disagree when
something is wrong:

1. **Sequence continuity** — `seq` increments by one modulo 65536. A gap means a
   packet left the device and did not arrive. Computed on the host from the wire,
   not read from a device counter, because a firmware counter can be wrong in
   exactly the same way the firmware is wrong.
2. **Timestamp continuity** — consecutive `t_us` differ by 532 µs ± tolerance.
   A gap here means a *conversion* went missing inside the device, before
   packetisation. This is visible nowhere else.
3. **Flag bits** — the device's own report.

`ads1256 verify capture.npz` checks all three and fails if any disagrees.

## Metrics record (`A5 5B`, 64 bytes)

| Offset | Size | Field |
|---:|---:|---|
| 0 | 2 | sync `0xA5 0x5B` |
| 2 | 1 | ver |
| 3 | 1 | acquisition mode (0 = mux4, 1 = wideband) |
| 4 | 2 | seq |
| 6 | 4 | t_us |
| 10 | 16 | `rms[4]`, int32, ADC codes, 100 ms window |
| 26 | 16 | `thdn_mdb[4]`, int32 milli-dB, `INT32_MIN` = no tone |
| 42 | 16 | `fund_mhz[4]`, uint32 milli-Hz, 0 = no tone |
| 58 | 4 | `psd_serial`, increments per completed PSD block |
| 62 | 2 | crc16 |

`INT32_MIN` is a real value, not a placeholder for laziness: on a shorted input
the largest spectral bin is just the luckiest noise bin, and a THD+N computed
from it would be a confident-looking number that means nothing. The device
declines rather than inventing one.

## PSD record (`A5 5C`, 1044 bytes)

| Offset | Size | Field |
|---:|---:|---|
| 0 | 2 | sync `0xA5 0x5C` |
| 2 | 1 | ver |
| 3 | 1 | channel |
| 4 | 2 | seq |
| 6 | 4 | t_us |
| 10 | 2 | nbins = 513 |
| 12 | 4 | fs of the analysed stream, milli-Hz |
| 16 | 1026 | `bins[513]`, int16, dBFS/Hz × 100 |
| 1042 | 2 | crc16 |

513 bins is a 1024-point FFT, DC to Nyquist inclusive. dBFS/Hz × 100 in an
int16 covers ±327 dB at 0.01 dB resolution — ample for a floor around
−150 dBFS/Hz, and half the size of sending floats.

`fs` travels with the block because it differs by mode (1875 Hz muxed, 7500 Hz
wideband). A spectrum labelled with the wrong sample rate has the wrong
frequency axis, and nothing downstream can detect it.

See [dsp.md](dsp.md) for how these are computed.

## Host → device commands

ASCII, newline-terminated, on the bulk OUT endpoint. Responses on the UART.

| Command | Effect |
|---|---|
| `run` | start streaming |
| `stop` | stop streaming (acquisition continues; packets are not queued) |
| `stats` | dump counters, timing maxima, ring watermarks, stack high-water marks |
| `cal zero` | average 4096 scans with inputs shorted, store as offsets (RAM) |
| `cal save` | append the current calibration to the flash log |
| `cal reset` | revert to unity gain / zero offset (RAM only) |
| `mode mux` | 4-channel multiplexed, 1875 scans/s |
| `mode wide` | single channel, 30 kSPS, M=4 decimation to 7.5 kSPS |
| `help` | list commands |

## Reference encoder

```python
from ads1256_host.protocol import Packet, encode_packet, decode_packet

pkt = Packet(seq=0x0102, t_us=0x01020304, flags=0x00,
             codes=(1, 0x7FFFFF, 0, -1))
wire = encode_packet(pkt)
assert wire.hex() == (
    "a55a01" "00"            # sync, version, flags
    "0201"                   # seq   = 0x0102
    "04030201"               # t_us  = 0x01020304
    "010000" "ffff7f"        # ch0 = +1,  ch1 = +full scale
    "000000" "ffffff"        # ch2 =  0,  ch3 = -1
    "b2a6"                   # CRC-16/CCITT-FALSE
)
assert decode_packet(wire).codes == (1, 8388607, 0, -1)
```
