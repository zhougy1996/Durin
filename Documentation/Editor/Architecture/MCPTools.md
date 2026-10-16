# Local MCP Toolset Adapter

Summary: DurinMCP exposes external editor Toolsets and session-owned Python script jobs through authenticated loopback Streamable HTTP.
Modules: DurinMCP, DurinPython, HTTPServer, DurinEd, MainFrame, Core

Last reviewed: 2026-10-16

## Ownership and Entry Points

DurinMCP owns protocol handling, transport policy, schema/value conversion,
discovery names, pending MCP calls and script job ownership. It depends privately on DurinEd, DurinPython and
[HTTPServer](../../Runtime/Networking/HTTPServer.md). HTTPServer owns listeners,
socket lifecycle, HTTP routing, and chunked response writes, and privately wraps
the pinned MIT cpp-httplib dependency. MCP authentication, sessions, SSE message
formatting, heartbeats, and invocation cancellation remain in DurinMCP. Core's
existing FJsonDocument/yyjson owns JSON parsing and writing. Protocol types stay
private to DurinMCP; third-party transport types stay private to HTTPServer.
Wire serialization selects Core's `EJsonFormat::Compact` directly. There is no
adapter-side whitespace rewriting; JSON escaping preserves string contents and
keeps each SSE data message on one line.

`FMCPServer::Start(DEditorEngine&, Options)` selects one editor and acquires its
detached `FEditorToolService` on the GameThread. Start and Stop are control-thread
operations and must not race one another. Workers retain only owned protocol data
and the detached endpoint; they never resolve DObjects or invoke providers.

MainFrame loads DurinMCP and calls `StartForEditor` during explicit host creation.
Its host destruction stops the server before releasing the host. Module shutdown
also stops the module-owned server. Merely loading DurinMCP does not open a port.
Startup failure logs a bounded explanation and leaves normal editor use available.
When MCP is enabled, its host acquires the Python module's shared runtime through
EnsureRuntime on GameThread. Python initialization failure omits script tools and
logs the reason while ordinary MCP remains available. UI and MCP use the same
interpreter with separate native sessions. The host stops MCP before Python.

