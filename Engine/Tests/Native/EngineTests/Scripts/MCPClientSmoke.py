"""Exercise DurinMCP through the official Python SDK against a native CPU editor.

Install mcp==2.3.0 under Build/MCP/site before running with the host Python.
The native test runs through DevTool; all temporary evidence stays under Build/.
"""

import argparse
import asyncio
import os
import json
from pathlib import Path
import site
import socket
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[5]
site.addsitedir(str(ROOT / "Build/MCP/site"))

import httpx2  # noqa: E402
from mcp.client import Client  # noqa: E402
from mcp.client.streamable_http import streamable_http_client  # noqa: E402


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


async def exercise(port: int, discovery_only: bool, control: Path) -> None:
    url = f"http://127.0.0.1:{port}/mcp"
    queued_opened = asyncio.Event()

    async def on_response(response):
        if response.request.method == "POST" and json.loads(response.request.content).get("params", {}).get("name") == "tests.queued":
            queued_opened.set()

    async with httpx2.AsyncClient(
        headers={"Authorization": "Bearer DurinMCPIntegrationTestToken123456789"},
        trust_env=False, event_hooks={"response": [on_response]},
    ) as http:
        for _ in range(180):
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
            if discovery_only:
                print("Official SDK discovery passed")
                return
            for name, arguments, expected in [
                ("status", {}, "stopped"), ("play", {}, "playing"),
                ("pause", {"paused": True}, "paused"), ("stop", {}, "stopped"),
            ]:
                result = await client.call_tool(f"editor.pie.{name}", arguments)
                assert not result.is_error, result
                assert result.structured_content["data"]["state"] == expected, result
            print("Official SDK PIE status/play/pause/stop passed")
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
            print("Official SDK queued and running cancellation passed")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--discovery-only", action="store_true")
    args = parser.parse_args()
    evidence = ROOT / "Build/MCP"
    evidence.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="sdk-", dir=evidence) as control:
        with socket.socket() as probe:
            probe.bind(("127.0.0.1", 0))
            port = probe.getsockname()[1]
        env = os.environ.copy()
        env.update(DURIN_MCP_SMOKE_CONTROL=control, DURIN_MCP_SMOKE_PORT=str(port))
        log_path = evidence / "sdk-native.log"
        with log_path.open("w", encoding="utf-8") as log:
            process = subprocess.Popen(
                [str(ROOT / "DevTool.bat"), "test", "MCPIntegrationTests", "FMCPIntegrationTests.ServesOfficialSDKClient"],
                cwd=ROOT, env=env, stdout=log, stderr=subprocess.STDOUT,
            )
            try:
                asyncio.run(exercise(port, args.discovery_only, Path(control)))
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
    print(f"Native editor fixture passed; evidence: {log_path}")


if __name__ == "__main__":
    main()
