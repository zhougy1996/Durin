"""Runtime launch and native-test execution services."""

from __future__ import annotations

import os
import secrets
import re
from pathlib import Path

from .config import (
    REPO_ROOT,
    BuildContext,
    BuildProfile,
    BuildToolError,
    ConfigurePreset,
    TestGranularity,
    TestMode,
    preset_build_directory,
    preset_cache_string,
)
from .locations import resolve_location
from .output import BuildOutput
from .process import run_command
from .crash import analyze_crash, discover_current_crash, format_crash_summary, format_windows_status


def runtime_executable_path(
    profile: BuildProfile,
    preset: ConfigurePreset,
    *,
    root: Path = REPO_ROOT,
) -> Path:
    runtime_variant = preset_cache_string(preset, "DURIN_RUNTIME_VARIANT")
    directory = resolve_location(
        "runtime",
        profile=profile,
        preset=preset,
        root=root,
    ).path
    return directory / f"{runtime_variant}{profile.test_executable_suffix}"


def test_executable_path(
    profile: BuildProfile,
    preset: ConfigurePreset,
    target: str,
    *,
    root: Path = REPO_ROOT,
) -> Path:
    directory = resolve_location(
        "tests",
        profile=profile,
        preset=preset,
        root=root,
    ).path
    return directory / f"{target}{profile.test_executable_suffix}"


def run_native_test(context: BuildContext, output: BuildOutput) -> None:
    request = context.request
    executable = test_executable_path(context.profile, context.preset, context.target)
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
        )


def ctest_command(cmake: str) -> str:
    cmake_path = Path(cmake)
    executable_name = "ctest.exe" if cmake_path.suffix.casefold() == ".exe" else "ctest"
    if cmake_path.parent == Path("."):
        return executable_name
    return str(cmake_path.with_name(executable_name))


def _resolved_junit_path(path: Path | None) -> Path | None:
    if path is None:
        return None
    resolved = path if path.is_absolute() else REPO_ROOT / path
    resolved.parent.mkdir(parents=True, exist_ok=True)
    return resolved


def _direct_junit_path(path: Path) -> Path:
    suffix = path.suffix or ".xml"
    stem = path.stem if path.suffix else path.name
    return path.with_name(f"{stem}.direct{suffix}")


def _all_native_tests_command(
    context: BuildContext,
    *,
    granularity: TestGranularity,
    compatibility_phase: bool,
    junit_path: Path | None,
) -> list[str]:
    request = context.request
    command = [
        ctest_command(context.cmake),
        "--test-dir",
        str(preset_build_directory(context.preset)),
        "--output-on-failure",
        (
            "--no-tests=ignore"
            if compatibility_phase and request.test_ctest_regex
            else "--no-tests=error"
        ),
        "-j",
        str(context.jobs),
    ]
    if compatibility_phase:
        excluded_labels = "native-test-characterization|native-test-qualification"
        if granularity is TestGranularity.HYBRID:
            excluded_labels += "|native-test-default"
        command.extend(["-L", "native-test-target", "-LE", excluded_labels])
    else:
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
    compatibility_phase: bool,
    junit_path: Path | None,
    environment: dict[str, str],
) -> None:
    request = context.request
    command = _all_native_tests_command(
        context,
        granularity=granularity,
        compatibility_phase=compatibility_phase,
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
            compatibility_phase=False,
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
    if not request.test_include_direct:
        return
    if granularity is TestGranularity.TARGET:
        output.info(
            "--include-direct selected no additional registrations in target mode."
        )
        return
    _run_all_native_test_phase(
        context,
        output,
        stage_name="Test compatibility direct lifecycles",
        granularity=granularity,
        compatibility_phase=True,
        junit_path=_direct_junit_path(junit_path) if junit_path is not None else None,
        environment=environment,
    )


def _selected_report_path(context: BuildContext) -> Path | None:
    request = context.request
    if request.test_mode is not TestMode.REPORT:
        return None
    if request.test_report_path is not None:
        return _resolved_junit_path(request.test_report_path)
    selection_slug = re.sub(r"[^A-Za-z0-9_.-]+", "-", request.target).strip("-") or "selection"
    return _resolved_junit_path(
        Path("Build") / "NativeTestResults" / context.preset.name / f"{selection_slug}.xml"
    )


def run_selected_native_tests(context: BuildContext, output: BuildOutput) -> None:
    request = context.request
    names = context.resolved_test_targets
    if not names:
        raise BuildToolError("Native-test selection resolved no targets.")
    escaped_names = "|".join(re.escape(name) for name in names)
    command = [
        ctest_command(context.cmake),
        "--test-dir",
        str(preset_build_directory(context.preset)),
        "--output-on-failure",
        "--no-tests=error",
        "-j",
        str(context.jobs),
    ]
    environment = dict(context.environment or os.environ)
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
        )


def run_application(context: BuildContext, output: BuildOutput) -> None:
    executable = runtime_executable_path(context.profile, context.preset)
    if not executable.is_file():
        raise BuildToolError(
            f'Runtime executable was not found: "{executable}".',
            recovery="Build the complete runtime first with build --target all.",
        )
    arguments = [str(executable)]
    if context.request.project_path is not None:
        arguments.append(f"--project={context.request.project_path}")
    arguments.extend(context.request.run_arguments)
    with output.stage("Run"):
        try:
            run_command(
                arguments,
                environment=os.environ,
                output=output,
                colorize_log_levels=True,
                recovery_required_on_interrupt=False,
                wait_for_descendants=True,
                show_heartbeat=False,
            )
        except BuildToolError as error:
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
            analysis = analyze_crash(artifact, executable.parent)
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
