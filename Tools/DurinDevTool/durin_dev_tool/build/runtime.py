"""Runtime launch and native-test execution services."""

from __future__ import annotations

import os
import subprocess
from pathlib import Path

from .config import (
    REPO_ROOT,
    REPOSITORY_CONFIG,
    BuildContext,
    BuildProfile,
    BuildToolError,
    ConfigurePreset,
    preset_build_directory,
    preset_cache_string,
    preset_output_configuration,
)
from .output import BuildOutput
from .process import run_command


def runtime_executable_path(
    profile: BuildProfile,
    preset: ConfigurePreset,
    *,
    root: Path = REPO_ROOT,
) -> Path:
    runtime_variant = preset_cache_string(preset, "DURIN_RUNTIME_VARIANT")
    return (
        root
        / REPOSITORY_CONFIG.paths.runtime_binaries_directory
        / profile.platform
        / preset_output_configuration(preset)
        / "Runtime"
        / runtime_variant
        / f"{runtime_variant}{profile.test_executable_suffix}"
    )


def open_runtime_directory(context: BuildContext, output: BuildOutput) -> None:
    directory = runtime_executable_path(context.profile, context.preset).parent
    if not directory.is_dir():
        raise BuildToolError(
            f'Runtime directory was not found: "{directory}".',
            recovery="Build the complete runtime first with build --target all.",
        )
    try:
        if context.current_host == "windows":
            os.startfile(directory)  # type: ignore[attr-defined]
        else:
            opener = "open" if context.current_host == "macos" else "xdg-open"
            subprocess.Popen(
                [opener, str(directory)],
                cwd=REPO_ROOT,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
    except (AttributeError, OSError) as exc:
        raise BuildToolError(f'Could not open runtime directory "{directory}": {exc}') from exc
    output.success(f'Opened runtime directory: "{directory}"')


def test_executable_path(
    profile: BuildProfile,
    preset: ConfigurePreset,
    target: str,
) -> Path:
    runtime_variant = preset_cache_string(preset, "DURIN_RUNTIME_VARIANT")
    return (
        REPO_ROOT
        / REPOSITORY_CONFIG.paths.runtime_binaries_directory
        / profile.platform
        / preset_output_configuration(preset)
        / "Tests"
        / runtime_variant
        / "Bin"
        / f"{target}{profile.test_executable_suffix}"
    )


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
        )


def ctest_command(cmake: str) -> str:
    cmake_path = Path(cmake)
    executable_name = "ctest.exe" if cmake_path.suffix.casefold() == ".exe" else "ctest"
    if cmake_path.parent == Path("."):
        return executable_name
    return str(cmake_path.with_name(executable_name))


def run_all_native_tests(context: BuildContext, output: BuildOutput) -> None:
    request = context.request
    command = [
        ctest_command(context.cmake),
        "--test-dir",
        str(preset_build_directory(context.preset)),
        "--output-on-failure",
        "--no-tests=error",
        "-j",
        str(context.jobs),
    ]
    if request.test_timeout_seconds:
        command.extend(["--timeout", str(request.test_timeout_seconds)])
    if request.test_schedule_random:
        command.append("--schedule-random")
    if request.test_ctest_regex:
        command.extend(["-R", request.test_ctest_regex])
    if request.test_output_junit is not None:
        junit_path = request.test_output_junit
        if not junit_path.is_absolute():
            junit_path = REPO_ROOT / junit_path
        junit_path.parent.mkdir(parents=True, exist_ok=True)
        command.extend(["--output-junit", str(junit_path)])
    with output.stage("Test all"):
        run_command(
            command,
            environment=context.environment or os.environ,
            output=output,
            recovery_required_on_interrupt=False,
            interruption_message="Native test run was interrupted.",
            colorize_test_output=True,
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
        run_command(
            arguments,
            environment=os.environ,
            output=output,
            colorize_log_levels=True,
            recovery_required_on_interrupt=False,
            wait_for_descendants=True,
            show_heartbeat=False,
        )
