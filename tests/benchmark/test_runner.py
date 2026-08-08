from __future__ import annotations

import os
import shutil
import sys
import tempfile
import time
from pathlib import Path
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))

import benchmark_config as config  # noqa: E402
import benchmark_protocol as protocol  # noqa: E402
from run_benchmark import run_trial  # noqa: E402

# Emits warmup + samples lines, but never more than 5 total (so an
# over-request also exercises the early-exit path).
FAKE_EMIT = r"""
import json, os, sys, time
warmup = int(os.environ.get("IOQ3_BENCH_WARMUP", "0"))
samples = int(os.environ.get("IOQ3_BENCH_SAMPLES", "1"))
total = min(warmup + samples, 5)
for i in range(total):
    print(json.dumps({
        "event": "sample",
        "producer_seconds": 0.010 + i * 0.001,
        "finalize_seconds": 0.002,
        "gpu_seconds": 0.009,
    }), flush=True)
    time.sleep(0.005)
"""

# Mirrors the engine's finite JSONL contract (SCR_BenchAfterEndFrame in
# code/client/cl_scrn.c): exactly warmup+samples lines of the shape
# {"event":"sample","producer_seconds":...,"finalize_seconds":...}
# (no gpu_seconds), flushed, then a clean exit.
FAKE_ENGINE = r"""
import json, os, sys
warmup = int(os.environ.get("IOQ3_BENCH_WARMUP", "0"))
samples = int(os.environ.get("IOQ3_BENCH_SAMPLES", "1"))
for i in range(warmup + samples):
    print(json.dumps({
        "event": "sample",
        "producer_seconds": round(0.010 + i * 0.001, 9),
        "finalize_seconds": 0.002,
    }), flush=True)
"""

FAKE_HANG = "import time; time.sleep(60)\n"

FAKE_GARBAGE = (
    "import json, sys\n"
    "print('hello world')\n"
    "print('not json at all')\n"
    "print(json.dumps({'event': 'hello', 'backend': 'fake'}))\n"
)

FAKE_STDERR = "import sys; sys.stderr.write('backend warning\\n'); sys.stderr.flush()\n"

FAKE_OVERFLOW = r"""
import json, os, sys
warmup = int(os.environ.get("IOQ3_BENCH_WARMUP", "0"))
samples = int(os.environ.get("IOQ3_BENCH_SAMPLES", "1"))
for i in range(warmup + samples + 2):
    print(json.dumps({
        "event": "sample",
        "producer_seconds": 0.010,
        "finalize_seconds": 0.002,
    }), flush=True)
"""


