"""Build-specific toolchain environment policy and context preparation."""

from __future__ import annotations

import json
import os
import subprocess
import tempfile
from pathlib import Path
from typing import Mapping, Sequence

from ..toolchain import (
    capture_setup_environment,
    environment_value,
    find_command,
    find_vsdevcmd,
)
from .build_context import BuildContext
from .config import (
    BuildPaths,
    BuildProfile,
    BuildToolError,
    EnvironmentProvider,
    EnvironmentSetup,
    default_build_paths,
    resolve_cmake_command,
    resolve_jobs,
)
from .locking import state_file_component
from .recovery import write_json_state

_VISUAL_STUDIO_CACHE_VERSION = 2


def visual_studio_environment_cache_path(
    profile: BuildProfile,
    *,
    root: Path | None = None,
) -> Path:
    root = root or default_build_paths().state_directory
    return root / f"{state_file_component(profile.name)}.visual-studio-environment.json"


def file_fingerprint(path: Path) -> dict[str, int] | None:
    try:
        stat = path.stat()
    except OSError:
        return None
    if not path.is_file():
        return None
    return {"size": stat.st_size, "mtimeNs": stat.st_mtime_ns}


def environment_changes(
    before: Mapping[str, str],
    after: Mapping[str, str],
) -> tuple[dict[str, str], list[str]]:
    before_normalized = {name.upper(): value for name, value in before.items()}
    after_normalized = {name.upper(): value for name, value in after.items()}
    updates = {
        name: value
        for name, value in after_normalized.items()
        if before_normalized.get(name) != value
    }
    removed = sorted(set(before_normalized) - set(after_normalized))
    return updates, removed


def apply_environment_changes(
    environment: Mapping[str, str],
    updates: Mapping[str, str],
    removed: Sequence[str],
) -> dict[str, str]:
    result = dict(environment)
    names = {name.upper(): name for name in result}
    for normalized_name in [*removed, *updates]:
        existing_name = names.get(normalized_name.upper())
        if existing_name is not None:
            result.pop(existing_name, None)
    result.update(updates)
    return result


def load_visual_studio_environment_cache(
    profile: BuildProfile,
    script: Path | None,
    arguments: Sequence[str],
    *,
    state_directory: Path | None = None,
) -> dict[str, str] | None:
    path = visual_studio_environment_cache_path(profile, root=state_directory)
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return None
    if not isinstance(value, dict) or value.get("version") != _VISUAL_STUDIO_CACHE_VERSION:
        return None
    script_value = value.get("script")
    compiler_value = value.get("compiler")
    if (
        not isinstance(script_value, str)
        or not script_value
        or not isinstance(compiler_value, str)
        or not compiler_value
    ):
        return None
    cached_script = Path(script_value)
    if script is not None and cached_script != script.resolve():
        return None
    if value.get("arguments") != list(arguments):
        return None
    script_fingerprint = file_fingerprint(cached_script)
    if script_fingerprint is None or value.get("scriptFingerprint") != script_fingerprint:
        return None
    compiler = Path(compiler_value)
    compiler_fingerprint = file_fingerprint(compiler)
    if compiler_fingerprint is None or value.get("compilerFingerprint") != compiler_fingerprint:
        return None
    updates = value.get("updates")
    removed = value.get("removed")
    path_prefix = value.get("pathPrefix", "")
    if not isinstance(updates, dict) or not all(
        isinstance(name, str) and isinstance(item, str) for name, item in updates.items()
    ):
        return None
    if not isinstance(removed, list) or not all(isinstance(name, str) for name in removed):
        return None
    if not isinstance(path_prefix, str):
        return None
    environment = apply_environment_changes(os.environ, updates, removed)
    if path_prefix:
        _, inherited_path = environment_value(os.environ, "PATH")
        environment = apply_environment_changes(
            environment,
            {"PATH": path_prefix + inherited_path},
            (),
        )
    environment["VSLANG"] = "1033"
    return environment


def write_visual_studio_environment_cache(
    profile: BuildProfile,
    script: Path,
    arguments: Sequence[str],
    before: Mapping[str, str],
    environment: Mapping[str, str],
    *,
    state_directory: Path | None = None,
) -> None:
    compiler = find_command("cl.exe", environment)
    if not compiler:
        return
    updates, removed = environment_changes(before, environment)
    _, before_path = environment_value(before, "PATH")
    _, after_path = environment_value(environment, "PATH")
    path_prefix = ""
    if before_path and after_path.casefold().endswith(before_path.casefold()):
        path_prefix = after_path[: -len(before_path)]
        updates.pop("PATH", None)
    try:
        write_json_state(
            visual_studio_environment_cache_path(profile, root=state_directory),
            {
                "version": _VISUAL_STUDIO_CACHE_VERSION,
                "script": str(script.resolve()),
                "arguments": list(arguments),
                "scriptFingerprint": file_fingerprint(script),
                "compiler": str(Path(compiler).resolve()),
                "compilerFingerprint": file_fingerprint(Path(compiler)),
                "updates": updates,
                "removed": removed,
                "pathPrefix": path_prefix,
            },
        )
    except OSError:
        return


