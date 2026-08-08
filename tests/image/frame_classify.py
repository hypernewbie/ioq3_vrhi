#!/usr/bin/env python3
"""Dependency-free classification of normalized RGB8 frames.

The renderer slice plan needs a cheap, GPU-free oracle that tells a
"clear-only" capture (uniform backbuffer, nothing renderer-owned drawn) apart
from a capture that contains world or UI output. This module classifies
normalized top-left RGB8 pixel bytes with no third-party dependency and no
Vulkan/GL requirement, so the whole classification layer runs in unit tests.

Classes
-------
``clear_only``
    The frame is uniform (or within a small noise envelope) and its dominant
    color matches the expected clear color. Nothing renderer-owned was drawn;
    only capture noise may be present.

``uniform_other``
    The frame is uniform (or within the noise envelope) but its dominant color
    is *not* the expected clear color. This is the signature of a full-frame
    flat-color draw (for example the current solid-color world shader fill), a
    changed clear color, or uninitialized output. It is deliberately never
    treated as ``clear_only``.

``content``
    A meaningful fraction of pixels deviates from the dominant color beyond the
    noise envelope. World geometry, UI rectangles, gradients, or texture output
    is present.

The default expected clear color is the renderer_vrhi clear color
(0.035, 0.055, 0.085) normalized to RGB8 (9, 14, 22), matching
VRHI_BeginFrame in code/renderervrhi/vrhi_stub.cpp. Pass
``expected_clear_color=None`` to disable the expectation; any uniform frame
then classifies as ``clear_only``.
"""

from __future__ import annotations

import argparse
import json
from dataclasses import dataclass
from pathlib import Path

CLASS_CLEAR_ONLY = "clear_only"
CLASS_UNIFORM_OTHER = "uniform_other"
CLASS_CONTENT = "content"
CLASSES = (CLASS_CLEAR_ONLY, CLASS_UNIFORM_OTHER, CLASS_CONTENT)

# renderer_vrhi VRHI_BeginFrame clears to (0.035f, 0.055f, 0.085f);
# normalized to RGB8 this is (9, 14, 22).
VRHI_CLEAR_COLOR_RGB8 = (9, 14, 22)

# VRHI_WorldSolidPixelSource returns float4(0.24, 0.42, 0.22, 1.0),
# which is captured as this exact RGB8 color on the current backbuffer. It is
# a renderer-owned placeholder, not acceptable material output.
VRHI_SOLID_FALLBACK_RGB8 = (61, 107, 56)

# In-game console scene geometry (ioq3 cl_console.c Con_DrawSolidConsole):
# ``toggleconsole`` sets KEYCATCH_CONSOLE, Con_RunConsole ramps
# con.displayFrac to 0.5 (half screen) at scr_conspeed, and
# Con_DrawSolidConsole draws the console background pic over the top
# ``frac * SCREEN_HEIGHT`` rows, then a 2-pixel red separator line
# (SCR_FillRect with color (1,0,0,1)) at ``frac * SCREEN_HEIGHT``, then the
# scrollback text above it. The OpenArena 'console' shader
# (scripts/oanew.shader: ``console { { map textures/sfx/logo256.tga } }``)
# resolves through the script-stage table; when the alias falls back, the
# background becomes the renderer's solid placeholder rectangle
# (VRHI_SOLID_FALLBACK_RGB8).
CONSOLE_FRAC = 0.5
# Console background region inspected for texture content: top rows below the
# separator line (excludes the red line, the version string, and the world
# rendered below the console).
CONSOLE_REGION_FRAC = 0.45
# The pinned OpenArena console background logo256.tga (512x256, stretched
# over the top half) has 22 unique colors with a dominant dark red (93,23,23)
# at ~91%. A solid-rectangle fallback leaves only the fill color plus a
# handful of console-text colors (~10 or fewer), so a 16-color floor cleanly
# separates a real texture from any flat fallback fill.
CONSOLE_MIN_REGION_UNIQUE_COLORS = 16
# A solid-rectangle fallback covers the console region with the renderer
# sentinel (VRHI_SOLID_FALLBACK_RGB8); the real logo texture has none of it.
CONSOLE_MAX_SOLID_FALLBACK_FRACTION = 0.5
# The 2-pixel red separator line must span most of the width; this is the
# machine-readable proof that the console was open (and fully ramped) when
# the capture was taken.
CONSOLE_MIN_RED_RUN_FRACTION = 0.4


