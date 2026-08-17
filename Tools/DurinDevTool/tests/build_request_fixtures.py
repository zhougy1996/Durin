from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path

from durin_dev_tool.build.models import Action, CreateKind, LinkType, ModuleKind, TestGranularity, TestMode
from durin_dev_tool.build.requests import (
    BuildRequest,
    ConfigureRequest,
    LocationRequest,
    ModuleCreationRequest,
    NativeTestRequest,
    OutputOptions,
    ProjectCreationRequest,
    PurgeRequest,
    RebuildRequest,
    RequestContext,
    RunRequest,
    SimpleRequest,
)


@dataclass(frozen=True)
class BuildActionOptions:
    target: str = ""
    fresh: bool = False
    defines: tuple[str, ...] = ()


@dataclass(frozen=True)
class TestActionOptions:
    target: str = ""
    filter: str = ""
    operation: str = "run"
    query: str = ""
    mode: TestMode = TestMode.ROUTINE
    report_path: Path | None = None
    timeout_seconds: int = 300
    schedule_random: bool = False
    output_junit: Path | None = None
    ctest_regex: str = ""
    granularity: TestGranularity | None = None


@dataclass(frozen=True)
class RunActionOptions:
    project_path: Path | None = None
    arguments: tuple[str, ...] = ()


@dataclass(frozen=True)
class PurgeActionOptions:
    all_presets: bool = False
    yes: bool = False


@dataclass(frozen=True)
class LocationActionOptions:
    location: str = ""
    all_locations: bool = False


@dataclass(frozen=True)
class CreateActionOptions:
    kind: CreateKind
    name: str = ""
    project_path: Path | None = None
    destination_path: Path | None = None
    module_kind: ModuleKind = ModuleKind.RUNTIME
    link_type: LinkType = LinkType.SHARED
    pch: str = ""
    public_dependencies: tuple[str, ...] = ()
    private_dependencies: tuple[str, ...] = ()
    optional_public_dependencies: tuple[str, ...] = ()
    optional_private_dependencies: tuple[str, ...] = ()
    enablements: tuple[str, ...] | None = None
    dry_run: bool = False


def command_request(
    action: Action,
    context: RequestContext = RequestContext(),
    output: OutputOptions = OutputOptions(),
    options: object | None = None,
):
    if action is Action.CONFIGURE:
        value = options if isinstance(options, BuildActionOptions) else BuildActionOptions()
        return ConfigureRequest(context, output, value.fresh, value.defines)
    if action is Action.BUILD:
        value = options if isinstance(options, BuildActionOptions) else BuildActionOptions()
        return BuildRequest(context, output, value.target)
    if action is Action.REBUILD:
        value = options if isinstance(options, BuildActionOptions) else BuildActionOptions()
        return RebuildRequest(context, output, value.target)
    if action is Action.TEST:
        value = options if isinstance(options, TestActionOptions) else TestActionOptions()
        return NativeTestRequest(
            context, output, value.target, value.filter, value.operation, value.query,
            value.mode, value.report_path, value.timeout_seconds, value.schedule_random,
            value.output_junit, value.ctest_regex, value.granularity,
        )
    if action is Action.RUN:
        value = options if isinstance(options, RunActionOptions) else RunActionOptions()
        return RunRequest(context, output, value.project_path, value.arguments)
    if action is Action.PURGE:
        value = options if isinstance(options, PurgeActionOptions) else PurgeActionOptions()
        return PurgeRequest(context, output, value.all_presets, value.yes)
    if action in {Action.PATH, Action.OPEN}:
        value = options if isinstance(options, LocationActionOptions) else LocationActionOptions()
        return LocationRequest(context, output, action, value.location, value.all_locations)
    if action is Action.CREATE_MODULE:
        assert isinstance(options, CreateActionOptions)
        return ModuleCreationRequest(
            context, output, options.name, options.project_path, options.destination_path,
            options.module_kind, options.link_type, options.pch, options.public_dependencies,
            options.private_dependencies, options.optional_public_dependencies,
            options.optional_private_dependencies, options.enablements, options.dry_run,
        )
    if action is Action.CREATE_PROJECT:
        assert isinstance(options, CreateActionOptions)
        return ProjectCreationRequest(context, output, options.name, options.destination_path, options.dry_run)
    if options is not None:
        raise ValueError(f"{action.value} does not accept options")
    return SimpleRequest(context, output, action)
