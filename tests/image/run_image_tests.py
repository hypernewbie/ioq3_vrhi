#!/usr/bin/env python3
"""Run isolated, hidden-window OpenArena image smoke tests.

Captures come from the active renderer's own ``screenshot`` command. The GL
renderers use their stock screenshot paths; renderer_vrhi registers a
renderer-owned command that reads the bound backbuffer in EndFrame and writes a
validated 24-bit BGR TGA, which is that slice's authoritative final-present
capture. Runs under the ``vrhi`` renderer therefore report
``authoritative_capture: true``; GL runs still prove asset provisioning,
renderer startup, demo playback, screenshot output, and teardown, but are not a
final-present oracle.
"""

from __future__ import annotations

import argparse
import ctypes
from ctypes import wintypes
from dataclasses import dataclass, field
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import time

try:
    from frame_classify import (
        CLASS_CLEAR_ONLY,
        CLASS_CONTENT,
        VRHI_CLEAR_COLOR_RGB8,
        classify_frame,
        parse_expected_clear_color,
    )
except ImportError:  # pragma: no cover - module import fallback
    from .frame_classify import (
        CLASS_CLEAR_ONLY,
        CLASS_CONTENT,
        VRHI_CLEAR_COLOR_RGB8,
        classify_frame,
        parse_expected_clear_color,
    )

SCRIPT_DIR = Path(__file__).resolve().parent
ROOT = SCRIPT_DIR.parents[1]
ASSET_ROOT = ROOT / "temp" / "assets" / "openarena-0.8.8"
DEFAULT_MANIFEST = SCRIPT_DIR / "scenes.json"
DEFAULT_RUN_ROOT = ROOT / "temp" / "image-tests"
DEFAULT_TIMEOUT = 90.0

SCREENSHOT_RELATIVE = Path("baseoa") / "screenshots"
SAFE_NAME = re.compile(r"^[A-Za-z0-9_-]+$")
ACTIVE_ACTION_TEMPLATE = "{waits}; screenshot {screenshot}; quit"

# These are deliberately explicit in every test process. In particular, do
# not rely on a user's q3config.cfg for memory, audio, window, or timing state.
BASE_CVARS: dict[str, str] = {
    "s_initsound": "0",
    "s_volume": "0",
    "s_muteWhenUnfocused": "1",
    "sv_cheats": "1",
    "com_hunkMegs": "512",
    "com_zoneMegs": "64",
    "com_maxfps": "0",
    "fixedtime": "50",
    "timescale": "1",
    "cl_timeNudge": "0",
    "logfile": "2",
    "r_fullscreen": "0",
    "r_mode": "3",
    "r_swapInterval": "0",
    "r_ignorehwgamma": "1",
    "r_ext_multisample": "0",
    "r_picmip": "0",
    "cg_draw2D": "0",
    "cg_drawGun": "0",
    "cg_drawCrosshair": "0",
    "cg_brassTime": "0",
    "cg_marks": "0",
    "cg_gibs": "0",
    "timedemo": "1",
}


@dataclass(frozen=True)
class Scene:
    name: str
    source: str
    target: str
    screenshot: str
    description: str = ""
    tier: str = "extended"
    comparison: str = "exact"
    waits: int = 8
    viewpos: str | None = None
    cvars: dict[str, str] = field(default_factory=dict)
    expected_clear_color: str | None = None

    def __post_init__(self) -> None:
        for label, value in (("scene", self.name), ("screenshot", self.screenshot)):
            if not SAFE_NAME.fullmatch(value):
                raise ValueError(f"unsafe {label} name: {value!r}")
        if self.source not in {"demo", "map", "devmap"}:
            raise ValueError(f"unsupported scene source: {self.source!r}")
        if self.comparison not in {
            "exact",
            "tolerant",
            "capture",
            "content",
            "clear_only",
        }:
            raise ValueError(f"unsupported comparison policy: {self.comparison!r}")
        if not self.target or "/" in self.target or "\\" in self.target:
            raise ValueError(f"unsafe {self.source} target: {self.target!r}")
        if self.waits < 0 or self.waits > 1000:
            raise ValueError(f"invalid wait count for {self.name}: {self.waits}")
        if self.viewpos is not None:
            values = self.viewpos.split()
            if len(values) != 4:
                raise ValueError(f"viewpos must have x y z yaw: {self.viewpos!r}")
            try:
                [float(value) for value in values]
            except ValueError as error:
                raise ValueError(f"invalid viewpos: {self.viewpos!r}") from error
        if self.expected_clear_color is not None:
            # Raises ValueError for malformed or out-of-range 'R G B' text.
            parse_expected_clear_color(self.expected_clear_color)

    def as_dict(self) -> dict[str, object]:
        return {
            "name": self.name,
            "source": self.source,
            "target": self.target,
            "screenshot": self.screenshot,
            "description": self.description,
            "tier": self.tier,
            "comparison": self.comparison,
            "waits": self.waits,
            "viewpos": self.viewpos,
            "cvars": self.cvars,
            "expected_clear_color": self.expected_clear_color,
        }


