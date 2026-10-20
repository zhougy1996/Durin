---
name: durin-mcp
description: Connect an external agent to a running Durin editor through local MCP, discover tools, control PIE, and manage Python script jobs. Use for Durin MCP connection setup, diagnosis, or editor operations; engine development uses the repository documentation directly.
---

# Durin MCP

Use the Durin editor's own MCP server. The expected client configuration name is
`durin`; this skill does not install or start a server or change client settings
merely by being loaded.

## Installation and scope

This skill can be copied to a repository or personal `.agents/skills` directory.
It is intended for external agents working with Durin, including agents without
an engine source checkout. The destination must contain `durin-mcp/SKILL.md`.
Installing the skill supplies instructions, not a running editor or MCP connection.

## Editor availability and startup

Durin has no release distribution or established release startup command yet.
Do not guess an executable name, installation path, download URL, or release
launcher. This skill targets the current MCP contract, not a future package layout.

- If an editor is already running with MCP enabled, connect using its actual
  logged endpoint and the existing private credential configuration.
- If the editor must be started from source, first locate the Durin checkout
  (`Durin.dworkspace` and `DevTool.bat` or `DevTool`). Read
  `Documentation/Agents/BuildAndRun.md` and the enablement section of
  `Documentation/Editor/Guides/LocalMCP.md` there, then use that checkout's host
  launcher. Do not build or launch merely to read this skill.
- If no running endpoint or usable source checkout is available, explain that
  connection requires an editor instance with MCP enabled and request its
  endpoint or checkout location. Continue independent work, but do not invent a
  startup command or substitute a different MCP server.

When a future release supplies its own launch instructions, use those instructions
for that version. Do not apply the source launcher to a packaged editor.

The instructions below are self-contained for connecting to an enabled editor.
Use live discovery for tool availability and schemas. If source documents are
available, resolve `Documentation/Editor/Guides/LocalMCP.md` and
`Documentation/Editor/Architecture/MCPTools.md` from the engine checkout root for
the authoritative contract. Do not resolve them relative to this skill's install
location or require an engine checkout just to operate an existing connection.

## Connect and diagnose

1. Check the current conversation's available MCP servers/tools for Durin.
   Distinguish missing configuration, an offline configured server, and a client
   conversation whose tool inventory predates configuration changes.
2. If Durin tools are available, load `editor.pie.status` and call it with `{}`
   as a read-only connectivity check. Use the actual client-exposed tool name;
   clients may wrap MCP tool names with a namespace.
3. If connection fails or tools are absent, inspect the endpoint, authentication,
   editor log, and client configuration using the connection guidance below.
   Consult the Local MCP guide when available. Reuse existing endpoint and
   credentials rather than generating replacements.
4. After changing client configuration, connect a new Codex session. Saving the
   configuration does not reload an existing conversation's tool inventory.

## Current server enablement and client configuration

The server is disabled by default. For an editor instance whose startup you are
configuring, set these variables in the process that launches that editor:
`DURIN_MCP_PORT` (0-65535; 0 selects a free port) and
`DURIN_MCP_TOKEN_FILE` (absolute path to the token file). The token must contain
32-128 ASCII alphanumeric characters; a final newline is accepted, and the file
must be at most 256 bytes. The server reads it once at startup.

Use the endpoint reported by the editor log:
`MCP listening at http://127.0.0.1:<port>/mcp`. Port 60123 below is an example,
not a discovery rule. Use Streamable HTTP with bearer authentication, not stdio
or the older HTTP+SSE transport. The current server implements MCP 2025-06-18.
A raw POST without the MCP initialization lifecycle is not a connectivity test.

For Codex, the repository guide verifies the following personal
`~/.codex/config.toml` configuration shape. Substitute the actual endpoint and
token locally; use other clients' native Streamable HTTP configuration rather
than copying Codex's TOML schema:

```toml
[mcp_servers.durin]
url = "http://127.0.0.1:60123/mcp"
enabled = true
startup_timeout_sec = 30
tool_timeout_sec = 45
http_headers = { Authorization = "Bearer <actual token contents>" }
```

Keep credentials in personal configuration outside the repository. Do not paste
real tokens into chat, logs, commits, or this skill. Invalid token configuration
or an occupied port disables MCP while leaving the editor usable; inspect the
editor log instead of assuming the whole editor failed.

## Discover and call tools

Discover current tools and schemas through the MCP client, then load only tools
needed for the user's task. Names use `<toolset>.<tool>`, preserving underscores;
legacy double-underscore names are not aliases. Explicitly refresh discovery
after catalog changes; the server does not advertise list-change subscriptions.
Avoid injecting the complete tool catalog into the conversation unnecessarily.

PIE tools include:

- `editor.pie.status`: inspect current state with `{}`.
- `editor.pie.play` and `editor.pie.stop`: use their discovered input schemas.
- `editor.pie.pause`: pass an explicit boolean, e.g. `{"paused": true}`.

Business calls execute at the editor's next safe point and require editor
progress. Check wire `isError` (SDK `is_error`) before consuming
`structuredContent.data` (SDK `structured_content["data"]`). A successful
ordinary call returns its final business result, not just admission.

## Python jobs

When Durin's packaged Python runtime is enabled, discovery adds:

- `editor.python.submit`: supply exactly one of `code` or `file`; returns `job_id`
  after admission, not script completion.
- `editor.python.status`: pass `{"job_id": "<returned id>"}` to inspect `state`,
  `output`, and `error`. A successful status query can describe a failed script.
- `editor.python.cancel`: pass the same session-owned `job_id`, then inspect status
  until the job reaches an actual terminal state.

States are `queued`, `running`, `succeeded`, `failed`, and `cancelled`. Keep the
same MCP session for job operations; IDs cannot be used from another session.
Inline code is limited to 64 KiB UTF-8. File submissions read at most 1 MiB on the
editor machine at submission time; relative paths resolve against the editor's
project directory, not the client or process working directory. Later file edits
do not change an admitted job. Missing Python tools need runtime diagnosis,
not reinstallation of the ordinary MCP server.

Python executes trusted code with editor-process privileges and is not sandboxed.
Keep scripts within the user's requested work and keep endpoint credentials private.

## Cancellation and uncertain results

For an in-flight RPC, use the client's cancellation facility, which sends
`notifications/cancelled` with the request ID in the same session. After Python
submit returns, cancel the admitted job with `editor.python.cancel`, not the
completed submit RPC's ID. Cancellation is cooperative and does not roll back
side effects. A cancellation request is not proof that execution has stopped.

Disconnecting or closing the response stream does not itself cancel admitted
work. Session deletion/expiry and server stop cancel session-owned work. A timeout
or lost response does not prove a mutation did not happen: inspect resulting
state before considering a retry. Never automatically resubmit Python jobs or
repeat other mutations after an uncertain result.
