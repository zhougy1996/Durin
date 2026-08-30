"""Fresh-checkout setup orchestration using only the Python standard library."""

from __future__ import annotations

import json
import os
import shlex
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Mapping, Sequence

from ..context import CommandIO, RepositoryContext
from ..errors import DevToolError
from ..python_environment import prepared_python_path
from ..toolchain import find_command, find_vsdevcmd
from ..build.config_io import load_configure_presets, load_local_config, load_profiles
from ..build.errors import BuildToolError
from ..build.selection import (
    host_name,
    preset_cache_string,
    preset_output_configuration,
    select_profile,
)
from .agent_config import (
    AgentConfigError,
    config_path,
    ensure_agent_config,
    save_toolchain_config,
)
from .models import BootstrapError
from .preflight import PreflightError, validate_prerequisites
from .toolchain_selection import (
    DEFAULT_ENVIRONMENT_ARGUMENTS,
    ToolchainSelection,
    configured_cmake_command,
    configured_visual_studio_environment,
    find_vsdevcmd,
    resolve_toolchain,
)


MINIMUM_PYTHON = (3, 10)
VSCODE_TEMPLATE_FILES = ("settings.json", "extensions.json")


def _repository(
    repository: RepositoryContext | Path,
) -> RepositoryContext:
    if isinstance(repository, RepositoryContext):
        return repository
    return RepositoryContext.load().at_root(repository)


def _command_io(command_io: CommandIO | None = None) -> CommandIO:
    return command_io or CommandIO.system()


def is_linked_worktree(repository_root: Path) -> bool:
    return (repository_root / ".git").is_file()


def run_command(command: Sequence[str], *, cwd: Path, command_io: CommandIO) -> None:
    command_io.out(f"[run] {' '.join(command)}")
    try:
        result = subprocess.run(
            list(command),
            cwd=cwd,
            check=False,
            stdout=command_io.stdout,
            stderr=command_io.stderr,
        )
    except OSError as exc:
        raise BootstrapError(f"Could not run {command[0]}: {exc}") from exc
    if result.returncode != 0:
        raise BootstrapError(
            f"Command failed with exit code {result.returncode}: {' '.join(command)}"
        )


def _system_python_command() -> list[str]:
    if sys.platform == "win32" and find_command("py"):
        return ["py", "-3"]
    if sys.version_info >= MINIMUM_PYTHON:
        return [sys.executable]
    raise BootstrapError(
        "Python 3.10 or newer was not found. Install Python and enable "
        "the Python launcher or PATH option."
    )


def ensure_python_environment(
    repository: RepositoryContext | Path,
    command_io: CommandIO | None = None,
) -> Path:
    repository = _repository(repository)
    command_io = _command_io(command_io)
    repository_root = repository.root
    environment = repository_root / repository.config.worktrees.python_environment
    python = prepared_python_path(
        repository_root,
        repository.config.worktrees.python_environment,
    )
    if not python.is_file():
        command_io.out(f'Creating Python virtual environment at "{environment}"...')
        run_command(
            [*_system_python_command(), "-m", "venv", str(environment)],
            cwd=repository_root,
            command_io=command_io,
        )
    if not python.is_file():
        raise BootstrapError(
            f'Python environment creation did not produce "{python}".'
        )
    run_command(
        [
            str(python),
            "-c",
            "import sys; raise SystemExit(0 if sys.version_info >= (3, 10) else 1)",
        ],
        cwd=repository_root,
        command_io=command_io,
    )
    requirements = repository_root / "requirements.txt"
    if not requirements.is_file():
        raise BootstrapError(f'Requirements file does not exist: "{requirements}"')
    command_io.out(f'Installing Python dependencies from "{requirements}"...')
    run_command(
        [
            str(python),
            "-m",
            "pip",
            "install",
            "--disable-pip-version-check",
            "--requirement",
            str(requirements),
        ],
        cwd=repository_root,
        command_io=command_io,
    )
    run_command(
        [
            str(python),
            "-c",
            (
                "import clang.cindex, sys; from clang import native; "
                "from pathlib import Path; "
                "names = {'win32': ('libclang.dll',), "
                "'darwin': ('libclang.dylib',)}.get(sys.platform, ('libclang.so',)); "
                "raise SystemExit(0 if "
                "any((Path(native.__file__).parent / name).is_file() for name in names) else 1)"
            ),
        ],
        cwd=repository_root,
        command_io=command_io,
    )
    command_io.out(f'Python environment is ready: "{python}"')
    return python