class TrialRunnerTests(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = Path(tempfile.mkdtemp(prefix="bench-trial-"))
        self.addCleanup(shutil.rmtree, self._tmp, ignore_errors=True)
        self.backend = config.Backend(name="fake", args=("-c", FAKE_EMIT))
        self.workload = config.Workload(name="unit", args=())
        self.engine = Path(sys.executable)
        self.env_base = os.environ.copy()

    def run_trial(
        self,
        warmup: int,
        samples: int,
        timeout: float = 30.0,
        backend: config.Backend | None = None,
        workload: config.Workload | None = None,
        label: str = "trial",
    ) -> dict[str, object]:
        environment = self.env_base.copy()
        environment.update(config.benchmark_env("fake", warmup, samples))
        return run_trial(
            self.engine,
            backend or self.backend,
            workload or self.workload,
            warmup,
            samples,
            timeout,
            self._tmp / label,
            environment,
        )

    def test_samples_measured_and_classified(self) -> None:
        outcome = self.run_trial(warmup=2, samples=3)
        self.assertIsNone(outcome["error"])
        self.assertIsNone(outcome["warning"])
        self.assertEqual(outcome["exit_code"], 0)
        self.assertEqual(outcome["warmup_count"], 2)
        self.assertEqual(outcome["measured_count"], 3)
        self.assertEqual(outcome["extra_count"], 0)
        phases = [phase for phase, _ in outcome["entries"]]
        self.assertEqual(phases, ["warmup", "warmup", "sample", "sample", "sample"])
        measured = [s for p, s in outcome["entries"] if p == "sample"]
        expected = [0.012, 0.013, 0.014]
        self.assertEqual(len(measured), len(expected))
        for sample, value in zip(measured, expected):
            self.assertAlmostEqual(sample.producer_seconds, value, places=6)
        stdout_text = Path(outcome["stdout"]).read_text(encoding="utf-8")
        self.assertEqual(len(stdout_text.strip().splitlines()), 5)

    def test_early_exit_is_an_error(self) -> None:
        outcome = self.run_trial(warmup=0, samples=10)
        self.assertIsNotNone(outcome["error"])
        self.assertIn("exited early", outcome["error"])
        self.assertEqual(outcome["measured_count"], 5)

    def test_extra_lines_are_counted_not_measured(self) -> None:
        backend = config.Backend(name="overflow", args=("-c", FAKE_OVERFLOW))
        outcome = self.run_trial(warmup=1, samples=2, backend=backend)
        self.assertIsNone(outcome["error"])
        self.assertEqual(outcome["warmup_count"], 1)
        self.assertEqual(outcome["measured_count"], 2)
        self.assertEqual(outcome["extra_count"], 2)

    def test_gpu_optional_missing_field_is_fine(self) -> None:
        backend = config.Backend(name="overflow", args=("-c", FAKE_OVERFLOW))
        outcome = self.run_trial(warmup=0, samples=2, backend=backend)
        self.assertIsNone(outcome["error"])
        measured = [s for p, s in outcome["entries"] if p == "sample"]
        self.assertTrue(all(s.gpu_seconds is None for s in measured))

    def test_timeout_kills_process_tree_promptly(self) -> None:
        backend = config.Backend(name="hang", args=("-c", FAKE_HANG))
        start = time.monotonic()
        outcome = self.run_trial(
            warmup=0, samples=5, timeout=1.0, backend=backend, label="hang"
        )
        elapsed = time.monotonic() - start
        self.assertTrue(outcome["timed_out"])
        self.assertIsNotNone(outcome["error"])
        self.assertEqual(outcome["measured_count"], 0)
        self.assertIsNotNone(outcome["exit_code"])
        # Killed promptly; never waited for the 60s sleep.
        self.assertLess(elapsed, 20.0)

    def test_non_json_stdout_counted_as_invalid(self) -> None:
        backend = config.Backend(name="garbage", args=("-c", FAKE_GARBAGE))
        outcome = self.run_trial(
            warmup=0, samples=5, backend=backend, label="garbage"
        )
        self.assertEqual(outcome["invalid_count"], 2)
        self.assertEqual(outcome["ignored_count"], 1)
        self.assertIsNotNone(outcome["error"])

    def test_stderr_captured_to_file(self) -> None:
        backend = config.Backend(name="stderr", args=("-c", FAKE_STDERR))
        outcome = self.run_trial(
            warmup=0, samples=1, backend=backend, label="stderr"
        )
        self.assertIn(
            "backend warning", Path(outcome["stderr"]).read_text(encoding="utf-8")
        )

    def test_zero_warmup_is_allowed(self) -> None:
        outcome = self.run_trial(warmup=0, samples=3)
        self.assertIsNone(outcome["error"])
        self.assertEqual(outcome["warmup_count"], 0)
        self.assertEqual(outcome["measured_count"], 3)

    def test_finite_engine_contract_clean_termination(self) -> None:
        # A backend that emits exactly warmup+samples lines and then quits
        # cleanly (the engine's finite JSONL contract) must be a fully
        # successful trial: no error, no warning, exit code 0.
        backend = config.Backend(name="engine", args=("-c", FAKE_ENGINE))
        outcome = self.run_trial(
            warmup=3, samples=5, backend=backend, label="engine"
        )
        self.assertIsNone(outcome["error"])
        self.assertIsNone(outcome["warning"])
        self.assertEqual(outcome["exit_code"], 0)
        self.assertFalse(outcome["timed_out"])
        self.assertEqual(outcome["warmup_count"], 3)
        self.assertEqual(outcome["measured_count"], 5)
        self.assertEqual(outcome["extra_count"], 0)
        phases = [phase for phase, _ in outcome["entries"]]
        self.assertEqual(phases, ["warmup"] * 3 + ["sample"] * 5)
        measured = [s for p, s in outcome["entries"] if p == "sample"]
        self.assertTrue(all(s.gpu_seconds is None for s in measured))

    def test_finite_engine_contract_line_shape(self) -> None:
        # Every engine line carries exactly {event, producer_seconds,
        # finalize_seconds} with finite, non-negative values and never a
        # gpu_seconds key; each must parse as a protocol sample.
        backend = config.Backend(name="engine", args=("-c", FAKE_ENGINE))
        outcome = self.run_trial(
            warmup=1, samples=2, backend=backend, label="engine-shape"
        )
        lines = Path(outcome["stdout"]).read_text(encoding="utf-8").splitlines()
        self.assertEqual(len(lines), 3)
        for line in lines:
            result = protocol.parse_timing_line(line)
            self.assertEqual(result.kind, "sample")
            self.assertEqual(
                set(result.sample.raw),
                {"event", "producer_seconds", "finalize_seconds"},
            )
            self.assertIsNone(result.sample.gpu_seconds)
            self.assertGreaterEqual(result.sample.producer_seconds, 0.0)
            self.assertGreaterEqual(result.sample.finalize_seconds, 0.0)


if __name__ == "__main__":
    unittest.main()