def count_exact_color(rgb: bytes, color: tuple[int, int, int]) -> int:
    """Count exact RGB8 pixels without image-library dependencies."""
    if len(rgb) % 3:
        raise ValueError(f"frame byte count {len(rgb)} is not a multiple of 3")
    target = bytes(color)
    return sum(rgb[offset : offset + 3] == target for offset in range(0, len(rgb), 3))


def largest_exact_color_component(
    rgb: bytes, width: int, height: int, color: tuple[int, int, int]
) -> int:
    """Return the largest 4-connected exact-RGB8 component in a frame."""
    if width <= 0 or height <= 0 or len(rgb) != width * height * 3:
        raise ValueError("RGB8 bytes do not match positive frame dimensions")
    target = bytes(color)
    visited = bytearray(width * height)
    largest = 0
    for start in range(width * height):
        if visited[start] or rgb[start * 3 : start * 3 + 3] != target:
            continue
        visited[start] = 1
        pending = [start]
        component = 0
        while pending:
            index = pending.pop()
            component += 1
            x = index % width
            for neighbor in (
                index - width if index >= width else -1,
                index + width if index < width * (height - 1) else -1,
                index - 1 if x else -1,
                index + 1 if x + 1 < width else -1,
            ):
                if neighbor < 0 or visited[neighbor]:
                    continue
                if rgb[neighbor * 3 : neighbor * 3 + 3] != target:
                    continue
                visited[neighbor] = 1
                pending.append(neighbor)
        largest = max(largest, component)
    return largest


def _is_red_pixel(rgb: bytes, offset: int) -> bool:
    """Loose red predicate for the console separator line pixels."""
    return rgb[offset] >= 180 and rgb[offset + 1] <= 90 and rgb[offset + 2] <= 90


def _longest_horizontal_red_run(rgb: bytes, row: int, width: int) -> int:
    """Longest contiguous run of red pixels along one row."""
    best = 0
    run = 0
    for x in range(width):
        offset = (row * width + x) * 3
        if _is_red_pixel(rgb, offset):
            run += 1
            if run > best:
                best = run
        else:
            run = 0
    return best


def console_region_metrics(
    rgb: bytes,
    width: int,
    height: int,
    *,
    console_frac: float = CONSOLE_FRAC,
    region_frac: float = CONSOLE_REGION_FRAC,
) -> dict[str, object]:
    """Measure the console region of a normalized top-left RGB8 frame.

    The console region is the top ``region_frac`` of the frame (the console
    background pic area below the separator line); the boundary scan looks
    for the red separator line in the rows around ``console_frac * height``.
    Raises ValueError for malformed dimensions.
    """
    if width <= 0 or height <= 0 or len(rgb) != width * height * 3:
        raise ValueError("RGB8 bytes do not match positive frame dimensions")
    region_rows = max(0, int(region_frac * height))
    counts: dict[tuple[int, int, int], int] = {}
    sentinel = 0
    for offset in range(0, region_rows * width * 3, 3):
        pixel = (rgb[offset], rgb[offset + 1], rgb[offset + 2])
        counts[pixel] = counts.get(pixel, 0) + 1
        if pixel == VRHI_SOLID_FALLBACK_RGB8:
            sentinel += 1
    region_pixels = region_rows * width
    modal_color: tuple[int, int, int] | None = None
    modal_count = 0
    if counts:
        modal_color = max(counts, key=counts.__getitem__)
        modal_count = counts[modal_color]
    boundary_row = int(console_frac * height)
    best_run = 0
    # The separator line is drawn at rows boundary_row..boundary_row+1;
    # scan one row either side for rasterization tolerance. Rows below the
    # console (world content) are deliberately excluded so world red pixels
    # cannot masquerade as the separator.
    for row in range(max(0, boundary_row - 1), min(height, boundary_row + 2)):
        best_run = max(best_run, _longest_horizontal_red_run(rgb, row, width))
    return {
        "console_frac": console_frac,
        "region_rows": region_rows,
        "region_pixels": region_pixels,
        "region_unique_colors": len(counts),
        "region_modal_color": list(modal_color) if modal_color else None,
        "region_modal_fraction": (
            round(modal_count / region_pixels, 9) if region_pixels else 0.0
        ),
        "region_solid_fallback_pixels": sentinel,
        "region_solid_fallback_fraction": (
            round(sentinel / region_pixels, 9) if region_pixels else 0.0
        ),
        "boundary_row": boundary_row,
        "boundary_red_run_pixels": best_run,
        "boundary_red_run_fraction": round(best_run / width, 9) if width else 0.0,
    }


