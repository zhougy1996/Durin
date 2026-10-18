"""Exercise DurinMCP through the official Python SDK against a native CPU editor.

Run through DevTool mcp sdk-test after installing mcp==2.3.0 in Build/MCP/site.
The native test runs through the host launcher; evidence stays under Build/.
"""

import argparse
import asyncio
from importlib import metadata
import os
import json
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time
import xml.etree.ElementTree as ET


ROOT = Path(__file__).resolve().parents[5]
SDK_VERSION = "2.3.0"
HOST_CASE = "FMCPIntegrationTests.DISABLED_ServesOfficialSDKClient"


def load_sdk():
    sdk_site = ROOT / "Build/MCP/site"
    installed = {
        dist.metadata["Name"].lower(): dist.version
        for dist in metadata.distributions(path=[str(sdk_site)])
    }
    if installed.get("mcp") != SDK_VERSION:
        python = r".venv\Scripts\python.exe" if sys.platform == "win32" else ".venv/bin/python"
        raise RuntimeError(
            f"SDK acceptance requires mcp=={SDK_VERSION} in {sdk_site}. "
            f"Install with: {python} -m pip install --upgrade --target Build/MCP/site 'mcp=={SDK_VERSION}'"
        )
    sys.path.insert(0, str(sdk_site))
    try:
        import httpx2
        from mcp.client import Client
        from mcp.client.streamable_http import streamable_http_client
    except ImportError as error:
        raise RuntimeError(f"SDK acceptance dependencies are incomplete in {sdk_site}: {error}") from error
    return httpx2, Client, streamable_http_client


def native_command(root: Path, platform: str, options: list[str], report: Path) -> list[str]:
    launcher = root / ("DevTool.bat" if platform == "win32" else "DevTool")
    return [str(launcher), "test", "MCPIntegrationTests", HOST_CASE, "--report", str(report), *options]


def verify_native_report(report: Path) -> None:
    # A zero-test or skipped native process must never count as SDK acceptance.
    suites = ET.parse(report).getroot()
    cases = list(suites.iter("testcase"))
    if len(cases) != 1 or cases[0].get("name") != HOST_CASE.split(".", 1)[1]:
        raise AssertionError(f"Expected exactly the SDK host case in {report}")
    case = cases[0]
    if case.get("status") != "run" or case.get("result") != "completed" or any(
        case.find(tag) is not None for tag in ("skipped", "failure", "error")
    ):
        raise AssertionError(f"SDK native host did not pass: {report}")


async def wait_marker(path: Path) -> None:
    for _ in range(300):
        if path.exists():
            return
        await asyncio.sleep(0.01)
    raise AssertionError(f"Native acceptance marker did not appear: {path}")


async def abandon(task: asyncio.Task) -> None:
    task.cancel()
    try:
        await task
    except asyncio.CancelledError:
        pass


