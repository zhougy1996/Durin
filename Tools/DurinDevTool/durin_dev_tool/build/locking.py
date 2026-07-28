"""Cross-process checkout ownership and active-operation control."""

from __future__ import annotations

import json
import os
import signal
import subprocess
from pathlib import Path
from typing import Any, Mapping

from .config import LOCK_DIR, REPO_ROOT, BuildToolError


def state_file_component(value: str) -> str:
    allowed = set("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.+-")
    return "".join(character if character in allowed else "_" for character in value)


def lock_file_path(root: Path = LOCK_DIR) -> Path:
    # Presets share final outputs and generated metadata, so ownership belongs to the checkout.
    return root / "checkout.lock"


def lock_acl_recovery(path: Path) -> str:
    directory = path.parent
    return (
        "Confirm that no DurinDevTool, DurinEditor, CMake, or Ninja process for this checkout is still running. "
        f'Then, from your normal PowerShell, run: icacls "{directory}" /inheritance:e /T; '
        f'Remove-Item -LiteralPath "{path}" -Force'
    )


def inaccessible_lock_error(path: Path, exc: OSError) -> BuildToolError:
    return BuildToolError(
        f'Could not access DurinDevTool checkout lock "{path}": {exc}. '
        "The lock file could not be opened, so this is a file-permission problem rather than proof "
        "that another process still owns the checkout.",
        recovery=lock_acl_recovery(path),
    )


def recover_inaccessible_windows_lock(path: Path) -> bool:
    """Replace an inaccessible, unowned lock without splitting an active Windows lock."""
    if os.name != "nt":
        return False
    quarantine = path.with_name(f"{path.name}.{os.getpid()}.stale")
    try:
        os.replace(path, quarantine)
    except OSError:
        # Windows denies rename while DurinDevTool has the file open. Failure
        # preserves a possibly live lock and lets the caller report ACL recovery.
        return False
    try:
        quarantine.unlink(missing_ok=True)
    except OSError:
        # The renamed file is no longer the ownership path. Its incompatible ACL
        # may prevent cleanup, but must not keep the checkout unusable.
        pass
    return True


def open_checkout_lock(path: Path) -> Any:
    try:
        return path.open("a+b")
    except PermissionError as exc:
        if recover_inaccessible_windows_lock(path):
            try:
                return path.open("a+b")
            except OSError as retry_exc:
                raise inaccessible_lock_error(path, retry_exc) from retry_exc
        raise inaccessible_lock_error(path, exc) from exc
    except OSError as exc:
        raise BuildToolError(f'Could not open DurinDevTool checkout lock "{path}": {exc}') from exc


def normalize_windows_lock_acl(path: Path) -> bool:
    """Try to reset a lock file to the ACL inherited from the shared lock directory."""
    if os.name != "nt":
        return True
    result = subprocess.run(
        ["icacls", str(path), "/reset", "/q"],
        cwd=REPO_ROOT,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        check=False,
    )
    # An existing lock can be writable without granting this sandbox identity
    # WRITE_DAC. Lock ownership remains valid in that case; a later inaccessible
    # opener will use the stale-file recovery path and receive explicit guidance.
    return result.returncode == 0


def lock_is_owned(path: Path) -> bool:
    """Return whether another process currently holds the checkout lock."""
    try:
        handle = path.open("r+b")
    except FileNotFoundError:
        return False
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
        except OSError:
            return True
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


def stop_active_operation() -> bool:
    """Stop the DurinDevTool process recorded in the checkout ownership lock."""
    lock_path = lock_file_path()
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
            cwd=REPO_ROOT,
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
    def __init__(self, path: Path, metadata: Mapping[str, Any]):
        self.path = path
        self.metadata = dict(metadata)
        self.handle: Any = None

    def __enter__(self) -> "BuildToolLock":
        self.path.parent.mkdir(parents=True, exist_ok=True)
        self.handle = open_checkout_lock(self.path)
        if self.path.stat().st_size == 0:
            self.handle.write(b"\0")
            self.handle.flush()
        self.handle.seek(0)
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
            raise BuildToolError(
                "Another DurinDevTool operation already owns this checkout. "
                + read_state_description(self.path, locked=True)
            ) from exc
        normalize_windows_lock_acl(self.path)
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
