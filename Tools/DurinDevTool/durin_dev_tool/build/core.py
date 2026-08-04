"""Build context, toolchain setup, and command orchestration.

Extracted build services remain re-exported here for compatibility.
"""

from __future__ import annotations

import json
import os
import shlex
import shutil
import subprocess
import tempfile
from dataclasses import replace
from datetime import datetime, timezone
from pathlib import Path
from time import perf_counter
from typing import Any, Callable, Mapping, Sequence

from .config import (
    REPO_ROOT,
    REPOSITORY_CONFIG,
    STATE_DIR,
    Action,
    BuildContext,
    BuildProfile,
    BuildToolError,
    CommandRequest,
    ConfigurePreset,
    EnvironmentProvider,
    EnvironmentSetup,
    host_name,
    load_configure_presets,
    load_local_config,
    load_profiles,
    preset_build_directory,
    preset_cache_bool,
    preset_cache_string,
    resolve_cmake_command,
    resolve_jobs,
    select_preset,
    select_profile,
)
from .output import BuildOutput
from .locking import (
    BuildToolLock,
    inaccessible_lock_error,
    lock_acl_recovery,
    lock_file_path,
    lock_is_owned,
    normalize_windows_lock_acl,
    open_checkout_lock,
    read_lock_metadata,
    read_state_description,
    recover_inaccessible_windows_lock,
    state_file_component,
    stop_active_operation,
)
from .process import (
    ANSI_ESCAPE_PATTERN,
    COMMAND_EXCERPT_CHARACTER_LIMIT,
    COMMAND_EXCERPT_LINE_LIMIT,
    COMMAND_LOG_LIMIT,
    DIAGNOSTIC_PATTERN,
    TEST_SUMMARY_PATTERN,
    CommandTranscript,
    WindowsProcessJob,
    command_log_path,
    prune_command_logs,
    run_command,
    terminate_process_tree,
)
from .purge import (
    collect_purge_paths,
    execute_purge,
    remove_purge_paths,
    require_purge_child,
    workspace_project_roots,
)
from .recovery import (
    execute_with_recovery_marker,
    interruption_marker_path,
    recoverable_target,
    recovery_target,
    restore_state_file,
    write_json_state,
)
from .runtime import (
    ctest_command,
    run_all_native_tests,
    run_application,
    run_native_test,
    runtime_executable_path,
    test_executable_path,
)

ALL_NATIVE_TESTS_TARGET = "DurinNativeTests"


def parse_environment_output(output: str, *, case_insensitive: bool = False) -> dict[str, str]:
    environment: dict[str, str] = {}
    for entry in output.replace("\r\n", "\n").split("\0" if "\0" in output else "\n"):
        if "=" not in entry:
            continue
        name, value = entry.split("=", 1)
        if name:
            environment[name] = value
    if not case_insensitive:
        return environment
    normalized: dict[str, str] = {}
    for name, value in environment.items():
        normalized_name = name.upper()
        if normalized_name not in normalized or name == normalized_name:
            normalized[normalized_name] = value
    return normalized


def capture_setup_environment(
    script: Path,
    arguments: Sequence[str],
    *,
    current_host: str,
) -> dict[str, str]:
    if not script.is_file():
        raise BuildToolError(f'Environment setup script does not exist: "{script}"')
    if current_host == "windows":
        if script.suffix.lower() not in {".bat", ".cmd"}:
            raise BuildToolError("Windows environment setup scripts must use the .bat or .cmd extension.")
        command = [
            os.environ.get("COMSPEC", "cmd.exe"),
            "/d",
            "/s",
            "/c",
            "call",
            str(script),
            *arguments,
            ">nul",
            "&&",
            "set",
        ]
    else:
        argument_text = " ".join(shlex.quote(item) for item in [str(script), *arguments])
        command = ["/bin/sh", "-c", f". {argument_text} >/dev/null && env -0"]
    result = subprocess.run(command, cwd=REPO_ROOT, capture_output=True, text=True, check=False)
    if result.returncode != 0:
        details = result.stderr.strip()
        raise BuildToolError(
            f'Environment setup script failed with exit code {result.returncode}: "{script}"'
            + (f"\n{details}" if details else ""),
            command=command,
            exit_code=result.returncode,
        )
    return parse_environment_output(result.stdout, case_insensitive=current_host == "windows")