def check_console_open_invariants(
    rgb: bytes,
    width: int,
    height: int,
    **kwargs: float,
) -> dict[str, object]:
    """Machine-check that a capture shows the standard in-game console open.

    Three console-specific invariants, each stronger than a whole-frame
    'image is content' classification:

    - ``console_boundary_line``: a red separator run spanning at least
      CONSOLE_MIN_RED_RUN_FRACTION of the width at the console boundary. This
      is the proof the console was actually open (and fully ramped) when the
      capture was taken; a closed console has no such line.
    - ``console_background_textured``: the console region holds at least
      CONSOLE_MIN_REGION_UNIQUE_COLORS distinct colors. The pinned OpenArena
      background logo256.tga has 22; a solid-rectangle fallback of any color
      leaves only the fill plus a few console-text colors.
    - ``console_background_not_solid_fallback``: the renderer solid placeholder
      (VRHI_SOLID_FALLBACK_RGB8) covers at most
      CONSOLE_MAX_SOLID_FALLBACK_FRACTION of the console region. This directly
      encodes "the console script alias did not fall back to a solid
      rectangle" and is scoped to the console region, not the whole frame.

    Returns ``{"pass": bool, "checks": [...], "metrics": {...}}``.
    """
    metrics = console_region_metrics(rgb, width, height, **kwargs)
    red_run = metrics["boundary_red_run_fraction"]
    unique = metrics["region_unique_colors"]
    sentinel = metrics["region_solid_fallback_fraction"]
    checks = [
        {
            "name": "console_boundary_line",
            "pass": red_run >= CONSOLE_MIN_RED_RUN_FRACTION,
            "metric": "boundary_red_run_fraction",
            "value": red_run,
            "threshold": CONSOLE_MIN_RED_RUN_FRACTION,
            "reason": (
                f"red separator run is {red_run:.3f} of the width "
                f"(need >= {CONSOLE_MIN_RED_RUN_FRACTION:.1f}); the console "
                "must be open and fully ramped at capture time"
            ),
        },
        {
            "name": "console_background_textured",
            "pass": unique >= CONSOLE_MIN_REGION_UNIQUE_COLORS,
            "metric": "region_unique_colors",
            "value": unique,
            "threshold": CONSOLE_MIN_REGION_UNIQUE_COLORS,
            "reason": (
                f"console region has {unique} unique colors "
                f"(need >= {CONSOLE_MIN_REGION_UNIQUE_COLORS}); a "
                "solid-rectangle console alias fallback leaves only the fill "
                "color plus a few text colors"
            ),
        },
        {
            "name": "console_background_not_solid_fallback",
            "pass": sentinel <= CONSOLE_MAX_SOLID_FALLBACK_FRACTION,
            "metric": "region_solid_fallback_fraction",
            "value": sentinel,
            "threshold": CONSOLE_MAX_SOLID_FALLBACK_FRACTION,
            "reason": (
                f"console region is {sentinel:.3f} renderer solid-fallback "
                f"color (need <= {CONSOLE_MAX_SOLID_FALLBACK_FRACTION:.1f}); "
                "the console background must not be the solid placeholder "
                "rectangle"
            ),
        },
    ]
    return {
        "pass": all(check["pass"] for check in checks),
        "checks": checks,
        "metrics": metrics,
    }