def generate_vscode_launch_configuration(
    repository: RepositoryContext | Path,
    *,
    current_host: str | None = None,
    environment: Mapping[str, str] | None = None,
) -> dict[str, object]:
    repository = _repository(repository)
    repository_root = repository.root
    config = load_local_config(
        repository_root / repository.config.paths.local_build_config
    )
    profile_file = repository.resolve(repository.config.paths.build_profiles)
    profiles = load_profiles(profile_file)
    profile = select_profile(
        profiles,
        configured=config.default_build_profile,
        environment=environment,
        current_host=current_host or host_name(),
        profile_file=profile_file,
    )
    presets = load_configure_presets(
        repository_root / repository.config.paths.cmake_presets
    )
    configurations: list[dict[str, object]] = []
    for preset_name in profile.presets:
        preset = presets.get(preset_name)
        if preset is None:
            raise BuildToolError(
                f'Build profile "{profile.name}" references unknown CMake preset "{preset_name}".'
            )
        runtime_variant = preset_cache_string(preset, "DURIN_RUNTIME_VARIANT")
        output_configuration = preset_output_configuration(preset)
        executable = (
            "${workspaceFolder}/"
            f"{repository.config.paths.runtime_binaries_directory.as_posix()}/"
            f"{profile.platform}/{output_configuration}/Runtime/"
            f"{runtime_variant}/{runtime_variant}{profile.test_executable_suffix}"
        )
        launch: dict[str, object] = {
            "name": preset.name,
            "type": "cppvsdbg" if profile.host == "windows" else "cppdbg",
            "request": "launch",
            "program": executable,
            "cwd": "${workspaceFolder}",
            "stopAtEntry": False,
            "console": "integratedTerminal",
        }
        if profile.host == "macos":
            launch["MIMode"] = "lldb"
        configurations.append(launch)
    return {"version": "0.2.0", "configurations": configurations}


def ensure_vscode_configuration(
    repository: RepositoryContext | Path,
    command_io: CommandIO | None = None,
) -> Path:
    repository = _repository(repository)
    command_io = _command_io(command_io)
    repository_root = repository.root
    target_directory = repository_root / repository.config.worktrees.vscode_directory
    template_directory = repository.resolve(repository.config.paths.vscode_templates)
    if target_directory.exists() and not target_directory.is_dir():
        raise BootstrapError(
            f'VS Code configuration path is not a directory: "{target_directory}"'
        )
    missing_templates = [
        template_directory / file_name
        for file_name in VSCODE_TEMPLATE_FILES
        if not (template_directory / file_name).is_file()
    ]
    if missing_templates:
        formatted = "\n".join(f'  "{path}"' for path in missing_templates)
        raise BootstrapError(f"VS Code configuration templates are missing:\n{formatted}")

    launch_target = target_directory / "launch.json"
    launch_configuration: dict[str, object] | None = None
    if launch_target.is_file() and not launch_target.is_symlink():
        command_io.out(f'VS Code configuration already exists: "{launch_target}"')
    elif launch_target.exists() or launch_target.is_symlink():
        raise BootstrapError(
            f'VS Code configuration path is not a regular file: "{launch_target}"'
        )
    else:
        try:
            launch_configuration = generate_vscode_launch_configuration(repository)
        except BuildToolError as exc:
            raise BootstrapError(str(exc)) from exc

    target_directory.mkdir(parents=True, exist_ok=True)
    for file_name in VSCODE_TEMPLATE_FILES:
        target = target_directory / file_name
        if target.is_file() and not target.is_symlink():
            command_io.out(f'VS Code configuration already exists: "{target}"')
            continue
        if target.exists() or target.is_symlink():
            raise BootstrapError(
                f'VS Code configuration path is not a regular file: "{target}"'
            )
        template = template_directory / file_name
        shutil.copy2(template, target)
        command_io.out(f'Created VS Code configuration: "{target}"')
    if launch_configuration is not None:
        launch_target.write_text(
            json.dumps(launch_configuration, indent=2) + "\n",
            encoding="utf-8",
        )
        command_io.out(f'Created VS Code configuration: "{launch_target}"')
    return target_directory


def _prompt_value(label: str, default: str = "") -> str:
    suffix = f" [{default}]" if default else ""
    try:
        value = input(f"{label}{suffix}: ").strip()
    except EOFError as exc:
        raise BootstrapError(
            "Interactive toolchain setup requires a terminal. "
            "Use --non-interactive with configured paths instead."
        ) from exc
    return value or default


def _confirm_toolchain(selection: ToolchainSelection, command_io: CommandIO) -> bool:
    command_io.out("Automatically detected toolchain:")
    command_io.out(f'  CMake: {selection.cmake_command}')
    if selection.environment_script is None:
        command_io.out("  Environment: inherited from the current process")
    else:
        command_io.out(f'  Environment script: {selection.environment_script}')
        command_io.out(f'  Arguments: {" ".join(selection.environment_arguments)}')
    return _prompt_value("Use these settings?", "Y").casefold() in {"y", "yes"}