def detect_msvc_showincludes_prefix(environment: Mapping[str, str]) -> str:
    compiler = find_command("cl.exe", environment)
    if not compiler:
        raise BuildToolError("MSVC cl.exe was not found after initializing the Visual Studio environment.")

    with tempfile.TemporaryDirectory(prefix="durin-showincludes-") as directory:
        root = Path(directory)
        header = root / "durin_showincludes_probe.h"
        source = root / "durin_showincludes_probe.c"
        object_file = root / "durin_showincludes_probe.obj"
        header.write_text("\n", encoding="utf-8")
        source.write_text(
            f'#include "{header.name}"\nint main(void) {{ return 0; }}\n',
            encoding="utf-8",
        )
        result = subprocess.run(
            [compiler, "/nologo", "/showIncludes", "/c", source.name, f"/Fo{object_file}"],
            cwd=root,
            env=dict(environment),
            capture_output=True,
            check=False,
        )
        output = (result.stdout + b"\n" + result.stderr).decode("mbcs", errors="replace")
        header_path = str(header)
        for line in output.splitlines():
            index = line.lower().find(header_path.lower())
            if index >= 0:
                return line[:index]

    raise BuildToolError(
        "MSVC /showIncludes prefix could not be detected. "
        "The compiler probe did not report its included header."
    )


def build_environment(
    profile: BuildProfile,
    environment_setup: EnvironmentSetup,
    *,
    current_host: str,
    paths: BuildPaths | None = None,
) -> dict[str, str]:
    paths = paths or default_build_paths()
    provider = profile.environment_provider
    configured_script = environment_setup.script
    configured_arguments = list(environment_setup.arguments)
    if provider is EnvironmentProvider.INHERIT and not configured_script:
        return dict(os.environ)
    if provider is EnvironmentProvider.VISUAL_STUDIO:
        if current_host != "windows":
            raise BuildToolError('The "visual-studio" environment provider is only supported on Windows.')
        arguments = configured_arguments or ["-arch=x64", "-host_arch=x64"]
        requested_script = Path(configured_script).expanduser().resolve() if configured_script else None
        cached = load_visual_studio_environment_cache(
            profile,
            requested_script,
            arguments,
            state_directory=paths.state_directory,
        )
        if cached is not None:
            return cached
        script = requested_script or find_vsdevcmd(os.environ)
        inherited_environment = dict(os.environ)
        environment = capture_setup_environment(
            script, arguments, current_host=current_host, cwd=paths.root
        )
        environment["VSLANG"] = "1033"
        showincludes_prefix = detect_msvc_showincludes_prefix(environment)
        if showincludes_prefix.strip().casefold() != "note: including file:":
            raise BuildToolError(
                "MSVC did not emit English diagnostics after VSLANG=1033. "
                "Install the English language pack for Visual Studio C++ tools, then rerun DevTool. "
                f"Detected /showIncludes prefix: {showincludes_prefix.strip()!r}"
            )
        write_visual_studio_environment_cache(
            profile,
            script,
            arguments,
            inherited_environment,
            environment,
            state_directory=paths.state_directory,
        )
        return environment
    if provider is EnvironmentProvider.SCRIPT or configured_script:
        if not configured_script:
            raise BuildToolError(
                f'Profile environment provider "{provider.value}" requires '
                "toolchain.environmentScript "
                'in ".agents/DevTool.user.json".'
            )
        return capture_setup_environment(
            Path(configured_script).expanduser(),
            configured_arguments,
            current_host=current_host,
            cwd=paths.root,
        )
    return dict(os.environ)


def ensure_required_commands(profile: BuildProfile, environment: dict[str, str]) -> None:
    path_name, search_path = environment_value(environment, "PATH")
    for command in profile.required_commands:
        if find_command(command, environment):
            continue
        if profile.environment_provider is EnvironmentProvider.VISUAL_STUDIO and command.lower() == "ninja":
            _, visual_studio_root = environment_value(environment, "VSINSTALLDIR")
            bundled_ninja = (
                Path(visual_studio_root)
                / "Common7"
                / "IDE"
                / "CommonExtensions"
                / "Microsoft"
                / "CMake"
                / "Ninja"
                / "ninja.exe"
            )
            if visual_studio_root and bundled_ninja.is_file():
                environment[path_name] = str(bundled_ninja.parent) + os.pathsep + search_path
                search_path = environment[path_name]
                continue
        raise BuildToolError(
            f'Required command "{command}" was not found for the selected build profile. '
            "Initialize it through toolchain.environmentScript "
            'in ".agents/DevTool.user.json".'
        )


def require_windows_long_paths_enabled() -> None:
    if os.name != "nt":
        return

    import winreg

    registry_key = r"SYSTEM\CurrentControlSet\Control\FileSystem"
    try:
        with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, registry_key) as key:
            value, value_type = winreg.QueryValueEx(key, "LongPathsEnabled")
        enabled = value_type == winreg.REG_DWORD and value == 1
    except OSError:
        enabled = False
    if not enabled:
        raise BuildToolError(
            r"Windows long-path support is required, but "
            r"HKLM\SYSTEM\CurrentControlSet\Control\FileSystem\LongPathsEnabled "
            r"is missing or is not REG_DWORD 1.",
            recovery=(
                "Enable Computer Configuration > Administrative Templates > System > Filesystem > "
                "Enable Win32 long paths, then restart Windows and rerun DevTool. "
                "DurinDevTool does not change machine policy."
            ),
        )


def prepare_toolchain_environment(context: BuildContext) -> None:
    if context.environment is not None:
        return
    require_windows_long_paths_enabled()
    paths = BuildPaths.from_repository(context.repository) if context.repository else default_build_paths()
    environment = build_environment(
        context.profile,
        context.config.environment_setup,
        current_host=context.current_host,
        paths=paths,
    )
    ensure_required_commands(context.profile, environment)
    context.environment = environment


def prepare_command_context(context: BuildContext) -> None:
    paths = BuildPaths.from_repository(context.repository) if context.repository else default_build_paths()
    context.cmake = resolve_cmake_command(
        context.request.cmake,
        context.config.cmake_command,
        environment=context.environment,
        local_config_file=paths.local_config_file,
    )
    context.jobs = resolve_jobs(context.request.jobs, context.config.jobs)


def prepare_toolchain_context(context: BuildContext) -> None:
    prepare_toolchain_environment(context)
    prepare_command_context(context)
