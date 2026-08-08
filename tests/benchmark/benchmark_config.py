"""Manifest loading, run-configuration validation, command and order planning.

The workload manifest is an immutable, committed artifact: loading never
modifies it, every backend/workload name must resolve against it, and any
intended change requires a deliberate schema bump.
"""

from __future__ import annotations

import hashlib
import json
import random
import re
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Sequence

from benchmark_protocol import ENV_BACKEND, ENV_JSONL, ENV_SAMPLES, ENV_WARMUP

# Opt-in real-engine cvars. Machine-specific paths (asset root, home root)
# never appear here: they are passed on the command line at run time.
BASE_GAME_DEFAULT = "baseoa"
NO_AUDIO_CVARS = ("s_initsound", "0", "s_volume", "0")
WINDOWED_CVARS = ("r_fullscreen", "0")
VSYNC_OFF_CVARS = ("r_swapInterval", "0")
FIXED_TIMING_CVARS = (
    "sv_cheats", "1",
    "com_maxfps", "0",
    "fixedtime", "0",
    "timescale", "1",
    "cl_timeNudge", "0",
)

SAFE_NAME = re.compile(r"^[A-Za-z0-9_-]+$")
SCHEMA = 1
KNOWN_FIELDS = {"producer_seconds", "finalize_seconds", "gpu_seconds"}
REQUIRED_MANIFEST_FIELDS = {"producer_seconds", "finalize_seconds"}
VALID_ORDERS = {"fixed", "random", "abba"}


@dataclass(frozen=True)
class Backend:
    name: str
    args: tuple[str, ...]
    description: str = ""
    jsonl_support: bool = True


@dataclass(frozen=True)
class Workload:
    name: str
    args: tuple[str, ...]
    description: str = ""


@dataclass(frozen=True)
class Manifest:
    name: str
    description: str
    fields: dict[str, bool]
    defaults: dict[str, Any]
    backends: tuple[Backend, ...]
    workloads: tuple[Workload, ...]
    path: Path
    sha256: str


@dataclass(frozen=True)
class EngineOptions:
    """Opt-in real-engine launch options; all paths are machine-specific.

    Every field defaults to the API-neutral empty state: no extra command
    arguments and no environment overrides. Machine-specific paths (asset
    root, home root) come from the command line only; they never belong in
    the committed manifest (see workloads.json).
    """

    asset_root: Path | None = None
    home_root: Path | None = None
    basegame: str = BASE_GAME_DEFAULT
    hidden: bool = False
    no_audio: bool = False
    windowed: bool = False
    vsync_off: bool = False
    fixed_timing: bool = False

    def resolved_asset_root(self) -> Path | None:
        if self.asset_root is None:
            return None
        return self.asset_root.expanduser().resolve()

    def resolved_home_root(self, run_root: Path) -> Path:
        """Parent for per-trial isolated homes (default: under the run
        root, which itself lives under temp/benchmark)."""
        if self.home_root is None:
            return run_root / "homes"
        return self.home_root.expanduser().resolve()

    def validate(self) -> None:
        """Fail fast on obviously unusable engine options."""
        root = self.resolved_asset_root()
        if root is None:
            return
        if not SAFE_NAME.fullmatch(self.basegame):
            raise ValueError(f"unsafe base game name: {self.basegame!r}")
        if not (root / self.basegame).is_dir():
            raise ValueError(
                f"asset root {root} does not contain a {self.basegame!r} "
                "directory (provision it with tools/get_openarena.py download)"
            )

    def command_prefix(self, home: Path | None) -> list[str]:
        """Engine argument prefix (+set cvar pairs) for one trial.

        Empty when fully neutral. The prefix always precedes backend and
        workload args in the final command, so a backend's own +set (e.g.
        cl_renderer) still wins on the command line.
        """
        args: list[str] = []
        root = self.resolved_asset_root()
        if root is not None:
            args += ["+set", "fs_basepath", str(root),
                     "+set", "com_basegame", self.basegame]
        if home is not None:
            args += ["+set", "fs_homepath", str(home)]
        if self.no_audio:
            args += ["+set"] + list(NO_AUDIO_CVARS)
        if self.windowed:
            args += ["+set"] + list(WINDOWED_CVARS)
        if self.vsync_off:
            args += ["+set"] + list(VSYNC_OFF_CVARS)
        if self.fixed_timing:
            args += ["+set"] + list(FIXED_TIMING_CVARS)
        return args

    def environment(self, base: dict[str, str]) -> dict[str, str]:
        """The API-neutral environment plus opt-in overrides."""
        env = dict(base)
        if self.no_audio:
            env["SDL_AUDIODRIVER"] = "dummy"
        return env

    def as_dict(self, home_root: Path) -> dict[str, object]:
        """Provenance for the summary report."""
        root = self.resolved_asset_root()
        return {
            "asset_root": str(root) if root is not None else None,
            "home_root": str(home_root),
            "basegame": self.basegame,
            "hidden": self.hidden,
            "no_audio": self.no_audio,
            "windowed": self.windowed,
            "vsync_off": self.vsync_off,
            "fixed_timing": self.fixed_timing,
        }


