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

from .config import (
    BuildToolError,
    CommandRequest,
    CreateKind,
    LinkType,
    ModuleKind,
    PACKAGE_DIR,
)
from .descriptors import (
    ProjectDescriptor,
    WorkspaceDescriptors,
    load_module_descriptor,
    load_project_descriptor,
    load_workspace_descriptors,
    validate_create_request,
)


TEMPLATE_DIR = PACKAGE_DIR / "templates"
TEMPLATE_VARIABLE_PATTERN = re.compile(r"\{\{([A-Z][A-Z0-9_]*)\}\}")
ADD_SUBDIRECTORY_PATTERN = re.compile(
    r"(?im)^[ \t]*add_subdirectory[ \t]*\([ \t]*"
    r"(?:\"([^\"]+)\"|'([^']+)'|([^)'\" \t\r\n]+))"
)
CMAKE_TARGET_PATTERN = re.compile(
    r"(?im)^[ \t]*(?:add_durin_module|add_library|add_executable)[ \t]*"
    r"\([ \t]*[\"']?([^\"') \t\r\n]+)"
)
ROOT_ADD_SUBDIRECTORY_LINE_PATTERN = re.compile(
    r"(?im)^[ \t]*add_subdirectory[ \t]*\([^\r\n]*\)[ \t]*(?:#[^\r\n]*)?(?:\r?\n|$)"
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
    values = []
    for match in pattern.finditer(content):
        value = next(group for group in match.groups() if group is not None)
        values.append(value.replace("\\", "/").rstrip("/"))
    return tuple(values)


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


def _json_indentation(content: str) -> int:
    for line in content.splitlines():
        stripped = line.lstrip(" ")
        if stripped and stripped != line:
            return len(line) - len(stripped)
    return 4


def _render_updated_project_descriptor(
    project: ProjectDescriptor,
    *,
    module_name: str,
    module_directory: str,
    enablements: Sequence[str],
) -> bytes:
    original = project.path.read_bytes()
    text = original.decode("utf-8")
    data = json.loads(text)
    module_dirs = data.setdefault("ModuleDirs", {})
    module_dirs[module_name] = module_directory
    for enablement in enablements:
        if enablement.casefold() == "base":
            modules = data.setdefault("BaseModules", [])
        else:
            runtime_variants = data.setdefault("ExtraModules", {})
            runtime_variant = next(
                (
                    value
                    for name, value in runtime_variants.items()
                    if name.casefold() == enablement.casefold()
                ),
                None,
            )
            if runtime_variant is None:
                runtime_variant = {"Modules": []}
                runtime_variants[enablement] = runtime_variant
            modules = runtime_variant.setdefault("Modules", [])
        if all(existing.casefold() != module_name.casefold() for existing in modules):
            modules.append(module_name)
    newline = "\r\n" if b"\r\n" in original else "\n"
    rendered = json.dumps(
        data,
        indent=_json_indentation(text),
        ensure_ascii=False,
    )
    return (rendered.replace("\n", newline) + newline).encode("utf-8")


def _canonical_enablements(
    request: CommandRequest,
    workspace: WorkspaceDescriptors,
) -> tuple[str, ...]:
    requested = request.enablements
    if requested is None:
        return (
            ("base",)
            if request.module_kind is ModuleKind.RUNTIME
            else ("DurinEditor",)
        )
    if len(requested) == 1 and requested[0].casefold() == "none":
        return ()
    runtime_variants = {name.casefold(): name for name in workspace.runtime_variants}
    return tuple(
        "base"
        if value.casefold() == "base"
        else runtime_variants[value.casefold()]
        for value in requested
    )


def _render_module_files(
    module_name: str,
    module_directory: Path,
    template_renderer: TemplateRenderer,
    *,
    link_type: LinkType = LinkType.SHARED,
    pch: str = "",
    private_dependencies: Sequence[str] = (),
    public_dependencies: Sequence[str] = (),
    optional_private_dependencies: Sequence[str] = (),
    optional_public_dependencies: Sequence[str] = (),
) -> list[tuple[Path, bytes]]:
    descriptor_variables = {
        "MODULE_NAME": module_name,
        "LINK_TYPE": "Static" if link_type is LinkType.STATIC else "Shared",
        "PCH": pch or "Self",
        "PRIVATE_DEPENDENCIES": json_template_value(list(private_dependencies)),
        "PUBLIC_DEPENDENCIES": json_template_value(list(public_dependencies)),
        "OPTIONAL_PRIVATE_DEPENDENCIES": json_template_value(
            list(optional_private_dependencies)
        ),
        "OPTIONAL_PUBLIC_DEPENDENCIES": json_template_value(
            list(optional_public_dependencies)
        ),
    }
    generated_files = [
        (
            module_directory / f"{module_name}.dmodule",
            template_renderer.render(
                "module/descriptor.json.template",
                descriptor_variables,
            ),
        ),
        (
            module_directory / "CMakeLists.txt",
            template_renderer.render(
                "module/CMakeLists.txt.template",
                {"MODULE_NAME": module_name},
            ),
        ),
        (
            module_directory / "Private" / f"{module_name}Module.cpp",
            template_renderer.render(
                "module/entry_point.cpp.template",
                {"MODULE_NAME": module_name},
            ),
        ),
        (
            module_directory / "Public" / f"{module_name}API.h",
            template_renderer.render(
                "module/api.h.template",
                {"MODULE_NAME_UPPER": module_name.upper()},
            ),
        ),
    ]
    if (pch or "Self").casefold() == "self":
        generated_files.append(
            (
                module_directory / "Private" / f"PCH.{module_name}.h",
                template_renderer.render("module/pch.h.template", {}),
            )
        )
    return generated_files


def plan_module_creation(
    request: CommandRequest,
    root: Path,
    *,
    renderer: TemplateRenderer | None = None,
) -> ScaffoldPlan:
    if request.create_kind is not CreateKind.MODULE:
        raise BuildToolError("Module scaffolding requires a create module request.")
    discovery = discover_workspace_projects(root)
    project_paths = tuple(project.descriptor.path for project in discovery.projects)
    workspace = load_workspace_descriptors(discovery.root, project_paths=project_paths)
    validate_create_request(request, workspace)
    assert request.project_path is not None
    requested_project_path = request.project_path
    if not requested_project_path.is_absolute():
        requested_project_path = discovery.root / requested_project_path
    requested_project_path = requested_project_path.resolve()
    project = next(
        item for item in workspace.projects if item.path == requested_project_path
    )
    workspace_project = next(
        item for item in discovery.projects if item.descriptor.path == requested_project_path
    )
    require_available_cmake_target(request.create_name, discovery)

    if request.destination_path is None:
        kind_directory = "Runtime" if request.module_kind is ModuleKind.RUNTIME else "Editor"
        requested_module_directory = (
            workspace_project.root / "Source" / kind_directory / request.create_name
        )
    else:
        requested_module_directory = request.destination_path
        if not requested_module_directory.is_absolute():
            requested_module_directory = workspace_project.root / requested_module_directory
    module_directory = require_contained_path(
        requested_module_directory,
        workspace_project.root,
        label="Module destination",
    )
    errors: list[str] = []
    if module_directory == workspace_project.root:
        errors.append("Module destination cannot be the project root.")
    if module_directory.exists():
        errors.append(f'Module destination already exists: "{module_directory}".')
    for existing_module in workspace.modules:
        existing_root = existing_module.path.parent.resolve()
        if (
            module_directory == existing_root
            or _resolved_within(module_directory, existing_root)
            or _resolved_within(existing_root, module_directory)
        ):
            errors.append(
                f'Module destination overlaps module "{existing_module.name}" '
                f'at "{existing_root}".'
            )
    parent = module_directory.parent
    if parent.is_dir() and any(
        child.name.casefold() == module_directory.name.casefold()
        for child in parent.iterdir()
    ):
        collision = next(
            child
            for child in parent.iterdir()
            if child.name.casefold() == module_directory.name.casefold()
        )
        errors.append(f'Module destination conflicts with existing path: "{collision}".')
    if errors:
        raise BuildToolError("\n".join(errors))

    enablements = _canonical_enablements(request, workspace)
    template_renderer = renderer or TemplateRenderer()
    relative_module_directory = module_directory.relative_to(workspace_project.root).as_posix()
    updated_project = _render_updated_project_descriptor(
        project,
        module_name=request.create_name,
        module_directory=relative_module_directory,
        enablements=enablements,
    )
    generated_files = _render_module_files(
        request.create_name,
        module_directory,
        template_renderer,
        link_type=request.link_type,
        pch=request.pch,
        private_dependencies=request.private_dependencies,
        public_dependencies=request.public_dependencies,
        optional_private_dependencies=request.optional_private_dependencies,
        optional_public_dependencies=request.optional_public_dependencies,
    )

    def validate_workspace_after_creation(_: ScaffoldPlan) -> None:
        load_workspace_descriptors(discovery.root, project_paths=project_paths)

    missing_directories: list[Path] = []
    candidate = module_directory
    while not candidate.exists():
        missing_directories.append(candidate)
        candidate = candidate.parent
    if not _resolved_within(candidate, workspace_project.root):
        raise BuildToolError(
            f'Module destination must stay inside project "{workspace_project.root}".'
        )
    missing_directories.reverse()

    return ordered_plan(
        discovery.root,
        (workspace_project.root,),
        directories=(
            *missing_directories,
            module_directory / "Private",
            module_directory / "Public",
        ),
        files=generated_files,
        replacements=((project.path, updated_project),),
        validators=(validate_workspace_after_creation,),
    )


def _render_root_project_registration(
    root_cmake: Path,
    discovery: WorkspaceDiscovery,
    project_directory_name: str,
) -> bytes:
    original = root_cmake.read_bytes()
    text = original.decode("utf-8")
    newline = "\r\n" if b"\r\n" in original else "\n"
    registration = f'add_subdirectory("{project_directory_name}"){newline}'
    matches = tuple(ROOT_ADD_SUBDIRECTORY_LINE_PATTERN.finditer(text))
    if not matches:
        raise BuildToolError(
            f'Workspace root CMake file has no project add_subdirectory registration: "{root_cmake}".'
        )

    known_registrations = {
        project.cmake_registration.casefold() for project in discovery.projects
    }
    insertion_match = None
    for match in matches:
        values = _parse_cmake_values(ADD_SUBDIRECTORY_PATTERN, match.group(0))
        if values and values[0].casefold() in known_registrations:
            insertion_match = match
    if insertion_match is None:
        raise BuildToolError(
            f'Workspace root CMake project registrations could not be located: "{root_cmake}".'
        )
    separator = "" if insertion_match.group(0).endswith(("\n", "\r")) else newline
    updated = (
        text[: insertion_match.end()]
        + separator
        + registration
        + text[insertion_match.end() :]
    )
    return updated.encode("utf-8")


def plan_project_creation(
    request: CommandRequest,
    root: Path,
    *,
    renderer: TemplateRenderer | None = None,
) -> ScaffoldPlan:
    if request.create_kind is not CreateKind.PROJECT:
        raise BuildToolError("Project scaffolding requires a create project request.")
    if request.destination_path is None:
        raise BuildToolError("create project requires --path <path>.")

    discovery = discover_workspace_projects(root)
    project_paths = tuple(project.descriptor.path for project in discovery.projects)
    workspace = load_workspace_descriptors(discovery.root, project_paths=project_paths)
    validate_create_request(request, workspace)
    require_available_cmake_target(request.create_name, discovery)
    destination = validate_destination(
        request.destination_path,
        discovery,
        label="Project destination",
    )
    if destination.parent != discovery.root:
        raise BuildToolError(
            "Project destination must be a direct child of the workspace root so it can be "
            f'registered safely: "{destination}".'
        )
    if any(character in destination.name for character in ('"', "\\", "$", ";", "\r", "\n")):
        raise BuildToolError(
            f'Project destination cannot be expressed safely in root CMake: "{destination}".'
        )
    root_registrations = _parse_cmake_values(
        ADD_SUBDIRECTORY_PATTERN,
        discovery.root_cmake.read_text(encoding="utf-8"),
    )
    if any(
        registration.casefold() == destination.name.casefold()
        for registration in root_registrations
    ):
        raise BuildToolError(
            f'Project destination already has a root CMake registration: "{destination.name}".'
        )

    template_renderer = renderer or TemplateRenderer()
    module_directory = destination / "Source" / "Runtime" / request.create_name
    generated_files = [
        (
            destination / f"{request.create_name}.dproject",
            template_renderer.render(
                "project/descriptor.json.template",
                {"PROJECT_NAME": request.create_name},
            ),
        ),
        (
            destination / "CMakeLists.txt",
            template_renderer.render(
                "project/CMakeLists.txt.template",
                {"PROJECT_NAME": request.create_name},
            ),
        ),
        (
            destination / "CMake" / f"{request.create_name}Setup.cmake",
            template_renderer.render(
                "project/setup.cmake.template",
                {"PROJECT_NAME": request.create_name},
            ),
        ),
        *_render_module_files(
            request.create_name,
            module_directory,
            template_renderer,
            private_dependencies=("Core",),
        ),
    ]
    updated_root_cmake = _render_root_project_registration(
        discovery.root_cmake,
        discovery,
        destination.name,
    )

    def validate_workspace_after_creation(_: ScaffoldPlan) -> None:
        updated_discovery = discover_workspace_projects(discovery.root)
        updated_paths = tuple(
            project.descriptor.path for project in updated_discovery.projects
        )
        updated_workspace = load_workspace_descriptors(
            discovery.root,
            project_paths=updated_paths,
        )
        project = updated_workspace.find_project(request.create_name)
        module = updated_workspace.find_module(request.create_name)
        if project is None or module is None or module.owning_project != project.name:
            raise BuildToolError(
                f'Generated project "{request.create_name}" failed final workspace validation.'
            )

    return ordered_plan(
        discovery.root,
        (discovery.root,),
        directories=(
            destination,
            destination / "CMake",
            destination / "Configs",
            destination / "Content",
            destination / "Source",
            destination / "Source" / "Runtime",
            module_directory,
            module_directory / "Private",
            module_directory / "Public",
        ),
        files=generated_files,
        replacements=((discovery.root_cmake, updated_root_cmake),),
        validators=(validate_workspace_after_creation,),
    )


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
