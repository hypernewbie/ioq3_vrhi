from __future__ import annotations

import sys
from pathlib import Path
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))

from run_image_tests import (  # noqa: E402
    BASE_CVARS,
    Scene,
    assess_repeatability,
    build_command,
    is_authoritative_capture,
    load_scenes,
    public_result,
    select_scenes,
)


class RunnerConfigTests(unittest.TestCase):
    def test_manifest_contains_smoke_and_stress_scenes(self) -> None:
        scenes = load_scenes()
        tiers = {scene.tier for scene in scenes}
        self.assertIn("smoke", tiers)
        self.assertIn("stress", tiers)
        self.assertGreaterEqual(len(scenes), 10)
        self.assertEqual(scenes[0].comparison, "exact")
        self.assertEqual(scenes[1].comparison, "capture")

    def test_command_disables_audio_and_uses_safe_frame_script(self) -> None:
        scene = Scene(
            name="unit-scene",
            source="demo",
            target="unit_demo",
            screenshot="unit-scene",
        )
        command = build_command(
            Path("ioquake3.exe"), Path("home"), "opengl2", scene
        )
        self.assertEqual(command[0], "ioquake3.exe")
        self.assertIn("s_initsound", command)
        self.assertIn("0", command[command.index("s_initsound") + 1 :])
        self.assertIn("activeAction", command)
        action = command[command.index("activeAction") + 1]
        self.assertIn("screenshot unit-scene", action)
        self.assertTrue(action.endswith("quit"))
        self.assertIn("+demo", command)
        self.assertEqual(command[command.index("+demo") + 1], "unit_demo")

    def test_vrhi_action_waits_after_renderer_capture(self) -> None:
        scene = Scene(
            name="unit-vrhi",
            source="demo",
            target="unit_demo",
            screenshot="unit-vrhi",
        )
        command = build_command(Path("ioquake3.exe"), Path("home"), "vrhi", scene)
        action = command[command.index("activeAction") + 1]
        # Cbuf_Execute runs twice per engine frame, so a single wait can be
        # consumed by the same frame that processes "screenshot". Two waits
        # guarantee an EndFrame between the capture ticket and quit.
        self.assertIn("screenshot unit-vrhi; wait; wait; quit", action)
        self.assertNotIn("screenshot unit-vrhi; wait; quit", action)
        self.assertNotIn("screenshot unit-vrhi; quit", action)

    def test_map_scene_uses_devmap_for_pinned_viewpos(self) -> None:
        scene = Scene(
            name="unit-map",
            source="map",
            target="oa_dm1",
            screenshot="unit-map",
            viewpos="1 2 3 90",
        )
        command = build_command(Path("ioquake3.exe"), Path("home"), "opengl2", scene)
        self.assertIn("+devmap", command)
        self.assertEqual(command[command.index("+devmap") + 1], "oa_dm1")
        self.assertNotIn("+demo", command)
        action = command[command.index("activeAction") + 1]
        self.assertTrue(action.startswith("setviewpos 1 2 3 90;"))

    def test_scene_cvars_override_defaults(self) -> None:
        scene = Scene(
            name="unit-scene",
            source="demo",
            target="unit_demo",
            screenshot="unit-scene",
            cvars={"r_mode": "4", "timedemo": "0"},
        )
        command = build_command(Path("ioquake3.exe"), Path("home"), "opengl2", scene)
        r_mode_indices = [i for i, value in enumerate(command) if value == "r_mode"]
        timedemo_indices = [i for i, value in enumerate(command) if value == "timedemo"]
        self.assertEqual(command[r_mode_indices[-1] + 1], "4")
        self.assertEqual(command[timedemo_indices[-1] + 1], "0")
        self.assertEqual(BASE_CVARS["s_initsound"], "0")

    def test_vrhi_renderer_marks_capture_as_authoritative(self) -> None:
        self.assertTrue(is_authoritative_capture("vrhi"))
        self.assertFalse(is_authoritative_capture("opengl2"))
        self.assertFalse(is_authoritative_capture("opengl1"))

    def test_assess_repeatability_capture_policy_skips_hashes(self) -> None:
        scene = Scene(
            name="unit-capture",
            source="map",
            target="oa_dm1",
            screenshot="unit-capture",
            comparison="capture",
        )
        results = [{"normalized_rgb_sha256": "abc"}]
        passed, metrics = assess_repeatability(scene, results, 1)
        self.assertTrue(passed)
        self.assertEqual(metrics, [])

    def test_assess_repeatability_rejects_error_results(self) -> None:
        scene = Scene(name="unit-exact", source="demo", target="unit_demo",
                      screenshot="unit-exact")
        passed, metrics = assess_repeatability(scene, [{"error": "boom"}], 1)
        self.assertFalse(passed)
        self.assertEqual(metrics, [])

    def test_assess_repeatability_exact_policy_requires_identical_hashes(self) -> None:
        scene = Scene(name="unit-exact", source="demo", target="unit_demo",
                      screenshot="unit-exact")
        first = {"normalized_rgb_sha256": "abc"}
        second = {"normalized_rgb_sha256": "abd"}
        passed, metrics = assess_repeatability(scene, [first, second], 2)
        self.assertFalse(passed)
        self.assertEqual(metrics, [])
        passed, _ = assess_repeatability(scene, [first, first], 2)
        self.assertTrue(passed)

    def test_public_result_strips_raw_pixel_bytes(self) -> None:
        result = {"index": 1, "_normalized_rgb": b"\x00\x01\x02"}
        public = public_result(result)
        self.assertNotIn("_normalized_rgb", public)
        self.assertEqual(public["index"], 1)

    def test_scene_rejects_unsafe_names(self) -> None:
        with self.assertRaises(ValueError):
            Scene(name="bad/name", source="demo", target="unit_demo",
                  screenshot="unit-demo")
        with self.assertRaises(ValueError):
            Scene(name="unit-demo", source="demo", target="unit_demo",
                  screenshot="bad name")

    def test_scene_rejects_invalid_viewpos(self) -> None:
        with self.assertRaises(ValueError):
            Scene(name="unit-map", source="map", target="oa_dm1",
                  screenshot="unit-map", viewpos="1 2 3")
        with self.assertRaises(ValueError):
            Scene(name="unit-map", source="map", target="oa_dm1",
                  screenshot="unit-map", viewpos="1 2 3 nope")

    def test_scene_rejects_unsupported_source_and_comparison(self) -> None:
        with self.assertRaises(ValueError):
            Scene(name="unit-map", source="singleplayer", target="oa_dm1",
                  screenshot="unit-map")
        with self.assertRaises(ValueError):
            Scene(name="unit-map", source="map", target="oa_dm1",
                  screenshot="unit-map", comparison="fuzzy")

    def test_select_scenes_rejects_unknown_name(self) -> None:
        scenes = load_scenes()
        with self.assertRaises(ValueError):
            select_scenes(scenes, ["does-not-exist"], None)

    def test_load_scenes_defaults_map_to_capture_comparison(self) -> None:
        scenes = load_scenes()
        map_scenes = [scene for scene in scenes if scene.source == "map"]
        self.assertTrue(map_scenes)
        self.assertTrue(all(scene.comparison == "capture" for scene in map_scenes))
        demo_scenes = [scene for scene in scenes if scene.source == "demo"]
        self.assertTrue(demo_scenes)
        self.assertTrue(all(scene.comparison == "exact" for scene in demo_scenes))


if __name__ == "__main__":
    unittest.main()