def load_scenes(path: Path = DEFAULT_MANIFEST) -> list[Scene]:
    payload = json.loads(path.read_text(encoding="utf-8"))
    if payload.get("schema") != 1:
        raise ValueError(f"unsupported scene manifest schema in {path}")

    scenes = []
    names: set[str] = set()
    for item in payload.get("scenes", []):
        source = str(item.get("source", "demo"))
        target_key = "demo" if source == "demo" else "map"
        if target_key not in item:
            raise ValueError(f"scene {item.get('name', '<unnamed>')} lacks {target_key}")
        scene = Scene(
            name=str(item["name"]),
            source=source,
            target=str(item[target_key]),
            screenshot=str(item.get("screenshot", item["name"])),
            description=str(item.get("description", "")),
            tier=str(item.get("tier", "extended")),
            comparison=str(
                item.get("comparison", "capture" if source == "map" else "exact")
            ),
            waits=int(item.get("waits", 8)),
            viewpos=(str(item["viewpos"]) if item.get("viewpos") is not None else None),
            cvars={str(key): str(value) for key, value in item.get("cvars", {}).items()},
            expected_clear_color=(
                str(item["expected_clear_color"])
                if item.get("expected_clear_color") is not None
                else None
            ),
        )
        if scene.name in names:
            raise ValueError(f"duplicate scene name in {path}: {scene.name}")
        names.add(scene.name)
        scenes.append(scene)

    if not scenes:
        raise ValueError(f"scene manifest has no scenes: {path}")
    return scenes


def _windows_process_flags() -> tuple[int, object | None]:
    if os.name != "nt":
        return 0, None

    flags = subprocess.CREATE_NEW_PROCESS_GROUP | subprocess.CREATE_NO_WINDOW
    startupinfo = subprocess.STARTUPINFO()
    startupinfo.dwFlags |= subprocess.STARTF_USESHOWWINDOW
    startupinfo.wShowWindow = 0
    return flags, startupinfo


def hide_process_windows(pid: int) -> int:
    """Hide every top-level window owned by pid; return the number touched."""
    if os.name != "nt":
        return 0

    user32 = ctypes.WinDLL("user32", use_last_error=True)
    callback_type = ctypes.WINFUNCTYPE(
        wintypes.BOOL, wintypes.HWND, wintypes.LPARAM
    )
    hidden = 0

    @callback_type
    def callback(hwnd: int, _lparam: int) -> bool:
        nonlocal hidden
        owner = wintypes.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
        if owner.value == pid:
            # SW_HIDE = 0. This also hides an SDL window after ioquake3's
            # startup SDL_ShowWindow call.
            user32.ShowWindow(hwnd, 0)
            hidden += 1
        return True

    user32.EnumWindows(callback, 0)
    return hidden


def terminate_process_tree(process: subprocess.Popen[bytes]) -> None:
    if process.poll() is not None:
        return

    if os.name == "nt":
        subprocess.run(
            ["taskkill", "/PID", str(process.pid), "/T", "/F"],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            check=False,
            creationflags=subprocess.CREATE_NO_WINDOW,
        )
    else:
        process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()


def find_engine(requested: str | None) -> Path:
    if requested:
        candidate = Path(requested).expanduser()
        if not candidate.is_absolute():
            candidate = ROOT / candidate
        candidate = candidate.resolve()
        if not candidate.is_file():
            raise FileNotFoundError(f"engine executable not found: {candidate}")
        return candidate

    candidates = []
    if os.environ.get("IOQ3_EXECUTABLE"):
        candidates.append(Path(os.environ["IOQ3_EXECUTABLE"]))
    candidates.extend(
        [
            ROOT / "build" / "llvm-clangcl-release" / "Release" / "ioquake3.exe",
            ROOT / "build" / "Release" / "ioquake3.exe",
            ROOT / "build" / "ioquake3.exe",
        ]
    )
    for candidate in candidates:
        if not candidate.is_absolute():
            candidate = ROOT / candidate
        if candidate.is_file():
            return candidate.resolve()
    raise FileNotFoundError("ioquake3 executable not found; pass --engine")


