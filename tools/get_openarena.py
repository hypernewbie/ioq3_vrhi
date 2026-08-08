#!/usr/bin/env python3
"""Fetch pinned OpenArena 0.8.8 assets and optionally launch ioquake3.

The asset mirror is pinned to a commit, so a mutable download cannot silently
change the test inputs. Files are stored below temp/, which is gitignored.

Examples:
    python tools/get_openarena.py download
    python tools/get_openarena.py run --engine build/ioquake3.exe
    python tools/get_openarena.py run -- --set cl_renderer opengl2
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time
import urllib.error
import urllib.request
import zipfile

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_ASSET_DIR = ROOT / "temp" / "assets" / "openarena-0.8.8"
DEFAULT_HOME_DIR = ROOT / "temp" / "openarena-home"

# OpenArena-Ioq3/oa-assets is a mirror of the GPL OpenArena 0.8.8 asset set.
# Keep the commit and Git blob hashes pinned. The hash is verified as a Git
# blob hash (SHA-1 over "blob <size>\\0<bytes>") in addition to SHA-256 being
# recorded in the local manifest.
ASSET_REPOSITORY = "OpenArena-Ioq3/oa-assets"
ASSET_COMMIT = "dce830d0b44d0d23766803bf2820594f3d69cda6"
ASSETS = (
    ("pak0.pk3", 38138505, "45902fdb5f6bf4985194371deae92480edc77f46"),
    ("pak1-maps.pk3", 38421794, "8cb9f8f886a60067c1325b4130d6d9d47d3bbf1f"),
    ("pak2-players-mature.pk3", 26754265, "15324dc696fbe57c632f9507941c3051efdc5163"),
    ("pak2-players.pk3", 74389371, "e10404f0cd7b1673eadbffef6116f99dad49c3e9"),
    ("pak4-textures.pk3", 97077243, "62c36beb6e852784cb2dd826814b6e711ae3e212"),
    ("pak5-TA.pk3", 2907315, "c9a09dee96011cc6225fccab458ee952d6b80df0"),
    ("pak6-misc.pk3", 24912892, "fd8dda16d877c76a88e716f7a73f8841c8760993"),
    ("pak6-patch085.pk3", 36972040, "f5e94f10c6e8e964bab6ddab10ee944572c0d3f3"),
    ("pak6-patch088.pk3", 70645224, "c7fd82d3f8ce0b851147850b8770d9dc00be8437"),
)


def git_blob_sha1(path: Path, size: int) -> str:
    digest = hashlib.sha1()
    digest.update(f"blob {size}\0".encode("ascii"))
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def valid_asset(path: Path, expected_size: int, expected_git_sha1: str) -> bool:
    return (
        path.is_file()
        and path.stat().st_size == expected_size
        and git_blob_sha1(path, expected_size) == expected_git_sha1
    )


def download_file(url: str, destination: Path, expected_size: int) -> None:
    partial = destination.with_suffix(destination.suffix + ".part")
    if partial.exists():
        partial.unlink()

    request = urllib.request.Request(
        url,
        headers={"User-Agent": "ioq3-vrhi-openarena-fetch/1"},
    )
    for attempt in range(1, 4):
        try:
            with urllib.request.urlopen(request, timeout=60) as response:
                content_length = response.headers.get("Content-Length")
                if content_length and int(content_length) != expected_size:
                    raise RuntimeError(
                        f"unexpected size for {url}: {content_length} != {expected_size}"
                    )
                read = 0
                last_report = time.monotonic()
                with partial.open("wb") as output:
                    while True:
                        chunk = response.read(1024 * 1024)
                        if not chunk:
                            break
                        output.write(chunk)
                        read += len(chunk)
                        now = time.monotonic()
                        if now - last_report >= 2.0:
                            print(
                                f"    {destination.name}: {read / 1048576:.1f} MiB / "
                                f"{expected_size / 1048576:.1f} MiB",
                                flush=True,
                            )
                            last_report = now
                if read != expected_size:
                    raise RuntimeError(
                        f"short download for {url}: {read} != {expected_size}"
                    )
            partial.replace(destination)
            return
        except (OSError, urllib.error.URLError, RuntimeError) as error:
            if partial.exists():
                partial.unlink()
            if attempt == 3:
                raise RuntimeError(f"failed to download {url}: {error}") from error
            print(f"    download attempt {attempt} failed: {error}; retrying")
            time.sleep(attempt * 2)


def ensure_assets(asset_dir: Path) -> Path:
    baseoa = asset_dir / "baseoa"
    baseoa.mkdir(parents=True, exist_ok=True)
    print(f"[OpenArena] Asset directory: {baseoa}")

    for filename, expected_size, expected_git_sha1 in ASSETS:
        destination = baseoa / filename
        if valid_asset(destination, expected_size, expected_git_sha1):
            print(f"  verified {filename}")
            continue

        if destination.exists():
            print(f"  replacing invalid {filename}")
            destination.unlink()
        url = (
            f"https://raw.githubusercontent.com/{ASSET_REPOSITORY}/"
            f"{ASSET_COMMIT}/baseoa/{filename}"
        )
        print(f"  downloading {filename} ({expected_size / 1048576:.1f} MiB)")
        download_file(url, destination, expected_size)
        actual = git_blob_sha1(destination, expected_size)
        if actual != expected_git_sha1:
            destination.unlink(missing_ok=True)
            raise RuntimeError(
                f"integrity failure for {filename}: Git blob {actual}, "
                f"expected {expected_git_sha1}"
            )
        with zipfile.ZipFile(destination) as archive:
            bad_member = archive.testzip()
            if bad_member is not None:
                destination.unlink(missing_ok=True)
                raise RuntimeError(f"corrupt PK3 {filename}, first bad entry: {bad_member}")
        print(f"    verified {filename}")

    manifest = {
        "schema": 1,
        "repository": ASSET_REPOSITORY,
        "commit": ASSET_COMMIT,
        "asset_directory": str(baseoa),
        "files": [
            {
                "name": filename,
                "size": expected_size,
                "git_blob_sha1": expected_git_sha1,
                "sha256": sha256(baseoa / filename),
            }
            for filename, expected_size, expected_git_sha1 in ASSETS
        ],
    }
    manifest_path = asset_dir / "manifest.json"
    temporary_manifest = manifest_path.with_suffix(".json.tmp")
    temporary_manifest.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    temporary_manifest.replace(manifest_path)
    print(f"[OpenArena] Assets ready: {baseoa}")
    return asset_dir


def find_engine(requested: str | None) -> Path:
    if requested:
        engine = Path(requested).expanduser()
        if not engine.is_absolute():
            engine = (Path.cwd() / engine).resolve()
        if not engine.is_file():
            raise FileNotFoundError(f"engine executable not found: {engine}")
        return engine

    environment = os.environ.get("IOQ3_EXECUTABLE")
    candidates = [Path(environment)] if environment else []
    candidates += [
        ROOT / "build" / "ioquake3.exe",
        ROOT / "build" / "ioquake3",
        ROOT / "build" / "Release" / "ioquake3.exe",
        ROOT / "build" / "Release" / "ioquake3",
        ROOT / "build" / "Debug" / "ioquake3.exe",
        ROOT / "build" / "Debug" / "ioquake3",
    ]
    for candidate in candidates:
        if candidate and candidate.is_file():
            return candidate.resolve()
    raise FileNotFoundError(
        "ioquake3 executable not found; pass --engine PATH or set IOQ3_EXECUTABLE"
    )


def run_engine(args: argparse.Namespace, extra_args: list[str]) -> int:
    asset_dir = ensure_assets(args.asset_dir.expanduser().resolve())
    engine = find_engine(args.engine)
    home_dir = args.home_dir.expanduser().resolve()
    home_dir.mkdir(parents=True, exist_ok=True)

    command = [
        str(engine),
        "+set",
        "fs_basepath",
        str(asset_dir),
        "+set",
        "com_basegame",
        "baseoa",
        "+set",
        "fs_homepath",
        str(home_dir),
    ]
    if args.renderer:
        command += ["+set", "cl_renderer", args.renderer]
    command += extra_args
    print("[OpenArena] Launching:")
    print("  " + " ".join(f'"{part}"' if " " in part else part for part in command))
    return subprocess.call(command, cwd=engine.parent)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=False)

    for name in ("download", "run"):
        subparser = subparsers.add_parser(name)
        subparser.add_argument(
            "--asset-dir",
            type=Path,
            default=DEFAULT_ASSET_DIR,
            help=f"asset root containing baseoa (default: {DEFAULT_ASSET_DIR})",
        )

    run_parser = subparsers.choices["run"]
    run_parser.add_argument("--engine", help="path to ioquake3 executable")
    run_parser.add_argument("--home-dir", type=Path, default=DEFAULT_HOME_DIR)
    run_parser.add_argument("--renderer", help="renderer cvar, for example opengl2")
    run_parser.add_argument(
        "engine_args",
        nargs=argparse.REMAINDER,
        help="arguments passed to ioquake3 after --",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if args.command in (None, "download"):
        ensure_assets(args.asset_dir)
        return 0

    extra_args = args.engine_args
    if extra_args[:1] == ["--"]:
        extra_args = extra_args[1:]
    return run_engine(args, extra_args)


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (FileNotFoundError, RuntimeError, zipfile.BadZipFile) as error:
        print(f"[OpenArena] ERROR: {error}", file=sys.stderr)
        raise SystemExit(1)
