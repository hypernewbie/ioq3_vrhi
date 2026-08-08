# VRHI artifact sync

`tools/sync_vrhi_artifacts.py` copies already-built VRHI dependency/library
artifacts from this repository's VRHI artifact root into a sibling ioquake3
worktree, verifies the expected artifact names and their SHA-256 hashes
after the copy, and writes a JSON hash manifest next to the copied
artifacts.

The tool is a plain Python 3 script using only the standard library. It
does not configure or invoke CMake and never writes into the VRHI
submodule.

## Prerequisites

- The VRHI dependencies must already be built: the VRHI artifact root must
  contain `lib/win_llvm_md_debug/` and/or `lib/win_llvm_md_release/` with
  the dependency libraries, one VRHI static library build tree, and the
  `.vdeps-state.json` state file.
- The destination ioquake3 worktree must already exist as a directory (the
  tool creates the artifact subdirectories inside it, not the worktree
  itself).

## Artifacts

The tool copies exactly this fixed list (paths relative to the artifact
root); `<config>` is `debug` or `release`:

| Artifact | Source | Destination (under the destination worktree) |
| --- | --- | --- |
| Dependency libraries (6 files) | `lib/win_llvm_md_<config>/` | `code/thirdparty/vrhi/lib/win_llvm_md_<config>/` |
| VRHI static library | `build/windows-llvm-md-release/vrhi_md.lib` or `build/windows-llvm-md-debug/vrhi_mdd.lib` | same relative path under `code/thirdparty/vrhi/` |
| Dependency build state | `.vdeps-state.json` | `code/thirdparty/vrhi/.vdeps-state.json` |
| Hash manifest (written) | — | `code/thirdparty/vrhi/vrhi-manifest.json` |

The destination layout mirrors the ioq3_vrhi repository layout, where the
VRHI submodule lives at `code/thirdparty/vrhi`.

The selected configuration must contain exactly one `vrhi_*.lib`. The
standalone VRHI CMake target names the release archive `vrhi_md.lib` and the
Debug archive `vrhi_mdd.lib`; an extra stale archive in the selected build
directory is refused as ambiguous. Release and debug directories may both
exist because `--config` selects one explicitly.

## Usage

```
python tools/sync_vrhi_artifacts.py [options]
```

Defaults:

- `--source-root` defaults to `code/thirdparty/vrhi` in this repository
  (the VRHI artifact root).
- `--destination-root` defaults to the sibling directory `ioq3` next to
  this repository (the sibling ioquake3 worktree).
- `--config` defaults to `release`; choices are `debug` and `release`.

Examples:

```
# Copy release artifacts into the sibling worktree and write the manifest.
python tools/sync_vrhi_artifacts.py

# Copy debug dependency libraries (plus the static library and state file).
python tools/sync_vrhi_artifacts.py --config debug

# Validate sources and print the plan without writing anything.
python tools/sync_vrhi_artifacts.py --dry-run

# Overwrite destination files that differ from the source.
python tools/sync_vrhi_artifacts.py --force

# Explicit roots.
python tools/sync_vrhi_artifacts.py \
    --source-root D:/src/vrhi-artifacts --destination-root D:/src/ioq3 \
    --config release
```

### Options

| Option | Meaning |
| --- | --- |
| `--source-root PATH` | VRHI artifact root to copy from. |
| `--destination-root PATH` | Destination ioquake3 worktree root. Must exist. |
| `--config debug\|release` | Which dependency library configuration to copy. |
| `--dry-run` | Validate sources, classify every destination file, print the plan; write nothing. Missing or ambiguous sources still fail. |
| `--force` | Overwrite destination files whose content differs from the source. |

## Behavior

- **Missing sources are refused.** Any expected artifact that does not
  exist under the source root aborts the run with an error listing every
  missing path, before anything is written.
- **Ambiguous sources are refused.** More than one VRHI static library
  variant aborts the run.
- **Destination conflicts are refused without `--force`.** A destination
  file that exists with different content aborts the run and lists every
  conflicting path; `--force` overwrites them. Files that already match
  are reported as up to date and left untouched (re-runs are idempotent).
- **Destination directories are created** as needed below the destination
  worktree root.
- **Verification.** After copying, every expected artifact name is checked
  to exist at its exact destination path and its SHA-256 is compared
  against the source hash. Any mismatch is an error.
- **Manifest.** `vrhi-manifest.json` is written atomically (temporary file
  plus rename) and records the schema, config, source/destination roots,
  creation time, and one entry per artifact with path, size, and SHA-256.

### Safety guarantees

- Only the explicitly listed artifact files are ever copied. Build trees
  are never copied as trees (`CMakeCache.txt`, `build.ninja`, object
  files, ...) and VRHI source files (`include/`, `CMakeLists.txt`, ...)
  are never copied, even when the source root also contains them.
- The tool refuses to run when source and destination roots resolve to the
  same directory, when the destination root does not exist, or when the
  source root does not look like a VRHI artifact root (missing `lib/` or
  `.vdeps-state.json`).
- `--force` never bypasses missing/ambiguous source validation; it only
  permits overwriting differing destination files.

## Manifest format

```json
{
  "schema": 1,
  "config": "release",
  "source_root": "D:/code/github/ioq3_vrhi_C/code/thirdparty/vrhi",
  "destination_root": "D:/code/github/ioq3",
  "created_at": "2026-08-08T07:20:05+00:00",
  "files": [
    {"path": "lib/win_llvm_md_release/nvrhi.lib", "size": 3683774, "sha256": "..."},
    {"path": "lib/win_llvm_md_release/vk-bootstrap.lib", "size": 7181718, "sha256": "..."}
  ]
}
```

## Testing

```
python -m unittest discover -s tests -p "test_sync_vrhi_artifacts.py" -v
```

The tests are dependency-free: they build fake small artifact trees in
temporary directories and never touch the real VRHI build artifacts or the
VRHI submodule.
