"""Child-process lifetime, output capture, and command logging."""

from __future__ import annotations

import locale
import os
import re
import signal
import subprocess
import threading
from collections import deque
from datetime import datetime
from pathlib import Path
from time import perf_counter, sleep
from typing import Any, Mapping, Sequence

from .config import REPO_ROOT, STATE_DIR, BuildToolError, BuildToolInterruptedError
from .locking import state_file_component
from .output import BuildOutput


COMMAND_LOG_LIMIT = 40
COMMAND_EXCERPT_LINE_LIMIT = 60
COMMAND_EXCERPT_CHARACTER_LIMIT = 24_000
DIAGNOSTIC_PATTERN = re.compile(
    r"(^FAILED:|fatal error|(?:^|[^a-z])error(?:[^a-z]|$)|"
    r"\bLNK\d{4}\b|\bninja: build stopped\b|\bCMake Error\b|"
    r"\[\s*FAILED\s*\]|assertion failed)",
    re.IGNORECASE,
)
ANSI_ESCAPE_PATTERN = re.compile(r"\x1b\[[0-?]*[ -/]*[@-~]")
TEST_SUMMARY_PATTERN = re.compile(r"^\[(?:=+|\s*PASSED\s*)\].*\btests?\b", re.IGNORECASE)


class CommandTranscript:
    def __init__(self) -> None:
        self.tail: deque[str] = deque(maxlen=COMMAND_EXCERPT_LINE_LIMIT)
        self.diagnostics: deque[str] = deque(maxlen=COMMAND_EXCERPT_LINE_LIMIT)

    def add(self, text: str) -> None:
        clean = ANSI_ESCAPE_PATTERN.sub("", text.rstrip("\r\n"))
        if not clean:
            return
        self.tail.append(clean)
        if DIAGNOSTIC_PATTERN.search(clean):
            self.diagnostics.append(clean)

    def excerpt(self) -> str:
        lines: list[str] = []
        seen: set[str] = set()
        for line in (*self.diagnostics, *self.tail):
            if line in seen:
                continue
            seen.add(line)
            lines.append(line)
        text = "\n".join(lines)
        if len(text) <= COMMAND_EXCERPT_CHARACTER_LIMIT:
            return text
        return "... excerpt truncated ...\n" + text[-COMMAND_EXCERPT_CHARACTER_LIMIT:]

    def success_summary(self) -> str:
        return "\n".join(line for line in self.tail if TEST_SUMMARY_PATTERN.search(line))


def command_log_path(command: Sequence[str], root: Path = STATE_DIR) -> Path:
    log_directory = root / "logs"
    executable = state_file_component(Path(command[0]).stem or "command")
    timestamp = datetime.now().strftime("%Y%m%d-%H%M%S-%f")
    return log_directory / f"{timestamp}-{os.getpid()}-{executable}.log"


