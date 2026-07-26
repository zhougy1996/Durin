from __future__ import annotations

import json
import re
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Iterable, Mapping, Sequence

from .config import BuildToolError, CommandRequest, CreateKind, ModuleKind


DEPENDENCY_FIELDS = (
    "PrivateDependencies",
    "PublicDependencies",
    "OptionalPrivateDependencies",
    "OptionalPublicDependencies",
)
CPP_IDENTIFIER_PATTERN = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")


@dataclass(frozen=True)
class ProjectProfileDescriptor:
    modules: tuple[str, ...] = ()


@dataclass(frozen=True)
class ProjectDescriptor:
    name: str
    path: Path
    module_dirs: Mapping[str, str]
    base_modules: tuple[str, ...]
    extra_modules: Mapping[str, ProjectProfileDescriptor]


@dataclass(frozen=True)
class ModuleDescriptor:
    name: str
    path: Path
    owning_project: str
    link_type: str = "Shared"
    pch: str = "Self"
    private_dependencies: tuple[str, ...] = ()
    public_dependencies: tuple[str, ...] = ()
    optional_private_dependencies: tuple[str, ...] = ()
    optional_public_dependencies: tuple[str, ...] = ()
    reflect_headers: tuple[str, ...] = ()

    @property
    def dependencies(self) -> tuple[str, ...]:
        return (
            self.private_dependencies
            + self.public_dependencies
            + self.optional_private_dependencies
            + self.optional_public_dependencies
        )


@dataclass(frozen=True)
class WorkspaceDescriptors:
    root: Path
    projects: tuple[ProjectDescriptor, ...]
    modules: tuple[ModuleDescriptor, ...]

    @property
    def profile_names(self) -> tuple[str, ...]:
        return tuple(
            sorted(
                {
                    profile
                    for project in self.projects
                    for profile in project.extra_modules
                },
                key=str.casefold,
            )
        )

    def find_project(self, name: str) -> ProjectDescriptor | None:
        folded = name.casefold()
        return next((project for project in self.projects if project.name.casefold() == folded), None)

    def find_module(self, name: str) -> ModuleDescriptor | None:
        folded = name.casefold()
        return next((module for module in self.modules if module.name.casefold() == folded), None)


def _load_json_object(path: Path, label: str) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except FileNotFoundError as exc:
        raise BuildToolError(f'{label} was not found: "{path}"') from exc
    except json.JSONDecodeError as exc:
        raise BuildToolError(
            f'{label} contains malformed JSON at line {exc.lineno}, column {exc.colno}: "{path}"'
        ) from exc
    except OSError as exc:
        raise BuildToolError(f'Could not read {label.lower()} "{path}": {exc}') from exc
    if not isinstance(value, dict):
        raise BuildToolError(f'{label} must contain a JSON object: "{path}"')
    return value


def _required_string(data: Mapping[str, Any], key: str, path: Path) -> str:
    if key not in data:
        raise BuildToolError(f'Descriptor "{path}" is missing required field "{key}".')
    value = data[key]
    if not isinstance(value, str) or not value:
        raise BuildToolError(f'Descriptor "{path}" field "{key}" must be a non-empty string.')
    return value


def _optional_string(data: Mapping[str, Any], key: str, default: str, path: Path) -> str:
    value = data.get(key, default)
    if not isinstance(value, str) or not value:
        raise BuildToolError(f'Descriptor "{path}" field "{key}" must be a non-empty string.')
    return value


def _string_list(data: Mapping[str, Any], key: str, path: Path) -> tuple[str, ...]:
    value = data.get(key, [])
    if not isinstance(value, list) or any(not isinstance(item, str) or not item for item in value):
        raise BuildToolError(f'Descriptor "{path}" field "{key}" must be an array of non-empty strings.')
    folded: set[str] = set()
    for item in value:
        if item.casefold() in folded:
            raise BuildToolError(f'Descriptor "{path}" field "{key}" contains duplicate "{item}".')
        folded.add(item.casefold())
    return tuple(value)


