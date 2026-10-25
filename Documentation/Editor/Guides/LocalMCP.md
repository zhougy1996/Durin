# Connect to Local Editor MCP

Summary: Enable the local DurinMCP endpoint and connect Codex or another MCP 2025-06-18 Streamable HTTP client.
Modules: DurinMCP, DurinPython, MainFrame

Last reviewed: 2026-10-18

## Enable the Server

The server is disabled by default. Set the following environment variables in
the process that launches the editor:

- `DURIN_MCP_PORT`: an integer from 0 to 65535. Zero selects an available port.
- `DURIN_MCP_TOKEN_FILE`: an absolute path to a file containing a 32-128 character
  ASCII alphanumeric bearer token. A final newline is accepted; the file must
  contain at most 256 bytes.

For example, create a token for this editor instance in PowerShell:

```powershell
$mcpDirectory = Join-Path $env:LOCALAPPDATA 'Durin/MCP'
New-Item -ItemType Directory -Force $mcpDirectory | Out-Null
$mcpTokenPath = Join-Path $mcpDirectory 'editor-token.txt'
$mcpToken = [Convert]::ToHexString([Security.Cryptography.RandomNumberGenerator]::GetBytes(32))
[IO.File]::WriteAllText($mcpTokenPath, $mcpToken)
$env:DURIN_MCP_PORT = '60123'
$env:DURIN_MCP_TOKEN_FILE = $mcpTokenPath
.\DevTool.bat run
```

Use the normal [build/run workflow](../../Agents/BuildAndRun.md) to build the
editor first. On successful startup the log reports
`MCP listening at http://127.0.0.1:<port>/mcp`. An occupied port or invalid token
configuration disables MCP with an explanation; the editor remains usable.
The token is read once at startup. Stop/restart the editor to change it.

## Client Connection

Configure a Streamable HTTP client with the logged endpoint and an HTTP header:

```text
Authorization: Bearer <contents of DURIN_MCP_TOKEN_FILE>
```

The endpoint implements only MCP revision `2025-06-18`, with initialize,
initialized notification, HTTP sessions, and explicit cancellation notifications.
The older HTTP+SSE transport is not implemented. Verified clients are Codex CLI
0.144.1 and the official Python MCP SDK 2.3.0 in handshake mode.

For Codex, add the following table to the personal `~/.codex/config.toml`,
substituting the actual token contents. Keep credentials outside the repository.
Codex 0.144.1 supports the literal `http_headers` table used here:

```toml
[mcp_servers.durin]
url = "http://127.0.0.1:60123/mcp"
enabled = true
startup_timeout_sec = 30
tool_timeout_sec = 45
http_headers = { Authorization = "Bearer <token contents>" }
```

