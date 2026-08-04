from __future__ import annotations

import os
from dataclasses import dataclass
from pathlib import Path
from time import perf_counter
from typing import Any, Sequence

from rich.markup import escape
from rich.table import Table
from rich.text import Text

from .config import (
    Action,
    BuildContext,
    BuildToolError,
    CMAKE_ENV_VARS,
    CommandRequest,
    JOBS_ENV_VAR,
    REPO_ROOT,
    preset_cache_string,
)
from .locations import resolve_all_locations, resolve_location
from .opener import open_location
from .core import (
    create_context,
    derive_context,
    execute_context,
    prepare_command_context,
    prepare_toolchain_environment,
)
from .locking import stop_active_operation
from .output import BuildOutput
from .recovery import interruption_marker_path, recoverable_target, recovery_target

TOOLCHAIN_ACTIONS = {
    Action.CONFIGURE,
    Action.BUILD,
    Action.CLEAN,
    Action.RECOVER,
    Action.REBUILD,
    Action.TEST,
}
CREATE_ACTIONS = {Action.CREATE_MODULE, Action.CREATE_PROJECT}


@dataclass(frozen=True)
class AcquiredRequest:
    request: CommandRequest
    context: BuildContext | None
    current_preset: str = ""
    preset_selected: bool = False


def execute_create_request(
    request: CommandRequest,
    output: BuildOutput,
    *,
    root: Path = REPO_ROOT,
) -> None:
    from .scaffolding import execute_plan, plan_module_creation, plan_project_creation

    plan = (
        plan_project_creation(request, root)
        if request.action is Action.CREATE_PROJECT
        else plan_module_creation(request, root)
    )
    if request.dry_run:
        output.info(plan.format(plain=output.plain))
        return
    execute_plan(plan)
    if request.action is Action.CREATE_PROJECT:
        output.success(
            f'Created project "{request.create_name}" at '
            f'"{request.destination_path}".'
        )
    else:
        output.success(
            f'Created module "{request.create_name}" in '
            f'"{request.project_path}".'
        )

def confirm_purge(output: BuildOutput, paths: Sequence[Path], all_presets: bool) -> bool:
    scope = "all registered presets" if all_presets else "the current preset"
    output.warning(f"Purge will permanently remove build artifacts for {scope}:")
    for path in paths:
        try:
            display = path.relative_to(Path.cwd())
        except ValueError:
            display = path
        output.info(f"  {display}")
    expected = "PURGE ALL" if all_presets else "PURGE"
    try:
        return input(f'Type "{expected}" to continue: ').strip() == expected
    except (EOFError, KeyboardInterrupt):
        output.info("")
        return False


def print_shell_help(output: BuildOutput) -> None:
    output.info(escape(shell_command_help()))
def show_presets(output: BuildOutput, context: BuildContext, current_preset: str) -> None:
    if output.plain:
        for index, preset in enumerate(context.profile.presets, start=1):
            markers = []
            if preset == context.profile.default_preset:
                markers.append("default")
            if preset == current_preset:
                markers.append("current")
            suffix = f' [{", ".join(markers)}]' if markers else ""
            output.info(escape(f"  {index:>2}  {preset}{suffix}"))
        return
    table = Table(
        title="Registered presets",
        title_style="bold cyan",
        header_style="bold white",
        border_style="bright_black",
    )
    table.add_column("#", justify="right", style="cyan")
    table.add_column("Preset")
    table.add_column("State")
    for index, preset in enumerate(context.profile.presets, start=1):
        markers = Text()
        if preset == context.profile.default_preset:
            markers.append("default", style="cyan")
        if preset == current_preset:
            if markers:
                markers.append(", ")
            markers.append("current", style="bold green")
        preset_text = Text(
            preset,
            style="bold green" if preset == current_preset else "",
        )
        table.add_row(str(index), preset_text, markers)
    output.console.print(table)


def resolve_shell_preset(value: str, context: BuildContext) -> str:
    matches = [preset for preset in context.profile.presets if preset.lower() == value.lower()]
    if len(matches) == 1:
        return matches[0]
    raise BuildToolError(f'Unknown preset "{value}". Use its full name or run presets.')


def resolve_shell_preset_number(value: str, context: BuildContext) -> str:
    if value.isdigit() and 1 <= int(value) <= len(context.profile.presets):
        return context.profile.presets[int(value) - 1]
    raise BuildToolError(f'Invalid preset number "{value}". Enter a number shown by presets.')


