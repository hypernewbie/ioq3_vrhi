"""Unit tests for tools/sync_vrhi_artifacts.py.

Dependency-free: uses only the Python standard library (unittest, tempfile,
hashlib, json). Fake VRHI artifacts are small files written into temporary
directories; the real VRHI build artifacts and the VRHI submodule are never
touched.

Run from the repository root:

    python -m unittest discover -s tests -p "test_sync_vrhi_artifacts.py" -v
"""

from __future__ import annotations

import contextlib
import hashlib
import io
import json
import sys
import tempfile
from pathlib import Path
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))

import sync_vrhi_artifacts as tool  # noqa: E402

STATIC_RELEASE = b"fake-vrhi-static-release"
STATIC_DEBUG = b"fake-vrhi-static-debug"


def fake_lib_content(config: str, name: str) -> bytes:
    return f"fake-{config}-{name}".encode()


def make_artifact_root(parent: Path, configs=("debug", "release")) -> Path:
    """Create a fake VRHI artifact root with small fake artifacts.

    Also drops build-tree internals and source files into the source root;
    the tool must never copy those.
    """
    source = parent / "vrhi"
    (source / "lib").mkdir(parents=True)
    for config in configs:
        (source / "build" / f"windows-llvm-md-{config}").mkdir(parents=True)
        config_dir = source / "lib" / f"win_llvm_md_{config}"
        config_dir.mkdir(parents=True)
        for name in tool.DEPENDENCY_LIBRARIES:
            (config_dir / name).write_bytes(fake_lib_content(config, name))
    for config in configs:
        static_name = tool.STATIC_LIBRARY_NAMES[config]
        static_content = STATIC_RELEASE if config == "release" else STATIC_DEBUG
        (source / "build" / f"windows-llvm-md-{config}" / static_name).write_bytes(
            static_content
        )
    (source / ".vdeps-state.json").write_text(
        json.dumps({"schema": 1, "records": {}}), encoding="utf-8"
    )
    # Build-tree internals and sources that must never be copied.
    (source / "build" / "windows-llvm-md-release").mkdir(parents=True, exist_ok=True)
    (source / "build" / "windows-llvm-md-release" / "CMakeCache.txt").write_text(
        "cache", encoding="utf-8"
    )
    (source / "build" / "windows-llvm-md-release" / "build.ninja").write_text(
        "ninja", encoding="utf-8"
    )
    (source / "CMakeLists.txt").write_text("cmake", encoding="utf-8")
    (source / "include").mkdir()
    (source / "include" / "vrhi.h").write_text("// header", encoding="utf-8")
    return source


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


