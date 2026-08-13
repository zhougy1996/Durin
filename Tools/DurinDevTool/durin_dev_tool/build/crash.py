"""Discovery and offline analysis for Durin native-crash artifacts."""

from __future__ import annotations

import os
import re
import subprocess
from dataclasses import dataclass
from datetime import datetime, timedelta, timezone
from pathlib import Path
from typing import Mapping, Sequence

from .config import default_build_paths
from .process import command_log_path, prune_command_logs


KNOWN_WINDOWS_EXCEPTIONS = {
    0xC0000005: "EXCEPTION_ACCESS_VIOLATION",
    0xC000001D: "EXCEPTION_ILLEGAL_INSTRUCTION",
    0xC000008C: "EXCEPTION_ARRAY_BOUNDS_EXCEEDED",
    0xC000008D: "EXCEPTION_FLT_DENORMAL_OPERAND",
    0xC000008E: "EXCEPTION_FLT_DIVIDE_BY_ZERO",
    0xC0000094: "EXCEPTION_INT_DIVIDE_BY_ZERO",
    0xC0000096: "EXCEPTION_PRIV_INSTRUCTION",
    0xC00000FD: "EXCEPTION_STACK_OVERFLOW",
    0xE0000001: "DURIN_STD_TERMINATE",
}
CONTEXT_NAME_PATTERN = re.compile(r"-CrashContext-v(?P<version>\d+)\.txt$")
TIMESTAMP_FORMAT = "%Y%m%dT%H%M%S.%fZ"
REQUIRED_CONTEXT_KEYS = {
    "FormatVersion",
	"CompletionState",
    "CrashId",
    "ReasonCode",
	"ReasonKind",
	"ExceptionAddress",
    "ProcessId",
    "FaultingThreadId",
    "RuntimeVariant",
	"BuildConfiguration",
	"BuildIdentity",
	"ExecutableImagePath",
    "UtcTimestamp",
	"ProcessUptimeMicroseconds",
    "ProcessPhase",
	"BreadcrumbWriteSequence",
	"BreadcrumbFirstSequence",
	"BreadcrumbCount",
	"ActiveLogPath",
	"LastAcceptedLogSequence",
	"LastProcessedLogSequence",
	"LastDurableLogSequence",
    "DumpPath",
    "DumpResult",
	"DirectoryError",
	"ContextError",
	"DumpError",
	"AccessViolationOperation",
	"AccessViolationAddress",
}


def unsigned_windows_status(status: int) -> int:
    return status & 0xFFFFFFFF


def format_windows_status(status: int) -> str:
    unsigned = unsigned_windows_status(status)
    name = KNOWN_WINDOWS_EXCEPTIONS.get(unsigned, "UNKNOWN_NATIVE_STATUS")
    return f"0x{unsigned:08X} ({name})"


@dataclass(frozen=True)
class CrashArtifact:
    directory: Path
    context_path: Path
    dump_path: Path | None
    complete: bool
    values: Mapping[str, str]
    breadcrumbs: tuple[str, ...]
    diagnostic: str = ""


@dataclass(frozen=True)
class CrashAnalysis:
    log_path: Path | None
    excerpt: str
    diagnostic: str
    command: tuple[str, ...] = ()


def parse_crash_context(path: Path) -> tuple[dict[str, str], tuple[str, ...], str]:
    match = CONTEXT_NAME_PATTERN.search(path.name)
    if match is None:
        return {}, (), "Crash context filename has no recognized version."
    if int(match.group("version")) != 1:
        return {}, (), f"Crash context version {match.group('version')} is not supported."
    try:
        text = path.read_text(encoding="utf-8")
    except (OSError, UnicodeError) as error:
        return {}, (), f"Crash context could not be read: {error}"
    values: dict[str, str] = {}
    breadcrumbs: list[str] = []
    malformed = 0
    for line in text.splitlines():
        if not line:
            continue
        if "=" not in line:
            malformed += 1
            continue
        key, value = line.split("=", 1)
        if key == "Breadcrumb":
            breadcrumbs.append(value)
        elif key and key not in values:
            values[key] = value
    missing = sorted(REQUIRED_CONTEXT_KEYS - values.keys())
    diagnostics: list[str] = []
    if missing:
        diagnostics.append("missing required keys: " + ", ".join(missing))
    if malformed:
        diagnostics.append(f"ignored {malformed} malformed line(s)")
    return values, tuple(breadcrumbs), "; ".join(diagnostics)


