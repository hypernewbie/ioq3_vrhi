"""JSONL timing protocol between the benchmark harness and backend commands.

The harness launches a backend command in a fresh process and reads JSON
lines from its stdout. A timing line looks like:

    {"event": "sample", "producer_seconds": 12.34,
     "finalize_seconds": 0.51, "gpu_seconds": 11.98}

Rules:

- `producer_seconds` and `finalize_seconds` are required, finite, >= 0.
- `gpu_seconds` is optional (the harness never assumes GPU timing exists).
- Lines with `"event": "hello"` (or any other non-sample event) are ignored
  as long as they carry no timing fields.
- Any other JSON object, non-object JSON, or non-JSON text is recorded as
  an invalid line (protocol violation) and counted.
- Backends must flush stdout after every line.

The harness stays API-neutral: it communicates only through environment
variables and this stdout contract, never through renderer internals:

    IOQ3_BENCH_JSONL=1          enable JSONL timing output
    IOQ3_BENCH_WARMUP=N         warmup samples the backend should run
    IOQ3_BENCH_SAMPLES=N        measured samples the backend should run
    IOQ3_BENCH_BACKEND=name     backend name for provenance
"""

from __future__ import annotations

import json
import math
from dataclasses import dataclass

ENV_JSONL = "IOQ3_BENCH_JSONL"
ENV_WARMUP = "IOQ3_BENCH_WARMUP"
ENV_SAMPLES = "IOQ3_BENCH_SAMPLES"
ENV_BACKEND = "IOQ3_BENCH_BACKEND"

SAMPLE_EVENT = "sample"

# Field names are the API-neutral timing contract; they must match the
# manifest's declared fields.
TIMING_FIELDS = ("producer_seconds", "finalize_seconds", "gpu_seconds")
REQUIRED_TIMING_FIELDS = ("producer_seconds", "finalize_seconds")


@dataclass(frozen=True)
class Sample:
    producer_seconds: float
    finalize_seconds: float
    gpu_seconds: float | None
    raw: dict[str, object]


@dataclass(frozen=True)
class LineResult:
    kind: str  # "sample", "ignored", "invalid", or "empty"
    sample: Sample | None = None
    message: str = ""


def _valid_timing(value: object) -> bool:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        return False
    number = float(value)
    return math.isfinite(number) and number >= 0


def parse_timing_line(line: str) -> LineResult:
    """Classify one stdout line from a backend command."""
    text = line.strip()
    if not text:
        return LineResult("empty")
    try:
        payload = json.loads(text)
    except ValueError:
        return LineResult("invalid", message="line is not valid JSON")
    if not isinstance(payload, dict):
        return LineResult("invalid", message="JSON line is not an object")

    if payload.get("event") != SAMPLE_EVENT:
        if any(key in payload for key in TIMING_FIELDS):
            return LineResult(
                "invalid",
                message=f"timing fields present but event is {payload.get('event')!r}",
            )
        return LineResult("ignored", message="non-sample event")

    missing = [key for key in REQUIRED_TIMING_FIELDS if key not in payload]
    if missing:
        return LineResult(
            "invalid",
            message=f"missing required field(s): {', '.join(missing)}",
        )
    for key in REQUIRED_TIMING_FIELDS:
        if not _valid_timing(payload[key]):
            return LineResult(
                "invalid", message=f"invalid {key}: {payload[key]!r}"
            )
    if "gpu_seconds" in payload and not _valid_timing(payload["gpu_seconds"]):
        return LineResult(
            "invalid", message=f"invalid gpu_seconds: {payload['gpu_seconds']!r}"
        )

    gpu = payload.get("gpu_seconds")
    return LineResult(
        "sample",
        sample=Sample(
            producer_seconds=float(payload["producer_seconds"]),
            finalize_seconds=float(payload["finalize_seconds"]),
            gpu_seconds=None if gpu is None else float(gpu),
            raw=dict(payload),
        ),
    )
