#!/usr/bin/env python3
"""API-neutral benchmark harness for ioq3 backends.

Launches a backend command in a fresh process per trial, consumes JSONL
timing lines (producer/finalize/optional GPU) from stdout, discards explicit
warmup samples, and computes median/mean/p90/p95/p99/MAD/min/max and
per-trial spread per backend and workload. Raw JSONL plus a summary report
are written under temp/benchmark/ (git-ignored).

Deliberate boundaries:

- The harness is API-neutral: it knows nothing about renderer internals or
  cvars. It only launches a command, sets documented environment variables,
  and reads the documented JSONL contract.
- An opt-in real-engine mode adds machine-specific plumbing entirely from
  the command line (--asset-root, --home-root) and safe common engine args
  (--hidden/--no-audio/--windowed/--vsync-off/--fixed-timing). All of it
  defaults off, so fake backends and unit callers see the plain contract.
  Machine-specific paths are never hard-coded into workloads.json.
- This harness does NOT claim OpenGL-versus-VRHI equivalence. It reports
  raw timings only.
"""

from __future__ import annotations

import argparse
import ctypes
from ctypes import wintypes
import json
import os
import queue
import subprocess
import sys
import threading
import time
from collections import defaultdict
from datetime import datetime, timezone
from pathlib import Path

import benchmark_config as config
import benchmark_protocol as protocol
import benchmark_stats as stats

SCRIPT_DIR = Path(__file__).resolve().parent
ROOT = SCRIPT_DIR.parents[1]
DEFAULT_MANIFEST = SCRIPT_DIR / "workloads.json"
DEFAULT_RUN_ROOT = ROOT / "temp" / "benchmark"

_EOF = object()
_WAIT = object()


def _windows_process_flags(
    hidden: bool = False,
) -> tuple[int, object | None]:
    """Windows: a fresh process group so a timeout can kill the whole tree.

    With hidden=True the child also gets no console window and an
    SW_HIDE startup hint (the engine's own window is hidden afterwards by
    hide_process_windows). Window policy stays opt-in: the default remains
    fully neutral.
    """
    if os.name != "nt":
        return 0, None
    flags = subprocess.CREATE_NEW_PROCESS_GROUP
    startupinfo = None
    if hidden:
        flags |= subprocess.CREATE_NO_WINDOW
        startupinfo = subprocess.STARTUPINFO()
        startupinfo.dwFlags |= subprocess.STARTF_USESHOWWINDOW
        startupinfo.wShowWindow = 0
    return flags, startupinfo


def hide_process_windows(pid: int) -> int:
    """Hide every top-level window owned by pid; return the count touched.

    Windows only; no-op elsewhere. ioquake3 creates its SDL window after
    startup, so the process-creation hint alone is not enough.
    """
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
            user32.ShowWindow(hwnd, 0)  # SW_HIDE
            hidden += 1
        return True

    user32.EnumWindows(callback, 0)
    return hidden


def terminate_process_tree(process: subprocess.Popen[object]) -> None:
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


def _read_lines(stream: object, line_queue: "queue.Queue[object]") -> None:
    """Drain the child's stdout pipe into a queue (cross-platform, avoids
    blocking the main loop while the timeout is pending)."""
    readline = stream.readline  # type: ignore[attr-defined]
    try:
        while True:
            line = readline()
            if line == "":
                break
            line_queue.put(line)
    finally:
        line_queue.put(_EOF)


def _record_line(line: str, outcome: dict[str, object]) -> None:
    result = protocol.parse_timing_line(line)
    if result.kind == "sample":
        outcome["samples"].append(result.sample)
    elif result.kind == "invalid":
        outcome["invalid_count"] += 1
        messages = outcome["invalid_messages"]
        if len(messages) < 3:  # type: ignore[arg-type]
            messages.append(result.message)
    elif result.kind == "ignored":
        outcome["ignored_count"] += 1