def find_vsdevcmd(environment: Mapping[str, str] | None = None) -> Path:
    environment = os.environ if environment is None else environment
    candidates = [
        Path(root) / "Microsoft Visual Studio" / "Installer" / "vswhere.exe"
        for variable in ("ProgramFiles(x86)", "ProgramFiles")
        if (root := environment.get(variable))
    ]
    vswhere = next((candidate for candidate in candidates if candidate.is_file()), None)
    if vswhere is None:
        raise BuildToolError(
            "Visual Studio environment could not be detected because vswhere.exe was not found. "
            'Set toolchain.environmentScript in ".agents/DevTool.user.json".'
        )
    command = [
        str(vswhere),
        "-latest",
        "-products",
        "*",
        "-requires",
        "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
        "-property",
        "installationPath",
    ]
    result = subprocess.run(command, capture_output=True, text=True, check=False)
    installation_path = result.stdout.strip()
    if result.returncode != 0 or not installation_path:
        raise BuildToolError(
            "vswhere.exe could not find a Visual Studio installation with the C++ toolchain.",
            command=command,
            exit_code=result.returncode,
        )
    script = Path(installation_path) / "Common7" / "Tools" / "VsDevCmd.bat"
    if not script.is_file():
        raise BuildToolError(f'Visual Studio environment script does not exist: "{script}"')
    return script


_VISUAL_STUDIO_CACHE_VERSION = 2


def visual_studio_environment_cache_path(profile: BuildProfile, *, root: Path = STATE_DIR) -> Path:
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
) -> dict[str, str] | None:
    path = visual_studio_environment_cache_path(profile)
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
) -> None:
    compiler = shutil.which("cl.exe", path=environment.get("PATH", ""))
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
            visual_studio_environment_cache_path(profile),
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
        # Cache persistence is an optimization and must never block a build.
        return


def build_environment(
    profile: BuildProfile,
    environment_setup: EnvironmentSetup,
    *,
    current_host: str,
) -> dict[str, str]:
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
        cached = load_visual_studio_environment_cache(profile, requested_script, arguments)
        if cached is not None:
            return cached
        script = requested_script or find_vsdevcmd()
        inherited_environment = dict(os.environ)
        environment = capture_setup_environment(script, arguments, current_host=current_host)
        # VsDevCmd may clear the wrapper's inherited value, so enforce the language
        # in the final environment shared by configure and build subprocesses.
        environment["VSLANG"] = "1033"
        showincludes_prefix = detect_msvc_showincludes_prefix(environment)
        if showincludes_prefix.strip().casefold() != "note: including file:":
            raise BuildToolError(
                "MSVC did not emit English diagnostics after VSLANG=1033. "
                "Install the English language pack for Visual Studio C++ tools, then rerun DevTool. "
                f"Detected /showIncludes prefix: {showincludes_prefix.strip()!r}"
            )
        # VsDevCmd and the compiler probe dominate BuildTool startup. Cache only
        # the script's environment delta so unrelated caller variables stay live.
        write_visual_studio_environment_cache(
            profile,
            script,
            arguments,
            inherited_environment,
            environment,
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
        )
    return dict(os.environ)


