from __future__ import annotations

import json
import os
import shutil
import stat
import uuid
from dataclasses import dataclass
from enum import Enum
from pathlib import Path
from typing import Callable, Iterable, Sequence

from .errors import BuildToolError
from .descriptors import load_module_descriptor, load_project_descriptor
from .scaffolding_workspace import (
    check_balanced_cmake,
    relative_display,
    resolved_within,
)


class PlanOperationKind(str, Enum):
    CREATE_DIRECTORY = "create directory"
    CREATE_FILE = "create file"
    REPLACE_FILE = "replace file"


@dataclass(frozen=True)
class PlanOperation:
    kind: PlanOperationKind
    path: Path
    content: bytes | None = None


PlanValidator = Callable[["ScaffoldPlan"], None]
FailureInjector = Callable[[str, int, Path], None]


@dataclass(frozen=True)
class ScaffoldPlan:
    root: Path
    allowed_roots: tuple[Path, ...]
    operations: tuple[PlanOperation, ...]
    validators: tuple[PlanValidator, ...] = ()

    def validate(self) -> None:
        errors: list[str] = []
        known: dict[str, PlanOperation] = {}
        plan_root = self.root.resolve()
        resolved_allowed = tuple(root.resolve() for root in self.allowed_roots)
        for allowed in resolved_allowed:
            if not resolved_within(allowed, plan_root):
                errors.append(f'Allowed plan root is outside the workspace: "{allowed}".')
        for operation in self.operations:
            lexical_path = operation.path.absolute()
            path = operation.path.resolve()
            key = str(path).casefold()
            previous = known.get(key)
            if previous is not None:
                errors.append(f'Plan targets "{path}" more than once.')
            known[key] = operation
            if not any(resolved_within(path, allowed) for allowed in resolved_allowed):
                errors.append(f'Plan path is outside the allowed roots: "{path}".')
            if operation.kind is PlanOperationKind.CREATE_DIRECTORY:
                if path.exists():
                    errors.append(f'Planned directory already exists: "{path}".')
                if operation.content is not None:
                    errors.append(f'Planned directory unexpectedly has content: "{path}".')
            elif operation.kind is PlanOperationKind.CREATE_FILE:
                if path.exists():
                    errors.append(f'Planned file already exists: "{path}".')
                if operation.content is None:
                    errors.append(f'Planned file has no content: "{path}".')
            elif operation.kind is PlanOperationKind.REPLACE_FILE:
                if not path.is_file():
                    errors.append(f'Planned replacement file does not exist: "{path}".')
                if operation.content is None:
                    errors.append(f'Planned replacement has no content: "{path}".')

            parent = lexical_path.parent
            if parent.is_dir():
                collisions = [
                    child
                    for child in parent.iterdir()
                    if child.name.casefold() == lexical_path.name.casefold()
                    and child.name != lexical_path.name
                ]
                if collisions:
                    errors.append(
                        f'Plan path has a case-insensitive collision with "{collisions[0]}".'
                    )
            if operation.content is not None:
                try:
                    text = operation.content.decode("utf-8")
                except UnicodeDecodeError:
                    text = ""
                if path.suffix.casefold() in {".dproject", ".dmodule"}:
                    try:
                        value = json.loads(text)
                    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
                        errors.append(
                            f'Planned descriptor is not valid UTF-8 JSON: "{path}" ({exc}).'
                        )
                    else:
                        if not isinstance(value, dict):
                            errors.append(
                                f'Planned descriptor must contain a JSON object: "{path}".'
                            )
                elif path.name.casefold() == "cmakelists.txt" or path.suffix.casefold() == ".cmake":
                    try:
                        check_balanced_cmake(text, path)
                    except BuildToolError as exc:
                        errors.append(str(exc))
        if errors:
            raise BuildToolError(
                "Scaffolding plan has conflicts:\n- " + "\n- ".join(sorted(set(errors)))
            )

    def format(self, *, plain: bool = True) -> str:
        lines = [f"Scaffolding plan ({len(self.operations)} operations)"]
        for operation in self.operations:
            path = relative_display(operation.path.resolve(), self.root.resolve())
            if plain:
                lines.append(f"  {operation.kind.value}: {path}")
            else:
                lines.append(f"  [cyan]{operation.kind.value}[/cyan]: {path}")
        return "\n".join(lines)