def run_trial(
    engine: Path,
    backend: config.Backend,
    workload: config.Workload,
    warmup: int,
    samples: int,
    timeout: float,
    trial_dir: Path,
    environment: dict[str, str],
    home: Path | None = None,
    engine_args: Sequence[str] = (),
    hidden: bool = False,
) -> dict[str, object]:
    """Run one fresh backend process and classify its JSONL samples.

    The first `warmup` sample lines of the process are warmup; the next
    `samples` are measured; anything after that is extra (counted, excluded
    from statistics). The process is always cleaned up, even on exceptions
    and timeouts.

    Optional real-engine plumbing (all defaults keep the API-neutral
    behavior): `home` is a fresh, isolated fs_homepath directory created
    before launch and recorded on the outcome; `engine_args` is the engine
    argument prefix inserted between the executable and the backend args;
    `hidden` hides the child's windows on Windows (opt-in window policy).
    """
    trial_dir.mkdir(parents=True, exist_ok=True)
    stdout_path = trial_dir / "stdout.log"
    stderr_path = trial_dir / "stderr.log"
    if home is not None:
        home.mkdir(parents=True, exist_ok=True)
    command = config.build_command(engine, backend, workload, engine_args)
    flags, startupinfo = _windows_process_flags(hidden)
    started = time.monotonic()

    outcome: dict[str, object] = {
        "backend": backend.name,
        "workload": workload.name,
        "command": command,
        "exit_code": None,
        "timed_out": False,
        "duration_seconds": 0.0,
        "error": None,
        "warning": None,
        "home": str(home) if home is not None else None,
        "engine_args": list(engine_args),
        "warmup_count": 0,
        "measured_count": 0,
        "extra_count": 0,
        "ignored_count": 0,
        "invalid_count": 0,
        "invalid_messages": [],
        "samples": [],
        "entries": [],  # list of (phase, Sample): warmup | sample | extra
        "stdout": str(stdout_path),
        "stderr": str(stderr_path),
    }

    process: subprocess.Popen[str] | None = None
    try:
        with stdout_path.open("w", encoding="utf-8") as stdout_log, stderr_path.open(
            "wb"
        ) as stderr_log:
            process = subprocess.Popen(
                command,
                cwd=str(engine.parent),
                stdout=subprocess.PIPE,
                stderr=stderr_log,
                env=environment,
                text=True,
                encoding="utf-8",
                errors="replace",
                bufsize=1,
                creationflags=flags,
                startupinfo=startupinfo,  # type: ignore[arg-type]
            )
            line_queue: "queue.Queue[object]" = queue.Queue()
            reader = threading.Thread(
                target=_read_lines, args=(process.stdout, line_queue), daemon=True
            )
            reader.start()

            eof_seen = False
            deadline = started + timeout
            drain_deadline = deadline + 5.0
            while True:
                if not eof_seen:
                    try:
                        item = line_queue.get(timeout=0.1)
                    except queue.Empty:
                        item = _WAIT
                    if item is _EOF:
                        eof_seen = True
                    elif item is not _WAIT:
                        stdout_log.write(item)
                        stdout_log.flush()
                        _record_line(item, outcome)

                if process.poll() is None:
                    if hidden:
                        hide_process_windows(process.pid)
                    if time.monotonic() >= deadline:
                        outcome["timed_out"] = True
                        terminate_process_tree(process)
                else:
                    if eof_seen:
                        break
                    if time.monotonic() >= drain_deadline:
                        break

            # The backend closed stdout but kept running: wait briefly, then kill.
            if eof_seen and process.poll() is None:
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    if outcome["error"] is None:
                        outcome["error"] = "backend closed stdout but did not exit"
                    terminate_process_tree(process)
                    try:
                        process.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        pass

            reader.join(timeout=5)
            if process.stdout is not None:
                process.stdout.close()
            exit_code = process.returncode
    finally:
        if process is not None and process.poll() is None:
            terminate_process_tree(process)
        if process is not None and process.stdout is not None:
            try:
                process.stdout.close()
            except OSError:
                pass

    sample_list = outcome["samples"]
    warmup_count = min(warmup, len(sample_list))
    measured = sample_list[warmup_count : warmup_count + samples]
    extra = sample_list[warmup_count + samples :]
    outcome["warmup_count"] = warmup_count
    outcome["measured_count"] = len(measured)
    outcome["extra_count"] = len(extra)
    outcome["entries"] = (
        [("warmup", s) for s in sample_list[:warmup_count]]
        + [("sample", s) for s in measured]
        + [("extra", s) for s in extra]
    )
    outcome["duration_seconds"] = round(time.monotonic() - started, 3)

    outcome["exit_code"] = exit_code
    if outcome["timed_out"] and len(measured) < samples:
        outcome["error"] = "process timeout"
    elif len(measured) < samples:
        outcome["error"] = (
            f"backend exited early: {len(measured)} measured samples, "
            f"expected {samples}"
        )
    if exit_code != 0 and outcome["error"] is None:
        outcome["warning"] = f"backend exited with code {exit_code}"
    return outcome


