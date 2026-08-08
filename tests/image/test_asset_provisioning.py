"""Dependency-free tests for asset provisioning (``tools/get_openarena.py``).

Covers the pieces that the concurrent-suite race depends on: atomic manifest
writes, Git-blob hash verification, and the cross-process lock integration that
serializes ``ensure_assets``/downloads/manifest replacement. No network access
is used: ``ensure_assets`` is exercised only through its lock timeout path, and
the manifest/hash helpers are tested directly.
"""

from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

TOOLS_DIR = Path(__file__).resolve().parents[2] / "tools"
sys.path.insert(0, str(TOOLS_DIR))

from asset_lock import LOCK_DIR_NAME, ProvisionLock  # noqa: E402
from get_openarena import (  # noqa: E402
    ASSET_REPOSITORY,
    git_blob_sha1,
    valid_asset,
    write_asset_manifest,
)

REPO_ROOT = Path(__file__).resolve().parents[2]
GET_OPENARENA = REPO_ROOT / "tools" / "get_openarena.py"


class ManifestTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.asset_dir = Path(self.tmp.name) / "assets"
        self.baseoa = self.asset_dir / "baseoa"
        self.baseoa.mkdir(parents=True)

    def tearDown(self) -> None:
        self.tmp.cleanup()

    def _fake_asset(self, name: str, payload: bytes) -> None:
        (self.baseoa / name).write_bytes(payload)

    def test_manifest_write_is_atomic_and_records_sha256(self) -> None:
        payload = b"fake pk3 content"
        self._fake_asset("pak0.pk3", payload)
        manifest_path = write_asset_manifest(
            self.asset_dir, [("pak0.pk3", len(payload), "deadbeef" * 5)]
        )
        # The atomic temp+rename pattern leaves no temporary file behind.
        self.assertTrue(manifest_path.is_file())
        self.assertFalse(manifest_path.with_suffix(".json.tmp").exists())

        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        self.assertEqual(manifest["schema"], 1)
        self.assertEqual(manifest["repository"], ASSET_REPOSITORY)
        self.assertEqual(manifest["asset_directory"], str(self.baseoa))
        entry = manifest["files"][0]
        self.assertEqual(entry["name"], "pak0.pk3")
        self.assertEqual(entry["size"], len(payload))
        self.assertEqual(entry["git_blob_sha1"], "deadbeef" * 5)
        self.assertEqual(
            entry["sha256"], hashlib.sha256(payload).hexdigest()
        )

    def test_manifest_write_replaces_previous_manifest(self) -> None:
        self._fake_asset("pak0.pk3", b"first")
        first = write_asset_manifest(
            self.asset_dir, [("pak0.pk3", 5, "a" * 40)]
        )
        self._fake_asset("pak0.pk3", b"second")
        write_asset_manifest(self.asset_dir, [("pak0.pk3", 6, "b" * 40)])
        manifest = json.loads(first.read_text(encoding="utf-8"))
        self.assertEqual(manifest["files"][0]["size"], 6)

    def test_valid_asset_verifies_git_blob_hash_and_size(self) -> None:
        payload = b"blob 11\x00hello world"  # 21 bytes, hash over prefix+bytes
        path = self.asset_dir / "sample.bin"
        path.write_bytes(payload)
        digest = git_blob_sha1(path, len(payload))
        self.assertEqual(len(digest), 40)

        self.assertTrue(valid_asset(path, len(payload), digest))
        self.assertFalse(
            valid_asset(path, len(payload), "0" * 40), "wrong hash"
        )
        self.assertFalse(
            valid_asset(path, len(payload) + 1, digest), "wrong size"
        )
        path.write_bytes(b"tampered!")
        self.assertFalse(
            valid_asset(path, len(payload), digest), "tampered content"
        )


class ProvisionLockIntegrationTests(unittest.TestCase):
    """The real ``get_openarena.py download`` CLI, without network."""

    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.asset_dir = Path(self.tmp.name) / "assets"

    def tearDown(self) -> None:
        self.tmp.cleanup()

    def test_download_fails_cleanly_when_lock_is_held(self) -> None:
        # A held lock must make a second provisioner time out before it
        # touches any file, proving ensure_assets runs under the lock.
        self.asset_dir.mkdir(parents=True)
        lock_dir = self.asset_dir / LOCK_DIR_NAME
        with ProvisionLock(lock_dir):
            result = subprocess.run(
                [
                    sys.executable,
                    str(GET_OPENARENA),
                    "download",
                    "--asset-dir",
                    str(self.asset_dir),
                    "--lock-timeout",
                    "0.5",
                ],
                capture_output=True,
                text=True,
                timeout=60,
            )
        self.assertEqual(result.returncode, 1)
        self.assertIn("could not acquire asset provisioning lock", result.stderr)
        self.assertIn(str(lock_dir.resolve()), result.stderr)
        # Nothing was downloaded or verified while the lock was held.
        self.assertFalse((self.asset_dir / "baseoa").exists())
        self.assertFalse((self.asset_dir / "manifest.json").exists())

    def test_lock_is_released_after_failed_provisioning(self) -> None:
        # A failing provisioner must release the lock on exit so the next
        # provisioner is not blocked. After the CLI times out on the lock,
        # the lock must be acquirable again immediately.
        self.asset_dir.mkdir(parents=True)
        lock_dir = self.asset_dir / LOCK_DIR_NAME
        with ProvisionLock(lock_dir):
            subprocess.run(
                [
                    sys.executable,
                    str(GET_OPENARENA),
                    "download",
                    "--asset-dir",
                    str(self.asset_dir),
                    "--lock-timeout",
                    "0.5",
                ],
                capture_output=True,
                text=True,
                timeout=60,
            )
        with ProvisionLock(lock_dir, timeout=5.0):
            pass
        self.assertFalse(lock_dir.exists())

    def test_lock_timeout_argument_is_accepted(self) -> None:
        result = subprocess.run(
            [sys.executable, str(GET_OPENARENA), "download", "--help"],
            capture_output=True,
            text=True,
            timeout=60,
        )
        self.assertEqual(result.returncode, 0)
        self.assertIn("--lock-timeout", result.stdout)


if __name__ == "__main__":
    unittest.main()
