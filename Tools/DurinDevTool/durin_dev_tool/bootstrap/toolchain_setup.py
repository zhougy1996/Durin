"""Configuration-first toolchain discovery, validation, and text fallback."""

from __future__ import annotations

import os
from dataclasses import dataclass
from pathlib import Path
from typing import Callable, Mapping, Sequence, TypeVar

from ..context import CommandIO, RepositoryContext
from ..errors import DevToolError
from ..toolchain import capture_windows_environment, environment_path, find_command, find_vsdevcmds
from .models import BootstrapError
from .preflight import PreflightError, check_cmake, compiler_environment_errors
from .toolchain_selection import DEFAULT_ENVIRONMENT_ARGUMENTS, ToolchainSelection, resolve_cmake_executable


T = TypeVar("T")


@dataclass(frozen=True)
class Candidate:
    path: Path
    source: str


def _unique(candidates: Sequence[Candidate]) -> tuple[Candidate, ...]:
    result: dict[str, Candidate] = {}
    for candidate in candidates:
        path = candidate.path.expanduser().resolve()
        result.setdefault(str(path).casefold(), Candidate(path, candidate.source))
    return tuple(result.values())


def visual_studio_candidates(command_io: CommandIO) -> tuple[Candidate, ...]:
    candidates: list[Candidate] = []
    try:
        candidates.extend(Candidate(path, "Visual Studio installation") for path in find_vsdevcmds(os.environ))
    except DevToolError as exc:
        command_io.out(f"Visual Studio discovery: {exc}")
    return _unique(candidates)


def _installation_root(script: Path) -> Path | None:
    if script.parent.name.casefold() == "tools" and script.parent.parent.name.casefold() == "common7":
        return script.parent.parent.parent
    return None


def cmake_candidates(script: Path, environment: Mapping[str, str]) -> tuple[Candidate, ...]:
    candidates: list[Candidate] = []
    installation = Path(environment["VSINSTALLDIR"]) if environment.get("VSINSTALLDIR") else _installation_root(script)
    if installation is not None:
        bundled = installation / "Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe"
        if bundled.is_file():
            candidates.append(Candidate(bundled, "selected Visual Studio"))
    for entry in environment_path(environment).split(os.pathsep):
        if entry:
            executable = Path(entry.strip('"')) / "cmake.exe"
            if executable.is_file():
                candidates.append(Candidate(executable, "PATH"))
    return _unique(candidates)


def _read(label: str) -> str:
    try:
        return input(f"{label}: ").strip()
    except EOFError as exc:
        raise BootstrapError("Interactive setup requires a terminal. Edit .agents/DevTool.user.json and run setup --non-interactive (or setup --check to inspect only).") from exc


def _path(value: str, *, directory: bool) -> Path:
    value = value.strip()
    if len(value) >= 2 and value[0] == value[-1] and value[0] in {'"', "'"}:
        value = value[1:-1]
    path = Path(os.path.expandvars(value)).expanduser().resolve()
    return path / "Common7/Tools/VsDevCmd.bat" if directory and path.is_dir() else path


def _select_component(
    label: str, discover: Callable[[], Sequence[Candidate]], validate: Callable[[Path], T],
    command_io: CommandIO, *, preferred: Candidate | None, interactive: bool,
    field: str, directory: bool = False,
) -> T:
    hint = f"Set {field} in .agents/DevTool.user.json; run setup --non-interactive (or setup --check to inspect only)."
    rejected = set()
    if preferred is not None:
        try:
            return validate(preferred.path)
        except DevToolError as exc:
            if not interactive:
                raise BootstrapError(f"Configured {label} is not usable: {exc}. {hint}") from exc
            command_io.out(f"Configured {label} is not usable: {exc}")
            rejected.add(str(preferred.path).casefold())
    available: list[tuple[Candidate, T]] = []
    for candidate in discover():
        if str(candidate.path).casefold() in rejected:
            continue
        try:
            available.append((candidate, validate(candidate.path)))
        except DevToolError as exc:
            command_io.out(f"Skipping {label} {candidate.path}: {exc}")
    if len(available) == 1:
        candidate, result = available[0]
        command_io.out(f"Automatically selected {label}: {candidate.path} [{candidate.source}]")
        return result
    if available:
        command_io.out(f"Usable {label} candidates:")
        for number, (candidate, _) in enumerate(available, 1):
            display = (_installation_root(candidate.path) or candidate.path) if directory else candidate.path
            command_io.out(f"  {number}  {display} [{candidate.source}]")
    if not interactive:
        reason = "Multiple usable candidates" if available else "No usable candidate found"
        raise BootstrapError(f"{label}: {reason}. {hint}")
    command_io.out(hint)
    while True:
        value = _read("Select a number or enter a path" if available else
                      "Visual Studio installation directory or VsDevCmd.bat path" if directory else "CMake executable path")
        if not value:
            continue
        if value.isdigit():
            if 1 <= int(value) <= len(available):
                return available[int(value) - 1][1]
            command_io.out("Choose a displayed number or enter a path.")
            continue
        try:
            return validate(_path(value, directory=directory))
        except (DevToolError, OSError, ValueError) as exc:
            command_io.out(f"{label} is not usable: {exc}")


def select_windows_toolchain(
    repository: RepositoryContext, command_io: CommandIO, *,
    configured_script: Path | None, configured_cmake: str,
    configured_arguments: Sequence[str], interactive: bool,
) -> ToolchainSelection:
    def validate_script(script: Path) -> tuple[Path, tuple[str, ...], dict[str, str]]:
        arguments = tuple(configured_arguments) if configured_arguments and (configured_script is None or script == configured_script) else DEFAULT_ENVIRONMENT_ARGUMENTS
        environment = capture_windows_environment(script, arguments)
        environment["VSLANG"] = "1033"
        if errors := compiler_environment_errors(environment):
            raise PreflightError("\n".join(errors))
        return script, tuple(arguments), environment

    script, arguments, environment = _select_component(
        "Visual Studio", lambda: visual_studio_candidates(command_io), validate_script, command_io,
        preferred=Candidate(configured_script, "saved configuration") if configured_script else None,
        interactive=interactive, field="toolchain.environmentScript", directory=True,
    )

    def validate_cmake(executable: Path) -> str:
        command = resolve_cmake_executable(str(executable), environment)
        if error := check_cmake(repository.root, environment=environment, command=command, repository_context=repository):
            raise PreflightError(error)
        return command

    override = os.environ.get("CMAKE_COMMAND", "") or configured_cmake
    preferred_cmake = None
    if override:
        preferred_cmake = Candidate(Path(find_command(override, environment) or override).expanduser().resolve(),
                                    "CMAKE_COMMAND" if os.environ.get("CMAKE_COMMAND") else "saved configuration")
    command = _select_component(
        "CMake", lambda: cmake_candidates(script, environment), validate_cmake, command_io,
        preferred=preferred_cmake, interactive=interactive, field="cmake.command (or correct CMAKE_COMMAND)",
    )
    command_io.out(f"Validated Visual Studio: {script}")
    command_io.out(f"Validated CMake: {command}")
    return ToolchainSelection(command, script, arguments, environment)
