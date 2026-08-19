"""Runtime launch and native-test execution services."""

from __future__ import annotations

import os
import secrets
import re
from dataclasses import dataclass
from pathlib import Path

from ..context import RepositoryContext
from .config import (
    BuildPaths,
    BuildContext,
    BuildProfile,
    BuildToolError,
    ConfigurePreset,
    TestMode,
    default_build_paths,
    preset_build_directory,
    preset_cache_string,
)
from .locations import resolve_location
from .native_test_registry import load_native_test_registry, resolve_selection
from .output import BuildOutput
from .process import run_command
from .crash import analyze_crash, discover_current_crash, format_crash_summary, format_windows_status


def _context_paths(context: BuildContext) -> BuildPaths:
    return (
        BuildPaths.from_repository(context.repository)
        if context.repository
        else default_build_paths()
    )


def runtime_executable_path(
    profile: BuildProfile,
    preset: ConfigurePreset,
    *,
    root: Path | None = None,
    repository: RepositoryContext | None = None,
) -> Path:
    repository = repository or RepositoryContext.load()
    root = root or repository.root
    runtime_variant = preset_cache_string(preset, "DURIN_RUNTIME_VARIANT")
    directory = resolve_location(
        "runtime",
        profile=profile,
        preset=preset,
        root=root,
        runtime_binaries_directory=repository.config.paths.runtime_binaries_directory,
        state_directory=repository.config.paths.state_directory,
    ).path
    return directory / f"{runtime_variant}{profile.test_executable_suffix}"


def test_executable_path(
    profile: BuildProfile,
    preset: ConfigurePreset,
    target: str,
    *,
    root: Path | None = None,
    repository: RepositoryContext | None = None,
) -> Path:
    repository = repository or RepositoryContext.load()
    root = root or repository.root
    directory = resolve_location(
        "tests",
        profile=profile,
        preset=preset,
        root=root,
        runtime_binaries_directory=repository.config.paths.runtime_binaries_directory,
        state_directory=repository.config.paths.state_directory,
    ).path
    return directory / f"{target}{profile.test_executable_suffix}"


def run_native_test(context: BuildContext, output: BuildOutput) -> None:
    request = context.request
    paths = _context_paths(context)
    executable = test_executable_path(
        context.profile,
        context.preset,
        context.target,
        root=paths.root,
        repository=context.repository,
    )
    if not executable.is_file():
        raise BuildToolError(f'Test target "{context.target}" did not produce "{executable}".')
    command = [str(executable)]
    if request.test_filter:
        command.append(f"--gtest_filter={request.test_filter}")
    if output.compact:
        command.append("--gtest_brief=1")
    with output.stage("Test"):
        run_command(
            command,
            environment=context.environment or os.environ,
            output=output,
            recovery_required_on_interrupt=False,
            interruption_message="Native test run was interrupted.",
            timeout_seconds=request.test_timeout_seconds or None,
            colorize_test_output=True,
            show_heartbeat=request.agent,
            cwd=paths.root,
            state_directory=paths.state_directory,
        )


def run_exact_native_test(context: BuildContext, output: BuildOutput) -> None:
    """Execute one configured target through its registry-selected strategy."""
    registry = load_native_test_registry(context)
    resolved = resolve_selection(
        registry,
        context.target,
        admit_characterization=False,
        admit_qualification=False,
    )
    if len(resolved.targets) != 1:
        raise BuildToolError(
            f'Exact native-test target "{context.target}" resolved '
            f"{len(resolved.targets)} targets."
        )
    if resolved.targets[0].resolved_execution_host == "application":
        context.resolved_test_targets = resolved.names
        run_selected_native_tests(context, output)
    else:
        run_native_test(context, output)


def ctest_command(cmake: str) -> str:
    cmake_path = Path(cmake)
    executable_name = "ctest.exe" if cmake_path.suffix.casefold() == ".exe" else "ctest"
    if cmake_path.parent == Path("."):
        return executable_name
    return str(cmake_path.with_name(executable_name))


def _resolved_junit_path(path: Path | None, *, root: Path | None = None) -> Path | None:
    if path is None:
        return None
    resolved = path if path.is_absolute() else (root or default_build_paths().root) / path
    resolved.parent.mkdir(parents=True, exist_ok=True)
    return resolved


