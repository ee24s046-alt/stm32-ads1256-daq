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
SYNC1 = 0x5A                 # sample record (kept as the historical name)
SYNC1_SAMPLE = 0x5A
SYNC1_METRICS = 0x5B
SYNC1_PSD = 0x5C
PROTO_VERSION = 0x01
PACKET_BYTES = 24
METRICS_BYTES = 64
PSD_BINS = 513
PSD_BYTES = 18 + PSD_BINS * 2       # 1044
NUM_CHANNELS = 4

#: Second sync byte -> total record length. The length is implied by the type
#: rather than carried in a field, so a corrupted stream cannot lie about it and
#: a receiver that does not understand a record still knows how far to skip.
RECORD_LENGTHS = {
    SYNC1_SAMPLE: PACKET_BYTES,
    SYNC1_METRICS: METRICS_BYTES,
    SYNC1_PSD: PSD_BYTES,
}

MODE_MUX4 = 0
MODE_WIDEBAND = 1
MODE_NAMES = {MODE_MUX4: "mux4", MODE_WIDEBAND: "wideband"}

#: Sentinel the device uses when there is no tone to measure THD+N against.
THDN_INVALID = -(2 ** 31)

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
    if buf[0] != SYNC0 or buf[1] != SYNC1_SAMPLE or buf[2] != PROTO_VERSION:
        return None
    if int.from_bytes(buf[22:24], "little") != crc16_ccitt(buf[:22]):
        return None

    _, _, _, flags, seq, t_us = _HEADER.unpack_from(buf, 0)
    codes = tuple(
        _sext24(int.from_bytes(buf[10 + 3 * c: 13 + 3 * c], "little"))
        for c in range(NUM_CHANNELS)
    )
    return Packet(seq=seq, t_us=t_us, flags=flags, codes=codes)


@dataclass(frozen=True)
class MetricsRecord:
    """Live on-device metrics, emitted every 100 ms."""

    seq: int
    t_us: int
    mode: int
    rms: tuple                 #: per channel, in ADC codes
    thdn_db: tuple             #: per channel, None where the device said invalid
    fundamental_hz: tuple      #: per channel, None where no tone was found
    psd_serial: int

    @property
    def mode_name(self) -> str:
        return MODE_NAMES.get(self.mode, f"mode{self.mode}")


@dataclass(frozen=True)
class PsdRecord:
    """One averaged power spectrum for one channel, emitted every ~478 ms."""

    seq: int
    t_us: int
    channel: int
    fs_hz: float
    bins_dbfs: "np.ndarray"    #: dBFS/Hz, DC..Nyquist inclusive

    @property
    def freqs_hz(self):
        import numpy as np
        return np.arange(len(self.bins_dbfs)) * (self.fs_hz / (2 * (len(self.bins_dbfs) - 1)))


def decode_metrics(buf: bytes) -> Optional[MetricsRecord]:
    if len(buf) < METRICS_BYTES:
        return None
    if buf[0] != SYNC0 or buf[1] != SYNC1_METRICS or buf[2] != PROTO_VERSION:
        return None
    if int.from_bytes(buf[62:64], "little") != crc16_ccitt(buf[:62]):
        return None

    def i32(off):
        return int.from_bytes(buf[off:off + 4], "little", signed=True)

    def u32(off):
        return int.from_bytes(buf[off:off + 4], "little")

    thdn = tuple(
        None if i32(26 + 4 * c) == THDN_INVALID else i32(26 + 4 * c) / 1000.0
        for c in range(NUM_CHANNELS)
    )
    fund = tuple(
        None if u32(42 + 4 * c) == 0 else u32(42 + 4 * c) / 1000.0
        for c in range(NUM_CHANNELS)
    )
    return MetricsRecord(
        seq=int.from_bytes(buf[4:6], "little"),
        t_us=u32(6),
        mode=buf[3],
        rms=tuple(i32(10 + 4 * c) for c in range(NUM_CHANNELS)),
        thdn_db=thdn,
        fundamental_hz=fund,
        psd_serial=u32(58),
    )


def decode_psd(buf: bytes) -> Optional[PsdRecord]:
    import numpy as np

    if len(buf) < PSD_BYTES:
        return None
    if buf[0] != SYNC0 or buf[1] != SYNC1_PSD or buf[2] != PROTO_VERSION:
        return None
    if int.from_bytes(buf[10:12], "little") != PSD_BINS:
        return None
    crc_off = 16 + PSD_BINS * 2
    if int.from_bytes(buf[crc_off:crc_off + 2], "little") != crc16_ccitt(buf[:crc_off]):
        return None

    bins = np.frombuffer(buf[16:16 + PSD_BINS * 2], dtype="<i2").astype(np.float64) / 100.0
    return PsdRecord(
        seq=int.from_bytes(buf[4:6], "little"),
        t_us=int.from_bytes(buf[6:10], "little"),
        channel=buf[3],
        fs_hz=int.from_bytes(buf[12:16], "little") / 1000.0,
        bins_dbfs=bins,
    )


