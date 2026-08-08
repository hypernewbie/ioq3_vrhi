from __future__ import annotations

import sys
from pathlib import Path
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))

from run_image_tests import (  # noqa: E402
    BASE_CVARS,
    Scene,
    build_command,
    load_scenes,
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


if __name__ == "__main__":
    unittest.main()
