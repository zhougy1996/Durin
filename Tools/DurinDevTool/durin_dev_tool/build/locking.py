"""Cross-process checkout ownership and active-operation control."""

from __future__ import annotations

import errno
import json
import os
import signal
import subprocess
from pathlib import Path
from typing import Any, Mapping

from .errors import BuildToolError
from .settings import default_build_paths


def state_file_component(value: str) -> str:
    allowed = set("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.+-")
    return "".join(character if character in allowed else "_" for character in value)


def lock_file_path(root: Path | None = None) -> Path:
    root = root or default_build_paths().lock_directory
    # Presets share final outputs and generated metadata, so ownership belongs to the checkout.
    return root / "checkout.lock"


def inaccessible_lock_error(path: Path, exc: OSError, *, scope: str = "checkout") -> BuildToolError:
    return BuildToolError(
        f'Access denied to DurinDevTool {scope} lock "{path}": {exc}. '
        "This does not establish lock ownership. The OS error alone cannot distinguish "
        "file permissions from execution-environment restrictions.",
        recovery=(
            "Execution environment restrictions: check whether the sandbox permits access to "
            f'the lock and its directory "{path.parent}". Shared dependency locks may be outside '
            "the current worktree; request access to that shared location and rerun the same command. "
            "File permissions: if access is also denied in an authorized normal shell, inspect "
            "the file and directory permissions and ownership with the administrator. "
            "Do not reset ACLs or delete the lock as a generic recovery step."
        ),
    )


def lock_contention(exc: OSError) -> bool:
    """Recognize OS nonblocking-lock conflicts, not arbitrary I/O failures."""
    winerror = getattr(exc, "winerror", None)
    if winerror is not None:
        return winerror in (32, 33)  # Sharing or byte-range lock violation.
    # The Windows CRT reports locking conflicts through errno without winerror.
    return exc.errno in (errno.EACCES, errno.EAGAIN, errno.EDEADLK)


def open_checkout_lock(path: Path, *, scope: str = "checkout") -> Any:
    try:
        return path.open("a+b")
    except PermissionError as exc:
        raise inaccessible_lock_error(path, exc, scope=scope) from exc
    except OSError as exc:
        raise BuildToolError(f'Could not open DurinDevTool {scope} lock "{path}": {exc}') from exc


