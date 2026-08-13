from __future__ import annotations

import re
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Mapping, Sequence

from ..context import RepositoryContext
from ..json_contract import JsonContractError, load_json_contract
from .config import BuildToolError, ConcreteRequest, CreateKind, ModuleKind


DEPENDENCY_FIELDS = (
    "PrivateDependencies",
    "PublicDependencies",
    "OptionalPrivateDependencies",
    "OptionalPublicDependencies",
)
CPP_IDENTIFIER_PATTERN = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")
PROJECT_SCHEMA = "durin-project.schema.json"
MODULE_SCHEMA = "durin-module.schema.json"


@dataclass(frozen=True)
class ProjectRuntimeVariantDescriptor:
    modules: tuple[str, ...] = ()


@dataclass(frozen=True)
class ProjectDescriptor:
    name: str
    path: Path
    module_dirs: Mapping[str, str]
    base_modules: tuple[str, ...]
    extra_modules: Mapping[str, ProjectRuntimeVariantDescriptor]


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
    def runtime_variants(self) -> tuple[str, ...]:
        return tuple(
            sorted(
                {
                    runtime_variant
                    for project in self.projects
                    for runtime_variant in project.extra_modules
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


def _load_json_object(
    path: Path,
    label: str,
    schema_file_name: str,
    schema_directory: Path,
) -> dict[str, Any]:
    try:
        value = load_json_contract(
            path,
            label=label,
            schema_path=schema_directory / schema_file_name,
        )
    except JsonContractError as exc:
        raise BuildToolError(str(exc)) from exc
    assert isinstance(value, dict)
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


def descriptor_schema_directory(repository: RepositoryContext) -> Path:
    return (
        repository.root
        / "Engine"
        / "Source"
        / "Programs"
        / "DurinHeaderTool"
        / "schemas"
    )


def load_project_descriptor(
    path: Path,
    *,
    schema_directory: Path | None = None,
) -> ProjectDescriptor:
    resolved = path.resolve()
    schemas = schema_directory or descriptor_schema_directory(RepositoryContext.load())
    data = _load_json_object(
        resolved, "Project descriptor", PROJECT_SCHEMA, schemas.resolve()
    )
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

    raw_runtime_variants = data.get("ExtraModules", {})
    if not isinstance(raw_runtime_variants, dict):
        raise BuildToolError(f'Descriptor "{resolved}" field "ExtraModules" must be an object.')
    runtime_variants: dict[str, ProjectRuntimeVariantDescriptor] = {}
    folded_runtime_variants: set[str] = set()
    for runtime_variant, runtime_variant_data in raw_runtime_variants.items():
        if not isinstance(runtime_variant, str) or not runtime_variant:
            raise BuildToolError(f'Descriptor "{resolved}" contains an invalid runtime variant name.')
        if runtime_variant.casefold() in folded_runtime_variants:
            raise BuildToolError(
                f'Descriptor "{resolved}" contains duplicate runtime variant "{runtime_variant}".'
            )
        folded_runtime_variants.add(runtime_variant.casefold())
        if not isinstance(runtime_variant_data, dict):
            raise BuildToolError(
                f'Descriptor "{resolved}" runtime variant "{runtime_variant}" must contain an object.'
            )
        runtime_variants[runtime_variant] = ProjectRuntimeVariantDescriptor(
            _string_list(runtime_variant_data, "Modules", resolved)
        )
    return ProjectDescriptor(
        name=name,
        path=resolved,
        module_dirs=dict(raw_module_dirs),
        base_modules=base_modules,
        extra_modules=runtime_variants,
    )


def load_module_descriptor(
    path: Path,
    owning_project: str,
    *,
    schema_directory: Path | None = None,
) -> ModuleDescriptor:
    resolved = path.resolve()
    schemas = schema_directory or descriptor_schema_directory(RepositoryContext.load())
    data = _load_json_object(
        resolved, "Module descriptor", MODULE_SCHEMA, schemas.resolve()
    )
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
    schema_directory: Path | None = None,
) -> WorkspaceDescriptors:
    schemas = schema_directory or descriptor_schema_directory(RepositoryContext.load())
    paths = (
        sorted((path.resolve() for path in project_paths), key=lambda path: str(path).casefold())
        if project_paths is not None
        else sorted(root.resolve().glob("*/*.dproject"), key=lambda path: str(path).casefold())
    )
    projects = tuple(
        load_project_descriptor(path, schema_directory=schemas) for path in paths
    )
    _reject_duplicate(((project.name, project.path) for project in projects), label="project")

    modules: list[ModuleDescriptor] = []
    for project in projects:
        for registered_name, relative_directory in project.module_dirs.items():
            module_path = project.path.parent / relative_directory / f"{registered_name}.dmodule"
            module = load_module_descriptor(
                module_path,
                project.name,
                schema_directory=schemas,
            )
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
        for runtime_variant, roots in (
            ("BaseModules", project.base_modules),
            *(
                (name, runtime_variant.modules)
                for name, runtime_variant in project.extra_modules.items()
            ),
        ):
            for module_name in roots:
                if module_name.casefold() not in owned:
                    raise BuildToolError(
                        f'Project "{project.name}" enables missing module "{module_name}" '
                        f'in runtime variant "{runtime_variant}".'
                    )
    for module in workspace.modules:
        for dependency in module.dependencies:
            if dependency.casefold() == module.name.casefold():
                raise BuildToolError(f'Module "{module.name}" cannot depend on itself.')
            if dependency.casefold() not in module_names:
                raise BuildToolError(
                    f'Module "{module.name}" depends on missing module "{dependency}".'
                )


def validate_create_request(request: ConcreteRequest, workspace: WorkspaceDescriptors) -> None:
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
    runtime_variants = {name.casefold() for name in workspace.runtime_variants}
    for enablement in enablements:
        if enablement.casefold() in {"none", "base"}:
            continue
        if enablement.casefold() not in runtime_variants:
            raise BuildToolError(
                f'Enablement runtime variant "{enablement}" does not exist in the workspace.'
            )
