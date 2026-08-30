from __future__ import annotations

import json
import re
from pathlib import Path
from typing import Sequence

from .errors import BuildToolError
from .models import CreateKind, LinkType, ModuleKind
from .requests import ConcreteRequest
from .descriptors import (
    ProjectDescriptor,
    WorkspaceDescriptors,
    load_workspace_descriptors,
    validate_create_request,
)
from .scaffolding_templates import TemplateRenderer
from .scaffolding_transaction import (
    PlanOperation,
    PlanOperationKind,
    ScaffoldPlan,
    execute_plan,
    ordered_plan,
)
from .scaffolding_workspace import (
    ADD_SUBDIRECTORY_PATTERN,
    WorkspaceDiscovery,
    discover_workspace_projects,
    parse_cmake_values,
    require_available_cmake_target,
    require_contained_path,
    resolved_within,
    validate_destination,
)


ROOT_ADD_SUBDIRECTORY_LINE_PATTERN = re.compile(
    r"(?im)^[ \t]*add_subdirectory[ \t]*\([^\r\n]*\)[ \t]*(?:#[^\r\n]*)?(?:\r?\n|$)"
)


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
    request: ConcreteRequest,
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
    request: ConcreteRequest,
    root: Path,
    *,
    renderer: TemplateRenderer | None = None,
    schema_directory: Path | None = None,
) -> ScaffoldPlan:
    if request.create_kind is not CreateKind.MODULE:
        raise BuildToolError("Module scaffolding requires a create module request.")
    discovery = discover_workspace_projects(root)
    project_paths = tuple(project.descriptor.path for project in discovery.projects)
    workspace = load_workspace_descriptors(
        discovery.root,
        project_paths=project_paths,
        schema_directory=schema_directory,
    )
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
        kind_directory = {
            ModuleKind.RUNTIME: "Runtime",
            ModuleKind.EDITOR: "Editor",
            ModuleKind.DEVELOPER: "Developer",
        }[request.module_kind]
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
            or resolved_within(module_directory, existing_root)
            or resolved_within(existing_root, module_directory)
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
        load_workspace_descriptors(
            discovery.root,
            project_paths=project_paths,
            schema_directory=schema_directory,
        )

    missing_directories: list[Path] = []
    candidate = module_directory
    while not candidate.exists():
        missing_directories.append(candidate)
        candidate = candidate.parent
    if not resolved_within(candidate, workspace_project.root):
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
        values = parse_cmake_values(ADD_SUBDIRECTORY_PATTERN, match.group(0))
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
    request: ConcreteRequest,
    root: Path,
    *,
    renderer: TemplateRenderer | None = None,
    schema_directory: Path | None = None,
) -> ScaffoldPlan:
    if request.create_kind is not CreateKind.PROJECT:
        raise BuildToolError("Project scaffolding requires a create project request.")
    if request.destination_path is None:
        raise BuildToolError("create project requires --path <path>.")

    discovery = discover_workspace_projects(root)
    project_paths = tuple(project.descriptor.path for project in discovery.projects)
    workspace = load_workspace_descriptors(
        discovery.root,
        project_paths=project_paths,
        schema_directory=schema_directory,
    )
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
    root_registrations = parse_cmake_values(
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
            schema_directory=schema_directory,
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
