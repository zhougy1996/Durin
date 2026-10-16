# Local MCP Toolset Integration Plan

Summary: Expose existing editor Toolsets through local Streamable HTTP with discovery, structured invocation, and cooperative cancellation.

Last reviewed: 2026-10-16

Status: Archived
Completed: 2026-10-16

## Current Status

Post-completion revision completed (2026-10-16): the user's installed Codex CLI
0.144.1 was tested against the real Sandbox editor. Its actual initialize request
uses 2025-06-18 and cannot connect to the original 2026-07-28-only endpoint. The
user explicitly selected replacing the protocol with the single Codex-supported
revision, without a bridge or parallel compatibility stack. The original stage
evidence below is historical. Current implementation/contracts use
initialize/initialized, bounded HTTP sessions, and notifications/cancelled;
HTTP disconnect alone does not cancel. Codex's own MCP client passed discovery
and PIE calls against the visible Sandbox editor, and native/SDK regressions pass.

### Codex protocol replacement acceptance

- Tested baseline: 03d2eb598 plus this protocol replacement. Windows profile
  windows-msvc-x64, Win64-Debug-DurinEditor. The single revision is 2025-06-18;
  no bridge or parallel version implementation remains.
- MCPCodecTests 8/8 passed. MCPIntegrationTests passed all 17 ordinary cases,
  both together and in isolated processes with four slots. The eighteenth case
  is the explicit SDK host and skips without its driver. MCPClientSmoke.py with
  Python mcp==2.3.0 in handshake mode passed that host, real PIE operations,
  queued cancellation, and cooperative running cancellation. Additional native
  cases verify disconnect retention, session isolation, initialized/session
  enforcement, and reserved control workers under call saturation.
- The cross-project all build passed. No shared Core/editor service API changed.
  Raw native evidence remains under ignored Build/.agent-state/logs and
  Build/MCP/sdk-native.log.
- Started the real Vulkan Sandbox editor through DevTool, with window title
  Durin - Sandbox and endpoint http://127.0.0.1:60123/mcp. Codex 0.144.1
  app-server was launched using the persistent personal MCP configuration, in an
  ephemeral context without a model turn. Its MCP client reported DurinMCP ready,
  discovered exactly four tools, and returned successful final structured results
  for status(stopped), play(playing), pause(paused=true), status(paused),
  stop(stopped), and status(stopped). The editor remains open with PIE stopped.
- Credentials and launcher stay in the user's personal Codex directory, outside
  Git. No token appears in committed files. Codex evidence is retained locally in
  ignored Build/MCP/codex-durin-acceptance.json and editor-launch.log. The initial
  sandboxed client could not write its user state directory; the successful
  acceptance used authorized execution outside the sandbox.
- Lasting protocol and client setup are documented in the adapter contract and
  connection guide. Changed-document, all-plan, and diff validation pass.

### Original stage completion summary

All stages are complete. DurinMCP exposes existing external Toolsets over local
Streamable HTTP with final business results and cooperative cancellation. The
official Python MCP SDK 2.3.0 with mcp-types 2.3.0 passed discovery, real PIE
status/play/explicit pause/stop, and deterministic queued/running cancellation
against the native CPU editor host. No required acceptance gate remains.
Resources and Prompts stay deferred and unadvertised. Physical archival is
separate from completion.
Stage 1 evidence: MCPCodecTests passes 7/7 cases covering exact numeric conversion,
malformed/bounded input, schema translation, external filtering and bounded names,
pagination, result envelopes, and per-request version/header consistency. The
production codec and private test seam use the same source; Core APIs are unchanged.
Stage 2 evidence: five native loopback/lifecycle cases passed; the sixth,
SDK-driven host case passed through MCPClientSmoke.py --discovery-only using
Python SDK 2.3.0. It is explicitly skipped when the external SDK driver is absent.
SDK validation required the revision's cacheScope and ttlMs result fields; these
are now emitted with private scope and zero TTL. Windows uses exclusive address
binding after the occupied-port test detected the library's permissive default.
Request-scoped ping SSE is covered, and a partial-body peer can be interrupted
during Stop without waiting for its remaining input. Socket closure and the
tracked descriptor set share a lock to prevent shutdown/reuse races.
HTTP runtime uses a private vendored dependency and no public Core API changes.
Stage 3 evidence: 13 native HTTP cases passed in isolated processes (four slots),
alongside MCPCodecTests 7/7. They cover final results on GameThread, independent
client IDs, queued/running cancellation, admission/domain/output errors, provider
replacement, completion/disconnect races, deadlines, Stop, closed services, and
expired results. The SDK driver also passed real PIE status/play/pause/stop.
Pending receipt ownership follows each response's RAII lifetime, keeping at most
eight calls without an additional unbounded request map. Full failure diagnostics
survive text limits in result metadata. MainFrame's Stage 2 build also passed.