Start the editor with the same port and token file before connecting a new Codex
session. A saved configuration does not reload an existing conversation's tool
inventory. See the [Codex MCP setup](https://learn.chatgpt.com/docs/extend/mcp?surface=cli)
for the client's general configuration workflow.

With that SDK installed, a connection looks like:

```python
import asyncio
from pathlib import Path
import httpx2
from mcp.client import Client
from mcp.client.streamable_http import streamable_http_client

async def main():
    token = Path("<absolute token file>").read_text().strip()
    async with httpx2.AsyncClient(
        headers={"Authorization": f"Bearer {token}"}, trust_env=False,
    ) as http:
        transport = streamable_http_client(
            "http://127.0.0.1:60123/mcp", http_client=http,
            max_sse_event_size=16 * 1024 * 1024,
        )
        async with Client(transport, mode="legacy", cache=None) as client:
            assert client.protocol_version == "2025-06-18"
            tools = await client.list_tools()
            print([tool.name for tool in tools.tools])
            result = await client.call_tool("editor.pie.status", {})
            print(result.structured_content)

asyncio.run(main())
```

The SDK performs the handshake and sends session/version headers. A generic HTTP
POST without initialization is not a complete MCP exchange. The matching architecture
[contract](../Architecture/Tools/MCPTools.md) describes errors and limits.

## Available Tools and Cancellation

Discovery publishes existing Toolset tools marked for external exposure.
MCP names join the Toolset and local tool with a dot. Toolsets may contain dots;
local tools cannot. Underscores are preserved. Refresh client discovery after
upgrading from the previous double-underscore naming; old names are not aliases.

PIE tools are `editor.pie.status`, `editor.pie.play`, `editor.pie.pause`, and
`editor.pie.stop`. Pause requires an explicit boolean:

```python
await client.call_tool("editor.pie.pause", {"paused": True})
```

Business work executes when the editor reaches its next safe point. A call
returns its final business result, with data under `structured_content["data"]`.
Check `is_error` before consuming a success result. Refresh tools/list to discover
changes; this server does not advertise list-change subscriptions.

Use the client's cancellation facility to send `notifications/cancelled` with
the request ID in the same session. Closing the response stream alone does not
cancel admitted work. Cancelling a queued call prevents execution; running handlers
must observe cancellation cooperatively. A call timeout, disconnect, or missing
result does not establish that no effects occurred, so do not automatically
retry mutations. No public receipt-polling or cancel business tools are needed.

## Submit, Inspect and Cancel Python Scripts

With the [packaged Python runtime](PythonScripts.md) prepared and enabled, MCP
automatically shares it with the editor's script panel. Discovery adds
`editor.python.submit`, `editor.python.status` and `editor.python.cancel`.
Python absence leaves ordinary Toolset tools available and logs the reason.

These tools execute trusted code with editor-process privileges. The MCP token
grants that ability; Python is not a sandbox. Scripts submitted remotely still
use External source when calling `durin.tools`, and cannot choose a different
editor or elevate the native tool source.

Inside the connected SDK client context above:

```python
submitted = await client.call_tool("editor.python.submit", {
    "code": "import durin\nprint(durin.tools.call('editor.pie', 'status', {}))",
})
assert not submitted.is_error
job_id = submitted.structured_content["data"]["job_id"]

while True:
    reply = await client.call_tool("editor.python.status", {"job_id": job_id})
    assert not reply.is_error
    snapshot = reply.structured_content["data"]
    if snapshot["state"] not in {"queued", "running"}:
        print(snapshot["state"], snapshot["output"], snapshot["error"])
        break
    await asyncio.sleep(0.1)

# For unfinished work, request cancellation and continue polling actual state:
# await client.call_tool("editor.python.cancel", {"job_id": job_id})
```

To run a file already present on the editor machine, submit instead:

```python
submitted = await client.call_tool("editor.python.submit", {"file": "Scripts/setup_scene.py"})
```

Supply exactly one of code and file. Relative file paths use the project directory;
absolute paths are accepted. The source is read at submission, before Python job
queueing, with a 1 MiB limit. Editing the file afterward does not change that job.
The resolved path sets __file__ and traceback filenames; no cwd/sys.path is changed.
Inline code uses the diagnostic filename `<durin>`.

These are native Toolset calls and require the editor to tick for admission,
query and cancellation handling. Scripts execute asynchronously; submit returns
admission, not script success. Inline code is limited to 64 KiB UTF-8. Status returns actual
state plus bounded output/traceback; a failed script has state=failed even though
the status query itself succeeds. Job IDs belong only to the creating MCP session.
Session deletion/expiry cancels its work; an HTTP disconnect alone does not.
Use the cancel tool for admitted jobs, not the completed submit RPC's request ID.
No automatic retry, hard termination or rollback is provided. See
[job limits and ownership](../Architecture/Tools/MCPTools.md#python-script-jobs).

After building the editor and installing the existing SDK dependency below, run
the application acceptance driver with the host Python:

```powershell
python Engine/Tests/Native/EngineTests/Scripts/MCPPythonClientSmoke.py
```

It launches a hidden Sandbox editor, verifies UI/MCP runtime sharing, submits a
script using PIE tools, checks traceback output and cancellation, and rejects a
foreign-session job query. Credentials are temporary and all evidence remains
under ignored Build/MCP. Run it after other DevTool operations finish because the
editor launcher requires exclusive checkout ownership.

## Reproduce SDK Acceptance

`DevTool mcp sdk-test` is the explicit official SDK interoperability gate. It
starts a native CPU editor host with a test-only token. No GPU or application
host is required. Install the pinned SDK into ignored `Build/MCP/site` using
the prepared Python environment, then run from the repository root.

On macOS:

```sh
.venv/bin/python -m pip install --upgrade --target Build/MCP/site 'mcp==2.3.0'
./DevTool mcp sdk-test
```

On Windows:

```powershell
.\.venv\Scripts\python.exe -m pip install --upgrade --target Build/MCP/site 'mcp==2.3.0'
.\DevTool.bat mcp sdk-test
```

The command accepts `--profile` and `--preset` and uses the selected host's
DevTool launcher to build and run the native host. Missing, incomplete, or
incorrectly versioned SDK dependencies fail before launch. It retains the native
log in `Build/MCP/sdk-native.log` and a successful host report in
`Build/MCP/sdk-native.xml`. Acceptance requires SDK assertions and exactly one
executed, passing native host case; an empty, disabled, or skipped selection fails.

The native host case `FMCPIntegrationTests.DISABLED_ServesOfficialSDKClient` is
disabled in ordinary native runs. Only the SDK driver enables it and supplies
its control environment; enabling it without the driver fails. Ordinary
invocation/cancellation regressions remain in `MCPIntegrationTests`; follow
[testing guidance](../../Agents/Testing.md) for selections and reports.
Run the SDK gate when changing MCP protocol, transport, or cancellation behavior,
or in a dedicated CI lane. It verifies negotiation, discovery, PIE calls and
both queued and running cancellation.
It waits for an opened queued response and a native running marker, rather than
relying on delays to infer cancellation states. Test markers live in the native
harness sandbox; they are not production MCP tools or files.