def resolve_preset_selector(value: str, context: BuildContext) -> str:
    return (
        resolve_shell_preset_number(value, context)
        if value.isdigit()
        else resolve_shell_preset(value, context)
    )


def show_status(output: BuildOutput, context: BuildContext) -> None:
    marker = interruption_marker_path(context.preset.name)
    recovery_required = marker.is_file()
    resumable_target = recoverable_target(marker) if recovery_required else None
    toolchain_resolved = context.environment is not None
    cmake_default = context.request.cmake or next(
        (os.environ[name].strip() for name in CMAKE_ENV_VARS if os.environ.get(name, "").strip()),
        "",
    )
    cmake_default = cmake_default or context.config.cmake_command or "cmake"
    if context.request.jobs is not None:
        jobs_default: object = context.request.jobs
    elif os.environ.get(JOBS_ENV_VAR, "").strip():
        jobs_default = os.environ[JOBS_ENV_VAR].strip()
    elif context.config.jobs:
        jobs_default = context.config.jobs
    else:
        jobs_default = "automatic"
    values = {
        "Profile": context.profile.name,
        "Preset": context.preset.name,
        "Runtime variant": preset_cache_string(
            context.preset,
            "DURIN_RUNTIME_VARIANT",
            required=False,
        )
        or "unspecified",
        "Build directory": resolve_location(
            "build",
            profile=context.profile,
            preset=context.preset,
        ).path,
        "Configuration": preset_cache_string(context.preset, "CMAKE_BUILD_TYPE"),
        "Preset role": preset_cache_string(
            context.preset,
            "DURIN_PRESET_ROLE",
            required=False,
        )
        or "Standard",
        "Tracy": (
            "enabled"
            if preset_cache_string(
                context.preset,
                "DURIN_ENABLE_TRACY",
                required=False,
            ).upper()
            in {"1", "ON", "TRUE", "YES"}
            else "disabled"
        ),
        "Toolchain context": "resolved" if toolchain_resolved else "unresolved",
        "Parallel jobs": context.jobs or f"unresolved (default: {jobs_default})",
        "CMake": context.cmake or f'unresolved (default: {cmake_default})',
        "Recovery state": (
            "recover required"
            if resumable_target
            else "rebuild required"
            if recovery_required
            else "clean"
        ),
    }
    if recovery_required:
        values["Recovery target"] = resumable_target or "unknown"
        values["Recovery command"] = (
            "recover" if resumable_target else f"rebuild --target {recovery_target(marker)}"
        )
    if output.plain:
        for label, value in values.items():
            output.info(f"{label}: {value}")
        return
    table = Table(title="DurinDevTool build status")
    table.add_column("Setting", style="bold cyan")
    table.add_column("Value")
    for label, value in values.items():
        table.add_row(label, str(value))
    output.console.print(table)


def show_locations(output: BuildOutput, context: BuildContext) -> None:
    locations = resolve_all_locations(
        profile=context.profile,
        preset=context.preset,
    )
    if output.plain:
        for location in locations:
            output.raw_line(f"{location.spec.name}\t{location.path}")
        return
    table = Table(title="DurinDevTool locations")
    table.add_column("Location", style="bold cyan")
    table.add_column("Path")
    for location in locations:
        table.add_row(location.spec.name, str(location.path))
    output.console.print(table)


def execute_location_request(
    request: CommandRequest,
    context: BuildContext,
    output: BuildOutput,
) -> None:
    if request.action is Action.PATH and request.all_locations:
        show_locations(output, context)
        return
    location = resolve_location(
        request.location,
        profile=context.profile,
        preset=context.preset,
    )
    if request.action is Action.PATH:
        output.raw_line(str(location.path))
        return
    open_location(location, current_host=context.current_host)
    output.success(
        f'Opened {location.spec.name} directory: "{location.path}"'
    )


