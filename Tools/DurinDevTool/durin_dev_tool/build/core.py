"""Build request validation and action orchestration."""

from __future__ import annotations

import os
from datetime import datetime, timezone
from pathlib import Path
from time import perf_counter
from typing import Any, Callable, Sequence

from ..context import RepositoryContext

from .config import (
    Action,
    BuildPaths,
    BuildContext,
    BuildToolError,
    ConcreteRequest,
    ConfigurePreset,
    EnvironmentProvider,
    TestGranularity,
    TestMode,
    default_build_paths,
    preset_build_directory,
    preset_cache_bool,
    preset_cache_string,
)
from .output import BuildOutput
from .locking import (
    BuildToolLock,
    lock_file_path,
)
from .process import (
    run_command,
)
from .purge import (
    execute_purge,
)
from .recovery import (
    execute_with_recovery_marker,
    interruption_marker_path,
)
from .runtime import (
    run_all_native_tests,
    run_application,
    run_exact_native_test,
    run_selected_native_tests,
)
from .native_test_registry import (
    is_test_set_selection,
    load_native_test_registry,
    resolve_selection,
)
from .toolchain_context import prepare_toolchain_context

ALL_NATIVE_TESTS_TARGET = "DurinNativeTests"


def validate_target(target: str, *, action: Action) -> None:
    if not target:
        raise BuildToolError(f"{action.value} requires --target <target-name>.")
    allowed = set("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.+-")
    if any(character not in allowed for character in target):
        raise BuildToolError(f'Build target contains unsupported characters: "{target}"')


def normalize_run_request(
    request: ConcreteRequest,
    *,
    preset: ConfigurePreset | None = None,
    root: Path | None = None,
    default_project: Path | None = None,
) -> ConcreteRequest:
    paths = default_build_paths() if root is None or default_project is None else None
    root = root or paths.root
    if request.action is not Action.RUN:
        return request

    project_selectors = [
        argument
        for argument in request.run_arguments
        if argument == "--project" or argument.startswith("--project=")
    ]
    project_path = request.project_path
    if (
        project_path is None
        and not project_selectors
        and preset is not None
        and preset_cache_string(preset, "DURIN_RUNTIME_VARIANT") == "DurinGame"
    ):
        project_path = default_project or RepositoryContext.load().config.paths.default_game_project
    if project_path is None:
        return request

    if not project_path.is_absolute():
        project_path = root / project_path
    project_path = project_path.resolve()
    if project_path.suffix.casefold() != ".dproject":
        raise BuildToolError(
            f'Project descriptor must use the .dproject extension: "{project_path}".'
        )
    if not project_path.is_file():
        raise BuildToolError(f'Project descriptor was not found: "{project_path}".')

    if project_selectors:
        raise BuildToolError(
            "run accepts project selection either through --project or through "
            "--args, but not both."
        )
    return request.with_project_path(project_path)


def validate_request(request: ConcreteRequest, preset: ConfigurePreset) -> None:
    if request.action is Action.BUILD:
        validate_target(request.target, action=request.action)
    if request.action is Action.TEST:
        if request.test_operation == "explain" and not request.target:
            raise BuildToolError("test explain requires a target or @set selection.")
        if request.test_operation == "run" and not request.target:
            raise BuildToolError(
                "test requires a target, @set selector, or all.",
                recovery="Run .\\DevTool.bat test list to inspect configured choices.",
            )
        if request.test_operation not in {"run", "list", "explain"}:
            raise BuildToolError(f'Unknown test operation "{request.test_operation}".')
        if request.test_operation != "run" and request.test_mode is not TestMode.ROUTINE:
            raise BuildToolError("test list and test explain do not accept --mode.")
    if request.action is Action.REBUILD and request.target:
        validate_target(request.target, action=request.action)
    if (
        request.action is Action.TEST
        and request.test_operation == "run"
        and request.target.casefold() == "all"
        and request.test_filter
    ):
        raise BuildToolError(
            "--filter requires a single native test target and cannot be used with "
            "--target all."
        )
    if (
        request.action is Action.TEST
        and request.test_operation == "run"
        and request.target.casefold() != "all"
        and (
            request.test_ctest_regex
            or request.test_granularity_explicit
            or (
                request.test_schedule_random
                and request.test_mode is TestMode.ROUTINE
            )
            or (
                request.test_output_junit is not None
                and request.test_mode is TestMode.ROUTINE
            )
        )
    ):
        raise BuildToolError(
            "--schedule-random, --output-junit, --ctest-regex, "
            "and --granularity require --target all."
        )
    if (
        request.action is Action.TEST
        and request.target.casefold() == "all"
        and request.test_ctest_regex
        and request.test_granularity is not TestGranularity.CASE
    ):
        raise BuildToolError(
            "--ctest-regex requires --granularity case because a case-name regex "
            "is ambiguous for batched target processes."
        )
    if request.action is Action.TEST and request.test_operation == "run":
        if request.test_mode is TestMode.ISOLATION:
            if not request.test_filter or request.target.casefold() == "all":
                raise BuildToolError(
                    "isolation mode requires a bounded selection and one case filter.",
                    recovery="Run test <target-or-@set> <suite.case> --mode isolation.",
                )
        elif request.test_filter and is_test_set_selection(request.target):
            raise BuildToolError(
                "A case filter on a set requires --mode isolation.",
                recovery=f"Run test {request.target} {request.test_filter} --mode isolation.",
            )
        if request.test_report_path is not None and request.test_mode is not TestMode.REPORT:
            raise BuildToolError("--report requires --mode report.")
        if request.test_mode in {TestMode.CHARACTERIZATION, TestMode.QUALIFICATION} and request.target.casefold() == "all":
            raise BuildToolError(
                f"{request.test_mode.value} mode requires an explicit target or @set."
            )
    if request.action is Action.TEST and not preset_cache_bool(preset, "BUILD_TESTING"):
        raise BuildToolError(f'Preset "{preset.name}" does not enable BUILD_TESTING.')