### Stage 4 acceptance

- Tested production baseline: 64f4acfa9, plus this Stage 4 commit's underscore
  encoding, exclusive-port policy, standards-compliant bearer-scheme handling,
  and SDK fixture changes. These final changes were rebuilt and revalidated.
- Host: Windows, windows-msvc-x64 profile, Win64-Debug-DurinEditor. No GPU,
  application-hosted, macOS, or Linux runtime acceptance is claimed.
- CoreToolTests 19/19, EditorOperationTests 81/81, EditorHostToolTests 23/23,
  MCPCodecTests 8/8, and MCPIntegrationTests 13 ordinary cases passed. The SDK
  driver explicitly passed the remaining native SDK-host case, for 145 distinct
  cases total. That case skips only in ordinary runs without its external driver.
  The final codec cases passed isolated execution; the 13 native HTTP cases also
  passed isolated execution during Stage 3.
- MCPClientSmoke.py with Python mcp==2.3.0 and mcp-types==2.3.0 passed real HTTP
  calls and cancellation. A native running marker and observed response headers
  establish the running/queued states; cancellation acceptance does not depend
  on sleeps or quick PIE operations. The queued handler never executes, and the
  running handler observes its token. Test markers remain in harness sandboxes.
- Final cross-project all build passed, including editor startup integration.
  No shared Core/editor service API was changed. Affected-test discovery was too
  broad with stale project metadata, so the explicit relevant targets above were
  selected instead of admitting unrelated GPU/application tests.
- Raw evidence: ignored Build/.agent-state/logs, Build/MCP/sdk-native.log, and
  Build/MCP/requirements.txt (the SDK dependency snapshot). Reproduce using the
  checked-in driver and [connection guide](../../../Editor/Guides/LocalMCP.md).
- Lasting contracts live in the [MCP adapter](../../../Editor/Architecture/MCPTools.md)
  and connection guide, with routing/module ownership and external handoff updated.
  Changed-document validation, all-plan lifecycle validation, and diff checks pass.

### Stage 0 decisions and evidence

- Protocol: only 2026-07-28, using the linked normative schema/transport. Required
  surface: server/discover, ping, tools/list, and tools/call. No legacy handshake,
  GET event stream, sessions, or optional change subscriptions.
- Client: Python mcp==2.3.0 and mcp-types==2.3.0; source inspection verified
  modern version selection and cancellation in client/streamable_http.py and
  client/session.py. The SDK will drive the final native-host smoke harness.
- Dependencies: existing Core FJsonDocument/yyjson (MIT) and vendored MIT
  cpp-httplib v0.61.0. Header SHA-256:
  BC77114769DB3B3C97E4A6D753DAB797C3CADAAA48BEEAD3806298B09702DF18.
  HTTP remains a private interface dependency, with Winsock linkage on Windows
  and system threads on other hosts. No TLS is needed on loopback. No network
  fetch occurs during configure/build. Core JSON preserves uint64/int64 and
  rejects malformed UTF-8; adapter checks duplicate keys and nesting budgets.