def _context_timestamp(values: Mapping[str, str]) -> datetime | None:
    try:
        return datetime.strptime(values["UtcTimestamp"], TIMESTAMP_FORMAT).replace(tzinfo=timezone.utc)
    except (KeyError, ValueError):
        return None


def discover_current_crash(
    executable: Path,
    runtime_variant: str,
    process_id: int | None,
    started_at_utc: datetime | None,
    ended_at_utc: datetime | None,
) -> CrashArtifact | None:
    roots = (executable.parent / "Saved" / "Crashes", executable.parent / "Crashes")
    candidates: list[tuple[datetime, CrashArtifact]] = []
    lower = (started_at_utc or datetime.now(timezone.utc)) - timedelta(seconds=2)
    upper = (ended_at_utc or datetime.now(timezone.utc)) + timedelta(seconds=2)
    for root in roots:
        try:
            directories = tuple(root.iterdir())
        except OSError:
            continue
        for directory in directories:
            if not directory.is_dir() or directory.is_symlink() or not directory.name.startswith(runtime_variant + "-"):
                continue
            contexts = tuple(directory.glob("*-CrashContext-v*.txt"))
            if len(contexts) != 1:
                continue
            values, breadcrumbs, diagnostic = parse_crash_context(contexts[0])
            timestamp = _context_timestamp(values)
            if timestamp is None or not (lower <= timestamp <= upper):
                continue
            try:
                artifact_pid = int(values.get("ProcessId", "-1"))
            except ValueError:
                continue
            if process_id is not None and artifact_pid != process_id:
                continue
            if values.get("RuntimeVariant") != runtime_variant:
                continue
            dump_text = values.get("DumpPath", "")
            dump_path = Path(dump_text) if dump_text and dump_text != "Unavailable" else None
            if dump_path is not None and dump_path.parent.resolve() != directory.resolve():
                diagnostic = "; ".join(filter(None, (diagnostic, "dump path escapes the crash directory")))
                dump_path = None
            artifact = CrashArtifact(
                directory=directory,
                context_path=contexts[0],
                dump_path=dump_path,
                complete=(directory / "Complete.marker").is_file(),
                values=values,
                breadcrumbs=breadcrumbs,
                diagnostic=diagnostic,
            )
            candidates.append((timestamp, artifact))
    return max(candidates, key=lambda item: item[0])[1] if candidates else None


def discover_cdb(environment: Mapping[str, str] | None = None) -> Path | None:
    environment = environment or os.environ
    explicit = environment.get("DURIN_CDB_PATH", "")
    if explicit:
        candidate = Path(explicit)
        return candidate if candidate.is_file() else None
    program_files_x86 = environment.get("ProgramFiles(x86)", r"C:\Program Files (x86)")
    debugger_root = Path(program_files_x86) / "Windows Kits" / "10" / "Debuggers" / "x64"
    candidate = debugger_root / "cdb.exe"
    return candidate if candidate.is_file() else None


def manual_cdb_command(cdb: Path | str, artifact: CrashArtifact, binary_directory: Path) -> tuple[str, ...]:
    return (
        str(cdb),
        "-z",
        str(artifact.dump_path),
        "-y",
        str(binary_directory),
        "-c",
		".lines -e; .ecxr; .exr -1; ln @rip; kpn 40; !analyze -v; q",
    )