def create_context(
    request: ConcreteRequest,
    *,
    prepare_tools: bool = True,
    repository_context: RepositoryContext | None = None,
) -> BuildContext:
    from .build_context import create_build_context

    context = create_build_context(
        request,
        repository=repository_context or RepositoryContext.load(),
    )
    if prepare_tools:
        prepare_toolchain_context(context)
    return context


def derive_context(
    base: BuildContext,
    request: ConcreteRequest,
) -> BuildContext:
    from .build_context import derive_build_context

    return derive_build_context(base, request)


def operation_metadata(context: BuildContext, *, target: str | None = None) -> dict[str, Any]:
    return {
        "pid": os.getpid(),
        "profile": context.profile.name,
        "preset": context.preset.name,
        "action": context.request.action.value,
        "target": context.target if target is None else target,
        "startedAt": datetime.now(timezone.utc).isoformat(),
    }


def cache_is_usable(cache_file: Path) -> bool:
    if not cache_file.is_file():
        return False
    try:
        content = cache_file.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return False
    return "CMAKE_MAKE_PROGRAM:FILEPATH=CMAKE_MAKE_PROGRAM-NOTFOUND" not in content


def ninja_uses_english_msvc_prefix(build_directory: Path) -> bool:
    rules_file = build_directory / "CMakeFiles" / "rules.ninja"
    try:
        content = rules_file.read_bytes().lower()
    except OSError:
        return False
    return b"msvc_deps_prefix = note: including file:" in content


def require_english_msvc_ninja_prefix(context: BuildContext, build_directory: Path) -> None:
    if (
        context.current_host == "windows"
        and context.profile.environment_provider is EnvironmentProvider.VISUAL_STUDIO
        and not ninja_uses_english_msvc_prefix(build_directory)
    ):
        raise BuildToolError(
            "CMake did not generate Ninja rules with the English MSVC /showIncludes prefix.",
            recovery="Confirm the Visual Studio English language pack is installed, then run configure again.",
        )


def cmake_build_target(context: BuildContext) -> str:
    if context.resolved_test_targets:
        return ";".join(context.resolved_test_targets)
    if context.request.action is Action.TEST and context.target == "all":
        return ALL_NATIVE_TESTS_TARGET
    return context.target


