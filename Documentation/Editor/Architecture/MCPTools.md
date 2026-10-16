# Local MCP Toolset Adapter

Summary: DurinMCP exposes external editor Toolsets through authenticated loopback Streamable HTTP and dispatches all business work through the editor tool service.
Modules: DurinMCP, DurinEd, MainFrame, Core

Last reviewed: 2026-10-16

## Ownership and Entry Points

DurinMCP owns protocol handling, transport, schema/value conversion, discovery
names, and pending HTTP requests. It depends privately on DurinEd and the pinned
MIT cpp-httplib dependency. Core's existing FJsonDocument/yyjson owns JSON parsing
and writing. Protocol and transport types stay private to DurinMCP.
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

`MCPCodecTests` covers the detached wire/schema boundary. `MCPIntegrationTests`
uses a real listener and CPU editor host to cover admission, dispatch,
cancellation, errors, and teardown. The checked-in `MCPClientSmoke.py` driver uses
the official Python SDK 2.3.0 against the same native host. Its explicit SDK host
case skips when the driver is absent; see [connection setup](../Guides/LocalMCP.md)
for reproduction. No GPU or application-hosted test is required for these paths.