def encode_metrics(rec: MetricsRecord) -> bytes:
    """Build a metrics record. Used by the simulator and the round-trip tests."""
    buf = bytearray(METRICS_BYTES)
    buf[0:3] = bytes([SYNC0, SYNC1_METRICS, PROTO_VERSION])
    buf[3] = rec.mode & 0xFF
    buf[4:6] = (rec.seq & 0xFFFF).to_bytes(2, "little")
    buf[6:10] = (rec.t_us & 0xFFFFFFFF).to_bytes(4, "little")

    for c in range(NUM_CHANNELS):
        buf[10 + 4 * c: 14 + 4 * c] = int(rec.rms[c]).to_bytes(4, "little", signed=True)
        thdn = rec.thdn_db[c]
        raw = THDN_INVALID if thdn is None else int(round(thdn * 1000))
        buf[26 + 4 * c: 30 + 4 * c] = raw.to_bytes(4, "little", signed=True)
        fund = rec.fundamental_hz[c]
        buf[42 + 4 * c: 46 + 4 * c] = int(0 if fund is None else round(fund * 1000)) \
            .to_bytes(4, "little")

    buf[58:62] = int(rec.psd_serial).to_bytes(4, "little")
    buf[62:64] = crc16_ccitt(bytes(buf[:62])).to_bytes(2, "little")
    return bytes(buf)


def encode_psd(rec: PsdRecord) -> bytes:
    """Build a PSD record."""
    import numpy as np

    buf = bytearray(PSD_BYTES)
    buf[0:3] = bytes([SYNC0, SYNC1_PSD, PROTO_VERSION])
    buf[3] = rec.channel & 0xFF
    buf[4:6] = (rec.seq & 0xFFFF).to_bytes(2, "little")
    buf[6:10] = (rec.t_us & 0xFFFFFFFF).to_bytes(4, "little")
    buf[10:12] = PSD_BINS.to_bytes(2, "little")
    buf[12:16] = int(round(rec.fs_hz * 1000)).to_bytes(4, "little")

    scaled = np.clip(np.round(np.asarray(rec.bins_dbfs) * 100.0), -32768, 32767)
    buf[16:16 + PSD_BINS * 2] = scaled.astype("<i2").tobytes()

    crc_off = 16 + PSD_BINS * 2
    buf[crc_off:crc_off + 2] = crc16_ccitt(bytes(buf[:crc_off])).to_bytes(2, "little")
    return bytes(buf)


_DECODERS = {
    SYNC1_SAMPLE: decode_packet,
    SYNC1_METRICS: decode_metrics,
    SYNC1_PSD: decode_psd,
}


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
        self.metrics_records = 0
        self.psd_records = 0

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
            kind = self._buf[1] if self._buf[0] == SYNC0 else None
            length = RECORD_LENGTHS.get(kind) if kind is not None else None

            if length is not None:
                if len(self._buf) < length:
                    # A known record type, but not all of it has arrived. Wait
                    # rather than resyncing - a 1044-byte PSD block spans many
                    # serial reads and treating a partial one as corruption
                    # would throw away a valid record every time.
                    break

                rec = _DECODERS[kind](bytes(self._buf[:length]))
                if rec is not None:
                    self._synced = True
                    del self._buf[:length]
                    self.decoded += 1
                    if isinstance(rec, Packet):
                        self._note_sequence(rec)
                    elif isinstance(rec, MetricsRecord):
                        self.metrics_records += 1
                    else:
                        self.psd_records += 1
                    yield rec
                    continue

                # Right place by sync and type, wrong CRC: a link fault, not a
                # framing loss. Counted separately because the two have
                # completely different causes.
                if self._buf[2] == PROTO_VERSION:
                    self.bad_crc += 1

            if self._synced:
                self.resyncs += 1
                self._synced = False

            # Advance to the next plausible sync position rather than one byte
            # at a time: on a 1 MB backlog of garbage that is the difference
            # between a millisecond and a second. Any record type counts as a
            # candidate, so the scan is for the shared first sync byte.
            nxt = self._buf.find(bytes([SYNC0]), 1)
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