def detect_msvc_showincludes_prefix(environment: Mapping[str, str]) -> str:
    compiler = shutil.which("cl.exe", path=environment.get("PATH", ""))
    if not compiler:
        raise BuildToolError("MSVC cl.exe was not found after initializing the Visual Studio environment.")

    with tempfile.TemporaryDirectory(prefix="durin-showincludes-") as directory:
        root = Path(directory)
        header = root / "durin_showincludes_probe.h"
        source = root / "durin_showincludes_probe.c"
        object_file = root / "durin_showincludes_probe.obj"
        header.write_text("\n", encoding="utf-8")
        source.write_text(f'#include "{header.name}"\nint main(void) {{ return 0; }}\n', encoding="utf-8")
        result = subprocess.run(
            [compiler, "/nologo", "/showIncludes", "/c", source.name, f"/Fo{object_file}"],
            cwd=root,
            env=dict(environment),
            capture_output=True,
            check=False,
        )

        # Redirected MSVC diagnostics use the Windows ANSI code page, independently
        # of the terminal encoding used by an interactive caller or an Agent pipe.
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


def environment_value(environment: Mapping[str, str], name: str) -> tuple[str, str]:
    for existing_name, value in environment.items():
        if existing_name.lower() == name.lower():
            return existing_name, value
    return name, ""


def ensure_required_commands(profile: BuildProfile, environment: dict[str, str]) -> None:
    path_name, search_path = environment_value(environment, "PATH")
    for command in profile.required_commands:
        if shutil.which(command, path=search_path):
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
            'Initialize it through toolchain.environmentScript '
            'in ".agents/DevTool.user.json".'
        )


def validate_target(target: str, *, action: Action) -> None:
    if not target:
        raise BuildToolError(f"{action.value} requires --target <target-name>.")
    allowed = set("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.+-")
    if any(character not in allowed for character in target):
        raise BuildToolError(f'Build target contains unsupported characters: "{target}"')


def normalize_run_request(
    request: CommandRequest,
    *,
    preset: ConfigurePreset | None = None,
    root: Path = REPO_ROOT,
) -> CommandRequest:
    if request.action is not Action.RUN:
        return request

    project_selectors = [
        argument
        for argument in request.run_arguments
        if argument == "--project" or argument.startswith("--project=")
    ]
    project_path = request.project_path
    if (
        project_path is None
        and not project_selectors
        and preset is not None
        and preset_cache_string(preset, "DURIN_RUNTIME_VARIANT") == "DurinGame"
    ):
        project_path = REPOSITORY_CONFIG.paths.default_game_project
    if project_path is None:
        return request

    if not project_path.is_absolute():
        project_path = root / project_path
    project_path = project_path.resolve()
    if project_path.suffix.casefold() != ".dproject":
        raise BuildToolError(
            f'Project descriptor must use the .dproject extension: "{project_path}".'
        )
    if not project_path.is_file():
        raise BuildToolError(f'Project descriptor was not found: "{project_path}".')

    if project_selectors:
        raise BuildToolError(
            "run accepts project selection either through --project or through "
            "--args, but not both."
        )
    return request.with_project_path(project_path)


def validate_request(request: CommandRequest, preset: ConfigurePreset) -> None:
    if request.action in {Action.BUILD, Action.TEST}:
        validate_target(request.target, action=request.action)
    if request.action is Action.REBUILD and request.target:
        validate_target(request.target, action=request.action)
    if (
        request.action is Action.TEST
        and request.target.casefold() == "all"
        and request.test_filter
    ):
        raise BuildToolError(
            "--filter requires a single native test target and cannot be used with "
            "--target all."
        )
    if (
        request.action is Action.TEST
        and request.target.casefold() != "all"
        and (
            request.test_schedule_random
            or request.test_output_junit is not None
            or request.test_ctest_regex
            or request.test_include_direct
        )
    ):
        raise BuildToolError(
            "--schedule-random, --output-junit, --ctest-regex, and --include-direct "
            "require --target all."
        )
    if request.action is Action.TEST and not preset_cache_bool(preset, "BUILD_TESTING"):
        raise BuildToolError(f'Preset "{preset.name}" does not enable BUILD_TESTING.')
    if request.action is not Action.PURGE and (request.all_presets or request.yes):
        raise BuildToolError("--all-presets and --yes are only valid with purge.")
    if request.action is not Action.CONFIGURE and request.fresh:
        raise BuildToolError("--fresh is only valid with configure.")