class SyncVrhiArtifactsTests(unittest.TestCase):
    def run_tool(self, *argv: str):
        """Run the tool in-process; return (exit code, stdout, stderr)."""
        stdout, stderr = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
            code = tool.main(list(argv))
        return code, stdout.getvalue(), stderr.getvalue()

    def artifact_dir(self, destination: Path) -> Path:
        return destination / "code" / "thirdparty" / "vrhi"

    # ------------------------------------------------------------- copying

    def test_copies_release_artifacts_and_writes_manifest(self):
        with tempfile.TemporaryDirectory() as tmp:
            source = make_artifact_root(Path(tmp))
            destination = Path(tmp) / "ioq3"
            destination.mkdir()

            code, out, err = self.run_tool(
                "--source-root", str(source),
                "--destination-root", str(destination),
                "--config", "release",
            )
            self.assertEqual(code, 0, err)
            artifacts = self.artifact_dir(destination)

            for name in tool.DEPENDENCY_LIBRARIES:
                copied = artifacts / "lib" / "win_llvm_md_release" / name
                self.assertTrue(copied.is_file(), copied)
                self.assertEqual(copied.read_bytes(), fake_lib_content("release", name))
            self.assertEqual(
                (artifacts / "build" / "windows-llvm-md-release" / "vrhi_md.lib")
                .read_bytes(),
                STATIC_RELEASE,
            )
            self.assertTrue((artifacts / ".vdeps-state.json").is_file())

            # Build-tree internals and source files must never be copied.
            for unwanted in (
                "CMakeCache.txt", "build.ninja", "CMakeLists.txt", "vrhi.h",
            ):
                self.assertFalse(
                    any(p.name == unwanted for p in artifacts.rglob("*")),
                    f"unwanted file copied: {unwanted}",
                )
            # The other config must not be copied.
            self.assertFalse((artifacts / "lib" / "win_llvm_md_debug").exists())

            manifest = json.loads((artifacts / tool.MANIFEST_NAME).read_text("utf-8"))
            self.assertEqual(manifest["schema"], 1)
            self.assertEqual(manifest["config"], "release")
            expected_paths = [
                ".vdeps-state.json",
                "build/windows-llvm-md-release/vrhi_md.lib",
            ] + [
                f"lib/win_llvm_md_release/{name}"
                for name in tool.DEPENDENCY_LIBRARIES
            ]
            self.assertEqual(
                [entry["path"] for entry in manifest["files"]], sorted(expected_paths)
            )
            for entry in manifest["files"]:
                path = artifacts / entry["path"]
                self.assertEqual(entry["size"], path.stat().st_size)
                self.assertEqual(entry["sha256"], sha256_bytes(path.read_bytes()))
            # Atomic manifest write leaves no temporary files behind.
            self.assertFalse(list(artifacts.glob("*.tmp")))
            # No temporary files anywhere in the destination tree.
            self.assertFalse(list((destination / "code").rglob("*.tmp")))

    def test_debug_config_copies_debug_libraries_only(self):
        with tempfile.TemporaryDirectory() as tmp:
            source = make_artifact_root(Path(tmp))
            destination = Path(tmp) / "ioq3"
            destination.mkdir()

            code, _out, err = self.run_tool(
                "--source-root", str(source),
                "--destination-root", str(destination),
                "--config", "debug",
            )
            self.assertEqual(code, 0, err)
            artifacts = self.artifact_dir(destination)
            for name in tool.DEPENDENCY_LIBRARIES:
                copied = artifacts / "lib" / "win_llvm_md_debug" / name
                self.assertTrue(copied.is_file(), copied)
                self.assertEqual(copied.read_bytes(), fake_lib_content("debug", name))
            self.assertFalse((artifacts / "lib" / "win_llvm_md_release").exists())
            manifest = json.loads((artifacts / tool.MANIFEST_NAME).read_text("utf-8"))
            self.assertEqual(manifest["config"], "debug")

    def test_uses_debug_static_library_when_only_debug_built(self):
        with tempfile.TemporaryDirectory() as tmp:
            source = make_artifact_root(Path(tmp), configs=("debug",))
            destination = Path(tmp) / "ioq3"
            destination.mkdir()

            code, _out, err = self.run_tool(
                "--source-root", str(source),
                "--destination-root", str(destination),
                "--config", "debug",
            )
            self.assertEqual(code, 0, err)
            copied = (
                self.artifact_dir(destination)
                / "build" / "windows-llvm-md-debug" / "vrhi_mdd.lib"
            )
            self.assertEqual(copied.read_bytes(), STATIC_DEBUG)

    # ------------------------------------------------------------- refusal

    def test_missing_dependency_library_is_refused(self):
        with tempfile.TemporaryDirectory() as tmp:
            source = make_artifact_root(Path(tmp))
            (source / "lib" / "win_llvm_md_release" / "nvrhi.lib").unlink()
            destination = Path(tmp) / "ioq3"
            destination.mkdir()

            code, _out, err = self.run_tool(
                "--source-root", str(source),
                "--destination-root", str(destination),
                "--config", "release",
            )
            self.assertEqual(code, 1)
            self.assertIn("missing source artifact", err)
            self.assertIn("nvrhi.lib", err)
            self.assertFalse((destination / "code").exists())

    def test_missing_static_library_is_refused(self):
        with tempfile.TemporaryDirectory() as tmp:
            source = make_artifact_root(Path(tmp))
            (source / "build" / "windows-llvm-md-release" / "vrhi_md.lib").unlink()
            destination = Path(tmp) / "ioq3"
            destination.mkdir()

            code, _out, err = self.run_tool(
                "--source-root", str(source),
                "--destination-root", str(destination),
            )
            self.assertEqual(code, 1)
            self.assertIn("missing source artifact", err)
            self.assertIn("vrhi_md.lib", err)
            self.assertFalse((destination / "code").exists())

    def test_ambiguous_static_library_is_refused(self):
        with tempfile.TemporaryDirectory() as tmp:
            source = make_artifact_root(Path(tmp))
            (source / "build" / "windows-llvm-md-release" / "vrhi_mdd.lib").write_bytes(
                STATIC_DEBUG
            )
            destination = Path(tmp) / "ioq3"
            destination.mkdir()

            code, _out, err = self.run_tool(
                "--source-root", str(source),
                "--destination-root", str(destination),
            )
            self.assertEqual(code, 1)
            self.assertIn("ambiguous VRHI static library", err)
            self.assertFalse((destination / "code").exists())

    def test_non_artifact_source_root_is_refused(self):
        with tempfile.TemporaryDirectory() as tmp:
            source = Path(tmp) / "not-artifacts"
            source.mkdir()
            (source / "junk.bin").write_bytes(b"junk")
            destination = Path(tmp) / "ioq3"
            destination.mkdir()

            code, _out, err = self.run_tool(
                "--source-root", str(source),
                "--destination-root", str(destination),
            )
            self.assertEqual(code, 1)
            self.assertIn("does not look like a VRHI artifact root", err)

    def test_missing_destination_root_is_refused(self):
        with tempfile.TemporaryDirectory() as tmp:
            source = make_artifact_root(Path(tmp))
            destination = Path(tmp) / "does-not-exist"
            code, _out, err = self.run_tool(
                "--source-root", str(source),
                "--destination-root", str(destination),
            )
            self.assertEqual(code, 1)
            self.assertIn("--destination-root is not a directory", err)

    def test_source_equals_destination_is_refused(self):
        with tempfile.TemporaryDirectory() as tmp:
            source = make_artifact_root(Path(tmp))
            code, _out, err = self.run_tool(
                "--source-root", str(source),
                "--destination-root", str(source),
            )
            self.assertEqual(code, 1)
            self.assertIn("same directory", err)

    def test_destination_file_that_is_a_directory_is_refused(self):
        with tempfile.TemporaryDirectory() as tmp:
            source = make_artifact_root(Path(tmp))
            destination = Path(tmp) / "ioq3"
            destination.mkdir()
            blocker = (
                self.artifact_dir(destination)
                / "lib" / "win_llvm_md_release" / "vk-bootstrap.lib"
            )
            blocker.parent.mkdir(parents=True)
            blocker.mkdir()

            code, _out, err = self.run_tool(
                "--source-root", str(source),
                "--destination-root", str(destination),
                "--force",
            )
            self.assertEqual(code, 1)
            self.assertIn("not a regular file", err)

    # --------------------------------------------------- dry run / force

    def test_dry_run_writes_nothing(self):
        with tempfile.TemporaryDirectory() as tmp:
            source = make_artifact_root(Path(tmp))
            destination = Path(tmp) / "ioq3"
            destination.mkdir()

            code, out, err = self.run_tool(
                "--source-root", str(source),
                "--destination-root", str(destination),
                "--dry-run",
            )
            self.assertEqual(code, 0, err)
            self.assertIn("would copy", out)
            self.assertIn("no files written", out)
            self.assertFalse((destination / "code").exists())

    def test_differing_destination_refused_without_force_then_overwritten(self):
        with tempfile.TemporaryDirectory() as tmp:
            source = make_artifact_root(Path(tmp))
            destination = Path(tmp) / "ioq3"
            destination.mkdir()
            stale = (
                self.artifact_dir(destination)
                / "lib" / "win_llvm_md_release" / "nvrhi.lib"
            )
            stale.parent.mkdir(parents=True)
            stale.write_bytes(b"stale-content")

            code, _out, err = self.run_tool(
                "--source-root", str(source),
                "--destination-root", str(destination),
            )
            self.assertEqual(code, 1)
            self.assertIn("pass --force", err)
            self.assertEqual(stale.read_bytes(), b"stale-content")

            code, out, err = self.run_tool(
                "--source-root", str(source),
                "--destination-root", str(destination),
                "--force",
            )
            self.assertEqual(code, 0, err)
            self.assertIn("overwriting", out)
            self.assertEqual(
                stale.read_bytes(), fake_lib_content("release", "nvrhi.lib")
            )

    def test_rerun_is_idempotent(self):
        with tempfile.TemporaryDirectory() as tmp:
            source = make_artifact_root(Path(tmp))
            destination = Path(tmp) / "ioq3"
            destination.mkdir()
            args = ("--source-root", str(source), "--destination-root", str(destination))

            first, _out1, err1 = self.run_tool(*args)
            self.assertEqual(first, 0, err1)
            copied = self.artifact_dir(destination) / "lib" / "win_llvm_md_release" / "rtxmu.lib"
            before = copied.read_bytes()

            second, out2, err2 = self.run_tool(*args)
            self.assertEqual(second, 0, err2)
            self.assertIn("up to date", out2)
            self.assertEqual(copied.read_bytes(), before)
            manifest = json.loads(
                (self.artifact_dir(destination) / tool.MANIFEST_NAME).read_text("utf-8")
            )
            self.assertEqual(len(manifest["files"]), 8)

    def test_unexpected_destination_files_warn_but_succeed(self):
        with tempfile.TemporaryDirectory() as tmp:
            source = make_artifact_root(Path(tmp))
            destination = Path(tmp) / "ioq3"
            destination.mkdir()
            stray = self.artifact_dir(destination) / "lib" / "win_llvm_md_release" / "stray.lib"
            stray.parent.mkdir(parents=True)
            stray.write_bytes(b"stray")

            code, out, err = self.run_tool(
                "--source-root", str(source),
                "--destination-root", str(destination),
            )
            self.assertEqual(code, 0, err)
            self.assertIn("WARNING", out)
            self.assertIn("stray.lib", out)


if __name__ == "__main__":
    unittest.main()