def acquire_request_context(
    request: CommandRequest,
    *,
    session_state: dict[str, object] | None,
) -> AcquiredRequest:
    if request.action in {Action.SHELL, Action.STOP, *CREATE_ACTIONS}:
        return AcquiredRequest(request, None)
    if session_state is None:
        if request.action is Action.PRESET:
            base = create_context(
                request.with_preset("").with_action(Action.SHELL),
                prepare_tools=False,
            )
            selected = (
                resolve_preset_selector(request.preset, base)
                if request.preset
                else base.preset.name
            )
            return AcquiredRequest(
                request.with_preset(selected),
                base,
                selected,
                preset_selected=bool(request.preset),
            )
        context = create_context(
            request,
            prepare_tools=request.action in TOOLCHAIN_ACTIONS,
        )
        return AcquiredRequest(request, context, context.preset.name)

    base = session_state.get("build_context")
    if base is None:
        base_request = request.with_preset("") if request.action is Action.PRESET else request
        base = create_context(base_request.with_action(Action.SHELL), prepare_tools=False)
        session_state["build_context"] = base
        session_state["build_preset"] = base.preset.name
    current_preset = str(session_state["build_preset"])

    if request.action is Action.PRESET:
        if request.preset:
            current_preset = resolve_preset_selector(request.preset, base)
            session_state["build_preset"] = current_preset
        return AcquiredRequest(
            request.with_preset(current_preset),
            base,
            current_preset,
            preset_selected=bool(request.preset),
        )

    request = request.with_preset(request.preset or current_preset)
    session_profile = base.profile.name
    needs_independent_context = (
        request.profile not in {"", session_profile}
        or request.environment_setup != base.request.environment_setup
        or (
            request.action not in TOOLCHAIN_ACTIONS
            and request.cmake != base.request.cmake
        )
    )
    if needs_independent_context:
        context = create_context(
            request,
            prepare_tools=request.action in TOOLCHAIN_ACTIONS,
        )
    else:
        context = derive_context(base, request)
        if request.action in TOOLCHAIN_ACTIONS and base.environment is None:
            prepare_toolchain_environment(base)
            context = derive_context(base, request)
        needs_command_preparation = (
            not context.cmake
            or not context.jobs
            or request.cmake != base.request.cmake
            or request.jobs != base.request.jobs
        )
        if request.action in TOOLCHAIN_ACTIONS and needs_command_preparation:
            prepare_command_context(context)
            if request.cmake == base.request.cmake:
                base.cmake = context.cmake
            if request.jobs == base.request.jobs:
                base.jobs = context.jobs
    return AcquiredRequest(request, context, current_preset)


def dispatch_request(
    acquired: AcquiredRequest,
    output: BuildOutput,
) -> None:
    request = acquired.request
    context = acquired.context
    if request.action is Action.STOP:
        if stop_active_operation():
            output.success("Stopped the active DurinDevTool build operation.")
        else:
            output.info("No active DurinDevTool build operation was found.")
        return
    if request.action in CREATE_ACTIONS:
        execute_create_request(request, output)
        return
    if context is None:
        raise BuildToolError(
            f"{request.action.value} requires an acquired build context."
        )
    if request.action is Action.PRESETS:
        show_presets(output, context, acquired.current_preset)
        return
    if request.action is Action.PRESET:
        label = "CMake preset selected" if acquired.preset_selected else "CMake preset"
        output.info(f'{label}: "{acquired.current_preset}"')
        return
    if request.action is Action.STATUS:
        show_status(output, context)
        return
    if request.action in {Action.PATH, Action.OPEN}:
        execute_location_request(request, context, output)
        return
    execute_context(
        context,
        output,
        confirm_purge=lambda paths, all_presets: confirm_purge(
            output,
            paths,
            all_presets,
        ),
    )


def execute_request(
    request: CommandRequest,
    *,
    stdout: Any = None,
    stderr: Any = None,
    session_state: dict[str, object] | None = None,
) -> int:
    started = perf_counter()
    output = BuildOutput(
        plain=request.plain,
        output_mode=request.output_mode,
        stdout=stdout,
        stderr=stderr,
    )
    context: BuildContext | None = None
    try:
        acquired = acquire_request_context(
            request,
            session_state=session_state,
        )
        context = acquired.context
        dispatch_request(acquired, output)
        return 0
    except BuildToolError as exc:
        output.failure(
            exc,
            context,
            perf_counter() - started,
            request=request,
            preset=(
                str(session_state.get("build_preset", ""))
                if session_state is not None
                else ""
            ),
        )
        return 1
    except OSError as exc:
        output.failure(
            BuildToolError(f"Operating system error: {exc}"),
            context,
            perf_counter() - started,
            request=request,
            preset=(
                str(session_state.get("build_preset", ""))
                if session_state is not None
                else ""
            ),
        )
        return 1
