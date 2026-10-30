# Editor Tool Service

Summary: Define per-editor tool discovery, exact provider routing, bounded cross-thread submission, and synchronous GameThread execution.

Modules: Core, DurinEd

Last reviewed: 2026-10-30

## Ownership and Entry Points

DurinEd registers `DEditorToolSubsystem` before editor subsystem initialization.
The active editor owns one service shared by windows, clients and PIE controls. MainFrame's
[Console adapter and LevelEditor PIE tools](ConsoleTools.md) and the
[MCP adapter](MCPTools.md) consume this service. [Core tool contracts](../../../Runtime/Core/Tools.md)
remain the authority for values, schemas, provider interfaces, and feature lifetime.

Acquire `GetService()` on GameThread and retain its shared native
`FEditorToolService` endpoint for external threads. Construction binds a weak
reference to its owning editor once; dispatch does not accept a caller-selected
editor. Only open GameThread dispatch resolves that reference. The endpoint
stores no provider pointer. `GetCatalog`, `Submit`, `GetInvocation`, and `Cancel`
use detached data and a mutex; they never call ModularFeature or resolve DObjects.
Retaining the endpoint retains its subsystem work/code gate. Release endpoints
as part of adapter shutdown before requesting provider module unload.

The subsystem's `ListToolsets`, `DescribeToolset`, `DescribeTool`, `CheckAvailability`, and
`CallNow` run on GameThread. Wrong-thread discovery/execution returns a
diagnostic before accessing host state; acquiring the endpoint on another thread
is a contract violation. Immediate execution is independent of pending queue
capacity and returns an owned result directly, without retaining a result receipt.
`CanCallNow` reports an idle host safe point with no same-service dispatch in
progress. This predicate and immediate/queued dispatch use the same service-owned
host-state check. Execution still rechecks live state after availability and
between calls. Adapters may otherwise queue once; they must not retry a provider result.

## Catalog Publication

Initialization, GameThread listing, and tick dispatch refresh the catalog when
the global feature publication revision changes. Metadata is immutable during a
registration; providers re-register to change descriptors. Worker-side catalog
reads use the latest published snapshot, which may lag registration until a
GameThread refresh. Reentrant listing during feature dispatch reads the previous
snapshot rather than recursively describing providers.

Discovery copies one owned `FToolsetDescriptor` from each `InvokeAll<IToolset>`
registration. `FEditorToolCatalog.Toolsets` holds grouped descriptions and exact
`ToolsetIdentity` values, sorted by toolset name with tools sorted by local name.
`FeatureRevision` is the global registry publication revision; the catalog has
its own monotonic Revision. Internal routes retain owned descriptors and exact
registration identities, never toolset objects or callbacks.

Calls use `FToolAddress{Toolset, Tool}`. Both names are case-sensitive and limited
to 256 bytes. Different toolsets may have the same local tool name. Duplicate
toolset namespaces exclude every registration claiming that namespace, even when
one has invalid metadata. Within a unique toolset, duplicate local names exclude
every candidate with that name, including invalid descriptors; unrelated tools
remain available. Invalid tools are diagnosed and excluded. Empty toolsets remain
visible. Description exceptions become diagnostics. Exceeding catalog capacity
publishes an empty catalog with a capacity diagnostic.

Submitting checks the tool/version, schema, defaults, source, input bounds,
and external-exposure flag against that snapshot. External exposure is not an
authorization system; future adapters own access policy. A queued request keeps
its admitted descriptor and exact provider identity. Replacing a provider never
redirects accepted requests to its replacement: dispatch reports unavailable.

## Dispatch and Host Context

Editor Tick calls `TickTools` immediately inside its host operation scope,
before retired PIE release and world ticking. Dispatch requires a Ready editor
collection, open work gate, and an abstract transactor accepting `CanTransact()`.
Idle and ordinary Recording admit work, allowing nested native composition.
Replay, recovery, and shutdown defer queued work.
Execution rechecks host admission between availability checking and execution,
and before every queued invocation. Same-service reentrant execution returns Busy.

All provider callbacks run through the active editor's `DispatchSubsystemCallback`, preserving host
and GC lifetime through synchronous return. A provider may request host shutdown;
gates close immediately while destruction waits for the host boundary. Tick stops
before subsequent editor/world work after such a request. A native immediate
callback defers cleanup until the next host Tick/shutdown boundary, matching the
[subsystem lifecycle](../Framework/EditorSubsystems.md).

