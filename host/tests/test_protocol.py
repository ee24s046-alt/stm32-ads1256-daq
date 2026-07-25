"""Decoder tests, including the cases that matter on a real serial link.

The golden vector here is the same one asserted in tests/test_packet.c on the
firmware side.  Two independent implementations checked against one fixed set
of bytes is the only thing that keeps an encoder and a decoder honest.
"""

import numpy as np
import pytest

from ads1256_host.protocol import (
    PACKET_BYTES,
    Packet,
    StreamDecoder,
    crc16_ccitt,
    decode_packet,
    encode_packet,
)


def test_crc_check_value():
    assert crc16_ccitt(b"123456789") == 0x29B1


def test_golden_vector_matches_firmware():
    pkt = Packet(seq=0x0102, t_us=0x01020304, flags=0x00,
                 codes=(1, 0x7FFFFF, 0, -1))
    wire = encode_packet(pkt)

    assert len(wire) == PACKET_BYTES
    assert wire[0:3] == bytes([0xA5, 0x5A, 0x01])
    assert wire[4:6] == bytes([0x02, 0x01])            # seq LE
    assert wire[6:10] == bytes([0x04, 0x03, 0x02, 0x01])
    assert wire[10:13] == bytes([0x01, 0x00, 0x00])    # ch0 = 1
    assert wire[13:16] == bytes([0xFF, 0xFF, 0x7F])    # ch1 = +FS
    assert wire[19:22] == bytes([0xFF, 0xFF, 0xFF])    # ch3 = -1
    assert int.from_bytes(wire[22:24], "little") == crc16_ccitt(wire[:22])


@pytest.mark.parametrize("codes", [
    (0, 0, 0, 0),
    (1, -1, 2, -2),
    (8388607, -8388608, 8388607, -8388608),
    (123456, -123456, 4194304, -4194304),
])
def test_roundtrip_sign_extension(codes):
    pkt = Packet(seq=7, t_us=999, flags=0x01, codes=codes)
    out = decode_packet(encode_packet(pkt))
    assert out is not None
    assert out.codes == codes


def test_every_single_bit_flip_is_caught():
    wire = bytearray(encode_packet(Packet(seq=5, t_us=6, flags=0, codes=(9, 8, 7, 6))))
    for i in range(len(wire)):
        for bit in range(8):
            corrupted = bytearray(wire)
            corrupted[i] ^= 1 << bit
            assert decode_packet(bytes(corrupted)) is None, (i, bit)


def test_decoder_handles_arbitrary_chunking():
    """A serial port splits reads wherever it likes; framing must not care."""
    packets = [Packet(seq=i, t_us=i * 133, flags=0, codes=(i, -i, 2 * i, -2 * i))
               for i in range(50)]
    stream = b"".join(encode_packet(p) for p in packets)

    for chunk_size in (1, 3, 7, 24, 25, 64, 97, 4096):
        dec = StreamDecoder()
        got = []
        for i in range(0, len(stream), chunk_size):
            got.extend(dec.feed(stream[i:i + chunk_size]))
        assert len(got) == 50, chunk_size
        assert [p.seq for p in got] == list(range(50))
        assert dec.dropped == 0
        assert dec.bad_crc == 0


def test_resync_after_leading_garbage():
    dec = StreamDecoder()
    good = b"".join(encode_packet(Packet(seq=i, t_us=i, flags=0, codes=(0,) * 4))
                    for i in range(5))
    got = list(dec.feed(b"\x11\x22\x33" * 9 + good))
    assert len(got) == 5
    assert dec.in_sync


def test_false_sync_inside_payload_is_rejected():
    """0xA5 0x5A occurs in real 24-bit data; sync alone must not frame on it."""
    dec = StreamDecoder()
    # ch0 = 0x5AA500 puts the bytes 00 A5 5A into the payload, so the sync word
    # appears at offset 11 of every single frame.
    stream = b"".join(
        encode_packet(Packet(seq=i, t_us=i * 133, flags=0,
                             codes=(0x5AA500, 0, 0, 0)))
        for i in range(4)
    )
    got = list(dec.feed(stream))
    assert len(got) == 4
    assert all(p.codes[0] == 0x5AA500 for p in got)
    assert dec.resyncs == 0
    assert dec.dropped == 0


def test_sequence_gap_is_counted():
    dec = StreamDecoder()
    frames = [encode_packet(Packet(seq=i, t_us=i, flags=0, codes=(0,) * 4))
              for i in range(10)]
    del frames[4]                          # lose one packet entirely
    list(dec.feed(b"".join(frames)))
    assert dec.dropped == 1


def test_sequence_wraps_at_65535():
    dec = StreamDecoder()
    seqs = [65534, 65535, 0, 1]
    stream = b"".join(encode_packet(Packet(seq=s, t_us=0, flags=0, codes=(0,) * 4))
                      for s in seqs)
    list(dec.feed(stream))
    assert dec.dropped == 0                # the wrap is not a gap


def test_corrupt_frame_counted_as_crc_not_resync_storm():
    dec = StreamDecoder()
    frames = [bytearray(encode_packet(Packet(seq=i, t_us=i, flags=0, codes=(0,) * 4)))
              for i in range(6)]
    frames[3][12] ^= 0x80                  # corrupt one payload byte
    list(dec.feed(b"".join(bytes(f) for f in frames)))
    assert dec.bad_crc >= 1
    assert dec.decoded == 5


def test_decoder_does_not_grow_without_bound():
    dec = StreamDecoder(max_buffer=4096)
    for _ in range(100):
        list(dec.feed(b"\x00" * 1024))
    assert len(dec._buf) <= 4096