# Noise envelope: pixels closer than this (per channel) to the dominant color
# are considered part of the clear; larger deviations count as drawn content.
DEFAULT_MAX_CHANNEL_SPREAD = 3
# A frame may ignore a tiny fraction of deviating pixels (stuck pixels,
# one-pixel artifacts) and still be considered clear-only.
DEFAULT_MAX_NON_MODAL_FRACTION = 0.001
# Tolerance for matching the dominant color against the expected clear color.
DEFAULT_MAX_CLEAR_DEVIATION = 3


@dataclass(frozen=True)
class FrameClassification:
    """Result of classifying one normalized top-left RGB8 frame."""

    frame_class: str
    width: int | None
    height: int | None
    pixel_count: int
    unique_colors: int
    modal_color: tuple[int, int, int] | None
    non_modal_fraction: float
    significant_fraction: float
    max_channel_spread: int
    expected_clear_color: tuple[int, int, int] | None
    expected_clear_matches: bool
    reason: str

    def as_dict(self) -> dict[str, object]:
        return {
            "frame_class": self.frame_class,
            "width": self.width,
            "height": self.height,
            "pixel_count": self.pixel_count,
            "unique_colors": self.unique_colors,
            "modal_color": list(self.modal_color) if self.modal_color else None,
            "non_modal_fraction": round(self.non_modal_fraction, 9),
            "significant_fraction": round(self.significant_fraction, 9),
            "max_channel_spread": self.max_channel_spread,
            "expected_clear_color": (
                list(self.expected_clear_color) if self.expected_clear_color else None
            ),
            "expected_clear_matches": self.expected_clear_matches,
            "reason": self.reason,
        }


def parse_expected_clear_color(text: str) -> tuple[int, int, int]:
    """Parse an 'R G B' string into an RGB8 tuple, e.g. '9 14 22'."""
    parts = text.split()
    if len(parts) != 3:
        raise ValueError(f"expected clear color must be 'R G B', got {text!r}")
    try:
        values = (int(parts[0]), int(parts[1]), int(parts[2]))
    except ValueError as error:
        raise ValueError(
            f"expected clear color channels must be integers: {text!r}"
        ) from error
    if any(value < 0 or value > 255 for value in values):
        raise ValueError(
            f"expected clear color channels must be in 0..255: {text!r}"
        )
    return values


