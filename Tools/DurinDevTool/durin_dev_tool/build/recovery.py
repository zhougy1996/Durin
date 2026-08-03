"""Interrupted-operation state and recovery-marker transactions."""

from __future__ import annotations

import json
import os
from pathlib import Path
from typing import Any, Callable, Mapping

from .config import STATE_DIR, Action, BuildToolError, BuildToolInterruptedError
from .locking import read_state_description, state_file_component


def interruption_marker_path(preset: str, root: Path = STATE_DIR) -> Path:
    return root / f"{state_file_component(preset)}.interrupted.json"


def recoverable_target(path: Path) -> str | None:
    """Return the target that can safely resume from an interrupted operation."""
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return None
    if not isinstance(value, dict):
        return None
    action = value.get("action")
    target = value.get("target")
    if action in {Action.CONFIGURE.value, Action.CLEAN.value}:
        return "all"
    if action not in {
        Action.BUILD.value,
        Action.REBUILD.value,
        Action.RECOVER.value,
        Action.TEST.value,
    }:
        return None
    if not isinstance(target, str) or not target:
        return None
    allowed = set("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.+-")
    if any(character not in allowed for character in target):
        return None
    return target


def recovery_target(path: Path) -> str:
    """Return the narrowest rebuild target, falling back safely for old state."""
    return recoverable_target(path) or "all"


def write_json_state(path: Path, value: Mapping[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(f"{path.name}.{os.getpid()}.tmp")
    temporary.write_text(json.dumps(dict(value), indent=2) + "\n", encoding="utf-8")
    os.replace(temporary, path)


def restore_state_file(path: Path, previous_content: bytes | None) -> None:
    if previous_content is None:
        path.unlink(missing_ok=True)
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(f"{path.name}.{os.getpid()}.tmp")
    temporary.write_bytes(previous_content)
    os.replace(temporary, path)


def execute_with_recovery_marker(
    *,
    action: Action,
    marker_file: Path,
    metadata: Mapping[str, Any],
    operation: Callable[[str | None], None],
) -> None:
    try:
        previous_content = marker_file.read_bytes()
    except FileNotFoundError:
        previous_content = None
    except OSError as exc:
        raise BuildToolError(f'Could not read DurinDevTool recovery state "{marker_file}": {exc}') from exc
    target_override: str | None = None
    if previous_content is None and action is Action.RECOVER:
        raise BuildToolError("No interrupted DurinDevTool operation was found for this preset.")
    if previous_content is not None and action in {Action.BUILD, Action.TEST}:
        target = recoverable_target(marker_file)
        raise BuildToolError(
            "The previous DurinDevTool operation did not return normally. "
            + read_state_description(marker_file),
            recovery=(
                "Confirm its old process tree has exited, then run "
                + (
                    "recover with the affected preset."
                    if target
                    else "rebuild --target all with the affected preset."
                )
            ),
        )
    if previous_content is not None and action is Action.REBUILD:
        required_target = recovery_target(marker_file)
        requested_target = metadata.get("target")
        previous_action = None
        try:
            previous_value = json.loads(previous_content)
            if isinstance(previous_value, dict):
                previous_action = previous_value.get("action")
        except json.JSONDecodeError:
            pass
        all_covers_required_target = previous_action != Action.TEST.value
        if requested_target != required_target and not (
            requested_target == "all" and all_covers_required_target
        ):
            alternatives = (
                f"Run rebuild --target {required_target}."
                if previous_action == Action.TEST.value
                else f"Run rebuild --target {required_target}, or rebuild --target all."
            )
            raise BuildToolError(
                f'Interrupted target "{required_target}" cannot be recovered by rebuilding '
                f'target "{requested_target}".',
                recovery=alternatives,
            )
    if previous_content is not None and action is Action.RECOVER:
        required_target = recoverable_target(marker_file)
        if required_target is None:
            raise BuildToolError(
                "The interrupted DurinDevTool state cannot be resumed safely.",
                recovery="Run rebuild --target all with the affected preset.",
            )
        target_override = required_target
        metadata = {**metadata, "target": required_target}
    write_json_state(marker_file, metadata)
    try:
        operation(target_override)
    except BuildToolInterruptedError:
        raise
    except BuildToolError:
        restore_state_file(marker_file, previous_content)
        raise
    else:
        if action in {Action.REBUILD, Action.RECOVER} or previous_content is None:
            restore_state_file(marker_file, None)
        else:
            restore_state_file(marker_file, previous_content)
