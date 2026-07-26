from __future__ import annotations

import json
import os
import re
import shutil
import stat
import uuid
from dataclasses import dataclass
from enum import Enum
from pathlib import Path
from typing import Callable, Iterable, Mapping, Sequence

from .config import BuildToolError, PACKAGE_DIR
from .descriptors import (
    ProjectDescriptor,
    load_module_descriptor,
    load_project_descriptor,
)


TEMPLATE_DIR = PACKAGE_DIR / "templates"
TEMPLATE_VARIABLE_PATTERN = re.compile(r"\{\{([A-Z][A-Z0-9_]*)\}\}")
ADD_SUBDIRECTORY_PATTERN = re.compile(
    r"(?im)^[ \t]*add_subdirectory[ \t]*\([ \t]*[\"']?([^\"') \t\r\n]+)"
)
CMAKE_TARGET_PATTERN = re.compile(
    r"(?im)^[ \t]*(?:add_durin_module|add_library|add_executable)[ \t]*"
    r"\([ \t]*[\"']?([^\"') \t\r\n]+)"
)


@dataclass(frozen=True)
class WorkspaceProject:
    descriptor: ProjectDescriptor
    root: Path
    cmake_registration: str


@dataclass(frozen=True)
class WorkspaceDiscovery:
    root: Path
    root_cmake: Path
    projects: tuple[WorkspaceProject, ...]
    cmake_targets: tuple[str, ...]


def _relative_display(path: Path, root: Path) -> str:
    try:
        return path.relative_to(root).as_posix()
    except ValueError:
        return str(path)


def _resolved_within(path: Path, root: Path) -> bool:
    try:
        path.resolve().relative_to(root.resolve())
    except ValueError:
        return False
    return True


def require_contained_path(path: Path, root: Path, *, label: str) -> Path:
    candidate = path if path.is_absolute() else root / path
    resolved = candidate.resolve()
    if not _resolved_within(resolved, root):
        raise BuildToolError(f'{label} must stay inside "{root.resolve()}": "{resolved}".')
    return resolved


def _parse_cmake_values(pattern: re.Pattern[str], content: str) -> tuple[str, ...]:
    return tuple(match.group(1).replace("\\", "/").rstrip("/") for match in pattern.finditer(content))


def _check_balanced_cmake(content: str, path: Path) -> None:
    depth = 0
    quoted = False
    escaped = False
    for character in content:
        if escaped:
            escaped = False
            continue
        if character == "\\":
            escaped = quoted
            continue
        if character == '"':
            quoted = not quoted
        elif not quoted and character == "(":
            depth += 1
        elif not quoted and character == ")":
            depth -= 1
            if depth < 0:
                break
    if quoted or depth != 0:
        raise BuildToolError(f'CMake file has unbalanced syntax: "{path}".')


def discover_workspace_projects(root: Path) -> WorkspaceDiscovery:
    workspace_root = root.resolve()
    root_cmake = workspace_root / "CMakeLists.txt"
    try:
        cmake_content = root_cmake.read_text(encoding="utf-8")
    except FileNotFoundError as exc:
        raise BuildToolError(f'Workspace root CMake file was not found: "{root_cmake}".') from exc
    except OSError as exc:
        raise BuildToolError(f'Could not read workspace root CMake file "{root_cmake}": {exc}') from exc
    _check_balanced_cmake(cmake_content, root_cmake)

    registrations = _parse_cmake_values(ADD_SUBDIRECTORY_PATTERN, cmake_content)
    registrations_by_root: dict[str, list[str]] = {}
    for registration in registrations:
        registration_root = registration.split("/", 1)[0]
        registrations_by_root.setdefault(registration_root.casefold(), []).append(registration)

    descriptor_paths = sorted(
        workspace_root.glob("*/*.dproject"),
        key=lambda path: path.as_posix().casefold(),
    )
    projects: list[WorkspaceProject] = []
    seen_roots: set[str] = set()
    seen_names: dict[str, Path] = {}
    errors: list[str] = []
    for descriptor_path in descriptor_paths:
        project_root = descriptor_path.parent.resolve()
        root_key = project_root.name.casefold()
        if root_key in seen_roots:
            errors.append(f'Multiple project descriptors exist in "{project_root}".')
            continue
        seen_roots.add(root_key)
        project = load_project_descriptor(descriptor_path)
        previous_descriptor = seen_names.get(project.name.casefold())
        if previous_descriptor is not None:
            errors.append(
                f'Duplicate project name "{project.name}" in "{previous_descriptor}" '
                f'and "{project.path}".'
            )
            continue
        seen_names[project.name.casefold()] = project.path
        matching = registrations_by_root.get(root_key, [])
        if len(matching) != 1:
            errors.append(
                f'Project "{project.name}" must have exactly one root add_subdirectory registration '
                f'for "{project_root.name}" (found {len(matching)}).'
            )
            continue
        projects.append(WorkspaceProject(project, project_root, matching[0]))

    if errors:
        raise BuildToolError("Workspace project discovery failed:\n- " + "\n- ".join(sorted(errors)))

    target_names: list[str] = []
    cmake_paths = {root_cmake}
    for project in projects:
        cmake_paths.update(project.root.rglob("CMakeLists.txt"))
    for cmake_path in sorted(cmake_paths, key=lambda path: str(path).casefold()):
        relative_parts = cmake_path.relative_to(workspace_root).parts
        if any(
            part.casefold() in {"build", "intermediate", "external", ".git"}
            for part in relative_parts
        ):
            continue
        try:
            content = cmake_path.read_text(encoding="utf-8")
        except OSError as exc:
            raise BuildToolError(f'Could not read CMake file "{cmake_path}": {exc}') from exc
        target_names.extend(_parse_cmake_values(CMAKE_TARGET_PATTERN, content))
    return WorkspaceDiscovery(
        workspace_root,
        root_cmake,
        tuple(projects),
        tuple(sorted(set(target_names), key=str.casefold)),
    )


