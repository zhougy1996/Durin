from __future__ import annotations

from dataclasses import dataclass, replace
from pathlib import Path
from typing import TypeAlias

from .errors import BuildToolError
from .models import Action, CreateKind, LinkType, ModuleKind, OutputMode, TestGranularity, TestMode

@dataclass(frozen=True)
class RequestContext:
    profile: str = ""
    preset: str = ""
    cmake: str = ""
    environment_setup: str = ""
    jobs: int | None = None


@dataclass(frozen=True)
class OutputOptions:
    plain: bool = False
    mode: OutputMode = OutputMode.AUTO
    agent: bool = False


@dataclass(frozen=True)
class BaseRequest:
    context: RequestContext = RequestContext()
    output: OutputOptions = OutputOptions()

    @property
    def profile(self) -> str:
        return self.context.profile

    @property
    def preset(self) -> str:
        return self.context.preset

    @property
    def cmake(self) -> str:
        return self.context.cmake

    @property
    def environment_setup(self) -> str:
        return self.context.environment_setup

    @property
    def jobs(self) -> int | None:
        return self.context.jobs

    @property
    def plain(self) -> bool:
        return self.output.plain

    @property
    def output_mode(self) -> OutputMode:
        return self.output.mode

    @property
    def agent(self) -> bool:
        return self.output.agent

    def with_preset(self, preset: str) -> "BaseRequest":
        return replace(self, context=replace(self.context, preset=preset))

    @property
    def requires_toolchain(self) -> bool:
        return False


@dataclass(frozen=True)
class SimpleRequest(BaseRequest):
    action: Action = Action.SHELL

    @property
    def requires_toolchain(self) -> bool:
        return self.action in {Action.CLEAN, Action.RECOVER}


@dataclass(frozen=True)
class ConfigureRequest(BaseRequest):
    fresh: bool = False
    action: Action = Action.CONFIGURE

    @property
    def requires_toolchain(self) -> bool:
        return True


@dataclass(frozen=True)
class BuildRequest(BaseRequest):
    target: str = "all"
    action: Action = Action.BUILD

    @property
    def requires_toolchain(self) -> bool:
        return True


@dataclass(frozen=True)
class RebuildRequest(BaseRequest):
    target: str = "all"
    action: Action = Action.REBUILD

    @property
    def requires_toolchain(self) -> bool:
        return True


@dataclass(frozen=True)
class NativeTestRequest(BaseRequest):
    target: str = ""
    test_filter: str = ""
    test_operation: str = "run"
    test_query: str = ""
    test_mode: TestMode = TestMode.ROUTINE
    test_report_path: Path | None = None
    test_timeout_seconds: int = 300
    test_schedule_random: bool = False
    test_output_junit: Path | None = None
    test_ctest_regex: str = ""
    test_granularity_value: TestGranularity | None = None
    action: Action = Action.TEST

    @property
    def test_granularity(self) -> TestGranularity:
        return self.test_granularity_value or TestGranularity.TARGET

    @property
    def test_granularity_explicit(self) -> bool:
        return self.test_granularity_value is not None

    @property
    def requires_toolchain(self) -> bool:
        return self.test_operation == "run"


@dataclass(frozen=True)
class RunRequest(BaseRequest):
    project_path: Path | None = None
    run_arguments: tuple[str, ...] = ()
    action: Action = Action.RUN

    def with_project_path(self, project_path: Path) -> "RunRequest":
        return replace(self, project_path=project_path)


@dataclass(frozen=True)
class PurgeRequest(BaseRequest):
    all_presets: bool = False
    yes: bool = False
    action: Action = Action.PURGE


@dataclass(frozen=True)
class LocationRequest(BaseRequest):
    action: Action = Action.PATH
    location: str = ""
    all_locations: bool = False

    def __post_init__(self) -> None:
        if self.action is Action.PATH:
            if bool(self.location) == self.all_locations:
                raise BuildToolError("path requires either one location or --all.")
        elif self.action is Action.OPEN:
            if not self.location:
                raise BuildToolError("open requires one location.")
        else:
            raise BuildToolError("location requests require path or open action.")


@dataclass(frozen=True)
class ModuleCreationRequest(BaseRequest):
    create_name: str = ""
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
    action: Action = Action.CREATE_MODULE
    create_kind: CreateKind = CreateKind.MODULE


@dataclass(frozen=True)
class ProjectCreationRequest(BaseRequest):
    create_name: str = ""
    destination_path: Path | None = None
    dry_run: bool = False
    action: Action = Action.CREATE_PROJECT
    create_kind: CreateKind = CreateKind.PROJECT


ConcreteRequest: TypeAlias = (
    SimpleRequest
    | ConfigureRequest
    | BuildRequest
    | RebuildRequest
    | NativeTestRequest
    | RunRequest
    | PurgeRequest
    | LocationRequest
    | ModuleCreationRequest
    | ProjectCreationRequest
)


AnyRequest: TypeAlias = ConcreteRequest


def request_target(request: AnyRequest) -> str:
    if isinstance(request, (BuildRequest, RebuildRequest, NativeTestRequest)):
        return request.target
    return ""