def aggregate_cell(
    backend_name: str,
    workload_name: str,
    outcomes: list[dict[str, object]],
    declared_fields: dict[str, bool],
) -> dict[str, object]:
    """Aggregate per-trial outcomes into one summary cell."""
    completed = [o for o in outcomes if o["error"] is None]
    trial_errors = [
        {
            "trial": o["trial"],
            "error": o["error"],
            "exit_code": o["exit_code"],
            "timed_out": o["timed_out"],
            "duration_seconds": o["duration_seconds"],
            "stdout": o["stdout"],
            "stderr": o["stderr"],
        }
        for o in outcomes
        if o["error"] is not None
    ]
    trial_warnings = [
        {"trial": o["trial"], "warning": o["warning"]}
        for o in outcomes
        if o.get("warning") is not None
    ]

    fields: dict[str, object] = {}
    fields_unobserved: list[str] = []
    for field, _required in declared_fields.items():
        values: list[float] = []
        trial_means: list[float] = []
        for outcome in completed:
            trial_values = [
                getattr(sample, field)
                for phase, sample in outcome["entries"]
                if phase == "sample"
                and getattr(sample, field) is not None
            ]
            if not trial_values:
                continue
            trial_means.append(stats.mean(trial_values))
            values.extend(trial_values)
        if values:
            summary = stats.summarize(values)
            summary["trial_means"] = trial_means
            summary["trial_spread"] = stats.trial_spread(
                trial_means, summary["mean"]
            )
            fields[field] = summary
        else:
            fields_unobserved.append(field)

    return {
        "backend": backend_name,
        "workload": workload_name,
        "trials_requested": len(outcomes),
        "trials_completed": len(completed),
        "trial_errors": trial_errors,
        "trial_warnings": trial_warnings,
        "lines": {
            "warmup": sum(o["warmup_count"] for o in outcomes),
            "measured": sum(o["measured_count"] for o in outcomes),
            "extra": sum(o["extra_count"] for o in outcomes),
            "ignored": sum(o["ignored_count"] for o in outcomes),
            "invalid": sum(o["invalid_count"] for o in outcomes),
        },
        "fields": fields,
        "fields_unobserved": fields_unobserved,
    }


def write_raw_lines(
    path: Path,
    backend_name: str,
    workload_name: str,
    trial: int,
    entries: list[tuple[str, protocol.Sample]],
) -> None:
    """Append raw sample lines (original fields plus provenance) as JSONL."""
    with path.open("a", encoding="utf-8") as handle:
        for phase, sample in entries:
            line: dict[str, object] = {
                "backend": backend_name,
                "workload": workload_name,
                "trial": trial,
                "phase": phase,
            }
            line.update(sample.raw)
            handle.write(json.dumps(line) + "\n")


