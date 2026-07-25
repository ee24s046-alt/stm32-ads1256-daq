"""Sources of packet bytes: a serial port, a raw capture file, or a simulator.

The simulator exists so that every host-side code path - framing, resync, gap
accounting, the noise analysis, the CLI - can be tested and demonstrated with
no hardware attached.  That is also what lets CI exercise the decoder.
"""

from __future__ import annotations

import time
from abc import ABC, abstractmethod
from pathlib import Path
from typing import Iterator, Optional

import numpy as np

from .protocol import Packet, StreamDecoder, encode_packet

# One complete 4-channel scan every 532 us: the ADS1256 delivers a settled
# conversion every 133 us at DRATE = 30 kSPS (4 filter settling periods), and
# four of those make one scan.  1875 scans/s.
SCAN_PERIOD_US = 532
DEFAULT_BAUD = 921600         # ignored by CDC, but pyserial wants a number


class ByteSource(ABC):
    @abstractmethod
    def read(self, max_bytes: int) -> bytes:
        ...

    def close(self) -> None:
        pass

    def write(self, data: bytes) -> None:
        raise NotImplementedError("this source is read-only")


class SerialSource(ByteSource):
    """USB CDC virtual COM port.

    Note on ``timeout``: a short read timeout is what keeps the capture loop
    responsive to Ctrl-C without polling.  ``in_waiting`` is checked first so a
    healthy stream is drained in large reads rather than one timeout-sized
    chunk at a time.
    """

    def __init__(self, port: str, timeout: float = 0.1) -> None:
        import serial  # imported lazily: the decoder must work without pyserial

        self._port = serial.Serial(port, DEFAULT_BAUD, timeout=timeout)

    def read(self, max_bytes: int) -> bytes:
        waiting = self._port.in_waiting
        return self._port.read(max(1, min(max_bytes, waiting or 1)))

    def write(self, data: bytes) -> None:
        self._port.write(data)

    def close(self) -> None:
        self._port.close()


class FileSource(ByteSource):
    """Replay a raw capture."""

    def __init__(self, path: Path) -> None:
        self._fh = open(path, "rb")

    def read(self, max_bytes: int) -> bytes:
        return self._fh.read(max_bytes)

    def close(self) -> None:
        self._fh.close()


class SimulatedSource(ByteSource):
    """Synthetic device: band-limited noise plus an optional tone.

    Deliberately imperfect.  ``drop_every`` skips a sequence number and
    ``corrupt_every`` flips a bit, so that the host's gap detection and CRC
    rejection are exercised by the same code path that handles real data
    rather than by a special test-only branch.
    """

    def __init__(
        self,
        noise_code_rms: float = 14.6,
        tone_hz: float = 0.0,
        tone_code_amplitude: float = 0.0,
        drop_every: int = 0,
        corrupt_every: int = 0,
        seed: int = 0,
    ) -> None:
        self._rng = np.random.default_rng(seed)
        self._noise = noise_code_rms
        self._tone_hz = tone_hz
        self._tone_amp = tone_code_amplitude
        self._drop_every = drop_every
        self._corrupt_every = corrupt_every
        self._seq = 0
        self._t_us = 0
        self._pending = bytearray()
        self._emitted = 0

    def _next_packet(self) -> bytes:
        self._emitted += 1
        if self._drop_every and (self._emitted % self._drop_every == 0):
            self._seq = (self._seq + 1) & 0xFFFF     # skip one, do not send it
            self._t_us += SCAN_PERIOD_US

        t_s = self._t_us * 1e-6
        codes = []
        for ch in range(4):
            value = self._rng.normal(0.0, self._noise)
            if self._tone_amp:
                value += self._tone_amp * np.sin(2 * np.pi * self._tone_hz * t_s
                                                 + ch * np.pi / 4)
            codes.append(int(round(value)))

        frame = bytearray(encode_packet(Packet(seq=self._seq, t_us=self._t_us,
                                               flags=0x01, codes=tuple(codes))))
        if self._corrupt_every and (self._emitted % self._corrupt_every == 0):
            frame[11] ^= 0x01

        self._seq = (self._seq + 1) & 0xFFFF
        self._t_us = (self._t_us + SCAN_PERIOD_US) & 0xFFFFFFFF
        return bytes(frame)

    def read(self, max_bytes: int) -> bytes:
        while len(self._pending) < max_bytes:
            self._pending += self._next_packet()
        out = bytes(self._pending[:max_bytes])
        del self._pending[:max_bytes]
        return out


def read_packets(
    source: ByteSource,
    limit: Optional[int] = None,
    duration_s: Optional[float] = None,
    chunk: int = 4096,
    raw_sink=None,
) -> Iterator[Packet]:
    """Decode packets from ``source`` until a limit is reached.

    ``raw_sink`` receives the undecoded bytes.  Saving the raw stream rather
    than only the decoded samples matters: if the decoder later turns out to be
    wrong, a raw capture can be re-analysed, whereas decoded samples have
    already had the mistake baked in.
    """
    decoder = StreamDecoder()
    started = time.monotonic()
    count = 0

    while True:
        if duration_s is not None and (time.monotonic() - started) >= duration_s:
            return
        data = source.read(chunk)
        if not data:
            if isinstance(source, FileSource):
                return
            continue
        if raw_sink is not None:
            raw_sink.write(data)

        for pkt in decoder.feed(data):
            yield pkt
            count += 1
            if limit is not None and count >= limit:
                return


def packets_to_arrays(packets):
    """Collect an iterable of packets into (codes[n,4], t_us[n], seq[n], flags[n])."""
    codes, times, seqs, flags = [], [], [], []
    for pkt in packets:
        codes.append(pkt.codes)
        times.append(pkt.t_us)
        seqs.append(pkt.seq)
        flags.append(pkt.flags)
    return (
        np.asarray(codes, dtype=np.int64).reshape(-1, 4),
        np.asarray(times, dtype=np.int64),
        np.asarray(seqs, dtype=np.int64),
        np.asarray(flags, dtype=np.int64),
    )
