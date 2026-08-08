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


def build_command(engine: Path, backend: Backend, workload: Workload) -> list[str]:
    """API-neutral command: engine executable + backend args + workload args."""
    return [str(engine)] + list(backend.args) + list(workload.args)


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