def provision_assets() -> None:
    command = [sys.executable, str(ROOT / "tools" / "get_openarena.py"), "download"]
    result = subprocess.run(command, cwd=ROOT, check=False, timeout=300)
    if result.returncode != 0:
        raise RuntimeError(f"asset provisioning failed with exit code {result.returncode}")


def is_authoritative_capture(renderer: str) -> bool:
    """Return whether the renderer's screenshot is a final-present readback.

    renderer_vrhi owns its ``screenshot`` command and reads the bound
    backbuffer in EndFrame, so its captures are authoritative for the rendered
    clear/UI frame. The GL renderers' stock screenshot paths are not treated as
    final-present oracles.
    """
    return renderer == "vrhi"


def build_command(engine: Path, home: Path, renderer: str, scene: Scene) -> list[str]:
    command = [
        str(engine),
        "+set",
        "fs_basepath",
        str(ASSET_ROOT.resolve()),
        "+set",
        "com_basegame",
        "baseoa",
        "+set",
        "fs_homepath",
        str(home.resolve()),
        "+set",
        "cl_renderer",
        renderer,
    ]

    cvars = dict(BASE_CVARS)
    cvars.update(scene.cvars)
    for key, value in cvars.items():
        command.extend(("+set", key, value))

    actions = []
    if scene.viewpos is not None:
        actions.append(f"setviewpos {scene.viewpos}")
    actions.extend("wait" for _ in range(scene.waits))
    waits = "; ".join(actions)
    if renderer == "vrhi":
        # renderer_vrhi services its capture ticket in EndFrame. Keep the
        # OpenGL action string byte-for-byte unchanged. Cbuf_Execute runs twice
        # per engine frame, so a single wait can be consumed by the same frame
        # that processes "screenshot" and let "quit" shut the renderer down
        # before EndFrame runs. Two waits guarantee at least one EndFrame
        # between the capture ticket and quit in either alignment.
        active_action = f"{waits}; screenshot {scene.screenshot}; wait; wait; quit"
    else:
        active_action = ACTIVE_ACTION_TEMPLATE.format(
            waits=waits,
            screenshot=scene.screenshot,
        )
    command.extend(("+set", "activeAction", active_action))
    map_command = "devmap" if scene.source == "map" and scene.viewpos else scene.source
    command.extend((f"+{map_command}", scene.target))
    return command


def apply_frame_classification(result: dict[str, object], scene: Scene) -> None:
    """Classify a captured frame and publish the class on the result dict.

    Publishes ``frame_class`` (clear_only/uniform_other/content or None) and
    ``frame_classification`` (full metrics dict) for every successful capture.
    The expected clear color defaults to the renderer_vrhi clear so that a
    wrong-color uniform frame is flagged as ``uniform_other`` instead of being
    silently treated as the expected clear.
    """
    normalized = result.get("_normalized_rgb")
    if not isinstance(normalized, bytes) or not normalized:
        result["frame_class"] = None
        result["frame_classification"] = None
        return
    expected: tuple[int, int, int] | None = (
        parse_expected_clear_color(scene.expected_clear_color)
        if scene.expected_clear_color is not None
        else VRHI_CLEAR_COLOR_RGB8
    )
    width = result.get("width")
    height = result.get("height")
    classification = classify_frame(
        normalized,
        width=int(width) if width is not None else None,
        height=int(height) if height is not None else None,
        expected_clear_color=expected,
    )
    result["frame_class"] = classification.frame_class
    result["frame_classification"] = classification.as_dict()


