from __future__ import annotations

import sys
from pathlib import Path
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))

from frame_classify import (  # noqa: E402
    CLASS_CLEAR_ONLY,
    CLASS_CONTENT,
    CONSOLE_REGION_FRAC,
    VRHI_CLEAR_COLOR_RGB8,
    VRHI_SOLID_FALLBACK_RGB8,
)
from run_image_tests import (  # noqa: E402
    BASE_CVARS,
    Scene,
    apply_console_gate,
    apply_frame_classification,
    apply_scene_gates,
    apply_vrhi_fallback_gate,
    assess_repeatability,
    build_command,
    is_authoritative_capture,
    load_scenes,
    public_result,
    select_scenes,
)
from test_frame_classify import (  # noqa: E402
    console_open_frame,
    logo_console_region,
)


def uniform_frame(
    width: int, height: int, color: tuple[int, int, int] = VRHI_CLEAR_COLOR_RGB8
) -> bytes:
    return bytes(color) * (width * height)


def rect_frame(
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


def classified_result(
    rgb: bytes, width: int, height: int, scene: Scene
) -> dict[str, object]:
    result: dict[str, object] = {
        "width": width,
        "height": height,
        "_normalized_rgb": rgb,
    }
    apply_frame_classification(result, scene)
    return result


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
        plain_maps = [scene for scene in map_scenes if not scene.open_console]
        console_maps = [scene for scene in map_scenes if scene.open_console]
        self.assertTrue(plain_maps)
        self.assertTrue(all(scene.comparison == "capture" for scene in plain_maps))
        # The console regression scene opts into the console policy and
        # explicitly opens the in-game console.
        self.assertTrue(console_maps)
        self.assertTrue(
            all(
                scene.comparison == "console" and scene.open_console
                for scene in console_maps
            )
        )
        demo_scenes = [scene for scene in scenes if scene.source == "demo"]
        self.assertTrue(demo_scenes)
        self.assertTrue(all(scene.comparison == "exact" for scene in demo_scenes))

    def test_console_policy_and_open_console_flag_are_valid(self) -> None:
        scene = Scene(
            name="unit-console",
            source="map",
            target="oa_dm1",
            screenshot="unit-console",
            comparison="console",
            open_console=True,
        )
        self.assertTrue(scene.open_console)
        self.assertEqual(scene.as_dict()["open_console"], True)

    def test_console_scene_action_opens_console_before_capture(self) -> None:
        scene = Scene(
            name="unit-console",
            source="map",
            target="oa_dm1",
            screenshot="unit-console",
            comparison="console",
            open_console=True,
            waits=4,
        )
        command = build_command(Path("ioquake3.exe"), Path("home"), "vrhi", scene)
        action = command[command.index("activeAction") + 1]
        self.assertTrue(action.startswith("toggleconsole;"))
        self.assertIn("screenshot unit-console; wait; wait; quit", action)

        closed = Scene(
            name="unit-map",
            source="map",
            target="oa_dm1",
            screenshot="unit-map",
            comparison="capture",
        )
        other = build_command(Path("ioquake3.exe"), Path("home"), "vrhi", closed)
        self.assertNotIn(
            "toggleconsole", other[other.index("activeAction") + 1]
        )


class ClassificationPolicyTests(unittest.TestCase):
    def test_content_and_clear_only_policies_are_valid(self) -> None:
        Scene(
            name="unit-content",
            source="map",
            target="oa_dm1",
            screenshot="unit-content",
            comparison="content",
        )
        Scene(
            name="unit-clear",
            source="map",
            target="oa_dm1",
            screenshot="unit-clear",
            comparison="clear_only",
        )

    def test_unknown_policy_is_rejected(self) -> None:
        with self.assertRaises(ValueError):
            Scene(
                name="unit-bogus",
                source="map",
                target="oa_dm1",
                screenshot="unit-bogus",
                comparison="sparkly",
            )

    def test_expected_clear_color_is_validated(self) -> None:
        Scene(
            name="unit-ok",
            source="map",
            target="oa_dm1",
            screenshot="unit-ok",
            comparison="clear_only",
            expected_clear_color="9 14 22",
        )
        for bad in ("9 14", "a b c", "300 0 0", "9 14 22 1"):
            with self.assertRaises(ValueError):
                Scene(
                    name="unit-bad",
                    source="map",
                    target="oa_dm1",
                    screenshot="unit-bad",
                    comparison="clear_only",
                    expected_clear_color=bad,
                )

    def test_vrhi_fallback_gate_rejects_visible_green_placeholder(self) -> None:
        result: dict[str, object] = {
            "width": 8,
            "height": 8,
            "_normalized_rgb": bytes((61, 107, 56)) * 64,
        }
        apply_vrhi_fallback_gate(result, "vrhi")
        self.assertIn("error", result)
        self.assertEqual(result["vrhi_solid_fallback_pixels"], 64)
        self.assertEqual(result["vrhi_solid_fallback_largest_component"], 64)

    def test_vrhi_fallback_gate_allows_isolated_matching_pixels(self) -> None:
        rgb = bytearray(bytes(VRHI_CLEAR_COLOR_RGB8) * 64)
        for y in range(8):
            for x in range(8):
                if (x + y) % 2 == 0:
                    pixel = y * 8 + x
                    rgb[pixel * 3 : pixel * 3 + 3] = bytes((61, 107, 56))
        result: dict[str, object] = {"width": 8, "height": 8, "_normalized_rgb": bytes(rgb)}
        apply_vrhi_fallback_gate(result, "vrhi")
        self.assertNotIn("error", result)
        self.assertEqual(result["vrhi_solid_fallback_largest_component"], 1)

    def test_vrhi_fallback_gate_ignores_other_renderers(self) -> None:
        result: dict[str, object] = {
            "width": 10,
            "height": 1,
            "_normalized_rgb": bytes((61, 107, 56)) * 10,
        }
        apply_vrhi_fallback_gate(result, "opengl2")
        self.assertNotIn("error", result)

    def test_apply_frame_classification_labels_clear_only(self) -> None:
        scene = Scene(
            name="unit-clear",
            source="map",
            target="oa_dm1",
            screenshot="unit-clear",
            comparison="clear_only",
        )
        result = classified_result(uniform_frame(8, 8), 8, 8, scene)
        self.assertEqual(result["frame_class"], CLASS_CLEAR_ONLY)
        self.assertTrue(result["frame_classification"]["expected_clear_matches"])

    def test_apply_frame_classification_labels_content(self) -> None:
        scene = Scene(
            name="unit-content",
            source="map",
            target="oa_dm1",
            screenshot="unit-content",
            comparison="content",
        )
        rgb = rect_frame(8, 8, VRHI_CLEAR_COLOR_RGB8, (255, 0, 0), 2, 2, 6, 6)
        result = classified_result(rgb, 8, 8, scene)
        self.assertEqual(result["frame_class"], CLASS_CONTENT)

    def test_apply_frame_classification_flags_wrong_clear_color(self) -> None:
        scene = Scene(
            name="unit-clear",
            source="map",
            target="oa_dm1",
            screenshot="unit-clear",
            comparison="clear_only",
        )
        result = classified_result(uniform_frame(8, 8, (0, 0, 0)), 8, 8, scene)
        self.assertEqual(result["frame_class"], "uniform_other")
        self.assertFalse(result["frame_classification"]["expected_clear_matches"])

    def test_apply_frame_classification_skips_failed_runs(self) -> None:
        scene = Scene(
            name="unit-clear",
            source="map",
            target="oa_dm1",
            screenshot="unit-clear",
            comparison="clear_only",
        )
        result: dict[str, object] = {"error": "engine exited with 1"}
        apply_frame_classification(result, scene)
        self.assertIsNone(result["frame_class"])
        self.assertIsNone(result["frame_classification"])

    def test_content_policy_requires_content_frames(self) -> None:
        scene = Scene(
            name="unit-content",
            source="map",
            target="oa_dm1",
            screenshot="unit-content",
            comparison="content",
        )
        content = classified_result(
            rect_frame(8, 8, VRHI_CLEAR_COLOR_RGB8, (255, 0, 0), 2, 2, 6, 6),
            8,
            8,
            scene,
        )
        cleared = classified_result(uniform_frame(8, 8), 8, 8, scene)
        self.assertTrue(assess_repeatability(scene, [content, content], 2)[0])
        self.assertFalse(assess_repeatability(scene, [content, cleared], 2)[0])
        self.assertFalse(assess_repeatability(scene, [cleared, cleared], 2)[0])

    def test_content_policy_metrics_are_exposed(self) -> None:
        scene = Scene(
            name="unit-content",
            source="map",
            target="oa_dm1",
            screenshot="unit-content",
            comparison="content",
        )
        content = classified_result(
            rect_frame(8, 8, VRHI_CLEAR_COLOR_RGB8, (255, 0, 0), 2, 2, 6, 6),
            8,
            8,
            scene,
        )
        _, metrics = assess_repeatability(scene, [content, content], 2)
        self.assertEqual(len(metrics), 2)
        self.assertEqual(metrics[0]["frame_class"], CLASS_CONTENT)

    def test_clear_only_policy_requires_clear_frames(self) -> None:
        scene = Scene(
            name="unit-clear",
            source="map",
            target="oa_dm1",
            screenshot="unit-clear",
            comparison="clear_only",
        )
        cleared = classified_result(uniform_frame(8, 8), 8, 8, scene)
        content = classified_result(
            rect_frame(8, 8, VRHI_CLEAR_COLOR_RGB8, (255, 0, 0), 2, 2, 6, 6),
            8,
            8,
            scene,
        )
        self.assertTrue(assess_repeatability(scene, [cleared, cleared], 2)[0])
        self.assertFalse(assess_repeatability(scene, [cleared, content], 2)[0])

    def test_clear_only_policy_fails_wrong_uniform_color(self) -> None:
        scene = Scene(
            name="unit-clear",
            source="map",
            target="oa_dm1",
            screenshot="unit-clear",
            comparison="clear_only",
        )
        wrong = classified_result(uniform_frame(8, 8, (61, 107, 56)), 8, 8, scene)
        self.assertEqual(wrong["frame_class"], "uniform_other")
        self.assertFalse(assess_repeatability(scene, [wrong, wrong], 2)[0])

    def test_console_policy_requires_open_console_invariants(self) -> None:
        scene = Scene(
            name="unit-console",
            source="map",
            target="oa_dm1",
            screenshot="unit-console",
            comparison="console",
        )
        passed = {"pass": True, "checks": []}
        failed = {"pass": False, "checks": []}
        self.assertTrue(
            assess_repeatability(
                scene, [{"console_invariants": passed}], 1
            )[0]
        )
        self.assertFalse(
            assess_repeatability(
                scene, [{"console_invariants": failed}], 1
            )[0]
        )
        self.assertFalse(
            assess_repeatability(scene, [{"error": "boom"}], 1)[0]
        )

    def test_apply_console_gate_publishes_metrics_and_passes_texture(self) -> None:
        rows = int(CONSOLE_REGION_FRAC * 480)
        frame = console_open_frame(
            640,
            480,
            logo_console_region(640, rows),
            world_color=(61, 107, 56),
        )
        result: dict[str, object] = {
            "width": 640,
            "height": 480,
            "_normalized_rgb": frame,
        }
        apply_console_gate(result)
        self.assertNotIn("error", result)
        self.assertTrue(result["console_invariants"]["pass"])
        self.assertEqual(result["console_metrics"]["boundary_row"], 240)

    def test_apply_console_gate_fails_solid_fallback_rectangle(self) -> None:
        rows = int(CONSOLE_REGION_FRAC * 480)
        background = bytes((61, 107, 56)) * (640 * rows)
        frame = console_open_frame(
            640, 480, background, world_color=VRHI_CLEAR_COLOR_RGB8
        )
        result: dict[str, object] = {
            "width": 640,
            "height": 480,
            "_normalized_rgb": frame,
        }
        apply_console_gate(result)
        self.assertIn("error", result)
        self.assertIn("console-open invariants failed", str(result["error"]))
        self.assertIn(
            "console_background_textured", str(result["error"])
        )
        self.assertIn(
            "console_background_not_solid_fallback", str(result["error"])
        )

    def test_apply_console_gate_skips_failed_runs(self) -> None:
        result: dict[str, object] = {"error": "engine exited with 1"}
        apply_console_gate(result)
        self.assertNotIn("console_invariants", result)

    def _closed_console_frame(self) -> bytes:
        # A capture with the console closed: no separator line at the console
        # boundary, no textured console region. If the console gate ran on
        # this frame it would fail console_boundary_line and
        # console_background_textured (see test_closed_console_fails_*).
        rows = int(CONSOLE_REGION_FRAC * 480)
        background = bytes(VRHI_CLEAR_COLOR_RGB8) * (640 * rows)
        return console_open_frame(
            640,
            480,
            background,
            world_color=VRHI_CLEAR_COLOR_RGB8,
            boundary_row=0,
        )

    def test_scene_gates_skip_console_checks_for_ordinary_scenes(self) -> None:
        # Regression: run_one invoked apply_console_gate unconditionally, so
        # every ordinary non-console scene failed its open-console invariants.
        # A frame that would fail the console checks must not gain a console
        # error (or any console keys) for capture/content scenes.
        for comparison in ("capture", "content"):
            scene = Scene(
                name=f"unit-{comparison}",
                source="map",
                target="oa_dm1",
                screenshot=f"unit-{comparison}",
                comparison=comparison,
            )
            result: dict[str, object] = {
                "width": 640,
                "height": 480,
                "_normalized_rgb": self._closed_console_frame(),
            }
            apply_scene_gates(result, scene, "vrhi")
            self.assertNotIn("error", result)
            self.assertNotIn("console_invariants", result)
            self.assertNotIn("console_metrics", result)
            # The other gates still ran: the frame is classified and VRHI
            # metrics are published.
            self.assertEqual(result["frame_class"], CLASS_CONTENT)
            self.assertIn("vrhi_solid_fallback_fraction", result)

    def test_scene_gates_console_scene_gets_console_metrics(self) -> None:
        rows = int(CONSOLE_REGION_FRAC * 480)
        frame = console_open_frame(
            640,
            480,
            logo_console_region(640, rows),
            world_color=VRHI_CLEAR_COLOR_RGB8,
        )
        scene = Scene(
            name="unit-console",
            source="map",
            target="oa_dm1",
            screenshot="unit-console",
            comparison="console",
        )
        result: dict[str, object] = {
            "width": 640,
            "height": 480,
            "_normalized_rgb": frame,
        }
        apply_scene_gates(result, scene, "vrhi")
        self.assertNotIn("error", result)
        self.assertTrue(result["console_invariants"]["pass"])
        self.assertEqual(result["console_metrics"]["boundary_row"], 240)

    def test_scene_gates_console_scene_fails_solid_fallback(self) -> None:
        rows = int(CONSOLE_REGION_FRAC * 480)
        background = bytes(VRHI_SOLID_FALLBACK_RGB8) * (640 * rows)
        frame = console_open_frame(
            640, 480, background, world_color=VRHI_CLEAR_COLOR_RGB8
        )
        scene = Scene(
            name="unit-console",
            source="map",
            target="oa_dm1",
            screenshot="unit-console",
            comparison="console",
        )
        result: dict[str, object] = {
            "width": 640,
            "height": 480,
            "_normalized_rgb": frame,
        }
        apply_scene_gates(result, scene, "vrhi")
        self.assertIn("error", result)
        self.assertIn("console-open invariants failed", str(result["error"]))
        self.assertFalse(result["console_invariants"]["pass"])

    def test_classification_policies_fail_on_errored_results(self) -> None:
        scene = Scene(
            name="unit-content",
            source="map",
            target="oa_dm1",
            screenshot="unit-content",
            comparison="content",
        )
        errored: dict[str, object] = {"error": "process timeout"}
        passed, metrics = assess_repeatability(scene, [errored, errored], 2)
        self.assertFalse(passed)
        self.assertEqual(metrics, [])


if __name__ == "__main__":
    unittest.main()
