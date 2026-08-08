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
    CONSOLE_FRAC,
    CONSOLE_REGION_FRAC,
    VRHI_CLEAR_COLOR_RGB8,
    VRHI_SOLID_FALLBACK_RGB8,
    check_console_open_invariants,
    classify_frame,
    count_exact_color,
    largest_exact_color_component,
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


# Synthetic console fixtures. The pinned OpenArena console background
# logo256.tga (512x256, stretched over the top half of the frame) has 22
# unique colors with a dominant dark red (93,23,23) at ~91%; these 21 dark-red
# shades plus black logo regions reproduce that profile.
CONSOLE_LOGO_SHADES = [
    (93, 23, 23), (90, 19, 19), (53, 0, 0), (84, 10, 10), (80, 16, 16),
    (70, 12, 12), (99, 30, 30), (100, 20, 20), (60, 5, 5), (88, 18, 18),
    (95, 25, 25), (75, 14, 14), (86, 12, 12), (92, 22, 22), (64, 8, 8),
    (97, 28, 28), (72, 11, 11), (82, 15, 15), (77, 13, 13), (68, 9, 9),
    (90, 20, 20),
]


def logo_console_region(width: int, height: int) -> bytes:
    """Logo-like console background: 22 unique colors, modal dark red."""
    pixels = bytearray(width * height * 3)
    for index in range(width * height):
        if index % 12 == 0:
            color = (0, 0, 0)
        elif index % 5 == 0:
            color = CONSOLE_LOGO_SHADES[(index // 5) % 20 + 1]
        else:
            color = CONSOLE_LOGO_SHADES[0]
        offset = index * 3
        pixels[offset : offset + 3] = bytes(color)
    return bytes(pixels)


def console_open_frame(
    width: int,
    height: int,
    background: bytes,
    world_color: tuple[int, int, int],
    *,
    boundary_row: int | None = None,
) -> bytes:
    """Synthesize a console-open capture for the invariants unit tests.

    Mirrors Con_DrawSolidConsole: the console background fills the top
    ``CONSOLE_REGION_FRAC`` rows (with sparse white/red/green console-text
    pixels), a 2-pixel red separator line sits at the console boundary, and
    the world fills the rows below it.
    """
    boundary_row = (
        int(CONSOLE_FRAC * height) if boundary_row is None else boundary_row
    )
    region_rows = int(CONSOLE_REGION_FRAC * height)
    if len(background) != region_rows * width * 3:
        raise ValueError("background bytes must cover the console region")
    pixels = bytearray(width * height * 3)
    region = bytearray(background)
    for index in range(region_rows * width):
        offset = index * 3
        if index % 41 == 0:
            region[offset : offset + 3] = bytes((255, 255, 255))
        elif index % 97 == 0:
            region[offset : offset + 3] = bytes((255, 0, 0))
        elif index % 113 == 0:
            region[offset : offset + 3] = bytes((0, 255, 0))
    pixels[: len(region)] = region
    for y in range(boundary_row, min(height, boundary_row + 2)):
        for x in range(width):
            offset = (y * width + x) * 3
            pixels[offset : offset + 3] = bytes((255, 0, 0))
    for y in range(boundary_row + 2, height):
        for x in range(width):
            offset = (y * width + x) * 3
            pixels[offset : offset + 3] = bytes(world_color)
    return bytes(pixels)


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
    def test_counts_renderer_solid_fallback_sentinel(self) -> None:
        rgb = bytes(VRHI_SOLID_FALLBACK_RGB8) * 3 + bytes((1, 2, 3))
        self.assertEqual(count_exact_color(rgb, VRHI_SOLID_FALLBACK_RGB8), 3)
        self.assertEqual(count_exact_color(rgb, (1, 2, 3)), 1)

    def test_largest_exact_color_component_uses_4_connectivity(self) -> None:
        rgb = bytearray(bytes((0, 0, 0)) * 16)
        for pixel in (0, 1, 4, 5, 10):
            rgb[pixel * 3 : pixel * 3 + 3] = bytes(VRHI_SOLID_FALLBACK_RGB8)
        self.assertEqual(
            largest_exact_color_component(
                bytes(rgb), 4, 4, VRHI_SOLID_FALLBACK_RGB8
            ),
            4,
        )

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


class ConsoleInvariantTests(unittest.TestCase):
    """Console-open scene invariants (see check_console_open_invariants)."""

    WIDTH = 640
    HEIGHT = 480

    @staticmethod
    def _region_rows() -> int:
        return int(CONSOLE_REGION_FRAC * ConsoleInvariantTests.HEIGHT)

    def _region(self, builder):
        return builder(self.WIDTH, self._region_rows())

    def _invariants(self, frame: bytes) -> dict[str, object]:
        return check_console_open_invariants(frame, self.WIDTH, self.HEIGHT)

    def test_textured_console_passes_all_invariants(self) -> None:
        # Sentinels below the console (world band) must not pollute the
        # region-scoped checks.
        frame = console_open_frame(
            self.WIDTH,
            self.HEIGHT,
            self._region(logo_console_region),
            world_color=VRHI_SOLID_FALLBACK_RGB8,
        )
        result = self._invariants(frame)
        self.assertTrue(result["pass"])
        metrics = result["metrics"]
        self.assertGreaterEqual(metrics["region_unique_colors"], 22)
        self.assertLessEqual(metrics["region_solid_fallback_fraction"], 0.01)
        self.assertGreaterEqual(metrics["boundary_red_run_fraction"], 0.99)
        self.assertEqual(metrics["boundary_row"], 240)

    def test_solid_fallback_console_fails(self) -> None:
        background = bytes(VRHI_SOLID_FALLBACK_RGB8) * (
            self.WIDTH * self._region_rows()
        )
        frame = console_open_frame(
            self.WIDTH, self.HEIGHT, background, world_color=VRHI_CLEAR_COLOR_RGB8
        )
        result = self._invariants(frame)
        self.assertFalse(result["pass"])
        by_name = {check["name"]: check for check in result["checks"]}
        # The console IS open (separator line present); the background is the
        # solid placeholder, which both region checks must reject.
        self.assertTrue(by_name["console_boundary_line"]["pass"])
        self.assertFalse(by_name["console_background_textured"]["pass"])
        self.assertFalse(by_name["console_background_not_solid_fallback"]["pass"])
        self.assertGreater(result["metrics"]["region_solid_fallback_fraction"], 0.9)
        self.assertLess(result["metrics"]["region_unique_colors"], 16)

    def test_other_flat_color_fallback_fails_textured_check(self) -> None:
        # A future solid fallback of a different color is caught by the
        # color-agnostic texture check even without the sentinel.
        background = bytes((20, 40, 60)) * (self.WIDTH * self._region_rows())
        frame = console_open_frame(
            self.WIDTH, self.HEIGHT, background, world_color=VRHI_CLEAR_COLOR_RGB8
        )
        result = self._invariants(frame)
        self.assertFalse(result["pass"])
        by_name = {check["name"]: check for check in result["checks"]}
        self.assertTrue(by_name["console_background_not_solid_fallback"]["pass"])
        self.assertFalse(by_name["console_background_textured"]["pass"])

    def test_closed_console_fails_boundary_line(self) -> None:
        # Console not open: no separator line, top region is world content.
        background = bytes(VRHI_CLEAR_COLOR_RGB8) * (
            self.WIDTH * self._region_rows()
        )
        frame = console_open_frame(
            self.WIDTH, self.HEIGHT, background, world_color=VRHI_CLEAR_COLOR_RGB8,
            boundary_row=0,
        )
        result = self._invariants(frame)
        self.assertFalse(result["pass"])
        by_name = {check["name"]: check for check in result["checks"]}
        self.assertFalse(by_name["console_boundary_line"]["pass"])

    def test_console_region_metrics_reject_malformed_input(self) -> None:
        from frame_classify import console_region_metrics

        with self.assertRaises(ValueError):
            console_region_metrics(b"\x00" * 4, 2, 2)
        with self.assertRaises(ValueError):
            console_region_metrics(b"\x00" * 12, 0, 4)


if __name__ == "__main__":
    unittest.main()
