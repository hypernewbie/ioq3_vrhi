from __future__ import annotations

import hashlib
import json
import shutil
import sys
import tempfile
from pathlib import Path
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))

from benchmark_config import (  # noqa: E402
    Backend,
    Workload,
    benchmark_env,
    build_command,
    load_manifest,
    plan_order,
    resolve_backends,
    resolve_workloads,
    validate_run_config,
)
from benchmark_protocol import (  # noqa: E402
    ENV_BACKEND,
    ENV_JSONL,
    ENV_SAMPLES,
    ENV_WARMUP,
)

MANIFEST_TEMPLATE: dict[str, object] = {
    "schema": 1,
    "name": "unit-test-manifest",
    "description": "fixture",
    "immutable": True,
    "fields": {
        "producer_seconds": True,
        "finalize_seconds": True,
        "gpu_seconds": False,
    },
    "defaults": {
        "warmup": 2,
        "samples": 5,
        "trials": 2,
        "timeout_seconds": 30,
        "order": "fixed",
        "seed": 7,
    },
    "backends": [
        {"name": "backend_a", "args": ["--a"], "description": "A"},
        {"name": "backend_b", "args": ["--b"], "jsonl_support": False},
    ],
    "workloads": [{"name": "workload_x", "args": ["-x", "1"]}],
}