def validate_destination(
    destination: Path,
    discovery: WorkspaceDiscovery,
    *,
    label: str,
    allow_existing: bool = False,
) -> Path:
    candidate = destination if destination.is_absolute() else discovery.root / destination
    resolved = require_contained_path(candidate, discovery.root, label=label)
    errors: list[str] = []
    if resolved == discovery.root:
        errors.append(f"{label} cannot be the workspace root.")
    if resolved.exists() and not allow_existing:
        errors.append(f'{label} already exists: "{resolved}".')
    for project in discovery.projects:
        if resolved == project.root or _resolved_within(resolved, project.root) or _resolved_within(
            project.root, resolved
        ):
            errors.append(f'{label} overlaps project "{project.descriptor.name}" at "{project.root}".')
    parent = candidate.parent
    if parent.is_dir():
        folded = candidate.name.casefold()
        collisions = sorted(
            child.name
            for child in parent.iterdir()
            if child.name.casefold() == folded and child.name != candidate.name
        )
        if collisions:
            errors.append(
                f'{label} has a case-insensitive collision with "{parent / collisions[0]}".'
            )
    if errors:
        raise BuildToolError("\n".join(errors))
    return resolved


def require_available_cmake_target(name: str, discovery: WorkspaceDiscovery) -> None:
    collision = next((target for target in discovery.cmake_targets if target.casefold() == name.casefold()), None)
    if collision is not None:
        raise BuildToolError(f'CMake target "{name}" conflicts with existing target "{collision}".')


class TemplateRenderer:
    def __init__(self, template_root: Path = TEMPLATE_DIR):
        self.template_root = template_root.resolve()

    def render(self, template_name: str, variables: Mapping[str, str]) -> bytes:
        template_path = require_contained_path(
            Path(template_name),
            self.template_root,
            label="Template path",
        )
        try:
            template = template_path.read_text(encoding="utf-8")
        except FileNotFoundError as exc:
            raise BuildToolError(f'Scaffolding template was not found: "{template_path}".') from exc
        except OSError as exc:
            raise BuildToolError(f'Could not read scaffolding template "{template_path}": {exc}') from exc
        expected = set(TEMPLATE_VARIABLE_PATTERN.findall(template))
        supplied = set(variables)
        missing = sorted(expected - supplied)
        unknown = sorted(supplied - expected)
        if missing or unknown:
            details = []
            if missing:
                details.append("missing " + ", ".join(missing))
            if unknown:
                details.append("unknown " + ", ".join(unknown))
            raise BuildToolError(
                f'Template variables for "{template_name}" are invalid: {"; ".join(details)}.'
            )
        rendered = TEMPLATE_VARIABLE_PATTERN.sub(lambda match: variables[match.group(1)], template)
        unresolved = TEMPLATE_VARIABLE_PATTERN.findall(rendered)
        if unresolved:
            raise BuildToolError(
                f'Template "{template_name}" left unresolved variables: {", ".join(unresolved)}.'
            )
        return rendered.replace("\r\n", "\n").encode("utf-8")


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
            if not _resolved_within(allowed, plan_root):
                errors.append(f'Allowed plan root is outside the workspace: "{allowed}".')
        for operation in self.operations:
            lexical_path = operation.path.absolute()
            path = operation.path.resolve()
            key = str(path).casefold()
            previous = known.get(key)
            if previous is not None:
                errors.append(f'Plan targets "{path}" more than once.')
            known[key] = operation
            if not any(_resolved_within(path, allowed) for allowed in resolved_allowed):
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
                        errors.append(f'Planned descriptor is not valid UTF-8 JSON: "{path}" ({exc}).')
                    else:
                        if not isinstance(value, dict):
                            errors.append(f'Planned descriptor must contain a JSON object: "{path}".')
                elif path.name.casefold() == "cmakelists.txt" or path.suffix.casefold() == ".cmake":
                    try:
                        _check_balanced_cmake(text, path)
                    except BuildToolError as exc:
                        errors.append(str(exc))
        if errors:
            raise BuildToolError("Scaffolding plan has conflicts:\n- " + "\n- ".join(sorted(set(errors))))

    def format(self, *, plain: bool = True) -> str:
        lines = [f"Scaffolding plan ({len(self.operations)} operations)"]
        for operation in self.operations:
            path = _relative_display(operation.path.resolve(), self.root.resolve())
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
                raise BuildToolError(f'Could not reparse generated CMake file "{path}": {exc}') from exc
            _check_balanced_cmake(content, path)


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


def json_template_value(value: object, *, indentation: int = 4) -> str:
    rendered = json.dumps(value, indent=4, ensure_ascii=False)
    continuation = " " * indentation
    return rendered.replace("\n", "\n" + continuation)


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
            *(PlanOperation(PlanOperationKind.CREATE_DIRECTORY, path) for path in directories),
            *(PlanOperation(PlanOperationKind.CREATE_FILE, path, content) for path, content in files),
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