@dataclass(frozen=True)
class CTestInvocation:
    command: list[str]
    environment: dict[str, str]


def _ctest_environment(
    context: BuildContext,
    output: BuildOutput,
    *,
    gtest_filter: str = "",
    shuffle: bool = False,
) -> dict[str, str]:
    environment = dict(context.environment or os.environ)
    if output.compact:
        environment["GTEST_BRIEF"] = "1"
    if gtest_filter:
        environment["GTEST_FILTER"] = gtest_filter
    if not shuffle:
        return environment

    seed_text = environment.get("GTEST_RANDOM_SEED", "")
    if seed_text:
        try:
            seed = int(seed_text)
        except ValueError as error:
            raise BuildToolError(
                "GTEST_RANDOM_SEED must be an integer from 1 through 99999."
            ) from error
        if seed < 1 or seed > 99999:
            raise BuildToolError(
                "GTEST_RANDOM_SEED must be an integer from 1 through 99999."
            )
    else:
        seed = secrets.randbelow(99999) + 1
    environment["GTEST_SHUFFLE"] = "1"
    environment["GTEST_RANDOM_SEED"] = str(seed)
    output.info(
        f"GoogleTest shuffle seed: {seed} "
        f"(reproduce with GTEST_RANDOM_SEED={seed})"
    )
    return environment


def _ctest_invocation(
    context: BuildContext,
    output: BuildOutput,
    *,
    selection_arguments: list[str],
    junit_path: Path | None,
    schedule_random: bool = False,
    shuffle_google_test: bool = False,
    gtest_filter: str = "",
) -> CTestInvocation:
    request = context.request
    paths = _context_paths(context)
    command = [
        ctest_command(context.cmake),
        "--test-dir",
        str(preset_build_directory(context.preset, root=paths.root)),
        "--output-on-failure",
        "--no-tests=error",
        "-j",
        str(context.jobs),
    ]
    command.extend(selection_arguments)
    if request.test_timeout_seconds:
        command.extend(["--timeout", str(request.test_timeout_seconds)])
    if schedule_random:
        command.append("--schedule-random")
    if junit_path is not None:
        command.extend(["--output-junit", str(junit_path)])
    return CTestInvocation(
        command=command,
        environment=_ctest_environment(
            context,
            output,
            gtest_filter=gtest_filter,
            shuffle=shuffle_google_test,
        ),
    )


def _run_all_native_test_phase(
    context: BuildContext,
    output: BuildOutput,
    *,
    stage_name: str,
    junit_path: Path | None,
) -> None:
    request = context.request
    paths = _context_paths(context)
    stress = request.test_mode is TestMode.STRESS
    invocation = _ctest_invocation(
        context,
        output,
        selection_arguments=[
            "-L",
            "native-test-target",
            "-LE",
            "native-test-characterization|native-test-qualification",
        ],
        junit_path=junit_path,
        schedule_random=stress,
        shuffle_google_test=stress,
    )
    with output.stage(stage_name):
        run_command(
            invocation.command,
            environment=invocation.environment,
            output=output,
            recovery_required_on_interrupt=False,
            interruption_message="Native test run was interrupted.",
            colorize_test_output=True,
            show_heartbeat=request.agent,
            cwd=paths.root,
            state_directory=paths.state_directory,
        )


def run_all_native_tests(context: BuildContext, output: BuildOutput) -> None:
    request = context.request
    junit_path = None
    if request.test_mode is TestMode.REPORT:
        junit_path = _resolved_junit_path(
            request.test_report_path
            or Path("Build") / "NativeTestResults" / context.preset.name / "all.xml"
        )
    try:
        _run_all_native_test_phase(
            context,
            output,
            stage_name="Test native targets",
            junit_path=junit_path,
        )
    except BuildToolError:
        output.warning(
            "Batched native-test failure. Diagnose a reported case with: "
            ".\\DevTool.bat test <failed-target> <suite.case>"
        )
        raise


def _selected_report_path(context: BuildContext) -> Path | None:
    request = context.request
    if request.test_mode is not TestMode.REPORT:
        return None
    if request.test_report_path is not None:
        return _resolved_junit_path(request.test_report_path, root=_context_paths(context).root)
    selection_slug = re.sub(r"[^A-Za-z0-9_.-]+", "-", request.target).strip("-") or "selection"
    return _resolved_junit_path(
        Path("Build") / "NativeTestResults" / context.preset.name / f"{selection_slug}.xml",
        root=_context_paths(context).root,
    )