See [connection setup](../Guides/LocalMCP.md) and the authoritative
[editor service contract](EditorTools.md#external-adapter-handoff).

## Protocol Surface

Only MCP revision `2025-06-18` is implemented. The single `/mcp` endpoint supports
`initialize`, `notifications/initialized`, `ping`, `tools/list`, `tools/call`, and
`notifications/cancelled`. Initialization negotiates this revision and advertises
only the tools capability. Resources, Prompts, subscriptions, stdio, and the older
HTTP+SSE transport are absent. This revision matches the verified Codex 0.144.1
client; no second protocol implementation or compatibility bridge is retained.

Initialize requires clientInfo, capabilities, and protocolVersion. Its response
returns serverInfo, capabilities, protocolVersion, and a random `Mcp-Session-Id`
header. Subsequent requests carry that session header, and clients send
`MCP-Protocol-Version: 2025-06-18`. A missing version header can use the session's
negotiated revision; a supplied unsupported or duplicate version is rejected.
Tools require the initialized notification first. Missing sessions return 400;
unknown or expired sessions return 404. DELETE terminates a session and cancels
its calls. GET with a valid session returns 405 because server event streams are
not provided. Unsupported RPC methods return -32601/404.

POST Content-Type must be application/json and Accept must include both JSON and
SSE. Ping returns an empty JSON result. Accepted notifications return 202 without
a JSON-RPC response. The official
[lifecycle](https://modelcontextprotocol.io/specification/2025-06-18/basic/lifecycle)
and [transport rules](https://modelcontextprotocol.io/specification/2025-06-18/basic/transports)
define wire semantics. Clients explicitly refresh discovery; the server does not
advertise catalog change notifications or stream resumption.

## Discovery and Values

Discovery consumes owned catalog snapshots and filters `bExposeExternally`.
External admission checks exposure again. Transient availability is evaluated at
execution; it does not remove a tool from discovery. Catalog descriptions are
cached by revision and sorted by advertised name.

Names are `<escaped-toolset>__<escaped-tool>`, for example `editor.pie__pause`.
Every native underscore is escaped as `_u`, so a component cannot contain the
`__` separator. For example, native tool `add_component` becomes
`add_ucomponent`. A name longer than
128 bytes becomes its first 92 bytes, `__h`, and the full XXH3-128 hexadecimal
digest. A collision rejects the catalog instead of dispatching ambiguously.
The adapter retains the original `FToolAddress`, ContractVersion, and input schema
for dispatch, alongside the converted protocol description. It does not retain a
second complete native descriptor.

Tools/list returns the complete catalog in name order, without pagination or
continuation cursors. A supplied cursor is rejected with -32602. The response byte
budget applies to the complete result; excess data fails instead of omitting tools.

The [Core schema vocabulary](../../Runtime/Core/Tools.md) maps to JSON Schema
closed objects, required properties, defaults, arrays, enums, and numeric bounds.
Signed and unsigned integer types retain their natural 64-bit bounds. Native
string size bounds count UTF-8 bytes, so `x-durin-minUtf8Bytes` and
`x-durin-maxUtf8Bytes` communicate them without falsely advertising code-point
length rules. Server-side Core validation remains authoritative.

Wire integers are parsed/written exactly; signed and Number arguments are
converted according to their declared schemas. Client runtimes that round large
JSON integers can still lose precision before the server receives a value.
Defaults and business validation run once in the shared service; MCP performs
only bounded wire conversion and envelope validation.

## Calls, Results, and Cancellation

Calls submit `FToolRequest` with `EToolSource::External`. The detached endpoint
queues business work for the existing GameThread safe point. The HTTP response
opens an SSE stream, polls its native receipt every 10 ms, and sends exactly one
final result. An admission receipt is never returned as successful business data.
Receipts are keyed by session and JSON-RPC ID; different sessions can reuse an ID.

Successful calls return `structuredContent: {data: <native result>}` and a text
content block. Advertised outputSchema describes that object envelope, including
scalar, array, and null native results. Text beyond 64 KiB becomes a short pointer
to structuredContent; structured business data is never truncated.

Native argument/admission/domain/output/receipt failures return `isError: true`
with diagnostic code, path, and message. Full diagnostics are retained in
`_meta["durin/diagnostics"]`; text is bounded. Unknown tool names or malformed
method parameters use -32602. Malformed JSON and invalid JSON-RPC envelopes use
-32700 and -32600 respectively. Failures never trigger automatic retries.
Effects and retry annotations describe behavior; they do not grant access.
Request parsing returns an explicit error category and message; diagnostic wording
does not determine the wire error code.

`notifications/cancelled` cancels the matching request only within its session.
A queued cancellation prevents execution. Running cancellation sets the native
token cooperatively; it cannot preempt C++ handlers or roll back effects. Closing
an SSE stream alone does not cancel. The session retains disconnected work until
completion, explicit cancellation, or its original deadline; there is no replay
endpoint. A maintenance thread checks retained receipts every 20 ms. Session
termination, server deadlines, and Stop also cancel. Completion races never cause
automatic retries or a second final response.

Each pending record distinguishes unadmitted, pending, completed, and
cancel-requested adapter states. Requesting cancellation is not proof that a
running native handler has stopped. Response closure is tracked independently.
One polling routine checks native results and deadlines, called by the live
response worker or by maintenance after the response worker exits. Completion and
cancellation compete for one state transition; cleanup cannot overwrite the winner.

## Python Script Jobs

When the owner attaches a running FPythonRuntime in FMCPServerOptions, discovery
also advertises three adapter-owned tools. These use normal MCP tools/call;
they are not a protocol Tasks extension or native FToolResult::OperationId.
Routes are explicit and advertised-name collisions with native Toolsets reject
the catalog. Script tools do not execute through GameThread provider callbacks.
Their scripts still call native Toolsets through the existing editor tool service.

| Tool | Input | Successful data |
| --- | --- | --- |
| `editor.python__submit` | Required `code` string, up to 64 KiB UTF-8; optional `filename`, up to 4096 bytes, default `<mcp>` | `{job_id}` after admission only |
| `editor.python__status` | Required session-owned `job_id` string | `{job_id, state, output, error}` |
| `editor.python__cancel` | Required session-owned `job_id` string | `{job_id, cancellation_requested}` |

Inputs are closed objects validated against Core schemas. There is no remote file
path runner or source/host override. Submit is non-idempotent and is never retried
automatically, including when a response is lost. Status is read-only; cancel is
idempotent and cooperative. All data uses the normal structuredContent.data envelope.

Job IDs are opaque strings scoped by the MCP session, separate from JSON-RPC IDs.
Every submitting MCP session lazily obtains its own External Python session bound
to the selected editor endpoint. Unknown, foreign or expired IDs fail without
revealing other sessions' jobs. The global Python retention/queue limits apply;
each MCP session retains at most 64 job mappings and evicts its oldest terminal
mapping when necessary. Runtime evictions are observed as expired jobs.

Status snapshots expose queued, running, succeeded, failed or cancelled. Polling
a failed script succeeds as a status query, with state=failed and its traceback
in error; it is not an admission error. stdout/stderr and error text retain the
Python runtime's 64 KiB budgets, truncated only at complete UTF-8 boundaries.
`cancellation_requested` does not claim the job has stopped or rolled back effects;
poll status to observe its actual outcome. It is false if no request can be made,
including terminal jobs or a concurrent retention eviction.

MCP session deletion, idle expiry and server stop close its Python session and
cancel owned jobs, including unfinished native tool calls. They do not stop the
shared runtime or cancel UI/other-client sessions. HTTP disconnect does not cancel
admitted scripts. notifications/cancelled addresses an in-flight RPC only; after
submit returns, cancel the job through editor.python__cancel. The job retains its
original cooperative runtime deadline independently of later status queries.

Runtime lifecycle locking protects short native calls during owner shutdown, and
the shared native handle prevents dangling runtime access. Start/Stop initialize
and finalize CPython only on its worker. No HTTP worker initializes an interpreter,
accesses DObjects or waits for a script to complete. See the
[Python adapter](PythonTools.md) for supported scripts and shutdown limitations.

Python runs with ordinary editor-process privileges, not in a sandbox. When these
tools are enabled the MCP bearer token authorizes trusted code execution, including
file and other process actions; native Toolset exposure does not restrict Python's
standard library. Keep the endpoint local and credentials private.

## Limits and Shutdown

The listener binds exclusively to 127.0.0.1. Each request requires the configured
bearer token. Host and any supplied Origin must match the actual local endpoint;
tokens never appear in URLs or logs. User-owned token files survive server stop.

| Resource | Limit |
| --- | --- |
| HTTP body | 1 MiB before parsing |
| Protocol JSON tree | Depth 40, 8192 nodes, 2048 entries/container |
| Protocol strings | 128 KiB each, 1 MiB combined |
| Serialized JSON response | 16 MiB |
| Text content | 64 KiB |
| HTTP workers / queued socket jobs | 8 / 8 |
| Live call response streams | 6 (reserves two workers for control requests) |
| Retained pending calls across sessions | 32 |
| Sessions / idle lifetime | 16 / 10 minutes |
| Read/write idle timeout | 2 seconds |
| Call deadline | 30 seconds by default, positive and at most 30 seconds |

Core business limits and editor service queue/result budgets apply independently.
Excess work is rejected before admission or fails with a bounded error. HTTP
socket job exhaustion closes the excess connection rather than growing a queue.
The OS accept backlog is independent of admitted worker jobs.

Stop closes admission and the listener, interrupts tracked accepted sockets,
cancels session-owned invocations, and joins workers and maintenance before releasing the service
and transport. Descriptor tracking and closure share a lock so shutdown cannot
close a descriptor reused by another connection. Partial HTTP bodies cannot keep
module shutdown waiting for client input. Stop never waits for an editor tick.

## Verification

`MCPPythonJobTests` covers script admission, snapshots, cancellation, session
ownership, limits, Unicode output, runtime sharing and closure over real HTTP.
`MCPPythonClientSmoke.py` exercises the official SDK against a real editor with
both UI and MCP Python sessions. `MCPCodecTests` covers the detached wire/schema boundary. `MCPIntegrationTests`
uses a real listener and CPU editor host to cover admission, dispatch,
cancellation, errors, and teardown. The checked-in `MCPClientSmoke.py` driver uses
the official Python SDK 2.3.0 against the same native host. Its explicit SDK host
case skips when the driver is absent; see [connection setup](../Guides/LocalMCP.md)
for reproduction. No GPU or application-hosted test is required for these paths.