- MainFrame explicitly starts/stops the module-owned server for its host;
  DurinMCP depends privately on DurinEd, and MainFrame privately on DurinMCP.
  A public RAII server class permits native-host integration tests without UI.
  Enable with DURIN_MCP_PORT (0 means ephemeral); absent means disabled. Read a
  32-128 character alphanumeric bearer token from DURIN_MCP_TOKEN_FILE, an
  absolute user-owned file of at most 256 bytes. Never delete that file. Log
  only the actual endpoint; client config supplies its URL and bearer header.
- Limits: 1 MiB body before JSON parsing; depth 40, 8192 nodes, 2048 entries per
  container, 128 KiB per protocol string, 1 MiB combined strings. Core's stricter
  business-value limits still apply. Response cap 16 MiB, text content cap 64 KiB,
  discovery page at most 16 tools with revision-bound cursors. Strings advertise
  native UTF-8 byte bounds as x-durin annotations, not JSON Schema code-point
  lengths. Oversized data fails instead of being truncated.
- Eight fixed HTTP workers, at most eight queued socket jobs, one request per
  connection, at most eight active tool calls; no unbounded connection-owned
  receipt map. Read/write timeouts 2 seconds; call deadline 30 seconds; completion
  polling every 10 ms. Tests can shorten the call deadline. Local Host and any
  Origin must match the actual loopback endpoint; every request authenticates.
- Names: escape each native underscore as _u, then join the two components with
  __, up to 128 bytes; longer names use the first
  92 bytes plus __h and the full XXH3-128 hex digest. Reject a colliding catalog.
  Cursors contain revision:offset and fail on catalog change. JSON integers stay
  exact on the wire; signed/number conversion is schema-directed, with no change
  to Core validation or default ownership.
- Errors: malformed JSON/envelope uses -32700/-32600; invalid params or unknown
  tool/cursor uses -32602; unsupported methods use -32601 and HTTP 404; metadata
  header mismatch uses -32020 and HTTP 400; unsupported version uses -32022 and
  supported/requested data. Native admission/domain/output/receipt failures use
  isError tool results with diagnostic code/path/message. Deadline failure is a
  tool error; SSE disconnect sends no later response. No retries or rollback.
- New MCPIntegrationTests target will link the real DurinMCP server, use the
  existing CPU editor/DObject fixture pattern, and compile the private codec
  for focused representation assertions. Real HTTP tests require no GPU/UI.
  Existing CoreToolTests, EditorOperationTests, and EditorHostToolTests cover
  shared contracts. Runtime shared API changes are not planned.

## Goal

Allow an MCP client to connect to an already running editor on loopback, discover
externally exposed Toolsets, invoke their existing handlers, and cancel pending
or running requests without blocking the GameThread. Use the existing PIE tools
as the initial real-client acceptance path.

## Scope and Ownership

| Owner | Responsibility |
| --- | --- |
| DurinMCP | Server lifecycle, local HTTP transport, MCP protocol handling, schema/value conversion, advertised names, request tracking, and access policy |
| DurinEd | Existing catalog, external admission, exact registration routing, safe-point execution, receipt retention, and cooperative cancellation |
| Domain modules | Existing Toolset descriptions, availability, handlers, domain validation, and transaction ownership |
| Editor startup/shutdown owner | Explicitly select the editor instance, start the optional MCP server after service readiness, and stop it before editor teardown |
| Registered native test targets | Codec, protocol, loopback transport, cancellation, and editor integration coverage |

Add DurinEd as a private DurinMCP dependency. Keep transport/parser dependencies
private to DurinMCP; use the repository dependency mechanism. Business modules do
not depend on MCP. Do not add another business registry, call Console text, or
introduce a general HTTP/networking module solely for this delivery.

Authoritative existing contracts:

