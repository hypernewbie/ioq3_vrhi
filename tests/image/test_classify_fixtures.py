from __future__ import annotations

import json
import sys
import tempfile
from pathlib import Path
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))

from classify_fixtures import (  # noqa: E402
    PATTERNS,
    build_fixture,
    generate_checker,
    generate_gradient,
    generate_noise,
    generate_rect,
    generate_speck,
    generate_uniform,
    load_manifest,
    run_manifest,
)
from frame_classify import (  # noqa: E402
    CLASS_CLEAR_ONLY,
    CLASS_CONTENT,
    CLASS_UNIFORM_OTHER,
    classify_frame,
)


class FixtureGeneratorTests(unittest.TestCase):
    def test_uniform_has_expected_size_and_color(self) -> None:
        rgb = generate_uniform(4, 3, [9, 14, 22])
        self.assertEqual(len(rgb), 4 * 3 * 3)
        self.assertEqual(rgb, bytes((9, 14, 22)) * 12)

    def test_rect_paints_only_its_region(self) -> None:
        rgb = generate_rect(8, 8, [9, 14, 22], [255, 0, 0], [0.25, 0.25, 0.5, 0.5])
        classification = classify_frame(rgb, width=8, height=8)
        self.assertEqual(classification.frame_class, CLASS_CONTENT)
        self.assertEqual(classification.unique_colors, 2)
        self.assertAlmostEqual(classification.non_modal_fraction, 0.25, places=2)

    def test_speck_is_deterministic_for_a_seed(self) -> None:
        first = generate_speck(16, 16, [9, 14, 22], [255, 255, 255], 5, seed=7)
        second = generate_speck(16, 16, [9, 14, 22], [255, 255, 255], 5, seed=7)
        self.assertEqual(first, second)

    def test_noise_stays_within_amplitude(self) -> None:
        rgb = generate_noise(16, 16, [9, 14, 22], 2, seed=3)
        for offset in range(0, len(rgb), 3):
            for channel in range(3):
                value = rgb[offset + channel]
                self.assertGreaterEqual(value, 0)
                self.assertLessEqual(value, 255)

    def test_gradient_has_many_colors(self) -> None:
        rgb = generate_gradient(32, 1, [9, 14, 22], [255, 255, 255])
        classification = classify_frame(rgb, width=32, height=1)
        self.assertEqual(classification.frame_class, CLASS_CONTENT)
        self.assertGreater(classification.unique_colors, 16)

    def test_checker_uses_two_colors(self) -> None:
        rgb = generate_checker(16, 16, [9, 14, 22], [255, 0, 0], 4)
        classification = classify_frame(rgb, width=16, height=16)
        self.assertEqual(classification.frame_class, CLASS_CONTENT)
        self.assertEqual(classification.unique_colors, 2)

    def test_unknown_pattern_is_rejected(self) -> None:
        self.assertNotIn("hologram", PATTERNS)


class ManifestTests(unittest.TestCase):
    def test_manifest_covers_all_three_classes(self) -> None:
        defaults, fixtures = load_manifest()
        expects = {fixture["expect"] for fixture in fixtures}
        self.assertEqual(
            expects, {CLASS_CLEAR_ONLY, CLASS_UNIFORM_OTHER, CLASS_CONTENT}
        )

    def test_manifest_fixtures_are_unique(self) -> None:
        _, fixtures = load_manifest()
        ids = [fixture["id"] for fixture in fixtures]
        self.assertEqual(len(ids), len(set(ids)))

    def test_build_fixture_rejects_unknown_expect(self) -> None:
        with self.assertRaises(ValueError):
            build_fixture(
                {"id": "x", "pattern": "uniform", "expect": "sparkly"},
                {},
            )

    def test_build_fixture_rejects_bad_color(self) -> None:
        with self.assertRaises(ValueError):
            build_fixture(
                {
                    "id": "x",
                    "pattern": "uniform",
                    "color": [300, 0, 0],
                    "expect": CLASS_CLEAR_ONLY,
                },
                {},
            )

    def test_build_fixture_resolves_defaults_and_overrides(self) -> None:
        fixture = build_fixture(
            {
                "id": "x",
                "pattern": "uniform",
                "color": [0, 0, 0],
                "expect": CLASS_CLEAR_ONLY,
                "expected_clear_color": None,
            },
            {"width": 32, "seed": 5, "expected_clear_color": [9, 14, 22]},
        )
        self.assertEqual(fixture["width"], 32)
        self.assertIsNone(fixture["classify_kwargs"]["expected_clear_color"])
        self.assertEqual(fixture["generator_kwargs"]["seed"], 5)


class RunManifestTests(unittest.TestCase):
    def test_all_fixtures_meet_expected_classes(self) -> None:
        outcome = run_manifest(report_dir=None)
        self.assertTrue(outcome["passed"], [r for r in outcome["results"] if not r["pass"]])
        self.assertGreaterEqual(outcome["total"], 10)

    def test_report_is_written_and_parses(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            outcome = run_manifest(report_dir=Path(directory))
            self.assertIsNotNone(outcome["report_path"])
            report = json.loads(Path(outcome["report_path"]).read_text(encoding="utf-8"))
        self.assertTrue(report["passed"])
        self.assertEqual(report["total"], outcome["total"])

    def test_wrong_expectation_fails_the_run(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            manifest = Path(directory) / "broken.json"
            manifest.write_text(
                json.dumps(
                    {
                        "schema": 1,
                        "fixtures": [
                            {
                                "id": "nope",
                                "pattern": "uniform",
                                "color": [9, 14, 22],
                                "expect": CLASS_CONTENT,
                            }
                        ],
                    }
                ),
                encoding="utf-8",
            )
            outcome = run_manifest(manifest, report_dir=None)
        self.assertFalse(outcome["passed"])
        self.assertEqual(outcome["results"][0]["frame_class"], CLASS_CLEAR_ONLY)


if __name__ == "__main__":
    unittest.main()
