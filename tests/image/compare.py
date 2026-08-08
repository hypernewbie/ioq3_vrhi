#!/usr/bin/env python3
"""Small, dependency-free image helpers for the external ioq3 tests."""

from __future__ import annotations

import hashlib
from pathlib import Path


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def load_tga_rgb(path: Path) -> tuple[int, int, bytes]:
    """Read an uncompressed 24-bit TGA and normalize it to top-left RGB8."""
    data = path.read_bytes()
    if len(data) < 18:
        raise ValueError(f"TGA is shorter than its header: {path}")

    header = data[:18]
    id_length = header[0]
    color_map_type = header[1]
    image_type = header[2]
    width = int.from_bytes(header[12:14], "little")
    height = int.from_bytes(header[14:16], "little")
    pixel_depth = header[16]
    descriptor = header[17]

    if color_map_type != 0 or image_type != 2:
        raise ValueError(f"unsupported TGA type in {path}: {image_type}")
    if pixel_depth != 24:
        raise ValueError(f"expected 24-bit TGA in {path}, got {pixel_depth}")
    if width <= 0 or height <= 0:
        raise ValueError(f"invalid TGA dimensions in {path}: {width}x{height}")

    pixels_start = 18 + id_length
    row_bytes = width * 3
    pixels_end = pixels_start + row_bytes * height
    if pixels_end != len(data):
        raise ValueError(
            f"unexpected TGA size in {path}: {len(data)} != {pixels_end}"
        )

    top_origin = bool(descriptor & 0x20)
    right_origin = bool(descriptor & 0x10)
    rows = range(height) if top_origin else range(height - 1, -1, -1)
    normalized = bytearray(width * height * 3)
    output = 0

    for y in rows:
        row = data[pixels_start + y * row_bytes : pixels_start + (y + 1) * row_bytes]
        xs = range(width - 1, -1, -1) if right_origin else range(width)
        for x in xs:
            pixel = x * 3
            # ioquake3 writes BGR TGA pixels; the contract used by tests is RGB.
            normalized[output : output + 3] = row[pixel + 2], row[pixel + 1], row[pixel]
            output += 3

    return width, height, bytes(normalized)


def compare_exact(expected: bytes, actual: bytes) -> dict[str, int | bool]:
    differing = sum(left != right for left, right in zip(expected, actual))
    differing += abs(len(expected) - len(actual))
    return {
        "equal": expected == actual,
        "expected_bytes": len(expected),
        "actual_bytes": len(actual),
        "different_bytes": differing,
    }
