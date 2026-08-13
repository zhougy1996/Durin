
from datetime import datetime, timedelta, timezone
from pathlib import Path
from unittest import mock

from durin_dev_tool.build import crash


def write_artifact(
    root: Path,
    *,
    timestamp: datetime,
    process_id: int = 42,
    runtime_variant: str = "DurinEditor",
    complete: bool = True,
) -> crash.CrashArtifact:
    stamp = timestamp.strftime(crash.TIMESTAMP_FORMAT)
    crash_id = f"{runtime_variant}-{stamp}-{process_id}"
    directory = root / crash_id
    directory.mkdir(parents=True)
    dump = directory / f"{crash_id}.dmp"
    dump.write_bytes(b"minidump")
    context = directory / f"{crash_id}-CrashContext-v1.txt"
    context.write_text(
        "\n".join(
            (
                "FormatVersion=1",
                "CompletionState=PendingMarker",
                f"CrashId={crash_id}",
                "ReasonCode=0xc0000005",
                "ReasonKind=UnhandledSEH",
                "ExceptionAddress=0x1234",
                f"ProcessId={process_id}",
                "FaultingThreadId=99",
                f"RuntimeVariant={runtime_variant}",
                "BuildConfiguration=Debug",
                "BuildIdentity=0.1.0-dev",
				"ExecutableImagePath=C:/Runtime/DurinEditor.exe",
                f"UtcTimestamp={stamp}",
				"ProcessUptimeMicroseconds=1000",
                "ProcessPhase=ObjectCollection",
				"BreadcrumbWriteSequence=1",
				"BreadcrumbFirstSequence=1",
				"BreadcrumbCount=1",
				"ActiveLogPath=C:/Runtime/Saved/Logs/Durin.log",
				"LastAcceptedLogSequence=7",
				"LastProcessedLogSequence=6",
				"LastDurableLogSequence=5",
                f"DumpPath={dump}",
                "DumpResult=Written",
				"DirectoryError=0",
				"ContextError=0",
				"DumpError=0",
				"AccessViolationOperation=Read",
				"AccessViolationAddress=0x1",
                "FutureOptionalKey=preserved-by-forward-parser",
                "Breadcrumb=1,PhaseChanged,99,100,8,0",
            )
        ),
        encoding="utf-8",
    )
    if complete:
        (directory / "Complete.marker").write_text("CrashContextVersion=1\n", encoding="utf-8")
    values, breadcrumbs, diagnostic = crash.parse_crash_context(context)
    return crash.CrashArtifact(directory, context, dump, complete, values, breadcrumbs, diagnostic)


def test_formats_unsigned_native_status_and_known_name() -> None:
    assert crash.format_windows_status(-1073741819) == "0xC0000005 (EXCEPTION_ACCESS_VIOLATION)"
    assert crash.format_windows_status(0xE0000001) == "0xE0000001 (DURIN_STD_TERMINATE)"


def test_parser_accepts_unknown_keys_and_reports_malformed_required_data(tmp_path: Path) -> None:
    artifact = write_artifact(tmp_path, timestamp=datetime.now(timezone.utc))
    assert artifact.values["FutureOptionalKey"] == "preserved-by-forward-parser"
    assert artifact.breadcrumbs == ("1,PhaseChanged,99,100,8,0",)
    malformed = artifact.context_path.with_name("Malformed-CrashContext-v1.txt")
    malformed.write_text("FormatVersion=1\nnot-a-field\n", encoding="utf-8")
    _, _, diagnostic = crash.parse_crash_context(malformed)
    assert "missing required keys" in diagnostic
    assert "malformed" in diagnostic


def test_discovery_matches_current_process_interval_and_rejects_stale_artifacts(tmp_path: Path) -> None:
    executable = tmp_path / "Runtime" / "DurinEditor.exe"
    executable.parent.mkdir(parents=True)
    executable.write_bytes(b"")
    now = datetime.now(timezone.utc)
    write_artifact(executable.parent / "Saved" / "Crashes", timestamp=now - timedelta(hours=1), process_id=42)
    current = write_artifact(executable.parent / "Crashes", timestamp=now, process_id=73, complete=False)

    found = crash.discover_current_crash(
        executable,
        "DurinEditor",
        73,
        now - timedelta(seconds=1),
        now + timedelta(seconds=1),
    )
    assert found is not None
    assert found.directory == current.directory
    assert not found.complete
    assert crash.discover_current_crash(
        executable,
        "DurinEditor",
        42,
        now - timedelta(seconds=1),
        now + timedelta(seconds=1),
    ) is None


def test_discovery_rejects_dump_path_outside_crash_directory(tmp_path: Path) -> None:
    executable = tmp_path / "含 空格" / "DurinEditor.exe"
    executable.parent.mkdir(parents=True)
    executable.write_bytes(b"")
    now = datetime.now(timezone.utc)
    artifact = write_artifact(executable.parent / "Crashes", timestamp=now)
    text = artifact.context_path.read_text(encoding="utf-8")
    artifact.context_path.write_text(text.replace(str(artifact.dump_path), str(tmp_path / "outside.dmp")), encoding="utf-8")
    found = crash.discover_current_crash(executable, "DurinEditor", 42, now, now)
    assert found is not None
    assert found.dump_path is None
    assert "escapes" in found.diagnostic


def test_missing_debugger_is_actionable_and_nonfatal(tmp_path: Path) -> None:
    artifact = write_artifact(tmp_path, timestamp=datetime.now(timezone.utc))
    result = crash.analyze_crash(artifact, tmp_path, environment={"ProgramFiles(x86)": str(tmp_path / "missing")})
    assert result.log_path is None
    assert "Manual command" in result.diagnostic
    assert str(artifact.dump_path) in result.diagnostic


def test_fake_debugger_output_is_logged_bounded_and_mismatch_is_distinguished(tmp_path: Path) -> None:
    artifact = write_artifact(tmp_path / "crashes", timestamp=datetime.now(timezone.utc))
    cdb = tmp_path / "cdb.exe"
    cdb.write_bytes(b"fake")
    output = "EXCEPTION_CODE: c0000005\n" + "DurinEditor!Frame\n" * 100 + "PDB does not match image\n"
    completed = mock.Mock(returncode=0, stdout=output)
    with mock.patch.object(crash.subprocess, "run", return_value=completed):
        result = crash.analyze_crash(artifact, tmp_path, cdb=cdb, state_dir=tmp_path / "state")
    assert result.log_path is not None and result.log_path.read_text(encoding="utf-8") == output
    assert "mismatched PDBs" in result.diagnostic
    assert len(result.excerpt.splitlines()) <= 50


def test_summary_reports_phase_thread_and_artifact_paths(tmp_path: Path) -> None:
    artifact = write_artifact(tmp_path, timestamp=datetime.now(timezone.utc))
    summary = crash.format_crash_summary(artifact)
    assert "EXCEPTION_ACCESS_VIOLATION" in summary
    assert "ObjectCollection" in summary
    assert "Faulting thread: 99" in summary
    assert str(artifact.context_path) in summary