def trial_home(
    home_root: Path, backend_name: str, workload_name: str, trial_number: int
) -> Path:
    """Fresh, isolated fs_homepath directory for one trial.

    The run root is a unique timestamped directory, so a home produced
    here never existed before this invocation; each trial therefore starts
    with an empty home (no user q3config.cfg, no leftover state).
    """
    return (
        home_root
        / f"{backend_name}__{workload_name}"
        / f"trial-{trial_number}"
        / "home"
    )


def _check_safe_name(label: str, value: str) -> None:
    if not SAFE_NAME.fullmatch(value):
        raise ValueError(f"unsafe {label} name: {value!r}")


def _check_args(label: str, args: object) -> tuple[str, ...]:
    if not isinstance(args, list) or not all(
        isinstance(arg, str) and arg for arg in args
    ):
        raise ValueError(
            f"{label} args must be a list of non-empty strings"
        )
    return tuple(args)


def _validate_counts(
    label: str, warmup: object, samples: object, trials: object
) -> None:
    for name, value, minimum in (
        ("warmup", warmup, 0),
        ("samples", samples, 1),
        ("trials", trials, 1),
    ):
        if not isinstance(value, int) or isinstance(value, bool) or value < minimum:
            raise ValueError(
                f"{label}.{name} must be an integer >= {minimum}, got {value!r}"
            )


def load_manifest(path: Path) -> Manifest:
    """Load and validate the immutable workload manifest."""
    path = path.resolve()
    payload = json.loads(path.read_text(encoding="utf-8"))
    if payload.get("schema") != SCHEMA:
        raise ValueError(
            f"unsupported benchmark manifest schema in {path}: "
            f"{payload.get('schema')!r}"
        )
    if payload.get("immutable") is not True:
        raise ValueError(f"benchmark manifest must be immutable: {path}")

    fields = payload.get("fields")
    if not isinstance(fields, dict):
        raise ValueError("manifest 'fields' must be an object")
    normalized_fields: dict[str, bool] = {}
    for key, required in fields.items():
        if key not in KNOWN_FIELDS:
            raise ValueError(f"unknown timing field in manifest: {key!r}")
        if not isinstance(required, bool):
            raise ValueError(f"field requirement for {key!r} must be a boolean")
        normalized_fields[key] = required
    for key in REQUIRED_MANIFEST_FIELDS:
        if normalized_fields.get(key) is not True:
            raise ValueError(f"manifest must declare {key!r} as a required field")

    defaults = payload.get("defaults")
    if not isinstance(defaults, dict):
        raise ValueError("manifest 'defaults' must be an object")
    _validate_counts(
        "defaults",
        defaults.get("warmup", 0),
        defaults.get("samples", 1),
        defaults.get("trials", 1),
    )
    timeout = defaults.get("timeout_seconds", 120)
    if not isinstance(timeout, (int, float)) or isinstance(timeout, bool) or timeout <= 0:
        raise ValueError("defaults.timeout_seconds must be a positive number")
    order = str(defaults.get("order", "fixed"))
    if order not in VALID_ORDERS:
        raise ValueError(
            f"defaults.order must be one of {sorted(VALID_ORDERS)}, got {order!r}"
        )
    seed = defaults.get("seed", 1)
    if not isinstance(seed, int) or isinstance(seed, bool):
        raise ValueError("defaults.seed must be an integer")

    backends: list[Backend] = []
    names: set[str] = set()
    for item in payload.get("backends", []):
        if not isinstance(item, dict) or not item.get("name"):
            raise ValueError("each backend must be an object with a name")
        name = str(item["name"])
        _check_safe_name("backend", name)
        if name in names:
            raise ValueError(f"duplicate backend name: {name}")
        names.add(name)
        backends.append(
            Backend(
                name=name,
                args=_check_args(f"backend {name}", item.get("args")),
                description=str(item.get("description", "")),
                jsonl_support=bool(item.get("jsonl_support", True)),
            )
        )
    if not backends:
        raise ValueError("manifest declares no backends")

    workloads: list[Workload] = []
    names = set()
    for item in payload.get("workloads", []):
        if not isinstance(item, dict) or not item.get("name"):
            raise ValueError("each workload must be an object with a name")
        name = str(item["name"])
        _check_safe_name("workload", name)
        if name in names:
            raise ValueError(f"duplicate workload name: {name}")
        names.add(name)
        workloads.append(
            Workload(
                name=name,
                args=_check_args(f"workload {name}", item.get("args")),
                description=str(item.get("description", "")),
            )
        )
    if not workloads:
        raise ValueError("manifest declares no workloads")

    return Manifest(
        name=str(payload.get("name", path.stem)),
        description=str(payload.get("description", "")),
        fields=normalized_fields,
        defaults=dict(defaults),
        backends=tuple(backends),
        workloads=tuple(workloads),
        path=path,
        sha256=hashlib.sha256(path.read_bytes()).hexdigest(),
    )


