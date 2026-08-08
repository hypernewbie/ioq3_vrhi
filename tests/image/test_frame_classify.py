from __future__ import annotations

import sys
import tempfile
from pathlib import Path
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))

from frame_classify import (  # noqa: E402
    CLASS_CLEAR_ONLY,
    CLASS_CONTENT,
    CLASS_UNIFORM_OTHER,
    VRHI_CLEAR_COLOR_RGB8,
    classify_frame,
    classify_tga,
    parse_expected_clear_color,
)


def uniform(width: int, height: int, color: tuple[int, int, int]) -> bytes:
    return bytes(color) * (width * height)


def speck(
    width: int,
    height: int,
    background: tuple[int, int, int],
    color: tuple[int, int, int],
    count: int,
) -> bytes:
    pixels = bytearray(width * height * 3)
    for offset in range(0, len(pixels), 3):
        pixels[offset : offset + 3] = bytes(background)
    for index in range(min(count, width * height)):
        offset = index * 3
        pixels[offset : offset + 3] = bytes(color)
    return bytes(pixels)


def rect(
    width: int,
    height: int,
    background: tuple[int, int, int],
    color: tuple[int, int, int],
    x0: int,
    y0: int,
    x1: int,
    y1: int,
) -> bytes:
    pixels = bytearray(width * height * 3)
    for offset in range(0, len(pixels), 3):
        pixels[offset : offset + 3] = bytes(background)
    for y in range(y0, min(y1, height)):
        for x in range(x0, min(x1, width)):
            offset = (y * width + x) * 3
            pixels[offset : offset + 3] = bytes(color)
    return bytes(pixels)


def write_tga(path: Path, width: int, height: int, rgb: bytes) -> None:
    """Write a tiny uncompressed 24-bit top-left BGR TGA from RGB pixels."""
    header = bytearray(18)
    header[2] = 2
    header[12:14] = width.to_bytes(2, "little")
    header[14:16] = height.to_bytes(2, "little")
    header[16] = 24
    header[17] = 0x20
    rows = bytearray()
    for y in range(height):
        for x in range(width):
            pixel = (y * width + x) * 3
            rows.extend((rgb[pixel + 2], rgb[pixel + 1], rgb[pixel]))
    path.write_bytes(bytes(header) + bytes(rows))


class ParseExpectedClearColorTests(unittest.TestCase):
    def test_accepts_valid_rgb_string(self) -> None:
        self.assertEqual(parse_expected_clear_color("9 14 22"), (9, 14, 22))

    def test_rejects_wrong_field_count(self) -> None:
        with self.assertRaises(ValueError):
            parse_expected_clear_color("9 14")

    def test_rejects_non_integers(self) -> None:
        with self.assertRaises(ValueError):
            parse_expected_clear_color("a b c")

    def test_rejects_out_of_range_channels(self) -> None:
        with self.assertRaises(ValueError):
            parse_expected_clear_color("300 0 0")


