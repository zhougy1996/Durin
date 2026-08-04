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

from ..configuration import load_repository_config
from ..build.config import (
    BuildToolError,
    host_name,
    load_configure_presets,
    load_local_config,
    load_profiles,
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
from .dependencies import BootstrapError, DependencyRequest, prepare_dependencies
from .preflight import (
    DEFAULT_ENVIRONMENT_ARGUMENTS,
    PreflightError,
    ToolchainSelection,
    configured_cmake_command,
    configured_visual_studio_environment,
    find_vsdevcmd,
    resolve_toolchain,
    validate_prerequisites,
)


REPOSITORY_CONFIG = load_repository_config()
MINIMUM_PYTHON = (3, 10)
VSCODE_TEMPLATE_DIRECTORY = REPOSITORY_CONFIG.paths.vscode_templates
VSCODE_TEMPLATE_FILES = ("settings.json", "extensions.json")


def is_linked_worktree(repository_root: Path) -> bool:
    return (repository_root / ".git").is_file()


def run_command(command: Sequence[str], *, cwd: Path) -> None:
    print(f"[run] {' '.join(command)}")
    try:
        result = subprocess.run(list(command), cwd=cwd, check=False)
    except OSError as exc:
        raise BootstrapError(f"Could not run {command[0]}: {exc}") from exc
    if result.returncode != 0:
        raise BootstrapError(
            f"Command failed with exit code {result.returncode}: {' '.join(command)}"
        )


def _system_python_command() -> list[str]:
    if sys.platform == "win32" and shutil.which("py"):
        return ["py", "-3"]
    if executable := shutil.which("python"):
        return [executable]
    raise BootstrapError(
        "Python 3.10 or newer was not found. Install Python and enable "
        "the Python launcher or PATH option."
    )


def ensure_python_environment(repository_root: Path) -> Path:
    environment = repository_root / REPOSITORY_CONFIG.worktrees.python_environment
    python = environment / "Scripts" / "python.exe"
    if not python.is_file():
        print(f'Creating Python virtual environment at "{environment}"...')
        run_command(
            [*_system_python_command(), "-m", "venv", str(environment)],
            cwd=repository_root,
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
    )
    requirements = repository_root / "requirements.txt"
    if not requirements.is_file():
        raise BootstrapError(f'Requirements file does not exist: "{requirements}"')
    print(f'Installing Python dependencies from "{requirements}"...')
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
    )
    run_command(
        [
            str(python),
            "-c",
            (
                "import clang.cindex; from clang import native; "
                "from pathlib import Path; "
                "raise SystemExit(0 if "
                "(Path(native.__file__).parent / 'libclang.dll').is_file() else 1)"
            ),
        ],
        cwd=repository_root,
    )
    print(f'Python environment is ready: "{python}"')
    return python


def generate_vscode_launch_configuration(
    repository_root: Path,
    *,
    current_host: str | None = None,
    environment: Mapping[str, str] | None = None,
) -> dict[str, object]:
    config = load_local_config(
        repository_root / REPOSITORY_CONFIG.paths.local_build_config
    )
    profiles = load_profiles()
    profile = select_profile(
        profiles,
        configured=config.default_build_profile,
        environment=environment,
        current_host=current_host or host_name(),
    )
    presets = load_configure_presets(
        repository_root / REPOSITORY_CONFIG.paths.cmake_presets
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
            f"{REPOSITORY_CONFIG.paths.runtime_binaries_directory.as_posix()}/"
            f"{profile.platform}/{output_configuration}/Runtime/"
            f"{runtime_variant}/{runtime_variant}{profile.test_executable_suffix}"
        )
        configurations.append(
            {
                "name": preset.name,
                "type": "cppvsdbg",
                "request": "launch",
                "program": executable,
                "cwd": "${workspaceFolder}",
                "stopAtEntry": False,
                "console": "integratedTerminal",
            }
        )
    return {"version": "0.2.0", "configurations": configurations}


def ensure_vscode_configuration(repository_root: Path) -> Path:
    target_directory = repository_root / REPOSITORY_CONFIG.worktrees.vscode_directory
    template_directory = repository_root / VSCODE_TEMPLATE_DIRECTORY
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
        print(f'VS Code configuration already exists: "{launch_target}"')
    elif launch_target.exists() or launch_target.is_symlink():
        raise BootstrapError(
            f'VS Code configuration path is not a regular file: "{launch_target}"'
        )
    else:
        try:
            launch_configuration = generate_vscode_launch_configuration(repository_root)
        except BuildToolError as exc:
            raise BootstrapError(str(exc)) from exc

    target_directory.mkdir(parents=True, exist_ok=True)
    for file_name in VSCODE_TEMPLATE_FILES:
        target = target_directory / file_name
        if target.is_file() and not target.is_symlink():
            print(f'VS Code configuration already exists: "{target}"')
            continue
        if target.exists() or target.is_symlink():
            raise BootstrapError(
                f'VS Code configuration path is not a regular file: "{target}"'
            )
        template = template_directory / file_name
        shutil.copy2(template, target)
        print(f'Created VS Code configuration: "{target}"')
    if launch_configuration is not None:
        launch_target.write_text(
            json.dumps(launch_configuration, indent=2) + "\n",
            encoding="utf-8",
        )
        print(f'Created VS Code configuration: "{launch_target}"')
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


