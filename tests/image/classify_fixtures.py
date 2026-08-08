#!/usr/bin/env python3
"""Controlled image-harness manifest runner for clear-only vs content frames.

This is the dependency-free counterpart of the engine image suite: it reads
``tests/image/clear_content_fixtures.json``, synthesizes each fixture's RGB8
frame in memory, classifies it with ``frame_classify.classify_frame``, and
asserts the expected class. No engine, Vulkan, or GL is involved, so the
boundary between clear-only output and renderer-owned world/UI output is
pinned by plain unit-testable data.

Runtime reports are written below the ignored ``temp/image-classify/`` tree.
"""

from __future__ import annotations

import argparse
import json
import os
import random
import sys
import time
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
ROOT = SCRIPT_DIR.parents[1]
DEFAULT_MANIFEST = SCRIPT_DIR / "clear_content_fixtures.json"
DEFAULT_REPORT_ROOT = ROOT / "temp" / "image-classify"

try:
    from frame_classify import CLASSES, classify_frame
except ImportError:  # pragma: no cover - module import fallback
    from .frame_classify import CLASSES, classify_frame

# Manifest keys that configure classification or identity rather than the
# pattern generator; every other key becomes a generator argument.
RESERVED_KEYS = frozenset(
    {
        "id",
        "pattern",
        "expect",
        "description",
        "width",
        "height",
        "expected_clear_color",
        "max_channel_spread",
        "max_non_modal_fraction",
        "max_clear_deviation",
    }
)


def rgb_color(value: object, label: str) -> tuple[int, int, int]:
    if not isinstance(value, list) or len(value) != 3:
        raise ValueError(f"{label} must be an [r, g, b] list, got {value!r}")
    channels = []
    for channel in value:
        if not isinstance(channel, int) or isinstance(channel, bool):
            raise ValueError(f"{label} channels must be integers, got {value!r}")
        channels.append(channel)
    if any(channel < 0 or channel > 255 for channel in channels):
        raise ValueError(f"{label} channels must be in 0..255, got {value!r}")
    return (channels[0], channels[1], channels[2])


def generate_uniform(
    width: int, height: int, color: list[int], seed: int = 0
) -> bytes:
    del seed  # uniform frames need no randomness
    return bytes(rgb_color(color, "color")) * (width * height)


def generate_rect(
    width: int,
    height: int,
    background: list[int],
    color: list[int],
    rect: list[float],
    seed: int = 0,
) -> bytes:
    del seed  # rectangles are deterministic
    background_rgb = rgb_color(background, "background")
    color_rgb = rgb_color(color, "color")
    if not isinstance(rect, list) or len(rect) != 4:
        raise ValueError(f"rect must be [fx, fy, fw, fh], got {rect!r}")
    try:
        fx, fy, fw, fh = (float(value) for value in rect)
    except (TypeError, ValueError) as error:
        raise ValueError(f"rect values must be numbers, got {rect!r}") from error
    if fw <= 0.0 or fh <= 0.0:
        raise ValueError(f"rect width/height must be positive, got {rect!r}")
    x0 = max(0, min(width - 1, int(round(fx * width))))
    y0 = max(0, min(height - 1, int(round(fy * height))))
    x1 = max(x0 + 1, min(width, int(round((fx + fw) * width))))
    y1 = max(y0 + 1, min(height, int(round((fy + fh) * height))))
    pixels = bytearray(width * height * 3)
    for offset in range(0, len(pixels), 3):
        pixels[offset : offset + 3] = bytes(background_rgb)
    for y in range(y0, y1):
        for x in range(x0, x1):
            offset = (y * width + x) * 3
            pixels[offset : offset + 3] = bytes(color_rgb)
    return bytes(pixels)


def generate_speck(
    width: int,
    height: int,
    background: list[int],
    color: list[int],
    count: int,
    seed: int,
) -> bytes:
    background_rgb = rgb_color(background, "background")
    color_rgb = rgb_color(color, "color")
    if not isinstance(count, int) or count < 0:
        raise ValueError(f"speck count must be a non-negative integer, got {count!r}")
    pixels = bytearray(width * height * 3)
    for offset in range(0, len(pixels), 3):
        pixels[offset : offset + 3] = bytes(background_rgb)
    chosen = set(
        random.Random(seed).sample(range(width * height), min(count, width * height))
    )
    for index in chosen:
        offset = index * 3
        pixels[offset : offset + 3] = bytes(color_rgb)
    return bytes(pixels)