def find_engine(requested: str | None) -> Path:
    if requested:
        candidate = Path(requested).expanduser()
        if not candidate.is_absolute():
            candidate = ROOT / candidate
        candidate = candidate.resolve()
        if not candidate.is_file():
            raise FileNotFoundError(f"backend executable not found: {candidate}")
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
    raise FileNotFoundError(
        "backend executable not found; pass --engine or set IOQ3_EXECUTABLE"
    )


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--engine", help="path to the backend executable")
    parser.add_argument(
        "--manifest", type=Path, default=DEFAULT_MANIFEST,
        help="immutable workload manifest (default: tests/benchmark/workloads.json)",
    )
    parser.add_argument(
        "--backend",
        action="append",
        help="backend name from the manifest; repeat for several (default: all)",
    )
    parser.add_argument(
        "--workload",
        action="append",
        help="workload name from the manifest; repeat for several (default: all)",
    )
    parser.add_argument("--trials", type=int, default=None)
    parser.add_argument("--warmup", type=int, default=None)
    parser.add_argument("--samples", type=int, default=None)
    parser.add_argument("--timeout", type=float, default=None)
    parser.add_argument(
        "--order", default=None, help="backend trial order: fixed | random | abba"
    )
    parser.add_argument(
        "--seed", type=int, default=None,
        help="seed for --order random (default: manifest default)",
    )
    parser.add_argument("--list-backends", action="store_true")
    parser.add_argument("--list-workloads", action="store_true")
    parser.add_argument(
        "--run-root", type=Path, default=DEFAULT_RUN_ROOT,
        help="parent directory for runs (default: temp/benchmark)",
    )
    parser.add_argument(
        "--asset-root", type=Path, default=None,
        help="pinned asset root containing the base game directory (e.g. "
        "temp/assets/openarena-0.8.8). Machine-specific: supplied on the "
        "command line only, never stored in workloads.json. Adds +set "
        "fs_basepath/com_basegame to every trial command.",
    )
    parser.add_argument(
        "--basegame", default=config.BASE_GAME_DEFAULT,
        help="base game directory inside --asset-root "
        f"(default: {config.BASE_GAME_DEFAULT})",
    )
    parser.add_argument(
        "--home-root", type=Path, default=None,
        help="parent for per-trial isolated fs_homepath directories "
        "(default: <run root>/homes, itself under temp/benchmark). "
        "Every trial gets a fresh, empty home; no user q3config.cfg is "
        "ever reused.",
    )
    parser.add_argument(
        "--hidden", action="store_true",
        help="hide the engine window (Windows: no console window plus "
        "periodic hiding of the SDL window); no-op on POSIX",
    )
    parser.add_argument(
        "--no-audio", action="store_true",
        help="disable engine audio (s_initsound 0, s_volume 0, "
        "SDL_AUDIODRIVER=dummy)",
    )
    parser.add_argument(
        "--windowed", action="store_true",
        help="run windowed instead of fullscreen (r_fullscreen 0)",
    )
    parser.add_argument(
        "--vsync-off", action="store_true",
        help="disable swap interval (r_swapInterval 0)",
    )
    parser.add_argument(
        "--fixed-timing", action="store_true",
        help="decouple engine frame timing from display and user config "
        "(sv_cheats 1, com_maxfps 0, fixedtime 0, timescale 1, "
        "cl_timeNudge 0)",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    manifest = config.load_manifest(args.manifest)

    if args.list_backends:
        for backend in manifest.backends:
            print(
                f"{backend.name}\tjsonl_support={backend.jsonl_support}\t"
                f"{backend.description}"
            )
        return 0
    if args.list_workloads:
        for workload in manifest.workloads:
            print(f"{workload.name}\t{workload.description}")
        return 0

    backends = config.resolve_backends(manifest, args.backend)
    workloads = config.resolve_workloads(manifest, args.workload)
    defaults = manifest.defaults
    warmup = args.warmup if args.warmup is not None else defaults["warmup"]
    samples = args.samples if args.samples is not None else defaults["samples"]
    trials = args.trials if args.trials is not None else defaults["trials"]
    timeout = args.timeout if args.timeout is not None else defaults["timeout_seconds"]
    order = args.order if args.order is not None else defaults["order"]
    seed = args.seed if args.seed is not None else defaults["seed"]
    config.validate_run_config(backends, trials, warmup, samples, timeout, order)

    engine = find_engine(args.engine)
    plan = config.plan_order([b.name for b in backends], trials, order, seed)

    engine_opts = config.EngineOptions(
        asset_root=args.asset_root,
        home_root=args.home_root,
        basegame=args.basegame,
        hidden=args.hidden,
        no_audio=args.no_audio,
        windowed=args.windowed,
        vsync_off=args.vsync_off,
        fixed_timing=args.fixed_timing,
    )
    engine_opts.validate()

    for backend in backends:
        if not backend.jsonl_support:
            print(
                f"[bench] WARNING: backend {backend.name!r} is marked "
                "jsonl_support=false; its trials may time out with zero "
                "samples."
            )

    run_root = args.run_root.expanduser().resolve() / (
        time.strftime("%Y%m%d-%H%M%S") + f"-{os.getpid()}"
    )
    run_root.mkdir(parents=True, exist_ok=False)
    raw_path = run_root / "raw.jsonl"
    home_root = engine_opts.resolved_home_root(run_root)

    by_backend = {b.name: b for b in backends}
    trial_counts: dict[str, int] = defaultdict(int)
    outcomes_by_cell: dict[tuple[str, str], list[dict[str, object]]] = defaultdict(list)
    interrupted = False

    print(
        f"[bench] run root: {run_root}\n"
        f"[bench] engine: {engine}\n"
        f"[bench] manifest: {manifest.path} sha256={manifest.sha256}\n"
        f"[bench] order={order} seed={seed} trials={trials} "
        f"warmup={warmup} samples={samples} timeout={timeout:g}s\n"
        f"[bench] engine options: {json.dumps(engine_opts.as_dict(home_root))}\n"
        f"[bench] execution order: {' > '.join(plan)}"
    )

    try:
        for workload in workloads:
            for backend_name in plan:
                backend = by_backend[backend_name]
                trial_counts[backend_name] += 1
                trial_number = trial_counts[backend_name]
                trial_dir = (
                    run_root
                    / "logs"
                    / f"{backend_name}__{workload.name}"
                    / f"trial-{trial_number}"
                )
                home = config.trial_home(
                    home_root, backend_name, workload.name, trial_number
                )
                environment = engine_opts.environment(os.environ.copy())
                environment.update(
                    config.benchmark_env(backend_name, warmup, samples)
                )
                outcome = run_trial(
                    engine,
                    backend,
                    workload,
                    warmup,
                    samples,
                    timeout,
                    trial_dir,
                    environment,
                    home=home,
                    engine_args=engine_opts.command_prefix(home),
                    hidden=engine_opts.hidden,
                )
                outcome["trial"] = trial_number
                outcomes_by_cell[(backend_name, workload.name)].append(outcome)
                write_raw_lines(
                    raw_path,
                    backend_name,
                    workload.name,
                    trial_number,
                    outcome["entries"],
                )
                status = (
                    f"ok" if outcome["error"] is None else f"FAIL: {outcome['error']}"
                )
                print(
                    f"[bench] {backend_name}/{workload.name} trial "
                    f"{trial_number}/{trials}: "
                    f"{outcome['measured_count']} samples, "
                    f"exit={outcome['exit_code']}, "
                    f"{outcome['duration_seconds']:.1f}s {status}"
                )
    except KeyboardInterrupt:
        interrupted = True
        print("[bench] interrupted; writing partial results", file=sys.stderr)

    cells = [
        aggregate_cell(name, wname, outcomes, manifest.fields)
        for (name, wname), outcomes in sorted(outcomes_by_cell.items())
    ]

    summary = {
        "schema": 1,
        "tool": "ioq3-benchmark",
        "equivalence_claim": "none",
        "note": (
            "Raw timings only; this harness claims no GL2/VRHI equivalence "
            "and no correctness."
        ),
        "created": datetime.now(timezone.utc).isoformat(),
        "run_root": str(run_root),
        "engine": str(engine),
        "manifest": {
            "path": str(manifest.path),
            "name": manifest.name,
            "immutable": True,
            "sha256": manifest.sha256,
        },
        "config": {
            "order": order,
            "seed": seed,
            "trials": trials,
            "warmup": warmup,
            "samples": samples,
            "timeout_seconds": timeout,
        },
        "engine_options": engine_opts.as_dict(home_root),
        "execution_order": plan,
        "results": cells,
    }
    summary_path = run_root / "summary.json"
    summary_path.write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")

    for cell in cells:
        print(
            f"[bench] {cell['backend']}/{cell['workload']}: "
            f"trials {cell['trials_completed']}/{cell['trials_requested']} completed"
        )
        for error in cell["trial_errors"]:
            print(f"[bench]   trial {error['trial']} FAILED: {error['error']}")
        for warning in cell["trial_warnings"]:
            print(f"[bench]   trial {warning['trial']} WARNING: {warning['warning']}")
        for field, field_stats in cell["fields"].items():
            spread = field_stats["trial_spread"]
            spread_text = "n/a" if spread is None else f"{spread:.4f}"
            print(
                f"[bench]   {field:<18} median={field_stats['median']:.6f} "
                f"mean={field_stats['mean']:.6f} "
                f"p95={field_stats['p95']:.6f} p99={field_stats['p99']:.6f} "
                f"mad={field_stats['mad']:.6f} spread={spread_text}"
            )
    print(f"[bench] raw JSONL: {raw_path}")
    print(f"[bench] summary:   {summary_path}")
    print(
        "[bench] NOTE: this harness reports raw timings only; "
        "it claims no GL2/VRHI equivalence."
    )

    if interrupted:
        return 130
    if not cells or any(cell["trial_errors"] for cell in cells):
        print("[bench] FAIL: one or more trials failed", file=sys.stderr)
        return 1
    print("[bench] PASS: all trials completed")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (FileNotFoundError, RuntimeError, ValueError, TimeoutError) as error:
        print(f"[bench] ERROR: {error}", file=sys.stderr)
        raise SystemExit(2)
