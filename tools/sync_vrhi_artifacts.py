#!/usr/bin/env python3
"""Sync built VRHI artifacts into a sibling ioquake3 worktree.

Copies the already-built VRHI dependency libraries, the VRHI static
library, and the dependency build state file from this repository's VRHI
artifact root into a sibling ioquake3 worktree, verifies the expected
artifact names and their SHA-256 hashes after the copy, and writes a JSON
hash manifest next to the copied artifacts.

Only the explicitly listed artifacts are copied. The tool never copies
build trees as trees (no CMakeCache.txt, build.ninja, object files, ...)
and never copies VRHI source files: the artifact list is fixed, every
source file is validated before anything is written, and missing or
ambiguous source files are refused with an error.

Artifacts (paths relative to the VRHI artifact root):

    lib/win_llvm_md_<config>/SPIRV-Tools-opt.lib   dependency lib
    lib/win_llvm_md_<config>/SPIRV-Tools.lib       dependency lib
    lib/win_llvm_md_<config>/nvrhi.lib             dependency lib
    lib/win_llvm_md_<config>/nvrhi_vk.lib          dependency lib
    lib/win_llvm_md_<config>/rtxmu.lib             dependency lib
    lib/win_llvm_md_<config>/vk-bootstrap.lib      dependency lib
    build/windows-llvm-md-release/vrhi_md.lib     VRHI static library
    build/windows-llvm-md-debug/vrhi_mdd.lib       VRHI static library
    .vdeps-state.json                              dependency build state

<config> is selected with --config (debug or release). The selected
configuration supplies the VRHI static library; release and debug build
trees may both exist. An extra `vrhi_*.lib` in the selected tree is
ambiguous and is refused.

Destination layout mirrors the ioq3_vrhi repository: artifacts land below
<destination-root>/code/thirdparty/vrhi/, and the hash manifest is
written to <destination-root>/code/thirdparty/vrhi/vrhi-manifest.json.

Examples:
    python tools/sync_vrhi_artifacts.py
    python tools/sync_vrhi_artifacts.py --config debug
    python tools/sync_vrhi_artifacts.py --dry-run
    python tools/sync_vrhi_artifacts.py --force
    python tools/sync_vrhi_artifacts.py \\
        --source-root D:/src/vrhi-artifacts --destination-root D:/src/ioq3
"""

from __future__ import annotations

import argparse
import hashlib
import json
import shutil
import sys
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_SOURCE_ROOT = ROOT / "code" / "thirdparty" / "vrhi"
DEFAULT_DESTINATION_ROOT = ROOT.parent / "ioq3"

CONFIGS = ("debug", "release")

# Dependency libraries produced by the VRHI dependency build. The same
# file names are produced for debug and release; they live below
# lib/win_llvm_md_<config>/. This list is the only thing ever taken from
# the library directories.
DEPENDENCY_LIBRARIES = (
    "SPIRV-Tools-opt.lib",
    "SPIRV-Tools.lib",
    "nvrhi.lib",
    "nvrhi_vk.lib",
    "rtxmu.lib",
    "vk-bootstrap.lib",
)

# VRHI's standalone CMake target uses `vrhi_md.lib` for RelWithDebInfo and
# `vrhi_mdd.lib` for Debug. Only the selected configuration is considered;
# an extra vrhi_*.lib in that same build directory is treated as ambiguous.
STATIC_LIBRARY_NAMES = {
    "debug": "vrhi_mdd.lib",
    "release": "vrhi_md.lib",
}

# Dependency build state file written next to the artifacts.
STATE_FILE = ".vdeps-state.json"

