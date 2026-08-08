#!/usr/bin/env python3
"""Run isolated, hidden-window OpenArena image smoke tests.

The first test intentionally uses ioquake3's stock screenshot command. It
proves asset provisioning, renderer startup, demo playback, screenshot output,
and teardown. The screenshot is not yet an authoritative final-present image;
that capture path will be added before backend parity is gated.
"""

from __future__ import annotations

import argparse
import ctypes
from ctypes import wintypes
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time

SCRIPT_DIR = Path(__file__).resolve().parent
ROOT = SCRIPT_DIR.parents[1]
ASSET_ROOT = ROOT / "temp" / "assets" / "openarena-0.8.8"
DEFAULT_RUN_ROOT = ROOT / "temp" / "image-tests"
DEFAULT_DEMO = "demo088-test1"
DEFAULT_TIMEOUT = 90.0

# Keep the smoke test deliberately conservative. It is run against OpenArena's
# pinned test demo and only checks the stock screenshot path for now.
SCREENSHOT_NAME = "smoke"
SCREENSHOT_RELATIVE = Path("baseoa") / "screenshots" / f"{SCREENSHOT_NAME}.tga"
ACTIVE_ACTION = "wait ; wait ; wait ; wait ; wait ; wait ; wait ; wait ; screenshot smoke ; quit"


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


def build_command(engine: Path, home: Path, renderer: str, demo: str) -> list[str]:
    return [
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
        # Never initialize or output audio during an automated test.
        "+set",
        "s_initsound",
        "0",
        "+set",
        "s_volume",
        "0",
        "+set",
        "s_muteWhenUnfocused",
        "1",
        # Leave enough hunk space for the OpenArena VM and demo.
        "+set",
        "com_hunkMegs",
        "512",
        "+set",
        "com_zoneMegs",
        "64",
        "+set",
        "logfile",
        "2",
        "+set",
        "r_fullscreen",
        "0",
        "+set",
        "r_mode",
        "3",
        "+set",
        "r_swapInterval",
        "0",
        "+set",
        "timedemo",
        "1",
        "+set",
        "activeAction",
        ACTIVE_ACTION,
        "+demo",
        demo,
    ]


def run_one(
    engine: Path,
    run_dir: Path,
    renderer: str,
    demo: str,
    timeout: float,
    index: int,
) -> dict[str, object]:
    run_dir.mkdir(parents=True, exist_ok=True)
    home = run_dir / "home"
    home.mkdir(parents=True, exist_ok=True)
    stdout_path = run_dir / "stdout.log"
    stderr_path = run_dir / "stderr.log"
    screenshot = home / SCREENSHOT_RELATIVE
    command = build_command(engine, home, renderer, demo)
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
        "renderer": renderer,
        "demo": demo,
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
        "authoritative_capture": False,
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

    from compare import load_tga_rgb, sha256_bytes

    width, height, normalized = load_tga_rgb(screenshot)
    result.update(
        {
            "width": width,
            "height": height,
            "normalized_rgb_bytes": len(normalized),
            "normalized_rgb_sha256": sha256_bytes(normalized),
        }
    )
    return result


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--engine", help="path to the compiled ioquake3 executable")
    parser.add_argument("--renderer", default="opengl2")
    parser.add_argument("--demo", default=DEFAULT_DEMO)
    parser.add_argument("--repeat", type=int, default=2)
    parser.add_argument("--timeout", type=float, default=DEFAULT_TIMEOUT)
    parser.add_argument("--run-root", type=Path, default=DEFAULT_RUN_ROOT)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if args.repeat < 1:
        raise ValueError("--repeat must be at least 1")
    if args.timeout <= 0:
        raise ValueError("--timeout must be positive")

    engine = find_engine(args.engine)
    provision_assets()
    run_root = args.run_root.expanduser().resolve() / (
        time.strftime("%Y%m%d-%H%M%S") + f"-{os.getpid()}"
    )
    run_root.mkdir(parents=True, exist_ok=False)

    results = []
    for index in range(1, args.repeat + 1):
        print(f"[image-test] run {index}/{args.repeat}: hidden, muted, timeout={args.timeout:g}s")
        results.append(
            run_one(
                engine,
                run_root / f"run-{index}",
                args.renderer,
                args.demo,
                args.timeout,
                index,
            )
        )
        result = results[-1]
        if "error" in result:
            print(f"[image-test] FAIL: {result['error']}")
            break
        print(
            f"[image-test] capture {result['width']}x{result['height']} "
            f"sha256={result['normalized_rgb_sha256']}"
        )

    hashes = [result.get("normalized_rgb_sha256") for result in results]
    repeatable = len(results) == args.repeat and len(set(hashes)) == 1
    report = {
        "schema": 1,
        "test": "stock_screenshot_smoke",
        "authoritative_capture": False,
        "engine": str(engine),
        "renderer": args.renderer,
        "demo": args.demo,
        "repeat": args.repeat,
        "repeatable_normalized_rgb": repeatable,
        "run_root": str(run_root),
        "results": results,
    }
    report_path = run_root / "report.json"
    report_path.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(f"[image-test] report: {report_path}")

    passed = (
        len(results) == args.repeat
        and all("error" not in result for result in results)
        and repeatable
    )
    if not passed:
        print("[image-test] FAIL: stock screenshot smoke test did not pass")
        return 1
    print(
        f"[image-test] PASS: repeated {args.renderer} stock screenshots match exactly"
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (FileNotFoundError, RuntimeError, ValueError, TimeoutError) as error:
        print(f"[image-test] ERROR: {error}", file=sys.stderr)
        raise SystemExit(2)
