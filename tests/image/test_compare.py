from __future__ import annotations

import sys
import tempfile
from pathlib import Path
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))

from compare import compare_exact, compare_tolerant, load_tga_rgb  # noqa: E402


def write_tga(path: Path, width: int, height: int, rgb: bytes, descriptor: int) -> None:
    """Write a tiny uncompressed BGR TGA from top-left RGB pixels."""
    row_bytes = width * 3
    rows = []
    y_values = range(height) if descriptor & 0x20 else range(height - 1, -1, -1)
    for y in y_values:
        x_values = range(width - 1, -1, -1) if descriptor & 0x10 else range(width)
        row = bytearray()
        for x in x_values:
            pixel = (y * width + x) * 3
            row.extend((rgb[pixel + 2], rgb[pixel + 1], rgb[pixel]))
        assert len(row) == row_bytes
        rows.append(bytes(row))

    header = bytearray(18)
    header[2] = 2
    header[12:14] = width.to_bytes(2, "little")
    header[14:16] = height.to_bytes(2, "little")
    header[16] = 24
    header[17] = descriptor
    path.write_bytes(bytes(header) + b"".join(rows))


class CompareTests(unittest.TestCase):
    def test_normalizes_bottom_left_bgr(self) -> None:
        expected = bytes(
            (
                255,
                0,
                0,
                0,
                255,
                0,
                0,
                0,
                255,
                255,
                255,
                255,
            )
        )
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "bottom-left.tga"
            write_tga(path, 2, 2, expected, 0)
            width, height, actual = load_tga_rgb(path)
        self.assertEqual((width, height), (2, 2))
        self.assertEqual(actual, expected)

    def test_normalizes_top_right_origin(self) -> None:
        expected = bytes(
            (
                1,
                2,
                3,
                4,
                5,
                6,
                7,
                8,
                9,
                10,
                11,
                12,
            )
        )
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "top-right.tga"
            write_tga(path, 2, 2, expected, 0x30)
            _, _, actual = load_tga_rgb(path)
        self.assertEqual(actual, expected)

    def test_exact_comparison_reports_differences(self) -> None:
        result = compare_exact(b"abc", b"abd")
        self.assertFalse(result["equal"])
        self.assertEqual(result["different_bytes"], 1)

    def test_tolerant_comparison_exposes_metrics(self) -> None:
        result = compare_tolerant(bytes((10, 10, 10)), bytes((12, 10, 10)))
        self.assertTrue(result["pass"])
        self.assertEqual(result["max_channel_error"], 2)
        self.assertEqual(result["bad_pixels"], 0)

    def test_rejects_non_24_bit_tga(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "rgba.tga"
            header = bytearray(18)
            header[2] = 2
            header[12:14] = (1).to_bytes(2, "little")
            header[14:16] = (1).to_bytes(2, "little")
            header[16] = 32
            path.write_bytes(bytes(header) + b"\0\0\0\0")
            with self.assertRaises(ValueError):
                load_tga_rgb(path)


if __name__ == "__main__":
    unittest.main()