def create_context(
    request: CommandRequest,
    *,
    prepare_tools: bool = True,
) -> BuildContext:
    config = load_local_config()
    if request.environment_setup:
        config = config.with_environment_script(request.environment_setup)
    profiles = load_profiles()
    current_host = host_name()
    profile = select_profile(
        profiles,
        requested=request.profile,
        configured=config.default_build_profile,
        current_host=current_host,
    )
    presets = load_configure_presets()
    preset = select_preset(profile, presets, requested=request.preset)
    request = normalize_run_request(request, preset=preset)
    validate_request(request, preset)
    context = BuildContext(request, config, profile, presets, preset, current_host)
    if prepare_tools:
        prepare_toolchain_context(context)
    return context


def prepare_toolchain_context(context: BuildContext) -> None:
    prepare_toolchain_environment(context)
    prepare_command_context(context)


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
    environment = build_environment(
        context.profile,
        context.config.environment_setup,
        current_host=context.current_host,
    )
    ensure_required_commands(context.profile, environment)
    context.environment = environment


def prepare_command_context(context: BuildContext) -> None:
    cmake = resolve_cmake_command(
        context.request.cmake,
        context.config.cmake_command,
        environment=context.environment,
    )
    jobs = resolve_jobs(context.request.jobs, context.config.jobs)
    context.cmake = cmake
    context.jobs = jobs


def derive_context(
    base: BuildContext,
    request: CommandRequest,
) -> BuildContext:
    preset = select_preset(base.profile, base.presets, requested=request.preset)
    request = normalize_run_request(request, preset=preset)
    validate_request(request, preset)
    return BuildContext(
        request=request,
        config=base.config,
        profile=base.profile,
        presets=base.presets,
        preset=preset,
        current_host=base.current_host,
        cmake=base.cmake,
        jobs=base.jobs if request.jobs is None else request.jobs,
        environment=base.environment,
    )


def operation_metadata(context: BuildContext, *, target: str | None = None) -> dict[str, Any]:
    return {
        "pid": os.getpid(),
        "profile": context.profile.name,
        "preset": context.preset.name,
        "action": context.request.action.value,
        "target": context.target if target is None else target,
        "startedAt": datetime.now(timezone.utc).isoformat(),
    }


def cache_is_usable(cache_file: Path) -> bool:
    if not cache_file.is_file():
        return False
    try:
        content = cache_file.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return False
    return "CMAKE_MAKE_PROGRAM:FILEPATH=CMAKE_MAKE_PROGRAM-NOTFOUND" not in content


def ninja_uses_english_msvc_prefix(build_directory: Path) -> bool:
    rules_file = build_directory / "CMakeFiles" / "rules.ninja"
    try:
        content = rules_file.read_bytes().lower()
    except OSError:
        return False
    return b"msvc_deps_prefix = note: including file:" in content


def require_english_msvc_ninja_prefix(context: BuildContext, build_directory: Path) -> None:
    if (
        context.current_host == "windows"
        and context.profile.environment_provider is EnvironmentProvider.VISUAL_STUDIO
        and not ninja_uses_english_msvc_prefix(build_directory)
    ):
        raise BuildToolError(
            "CMake did not generate Ninja rules with the English MSVC /showIncludes prefix.",
            recovery="Confirm the Visual Studio English language pack is installed, then run configure again.",
        )


def cmake_build_target(context: BuildContext) -> str:
    if context.request.action is Action.TEST and context.target == "all":
        return ALL_NATIVE_TESTS_TARGET
    return context.target


