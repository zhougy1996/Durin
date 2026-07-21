from __future__ import annotations

import json
import os
import shlex
import shutil
import signal
import subprocess
import tempfile
from dataclasses import replace
from datetime import datetime, timezone
from pathlib import Path
from time import perf_counter, sleep
from typing import Any, Callable, Mapping, Sequence

from .config import (
    LOCK_DIR,
    REPO_ROOT,
    STATE_DIR,
    Action,
    BuildContext,
    BuildProfile,
    BuildToolError,
    BuildToolInterruptedError,
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
    preset_install_directory,
    preset_output_configuration,
    resolve_cmake_command,
    resolve_jobs,
    select_preset,
    select_profile,
)
from .output import BuildOutput


def state_file_component(value: str) -> str:
    allowed = set("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.+-")
    return "".join(character if character in allowed else "_" for character in value)


def lock_file_path(root: Path = LOCK_DIR) -> Path:
    # Presets share final outputs and generated metadata, so ownership belongs to the checkout.
    return root / "checkout.lock"


def lock_is_owned(path: Path) -> bool:
    """Return whether another process currently holds the checkout lock."""
    try:
        handle = path.open("r+b")
    except FileNotFoundError:
        return False
    except OSError as exc:
        raise BuildToolError(f'Could not open BuildTool lock "{path}": {exc}') from exc
    try:
        if path.stat().st_size == 0:
            return False
        handle.seek(0)
        try:
            if os.name == "nt":
                import msvcrt

                msvcrt.locking(handle.fileno(), msvcrt.LK_NBLCK, 1)
            else:
                import fcntl

                fcntl.flock(handle.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError:
            return True
        if os.name == "nt":
            msvcrt.locking(handle.fileno(), msvcrt.LK_UNLCK, 1)
        else:
            fcntl.flock(handle.fileno(), fcntl.LOCK_UN)
        return False
    finally:
        handle.close()


def read_lock_metadata(path: Path) -> dict[str, Any]:
    """Read metadata without touching the locked ownership byte."""
    try:
        with path.open("rb") as handle:
            handle.seek(1)
            content = handle.read()
    except OSError as exc:
        raise BuildToolError(f'Could not read BuildTool lock "{path}": {exc}') from exc
    # Older lock files stored the opening JSON brace in the locked byte.
    for candidate in (content, b"{" + content):
        try:
            metadata = json.loads(candidate.decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError):
            continue
        if isinstance(metadata, dict):
            return metadata
    raise BuildToolError(f'BuildTool lock does not contain valid metadata: "{path}"')


def stop_active_operation() -> bool:
    """Stop the BuildTool process recorded in the checkout ownership lock."""
    lock_path = lock_file_path()
    if not lock_is_owned(lock_path):
        return False
    metadata = read_lock_metadata(lock_path)
    try:
        pid = int(metadata["pid"])
    except (ValueError, TypeError, KeyError) as exc:
        raise BuildToolError(f'BuildTool lock does not contain a valid process ID: "{lock_path}"') from exc
    if pid <= 0 or pid == os.getpid():
        raise BuildToolError(f'BuildTool lock contains an invalid process ID: {pid}')

    if os.name == "nt":
        result = subprocess.run(
            ["taskkill", "/PID", str(pid), "/T", "/F"],
            cwd=REPO_ROOT,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            check=False,
        )
        if result.returncode != 0:
            raise BuildToolError(
                f"Could not stop the active BuildTool process (PID {pid}). It may have already exited."
            )
    else:
        try:
            os.killpg(pid, signal.SIGTERM)
        except ProcessLookupError:
            raise BuildToolError(f"The active BuildTool process (PID {pid}) has already exited.")
    return True


def interruption_marker_path(preset: str, root: Path = STATE_DIR) -> Path:
    return root / f"{state_file_component(preset)}.interrupted.json"


def read_state_description(path: Path, *, locked: bool = False) -> str:
    try:
        value = read_lock_metadata(path) if locked else json.loads(path.read_text(encoding="utf-8"))
    except (BuildToolError, OSError, json.JSONDecodeError):
        return f'Existing state file: "{path}"'
    if not isinstance(value, dict):
        return f'Existing state file: "{path}"'
    fields = []
    for key, label in (
        ("pid", "PID"),
        ("profile", "profile"),
        ("preset", "preset"),
        ("action", "action"),
        ("target", "target"),
        ("startedAt", "started"),
    ):
        if value.get(key) not in (None, ""):
            fields.append(f"{label}={value[key]}")
    return ", ".join(fields) if fields else f'Existing state file: "{path}"'


def write_json_state(path: Path, value: Mapping[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(f"{path.name}.{os.getpid()}.tmp")
    temporary.write_text(json.dumps(dict(value), indent=2) + "\n", encoding="utf-8")
    os.replace(temporary, path)


class BuildToolLock:
    def __init__(self, path: Path, metadata: Mapping[str, Any]):
        self.path = path
        self.metadata = dict(metadata)
        self.handle: Any = None

    def __enter__(self) -> "BuildToolLock":
        self.path.parent.mkdir(parents=True, exist_ok=True)
        self.handle = self.path.open("a+b")
        if self.path.stat().st_size == 0:
            self.handle.write(b"\0")
            self.handle.flush()
        self.handle.seek(0)
        try:
            if os.name == "nt":
                import msvcrt

                msvcrt.locking(self.handle.fileno(), msvcrt.LK_NBLCK, 1)
            else:
                import fcntl

                fcntl.flock(self.handle.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError as exc:
            self.handle.close()
            self.handle = None
            raise BuildToolError(
                "Another Durin BuildTool operation already owns this checkout. "
                + read_state_description(self.path, locked=True)
            ) from exc
        self.handle.seek(0)
        # Byte zero is reserved for ownership so other processes can read the JSON while it is locked.
        self.handle.truncate()
        self.handle.write(b"\0")
        self.handle.write((json.dumps(self.metadata, indent=2) + "\n").encode("utf-8"))
        self.handle.flush()
        return self

    def __exit__(self, exc_type: Any, exc: Any, traceback: Any) -> None:
        if self.handle is None:
            return
        try:
            self.handle.seek(0)
            if os.name == "nt":
                import msvcrt

                msvcrt.locking(self.handle.fileno(), msvcrt.LK_UNLCK, 1)
            else:
                import fcntl

                fcntl.flock(self.handle.fileno(), fcntl.LOCK_UN)
        finally:
            self.handle.close()
            self.handle = None


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
            'Set environmentSetup.script in ".agents/build-config.json".'
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
                "Install the English language pack for Visual Studio C++ tools, then rerun BuildTool. "
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
                f'Profile environment provider "{provider.value}" requires environmentSetup.script '
                'in ".agents/build-config.json".'
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
            'Initialize it through environmentSetup.script in ".agents/build-config.json".'
        )


def terminate_process_tree(process: subprocess.Popen[Any]) -> None:
    if process.poll() is not None:
        return
    if os.name == "nt":
        subprocess.run(
            ["taskkill", "/PID", str(process.pid), "/T", "/F"],
            cwd=REPO_ROOT,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            check=False,
        )
    else:
        try:
            os.killpg(process.pid, signal.SIGTERM)
        except ProcessLookupError:
            return
    try:
        process.wait(timeout=10)
        return
    except subprocess.TimeoutExpired:
        pass
    if os.name == "nt":
        process.kill()
    else:
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            return
    process.wait()


class WindowsProcessJob:
    """Keeps relaunched application processes in one waitable lifetime."""

    _JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE = 0x00002000
    _JOB_OBJECT_BASIC_ACCOUNTING_INFORMATION = 1
    _JOB_OBJECT_EXTENDED_LIMIT_INFORMATION = 9

    def __init__(self) -> None:
        import ctypes
        from ctypes import wintypes

        class BasicLimitInformation(ctypes.Structure):
            _fields_ = [
                ("PerProcessUserTimeLimit", ctypes.c_int64),
                ("PerJobUserTimeLimit", ctypes.c_int64),
                ("LimitFlags", wintypes.DWORD),
                ("MinimumWorkingSetSize", ctypes.c_size_t),
                ("MaximumWorkingSetSize", ctypes.c_size_t),
                ("ActiveProcessLimit", wintypes.DWORD),
                ("Affinity", ctypes.c_size_t),
                ("PriorityClass", wintypes.DWORD),
                ("SchedulingClass", wintypes.DWORD),
            ]

        class IoCounters(ctypes.Structure):
            _fields_ = [
                ("ReadOperationCount", ctypes.c_uint64),
                ("WriteOperationCount", ctypes.c_uint64),
                ("OtherOperationCount", ctypes.c_uint64),
                ("ReadTransferCount", ctypes.c_uint64),
                ("WriteTransferCount", ctypes.c_uint64),
                ("OtherTransferCount", ctypes.c_uint64),
            ]

        class ExtendedLimitInformation(ctypes.Structure):
            _fields_ = [
                ("BasicLimitInformation", BasicLimitInformation),
                ("IoInfo", IoCounters),
                ("ProcessMemoryLimit", ctypes.c_size_t),
                ("JobMemoryLimit", ctypes.c_size_t),
                ("PeakProcessMemoryUsed", ctypes.c_size_t),
                ("PeakJobMemoryUsed", ctypes.c_size_t),
            ]

        class BasicAccountingInformation(ctypes.Structure):
            _fields_ = [
                ("TotalUserTime", ctypes.c_int64),
                ("TotalKernelTime", ctypes.c_int64),
                ("ThisPeriodTotalUserTime", ctypes.c_int64),
                ("ThisPeriodTotalKernelTime", ctypes.c_int64),
                ("TotalPageFaultCount", wintypes.DWORD),
                ("TotalProcesses", wintypes.DWORD),
                ("ActiveProcesses", wintypes.DWORD),
                ("TotalTerminatedProcesses", wintypes.DWORD),
            ]

        self._ctypes = ctypes
        self._basic_accounting_type = BasicAccountingInformation
        self._kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        self._kernel32.CreateJobObjectW.argtypes = [ctypes.c_void_p, wintypes.LPCWSTR]
        self._kernel32.CreateJobObjectW.restype = wintypes.HANDLE
        self._kernel32.SetInformationJobObject.argtypes = [
            wintypes.HANDLE,
            ctypes.c_int,
            ctypes.c_void_p,
            wintypes.DWORD,
        ]
        self._kernel32.SetInformationJobObject.restype = wintypes.BOOL
        self._kernel32.AssignProcessToJobObject.argtypes = [wintypes.HANDLE, wintypes.HANDLE]
        self._kernel32.AssignProcessToJobObject.restype = wintypes.BOOL
        self._kernel32.QueryInformationJobObject.argtypes = [
            wintypes.HANDLE,
            ctypes.c_int,
            ctypes.c_void_p,
            wintypes.DWORD,
            ctypes.c_void_p,
        ]
        self._kernel32.QueryInformationJobObject.restype = wintypes.BOOL
        self._kernel32.TerminateJobObject.argtypes = [wintypes.HANDLE, wintypes.UINT]
        self._kernel32.TerminateJobObject.restype = wintypes.BOOL
        self._kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
        self._kernel32.CloseHandle.restype = wintypes.BOOL

        self.handle = self._kernel32.CreateJobObjectW(None, None)
        if not self.handle:
            raise BuildToolError(
                f"Could not create the application process job (Win32 error {ctypes.get_last_error()})."
            )
        limits = ExtendedLimitInformation()
        limits.BasicLimitInformation.LimitFlags = self._JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
        if not self._kernel32.SetInformationJobObject(
            self.handle,
            self._JOB_OBJECT_EXTENDED_LIMIT_INFORMATION,
            ctypes.byref(limits),
            ctypes.sizeof(limits),
        ):
            error = ctypes.get_last_error()
            self.close()
            raise BuildToolError(f"Could not configure the application process job (Win32 error {error}).")

    def assign(self, process: subprocess.Popen[Any]) -> None:
        if not self._kernel32.AssignProcessToJobObject(self.handle, int(process._handle)):
            raise BuildToolError(
                f"Could not track the application process tree (Win32 error {self._ctypes.get_last_error()})."
            )

    def wait(self) -> None:
        while True:
            accounting = self._basic_accounting_type()
            if not self._kernel32.QueryInformationJobObject(
                self.handle,
                self._JOB_OBJECT_BASIC_ACCOUNTING_INFORMATION,
                self._ctypes.byref(accounting),
                self._ctypes.sizeof(accounting),
                None,
            ):
                raise BuildToolError(
                    f"Could not query the application process job (Win32 error {self._ctypes.get_last_error()})."
                )
            if accounting.ActiveProcesses == 0:
                return
            sleep(0.05)

    def terminate(self) -> None:
        if self.handle:
            self._kernel32.TerminateJobObject(self.handle, 1)

    def close(self) -> None:
        if self.handle:
            self._kernel32.CloseHandle(self.handle)
            self.handle = None


def run_command(
    command: Sequence[str],
    *,
    environment: Mapping[str, str],
    output: BuildOutput,
    recovery_required_on_interrupt: bool = True,
    wait_for_descendants: bool = False,
) -> None:
    command_list = list(command)
    output.command(subprocess.list2cmdline(command_list))
    popen_options: dict[str, Any] = {}
    if os.name == "nt":
        popen_options["creationflags"] = subprocess.CREATE_NEW_PROCESS_GROUP
    else:
        popen_options["start_new_session"] = True
    process_job = WindowsProcessJob() if wait_for_descendants and os.name == "nt" else None
    try:
        process = subprocess.Popen(
            command_list,
            cwd=REPO_ROOT,
            env=dict(environment),
            stderr=subprocess.STDOUT,
            **popen_options,
        )
    except OSError as exc:
        if process_job:
            process_job.close()
        raise BuildToolError(f'Could not start command "{command_list[0]}": {exc}', command=command_list) from exc
    if process_job:
        try:
            process_job.assign(process)
        except BuildToolError:
            terminate_process_tree(process)
            process_job.close()
            raise
    try:
        return_code = process.wait()
        if process_job:
            # Relaunched editors inherit job membership, so active membership reaches zero only after the final instance exits.
            process_job.wait()
    except KeyboardInterrupt as exc:
        if process_job:
            process_job.terminate()
        terminate_process_tree(process)
        if not recovery_required_on_interrupt:
            raise BuildToolError("Application run was interrupted.", command=command_list) from exc
        raise BuildToolInterruptedError(
            "Durin BuildTool was interrupted.",
            command=command_list,
            recovery=(
                "Confirm that the old build process tree has exited, then run "
                "rebuild --target all with the affected preset."
            ),
        ) from exc
    finally:
        if process_job:
            process_job.close()
    if return_code != 0:
        raise BuildToolError(
            f'Command failed: "{command_list[0]}"',
            command=command_list,
            exit_code=return_code,
            recovery="Inspect the command output above, fix the reported error, and rerun the same command.",
        )


def validate_target(target: str, *, action: Action) -> None:
    if not target:
        raise BuildToolError(f"{action.value} requires --target <target-name>.")
    allowed = set("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.+-")
    if any(character not in allowed for character in target):
        raise BuildToolError(f'Build target contains unsupported characters: "{target}"')


def validate_request(request: CommandRequest, preset: ConfigurePreset) -> None:
    if request.action in {Action.BUILD, Action.TEST}:
        validate_target(request.target, action=request.action)
    if request.action is Action.REBUILD and request.target:
        validate_target(request.target, action=request.action)
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
    validate_request(request, preset)
    context = BuildContext(request, config, profile, presets, preset, current_host)
    if prepare_tools:
        context.cmake = resolve_cmake_command(request.cmake, config.cmake_command)
        context.jobs = resolve_jobs(request.jobs, config.jobs)
        context.environment = build_environment(
            profile,
            config.environment_setup,
            current_host=current_host,
        )
        ensure_required_commands(profile, context.environment)
    return context


def derive_context(
    base: BuildContext,
    request: CommandRequest,
) -> BuildContext:
    preset = select_preset(base.profile, base.presets, requested=request.preset)
    validate_request(request, preset)
    return BuildContext(
        request=request,
        config=base.config,
        profile=base.profile,
        presets=base.presets,
        preset=preset,
        current_host=base.current_host,
        cmake=base.cmake,
        jobs=request.jobs or base.jobs,
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


def restore_state_file(path: Path, previous_content: bytes | None) -> None:
    if previous_content is None:
        path.unlink(missing_ok=True)
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(f"{path.name}.{os.getpid()}.tmp")
    temporary.write_bytes(previous_content)
    os.replace(temporary, path)


def execute_with_recovery_marker(
    *,
    action: Action,
    marker_file: Path,
    metadata: Mapping[str, Any],
    operation: Callable[[], None],
) -> None:
    try:
        previous_content = marker_file.read_bytes()
    except FileNotFoundError:
        previous_content = None
    except OSError as exc:
        raise BuildToolError(f'Could not read BuildTool recovery state "{marker_file}": {exc}') from exc
    if previous_content is not None and action in {Action.BUILD, Action.TEST}:
        raise BuildToolError(
            "The previous Durin BuildTool operation did not return normally. "
            + read_state_description(marker_file),
            recovery="Confirm its old process tree has exited, then run rebuild --target all.",
        )
    write_json_state(marker_file, metadata)
    try:
        operation()
    except BuildToolInterruptedError:
        raise
    except BuildToolError:
        restore_state_file(marker_file, previous_content)
        raise
    except BaseException:
        raise
    else:
        if action is Action.REBUILD or previous_content is None:
            restore_state_file(marker_file, None)
        else:
            restore_state_file(marker_file, previous_content)


def runtime_executable_path(
    profile: BuildProfile,
    preset: ConfigurePreset,
    *,
    root: Path = REPO_ROOT,
) -> Path:
    runtime_profile = preset_cache_string(preset, "DURIN_PROFILE_NAME")
    return (
        root
        / "Engine"
        / "Binaries"
        / profile.platform
        / preset_output_configuration(preset)
        / "Runtime"
        / runtime_profile
        / f"{runtime_profile}{profile.test_executable_suffix}"
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
    runtime_profile = preset_cache_string(preset, "DURIN_PROFILE_NAME")
    return (
        REPO_ROOT
        / "Engine"
        / "Binaries"
        / profile.platform
        / preset_output_configuration(preset)
        / "Tests"
        / runtime_profile
        / "Bin"
        / f"{target}{profile.test_executable_suffix}"
    )


def perform_action(context: BuildContext, output: BuildOutput) -> None:
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

    target = context.target
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
    if request.action is Action.TEST:
        executable = test_executable_path(context.profile, context.preset, target)
        if not executable.is_file():
            raise BuildToolError(f'Test target "{target}" did not produce "{executable}".')
        command = [str(executable)]
        if request.test_filter:
            command.append(f"--gtest_filter={request.test_filter}")
        with output.stage("Test"):
            run_command(command, environment=environment, output=output)


def run_application(context: BuildContext, output: BuildOutput) -> None:
    executable = runtime_executable_path(context.profile, context.preset)
    if not executable.is_file():
        raise BuildToolError(
            f'Runtime executable was not found: "{executable}".',
            recovery="Build the complete runtime first with build --target all.",
        )
    with output.stage("Run"):
        run_command(
            [str(executable), *context.request.run_arguments],
            environment=os.environ,
            output=output,
            recovery_required_on_interrupt=False,
            wait_for_descendants=True,
        )


def workspace_project_roots(root: Path = REPO_ROOT) -> list[Path]:
    return sorted({descriptor.parent for descriptor in root.glob("*/*.dproject")})


def require_purge_child(path: Path, parent: Path) -> Path:
    resolved = path.resolve()
    try:
        relative = resolved.relative_to(parent.resolve())
    except ValueError as exc:
        raise BuildToolError(f'Purge path escapes its allowed root: "{resolved}"') from exc
    if not relative.parts:
        raise BuildToolError(f'Purge cannot remove an output root directly: "{resolved}"')
    return resolved


def collect_purge_paths(
    profile: BuildProfile,
    selected_presets: Sequence[ConfigurePreset],
    *,
    root: Path = REPO_ROOT,
) -> list[Path]:
    paths: set[Path] = set()
    output_configs: set[str] = set()
    intermediate_profiles: set[tuple[str, str, str]] = set()
    for preset in selected_presets:
        paths.add(require_purge_child(preset_build_directory(preset, root=root), root / "Build"))
        install_directory = preset_install_directory(preset, root=root)
        if install_directory is not None:
            paths.add(require_purge_child(install_directory, root / "Install"))
        identifier = preset_cache_string(preset, "DURIN_BUILD_IDENTIFIER", required=False)
        output_configs.add(preset_output_configuration(preset))
        intermediate_profiles.add(
            (
                f"Build-{identifier}" if identifier else "Build",
                profile.platform,
                preset_cache_string(preset, "DURIN_PROFILE_NAME"),
            )
        )
        paths.add(interruption_marker_path(preset.name, root / "Build" / ".agent-state"))
    for project_root in workspace_project_roots(root):
        for output_config in output_configs:
            paths.add(
                require_purge_child(
                    project_root / "Binaries" / profile.platform / output_config,
                    project_root / "Binaries",
                )
            )
        for intermediate_root, platform_name, runtime_profile in intermediate_profiles:
            paths.add(
                require_purge_child(
                    project_root / "Intermediate" / intermediate_root / platform_name / runtime_profile,
                    project_root / "Intermediate",
                )
            )
    return sorted(paths, key=lambda path: (len(path.parts), str(path).lower()), reverse=True)


def remove_purge_paths(paths: Sequence[Path], *, root: Path = REPO_ROOT) -> None:
    checkout_root = root.resolve()
    for path in paths:
        resolved = path.resolve()
        try:
            resolved.relative_to(checkout_root)
        except ValueError as exc:
            raise BuildToolError(f'Purge path escapes the checkout: "{resolved}"') from exc
        if resolved == checkout_root:
            raise BuildToolError("Purge cannot remove the checkout root.")
        try:
            if path.is_symlink() or path.is_file():
                path.unlink(missing_ok=True)
            elif path.is_dir():
                shutil.rmtree(path)
        except OSError as exc:
            raise BuildToolError(f'Could not purge build artifact path "{path}": {exc}') from exc


def execute_purge(
    context: BuildContext,
    output: BuildOutput,
    confirm: Callable[[Sequence[Path], bool], bool],
) -> None:
    selected = [context.preset]
    if context.request.all_presets:
        selected = [context.presets[name] for name in context.profile.presets]
    paths = [path for path in collect_purge_paths(context.profile, selected) if path.exists() or path.is_symlink()]
    if not paths:
        scope = "all registered presets" if context.request.all_presets else f'preset "{context.preset.name}"'
        output.warning(f"No build artifacts were found for {scope}.")
        return
    if not context.request.yes and not confirm(paths, context.request.all_presets):
        output.cancelled("Purge cancelled.")
        return
    with output.stage("Purge"):
        remove_purge_paths(paths)
    output.success(f"Purged {len(paths)} build artifact path(s).")


def execute_context(
    context: BuildContext,
    output: BuildOutput,
    *,
    confirm_purge: Callable[[Sequence[Path], bool], bool],
) -> float:
    started = perf_counter()
    output.context(context)
    metadata = operation_metadata(
        context,
        target=(
            "all-presets"
            if context.request.action is Action.PURGE and context.request.all_presets
            else context.target
        ),
    )
    with BuildToolLock(lock_file_path(), metadata):
        if context.request.action is Action.PURGE:
            execute_purge(context, output, confirm_purge)
        elif context.request.action is Action.RUN:
            run_application(context, output)
        else:
            execute_with_recovery_marker(
                action=context.request.action,
                marker_file=interruption_marker_path(context.preset.name),
                metadata=metadata,
                operation=lambda: perform_action(context, output),
            )
    elapsed = perf_counter() - started
    if context.request.action is not Action.PURGE:
        output.success(f"{context.request.action.value} completed in {elapsed:.2f}s.")
    return elapsed
