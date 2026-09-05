"""Build request validation and action orchestration."""

from __future__ import annotations

import os
from datetime import datetime, timezone
from pathlib import Path
from time import perf_counter
from typing import Any, Callable, Sequence

from ..context import RepositoryContext

from .build_context import BuildContext, create_build_context, derive_build_context
from .errors import BuildToolError
from .models import Action, EnvironmentProvider, TestMode
from .requests import ConcreteRequest
from .selection import preset_build_directory
from .settings import BuildPaths, default_build_paths
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
from .dependencies import prepare_configure_dependencies

ALL_NATIVE_TESTS_TARGET = "DurinNativeTests"


def create_context(
    request: ConcreteRequest,
    *,
    prepare_tools: bool = True,
    repository_context: RepositoryContext | None = None,
) -> BuildContext:
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
    if "CMAKE_MAKE_PROGRAM:FILEPATH=CMAKE_MAKE_PROGRAM-NOTFOUND" in content:
        return False
    # Every registered Durin preset uses Ninja. CMake writes its cache before
    # generation completes, so a failed configure can leave a plausible cache
    # without the build graph that cmake --build requires.
    return (cache_file.parent / "build.ninja").is_file()


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


def configure_build_tree(
    context: BuildContext,
    output: BuildOutput,
    *,
    build_directory: Path,
    paths: BuildPaths,
    fresh: bool,
    definitions: Sequence[str] = (),
) -> None:
    command = [context.cmake]
    if fresh:
        command.append("--fresh")
    command.extend(["--preset", context.preset.name])
    command.extend(f"-D{definition}" for definition in definitions)
    with output.stage("Dependencies"):
        prepare_configure_dependencies(context, output)
    with output.stage("Configure"):
        run_command(
            command,
            environment=context.environment or os.environ,
            output=output,
            show_heartbeat=context.request.agent,
            cwd=paths.root,
            state_directory=paths.state_directory,
        )
    require_english_msvc_ninja_prefix(context, build_directory)


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
        configure_build_tree(
            context,
            output,
            build_directory=build_directory,
            paths=paths,
            fresh=fresh,
            definitions=request.defines,
        )
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
        configure_build_tree(
            context,
            output,
            build_directory=build_directory,
            paths=paths,
            fresh=True,
        )
    elif not cache_is_usable(cache_file) or (
        context.current_host == "windows"
        and context.profile.environment_provider is EnvironmentProvider.VISUAL_STUDIO
        and not ninja_uses_english_msvc_prefix(build_directory)
    ):
        if cache_is_usable(cache_file):
            output.warning("Existing Ninja rules do not use the English MSVC dependency prefix; reconfiguring.")
        configure_build_tree(
            context,
            output,
            build_directory=build_directory,
            paths=paths,
            # Avoid a redundant --fresh for a new tree, but discard unusable or
            # incompatible state.
            fresh=cache_file.exists(),
        )

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