def perform_action(
    context: BuildContext,
    output: BuildOutput,
    *,
    target_override: str | None = None,
) -> None:
    request = context.request
    environment = context.environment or os.environ
    build_directory = preset_build_directory(context.preset)
    cache_file = build_directory / "CMakeCache.txt"

    if request.action is Action.CONFIGURE:
        fresh = request.fresh or (cache_file.exists() and not cache_is_usable(cache_file))
        command = [context.cmake]
        if fresh:
            command.append("--fresh")
        command.extend(["--preset", context.preset.name])
        with output.stage("Configure"):
            run_command(
                command,
                environment=environment,
                output=output,
            )
        require_english_msvc_ninja_prefix(context, build_directory)
        return
    if request.action is Action.CLEAN:
        if not cache_is_usable(cache_file):
            output.warning(f'Build tree is already clean or unconfigured: "{build_directory}"')
            return
        with output.stage("Clean"):
            run_command(
                [context.cmake, "--build", str(build_directory), "--target", "clean"],
                environment=environment,
                output=output,
            )
        return

    target = target_override or cmake_build_target(context)
    if request.action is Action.REBUILD:
        if cache_is_usable(cache_file):
            with output.stage("Clean"):
                run_command(
                    [context.cmake, "--build", str(build_directory), "--target", "clean"],
                    environment=environment,
                    output=output,
                )
        else:
            output.warning(f'Skipping clean because the build tree is unconfigured: "{build_directory}"')
        with output.stage("Configure"):
            run_command(
                [context.cmake, "--fresh", "--preset", context.preset.name],
                environment=environment,
                output=output,
            )
        require_english_msvc_ninja_prefix(context, build_directory)
    elif not cache_is_usable(cache_file) or (
        context.current_host == "windows"
        and context.profile.environment_provider is EnvironmentProvider.VISUAL_STUDIO
        and not ninja_uses_english_msvc_prefix(build_directory)
    ):
        if cache_is_usable(cache_file):
            output.warning("Existing Ninja rules do not use the English MSVC dependency prefix; reconfiguring.")
        command = [context.cmake]
        # Avoid a redundant --fresh for a new tree, but discard unusable or incompatible state.
        if cache_file.exists():
            command.append("--fresh")
        command.extend(["--preset", context.preset.name])
        with output.stage("Configure"):
            run_command(
                command,
                environment=environment,
                output=output,
            )
        require_english_msvc_ninja_prefix(context, build_directory)

    with output.stage("Build"):
        run_command(
            [context.cmake, "--build", str(build_directory), "--target", target, "-j", str(context.jobs)],
            environment=environment,
            output=output,
        )


def execute_context(
    context: BuildContext,
    output: BuildOutput,
    *,
    confirm_purge: Callable[[Sequence[Path], bool], bool],
) -> float:
    started = perf_counter()
    output.context(context)
    marker_file = interruption_marker_path(context.preset.name)
    lock_metadata = operation_metadata(
        context,
        target=(
            "all-presets"
            if context.request.action is Action.PURGE and context.request.all_presets
            else "recorded-target"
            if context.request.action is Action.RECOVER
            else context.target
        ),
    )
    with BuildToolLock(lock_file_path(), lock_metadata):
        if context.request.action is Action.PURGE:
            execute_purge(context, output, confirm_purge)
        elif context.request.action is Action.RUN:
            run_application(context, output)
        else:
            metadata = operation_metadata(
                context,
                target=cmake_build_target(context),
            )
            execute_with_recovery_marker(
                action=context.request.action,
                marker_file=marker_file,
                metadata=metadata,
                operation=lambda target_override: perform_action(
                    context,
                    output,
                    target_override=target_override,
                ),
            )
            # Native tests only read completed build outputs. Their assertion failures,
            # hangs, and interruptions must not poison incremental build state.
            if context.request.action is Action.TEST:
                if context.target == "all":
                    run_all_native_tests(context, output)
                else:
                    run_native_test(context, output)
    elapsed = perf_counter() - started
    if context.request.action is not Action.PURGE:
        output.success(f"{context.request.action.value} completed in {elapsed:.2f}s.")
    return elapsed