def run_one(
    engine: Path,
    run_dir: Path,
    renderer: str,
    scene: Scene,
    timeout: float,
    index: int,
) -> dict[str, object]:
    run_dir.mkdir(parents=True, exist_ok=True)
    home = run_dir / "home"
    home.mkdir(parents=True, exist_ok=True)
    stdout_path = run_dir / "stdout.log"
    stderr_path = run_dir / "stderr.log"
    screenshot = home / SCREENSHOT_RELATIVE / f"{scene.screenshot}.tga"
    command = build_command(engine, home, renderer, scene)
    flags, startupinfo = _windows_process_flags()
    environment = os.environ.copy()
    environment["SDL_AUDIODRIVER"] = "dummy"

    started = time.monotonic()
    hidden_windows = 0
    timed_out = False
    process: subprocess.Popen[bytes] | None = None
    try:
        with stdout_path.open("wb") as stdout, stderr_path.open("wb") as stderr:
            process = subprocess.Popen(
                command,
                cwd=engine.parent,
                stdout=stdout,
                stderr=stderr,
                env=environment,
                creationflags=flags,
                startupinfo=startupinfo,
            )
            while process.poll() is None:
                hidden_windows += hide_process_windows(process.pid)
                if time.monotonic() - started > timeout:
                    timed_out = True
                    terminate_process_tree(process)
                    break
                time.sleep(0.05)
            hidden_windows += hide_process_windows(process.pid)
            if process.poll() is None:
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    terminate_process_tree(process)
            exit_code = process.returncode
    except BaseException:
        if process is not None:
            terminate_process_tree(process)
        raise

    result: dict[str, object] = {
        "index": index,
        "scene": scene.name,
        "scene_definition": scene.as_dict(),
        "renderer": renderer,
        "command": command,
        "timeout_seconds": timeout,
        "duration_seconds": round(time.monotonic() - started, 3),
        "exit_code": exit_code,
        "timed_out": timed_out,
        "hidden_windows_touched": hidden_windows,
        "home": str(home),
        "stdout": str(stdout_path),
        "stderr": str(stderr_path),
        "screenshot": str(screenshot),
        "screenshot_exists": screenshot.is_file(),
        # Mechanism-based: a vrhi run's capture is the renderer-owned backbuffer
        # readback even when this particular run failed to write the file (that
        # failure is reported separately via screenshot_exists/error).
        "authoritative_capture": is_authoritative_capture(renderer),
    }

    if timed_out:
        result["error"] = "process timeout"
        return result
    if exit_code != 0:
        result["error"] = f"engine exited with {exit_code}"
        return result
    if not screenshot.is_file():
        result["error"] = "engine exited without writing the screenshot"
        return result

    try:
        from compare import load_tga_rgb, sha256_bytes
    except ImportError:
        from .compare import load_tga_rgb, sha256_bytes

    width, height, normalized = load_tga_rgb(screenshot)
    result.update(
        {
            "width": width,
            "height": height,
            "normalized_rgb_bytes": len(normalized),
            "normalized_rgb_sha256": sha256_bytes(normalized),
            "_normalized_rgb": normalized,
        }
    )
    apply_frame_classification(result, scene)
    return result


def select_scenes(
    scenes: list[Scene], requested: list[str] | None, demo: str | None
) -> list[Scene]:
    if demo:
        return [Scene(name=demo, source="demo", target=demo, screenshot=demo)]
    if not requested:
        return scenes

    by_name = {scene.name: scene for scene in scenes}
    selected = []
    for name in requested:
        if name not in by_name:
            raise ValueError(f"unknown scene {name!r}; use --list-scenes")
        selected.append(by_name[name])
    return selected


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--engine", help="path to the compiled ioquake3 executable")
    parser.add_argument("--renderer", default="opengl2")
    parser.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST)
    parser.add_argument(
        "--scene",
        action="append",
        help="scene name to run; repeat for a subset (default: all scenes)",
    )
    parser.add_argument("--demo", help="run one demo directly, bypassing the manifest")
    parser.add_argument("--list-scenes", action="store_true")
    parser.add_argument("--repeat", type=int, default=2)
    parser.add_argument("--timeout", type=float, default=DEFAULT_TIMEOUT)
    parser.add_argument("--run-root", type=Path, default=DEFAULT_RUN_ROOT)
    return parser.parse_args()


