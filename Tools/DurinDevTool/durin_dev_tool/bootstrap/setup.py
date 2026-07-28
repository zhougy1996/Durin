"""Fresh-checkout setup orchestration using only the Python standard library."""

from __future__ import annotations

import json
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
from .agent_config import AgentConfigError, ensure_agent_config
from .dependencies import BootstrapError, DependencyRequest, prepare_dependencies
from .preflight import validate_prerequisites


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


def setup_repository(repository_root: Path) -> Path:
    """Prepare a main checkout in preflight-before-mutation order."""
    repository_root = repository_root.resolve()
    if is_linked_worktree(repository_root):
        raise BootstrapError(
            "DevTool setup only initializes the main checkout. "
            "Run 'DevTool worktree prepare' from this linked worktree instead."
        )
    validate_prerequisites(repository_root)
    try:
        ensure_agent_config(repository_root)
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
        ),
    )
    print("Durin setup completed successfully.")
    return python
