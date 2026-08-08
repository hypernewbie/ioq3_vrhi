# ioq3 benchmark harness (API-neutral scaffold)

This is the first, deliberately API-neutral benchmark harness for the
OpenGL 2 and VRHI backends. It is independent of the image smoke tests in
`tests/image/`.

The harness knows nothing about renderer internals, cvars, or window
policies. It only:

1. launches a backend command in a fresh process per trial,
2. hands the process a documented environment contract,
3. consumes JSONL timing lines from the backend's stdout,
4. discards explicit warmup samples, keeps measured samples,
5. computes median/mean/p90/p95/p99/MAD/min/max and per-trial spread,
6. writes raw JSONL plus a summary report under `temp/benchmark/` (ignored).

**No GL2/VRHI equivalence is claimed.** This harness reports raw timings
only; it is not a correctness or parity oracle. Both manifest backends
(`opengl2`, `vrhi`) declare `jsonl_support: true`: the engine emits a
finite JSONL stream (exactly `warmup + samples` sample lines, then a
normal quit) when `IOQ3_BENCH_JSONL` is set. The harness itself is
validated by unit tests and fake backends (see `test_runner.py`).

## The backend contract

The harness communicates exclusively through environment variables:

| Variable              | Meaning                                             |
| --------------------- | --------------------------------------------------- |
| `IOQ3_BENCH_JSONL`    | `1` when JSONL timing output is enabled             |
| `IOQ3_BENCH_WARMUP`   | warmup samples the backend should run (may be 0)    |
| `IOQ3_BENCH_SAMPLES`  | measured samples the backend should run             |
| `IOQ3_BENCH_BACKEND`  | backend name, for provenance                        |

The backend writes one JSON object per line to stdout (flushed after every
line):

```json
{"event": "sample", "producer_seconds": 12.34, "finalize_seconds": 0.51, "gpu_seconds": 11.98}
```

- `producer_seconds` and `finalize_seconds` are required, finite, >= 0.
- `gpu_seconds` is optional. The harness never assumes GPU timing exists.
- `{"event": "hello", ...}` lines are ignored.
- Non-JSON lines and JSON lines that violate the schema are counted as
  invalid protocol lines (see `lines` in the summary).
- The first `warmup` sample lines of each process are warmup; the next
  `samples` are measured; anything after that is extra (counted, excluded
  from statistics).

## Commands

List manifest contents (no run):

```text
python tests/benchmark/run_benchmark.py --list-backends
python tests/benchmark/run_benchmark.py --list-workloads
```

Run with ABBA backend order (requires exactly two backends):

```text
python tests/benchmark/run_benchmark.py \
  --engine build/llvm-clangcl-release/Release/ioquake3.exe \
  --backend opengl2 --backend vrhi \
  --workload demo-frame \
  --order abba --trials 3 --warmup 3 --samples 15 --timeout 120
```

Run with seeded randomized backend order:

```text
python tests/benchmark/run_benchmark.py \
  --engine build/llvm-clangcl-release/Release/ioquake3.exe \
  --order random --seed 7 --trials 5 --warmup 3 --samples 20
```

Unit tests (dependency-free, `unittest`):

```text
python -m unittest discover -s tests/benchmark -p "test_*.py" -v
```

## Order modes

- `fixed` — each backend runs all of its trials consecutively (A A A B B B).
- `random` — the same multiset shuffled with `--seed` (reproducible).
- `abba` — exactly two backends, interleaved A B B A A B B A ... (balanced
  for any trial count).

## Outputs

Every run goes to a fresh `temp/benchmark/<timestamp>-<pid>/` directory:

- `raw.jsonl` — every measured/warmup/extra sample line with provenance
  (`backend`, `workload`, `trial`, `phase`) plus the backend's own fields.
- `summary.json` — per backend/workload cell: trial counts, errors and
  warnings, line accounting, and per-field statistics.
- `logs/<backend>__<workload>/trial-<n>/stdout.log` — verbatim stdout.
- `logs/<backend>__<workload>/trial-<n>/stderr.log` — verbatim stderr.

Statistics per timing field: `n`, `min`, `max`, `mean`, `median`, `p90`,
`p95`, `p99`, unscaled `mad` (median absolute deviation), per-trial means,
and `trial_spread` = `(max trial mean - min trial mean) / overall mean`
(`null` when fewer than two completed trials or a zero mean). Percentiles
use the nearest-rank method.

## Safety

- Every trial has a wall-clock timeout; on timeout the complete process
  tree is killed (`taskkill /T /F` on Windows, terminate/kill elsewhere).
- Processes are created in a fresh process group on Windows and are always
  cleaned up, including on exceptions and Ctrl-C.
- The harness deliberately implements **no visible-window policy**; backend
  commands own their window behavior (windowless modes, sound disabling,
  etc. belong in backend/workload args).
- All outputs are written below the git-ignored `temp/` directory.

## The immutable workload manifest

`workloads.json` is a pinned, read-only contract (schema 1, `immutable:
true`). The runner never modifies it, resolves every backend/workload name
against it, and records its SHA-256 in every summary report. Changing it
requires a deliberate schema bump and review; machine-specific paths belong
on the command line (`--engine`), never in the manifest.

## Limitations

- The engine's JSONL stream is finite and self-terminating: it emits
  exactly `warmup + samples` sample lines (startup and re-entered frames
  are skipped), then requests a normal quit. A backend that hangs, buffers
  its stdout, or exits early still fails the trial.
- Backends must flush stdout after every JSONL line; the harness cannot
  unblock a backend that buffers its output.
- A trial's samples are kept even when the backend exits non-zero (a
  warning is recorded); samples are dropped only on timeout or early exit.
- ABBA order requires exactly two backends.
- Trial order within a workload is planned across backends; with multiple
  workloads each workload is benchmarked independently.