`GetToolEditor(Context)` resolves the admitting editor only during the exact
provider availability/execution callback for that context object. It returns
null for a copied/fabricated context, another thread, or outside the callback.
The callback binding stores only the exact invocation context; the editor comes
from the active process editor protected by the enclosing callback lifetime.
Do not retain its editor pointer. Core context stays engine-independent; provider
implementations receive the active editor through their validated callback context. Client/session identities remain independent of editor identity.

Provider exceptions become InternalError. Availability failure prevents execution;
execution still owns authoritative domain checks and transactions. Successful
output must satisfy its declared schema. All output values and diagnostics are
bounded, including failure output; invalid output and oversized diagnostics fail
the invocation. Ready and delayed Future outcomes pass the same final validation.
CallNow rejects may-pend descriptors before provider callbacks. A synchronous
provider returning pending fails without retry or a rollback claim.

## Queue, Results, and Cancellation

Each accepted queued request gets a host-session/monotonic-sequence identity.
Submission never waits for GameThread execution. Lookup returns Queued, Running,
Completed, Cancelled, or Expired with an owned result when available. Wrong-session,
zero, and future identities are unknown; an issued identity with no retained
receipt is expired. No unbounded tombstone list is kept.
`RetainInvocation` obtains the same validated native Future used by receipts; it
retains completion after TTL eviction. Lookup snapshots expose the same
`shared_ptr<const FToolResult>` allocation and accounting; copying a snapshot
does not copy the result tree. There is no optional-like result wrapper or
implicit owned-result conversion. MCP retains this allocation through bounded
protocol serialization; synthetic transport errors have separate owned storage. Immediate invocation
contexts have unique sequences but no retained receipt.

Queued cancellation removes the queue item and publishes a cancellation receipt.
Running includes dispatched pending Futures. Completion observations mark native
work ready using Core-owned `FFutureReadiness` markers; TickTools validates and
publishes on GameThread. Each invocation owns a move-only promise and shares its
standalone future with receipts. Close detaches observations and resolves public
receipts without joining a provider that needs GameThread. Accepted provider work
and module joins remain the provider's responsibility. Late completion retains
only independent future storage; retained receipt views keep their accounting
pins after service destruction. Running cancellation only signals the provider's cooperative token; it does not
interrupt execution, imply rollback, or overwrite successful domain work. No
service lock is held while calling a provider, so a worker can query/cancel a
running request without waiting for provider return.

Defaults in `FEditorToolLimits` are 256 queued requests, 64 pending calls with
16 MiB retained input/routes, 1024 live terminal slots, 1024 completed receipts, 128 catalog toolsets, 256 total catalog tools, 16 MiB catalog metadata accounting, 16 MiB queued
payload/route accounting, 32 MiB completed result accounting, and five-minute
receipt TTL. Core value/schema
limits apply independently before input copying and after normalization. Queue
accounting includes the retained descriptor cost for each admitted request;
pending work retains that charge until completion/detach releases its request
and route. Terminal admission reserves 1024 diagnostic bytes per slot. Final
publication charges allocated result size or uses the reserved bounded error.
Live consumer Futures, result aliases and snapshots retain their charge after
receipt eviction; admission prunes receipt ownership under pressure and applies
backpressure when consumer pins exhaust capacity. Accounting includes reserved string
and vector capacity; oversized provider results fail before retention. Counts
also bound container overhead. Catalog size is bounded by tool count, its
aggregate metadata budget, and per-descriptor Core metadata limits.

Tick dispatches at least one available item, then checks a 2 ms budget between
calls, with a maximum of 16 invocations per tick. This is not preemption or a
runtime bound on a single domain handler. Diagnostics are limited to 64 entries,
4096 message bytes and 1024 path bytes each. `Configure` runs on GameThread with
no queued/running/pending calls, validates finite positive capacities and live
consumer charges, prunes results,
and invalidates the catalog for the next refresh. Zero TTL is allowed.

