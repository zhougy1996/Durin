from __future__ import annotations

from contextlib import contextmanager
from pathlib import Path
import json
import logging
import os
import sys
import time
from typing import Iterator


_POLL_INTERVAL_SECONDS = 0.1


def _try_lock(lock_file) -> bool:
    lock_file.seek(0)
    if sys.platform == "win32":
        import msvcrt

        try:
            msvcrt.locking(lock_file.fileno(), msvcrt.LK_NBLCK, 1)
            return True
        except OSError:
            return False

    import fcntl

    try:
        fcntl.flock(lock_file.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
        return True
    except BlockingIOError:
        return False


def _unlock(lock_file) -> None:
    lock_file.seek(0)
    if sys.platform == "win32":
        import msvcrt

        msvcrt.locking(lock_file.fileno(), msvcrt.LK_UNLCK, 1)
        return

    import fcntl

    fcntl.flock(lock_file.fileno(), fcntl.LOCK_UN)


def _read_owner(lock_file) -> str:
    try:
        lock_file.seek(1)
        raw_owner = lock_file.read().decode("utf-8", errors="replace").strip()
        if not raw_owner:
            return "unknown owner"
        owner = json.loads(raw_owner)
        return f"pid {owner.get('pid', '?')} running {owner.get('operation', 'unknown operation')}"
    except (OSError, ValueError, TypeError):
        return "unknown owner"


def _write_owner(lock_file, operation: str) -> None:
    owner = json.dumps({"pid": os.getpid(), "operation": operation})
    lock_file.seek(1)
    lock_file.truncate()
    lock_file.write(owner.encode("utf-8"))
    lock_file.flush()
    os.fsync(lock_file.fileno())


def _clear_owner(lock_file) -> None:
    lock_file.seek(1)
    lock_file.truncate()
    lock_file.flush()


@contextmanager
def acquire_output_lock(lock_path: Path, operation: str) -> Iterator[None]:
    lock_path.parent.mkdir(parents=True, exist_ok=True)
    with open(lock_path, "a+b", buffering=0) as lock_file:
        if lock_path.stat().st_size == 0:
            lock_file.write(b"\0")
            lock_file.flush()

        waiting_logged = False
        while not _try_lock(lock_file):
            if not waiting_logged:
                logging.info(
                    "[DHT] Waiting for output lock %s held by %s",
                    lock_path,
                    _read_owner(lock_file),
                )
                waiting_logged = True
            time.sleep(_POLL_INTERVAL_SECONDS)

        _write_owner(lock_file, operation)
        logging.debug("[DHT] Acquired output lock %s", lock_path)
        try:
            yield
        finally:
            _clear_owner(lock_file)
            _unlock(lock_file)