def _validate_affected_files(plan: ScaffoldPlan) -> None:
    for operation in plan.operations:
        if operation.kind is PlanOperationKind.CREATE_DIRECTORY:
            continue
        path = operation.path.resolve()
        if path.suffix.casefold() == ".dproject":
            load_project_descriptor(path)
        elif path.suffix.casefold() == ".dmodule":
            load_module_descriptor(path, "<transaction>")
        elif path.name.casefold() == "cmakelists.txt" or path.suffix.casefold() == ".cmake":
            try:
                content = path.read_text(encoding="utf-8")
            except OSError as exc:
                raise BuildToolError(
                    f'Could not reparse generated CMake file "{path}": {exc}'
                ) from exc
            check_balanced_cmake(content, path)


def execute_plan(
    plan: ScaffoldPlan,
    *,
    failure_injector: FailureInjector | None = None,
) -> None:
    plan.validate()
    created_files: list[Path] = []
    created_directories: list[Path] = []
    backups: list[tuple[Path, Path, int]] = []
    temporary_paths: list[Path] = []
    boundary = 0

    def inject(phase: str, path: Path) -> None:
        nonlocal boundary
        boundary += 1
        if failure_injector is not None:
            failure_injector(phase, boundary, path)

    def temporary_sibling(path: Path, purpose: str) -> Path:
        return path.with_name(f".{path.name}.{purpose}.{uuid.uuid4().hex}.tmp")

    try:
        for operation in plan.operations:
            path = operation.path.resolve()
            inject("before-operation", path)
            if operation.kind is PlanOperationKind.CREATE_DIRECTORY:
                path.mkdir()
                created_directories.append(path)
            else:
                assert operation.content is not None
                if operation.kind is PlanOperationKind.REPLACE_FILE:
                    backup = temporary_sibling(path, "backup")
                    shutil.copy2(path, backup)
                    backups.append((path, backup, stat.S_IMODE(path.stat().st_mode)))
                    temporary_paths.append(backup)
                    inject("after-backup", path)
                temporary = temporary_sibling(path, "write")
                temporary_paths.append(temporary)
                with temporary.open("xb") as stream:
                    stream.write(operation.content)
                    stream.flush()
                    os.fsync(stream.fileno())
                if operation.kind is PlanOperationKind.REPLACE_FILE:
                    temporary.chmod(backups[-1][2])
                inject("after-temporary-write", path)
                os.replace(temporary, path)
                temporary_paths.remove(temporary)
                if operation.kind is PlanOperationKind.CREATE_FILE:
                    created_files.append(path)
                inject("after-replace", path)
            inject("after-operation", path)

        _validate_affected_files(plan)
        for validator in plan.validators:
            validator(plan)
        inject("after-validation", plan.root.resolve())
    except BaseException:
        for path in reversed(created_files):
            try:
                path.unlink(missing_ok=True)
            except OSError:
                pass
        for path, backup, mode in reversed(backups):
            try:
                if backup.exists():
                    os.replace(backup, path)
                    path.chmod(mode)
            except OSError:
                pass
        for path in temporary_paths:
            try:
                path.unlink(missing_ok=True)
            except OSError:
                pass
        for path in reversed(created_directories):
            try:
                path.rmdir()
            except OSError:
                pass
        raise
    else:
        for _, backup, _ in backups:
            backup.unlink(missing_ok=True)


def ordered_plan(
    root: Path,
    allowed_roots: Sequence[Path],
    *,
    directories: Iterable[Path] = (),
    files: Iterable[tuple[Path, bytes]] = (),
    replacements: Iterable[tuple[Path, bytes]] = (),
    validators: Iterable[PlanValidator] = (),
) -> ScaffoldPlan:
    operations = tuple(
        [
            *(
                PlanOperation(PlanOperationKind.CREATE_DIRECTORY, path)
                for path in directories
            ),
            *(
                PlanOperation(PlanOperationKind.CREATE_FILE, path, content)
                for path, content in files
            ),
            *(
                PlanOperation(PlanOperationKind.REPLACE_FILE, path, content)
                for path, content in replacements
            ),
        ]
    )
    plan = ScaffoldPlan(
        root.resolve(),
        tuple(path.resolve() for path in allowed_roots),
        operations,
        tuple(validators),
    )
    plan.validate()
    return plan