def load_project_descriptor(path: Path) -> ProjectDescriptor:
    resolved = path.resolve()
    data = _load_json_object(resolved, "Project descriptor")
    name = _required_string(data, "ProjectName", resolved)
    raw_module_dirs = data.get("ModuleDirs", {})
    if not isinstance(raw_module_dirs, dict) or any(
        not isinstance(module, str)
        or not module
        or not isinstance(directory, str)
        or not directory
        for module, directory in raw_module_dirs.items()
    ):
        raise BuildToolError(
            f'Descriptor "{resolved}" field "ModuleDirs" must be an object of non-empty string paths.'
        )
    folded_modules: set[str] = set()
    for module in raw_module_dirs:
        if module.casefold() in folded_modules:
            raise BuildToolError(f'Descriptor "{resolved}" contains duplicate module name "{module}".')
        folded_modules.add(module.casefold())

    base_modules = _string_list(data, "BaseModules", resolved)
    if "BaseModules" not in data and raw_module_dirs:
        base_modules = tuple(raw_module_dirs)

    raw_profiles = data.get("ExtraModules", {})
    if not isinstance(raw_profiles, dict):
        raise BuildToolError(f'Descriptor "{resolved}" field "ExtraModules" must be an object.')
    profiles: dict[str, ProjectProfileDescriptor] = {}
    folded_profiles: set[str] = set()
    for profile_name, profile_data in raw_profiles.items():
        if not isinstance(profile_name, str) or not profile_name:
            raise BuildToolError(f'Descriptor "{resolved}" contains an invalid profile name.')
        if profile_name.casefold() in folded_profiles:
            raise BuildToolError(f'Descriptor "{resolved}" contains duplicate profile "{profile_name}".')
        folded_profiles.add(profile_name.casefold())
        if not isinstance(profile_data, dict):
            raise BuildToolError(
                f'Descriptor "{resolved}" profile "{profile_name}" must contain an object.'
            )
        profiles[profile_name] = ProjectProfileDescriptor(
            _string_list(profile_data, "Modules", resolved)
        )
    return ProjectDescriptor(
        name=name,
        path=resolved,
        module_dirs=dict(raw_module_dirs),
        base_modules=base_modules,
        extra_modules=profiles,
    )


def load_module_descriptor(path: Path, owning_project: str) -> ModuleDescriptor:
    resolved = path.resolve()
    data = _load_json_object(resolved, "Module descriptor")
    name = _required_string(data, "ModuleName", resolved)
    dependencies = {field: _string_list(data, field, resolved) for field in DEPENDENCY_FIELDS}
    return ModuleDescriptor(
        name=name,
        path=resolved,
        owning_project=owning_project,
        link_type=_optional_string(data, "LinkType", "Shared", resolved),
        pch=_optional_string(data, "PCH", "Self", resolved),
        private_dependencies=dependencies["PrivateDependencies"],
        public_dependencies=dependencies["PublicDependencies"],
        optional_private_dependencies=dependencies["OptionalPrivateDependencies"],
        optional_public_dependencies=dependencies["OptionalPublicDependencies"],
        reflect_headers=_string_list(data, "ReflectHeaders", resolved),
    )


def _reject_duplicate(
    values: Iterable[tuple[str, Path]],
    *,
    label: str,
) -> None:
    known: dict[str, tuple[str, Path]] = {}
    for name, path in values:
        previous = known.get(name.casefold())
        if previous is not None:
            raise BuildToolError(
                f'Duplicate {label} name "{name}" in "{previous[1]}" and "{path}".'
            )
        known[name.casefold()] = (name, path)


def load_workspace_descriptors(
    root: Path,
    *,
    project_paths: Sequence[Path] | None = None,
) -> WorkspaceDescriptors:
    paths = (
        sorted((path.resolve() for path in project_paths), key=lambda path: str(path).casefold())
        if project_paths is not None
        else sorted(root.resolve().glob("*/*.dproject"), key=lambda path: str(path).casefold())
    )
    projects = tuple(load_project_descriptor(path) for path in paths)
    _reject_duplicate(((project.name, project.path) for project in projects), label="project")

    modules: list[ModuleDescriptor] = []
    for project in projects:
        for registered_name, relative_directory in project.module_dirs.items():
            module_path = project.path.parent / relative_directory / f"{registered_name}.dmodule"
            module = load_module_descriptor(module_path, project.name)
            if module.name != registered_name:
                raise BuildToolError(
                    f'Module descriptor "{module.path}" declares "{module.name}" but project '
                    f'"{project.name}" registers it as "{registered_name}".'
                )
            modules.append(module)
    _reject_duplicate(((module.name, module.path) for module in modules), label="module")
    workspace = WorkspaceDescriptors(root.resolve(), projects, tuple(modules))
    _validate_workspace_references(workspace)
    return workspace


