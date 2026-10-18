from __future__ import annotations

import argparse
import importlib.util
import io
from pathlib import Path
import subprocess
import sys

import pytest

from durin_dev_tool import mcp


ROOT = Path(__file__).resolve().parents[3]
DRIVER = ROOT / "Engine/Tests/Native/EngineTests/Scripts/MCPClientSmoke.py"
spec = importlib.util.spec_from_file_location("mcp_sdk_driver", DRIVER)
assert spec is not None and spec.loader is not None
driver = importlib.util.module_from_spec(spec)
spec.loader.exec_module(driver)


def test_sdk_entry_forwards_build_context_and_failure(monkeypatch, tmp_path):
    calls = []

    def run(command, **kwargs):
        calls.append((command, kwargs))
        return subprocess.CompletedProcess(command, 7)

    monkeypatch.setattr(mcp.subprocess, "run", run)
    stdout, stderr = io.StringIO(), io.StringIO()
    result = mcp.run_sdk_test(
        argparse.Namespace(profile="Mac", preset="Debug-Editor", color="never"),
        repository_root=tmp_path, stdout=stdout, stderr=stderr,
    )
    assert result == 7
    command, options = calls[0]
    assert command == [
        sys.executable, "-I", str(tmp_path / DRIVER.relative_to(ROOT)),
        "--profile", "Mac", "--preset", "Debug-Editor", "--color", "never",
    ]
    assert options == dict(cwd=tmp_path, stdout=stdout, stderr=stderr, check=False)


@pytest.mark.parametrize(("platform", "launcher"), [("darwin", "DevTool"), ("linux", "DevTool"), ("win32", "DevTool.bat")])
def test_sdk_host_uses_platform_launcher_and_explicit_filter(tmp_path, platform, launcher):
    report = tmp_path / "native.xml"
    command = driver.native_command(tmp_path, platform, ["--preset", "Editor"], report)
    assert command == [
        str(tmp_path / launcher), "test", "MCPIntegrationTests", driver.HOST_CASE,
        "--report", str(report), "--preset", "Editor",
    ]


@pytest.mark.parametrize("version", [None, "1.0.0"])
def test_missing_or_wrong_sdk_fails_before_native_launch(monkeypatch, tmp_path, version):
    monkeypatch.setattr(driver, "ROOT", tmp_path)
    if version:
        dist = tmp_path / "Build/MCP/site/mcp-1.0.0.dist-info"
        dist.mkdir(parents=True)
        (dist / "METADATA").write_text(f"Name: mcp\nVersion: {version}\n")
    monkeypatch.setattr(sys, "argv", [str(DRIVER)])

    def forbidden_launch(*_args, **_kwargs):
        pytest.fail("Missing SDK prerequisites must not launch a native process")

    monkeypatch.setattr(driver.subprocess, "Popen", forbidden_launch)
    with pytest.raises(RuntimeError, match="requires mcp==2.3.0"):
        driver.main()


@pytest.mark.parametrize("case", [
    "",  # Successful process with an empty selection.
    '<testcase name="Other" status="run" result="completed"/>',
    '<testcase name="DISABLED_ServesOfficialSDKClient" status="notrun" result="suppressed"/>',
    '<testcase name="DISABLED_ServesOfficialSDKClient" status="run" result="skipped"><skipped/></testcase>',
    '<testcase name="DISABLED_ServesOfficialSDKClient" status="run" result="completed"><failure/></testcase>',
])
def test_sdk_receipt_rejects_absent_disabled_skipped_or_failed_host(tmp_path, case):
    report = tmp_path / "native.xml"
    report.write_text(f"<testsuites><testsuite>{case}</testsuite></testsuites>")
    with pytest.raises(AssertionError):
        driver.verify_native_report(report)


def test_sdk_receipt_accepts_one_executed_host(tmp_path):
    report = tmp_path / "native.xml"
    report.write_text(
        '<testsuites><testsuite><testcase name="DISABLED_ServesOfficialSDKClient" '
        'status="run" result="completed"/></testsuite></testsuites>'
    )
    driver.verify_native_report(report)
