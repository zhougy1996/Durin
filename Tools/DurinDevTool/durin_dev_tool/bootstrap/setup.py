"""Fresh-checkout setup orchestration using only the Python standard library."""

from __future__ import annotations

import shutil
import subprocess
import sys
from pathlib import Path
from typing import Sequence

from .agent_config import AgentConfigError, ensure_agent_config
from .dependencies import BootstrapError, DependencyRequest, prepare_dependencies
from .preflight import validate_prerequisites


MINIMUM_PYTHON = (3, 10)
VSCODE_TEMPLATE_DIRECTORY = Path("Templates") / "VSCode"
VSCODE_CONFIGURATION_FILES = ("settings.json", "launch.json", "extensions.json")


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
    environment = repository_root / ".venv"
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


def ensure_vscode_configuration(repository_root: Path) -> Path:
    target_directory = repository_root / ".vscode"
    template_directory = repository_root / VSCODE_TEMPLATE_DIRECTORY
    if target_directory.exists() and not target_directory.is_dir():
        raise BootstrapError(
            f'VS Code configuration path is not a directory: "{target_directory}"'
        )
    missing_templates = [
        template_directory / file_name
        for file_name in VSCODE_CONFIGURATION_FILES
        if not (template_directory / file_name).is_file()
    ]
    if missing_templates:
        formatted = "\n".join(f'  "{path}"' for path in missing_templates)
        raise BootstrapError(f"VS Code configuration templates are missing:\n{formatted}")

    target_directory.mkdir(parents=True, exist_ok=True)
    for file_name in VSCODE_CONFIGURATION_FILES:
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