def _validate_workspace_references(workspace: WorkspaceDescriptors) -> None:
    module_names = {module.name.casefold(): module.name for module in workspace.modules}
    for project in workspace.projects:
        owned = {name.casefold() for name in project.module_dirs}
        for profile_name, roots in (
            ("BaseModules", project.base_modules),
            *((name, profile.modules) for name, profile in project.extra_modules.items()),
        ):
            for module_name in roots:
                if module_name.casefold() not in owned:
                    raise BuildToolError(
                        f'Project "{project.name}" enables missing module "{module_name}" '
                        f'in profile "{profile_name}".'
                    )
    for module in workspace.modules:
        for dependency in module.dependencies:
            if dependency.casefold() == module.name.casefold():
                raise BuildToolError(f'Module "{module.name}" cannot depend on itself.')
            if dependency.casefold() not in module_names:
                raise BuildToolError(
                    f'Module "{module.name}" depends on missing module "{dependency}".'
                )


def validate_create_request(request: CommandRequest, workspace: WorkspaceDescriptors) -> None:
    if request.create_kind is None:
        return
    if not CPP_IDENTIFIER_PATTERN.fullmatch(request.create_name):
        raise BuildToolError(
            f'Create {request.create_kind.value} name "{request.create_name}" must be a valid C++ identifier.'
        )
    if request.create_kind is CreateKind.PROJECT:
        if workspace.find_project(request.create_name) is not None:
            raise BuildToolError(f'Project name "{request.create_name}" already exists.')
        if workspace.find_module(request.create_name) is not None:
            raise BuildToolError(
                f'Initial module name "{request.create_name}" already exists in the workspace.'
            )
        return
    if request.project_path is None:
        raise BuildToolError("create module requires --project <descriptor>.")
    if workspace.find_module(request.create_name) is not None:
        raise BuildToolError(f'Module name "{request.create_name}" already exists.')
    project_path = request.project_path
    if not project_path.is_absolute():
        project_path = workspace.root / project_path
    project_path = project_path.resolve()
    if project_path.suffix.casefold() != ".dproject":
        raise BuildToolError(f'Project descriptor must use the .dproject extension: "{project_path}".')
    if all(project.path != project_path for project in workspace.projects):
        raise BuildToolError(f'Project descriptor is not registered in the workspace: "{project_path}".')

    dependencies = (
        request.private_dependencies
        + request.public_dependencies
        + request.optional_private_dependencies
        + request.optional_public_dependencies
    )
    seen_dependencies: set[str] = set()
    for dependency in dependencies:
        if dependency.casefold() in seen_dependencies:
            raise BuildToolError(
                f'Module "{request.create_name}" specifies dependency "{dependency}" more than once.'
            )
        seen_dependencies.add(dependency.casefold())
        if dependency.casefold() == request.create_name.casefold():
            raise BuildToolError(f'Module "{request.create_name}" cannot depend on itself.')
        if workspace.find_module(dependency) is None:
            raise BuildToolError(
                f'Module "{request.create_name}" depends on missing module "{dependency}".'
            )
    enablements = request.enablements
    if enablements is None:
        enablements = (
            ("base",)
            if request.module_kind is ModuleKind.RUNTIME
            else ("DurinEditor",)
        )
    folded_enablements = [enablement.casefold() for enablement in enablements]
    if len(set(folded_enablements)) != len(folded_enablements):
        raise BuildToolError("Module enablement targets must not be repeated.")
    if "none" in folded_enablements and len(enablements) != 1:
        raise BuildToolError('Module enablement "none" cannot be combined with other targets.')
    profiles = {name.casefold() for name in workspace.profile_names}
    for enablement in enablements:
        if enablement.casefold() in {"none", "base"}:
            continue
        if enablement.casefold() not in profiles:
            raise BuildToolError(f'Enablement profile "{enablement}" does not exist in the workspace.')