def assess_repeatability(
    scene: Scene, results: list[dict[str, object]], repeat: int
) -> tuple[bool, list[dict[str, object]]]:
    if len(results) != repeat or any("error" in result for result in results):
        return False, []

    if scene.comparison == "capture":
        return True, []

    if scene.comparison in (CLASS_CONTENT, CLASS_CLEAR_ONLY):
        classifications = [result.get("frame_classification") for result in results]
        if any(not isinstance(item, dict) for item in classifications):
            return False, []
        expected_class = (
            CLASS_CONTENT if scene.comparison == CLASS_CONTENT else CLASS_CLEAR_ONLY
        )
        passed = all(
            item.get("frame_class") == expected_class for item in classifications
        )
        return passed, classifications

    if scene.comparison == "exact":
        hashes = [result.get("normalized_rgb_sha256") for result in results]
        return len(set(hashes)) == 1, []

    try:
        from compare import compare_tolerant
    except ImportError:
        from .compare import compare_tolerant

    expected = results[0]["_normalized_rgb"]
    comparisons = []
    for result in results[1:]:
        comparisons.append(compare_tolerant(expected, result["_normalized_rgb"]))
    return all(comparison["pass"] for comparison in comparisons), comparisons


def public_result(result: dict[str, object]) -> dict[str, object]:
    return {key: value for key, value in result.items() if key != "_normalized_rgb"}


def main() -> int:
    args = parse_args()
    scenes = load_scenes(args.manifest)
    if args.list_scenes:
        for scene in scenes:
            print(
                f"{scene.name}\t{scene.source}\t{scene.target}\t"
                f"{scene.tier}\t{scene.description}"
            )
        return 0
    if args.repeat < 1:
        raise ValueError("--repeat must be at least 1")
    if args.timeout <= 0:
        raise ValueError("--timeout must be positive")

    selected = select_scenes(scenes, args.scene, args.demo)
    engine = find_engine(args.engine)
    provision_assets()
    run_root = args.run_root.expanduser().resolve() / (
        time.strftime("%Y%m%d-%H%M%S") + f"-{os.getpid()}"
    )
    run_root.mkdir(parents=True, exist_ok=False)

    scene_reports: list[dict[str, object]] = []
    for scene in selected:
        print(
            f"[image-test] scene {scene.name} ({scene.source} {scene.target}), "
            f"renderer={args.renderer}, repeat={args.repeat}"
        )
        results: list[dict[str, object]] = []
        for index in range(1, args.repeat + 1):
            print(
                f"[image-test]   run {index}/{args.repeat}: "
                f"hidden, muted, timeout={args.timeout:g}s"
            )
            result = run_one(
                engine,
                run_root / scene.name / f"run-{index}",
                args.renderer,
                scene,
                args.timeout,
                index,
            )
            results.append(result)
            if "error" in result:
                print(f"[image-test]   FAIL: {result['error']}")
                print(f"[image-test]   logs: {result['stderr']}")
                break
            print(
                f"[image-test]   capture {result['width']}x{result['height']} "
                f"sha256={result['normalized_rgb_sha256']} "
                f"class={result.get('frame_class')}"
            )

        capture_valid = len(results) == args.repeat and all(
            "error" not in result for result in results
        )
        policy_pass, repeatability_metrics = assess_repeatability(
            scene, results, args.repeat
        )
        scene_report = {
            "scene": scene.as_dict(),
            "repeat": args.repeat,
            "repeatability_policy": scene.comparison,
            "capture_valid": capture_valid,
            "policy_pass": policy_pass,
            "repeatable_normalized_rgb": (
                policy_pass if scene.comparison != "capture" else False
            ),
            "repeatability_metrics": repeatability_metrics,
            "results": [public_result(result) for result in results],
        }
        scene_reports.append(scene_report)
        if policy_pass:
            print(
                f"[image-test]   PASS: {scene.name} "
                f"({scene.comparison} policy)"
            )
        else:
            print(f"[image-test]   FAIL: {scene.name} was not repeatable")

    report = {
        "schema": 2,
        "test": "stock_screenshot_smoke_suite",
        "authoritative_capture": is_authoritative_capture(args.renderer),
        "engine": str(engine),
        "renderer": args.renderer,
        "manifest": str(args.manifest.resolve()),
        "repeat": args.repeat,
        "run_root": str(run_root),
        "scenes": scene_reports,
    }
    report_path = run_root / "report.json"
    report_path.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(f"[image-test] report: {report_path}")

    passed = len(scene_reports) == len(selected) and all(
        report["policy_pass"] for report in scene_reports
    )
    if not passed:
        print("[image-test] FAIL: one or more image smoke scenes failed")
        return 1
    print(
        f"[image-test] PASS: {len(scene_reports)} {args.renderer} scenes "
        "met their repeatability policies"
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (FileNotFoundError, RuntimeError, ValueError, TimeoutError) as error:
        print(f"[image-test] ERROR: {error}", file=sys.stderr)
        raise SystemExit(2)