def run_selected_native_tests(context: BuildContext, output: BuildOutput) -> None:
    request = context.request
    names = context.resolved_test_targets
    if not names:
        raise BuildToolError("Native-test selection resolved no targets.")
    escaped_names = "|".join(re.escape(name) for name in names)
    paths = _context_paths(context)
    selection_arguments: list[str]
    if request.test_mode in {TestMode.ISOLATION, TestMode.CHARACTERIZATION}:
        selection_arguments = ["-L", "native-test-case", "-L", f"^({escaped_names})$"]
        if request.test_mode is TestMode.CHARACTERIZATION:
            selection_arguments.extend(["-L", "native-test-characterization"])
        else:
            selection_arguments.extend(
                ["-LE", "native-test-characterization|native-test-qualification"]
            )
        if request.test_filter:
            selection_arguments.extend(["-R", request.test_filter])
    elif request.test_mode is TestMode.QUALIFICATION:
        selection_arguments = [
            "-L",
            "native-test-target",
            "-L",
            "native-test-qualification",
            "-R",
            rf"^Durin\.NativeTestDirect\.({escaped_names})$",
        ]
    else:
        selection_arguments = [
            "-L",
            "native-test-target",
            "-LE",
            "native-test-characterization|native-test-qualification",
            "-R",
            rf"^Durin\.NativeTestDirect\.({escaped_names})$",
        ]
    junit_path = _selected_report_path(context)
    stress = request.test_mode is TestMode.STRESS
    invocation = _ctest_invocation(
        context,
        output,
        selection_arguments=selection_arguments,
        junit_path=junit_path,
        schedule_random=stress,
        shuffle_google_test=stress,
        gtest_filter=request.test_filter,
    )
    with output.stage(f"Test {request.test_mode.value} selection"):
        run_command(
            invocation.command,
            environment=invocation.environment,
            output=output,
            recovery_required_on_interrupt=False,
            interruption_message="Native test run was interrupted.",
            colorize_test_output=True,
            show_heartbeat=request.agent,
            cwd=paths.root,
            state_directory=paths.state_directory,
        )


def run_application(context: BuildContext, output: BuildOutput) -> None:
    from ..runtime_program import (
        ExecutableDescription,
        RuntimeProcessPolicy,
        RuntimeSelection,
        invoke_runtime_program,
    )

    paths = _context_paths(context)
    repository = context.repository or RepositoryContext.load().at_root(paths.root)
    selection = RuntimeSelection(repository, context.profile, context.preset)
    description = ExecutableDescription("Runtime", "all")
    arguments: list[str] = []
    if context.request.project_path is not None:
        arguments.append(f"--project={context.request.project_path}")
    arguments.extend(context.request.run_arguments)
    with output.stage("Run"):
        try:
            invoke_runtime_program(
                selection,
                description,
                arguments,
                output=output,
                policy=RuntimeProcessPolicy(
                    interruption_message="Application run was interrupted.",
                    wait_for_descendants=True,
                    colorize_log_levels=True,
                ),
            )
        except BuildToolError as error:
            executable = runtime_executable_path(
                context.profile,
                context.preset,
                root=paths.root,
                repository=repository,
            )
            runtime_variant = preset_cache_string(context.preset, "DURIN_RUNTIME_VARIANT")
            artifact = discover_current_crash(
                executable,
                runtime_variant,
                error.process_id,
                error.started_at_utc,
                error.ended_at_utc,
            )
            if artifact is None:
                raise
            analysis = analyze_crash(
                artifact,
                executable.parent,
                state_dir=paths.state_directory,
            )
            summary = format_crash_summary(artifact, analysis)
            excerpt = "\n".join(filter(None, (error.output_excerpt, summary)))
            raise BuildToolError(
                f"Runtime terminated with {format_windows_status(error.exit_code or 0)}.",
                command=error.command,
                exit_code=error.exit_code,
                recovery="Inspect the crash context and analysis paths reported above.",
                output_excerpt=excerpt,
                log_path=error.log_path,
                process_id=error.process_id,
                started_at_utc=error.started_at_utc,
                ended_at_utc=error.ended_at_utc,
            ) from error