When the work gate closes, admission stops immediately. Deinitialization or an
endpoint observing closure clears previous completed data and the catalog,
cancels queued work, and retains bounded cancellation receipts. Running work only
receives cooperative cancellation; lifecycle scopes keep the host alive through
return. Pending observations safely detach owned data with bounded cancellation
outcomes; late producers cannot access the host. Detached endpoints remain safe after host destruction and GC. They never
reopen and cannot reach a replacement host or provider. Modules still own their
workers, callbacks, and unload safety under the main-thread feature contract.

## Script Execution Boundary

After TickTools publishes its receipts, the editor calls TickScripts. Native CPU
hosts must pump that boundary explicitly. IEditorScriptPump callbacks run outside
ToolService provider execution and can invoke CallNow synchronously or SubmitNow
for exactly one ready/pending invocation. SubmitNow reserves native pending/result
capacity before effects and returns identity plus its retained validated Future;
it never drains another request or ticks the editor. The host
callback and feature dispatch scopes retain host/code lifetime through return;
same-service native provider reentrancy remains rejected. One script step may
block UI; explicit continuations return to this pump on later ticks. Version 2
also supplies CloseEndpoint during host deinitialization for suspended job cleanup.

Only editor.python status/cancel has a detached control path while a script pump
is active. The endpoint reuses request/source/version/schema validation and checks
the bound catalog registration identity. Catalog refresh precedes the pump and
feature mutation/unload is pinned during it. The callback may inspect owned job
state and cancellation tokens only; it receives no DObject access. Outside that
boundary these tools queue normally. See [Python ownership](PythonTools.md).

## External Adapter Handoff

`DurinMCP` consumes this service through its private DurinEd dependency. Its
[local MCP adapter](MCPTools.md) owns authenticated Streamable HTTP, tool
advertisement, wire conversion, and request cancellation. It reuses native
business handlers and never invokes Console text. The following handoff remains
the shared contract for external adapters.

| Adapter responsibility | Existing native entry point | Thread and data contract |
| --- | --- | --- |
| Bind one editor | `DEditorToolSubsystem::GetService()` | Acquire on GameThread from the explicitly selected host; retain its detached endpoint |
| Discover groups/tools | `ListToolsets`, `DescribeToolset`, `DescribeTool`; endpoint `GetCatalog` | Subsystem discovery refreshes on GameThread; worker reads use the last owned catalog snapshot |
| Build a request | `FToolAddress`, `FToolRequest`, Core schemas | Keep Toolset and Tool separate; send the selected tool's ContractVersion and owned business Arguments |
| Submit external work | Endpoint `Submit(Request, EToolSource::External)` | Worker-safe admission and queuing; return a receipt without waiting for another GameThread tick |
| Read completion | Endpoint `GetInvocation(Id)` | Worker-safe owned state/result; distinguish submission rejection, domain failure, cancellation, unknown identity, and expired receipt |
| Cancel/disconnect | Endpoint `Cancel(Id)` | Cancel queued calls or signal running calls cooperatively; retain only bounded connection-owned receipt bookkeeping |

Adapters must filter advertised tools using each descriptor's `bExposeExternally`
flag. `GetCatalog` includes the full native catalog rather than applying transport
policy. External submission enforces the flag again. Effects and RetryPolicy describe behavior;
they are not authorization or permission to retry a failed call. The adapter owns
transport access policy and constructs trusted source/context information;
clients cannot override trusted context through business arguments.

The Core schema vocabulary and [validation limits](../../../Runtime/Core/Tools.md)
are authoritative. Translate schemas and detached values at the protocol boundary,
bound serialized input before parsing, and preserve signed/unsigned 64-bit values.
Do not retain parser views, DObjects, native handlers, or registry visitor
references on transport threads. Format session/sequence receipts as opaque
strings rather than lossy JSON floating-point numbers. Catalog revision and exact
ToolsetIdentity are process-local routing metadata, not persistent tool IDs.

Queued requests retain the admitted descriptor and exact registration identity.
Refreshing discovery must not redirect an existing receipt to a replacement
provider or editor. On shutdown, stop producer admission, cancel connection-owned
queued work, release endpoint references, and synchronize adapter workers/callbacks
before module unload. Running work is cooperative and may have completed domain
effects; disconnect, timeout, or invalid output does not promise rollback.
An invocation receipt and a retained Future observe one terminal service result.
The domain remains responsible for its producer and module lifetime.
