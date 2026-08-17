"""Runtime launch and native-test execution services."""

from __future__ import annotations

import os
import secrets
import re
from pathlib import Path

from ..context import RepositoryContext
from .config import (
    BuildPaths,
    BuildContext,
    BuildProfile,
    BuildToolError,
    ConfigurePreset,
    TestGranularity,
    TestMode,
    default_build_paths,
    preset_build_directory,
    preset_cache_string,
)
from .locations import resolve_location
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


def _all_native_tests_command(
    context: BuildContext,
    *,
    granularity: TestGranularity,
    junit_path: Path | None,
) -> list[str]:
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
    selected_label = {
        TestGranularity.CASE: "native-test-case",
        TestGranularity.TARGET: "native-test-target",
        TestGranularity.HYBRID: "native-test-default",
    }[granularity]
    command.extend(
        ["-L", selected_label, "-LE", "native-test-characterization|native-test-qualification"]
    )
    if request.test_timeout_seconds:
        command.extend(["--timeout", str(request.test_timeout_seconds)])
    if request.test_schedule_random:
        command.append("--schedule-random")
    if request.test_ctest_regex:
        command.extend(["-R", request.test_ctest_regex])
    if junit_path is not None:
        command.extend(["--output-junit", str(junit_path)])
    return command


def _run_all_native_test_phase(
    context: BuildContext,
    output: BuildOutput,
    *,
    stage_name: str,
    granularity: TestGranularity,
    junit_path: Path | None,
    environment: dict[str, str],
) -> None:
    request = context.request
    paths = _context_paths(context)
    command = _all_native_tests_command(
        context,
        granularity=granularity,
        junit_path=junit_path,
    )
    with output.stage(stage_name):
        try:
            run_command(
                command,
                environment=environment,
                output=output,
                recovery_required_on_interrupt=False,
                interruption_message="Native test run was interrupted.",
                colorize_test_output=True,
                show_heartbeat=request.agent,
                cwd=paths.root,
                state_directory=paths.state_directory,
            )
        except BuildToolError as error:
            failure_text = f"{error}\n{error.output_excerpt}"
            if request.test_ctest_regex and "No tests were found" in failure_text:
                raise BuildToolError(
                    f'No case registrations matched --ctest-regex "{request.test_ctest_regex}".',
                    command=error.command,
                    exit_code=error.exit_code,
                    recovery=(
                        "Inspect registered names with ctest --show-only, then rerun "
                        "test --target all --granularity case --ctest-regex <pattern>."
                    ),
                    output_excerpt=error.output_excerpt,
                    log_path=error.log_path,
                ) from error
            raise


def _aggregate_test_environment(
    context: BuildContext,
    output: BuildOutput,
) -> dict[str, str]:
    environment = dict(context.environment or os.environ)
    request = context.request
    if output.compact:
        environment["GTEST_BRIEF"] = "1"
    if (
        not request.test_schedule_random
        or request.test_granularity is TestGranularity.CASE
    ):
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


def run_all_native_tests(context: BuildContext, output: BuildOutput) -> None:
    request = context.request
    granularity = request.test_granularity
    junit_path = _resolved_junit_path(request.test_output_junit)
    if request.test_mode is TestMode.REPORT and junit_path is None:
        junit_path = _resolved_junit_path(
            Path("Build") / "NativeTestResults" / context.preset.name / "all.xml"
        )
    environment = _aggregate_test_environment(context, output)
    try:
        _run_all_native_test_phase(
            context,
            output,
            stage_name=f"Test {granularity.value} native tests",
            granularity=granularity,
            junit_path=junit_path,
            environment=environment,
        )
    except BuildToolError:
        if granularity is not TestGranularity.CASE:
            output.warning(
                "Batched native-test failure. Diagnose a reported case without "
                "launching the full case matrix: .\\DevTool.bat test "
                "--target <failed-target> --filter <suite.case>"
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
    command = [
        ctest_command(context.cmake),
        "--test-dir",
        str(preset_build_directory(context.preset, root=paths.root)),
        "--output-on-failure",
        "--no-tests=error",
        "-j",
        str(context.jobs),
    ]
    environment = dict(context.environment or os.environ)
    if output.compact:
        environment["GTEST_BRIEF"] = "1"
    if request.test_filter:
        environment["GTEST_FILTER"] = request.test_filter
    if request.test_mode in {TestMode.ISOLATION, TestMode.CHARACTERIZATION}:
        command.extend(["-L", "native-test-case", "-L", f"^({escaped_names})$"])
        if request.test_mode is TestMode.CHARACTERIZATION:
            command.extend(["-L", "native-test-characterization"])
        else:
            command.extend(["-LE", "native-test-characterization|native-test-qualification"])
        if request.test_filter:
            command.extend(["-R", request.test_filter])
    elif request.test_mode is TestMode.QUALIFICATION:
        command.extend(
            [
                "-L",
                "native-test-target",
                "-L",
                "native-test-qualification",
                "-R",
                rf"^Durin\.NativeTestDirect\.({escaped_names})$",
            ]
        )
    else:
        command.extend(
            [
                "-L",
                "native-test-target",
                "-LE",
                "native-test-characterization|native-test-qualification",
                "-R",
                rf"^Durin\.NativeTestDirect\.({escaped_names})$",
            ]
        )
    if request.test_timeout_seconds:
        command.extend(["--timeout", str(request.test_timeout_seconds)])
    if request.test_mode is TestMode.STRESS:
        command.append("--schedule-random")
        seed_text = environment.get("GTEST_RANDOM_SEED", "")
        seed = int(seed_text) if seed_text else secrets.randbelow(99999) + 1
        if seed < 1 or seed > 99999:
            raise BuildToolError("GTEST_RANDOM_SEED must be an integer from 1 through 99999.")
        environment["GTEST_SHUFFLE"] = "1"
        environment["GTEST_RANDOM_SEED"] = str(seed)
        output.info(f"GoogleTest shuffle seed: {seed} (reproduce with GTEST_RANDOM_SEED={seed})")
    junit_path = _selected_report_path(context)
    if junit_path is not None:
        command.extend(["--output-junit", str(junit_path)])
    with output.stage(f"Test {request.test_mode.value} selection"):
        run_command(
            command,
            environment=environment,
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