def generate_noise(
    width: int,
    height: int,
    background: list[int],
    amplitude: int,
    seed: int,
) -> bytes:
    background_rgb = rgb_color(background, "background")
    if not isinstance(amplitude, int) or amplitude < 0:
        raise ValueError(
            f"noise amplitude must be a non-negative integer, got {amplitude!r}"
        )
    rng = random.Random(seed)
    pixels = bytearray()
    for _ in range(width * height):
        for channel in range(3):
            value = background_rgb[channel] + rng.randint(-amplitude, amplitude)
            pixels.append(max(0, min(255, value)))
    return bytes(pixels)


def generate_gradient(
    width: int,
    height: int,
    from_color: list[int],
    to_color: list[int],
    seed: int = 0,
) -> bytes:
    del seed  # gradients are deterministic
    start = rgb_color(from_color, "from")
    end = rgb_color(to_color, "to")
    pixels = bytearray()
    for index in range(width * height):
        t = index / max(1, width * height - 1)
        pixels.extend(
            round(start[channel] + (end[channel] - start[channel]) * t)
            for channel in range(3)
        )
    return bytes(pixels)


def generate_checker(
    width: int,
    height: int,
    a: list[int],
    b: list[int],
    cell: int,
    seed: int = 0,
) -> bytes:
    del seed  # checkers are deterministic
    color_a = rgb_color(a, "a")
    color_b = rgb_color(b, "b")
    if not isinstance(cell, int) or cell < 1:
        raise ValueError(f"checker cell must be a positive integer, got {cell!r}")
    pixels = bytearray()
    for index in range(width * height):
        x = index % width
        y = index // width
        color = color_a if (x // cell + y // cell) % 2 == 0 else color_b
        pixels.extend(bytes(color))
    return bytes(pixels)


PATTERNS = {
    "uniform": generate_uniform,
    "rect": generate_rect,
    "speck": generate_speck,
    "noise": generate_noise,
    "gradient": generate_gradient,
    "checker": generate_checker,
}


def build_fixture(item: dict[str, object], defaults: dict[str, object]) -> dict[str, object]:
    """Validate one manifest fixture and resolve generator/classifier inputs."""
    fixture_id = item.get("id")
    if not isinstance(fixture_id, str) or not fixture_id:
        raise ValueError("each fixture needs a non-empty string 'id'")
    pattern = item.get("pattern")
    if pattern not in PATTERNS:
        raise ValueError(f"fixture {fixture_id!r}: unknown pattern {pattern!r}")
    expect = item.get("expect")
    if expect not in CLASSES:
        raise ValueError(f"fixture {fixture_id!r}: unknown expect {expect!r}")

    width = item.get("width", defaults.get("width", 64))
    height = item.get("height", defaults.get("height", 64))
    if (
        not isinstance(width, int)
        or not isinstance(height, int)
        or width < 1
        or height < 1
    ):
        raise ValueError(
            f"fixture {fixture_id!r}: width/height must be positive integers"
        )

    generator_kwargs = {
        str(key): value for key, value in item.items() if key not in RESERVED_KEYS
    }
    if "seed" not in generator_kwargs and "seed" in defaults:
        generator_kwargs["seed"] = defaults["seed"]
    # The gradient manifest uses 'from'/'to'; map them to parameter names
    # because 'from' is a Python keyword.
    if pattern == "gradient":
        for old_key, new_key in (("from", "from_color"), ("to", "to_color")):
            if old_key in generator_kwargs:
                generator_kwargs[new_key] = generator_kwargs.pop(old_key)
    # Validate color-like generator inputs at build time so manifest typos
    # fail loudly instead of surfacing later during generation.
    for key, value in generator_kwargs.items():
        if isinstance(value, list) and len(value) == 3:
            rgb_color(value, key)

    classify_kwargs: dict[str, object] = {}
    if "expected_clear_color" in item:
        expected = item["expected_clear_color"]
        classify_kwargs["expected_clear_color"] = (
            rgb_color(expected, "expected_clear_color") if expected is not None else None
        )
    elif "expected_clear_color" in defaults:
        classify_kwargs["expected_clear_color"] = rgb_color(
            defaults["expected_clear_color"], "defaults.expected_clear_color"
        )
    for key in (
        "max_channel_spread",
        "max_non_modal_fraction",
        "max_clear_deviation",
    ):
        if key in item:
            classify_kwargs[key] = item[key]

    return {
        "id": fixture_id,
        "pattern": pattern,
        "expect": expect,
        "description": str(item.get("description", "")),
        "width": width,
        "height": height,
        "generator_kwargs": generator_kwargs,
        "classify_kwargs": classify_kwargs,
    }


def load_manifest(
    path: Path = DEFAULT_MANIFEST,
) -> tuple[dict[str, object], list[dict[str, object]]]:
    payload = json.loads(path.read_text(encoding="utf-8"))
    if payload.get("schema") != 1:
        raise ValueError(f"unsupported fixture manifest schema in {path}")
    defaults = payload.get("defaults", {})
    if not isinstance(defaults, dict):
        raise ValueError(f"fixture manifest defaults must be an object in {path}")
    fixtures = payload.get("fixtures", [])
    if not isinstance(fixtures, list) or not fixtures:
        raise ValueError(f"fixture manifest has no fixtures: {path}")
    return defaults, [build_fixture(item, defaults) for item in fixtures]


def run_manifest(
    manifest_path: Path = DEFAULT_MANIFEST,
    report_dir: Path | None = None,
) -> dict[str, object]:
    """Run every fixture and return {passed, total, results, report_path}."""
    defaults, fixtures = load_manifest(manifest_path)
    results: list[dict[str, object]] = []
    for fixture in fixtures:
        generator = PATTERNS[fixture["pattern"]]
        rgb = generator(
            fixture["width"], fixture["height"], **fixture["generator_kwargs"]
        )
        classification = classify_frame(
            rgb,
            width=fixture["width"],
            height=fixture["height"],
            **fixture["classify_kwargs"],
        )
        metrics = classification.as_dict()
        passed = metrics["frame_class"] == fixture["expect"]
        results.append(
            {
                "id": fixture["id"],
                "pattern": fixture["pattern"],
                "description": fixture["description"],
                "expect": fixture["expect"],
                "pass": passed,
                "frame_class": metrics["frame_class"],
                "metrics": metrics,
            }
        )

    passed = all(result["pass"] for result in results)
    report: dict[str, object] = {
        "schema": 1,
        "manifest": str(manifest_path.resolve()),
        "passed": passed,
        "total": len(results),
        "fixtures": results,
    }
    report_path: Path | None = None
    if report_dir is not None:
        report_dir.mkdir(parents=True, exist_ok=True)
        report_path = report_dir / "report.json"
        report_path.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    return {
        "passed": passed,
        "total": len(results),
        "results": results,
        "report_path": report_path,
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--manifest", type=Path, default=DEFAULT_MANIFEST
    )
    parser.add_argument(
        "--report-dir",
        type=Path,
        default=None,
        help="override the report directory (default: temp/image-classify/<ts>)",
    )
    parser.add_argument(
        "--no-report", action="store_true", help="do not write a report file"
    )
    args = parser.parse_args(argv)

    report_dir: Path | None = None
    if not args.no_report:
        report_dir = (
            args.report_dir.expanduser().resolve()
            if args.report_dir is not None
            else DEFAULT_REPORT_ROOT
            / (time.strftime("%Y%m%d-%H%M%S") + f"-{os.getpid()}")
        )
    outcome = run_manifest(args.manifest, report_dir=report_dir)
    for result in outcome["results"]:
        marker = "PASS" if result["pass"] else "FAIL"
        print(
            f"[classify-fixtures] {marker}: {result['id']} "
            f"expect={result['expect']} got={result['frame_class']}"
        )
        if not result["pass"]:
            print(f"[classify-fixtures]   {result['description']}")
            print(f"[classify-fixtures]   {result['metrics']['reason']}")
    if outcome["report_path"] is not None:
        print(f"[classify-fixtures] report: {outcome['report_path']}")
    if outcome["passed"]:
        print(
            f"[classify-fixtures] PASS: {outcome['total']} fixtures "
            "met their expected classes"
        )
        return 0
    print(
        f"[classify-fixtures] FAIL: {outcome['total']} fixtures run, "
        "one or more expected classes were not met"
    )
    return 1


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (ValueError, OSError) as error:
        print(f"[classify-fixtures] ERROR: {error}", file=sys.stderr)
        raise SystemExit(2)