def _manual_toolchain_selection(
    repository: RepositoryContext,
    command_io: CommandIO,
    *,
    default_cmake: str,
    default_script: Path | None,
    default_arguments: Sequence[str],
    current_platform: str,
) -> ToolchainSelection:
    while True:
        cmake_command = _prompt_value("CMake executable or command", default_cmake)
        script_value = _prompt_value(
            "VsDevCmd.bat path" if current_platform == "win32" else "Environment setup script path (optional)",
            str(default_script) if default_script else "",
        )
        if current_platform == "win32" and not script_value:
            command_io.out("VsDevCmd.bat path is required.")
            continue
        arguments_text = _prompt_value(
            "VsDevCmd.bat arguments" if current_platform == "win32" else "Environment script arguments",
            " ".join(default_arguments),
        )
        try:
            arguments = tuple(shlex.split(arguments_text, posix=False))
            selection = resolve_toolchain(
                repository.root,
                cmake_command=cmake_command,
                environment_script=(
                    Path(script_value).expanduser().resolve() if script_value else None
                ),
                environment_arguments=arguments,
                repository_context=repository,
            )
        except (DevToolError, ValueError) as exc:
            command_io.out(f"Toolchain settings are not usable: {exc}")
            continue
        command_io.out(f'Validated CMake {selection.cmake_command}.')
        return selection


def select_setup_toolchain(
    repository: RepositoryContext | Path,
    command_io: CommandIO | None = None,
    *,
    interactive: bool,
) -> ToolchainSelection:
    repository = _repository(repository)
    command_io = _command_io(command_io)
    repository_root = repository.root
    config_exists = config_path(repository_root, repository).is_file()
    automatic_error = ""
    try:
        selection = resolve_toolchain(
            repository_root,
            repository_context=repository,
        )
    except DevToolError as exc:
        automatic_error = str(exc)
        selection = None
    if selection is not None and (config_exists or not interactive):
        return selection
    if selection is not None and _confirm_toolchain(selection, command_io):
        return selection
    if not interactive:
        detail = automatic_error if selection is None else "automatic settings were declined"
        raise BootstrapError(
            f"Toolchain setup was not confirmed: {detail}. "
            "Set cmake.command or an optional toolchain.environmentScript in "
            '".agents/DevTool.user.json" when automatic discovery is insufficient, '
            "then rerun with --non-interactive."
        )

    configured_script, configured_arguments = configured_visual_studio_environment(
        repository_root,
        repository_context=repository,
    )
    if configured_script is None:
        try:
            configured_script = find_vsdevcmd(os.environ)
        except DevToolError:
            pass
    default_cmake = configured_cmake_command(
        repository_root,
        repository_context=repository,
    )
    if selection is not None:
        default_cmake = selection.cmake_command
        configured_script = selection.environment_script
        configured_arguments = list(selection.environment_arguments)
    if selection is None:
        command_io.out(f"Automatic toolchain detection failed: {automatic_error}")
    return _manual_toolchain_selection(
        repository,
        command_io,
        default_cmake=default_cmake,
        default_script=configured_script,
        default_arguments=(
            configured_arguments or DEFAULT_ENVIRONMENT_ARGUMENTS
            if sys.platform == "win32"
            else configured_arguments
        ),
        current_platform=sys.platform,
    )


def setup_repository(
    repository: RepositoryContext | Path,
    command_io: CommandIO | None = None,
    *,
    interactive: bool = False,
) -> Path:
    """Prepare a main checkout in preflight-before-mutation order."""
    repository = _repository(repository)
    command_io = _command_io(command_io)
    repository_root = repository.root
    if is_linked_worktree(repository_root):
        raise BootstrapError(
            "DevTool setup only initializes the main checkout. "
            "Run 'DevTool worktree prepare' from this linked worktree instead."
        )
    try:
        selection = select_setup_toolchain(
            repository,
            command_io,
            interactive=interactive,
        )
        validate_prerequisites(
            repository_root,
            selection=selection,
            repository_context=repository,
            command_io=command_io,
        )
        try:
            ensure_agent_config(repository_root, repository, command_io)
        except AgentConfigError as exc:
            raise BootstrapError(str(exc)) from exc
        try:
            save_toolchain_config(
                repository_root,
                repository,
                command_io,
                cmake_command=selection.cmake_command,
                environment_script=selection.environment_script,
                environment_arguments=selection.environment_arguments,
            )
        except AgentConfigError as exc:
            raise BootstrapError(str(exc)) from exc
        ensure_vscode_configuration(repository, command_io)
        python = ensure_python_environment(repository, command_io)
    except OSError as exc:
        raise BootstrapError(str(exc)) from exc
    command_io.out("Durin setup completed successfully.")
    return python