# Destination layout inside the sibling worktree, mirroring where the
# ioq3_vrhi repository keeps the VRHI submodule.
DESTINATION_PREFIX = Path("code") / "thirdparty" / "vrhi"
MANIFEST_NAME = "vrhi-manifest.json"


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def build_plan(source_root: Path, config: str) -> list[tuple[Path, Path]]:
    """Validate sources and return [(dest-relative path, source path)] pairs.

    Raises RuntimeError when the source root is not a VRHI artifact root,
    when any expected artifact is missing, or when the selected VRHI static
    library is ambiguous (more than one variant exists in that build tree).
    """
    if not source_root.is_dir():
        raise RuntimeError(f"--source-root is not a directory: {source_root}")
    if not (source_root / "lib").is_dir() or not (source_root / STATE_FILE).is_file():
        raise RuntimeError(
            f"{source_root} does not look like a VRHI artifact root "
            f"(missing lib/ or {STATE_FILE}); build the VRHI dependencies first"
        )

    plan: list[tuple[Path, Path]] = []
    missing: list[Path] = []
    config_dir = source_root / "lib" / f"win_llvm_md_{config}"
    for name in DEPENDENCY_LIBRARIES:
        rel = Path("lib") / f"win_llvm_md_{config}" / name
        source = config_dir / name
        if source.is_file():
            plan.append((rel, source))
        else:
            missing.append(rel)

    static_dir = source_root / "build" / f"windows-llvm-md-{config}"
    static_name = STATIC_LIBRARY_NAMES[config]
    static = sorted(static_dir.glob("vrhi_*.lib")) if static_dir.is_dir() else []
    expected_static = Path("build") / f"windows-llvm-md-{config}" / static_name
    if len(static) > 1:
        raise RuntimeError(
            "ambiguous VRHI static library in selected configuration:\n  "
            + "\n  ".join(str(path) for path in static)
            + "\nremove the stale variant and re-run"
        )
    if static and static[0].name != static_name:
        raise RuntimeError(
            f"unexpected VRHI static library name in {static_dir}: "
            f"{static[0].name} (expected {static_name})"
        )
    if static:
        plan.append((expected_static, static[0]))
    else:
        missing.append(expected_static)

    plan.append((Path(STATE_FILE), source_root / STATE_FILE))

    if missing:
        raise RuntimeError(
            "missing source artifact(s) (config="
            + config
            + "):\n  "
            + "\n  ".join(str(source_root / rel) for rel in missing)
            + "\nbuild the VRHI dependencies first; build trees are never copied"
        )
    return plan


def verify_copied(artifact_root: Path, plan, source_hashes: dict) -> None:
    """Verify every expected artifact name exists with the expected SHA-256."""
    problems = []
    for rel, _source in plan:
        dest = artifact_root / rel
        if not dest.is_file():
            problems.append(f"missing after copy: {dest}")
        elif sha256(dest) != source_hashes[rel]:
            problems.append(f"SHA-256 mismatch after copy: {dest}")
    if problems:
        raise RuntimeError("verification failed:\n  " + "\n  ".join(problems))


def write_manifest(artifact_root: Path, config: str, source_root: Path,
                   destination_root: Path, plan) -> Path:
    files = []
    for rel, _source in sorted(plan, key=lambda entry: str(entry[0])):
        dest = artifact_root / rel
        files.append(
            {
                "path": rel.as_posix(),
                "size": dest.stat().st_size,
                "sha256": sha256(dest),
            }
        )
    manifest = {
        "schema": 1,
        "config": config,
        "source_root": str(source_root),
        "destination_root": str(destination_root),
        "created_at": datetime.now(timezone.utc).isoformat(timespec="seconds"),
        "files": files,
    }
    path = artifact_root / MANIFEST_NAME
    temporary = path.with_name(path.name + ".tmp")
    temporary.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    temporary.replace(path)
    return path


