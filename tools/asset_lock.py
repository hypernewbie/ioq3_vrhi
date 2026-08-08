#!/usr/bin/env python3
"""Cross-process advisory lock for OpenArena asset provisioning.

``tools/get_openarena.py`` downloads and verifies ~390 MiB of pinned assets
under ``temp/assets/`` and atomically replaces a local manifest. Two image
suites (for example the hidden OpenGL 1 and OpenGL 2 suites) may provision the
same directory concurrently; without a lock they race on ``.part`` downloads,
destination replacement, and manifest writes. This module provides a small,
dependency-free, cross-platform lock that serializes provisioners and cleans up
after crashed owners.

Mechanism
---------
A lock is an exclusive directory: ``os.mkdir`` is atomic on Windows and POSIX,
so exactly one contender can create it. The holder records itself in
``owner.json`` (pid, host, started, token) and refreshes a ``heartbeat`` file
while working. A lock is considered stale only when it is old enough to be
judged AND its recorded owner is gone (or never wrote an owner record); stale
locks are reclaimed with an atomic rename, so exactly one waiting contender
wins the reclaim. On release the holder removes only the lock it created
(token check), so a lock that was reclaimed after a long stall is never
deleted by the previous holder.

All lock state lives inside the gitignored temp asset directory, and the lock
imposes a configurable timeout so a stuck holder fails loudly instead of
hanging the image suites forever.
"""

from __future__ import annotations

import ctypes
import json
import os
from pathlib import Path
import random
import socket
import time
from typing import Any

if os.name == "nt":
    import ctypes.wintypes

LOCK_DIR_NAME = ".provision-lock"
OWNER_FILE = "owner.json"
HEARTBEAT_FILE = "heartbeat"
DEFAULT_TIMEOUT = 600.0
STALE_AFTER = 30 * 60.0
STALE_GRACE = 10.0
POLL_INTERVAL = 0.25
HEARTBEAT_INTERVAL = 15.0


class LockTimeout(RuntimeError):
    """Raised when the provisioning lock cannot be acquired in time."""


def pid_alive(pid: int) -> bool:
    """Return True when a process with ``pid`` exists on this host."""
    if not isinstance(pid, int) or pid <= 0:
        return False
    if os.name == "nt":
        # OpenProcess alone is not enough on modern Windows: a terminated
        # process's pid can still be opened. Query the exit code instead:
        # STILL_ACTIVE (259) means the process is actually running.
        PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
        STILL_ACTIVE = 259
        kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        handle = kernel32.OpenProcess(
            PROCESS_QUERY_LIMITED_INFORMATION, False, pid
        )
        if not handle:
            return False
        try:
            exit_code = ctypes.wintypes.DWORD()
            if not kernel32.GetExitCodeProcess(handle, ctypes.byref(exit_code)):
                return True  # cannot query; assume alive (conservative)
            return exit_code.value == STILL_ACTIVE
        finally:
            kernel32.CloseHandle(handle)
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        return True
    return True