async def exercise(port: int, control: Path, process: subprocess.Popen) -> None:
    httpx2, Client, streamable_http_client = load_sdk()
    url = f"http://127.0.0.1:{port}/mcp"
    queued_opened = asyncio.Event()

    async def on_response(response):
        if response.request.method == "POST" and json.loads(response.request.content).get("params", {}).get("name") == "tests.queued":
            queued_opened.set()

    async with httpx2.AsyncClient(
        headers={"Authorization": "Bearer DurinMCPIntegrationTestToken123456789"},
        trust_env=False, event_hooks={"response": [on_response]},
    ) as http:
        # DevTool configures/builds the target before starting the listener.
        deadline = time.monotonic() + 600
        while time.monotonic() < deadline:
            if process.poll() is not None:
                raise AssertionError(f"Native SDK host exited before becoming ready ({process.returncode})")
            try:
                response = await http.get(url, timeout=1)
                if response.status_code in (400, 405):
                    break
            except httpx2.HTTPError:
                pass
            await asyncio.sleep(0.5)
        else:
            raise AssertionError("Native MCP server did not become ready")
        async with Client(
            streamable_http_client(url, http_client=http, max_sse_event_size=16 * 1024 * 1024),
            mode="legacy", read_timeout_seconds=10, cache=None,
        ) as client:
            assert client.protocol_version == "2025-06-18"
            tools = await client.list_tools()
            names = {tool.name for tool in tools.tools}
            assert {"editor.pie.status", "editor.pie.play", "editor.pie.pause", "editor.pie.stop"} <= names
            print("Official SDK protocol negotiation and discovery passed", flush=True)
            for name, arguments, expected in [
                ("status", {}, "stopped"), ("play", {}, "playing"),
                ("pause", {"paused": True}, "paused"), ("stop", {}, "stopped"),
            ]:
                result = await client.call_tool(f"editor.pie.{name}", arguments)
                assert not result.is_error, result
                assert result.structured_content["data"]["state"] == expected, result
            print("Official SDK PIE status/play/pause/stop passed", flush=True)
            wait_tool = next(tool for tool in tools.tools if tool.name == "tests.wait")
            running_marker = Path(wait_tool.description.split("Marker: ", 1)[1])
            running = asyncio.create_task(client.call_tool("tests.wait", {}))
            await wait_marker(running_marker)
            await abandon(running)
            await client.call_tool("tests.pause", {})
            queued = asyncio.create_task(client.call_tool("tests.queued", {}))
            await asyncio.wait_for(queued_opened.wait(), timeout=3)
            await abandon(queued)
            (control / "resume").touch()
            await wait_marker(running_marker.parent / "resumed")
            stats = await client.call_tool("tests.stats", {})
            assert not stats.is_error, stats
            assert stats.structured_content["data"] == {
                "runningCancelled": True, "queuedExecutions": 0,
            }, stats
            print("Official SDK queued and running cancellation passed", flush=True)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--profile")
    parser.add_argument("--preset")
    parser.add_argument("--color", choices=("auto", "always", "never"))
    args = parser.parse_args()
    load_sdk()  # Fail before building or launching when prerequisites are absent.
    options = []
    for option in ("profile", "preset", "color"):
        value = getattr(args, option)
        if value:
            options.extend((f"--{option}", value))
    evidence = ROOT / "Build/MCP"
    evidence.mkdir(parents=True, exist_ok=True)
    (evidence / "sdk-native.xml").unlink(missing_ok=True)
    with tempfile.TemporaryDirectory(prefix="sdk-", dir=evidence) as control:
        with socket.socket() as probe:
            probe.bind(("127.0.0.1", 0))
            port = probe.getsockname()[1]
        env = os.environ.copy()
        env.update(DURIN_MCP_SMOKE_CONTROL=control, DURIN_MCP_SMOKE_PORT=str(port))
        env["GTEST_ALSO_RUN_DISABLED_TESTS"] = "1"
        report = Path(control) / "native.xml"
        log_path = evidence / "sdk-native.log"
        print(f"Starting official SDK acceptance; native build/run log: {log_path}", flush=True)
        with log_path.open("w", encoding="utf-8") as log:
            process = subprocess.Popen(
                native_command(ROOT, sys.platform, options, report),
                cwd=ROOT, env=env, stdout=log, stderr=subprocess.STDOUT,
            )
            try:
                asyncio.run(exercise(port, Path(control), process))
            finally:
                (Path(control) / "done").touch()
                try:
                    code = process.wait(timeout=45)
                except subprocess.TimeoutExpired:
                    process.terminate()
                    process.wait(timeout=10)
                    raise
                if code:
                    raise AssertionError(log_path.read_text(encoding="utf-8"))
            verify_native_report(report)
            (evidence / "sdk-native.xml").write_bytes(report.read_bytes())
    print(f"Native editor fixture passed; evidence: {log_path}")


if __name__ == "__main__":
    try:
        main()
    except (AssertionError, RuntimeError, OSError, ET.ParseError, subprocess.TimeoutExpired) as error:
        print(f"SDK acceptance failed: {error}\nNative evidence: {ROOT / 'Build/MCP/sdk-native.log'}", file=sys.stderr)
        raise SystemExit(1)