def warn_unexpected(artifact_root: Path, plan) -> None:
    """Warn about unexpected extra files in the directories we wrote to."""
    expected_names = {rel.name for rel, _source in plan}
    directories = sorted(
        {rel.parent for rel, _source in plan if rel.parent != Path(".")}
    )
    for directory in directories:
        full = artifact_root / directory
        if not full.is_dir():
            continue
        extra = sorted(
            entry.name
            for entry in full.iterdir()
            if entry.is_file() and entry.name not in expected_names
        )
        if extra:
            print(
                f"[sync-vrhi] WARNING: unexpected file(s) in {full}: "
                + ", ".join(extra)
            )


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--source-root",
        type=Path,
        default=DEFAULT_SOURCE_ROOT,
        help=f"VRHI artifact root (default: {DEFAULT_SOURCE_ROOT})",
    )
    parser.add_argument(
        "--destination-root",
        type=Path,
        default=DEFAULT_DESTINATION_ROOT,
        help=f"sibling ioquake3 worktree root (default: {DEFAULT_DESTINATION_ROOT})",
    )
    parser.add_argument(
        "--config",
        choices=CONFIGS,
        default="release",
        help="dependency library configuration to copy (default: release)",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="validate sources and print the plan without writing anything",
    )
    parser.add_argument(
        "--force",
        action="store_true",
        help="overwrite destination files whose content differs from the source",
    )
    return parser.parse_args(argv)


def run(args: argparse.Namespace) -> None:
    source_root = args.source_root.expanduser().resolve()
    destination_root = args.destination_root.expanduser().resolve()

    if source_root == destination_root:
        raise RuntimeError(
            "--source-root and --destination-root resolve to the same "
            f"directory: {source_root}"
        )
    if not destination_root.is_dir():
        raise RuntimeError(
            f"--destination-root is not a directory: {destination_root} "
            "(create the destination worktree first)"
        )
    if (source_root / "CMakeLists.txt").is_file() and (source_root / "include").is_dir():
        print(
            f"[sync-vrhi] WARNING: {source_root} looks like a VRHI source "
            "checkout; only the explicitly listed artifacts are copied"
        )

    plan = build_plan(source_root, args.config)
    artifact_root = destination_root / DESTINATION_PREFIX

    source_hashes = {rel: sha256(source) for rel, source in plan}
    states: dict[Path, str] = {}
    for rel, _source in plan:
        dest = artifact_root / rel
        if not dest.exists():
            states[rel] = "copy"
        elif not dest.is_file():
            raise RuntimeError(
                f"destination exists and is not a regular file: {dest}"
            )
        elif sha256(dest) == source_hashes[rel]:
            states[rel] = "up-to-date"
        else:
            states[rel] = "overwrite"

    if args.dry_run:
        for rel, _source in plan:
            state = states[rel]
            if state == "copy":
                print(f"  would copy  {rel} -> {artifact_root / rel}")
            elif state == "up-to-date":
                print(f"  up to date  {rel}")
            else:
                print(f"  would overwrite {rel} (pass --force)")
        print(
            f"[sync-vrhi] dry run: {len(plan)} artifact(s) validated "
            f"(config={args.config}), no files written"
        )
        print(f"[sync-vrhi] manifest would be: {artifact_root / MANIFEST_NAME}")
        return

    conflicts = sorted(rel for rel, state in states.items() if state == "overwrite")
    if conflicts and not args.force:
        raise RuntimeError(
            "destination file(s) differ from source:\n  "
            + "\n  ".join(str(artifact_root / rel) for rel in conflicts)
            + "\npass --force to overwrite them"
        )

    for rel, source in plan:
        dest = artifact_root / rel
        state = states[rel]
        if state == "up-to-date":
            print(f"  up to date  {rel}")
            continue
        dest.parent.mkdir(parents=True, exist_ok=True)
        print(f"  {'overwriting' if state == 'overwrite' else 'copying'}  {rel}")
        shutil.copy2(source, dest)

    verify_copied(artifact_root, plan, source_hashes)
    manifest_path = write_manifest(
        artifact_root, args.config, source_root, destination_root, plan
    )
    print(
        f"[sync-vrhi] verified {len(plan)} artifact(s) by SHA-256 "
        f"(config={args.config})"
    )
    print(f"[sync-vrhi] manifest written: {manifest_path}")
    warn_unexpected(artifact_root, plan)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    try:
        run(args)
    except (OSError, RuntimeError, ValueError) as error:
        print(f"[sync-vrhi] ERROR: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
