"""Host-side tooling for the STM32F407 + ADS1256 data-acquisition system.

The decoder here is the mirror image of firmware/lib/packet.c.  Both are
checked against the same golden vector (tests/test_packet.c on the C side,
tests/test_protocol.py here), which is what keeps them from drifting apart.
"""

from .protocol import (
    PACKET_BYTES,
    Packet,
    StreamDecoder,
    crc16_ccitt,
    decode_packet,
    encode_packet,
)
from .analysis import NoiseResult, analyse_noise, code_to_volts

__all__ = [
    "PACKET_BYTES",
    "Packet",
    "StreamDecoder",
    "crc16_ccitt",
    "decode_packet",
    "encode_packet",
    "NoiseResult",
    "analyse_noise",
    "code_to_volts",
]

__version__ = "1.0.0"
