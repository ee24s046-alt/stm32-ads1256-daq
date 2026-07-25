"""Wire protocol: the exact mirror of firmware/lib/packet.c.

Layout (24 bytes, little-endian) - see docs/protocol.md::

    off  size  field
     0    2    sync   0xA5 0x5A
     2    1    ver    0x01
     3    1    flags
     4    2    seq    uint16
     6    4    t_us   uint32
    10   12    ch[4]  int24 x 4
    22    2    crc16  CCITT-FALSE over bytes 0..21

Two things in here are load-bearing and easy to get wrong:

* Framing is sync AND version AND CRC.  The sync word alone appears in real
  sample data about once every 65 kB, which at 180 kB/s is roughly three false
  candidates per second.  A decoder that trusts sync will lock onto noise.
* ``StreamDecoder`` is resumable.  It is fed arbitrary chunks from a serial
  port, which splits transfers wherever it likes, so it has to hold a partial
  frame across calls rather than assuming chunk boundaries align with packets.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass
from typing import Iterator, List, Optional

SYNC0 = 0xA5
SYNC1 = 0x5A
PROTO_VERSION = 0x01
PACKET_BYTES = 24
NUM_CHANNELS = 4

FLAG_CAL_APPLIED = 0x01
FLAG_OVERRANGE = 0x02
FLAG_DRDY_LATE = 0x04
FLAG_RAW_OVF = 0x08
FLAG_TX_OVF = 0x10

FLAG_NAMES = {
    FLAG_CAL_APPLIED: "cal",
    FLAG_OVERRANGE: "overrange",
    FLAG_DRDY_LATE: "drdy-late",
    FLAG_RAW_OVF: "raw-overflow",
    FLAG_TX_OVF: "tx-overflow",
}

_HEADER = struct.Struct("<BBBBHI")  # sync0, sync1, ver, flags, seq, t_us


def crc16_ccitt(data: bytes) -> int:
    """CRC-16/CCITT-FALSE: poly 0x1021, init 0xFFFF, no reflection."""
    crc = 0xFFFF
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def _sext24(raw: int) -> int:
    return raw - 0x1000000 if raw & 0x800000 else raw


@dataclass(frozen=True)
class Packet:
    """One 4-channel scan."""

    seq: int
    t_us: int
    flags: int
    codes: tuple

    @property
    def flag_names(self) -> List[str]:
        return [name for bit, name in FLAG_NAMES.items() if self.flags & bit]

    @property
    def overrange(self) -> bool:
        return bool(self.flags & FLAG_OVERRANGE)


def encode_packet(pkt: Packet) -> bytes:
    """Build a wire frame.  Used by the tests and the synthetic generator."""
    body = bytearray(
        _HEADER.pack(SYNC0, SYNC1, PROTO_VERSION, pkt.flags & 0xFF, pkt.seq & 0xFFFF,
                     pkt.t_us & 0xFFFFFFFF)
    )
    for code in pkt.codes:
        body += (code & 0xFFFFFF).to_bytes(3, "little")
    body += crc16_ccitt(bytes(body)).to_bytes(2, "little")
    return bytes(body)


def decode_packet(buf: bytes) -> Optional[Packet]:
    """Validate and decode one frame.  Returns None if it is not a valid frame."""
    if len(buf) < PACKET_BYTES:
        return None
    if buf[0] != SYNC0 or buf[1] != SYNC1 or buf[2] != PROTO_VERSION:
        return None
    if int.from_bytes(buf[22:24], "little") != crc16_ccitt(buf[:22]):
        return None

    _, _, _, flags, seq, t_us = _HEADER.unpack_from(buf, 0)
    codes = tuple(
        _sext24(int.from_bytes(buf[10 + 3 * c: 13 + 3 * c], "little"))
        for c in range(NUM_CHANNELS)
    )
    return Packet(seq=seq, t_us=t_us, flags=flags, codes=codes)


class StreamDecoder:
    """Resumable framer over an arbitrarily chunked byte stream.

    Tracks three things the capture tool reports on:

    ``resyncs``
        How many times framing was lost and re-acquired.  Should be 0 or 1
        (one if the capture started mid-frame).
    ``dropped``
        Packets missing, inferred from gaps in the sequence counter.  This is
        the number that proves "zero dropped packets over a 5-minute run" -
        the firmware's own counter could be wrong in the same way the
        firmware is wrong, so the check has to be made from the wire.
    ``bad_crc``
        Frames that had valid sync and version but failed CRC.  Non-zero here
        means genuine link corruption, not framing loss.
    """

    def __init__(self, max_buffer: int = 1 << 20) -> None:
        self._buf = bytearray()
        self._max_buffer = max_buffer
        self._last_seq: Optional[int] = None
        self._synced = False
        self.resyncs = 0
        self.dropped = 0
        self.bad_crc = 0
        self.decoded = 0
        self.discarded_bytes = 0

    def feed(self, chunk: bytes) -> Iterator[Packet]:
        """Consume a chunk, yielding every complete packet it completes."""
        self._buf += chunk

        # Guard against unbounded growth if the link produces only garbage:
        # keep the tail, which is where a valid frame would eventually start.
        if len(self._buf) > self._max_buffer:
            self.discarded_bytes += len(self._buf) - PACKET_BYTES
            del self._buf[:-PACKET_BYTES]
            self._synced = False

        while len(self._buf) >= PACKET_BYTES:
            pkt = decode_packet(bytes(self._buf[:PACKET_BYTES]))
            if pkt is not None:
                if not self._synced:
                    self._synced = True
                del self._buf[:PACKET_BYTES]
                self._note_sequence(pkt)
                self.decoded += 1
                yield pkt
                continue

            # Not a frame at offset 0.  Distinguish "wrong place in the stream"
            # from "right place, corrupted frame": the latter is a link fault
            # worth counting separately.
            head = self._buf[:PACKET_BYTES]
            if head[0] == SYNC0 and head[1] == SYNC1 and head[2] == PROTO_VERSION:
                self.bad_crc += 1

            if self._synced:
                self.resyncs += 1
                self._synced = False

            # Advance to the next plausible sync position rather than one byte
            # at a time: on a 1 MB backlog of garbage that is the difference
            # between a millisecond and a second.
            nxt = self._buf.find(bytes([SYNC0, SYNC1]), 1)
            if nxt < 0:
                self.discarded_bytes += len(self._buf) - 1
                del self._buf[:-1]
                break
            self.discarded_bytes += nxt
            del self._buf[:nxt]

    def _note_sequence(self, pkt: Packet) -> None:
        if self._last_seq is not None:
            expected = (self._last_seq + 1) & 0xFFFF
            if pkt.seq != expected:
                # Modulo-65536 distance forward.  A backwards jump means the
                # device restarted; treat that as a resync, not 65000 drops.
                gap = (pkt.seq - expected) & 0xFFFF
                if gap < 0x8000:
                    self.dropped += gap
                else:
                    self.resyncs += 1
        self._last_seq = pkt.seq

    @property
    def in_sync(self) -> bool:
        return self._synced
