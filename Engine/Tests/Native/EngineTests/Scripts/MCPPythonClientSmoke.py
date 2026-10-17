"""Qualify Python MCP jobs with the official SDK against a real Sandbox editor.

Requires the existing Build/MCP/site installation of mcp==2.3.0 and a built editor.
All credentials, launch scripts and evidence stay beneath ignored Build/MCP.
"""

import asyncio
import json
import os
from pathlib import Path
import secrets
import site
import socket
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[5]
site.addsitedir(str(ROOT / "Build/MCP/site"))

import httpx2  # noqa: E402
from mcp.client import Client  # noqa: E402
from mcp.client.streamable_http import streamable_http_client  # noqa: E402


async def exercise(port: int, token: str, ready: Path, process: subprocess.Popen) -> dict:
    url = f"http://127.0.0.1:{port}/mcp"
    async with httpx2.AsyncClient(headers={"Authorization": f"Bearer {token}"}, trust_env=False) as http:
        for _ in range(1000):
            if process.poll() is not None:
                raise AssertionError("Editor launcher exited before readiness; inspect Build/MCP/python-sdk-editor.log")
            if ready.exists():
                break
            await asyncio.sleep(0.02)
        else:
            raise AssertionError("Editor UI Python session did not initialize")

        def transport():
            return streamable_http_client(url, http_client=http, max_sse_event_size=16 * 1024 * 1024)

        async with Client(transport(), mode="legacy", read_timeout_seconds=10, cache=None) as client:
            tools = await client.list_tools()
            names = {tool.name for tool in tools.tools}
            assert {"editor.python.submit", "editor.python.status", "editor.python.cancel"} <= names

            async def call(name: str, args: dict) -> dict:
                result = await client.call_tool(f"editor.python.{name}", args)
                assert not result.is_error, result
                return result.structured_content["data"]

            async def submit(code: str) -> str:
                return (await call("submit", {"code": code, "filename": "agent_sdk.py"}))["job_id"]

            async def wait(job: str) -> dict:
                for _ in range(1000):
                    result = await call("status", {"job_id": job})
                    if result["state"] not in {"queued", "running"}:
                        return result
                    await asyncio.sleep(0.01)
                raise AssertionError("Python job did not finish")

            script = """import durin
assert durin.mcp_ui_marker == 'shared interpreter'
for tool, args, expected in [
    ('status', {}, 'stopped'), ('play', {}, 'playing'),
    ('pause', {'paused': True}, 'paused'), ('stop', {}, 'stopped')]:
    assert durin.tools.call('editor.pie', tool, args)['state'] == expected
print('SDK_PYTHON_JOBS_OK')
"""
            job = await submit(script)
            success = await wait(job)
            assert success["state"] == "succeeded", success
            assert success["output"] == "SDK_PYTHON_JOBS_OK\n", success
            failure = await wait(await submit("print('before failure')\nraise ValueError('SDK expected failure')"))
            assert failure["state"] == "failed" and "agent_sdk.py" in failure["error"], failure
            assert "SDK expected failure" in failure["error"], failure
            running = await submit("while True:\n    pass\n")
            cancellation = await call("cancel", {"job_id": running})
            assert cancellation["cancellation_requested"], cancellation
            cancelled = await wait(running)
            assert cancelled["state"] == "cancelled", cancelled
            async with Client(transport(), mode="legacy", cache=None) as other:
                foreign = await other.call_tool("editor.python.status", {"job_id": job})
                assert foreign.is_error, foreign
            return {"tools": sorted(names), "success": success, "failure": failure, "cancelled": cancelled,
                    "foreign_job_rejected": True, "ui_mcp_runtime_shared": True}


def main() -> None:
    evidence = ROOT / "Build/MCP"
    evidence.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="python-app-", dir=evidence) as work:
        work_path = Path(work)
        token = secrets.token_hex(32)
        token_file = work_path / "token.txt"
        token_file.write_text(token, encoding="ascii")
        ready = work_path / "ui.ready"
        ui_script = work_path / "ui.py"
        ui_script.write_text("import durin\ndurin.mcp_ui_marker = 'shared interpreter'\n"
                             f"with open({str(ready)!r}, 'w') as ready:\n    ready.write('ready')\n", encoding="utf-8")
        with socket.socket() as probe:
            probe.bind(("127.0.0.1", 0))
            port = probe.getsockname()[1]
        env = os.environ.copy()
        env.update(DURIN_MCP_PORT=str(port), DURIN_MCP_TOKEN_FILE=str(token_file), DURIN_PYTHON_SCRIPT=str(ui_script))
        with (evidence / "python-sdk-editor.log").open("w", encoding="utf-8") as log:
            process = subprocess.Popen([str(ROOT / "DevTool.bat"), "run", "--project", "Sandbox/Sandbox.dproject",
                                        "--", "--exit-after-ticks=900"],
                                       cwd=ROOT, env=env, stdout=log, stderr=subprocess.STDOUT)
            try:
                result = asyncio.run(exercise(port, token, ready, process))
                (evidence / "python-sdk-results.json").write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding="utf-8")
            finally:
                code = process.wait(timeout=60)
            assert code == 0, f"Editor exited with {code}; inspect python-sdk-editor.log"
    print("Official SDK: Python submit/status/cancel, PIE tools, traceback, session isolation and shared UI runtime passed")


if __name__ == "__main__":
    main()