def perform_action(
    context: BuildContext,
    output: BuildOutput,
    *,
    target_override: str | None = None,
) -> None:
    request = context.request
    paths = BuildPaths.from_repository(context.repository) if context.repository else default_build_paths()
    environment = context.environment or os.environ
    build_directory = preset_build_directory(context.preset, root=paths.root)
    cache_file = build_directory / "CMakeCache.txt"

    if request.action is Action.CONFIGURE:
        fresh = request.fresh or (cache_file.exists() and not cache_is_usable(cache_file))
        command = [context.cmake]
        if fresh:
            command.append("--fresh")
        command.extend(["--preset", context.preset.name])
        command.extend(f"-D{definition}" for definition in request.defines)
        with output.stage("Configure"):
            run_command(
                command,
                environment=environment,
                output=output,
                show_heartbeat=request.agent,
                cwd=paths.root,
                state_directory=paths.state_directory,
            )
        require_english_msvc_ninja_prefix(context, build_directory)
        return
    if request.action is Action.CLEAN:
        if not cache_is_usable(cache_file):
            output.warning(f'Build tree is already clean or unconfigured: "{build_directory}"')
            return
        with output.stage("Clean"):
            run_command(
                [context.cmake, "--build", str(build_directory), "--target", "clean"],
                environment=environment,
                output=output,
                show_heartbeat=request.agent,
                cwd=paths.root,
                state_directory=paths.state_directory,
            )
        return

    target = target_override or cmake_build_target(context)
    if request.action is Action.REBUILD:
        if cache_is_usable(cache_file):
            with output.stage("Clean"):
                run_command(
                    [context.cmake, "--build", str(build_directory), "--target", "clean"],
                    environment=environment,
                    output=output,
                    show_heartbeat=request.agent,
                    cwd=paths.root,
                    state_directory=paths.state_directory,
                )
        else:
            output.warning(f'Skipping clean because the build tree is unconfigured: "{build_directory}"')
        with output.stage("Configure"):
            run_command(
                [context.cmake, "--fresh", "--preset", context.preset.name],
                environment=environment,
                output=output,
                show_heartbeat=request.agent,
                cwd=paths.root,
                state_directory=paths.state_directory,
            )
        require_english_msvc_ninja_prefix(context, build_directory)
    elif not cache_is_usable(cache_file) or (
        context.current_host == "windows"
        and context.profile.environment_provider is EnvironmentProvider.VISUAL_STUDIO
        and not ninja_uses_english_msvc_prefix(build_directory)
    ):
        if cache_is_usable(cache_file):
            output.warning("Existing Ninja rules do not use the English MSVC dependency prefix; reconfiguring.")
        command = [context.cmake]
        # Avoid a redundant --fresh for a new tree, but discard unusable or incompatible state.
        if cache_file.exists():
            command.append("--fresh")
        command.extend(["--preset", context.preset.name])
        with output.stage("Configure"):
            run_command(
                command,
                environment=environment,
                output=output,
                show_heartbeat=request.agent,
                cwd=paths.root,
                state_directory=paths.state_directory,
            )
        require_english_msvc_ninja_prefix(context, build_directory)

    with output.stage("Build"):
        targets = target.split(";")
        run_command(
            [context.cmake, "--build", str(build_directory), "--target", *targets, "-j", str(context.jobs)],
            environment=environment,
            output=output,
            show_heartbeat=request.agent,
            cwd=paths.root,
            state_directory=paths.state_directory,
        )


def execute_context(
    context: BuildContext,
    output: BuildOutput,
    *,
    confirm_purge: Callable[[Sequence[Path], bool], bool],
) -> float:
    started = perf_counter()
    output.context(context)
    request = context.request
    paths = BuildPaths.from_repository(context.repository) if context.repository else default_build_paths()
    if (
        request.action is Action.TEST
        and request.test_operation == "run"
        and request.target.casefold() != "all"
        and (is_test_set_selection(request.target) or request.test_mode is not TestMode.ROUTINE)
    ):
        registry = load_native_test_registry(context)
        resolved = resolve_selection(
            registry,
            request.target,
            admit_characterization=request.test_mode is TestMode.CHARACTERIZATION,
            admit_qualification=request.test_mode is TestMode.QUALIFICATION,
        )
        context.resolved_test_targets = resolved.names
        context.test_selection_explanation = resolved.explanation
        output.info(f"Selection: {request.target} ({resolved.explanation})")
        output.info(f"Resolved targets: {', '.join(resolved.names)}")
    marker_file = interruption_marker_path(context.preset.name, paths.state_directory)
    lock_metadata = operation_metadata(
        context,
        target=(
            "all-presets"
            if context.request.action is Action.PURGE and context.request.all_presets
            else "recorded-target"
            if context.request.action is Action.RECOVER
            else context.target
        ),
    )
    with BuildToolLock(
        lock_file_path(paths.lock_directory),
        lock_metadata,
        cwd=paths.root,
    ):
        if context.request.action is Action.PURGE:
            execute_purge(context, output, confirm_purge)
        elif context.request.action is Action.RUN:
            run_application(context, output)
        else:
            metadata = operation_metadata(
                context,
                target=cmake_build_target(context),
            )
            execute_with_recovery_marker(
                action=context.request.action,
                marker_file=marker_file,
                metadata=metadata,
                operation=lambda target_override: perform_action(
                    context,
                    output,
                    target_override=target_override,
                ),
            )
            # Native tests only read completed build outputs. Their assertion failures,
            # hangs, and interruptions must not poison incremental build state.
            if context.request.action is Action.TEST:
                if context.target == "all":
                    run_all_native_tests(context, output)
                elif context.resolved_test_targets:
                    run_selected_native_tests(context, output)
                else:
                    run_exact_native_test(context, output)
    elapsed = perf_counter() - started
    if context.request.action is not Action.PURGE:
        output.success(f"{context.request.action.value} completed in {elapsed:.2f}s.")
    return elapsed