def prune_command_logs(log_directory: Path, keep: int = COMMAND_LOG_LIMIT) -> None:
    try:
        logs = sorted(log_directory.glob("*.log"), key=lambda path: path.stat().st_mtime, reverse=True)
        for path in logs[keep:]:
            path.unlink(missing_ok=True)
    except OSError:
        # Log retention is best-effort and must not turn a successful build into a failure.
        return


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
    colorize_log_levels: bool = False,
    colorize_test_output: bool = False,
    recovery_required_on_interrupt: bool = True,
    interruption_message: str | None = None,
    timeout_seconds: int | None = None,
    wait_for_descendants: bool = False,
    show_heartbeat: bool = True,
) -> None:
    command_list = list(command)
    output.command(subprocess.list2cmdline(command_list))
    log_path = command_log_path(command_list)
    transcript = CommandTranscript()
    reader_error: list[OSError] = []
    popen_options: dict[str, Any] = {}
    if os.name == "nt":
        popen_options["creationflags"] = subprocess.CREATE_NEW_PROCESS_GROUP
    else:
        popen_options["start_new_session"] = True
    process_job = WindowsProcessJob() if wait_for_descendants and os.name == "nt" else None
    try:
        log_path.parent.mkdir(parents=True, exist_ok=True)
        log = log_path.open("w", encoding="utf-8", newline="")
    except OSError as exc:
        if process_job:
            process_job.close()
        raise BuildToolError(
            f'Could not capture command output in "{log_path}": {exc}',
            command=command_list,
            log_path=log_path,
        ) from exc
    try:
        process = subprocess.Popen(
            command_list,
            cwd=REPO_ROOT,
            env=dict(environment),
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            encoding="mbcs" if os.name == "nt" else locale.getpreferredencoding(False),
            errors="replace",
            bufsize=1,
            close_fds=True,
            **popen_options,
        )
    except OSError as exc:
        try:
            log.close()
        except OSError:
            pass
        if process_job:
            process_job.close()
        raise BuildToolError(f'Could not start command "{command_list[0]}": {exc}', command=command_list) from exc

    def drain_output() -> None:
        try:
            if process.stdout is None:
                return
            with process.stdout:
                for line in process.stdout:
                    if not reader_error:
                        try:
                            log.write(line)
                        except OSError as exc:
                            reader_error.append(exc)
                    transcript.add(line)
                    if not output.compact:
                        output.child_output(
                            line,
                            colorize_log_levels=colorize_log_levels,
                            colorize_test_output=colorize_test_output,
                        )
        finally:
            try:
                log.close()
            except OSError as exc:
                if not reader_error:
                    reader_error.append(exc)

    if process_job:
        try:
            process_job.assign(process)
        except BuildToolError:
            terminate_process_tree(process)
            try:
                log.close()
            except OSError:
                pass
            process_job.close()
            raise
    reader = threading.Thread(target=drain_output, name="DurinDevToolOutputReader", daemon=True)
    reader.start()
    try:
        started_at = perf_counter()
        deadline = perf_counter() + timeout_seconds if timeout_seconds else None
        while True:
            wait_seconds = 30.0
            if deadline is not None:
                remaining = deadline - perf_counter()
                if remaining <= 0:
                    if process_job:
                        process_job.terminate()
                    terminate_process_tree(process)
                    raise BuildToolError(
                        f'Command timed out after {timeout_seconds}s: "{command_list[0]}"',
                        command=command_list,
                        recovery="Inspect the output excerpt or full log, then rerun the same command.",
                    )
                wait_seconds = min(wait_seconds, remaining)
            try:
                return_code = process.wait(timeout=wait_seconds)
                break
            except subprocess.TimeoutExpired:
                if show_heartbeat:
                    output.info(f"Command is still running ({perf_counter() - started_at:.0f}s elapsed).")
        if process_job:
            # Relaunched editors inherit job membership, so active membership
            # reaches zero only after the final instance exits.
            process_job.wait()
    except BuildToolError as exc:
        reader.join()
        if output.compact:
            exc.output_excerpt = transcript.excerpt()
        exc.log_path = log_path
        raise
    except KeyboardInterrupt as exc:
        if process_job:
            process_job.terminate()
        terminate_process_tree(process)
        reader.join()
        excerpt = transcript.excerpt() if output.compact else ""
        if not recovery_required_on_interrupt:
            raise BuildToolError(
                interruption_message or "Application run was interrupted.",
                command=command_list,
                output_excerpt=excerpt,
                log_path=log_path,
            ) from exc
        raise BuildToolInterruptedError(
            interruption_message or "DurinDevTool was interrupted.",
            command=command_list,
            recovery=(
                "Confirm that the old build process tree has exited, then run DevTool status "
                "and use the recovery command it reports."
            ),
            output_excerpt=excerpt,
            log_path=log_path,
        ) from exc
    finally:
        reader.join()
        output.finish_child_output()
        if process_job:
            process_job.close()
        prune_command_logs(log_path.parent)
    if reader_error:
        raise BuildToolError(
            f'Could not capture command output in "{log_path}": {reader_error[0]}',
            command=command_list,
            log_path=log_path,
        )
    if return_code != 0:
        raise BuildToolError(
            f'Command failed: "{command_list[0]}"',
            command=command_list,
            exit_code=return_code,
            recovery="Inspect the output excerpt or full log, fix the reported error, and rerun the same command.",
            output_excerpt=transcript.excerpt() if output.compact else "",
            log_path=log_path,
        )
    if output.compact:
        if summary := transcript.success_summary():
            output.child_output(summary + "\n", colorize_test_output=colorize_test_output)
        output.info(f'Full output: "{log_path}"')