def resolve_backends(
    manifest: Manifest, requested: Sequence[str] | None
) -> tuple[Backend, ...]:
    if not requested:
        return manifest.backends
    by_name = {backend.name: backend for backend in manifest.backends}
    result = []
    for name in requested:
        if name not in by_name:
            raise ValueError(
                f"unknown backend {name!r}; manifest {manifest.path} declares: "
                f"{', '.join(sorted(by_name))}"
            )
        result.append(by_name[name])
    return tuple(result)


def resolve_workloads(
    manifest: Manifest, requested: Sequence[str] | None
) -> tuple[Workload, ...]:
    if not requested:
        return manifest.workloads
    by_name = {workload.name: workload for workload in manifest.workloads}
    result = []
    for name in requested:
        if name not in by_name:
            raise ValueError(
                f"unknown workload {name!r}; manifest {manifest.path} declares: "
                f"{', '.join(sorted(by_name))}"
            )
        result.append(by_name[name])
    return tuple(result)


def validate_run_config(
    backends: Sequence[Backend],
    trials: int,
    warmup: int,
    samples: int,
    timeout: float,
    order: str,
) -> None:
    if not backends:
        raise ValueError("at least one backend is required")
    _validate_counts("run", warmup, samples, trials)
    if not isinstance(timeout, (int, float)) or isinstance(timeout, bool) or timeout <= 0:
        raise ValueError(f"timeout must be a positive number, got {timeout!r}")
    if order not in VALID_ORDERS:
        raise ValueError(
            f"order must be one of {sorted(VALID_ORDERS)}, got {order!r}"
        )


def build_command(
    engine: Path,
    backend: Backend,
    workload: Workload,
    engine_args: Sequence[str] = (),
) -> list[str]:
    """API-neutral command: engine executable + optional engine prefix +
    backend args + workload args.

    engine_args is the opt-in real-engine prefix (fs_basepath/fs_homepath
    and safe cvar pairs); it defaults to empty so the plain concatenation
    contract is unchanged.
    """
    return (
        [str(engine)]
        + list(engine_args)
        + list(backend.args)
        + list(workload.args)
    )


def plan_order(
    backend_names: Sequence[str], trials: int, order: str, seed: int
) -> list[str]:
    """Return the backend execution order as a flat list of names.

    fixed:   each backend runs all of its trials consecutively (A A A B B B).
    random:  the fixed multiset shuffled with the given seed (reproducible).
    abba:    exactly two backends, interleaved A B B A A B B A ... (balanced
             for any trial count; requires exactly two backends).
    """
    names = list(backend_names)
    if not names:
        raise ValueError("no backends to order")
    if not isinstance(trials, int) or trials < 1:
        raise ValueError(f"trials must be an integer >= 1, got {trials!r}")

    if order == "fixed":
        return [name for name in names for _ in range(trials)]
    if order == "random":
        plan = [name for name in names for _ in range(trials)]
        random.Random(seed).shuffle(plan)
        return plan
    if order == "abba":
        if len(names) != 2:
            raise ValueError(
                f"ABBA order requires exactly two backends, got {len(names)}"
            )
        first, second = names
        plan: list[str] = []
        for index in range(trials):
            plan.extend((first, second) if index % 2 == 0 else (second, first))
        return plan
    raise ValueError(f"unknown order: {order!r}")


def benchmark_env(backend: str, warmup: int, samples: int) -> dict[str, str]:
    """Environment contract handed to every backend process."""
    return {
        ENV_JSONL: "1",
        ENV_WARMUP: str(warmup),
        ENV_SAMPLES: str(samples),
        ENV_BACKEND: backend,
    }