class ClassifyFrameTests(unittest.TestCase):
    def test_uniform_vrhi_clear_is_clear_only(self) -> None:
        result = classify_frame(
            uniform(8, 8, VRHI_CLEAR_COLOR_RGB8), width=8, height=8
        )
        self.assertEqual(result.frame_class, CLASS_CLEAR_ONLY)
        self.assertTrue(result.expected_clear_matches)
        self.assertEqual(result.unique_colors, 1)
        self.assertEqual(result.modal_color, VRHI_CLEAR_COLOR_RGB8)
        self.assertEqual(result.non_modal_fraction, 0.0)
        self.assertEqual(result.significant_fraction, 0.0)

    def test_uniform_other_color_is_uniform_other(self) -> None:
        # The current solid-color world shader fill is (0.24, 0.42, 0.22).
        result = classify_frame(
            uniform(8, 8, (61, 107, 56)), width=8, height=8
        )
        self.assertEqual(result.frame_class, CLASS_UNIFORM_OTHER)
        self.assertFalse(result.expected_clear_matches)

    def test_uniform_black_without_expectation_is_clear_only(self) -> None:
        result = classify_frame(
            uniform(4, 4, (0, 0, 0)),
            width=4,
            height=4,
            expected_clear_color=None,
        )
        self.assertEqual(result.frame_class, CLASS_CLEAR_ONLY)
        self.assertTrue(result.expected_clear_matches)

    def test_override_expected_clear_color_flips_class(self) -> None:
        result = classify_frame(
            uniform(4, 4, (9, 14, 22)),
            width=4,
            height=4,
            expected_clear_color=(255, 0, 0),
        )
        self.assertEqual(result.frame_class, CLASS_UNIFORM_OTHER)

    def test_small_amplitude_noise_stays_clear_only(self) -> None:
        base = VRHI_CLEAR_COLOR_RGB8
        pixels = bytearray()
        for index in range(8 * 8):
            pixels.extend(
                (
                    base[0] + (index % 3) - 1,
                    base[1] + ((index // 3) % 3) - 1,
                    base[2] + ((index // 9) % 3) - 1,
                )
            )
        result = classify_frame(bytes(pixels), width=8, height=8)
        self.assertEqual(result.frame_class, CLASS_CLEAR_ONLY)
        self.assertEqual(result.reason, "near-uniform")

    def test_large_amplitude_noise_is_content(self) -> None:
        base = VRHI_CLEAR_COLOR_RGB8
        pixels = bytearray()
        for index in range(8 * 8):
            pixels.extend(
                (
                    (base[0] + index * 7) % 256,
                    (base[1] + index * 13) % 256,
                    (base[2] + index * 29) % 256,
                )
            )
        result = classify_frame(bytes(pixels), width=8, height=8)
        self.assertEqual(result.frame_class, CLASS_CONTENT)

    def test_ui_rect_is_content(self) -> None:
        pixels = rect(
            16, 16, VRHI_CLEAR_COLOR_RGB8, (255, 0, 0), 4, 4, 12, 12
        )
        result = classify_frame(pixels, width=16, height=16)
        self.assertEqual(result.frame_class, CLASS_CONTENT)
        self.assertEqual(result.unique_colors, 2)
        self.assertAlmostEqual(result.non_modal_fraction, 0.25, places=3)
        self.assertTrue(result.expected_clear_matches)

    def test_tiny_speck_stays_clear_only(self) -> None:
        pixels = speck(64, 64, VRHI_CLEAR_COLOR_RGB8, (255, 255, 255), 1)
        result = classify_frame(pixels, width=64, height=64)
        self.assertEqual(result.frame_class, CLASS_CLEAR_ONLY)

    def test_speck_above_threshold_is_content(self) -> None:
        pixels = speck(64, 64, VRHI_CLEAR_COLOR_RGB8, (255, 255, 255), 400)
        result = classify_frame(pixels, width=64, height=64)
        self.assertEqual(result.frame_class, CLASS_CONTENT)

    def test_custom_threshold_can_promote_speck(self) -> None:
        pixels = speck(64, 64, VRHI_CLEAR_COLOR_RGB8, (255, 255, 255), 10)
        result = classify_frame(
            pixels, width=64, height=64, max_non_modal_fraction=0.01
        )
        self.assertEqual(result.frame_class, CLASS_CLEAR_ONLY)
        strict = classify_frame(
            pixels, width=64, height=64, max_non_modal_fraction=0.0001
        )
        self.assertEqual(strict.frame_class, CLASS_CONTENT)

    def test_gradient_is_content(self) -> None:
        pixels = bytearray()
        for index in range(64):
            pixels.extend((index * 4 % 256, index * 2 % 256, index % 256))
        result = classify_frame(bytes(pixels), width=64, height=1)
        self.assertEqual(result.frame_class, CLASS_CONTENT)

    def test_checker_is_content(self) -> None:
        pixels = bytearray()
        for index in range(16 * 16):
            color = (255, 0, 0) if (index // 16 + index % 16) % 2 else (0, 0, 0)
            pixels.extend(bytes(color))
        result = classify_frame(bytes(pixels), width=16, height=16)
        self.assertEqual(result.frame_class, CLASS_CONTENT)

    def test_one_by_one_uniform(self) -> None:
        result = classify_frame(
            bytes(VRHI_CLEAR_COLOR_RGB8), width=1, height=1
        )
        self.assertEqual(result.frame_class, CLASS_CLEAR_ONLY)
        self.assertEqual(result.pixel_count, 1)

    def test_empty_input_rejected(self) -> None:
        with self.assertRaises(ValueError):
            classify_frame(b"")

    def test_non_multiple_of_three_rejected(self) -> None:
        with self.assertRaises(ValueError):
            classify_frame(b"\x09\x0e")

    def test_invalid_thresholds_rejected(self) -> None:
        pixels = uniform(2, 2, VRHI_CLEAR_COLOR_RGB8)
        with self.assertRaises(ValueError):
            classify_frame(pixels, max_channel_spread=-1)
        with self.assertRaises(ValueError):
            classify_frame(pixels, max_non_modal_fraction=1.5)

    def test_as_dict_is_json_serializable(self) -> None:
        import json

        result = classify_frame(
            uniform(2, 2, VRHI_CLEAR_COLOR_RGB8), width=2, height=2
        )
        payload = json.dumps(result.as_dict())
        self.assertIn(CLASS_CLEAR_ONLY, payload)


class ClassifyTgaTests(unittest.TestCase):
    def test_classify_tga_roundtrip(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "capture.tga"
            write_tga(path, 4, 4, uniform(4, 4, VRHI_CLEAR_COLOR_RGB8))
            classification, sha256 = classify_tga(path)
        self.assertEqual(classification.frame_class, CLASS_CLEAR_ONLY)
        self.assertEqual(len(sha256), 64)

    def test_classify_tga_content_roundtrip(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "ui.tga"
            pixels = rect(
                8, 8, VRHI_CLEAR_COLOR_RGB8, (255, 255, 255), 2, 2, 6, 6
            )
            write_tga(path, 8, 8, pixels)
            classification, _ = classify_tga(path)
        self.assertEqual(classification.frame_class, CLASS_CONTENT)


if __name__ == "__main__":
    unittest.main()