def classify_frame(
    rgb: bytes,
    *,
    width: int | None = None,
    height: int | None = None,
    expected_clear_color: tuple[int, int, int] | None = VRHI_CLEAR_COLOR_RGB8,
    max_channel_spread: int = DEFAULT_MAX_CHANNEL_SPREAD,
    max_non_modal_fraction: float = DEFAULT_MAX_NON_MODAL_FRACTION,
    max_clear_deviation: int = DEFAULT_MAX_CLEAR_DEVIATION,
) -> FrameClassification:
    """Classify normalized top-left RGB8 bytes.

    ``rgb`` must contain ``width * height * 3`` bytes in RGB order. Raises
    ValueError for empty or non-RGB8 input and for invalid thresholds.
    """
    if not rgb:
        raise ValueError("empty frame has no pixels to classify")
    if len(rgb) % 3:
        raise ValueError(f"frame byte count {len(rgb)} is not a multiple of 3")
    if max_channel_spread < 0 or max_clear_deviation < 0:
        raise ValueError("spread/deviation thresholds must be non-negative")
    if not 0.0 <= max_non_modal_fraction <= 1.0:
        raise ValueError("max_non_modal_fraction must be in [0.0, 1.0]")

    pixel_count = len(rgb) // 3
    counts: dict[tuple[int, int, int], int] = {}
    channel_min = [255, 255, 255]
    channel_max = [0, 0, 0]
    for offset in range(0, len(rgb), 3):
        pixel = (rgb[offset], rgb[offset + 1], rgb[offset + 2])
        counts[pixel] = counts.get(pixel, 0) + 1
        for channel in range(3):
            value = pixel[channel]
            if value < channel_min[channel]:
                channel_min[channel] = value
            if value > channel_max[channel]:
                channel_max[channel] = value

    unique_colors = len(counts)
    modal_color = max(counts, key=counts.__getitem__)
    non_modal_fraction = (pixel_count - counts[modal_color]) / pixel_count

    significant = 0
    for offset in range(0, len(rgb), 3):
        pixel = (rgb[offset], rgb[offset + 1], rgb[offset + 2])
        deviation = max(abs(pixel[c] - modal_color[c]) for c in range(3))
        if deviation > max_channel_spread:
            significant += 1
    significant_fraction = significant / pixel_count
    observed_spread = max(channel_max[c] - channel_min[c] for c in range(3))

    near_uniform = significant_fraction <= max_non_modal_fraction

    expected_matches = True
    if expected_clear_color is not None:
        expected_matches = (
            max(abs(modal_color[c] - expected_clear_color[c]) for c in range(3))
            <= max_clear_deviation
        )

    if near_uniform:
        if expected_clear_color is None or expected_matches:
            frame_class = CLASS_CLEAR_ONLY
            reason = "uniform" if unique_colors == 1 else "near-uniform"
        else:
            frame_class = CLASS_UNIFORM_OTHER
            reason = "uniform color differs from the expected clear color"
    else:
        frame_class = CLASS_CONTENT
        reason = (
            f"{significant}/{pixel_count} pixels deviate from the dominant "
            "color beyond the noise envelope"
        )

    return FrameClassification(
        frame_class=frame_class,
        width=width,
        height=height,
        pixel_count=pixel_count,
        unique_colors=unique_colors,
        modal_color=modal_color,
        non_modal_fraction=non_modal_fraction,
        significant_fraction=significant_fraction,
        max_channel_spread=observed_spread,
        expected_clear_color=expected_clear_color,
        expected_clear_matches=expected_matches,
        reason=reason,
    )


def classify_tga(
    path: Path,
    **kwargs: object,
) -> tuple[FrameClassification, str]:
    """Load a 24-bit TGA capture, normalize it, and classify it.

    Returns ``(classification, sha256_of_normalized_rgb)``. Raises ValueError
    for unsupported TGA files. ``kwargs`` are forwarded to ``classify_frame``.
    """
    try:
        from compare import load_tga_rgb, sha256_bytes
    except ImportError:  # pragma: no cover - module import fallback
        from .compare import load_tga_rgb, sha256_bytes

    width, height, normalized = load_tga_rgb(path)
    classification = classify_frame(
        normalized, width=width, height=height, **kwargs
    )
    return classification, sha256_bytes(normalized)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Classify a TGA capture as clear_only, uniform_other, "
        "or content without any GPU."
    )
    parser.add_argument("tga", help="path to a 24-bit top-left TGA capture")
    parser.add_argument(
        "--expected-clear-color",
        metavar="R G B",
        help="override the expected clear color, e.g. '9 14 22'",
    )
    parser.add_argument(
        "--no-expected-clear-color",
        action="store_true",
        help="do not compare the dominant color against an expected clear color",
    )
    args = parser.parse_args(argv)

    expected: tuple[int, int, int] | None = VRHI_CLEAR_COLOR_RGB8
    if args.no_expected_clear_color:
        expected = None
    elif args.expected_clear_color:
        expected = parse_expected_clear_color(args.expected_clear_color)

    classification, sha256 = classify_tga(
        Path(args.tga), expected_clear_color=expected
    )
    output = classification.as_dict()
    output["sha256"] = sha256
    print(json.dumps(output, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