def _bounded_debugger_excerpt(text: str, maximum_lines: int = 50, maximum_characters: int = 12000) -> str:
    lines = [line.rstrip() for line in text.splitlines() if line.strip()]
    interesting = [line for line in lines if any(token in line.lower() for token in (
        "exception_code", "exceptionaddress", "durin", "!analyze", "stack_text", "warning", "error"))]
    selected = interesting[:maximum_lines] if interesting else lines[-maximum_lines:]
    excerpt = "\n".join(selected)
    return excerpt[:maximum_characters]


def analyze_crash(
    artifact: CrashArtifact,
    binary_directory: Path,
    *,
    cdb: Path | None = None,
    environment: Mapping[str, str] | None = None,
    timeout_seconds: int = 120,
    state_dir: Path | None = None,
) -> CrashAnalysis:
    state_dir = state_dir or default_build_paths().state_directory
    if artifact.dump_path is None or not artifact.dump_path.is_file():
        return CrashAnalysis(None, "", "Minidump is missing; context remains available.")
    cdb = cdb or discover_cdb(environment)
    manual = manual_cdb_command(cdb or "cdb.exe", artifact, binary_directory)
    if cdb is None:
        return CrashAnalysis(None, "", "CDB was not found. Manual command: " + subprocess.list2cmdline(manual), manual)
    command = manual_cdb_command(cdb, artifact, binary_directory)
    log_path = command_log_path(command, root=state_dir)
    try:
        completed = subprocess.run(
            command,
            cwd=binary_directory,
            env=dict(environment or os.environ),
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            encoding="utf-8",
            errors="replace",
            timeout=timeout_seconds,
            check=False,
        )
        log_path.parent.mkdir(parents=True, exist_ok=True)
        log_path.write_text(completed.stdout, encoding="utf-8")
        prune_command_logs(log_path.parent)
    except subprocess.TimeoutExpired as error:
        return CrashAnalysis(None, "", f"CDB analysis timed out after {timeout_seconds}s.", command)
    except OSError as error:
        return CrashAnalysis(None, "", f"CDB analysis failed to start: {error}", command)
    lowered = completed.stdout.lower()
    if "mismatched pdb" in lowered or "pdb does not match" in lowered:
        diagnostic = "CDB reported missing or mismatched PDBs; module offsets are not a source-symbolized stack."
    elif completed.returncode != 0:
        diagnostic = f"CDB analysis failed with exit code {completed.returncode}."
    elif not re.search(r"engine[\\/]+source[\\/]", completed.stdout, re.IGNORECASE):
        diagnostic = "CDB completed, but Durin source lines were unavailable; adjacent PDBs may be missing or mismatched."
    else:
        diagnostic = "CDB analysis completed with local runtime symbols only."
    return CrashAnalysis(log_path, _bounded_debugger_excerpt(completed.stdout), diagnostic, command)


def format_crash_summary(artifact: CrashArtifact, analysis: CrashAnalysis | None = None) -> str:
    values = artifact.values
    lines = [
        "Native crash artifacts: " + ("complete" if artifact.complete else "incomplete"),
        f"  Exception: {format_windows_status(int(values.get('ReasonCode', '0'), 0))}",
        f"  Phase: {values.get('ProcessPhase', 'Unavailable')}",
        f"  Faulting thread: {values.get('FaultingThreadId', 'Unavailable')}",
        f"  Context: {artifact.context_path}",
        f"  Dump: {artifact.dump_path or 'Unavailable'}",
    ]
    if artifact.diagnostic:
        lines.append(f"  Context diagnostic: {artifact.diagnostic}")
    if analysis is not None:
        lines.append(f"  Analysis: {analysis.diagnostic}")
        if analysis.log_path is not None:
            lines.append(f"  Analysis log: {analysis.log_path}")
        if analysis.excerpt:
            lines.extend(("  Faulting-thread stack:", analysis.excerpt))
    return "\n".join(lines)