def lock_is_owned(path: Path) -> bool:
    """Return whether another process currently holds the checkout lock."""
    try:
        handle = path.open("r+b")
    except FileNotFoundError:
        return False
    except PermissionError as exc:
        raise inaccessible_lock_error(path, exc) from exc
    except OSError as exc:
        raise BuildToolError(f'Could not open DurinDevTool lock "{path}": {exc}') from exc
    try:
        if path.stat().st_size == 0:
            return False
        handle.seek(0)
        try:
            if os.name == "nt":
                import msvcrt

                msvcrt.locking(handle.fileno(), msvcrt.LK_NBLCK, 1)
            else:
                import fcntl

                fcntl.flock(handle.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError as exc:
            if lock_contention(exc):
                return True
            if isinstance(exc, PermissionError):
                raise inaccessible_lock_error(path, exc) from exc
            raise BuildToolError(f'Could not probe DurinDevTool lock "{path}": {exc}') from exc
        if os.name == "nt":
            msvcrt.locking(handle.fileno(), msvcrt.LK_UNLCK, 1)
        else:
            fcntl.flock(handle.fileno(), fcntl.LOCK_UN)
        return False
    finally:
        handle.close()


def read_lock_metadata(path: Path) -> dict[str, Any]:
    """Read metadata without touching the locked ownership byte."""
    try:
        with path.open("rb") as handle:
            handle.seek(1)
            content = handle.read()
    except OSError as exc:
        raise BuildToolError(f'Could not read DurinDevTool lock "{path}": {exc}') from exc
    # Older lock files stored the opening JSON brace in the locked byte.
    for candidate in (content, b"{" + content):
        try:
            metadata = json.loads(candidate.decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError):
            continue
        if isinstance(metadata, dict):
            return metadata
    raise BuildToolError(f'DurinDevTool lock does not contain valid metadata: "{path}"')


def read_state_description(path: Path, *, locked: bool = False) -> str:
    try:
        value = read_lock_metadata(path) if locked else json.loads(path.read_text(encoding="utf-8"))
    except (BuildToolError, OSError, json.JSONDecodeError):
        return f'Existing state file: "{path}"'
    if not isinstance(value, dict):
        return f'Existing state file: "{path}"'
    fields = []
    for key, label in (
        ("pid", "PID"),
        ("profile", "profile"),
        ("preset", "preset"),
        ("action", "action"),
        ("target", "target"),
        ("startedAt", "started"),
    ):
        if value.get(key) not in (None, ""):
            fields.append(f"{label}={value[key]}")
    return ", ".join(fields) if fields else f'Existing state file: "{path}"'


def stop_active_operation(
    *,
    lock_directory: Path | None = None,
    cwd: Path | None = None,
) -> bool:
    """Stop the DurinDevTool process recorded in the checkout ownership lock."""
    lock_path = lock_file_path(lock_directory)
    if not lock_is_owned(lock_path):
        return False
    metadata = read_lock_metadata(lock_path)
    try:
        pid = int(metadata["pid"])
    except (ValueError, TypeError, KeyError) as exc:
        raise BuildToolError(f'DurinDevTool lock does not contain a valid process ID: "{lock_path}"') from exc
    if pid <= 0 or pid == os.getpid():
        raise BuildToolError(f'DurinDevTool lock contains an invalid process ID: {pid}')

    if os.name == "nt":
        result = subprocess.run(
            ["taskkill", "/PID", str(pid), "/T", "/F"],
            cwd=cwd or default_build_paths().root,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            check=False,
        )
        if result.returncode != 0:
            raise BuildToolError(
                f"Could not stop the active DurinDevTool process (PID {pid}). It may have already exited."
            )
    else:
        try:
            os.killpg(pid, signal.SIGTERM)
        except ProcessLookupError:
            raise BuildToolError(f"The active DurinDevTool process (PID {pid}) has already exited.")
    return True


class BuildToolLock:
    def __init__(
        self,
        path: Path,
        metadata: Mapping[str, Any],
        *,
        cwd: Path | None = None,
        scope: str = "checkout",
    ):
        self.path = path
        self.metadata = dict(metadata)
        self.cwd = cwd
        self.scope = scope
        self.handle: Any = None

    def __enter__(self) -> "BuildToolLock":
        try:
            self.path.parent.mkdir(parents=True, exist_ok=True)
            self.handle = open_checkout_lock(self.path, scope=self.scope)
            if self.path.stat().st_size == 0:
                self.handle.write(b"\0")
                self.handle.flush()
            self.handle.seek(0)
        except OSError as exc:
            if self.handle is not None:
                self.handle.close()
                self.handle = None
            if isinstance(exc, PermissionError):
                raise inaccessible_lock_error(self.path, exc, scope=self.scope) from exc
            raise BuildToolError(f'Could not initialize DurinDevTool {self.scope} lock "{self.path}": {exc}') from exc
        try:
            if os.name == "nt":
                import msvcrt

                msvcrt.locking(self.handle.fileno(), msvcrt.LK_NBLCK, 1)
            else:
                import fcntl

                fcntl.flock(self.handle.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError as exc:
            self.handle.close()
            self.handle = None
            if not lock_contention(exc):
                if isinstance(exc, PermissionError):
                    raise inaccessible_lock_error(self.path, exc, scope=self.scope) from exc
                raise BuildToolError(
                    f'Could not acquire DurinDevTool {self.scope} lock "{self.path}": {exc}'
                ) from exc
            raise BuildToolError(
                f"Another DurinDevTool operation already owns this {self.scope}. "
                + read_state_description(self.path, locked=True),
                recovery="Wait for the owning operation to finish before retrying; do not delete the lock file.",
            ) from exc
        self.handle.seek(0)
        # Byte zero is reserved for ownership so other processes can read the JSON while it is locked.
        self.handle.truncate()
        self.handle.write(b"\0")
        self.handle.write((json.dumps(self.metadata, indent=2) + "\n").encode("utf-8"))
        self.handle.flush()
        return self

    def __exit__(self, exc_type: Any, exc: Any, traceback: Any) -> None:
        if self.handle is None:
            return
        try:
            self.handle.seek(0)
            if os.name == "nt":
                import msvcrt

                msvcrt.locking(self.handle.fileno(), msvcrt.LK_UNLCK, 1)
            else:
                import fcntl

                fcntl.flock(self.handle.fileno(), fcntl.LOCK_UN)
        finally:
            self.handle.close()
            self.handle = None
