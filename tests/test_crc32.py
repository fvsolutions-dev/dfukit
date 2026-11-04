"""The CRC-32 of an image: the standard's, as zlib computes it, also in pieces."""

import zlib

from simulator import image

import dfukit


def test_check_value() -> None:
    assert dfukit.crc32(b"") == 0
    assert dfukit.crc32(b"123456789") == 0xCBF43926


def test_in_pieces() -> None:
    first = dfukit.crc32(b"1234")
    assert dfukit.crc32(b"56789", first) == 0xCBF43926


def test_same_as_zlib() -> None:
    data = image(100_000)
    assert dfukit.crc32(data) == zlib.crc32(data)
