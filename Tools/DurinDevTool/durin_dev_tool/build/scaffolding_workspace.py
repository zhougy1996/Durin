from __future__ import annotations

import re
from dataclasses import dataclass
from pathlib import Path

from .errors import BuildToolError
from .descriptors import ProjectDescriptor, load_project_descriptor


ADD_SUBDIRECTORY_PATTERN = re.compile(
    r"(?im)^[ \t]*add_subdirectory[ \t]*\([ \t]*"
    r"(?:\"([^\"]+)\"|'([^']+)'|([^)'\" \t\r\n]+))"
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


def relative_display(path: Path, root: Path) -> str:
    try:
        return path.relative_to(root).as_posix()
    except ValueError:
        return str(path)


def resolved_within(path: Path, root: Path) -> bool:
    try:
        path.resolve().relative_to(root.resolve())
    except ValueError:
        return False
    return True


def require_contained_path(path: Path, root: Path, *, label: str) -> Path:
    candidate = path if path.is_absolute() else root / path
    resolved = candidate.resolve()
    if not resolved_within(resolved, root):
        raise BuildToolError(f'{label} must stay inside "{root.resolve()}": "{resolved}".')
    return resolved


def parse_cmake_values(pattern: re.Pattern[str], content: str) -> tuple[str, ...]:
    values = []
    for match in pattern.finditer(content):
        value = next(group for group in match.groups() if group is not None)
        values.append(value.replace("\\", "/").rstrip("/"))
    return tuple(values)


def check_balanced_cmake(content: str, path: Path) -> None:
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
    check_balanced_cmake(cmake_content, root_cmake)

    registrations = parse_cmake_values(ADD_SUBDIRECTORY_PATTERN, cmake_content)
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
        target_names.extend(parse_cmake_values(CMAKE_TARGET_PATTERN, content))
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
        if (
            resolved == project.root
            or resolved_within(resolved, project.root)
            or resolved_within(project.root, resolved)
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
    collision = next(
        (target for target in discovery.cmake_targets if target.casefold() == name.casefold()),
        None,
    )
    if collision is not None:
        raise BuildToolError(f'CMake target "{name}" conflicts with existing target "{collision}".')