- [Toolset contracts](../../../Runtime/Core/Tools.md).
- [Editor tool service and external adapter handoff](../../../Editor/Architecture/EditorTools.md#external-adapter-handoff).
- [Console integration regression contract](../../../Editor/Architecture/ConsoleTools.md).

Out of scope: Resources, Prompts, stdio bridges, legacy HTTP+SSE, remote hosting,
OAuth infrastructure, multiple simultaneous protocol implementations, general
C++ reflection, embedded Python, new business tools, generic long-running jobs,
persistent replay/deduplication, and tool-list subscriptions/change notifications.
Do not advertise unsupported optional capabilities.

## Current Protocol Decision

The post-completion Codex acceptance replaces the original protocol decision:
support only `2025-06-18`. Use initialize/initialized, bounded HTTP sessions,
session-scoped notifications/cancelled, and DELETE session termination. A response
disconnect does not cancel admitted work. Keep six live call streams, 32 retained
calls, and 16 sessions with ten-minute idle expiry; reserve HTTP workers for
control traffic. The implemented [adapter contract](../../../Editor/Architecture/MCPTools.md)
owns current wire semantics, limits, and lifecycle. The sections below preserve
the original staged design and acceptance history; their superseded protocol
details are not current implementation requirements.

## Original Selected Technical Design

### Protocol and transport boundary

Implement a single loopback Streamable HTTP endpoint, `/mcp`, in the editor
process. Start only when explicitly enabled through editor startup configuration;
default to disabled. Bind to `127.0.0.1`, validate Host and any supplied Origin,
and require an instance-scoped bearer token. Do not log the token or put it in
URLs. Expose the actual endpoint and a deliberate local configuration path for
the client; define its storage and cleanup in Stage 0.

Use protocol revision `2026-07-28` as the initial candidate. The
[official versioning reference](https://modelcontextprotocol.io/docs/2026-07-28/learn/versioning)
identifies it as current. It uses per-request metadata and `server/discover`;
the [Streamable HTTP specification](https://github.com/modelcontextprotocol/modelcontextprotocol/blob/main/docs/specification/2026-07-28/basic/transports/streamable-http.mdx)
removes protocol sessions and the old GET stream, and treats closure of an SSE
response stream as cancellation. Do not mix these rules with the older
`initialize`/session/`notifications/cancelled` HTTP flow.

Stage 0 must verify the intended real client supports the selected revision,
required metadata, bearer headers, and request-stream cancellation. If that client
requires an earlier revision, record the replacement revision and its complete
lifecycle/cancellation rules here before implementation. Support one revision;
do not silently add a compatibility stack. Pin normative references and the test
client/SDK version in Stage 0 evidence.

For queued tool calls, establish a request-scoped SSE response and complete it
with the final MCP response. Simple discovery/error responses may use JSON.
Implement all mandatory methods, headers, and envelopes for the chosen revision;
the named business surface is `tools/list` and `tools/call`. Unsupported optional
methods use the selected protocol's defined errors. No subscription capability
is advertised; clients can explicitly refresh the catalog.

### Server and editor lifecycle

Keep four implementation responsibilities inside DurinMCP: server lifecycle,
transport, Toolset protocol adapter, and pending-request tracking. Prefer concrete
private classes; do not create generic plugin interfaces for a single transport.
Provide a small public startup/shutdown boundary, conceptually
`Start(DEditorEngine&, Options)` and `Stop()`, with forward-declared editor types.
Stage 0 selects the exact module interface and startup owner without creating a
DurinEd-to-DurinMCP dependency cycle.

Acquire `DEditorToolSubsystem::GetService()` on the GameThread for the explicitly
selected host. Workers retain only the detached service and owned data. Each
server binds one editor; separate editors use distinct endpoints/tokens. Module
load alone must not open a listener. Report bind/configuration failures without
preventing normal editor use, and permit a clean stop/start.

Stopping rejects new HTTP work, closes active request streams, cancels tracked
invocations, and joins transport callbacks/workers before module unload. Stop
must not wait on a GameThread tick while holding that thread. Endpoint closure or
editor shutdown completes/terminates pending responses deterministically. Follow
the existing main-thread registry contract; adapter synchronization is owned by
DurinMCP rather than a registry retirement mechanism.

### Discovery and schema conversion

Build discovery exclusively from the owned `GetCatalog()` snapshot, filtering
`bExposeExternally`. Preserve deterministic ordering and bind the description,
name mapping, and ContractVersion to one snapshot. Do not filter transiently
unavailable tools out of the list; execution checks availability through the
service. Refresh on catalog revision changes without retaining provider pointers.

Use `<escaped-toolset>__<escaped-tool>` for names, such as `editor.pie__pause`.
Stage 4 source review corrected the earlier Stage 0 assumption: native segments
do allow underscores. Escape every native underscore as `_u` before joining, so
the `__` separator cannot occur inside a component. This preserves ordinary PIE
names and distinguishes otherwise ambiguous valid native addresses without
compatibility aliases. Apply the bounded hash encoding after escaping, detect
collisions, and retain an owned name-to-address map. Never silently truncate names
or omit otherwise valid tools. Transport names do not replace `FToolAddress`.

Translate the entire supported Core schema subset to JSON Schema, including
closed objects, required fields, defaults, arrays, enums, numeric bounds, and
type-specific size bounds. Core remains the validation/default-preparation owner;
the adapter only validates the wire envelope and converts bounded values. Preserve
signed/unsigned 64-bit integers without a double round-trip and reject non-finite
numbers, duplicate object keys, malformed UTF-8, and excessive input before or
during bounded parsing. Document client-side large-integer limitations honestly.

Advertise output as an object envelope with a `data` property whose schema is
the native Output schema, so scalar, array, null/void, and object results all work.
Success returns `structuredContent: {data: ...}` plus a bounded text content block
for client presentation, using the selected revision's required result fields.
Map native effects/retry policy to annotations conservatively; annotations grant
neither access nor permission for automatic retries.

### Invocation, errors, and cancellation

Resolve a tool name, construct an owned `FToolRequest` with the current advertised
ContractVersion, and call `Submit(Request, EToolSource::External)`. The service
enforces exposure again and prepares arguments once before dispatch. Bind trusted
source/host context on the server; business arguments cannot override it.

Maintain a bounded mapping from an internal HTTP request identity to the native
invocation receipt. JSON-RPC IDs alone are not globally unique across clients.
Poll `GetInvocation()` on a bounded worker/event-loop schedule without busy
spinning or blocking the GameThread. Return the final result, never an admission
receipt presented as business success. Keep internal receipt IDs off the public
API; any diagnostic representation uses opaque strings rather than JSON doubles.

Protocol/transport errors, unknown tool names, admission failures, domain failures,
cancelled work, and missing/expired receipts need an explicit mapping table using
the selected specification's error rules. Preserve native diagnostic code, path,
and message within bounded tool errors. Do not infer success or retry mutations
when a result is lost. A catalog refresh cannot redirect an admitted invocation
to a replacement provider; retain the service's exact-registration semantics.

For the candidate revision, closing a call's SSE response stream invokes native
`Cancel()`. A server deadline or server stop also cancels the corresponding work.
Queued cancellation prevents execution; running cancellation signals the token
cooperatively and cannot roll back effects. Completion/cancellation races must
produce at most one terminal HTTP response and release tracking state once.
Do not introduce a public `cancel` business tool or native receipt-polling tools.

Define finite request-body, serialized-response, connection, pending-call,
discovery-response, deadline, and polling limits in Stage 0. Include serialization
overhead and match the existing service budgets. Exhaustion must reject work
before unbounded allocation or admission; never truncate structured business data.
Use snapshot-bound pagination if needed to advertise the complete bounded catalog.

## Implementation Stages

### Stage 0: Protocol, dependency, and lifecycle decisions

Depends on: no implementation stages.

- [x] Verify a real client's protocol, authentication, and cancellation support;
  pin one revision, normative references, and client/SDK version.
- [x] Select a maintained HTTP/SSE and JSON solution after checking existing
  dependencies, integration cost, license, integer fidelity, and cancellation.
  Record the pinned dependency and platform/build implications.
- [x] Select the explicit editor startup/shutdown owner and module loading path;
  define options, token handling, endpoint discovery, and failure behavior.
- [x] Record exact protocol methods, headers, errors, bounded naming, pagination,
  numeric behavior, and finite resource/deadline/polling limits.
- [x] Select registered native test targets and fixtures following
  [testing guidance](../../../Agents/Testing.md); identify a real-client smoke harness.

Completion condition: all decisions above are recorded here with no unresolved
protocol, dependency, public ownership, or unload-safety issue.

### Stage 1: Protocol codecs and Toolset discovery

Depends on: Stage 0.

- [x] Add private dependencies and bounded protocol/value/schema codecs.
- [x] Implement snapshot-based external filtering, deterministic name mapping,
  complete schema translation, and `tools/list` result generation.
- [x] Implement mandatory server discovery/version handling and the error table
  for the selected revision, without opening a listener yet.
- [x] Cover all native schema/value kinds, exact integer boundaries, defaults,
  malformed/budget-limited input, long names/collisions, hidden tools, catalog
  refresh, empty catalogs, and pagination if selected.

Completion condition: codec/discovery tests pass; supported native tools have
valid complete MCP descriptions and no editor/provider object reaches codecs.

### Stage 2: Local HTTP server and editor lifecycle

Depends on: Stage 1.

- [x] Add the optional explicit-host startup boundary and `/mcp` listener.
- [x] Implement selected-revision HTTP requirements, request-scoped SSE,
  authentication, loopback/Host/Origin policy, and bounded request admission.
- [x] Integrate editor readiness, bind errors, stop/start, and stop-before-teardown
  ordering; synchronize callbacks/workers before unloading code.
- [x] Exercise real loopback requests for discovery, malformed requests, wrong
  version/headers/token/origin/host, occupied ports, deadlines, and clean restart.

Completion condition: a real client discovers the server and existing external
PIE tools; disabled configuration opens no listener and shutdown leaves no workers.

### Stage 3: Structured invocation and cancellation

Depends on: Stage 2.

- [x] Connect `tools/call` to external Submit and bounded completion tracking;
  return the native result through the documented output envelope.
- [x] Map execution, availability, admission, expired/unknown receipt, and
  shutdown outcomes; prevent implicit retries and duplicate submissions.
- [x] Connect request-stream cancellation/deadlines/stop to native Cancel;
  resolve completion races and free all request-owned state.
- [x] Test queued cancellation without effects, running cooperative cancellation,
  simultaneous clients reusing RPC IDs, result/disconnect races, replaced
  providers, closed editor service, timeout, limits, and output failures.

Completion condition: real loopback calls execute only through editor safe points,
return final business results, and support bounded cancellation and teardown.

### Stage 4: Real-client integration and contract handoff

Depends on: Stage 3.

- [x] Run the pinned real client against PIE status/play/pause/stop, explicitly
  passing `paused`; verify state through the same shared domain handlers.
- [x] Demonstrate client cancellation with a deterministic test Toolset held
  queued/running; do not infer cancellation coverage from fast PIE calls.
- [x] Run relevant Core, editor-service, Console, and MCP regressions; build
  affected editor/project targets following [build guidance](../../../Agents/BuildAndRun.md).
  Complete an `all` build if a shared Engine API migration becomes necessary.
- [x] Publish implemented architecture in `Editor/Architecture`, connection and
  token setup in an editor guide, and update relevant routing/module ownership
  entries and the existing external-adapter handoff.
- [x] Record tested revision, client version, selections, reproduction settings,
  and remaining limitations here; retain raw evidence under ignored `Build/`.
- [x] Pass documentation and all-plan lifecycle validation, close only verified
  checklists, and mark this plan completed when all required gates pass.

Completion condition: a documented local-client workflow covers discovery,
invocation, cancellation, and editor closure; required regressions/builds pass.
Resources and Prompts remain unadvertised and deferred.

## Validation and Commit Discipline

Follow [documentation workflow](../../../Agents/Documentation.md) and the linked build
and test guidance rather than introducing another command workflow. This initial
plan requires changed-document and all-plan validation only; it makes no runtime
claims. During implementation, update Current Status and stage evidence in each
stage commit, using this exact plan path and stage heading for commit trailers.
Keep completed stages independently reviewable and do not mark real-client or
cancellation acceptance complete based solely on mocked protocol tests.