class ManifestFixtureTests(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = Path(tempfile.mkdtemp(prefix="bench-config-"))
        self.addCleanup(shutil.rmtree, self._tmp, ignore_errors=True)

    def write_manifest(self, payload: dict[str, object]) -> Path:
        path = self._tmp / "workloads.json"
        path.write_text(json.dumps(payload), encoding="utf-8")
        return path

    def test_load_valid_manifest(self) -> None:
        path = self.write_manifest(MANIFEST_TEMPLATE)
        manifest = load_manifest(path)
        self.assertEqual(manifest.name, "unit-test-manifest")
        self.assertEqual(
            [backend.name for backend in manifest.backends],
            ["backend_a", "backend_b"],
        )
        self.assertEqual(manifest.backends[0].args, ("--a",))
        self.assertFalse(manifest.backends[1].jsonl_support)
        self.assertEqual(manifest.workloads[0].args, ("-x", "1"))
        self.assertEqual(
            manifest.fields,
            {
                "producer_seconds": True,
                "finalize_seconds": True,
                "gpu_seconds": False,
            },
        )
        self.assertEqual(
            manifest.sha256, hashlib.sha256(path.read_bytes()).hexdigest()
        )

    def test_wrong_schema_rejected(self) -> None:
        payload = dict(MANIFEST_TEMPLATE, schema=2)
        with self.assertRaises(ValueError):
            load_manifest(self.write_manifest(payload))

    def test_non_immutable_rejected(self) -> None:
        payload = dict(MANIFEST_TEMPLATE, immutable=False)
        with self.assertRaises(ValueError):
            load_manifest(self.write_manifest(payload))

    def test_missing_required_field_rejected(self) -> None:
        payload = dict(
            MANIFEST_TEMPLATE,
            fields={"finalize_seconds": True, "gpu_seconds": False},
        )
        with self.assertRaises(ValueError):
            load_manifest(self.write_manifest(payload))

    def test_unknown_field_rejected(self) -> None:
        payload = dict(
            MANIFEST_TEMPLATE,
            fields={
                "producer_seconds": True,
                "finalize_seconds": True,
                "frame_time": True,
            },
        )
        with self.assertRaises(ValueError):
            load_manifest(self.write_manifest(payload))

    def test_invalid_defaults_rejected(self) -> None:
        for overrides in (
            {"samples": 0},
            {"trials": 0},
            {"warmup": -1},
            {"timeout_seconds": 0},
            {"order": "zigzag"},
            {"seed": "seven"},
        ):
            payload = dict(MANIFEST_TEMPLATE)
            payload["defaults"] = dict(MANIFEST_TEMPLATE["defaults"], **overrides)  # type: ignore[arg-type]
            with self.assertRaises(ValueError):
                load_manifest(self.write_manifest(payload))

    def test_duplicate_backend_rejected(self) -> None:
        payload = dict(MANIFEST_TEMPLATE)
        payload["backends"] = [
            {"name": "backend_a", "args": ["--a"]},
            {"name": "backend_a", "args": ["--a2"]},
        ]
        with self.assertRaises(ValueError):
            load_manifest(self.write_manifest(payload))

    def test_unsafe_backend_name_rejected(self) -> None:
        payload = dict(MANIFEST_TEMPLATE)
        payload["backends"] = [{"name": "bad name", "args": ["--a"]}]
        with self.assertRaises(ValueError):
            load_manifest(self.write_manifest(payload))

    def test_empty_workloads_rejected(self) -> None:
        payload = dict(MANIFEST_TEMPLATE, workloads=[])
        with self.assertRaises(ValueError):
            load_manifest(self.write_manifest(payload))

    def test_bad_backend_args_rejected(self) -> None:
        for args in ([""], "not-a-list", 7):
            payload = dict(MANIFEST_TEMPLATE)
            payload["backends"] = [{"name": "backend_a", "args": args}]
            with self.assertRaises(ValueError):
                load_manifest(self.write_manifest(payload))

    def test_empty_args_list_is_allowed(self) -> None:
        payload = dict(MANIFEST_TEMPLATE)
        payload["workloads"] = [{"name": "workload_empty", "args": []}]
        manifest = load_manifest(self.write_manifest(payload))
        self.assertEqual(manifest.workloads[0].args, ())

    def test_loading_never_modifies_manifest(self) -> None:
        path = self.write_manifest(MANIFEST_TEMPLATE)
        before = path.read_bytes()
        load_manifest(path)
        self.assertEqual(path.read_bytes(), before)
        self.assertEqual(len(list(self._tmp.iterdir())), 1)


class ResolutionTests(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = Path(tempfile.mkdtemp(prefix="bench-config-"))
        self.addCleanup(shutil.rmtree, self._tmp, ignore_errors=True)

    def load(self) -> object:
        path = self._tmp / "workloads.json"
        path.write_text(json.dumps(MANIFEST_TEMPLATE), encoding="utf-8")
        return load_manifest(path)

    def test_resolve_backends_default_and_requested(self) -> None:
        manifest = self.load()
        self.assertEqual(
            [b.name for b in resolve_backends(manifest, None)],
            ["backend_a", "backend_b"],
        )
        self.assertEqual(
            [b.name for b in resolve_backends(manifest, ["backend_b"])],
            ["backend_b"],
        )
        with self.assertRaises(ValueError):
            resolve_backends(manifest, ["nope"])

    def test_resolve_workloads_default_and_requested(self) -> None:
        manifest = self.load()
        self.assertEqual(
            [w.name for w in resolve_workloads(manifest, None)],
            ["workload_x"],
        )
        self.assertEqual(
            [w.name for w in resolve_workloads(manifest, ["workload_x"])],
            ["workload_x"],
        )
        with self.assertRaises(ValueError):
            resolve_workloads(manifest, ["missing"])


class RunConfigTests(unittest.TestCase):
    def _backends(self) -> list[Backend]:
        return [Backend(name="a", args=("--a",))]

    def test_valid_config_passes(self) -> None:
        validate_run_config(
            self._backends(), trials=3, warmup=0, samples=10, timeout=5.0, order="fixed"
        )

    def test_invalid_counts_rejected(self) -> None:
        cases = (
            {"trials": 0},
            {"trials": -1},
            {"warmup": -1},
            {"samples": 0},
            {"timeout": 0.0},
            {"timeout": -2.0},
            {"order": "bogus"},
        )
        for overrides in cases:
            with self.assertRaises(ValueError):
                validate_run_config(
                    self._backends(),
                    trials=overrides.get("trials", 3),
                    warmup=overrides.get("warmup", 0),
                    samples=overrides.get("samples", 10),
                    timeout=overrides.get("timeout", 5.0),
                    order=overrides.get("order", "fixed"),
                )

    def test_no_backends_rejected(self) -> None:
        with self.assertRaises(ValueError):
            validate_run_config([], trials=1, warmup=0, samples=1, timeout=5.0, order="fixed")

    def test_build_command_is_neutral_concatenation(self) -> None:
        command = build_command(
            Path("engine.exe"),
            Backend("b", ("+set", "cl_renderer", "opengl2")),
            Workload("w", ("+demo", "d1")),
        )
        self.assertEqual(
            command,
            ["engine.exe", "+set", "cl_renderer", "opengl2", "+demo", "d1"],
        )

    def test_benchmark_env_contract(self) -> None:
        env = benchmark_env("opengl2", warmup=3, samples=15)
        self.assertEqual(
            env,
            {
                ENV_JSONL: "1",
                ENV_WARMUP: "3",
                ENV_SAMPLES: "15",
                ENV_BACKEND: "opengl2",
            },
        )


class OrderPlanningTests(unittest.TestCase):
    def test_fixed_groups_backends(self) -> None:
        self.assertEqual(
            plan_order(["a", "b"], 2, "fixed", 1), ["a", "a", "b", "b"]
        )

    def test_random_is_seeded_permutation(self) -> None:
        first = plan_order(["a", "b", "c"], 2, "random", 42)
        second = plan_order(["a", "b", "c"], 2, "random", 42)
        self.assertEqual(first, second)
        self.assertEqual(sorted(first), ["a", "a", "b", "b", "c", "c"])

    def test_abba_pattern_and_balance(self) -> None:
        self.assertEqual(plan_order(["a", "b"], 1, "abba", 1), ["a", "b"])
        self.assertEqual(plan_order(["a", "b"], 2, "abba", 1), ["a", "b", "b", "a"])
        self.assertEqual(
            plan_order(["a", "b"], 3, "abba", 1), ["a", "b", "b", "a", "a", "b"]
        )
        for trials in range(1, 6):
            plan = plan_order(["a", "b"], trials, "abba", 1)
            self.assertEqual(plan.count("a"), trials)
            self.assertEqual(plan.count("b"), trials)

    def test_abba_requires_exactly_two_backends(self) -> None:
        with self.assertRaises(ValueError):
            plan_order(["a", "b", "c"], 2, "abba", 1)
        with self.assertRaises(ValueError):
            plan_order(["a"], 2, "abba", 1)

    def test_invalid_inputs_rejected(self) -> None:
        with self.assertRaises(ValueError):
            plan_order(["a"], 0, "fixed", 1)
        with self.assertRaises(ValueError):
            plan_order([], 2, "fixed", 1)
        with self.assertRaises(ValueError):
            plan_order(["a"], 2, "zigzag", 1)


if __name__ == "__main__":
    unittest.main()