def _confirm_toolchain(selection: ToolchainSelection) -> bool:
    print("Automatically detected toolchain:")
    print(f'  CMake: {selection.cmake_command}')
    print(f'  VsDevCmd: {selection.environment_script}')
    print(f'  Arguments: {" ".join(selection.environment_arguments)}')
    return _prompt_value("Use these settings?", "Y").casefold() in {"y", "yes"}


def _manual_toolchain_selection(
    repository_root: Path,
    *,
    default_cmake: str,
    default_script: Path | None,
    default_arguments: Sequence[str],
) -> ToolchainSelection:
    while True:
        cmake_command = _prompt_value("CMake executable or command", default_cmake)
        script_value = _prompt_value(
            "VsDevCmd.bat path",
            str(default_script) if default_script else "",
        )
        if not script_value:
            print("VsDevCmd.bat path is required.")
            continue
        arguments_text = _prompt_value(
            "VsDevCmd.bat arguments",
            " ".join(default_arguments),
        )
        try:
            arguments = tuple(shlex.split(arguments_text, posix=False))
            selection = resolve_toolchain(
                repository_root,
                cmake_command=cmake_command,
                environment_script=Path(script_value).expanduser().resolve(),
                environment_arguments=arguments,
            )
        except (PreflightError, ValueError) as exc:
            print(f"Toolchain settings are not usable: {exc}")
            continue
        print(f'Validated CMake {selection.cmake_command}.')
        return selection


def select_setup_toolchain(
    repository_root: Path,
    *,
    interactive: bool,
) -> ToolchainSelection:
    config_exists = config_path(repository_root).is_file()
    automatic_error = ""
    try:
        selection = resolve_toolchain(repository_root)
    except PreflightError as exc:
        automatic_error = str(exc)
        selection = None
    if selection is not None and (config_exists or not interactive):
        return selection
    if selection is not None and _confirm_toolchain(selection):
        return selection
    if not interactive:
        detail = automatic_error if selection is None else "automatic settings were declined"
        raise BootstrapError(
            f"Toolchain setup was not confirmed: {detail}. "
            "Set cmake.command and toolchain.environmentScript in "
            '".agents/DevTool.user.json", then rerun with --non-interactive.'
        )

    configured_script, configured_arguments = configured_visual_studio_environment(repository_root)
    if configured_script is None:
        try:
            configured_script = find_vsdevcmd(os.environ)
        except PreflightError:
            pass
    default_cmake = configured_cmake_command(repository_root)
    if selection is not None:
        default_cmake = selection.cmake_command
        configured_script = selection.environment_script
        configured_arguments = list(selection.environment_arguments)
    if selection is None:
        print(f"Automatic toolchain detection failed: {automatic_error}")
    return _manual_toolchain_selection(
        repository_root,
        default_cmake=default_cmake,
        default_script=configured_script,
        default_arguments=configured_arguments or DEFAULT_ENVIRONMENT_ARGUMENTS,
    )


def setup_repository(repository_root: Path, *, interactive: bool = False) -> Path:
    """Prepare a main checkout in preflight-before-mutation order."""
    repository_root = repository_root.resolve()
    if is_linked_worktree(repository_root):
        raise BootstrapError(
            "DevTool setup only initializes the main checkout. "
            "Run 'DevTool worktree prepare' from this linked worktree instead."
        )
    try:
        selection = select_setup_toolchain(repository_root, interactive=interactive)
        validate_prerequisites(repository_root, selection=selection)
        try:
            ensure_agent_config(repository_root)
        except AgentConfigError as exc:
            raise BootstrapError(str(exc)) from exc
        try:
            save_toolchain_config(
                repository_root,
                cmake_command=selection.cmake_command,
                environment_script=selection.environment_script,
                environment_arguments=selection.environment_arguments,
            )
        except AgentConfigError as exc:
            raise BootstrapError(str(exc)) from exc
        ensure_vscode_configuration(repository_root)
        python = ensure_python_environment(repository_root)
        prepare_dependencies(
            repository_root,
            DependencyRequest(
                use_all=True,
                with_tests=True,
                with_development=True,
                cmake_command=selection.cmake_command,
            ),
            environment=selection.environment,
        )
    except OSError as exc:
        raise BootstrapError(str(exc)) from exc
    print("Durin setup completed successfully.")
    return python
