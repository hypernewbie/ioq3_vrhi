"""Dependency-free tests for the cross-process asset provisioning lock.

The lock (``tools/asset_lock.py``) serializes ``ensure_assets`` across image
suites (for example the hidden OpenGL 1 and OpenGL 2 suites). These tests
cover mutual exclusion (in-process and across real processes), timeout
behavior, heartbeat refresh, stale-lock reclamation, and safe release after a
lock was reclaimed.
"""

from __future__ import annotations

import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import unittest

TOOLS_DIR = Path(__file__).resolve().parents[2] / "tools"
sys.path.insert(0, str(TOOLS_DIR))

from asset_lock import (  # noqa: E402
    HEARTBEAT_FILE,
    LOCK_DIR_NAME,
    OWNER_FILE,
    STALE_AFTER,
    STALE_GRACE,
    LockTimeout,
    ProvisionLock,
    _reclaim_lock,
    describe_owner,
    is_stale,
    pid_alive,
)

DEAD_PID = 2**20  # extremely unlikely to exist on any host


class ProvisionLockTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.asset_dir = Path(self.tmp.name) / "assets"
        self.lock_dir = self.asset_dir / LOCK_DIR_NAME

    def tearDown(self) -> None:
        self.tmp.cleanup()

    # -- basic lifecycle ---------------------------------------------------

    def test_acquire_release_roundtrip(self) -> None:
        with ProvisionLock(self.lock_dir) as lock:
            self.assertTrue(self.lock_dir.is_dir())
            self.assertTrue(lock.held)
            owner = json.loads((self.lock_dir / OWNER_FILE).read_text())
            self.assertEqual(owner["pid"], os.getpid())
            self.assertEqual(owner["token"], lock.token)
            self.assertIn("host", owner)
            self.assertIn("started", owner)
        self.assertFalse(self.lock_dir.exists())

    def test_context_manager_releases_on_error(self) -> None:
        with self.assertRaises(RuntimeError):
            with ProvisionLock(self.lock_dir):
                raise RuntimeError("boom")
        self.assertFalse(self.lock_dir.exists())

    def test_creates_parent_directories(self) -> None:
        lock_dir = self.asset_dir / "nested" / "lock"
        with ProvisionLock(lock_dir):
            self.assertTrue(lock_dir.is_dir())
        self.assertFalse(lock_dir.exists())

    def test_reacquire_after_release(self) -> None:
        with ProvisionLock(self.lock_dir):
            pass
        with ProvisionLock(self.lock_dir):
            pass
        self.assertFalse(self.lock_dir.exists())

    def test_acquire_is_idempotent(self) -> None:
        lock = ProvisionLock(self.lock_dir)
        lock.acquire()
        lock.acquire()  # must not raise or create a second lock
        lock.release()
        self.assertFalse(self.lock_dir.exists())

    # -- contention ---------------------------------------------------------

    def test_in_process_contention_times_out(self) -> None:
        first = ProvisionLock(self.lock_dir, timeout=5.0)
        first.acquire()
        try:
            second = ProvisionLock(
                self.lock_dir, timeout=0.5, poll_interval=0.02
            )
            with self.assertRaises(LockTimeout) as caught:
                second.acquire()
            message = str(caught.exception)
            self.assertIn(str(self.lock_dir), message)
            self.assertIn(str(os.getpid()), message)  # names the holder
        finally:
            first.release()
        self.assertFalse(self.lock_dir.exists())

    def _waiter_script(self) -> str:
        return (
            "import sys\n"
            f"sys.path.insert(0, {str(TOOLS_DIR)!r})\n"
            "from asset_lock import ProvisionLock\n"
            f"lock_dir = {str(self.lock_dir)!r}\n"
            "try:\n"
            "    with ProvisionLock(lock_dir, timeout=float(sys.argv[1]), "
            "poll_interval=0.05):\n"
            "        print('ACQUIRED')\n"
            "        sys.exit(0)\n"
            "except Exception as error:\n"
            "    print('FAILED', type(error).__name__, str(error))\n"
            "    sys.exit(3)\n"
        )

    def _run_waiter(self, timeout: float) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [sys.executable, "-c", self._waiter_script(), str(timeout)],
            capture_output=True,
            text=True,
            timeout=120,
        )

    def test_cross_process_contention_is_excluded(self) -> None:
        first = ProvisionLock(self.lock_dir, timeout=5.0)
        first.acquire()
        try:
            result = self._run_waiter(timeout=2.0)
            self.assertEqual(result.returncode, 3, result.stdout + result.stderr)
            self.assertIn("FAILED LockTimeout", result.stdout)
        finally:
            first.release()

    def test_waiter_proceeds_after_release(self) -> None:
        first = ProvisionLock(self.lock_dir, timeout=5.0)
        first.acquire()
        waiter = subprocess.Popen(
            [sys.executable, "-c", self._waiter_script(), "30.0"],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        try:
            time.sleep(0.6)  # let the waiter start and contend
            self.assertIsNone(waiter.poll())
            first.release()
            stdout, stderr = waiter.communicate(timeout=60)
            self.assertEqual(waiter.returncode, 0, stderr)
            self.assertIn("ACQUIRED", stdout)
        finally:
            if waiter.poll() is None:
                waiter.kill()
                waiter.wait()
            first.release()
        self.assertFalse(self.lock_dir.exists())

    # -- staleness ----------------------------------------------------------

    def _plant_lock(
        self,
        *,
        pid: int,
        age: float,
        heartbeat: float | None = None,
        token: str = "stale-token",
    ) -> None:
        """Create a lock directory that looks like a crashed holder's."""
        self.lock_dir.mkdir(parents=True)
        owner = {
            "pid": pid,
            "host": "test-host",
            "token": token,
            "started": time.time() - age,
        }
        (self.lock_dir / OWNER_FILE).write_text(
            json.dumps(owner), encoding="utf-8"
        )
        if heartbeat is not None:
            (self.lock_dir / HEARTBEAT_FILE).write_text("hb", encoding="utf-8")
            os.utime(
                self.lock_dir / HEARTBEAT_FILE, (time.time() - heartbeat,) * 2
            )
        os.utime(self.lock_dir, (time.time() - age,) * 2)

    def test_fresh_lock_with_live_owner_not_stale(self) -> None:
        with ProvisionLock(self.lock_dir):
            self.assertFalse(is_stale(self.lock_dir))

    def test_young_lock_with_dead_owner_not_stale_during_grace(self) -> None:
        self._plant_lock(pid=DEAD_PID, age=1.0)
        self.assertFalse(is_stale(self.lock_dir, grace=10.0))

    def test_recent_lock_with_dead_owner_is_stale_after_grace(self) -> None:
        self._plant_lock(pid=DEAD_PID, age=STALE_GRACE + 1.0)
        self.assertTrue(is_stale(self.lock_dir))

    def test_old_lock_live_owner_fresh_heartbeat_not_stale(self) -> None:
        self._plant_lock(
            pid=os.getpid(), age=STALE_AFTER + 60.0, heartbeat=5.0
        )
        self.assertFalse(is_stale(self.lock_dir))

    def test_old_lock_dead_owner_is_stale_even_with_fresh_heartbeat(
        self,
    ) -> None:
        self._plant_lock(pid=DEAD_PID, age=STALE_AFTER + 60.0, heartbeat=5.0)
        self.assertTrue(is_stale(self.lock_dir))

    def test_old_lock_live_owner_stale_heartbeat_is_stale(self) -> None:
        self._plant_lock(
            pid=os.getpid(),
            age=STALE_AFTER + 60.0,
            heartbeat=STALE_AFTER + 1.0,
        )
        self.assertTrue(is_stale(self.lock_dir))

    def test_old_lock_without_owner_record_is_stale(self) -> None:
        self.lock_dir.mkdir(parents=True)
        os.utime(self.lock_dir, (time.time() - STALE_AFTER - 60.0,) * 2)
        self.assertTrue(is_stale(self.lock_dir))

    def test_stale_lock_is_reclaimed_on_acquire(self) -> None:
        self._plant_lock(pid=DEAD_PID, age=STALE_GRACE + 1.0)
        with ProvisionLock(self.lock_dir, timeout=5.0) as lock:
            owner = json.loads((self.lock_dir / OWNER_FILE).read_text())
            self.assertEqual(owner["pid"], os.getpid())
            self.assertEqual(owner["token"], lock.token)
        self.assertFalse(self.lock_dir.exists())

    def test_reclaim_removes_stale_lock_completely(self) -> None:
        self._plant_lock(pid=DEAD_PID, age=STALE_GRACE + 1.0)
        self.assertTrue(_reclaim_lock(self.lock_dir))
        self.assertFalse(self.lock_dir.exists())
        self.assertEqual(list(self.asset_dir.glob(f"{LOCK_DIR_NAME}.reap-*")), [])

    def test_release_never_removes_a_reclaimed_lock(self) -> None:
        first = ProvisionLock(self.lock_dir, timeout=5.0)
        first.acquire()
        try:
            # Simulate a waiter reclaiming the lock after a long stall.
            (self.lock_dir / OWNER_FILE).unlink()
            self.lock_dir.rmdir()
            self.lock_dir.mkdir()
            new_owner = {
                "pid": os.getpid(),
                "host": "waiter",
                "token": "new-token",
                "started": time.time(),
            }
            (self.lock_dir / OWNER_FILE).write_text(
                json.dumps(new_owner), encoding="utf-8"
            )
            first.release()
            self.assertTrue(self.lock_dir.is_dir())
            owner = json.loads((self.lock_dir / OWNER_FILE).read_text())
            self.assertEqual(owner["token"], "new-token")
        finally:
            first.release()

    # -- heartbeat ----------------------------------------------------------

    def test_refresh_writes_heartbeat(self) -> None:
        with ProvisionLock(self.lock_dir, heartbeat_interval=0.0) as lock:
            self.assertFalse((self.lock_dir / HEARTBEAT_FILE).exists())
            lock.refresh()
            self.assertTrue((self.lock_dir / HEARTBEAT_FILE).exists())

    def test_refresh_is_throttled_by_interval(self) -> None:
        with ProvisionLock(self.lock_dir, heartbeat_interval=3600.0) as lock:
            lock.refresh()  # first refresh always writes
            first = (self.lock_dir / HEARTBEAT_FILE).read_text()
            time.sleep(0.02)
            lock.refresh()  # inside the interval: no rewrite
            self.assertEqual(
                (self.lock_dir / HEARTBEAT_FILE).read_text(), first
            )

    def test_refresh_requires_held_lock(self) -> None:
        lock = ProvisionLock(self.lock_dir)
        lock.refresh()  # must not create anything without holding
        self.assertFalse(self.lock_dir.exists())

    # -- helpers ------------------------------------------------------------

    def test_pid_alive(self) -> None:
        self.assertTrue(pid_alive(os.getpid()))
        self.assertFalse(pid_alive(DEAD_PID))
        self.assertFalse(pid_alive(0))
        self.assertFalse(pid_alive(-1))

    def test_killed_process_pid_is_reported_dead(self) -> None:
        # Regression: on modern Windows OpenProcess can still succeed for a
        # terminated process's pid; the exit-code query must catch that so
        # crashed provisioners' locks are actually reclaimed.
        victim = subprocess.Popen(
            [sys.executable, "-c", "import time; time.sleep(30)"]
        )
        try:
            self.assertTrue(pid_alive(victim.pid))
        finally:
            victim.kill()
            victim.wait(timeout=30)
        self.assertFalse(pid_alive(victim.pid))

    def test_describe_owner(self) -> None:
        with ProvisionLock(self.lock_dir):
            self.assertIn(str(os.getpid()), describe_owner(self.lock_dir))
        self.assertIn("unknown", describe_owner(self.lock_dir))


if __name__ == "__main__":
    unittest.main()