def _read_owner(lock_dir: Path) -> dict[str, Any] | None:
    """Return the owner record of a lock, or None when absent/unreadable."""
    try:
        payload = json.loads((lock_dir / OWNER_FILE).read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return None
    return payload if isinstance(payload, dict) else None


def describe_owner(lock_dir: Path) -> str:
    """Human-readable description of a lock's current holder."""
    owner = _read_owner(lock_dir)
    if not owner:
        return "unknown owner (no owner record)"
    return (
        f"pid {owner.get('pid', '?')} on {owner.get('host', '?')} "
        f"started {owner.get('started', '?')}"
    )


def is_stale(
    lock_dir: Path,
    stale_after: float = STALE_AFTER,
    grace: float = STALE_GRACE,
    now: float | None = None,
) -> bool:
    """Return True when an existing lock may be reclaimed safely.

    A lock younger than ``grace`` seconds is never reclaimed: its holder may
    still be between ``mkdir`` and writing the owner record. After that:

    - a lock whose recorded owner is dead is stale (covers crashes),
    - an old lock whose owner is alive but stopped heartbeating is stale
      (covers wedged holders and recycled pids),
    - anything else belongs to a live, working holder.
    """
    now = time.time() if now is None else now
    try:
        age = now - lock_dir.stat().st_mtime
    except OSError:
        return True
    if age <= grace:
        return False
    owner = _read_owner(lock_dir)
    owner_alive = bool(owner) and pid_alive(owner.get("pid", -1))
    if age <= stale_after:
        return not owner_alive
    try:
        heartbeat_age = now - (lock_dir / HEARTBEAT_FILE).stat().st_mtime
    except OSError:
        heartbeat_age = age
    return not (owner_alive and heartbeat_age <= stale_after)


def _reclaim_lock(lock_dir: Path) -> bool:
    """Atomically move a stale lock aside; True when this caller won.

    A rename (``os.replace``) is atomic on Windows and POSIX, so exactly one
    contender can reclaim a given stale lock; losers simply contend on the
    winner's fresh lock instead of deleting it.
    """
    if not lock_dir.exists():
        return True
    graveyard = lock_dir.with_name(
        f"{lock_dir.name}.reap-{os.getpid()}-{os.urandom(4).hex()}"
    )
    try:
        os.replace(lock_dir, graveyard)
    except OSError:
        return False
    try:
        for name in (OWNER_FILE, HEARTBEAT_FILE):
            try:
                (graveyard / name).unlink()
            except OSError:
                pass
        graveyard.rmdir()
    except OSError:
        pass
    return True


def _remove_lock_dir(lock_dir: Path, expected_token: str | None = None) -> None:
    """Best-effort removal of a lock directory.

    When ``expected_token`` is given, only a lock whose owner record carries
    that token is removed; a reclaimed lock (new token) is left untouched.
    """
    try:
        if expected_token is not None:
            owner = _read_owner(lock_dir)
            if not owner or owner.get("token") != expected_token:
                return
        for name in (OWNER_FILE, HEARTBEAT_FILE):
            try:
                (lock_dir / name).unlink()
            except OSError:
                pass
        lock_dir.rmdir()
    except OSError:
        pass


class ProvisionLock:
    """Exclusive, dependency-free cross-process lock over an asset directory.

    Example::

        with ProvisionLock(asset_dir / LOCK_DIR_NAME, timeout=600.0) as lock:
            download_and_verify(lock)  # call lock.refresh() during long work
    """

    def __init__(
        self,
        lock_dir: Path | str,
        timeout: float = DEFAULT_TIMEOUT,
        stale_after: float = STALE_AFTER,
        grace: float = STALE_GRACE,
        poll_interval: float = POLL_INTERVAL,
        heartbeat_interval: float = HEARTBEAT_INTERVAL,
    ) -> None:
        self.lock_dir = Path(lock_dir)
        self.timeout = float(timeout)
        self.stale_after = float(stale_after)
        self.grace = float(grace)
        self.poll_interval = float(poll_interval)
        self.heartbeat_interval = float(heartbeat_interval)
        self.token = os.urandom(16).hex()
        self._held = False
        self._last_heartbeat = 0.0
        self._last_notice = 0.0

    @property
    def held(self) -> bool:
        return self._held

    def acquire(self) -> None:
        """Take the lock, waiting up to ``self.timeout`` seconds.

        Raises :class:`LockTimeout` when another provisioner keeps the lock
        longer than the timeout.
        """
        if self._held:
            return
        deadline = time.monotonic() + self.timeout
        while True:
            try:
                self.lock_dir.mkdir(parents=True)
                break
            except FileExistsError:
                pass
            if is_stale(self.lock_dir, self.stale_after, self.grace):
                _reclaim_lock(self.lock_dir)
                continue
            if time.monotonic() - self._last_notice >= 5.0:
                print(
                    f"  waiting for another asset provisioning process "
                    f"({describe_owner(self.lock_dir)})...",
                    flush=True,
                )
                self._last_notice = time.monotonic()
            if time.monotonic() >= deadline:
                raise LockTimeout(
                    f"could not acquire asset provisioning lock {self.lock_dir} "
                    f"within {self.timeout:g}s; current holder: "
                    f"{describe_owner(self.lock_dir)}. If that process crashed, "
                    f"the lock is reclaimed automatically once it is older than "
                    f"{self.grace:g}s with a dead owner; otherwise increase "
                    f"--lock-timeout."
                )
            time.sleep(self.poll_interval * (1.0 + random.random()))
        try:
            self._write_owner()
        except BaseException:
            _reclaim_lock(self.lock_dir)
            raise
        self._held = True
        self._last_heartbeat = time.monotonic()
        self._last_notice = 0.0

    def _write_owner(self) -> None:
        owner = {
            "pid": os.getpid(),
            "host": socket.gethostname(),
            "token": self.token,
            "started": time.time(),
        }
        (self.lock_dir / OWNER_FILE).write_text(
            json.dumps(owner), encoding="utf-8"
        )

    def refresh(self) -> None:
        """Refresh the heartbeat so slow downloads are never reclaimed."""
        if not self._held:
            return
        now = time.monotonic()
        if now - self._last_heartbeat < self.heartbeat_interval:
            if (self.lock_dir / HEARTBEAT_FILE).exists():
                return
        self._last_heartbeat = now
        try:
            (self.lock_dir / HEARTBEAT_FILE).write_text(
                str(time.time()), encoding="utf-8"
            )
        except OSError:
            pass  # best effort: a missing heartbeat only delays reclamation

    def release(self) -> None:
        if not self._held:
            return
        self._held = False
        _remove_lock_dir(self.lock_dir, expected_token=self.token)

    def __enter__(self) -> "ProvisionLock":
        self.acquire()
        return self

    def __exit__(self, *exc_info: object) -> None:
        self.release()
