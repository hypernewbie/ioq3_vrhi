"""Dependency-free statistics used by the benchmark harness.

Percentiles use the nearest-rank method (deterministic, no interpolation).
MAD is the unscaled median absolute deviation. All functions operate on
finite floats only; empty inputs raise ValueError.

No third-party packages are used.
"""

from __future__ import annotations

import math
from statistics import median as _median
from typing import Iterable


def _values(values: Iterable[float]) -> list[float]:
    result = [float(value) for value in values]
    if not result:
        raise ValueError("statistics require at least one value")
    for value in result:
        if not math.isfinite(value):
            raise ValueError(f"statistics require finite values, got {value!r}")
    return result


def mean(values: Iterable[float]) -> float:
    data = _values(values)
    return sum(data) / len(data)


def median(values: Iterable[float]) -> float:
    return _median(_values(values))


def min_max(values: Iterable[float]) -> tuple[float, float]:
    data = _values(values)
    return min(data), max(data)


def percentile(values: Iterable[float], p: float) -> float:
    """Nearest-rank percentile; p must be in (0, 100]."""
    if not (0 < p <= 100):
        raise ValueError(f"percentile must be in (0, 100], got {p!r}")
    data = sorted(_values(values))
    index = math.ceil(p / 100.0 * len(data)) - 1
    return data[index]


def mad(values: Iterable[float]) -> float:
    """Unscaled median absolute deviation."""
    data = _values(values)
    center = _median(data)
    return _median([abs(value - center) for value in data])


def summarize(values: Iterable[float]) -> dict[str, float | int]:
    """median/mean/p90/p95/p99/MAD/min/max plus n for a value series."""
    data = _values(values)
    return {
        "n": len(data),
        "min": min(data),
        "max": max(data),
        "mean": mean(data),
        "median": median(data),
        "p90": percentile(data, 90),
        "p95": percentile(data, 95),
        "p99": percentile(data, 99),
        "mad": mad(data),
    }


def trial_spread(trial_means: Iterable[float], overall_mean: float) -> float | None:
    """Relative spread of per-trial means: (max - min) / overall_mean.

    Returns None when there are fewer than two trials or the overall mean is
    zero (the ratio would be undefined).
    """
    data = _values(trial_means)
    if len(data) < 2 or overall_mean == 0:
        return None
    return (max(data) - min(data)) / overall_mean
