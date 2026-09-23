# Render Graph

Summary: Define the deterministic frame-local graph compiler and its boundary with RHI execution state.

Modules: RenderCore, RHI

Last reviewed: 2026-09-23

## Reading and Calling the Graph

The authoring sequence follows the same setup/execute separation as
[UE Render Dependency Graph](https://dev.epicgames.com/documentation/en-us/unreal-engine/render-dependency-graph-in-unreal-engine):

1. Create a frame-local `FRDGBuilder`; register external resources or declare
   graph-owned textures, buffers, and values.
2. Call `AllocParameters<T>()`, fill the declared inputs and outputs, then move
   the parameter reference into `AddPass` or `AddRecordingPass`. Repeat in
   producer-before-consumer order. Callbacks run later during execution.
3. Queue extractions for resources needed outside the graph, or mark passes with
   external effects as roots. Call `Execute(CommandList, Allocator)` once and
   check its result before using extracted outputs.
4. Inspect `Capture()`, `GetStatistics()`, or the execution result on the same
   builder when diagnostics are needed.

`AddPass` receives an immediate command list. `AddRecordingPass` receives an
owned recording list; its optional `Parallel` policy permits independent CPU
recording. Both consume typed parameters and derive resource dependencies from
those parameters. CPU parallel recording and async-compute queue eligibility
are separate declarations.

Start in `Public/RDG/RDG.h` for this interface in call order. Its included
`RDGBuilder.inl` owns template construction and callback adaptation. Internally,
`RDGCompile.cpp` validates declarations, derives dependencies, culls passes, and
builds the logical plan. `RDGExecution.cpp` makes the physical stages explicit:

- `Execute`: single-use lifecycle and original execution result.
- `Record`: ordered orchestration of the following stages and phase timing.
- `AllocateResources`: acquire and validate the complete retained resource batch.
- `PrepareExecution`: resolve barriers and prepare queue transfers and waits in
  an execution-local `FExecutionContext`, before recording any callbacks.
- `RecordPasses`: record callbacks and the epilogue in scheduled batches, including
  dependency-safe parallel recording waves.
- `PublishExtractions`: attach allocation retirement and publish output references
  only after successful recording of all batches.

These are private implementation stages, not additional caller obligations.
Physical preparation does not modify the logical execution plan; preparation
failure cannot publish extractions or invoke pass callbacks.

## Ownership Boundary

Public headers follow the contracts they expose. Include `RDG/RDG.h` when authoring
or executing a graph through `FRDGBuilder`; narrower consumers use:

- `RDG/RDGDefinitions.h` for shared enums, resource descriptions, and graph handles.
- `RDG/RDGParameters.h` for parameter wrappers, metadata/layout construction,
  resolution, shader scopes, and metadata errors.
- `RDG/RDGAllocator.h` for allocation requests, results, retirement, and allocator errors.
- `RDG/RDGExecution.h` for compilation/preparation/execution errors and results,
  budgets, dependencies, barriers, and the logical execution plan.
- `RDG/RDGDiagnostics.h` for owning captures and observational statistics.

Errors belong to the contract that produces them; shared definitions do not
collect error domains. `RDG/RDG.h` includes these contracts, preserving the complete
graph-authoring entry point. Narrower headers do not include `RDG/RDG.h`.

`FRDGBuilder` owns declarations, parameters, typed values, callbacks, compiled
records, and retained resource references for one graph execution. Handles are
valid only for their originating builder. There is no public compile operation
or independently owned executable graph. Frozen resource declarations
remain in builder storage; compiled records borrow them and keep physical
backing and allocation observations in a separate execution table.
Compiled passes and the final epilogue own `FRDGBarrierBatch` records. Each
`FRDGBufferTransition` or `FRDGTextureTransition` carries its graph-local
resource ID, exact range, access handoff, and discard intent together. There
are no parallel resource-ID arrays or physical pointers in these batches.
`GetPasses()` and `GetFinalBarriers()` expose the same logical plan before and
after preparation and recording. One recording boundary resolves validated
backings into reusable temporary RHI batches; command recording copies those
payloads without modifying the compiled plan.

Graph-created textures and buffers use description-first `CreateTexture`/`CreateBuffer` declarations.

Non-const `Execute(CommandList, Allocator)` compiles, prepares retained
resources, records passes, and publishes extraction outputs. The optional
`FRDGAllocator*` supplies resource allocation; omitting it is valid only when no
retained logical RHI resource requires allocation. The allocator receives one
name-free batch of exact retained descriptors. The builder holds the complete
returned reference table through its own lifetime; allocator borrowing and RHI
retirement rules still apply.

The thread-confined lifecycle is Building -> Compiling -> Preparing -> Recording
-> Recorded, with terminal Failed on supported failures or C++ unwinding. Every
execution attempt consumes Building, including compile and preparation failure.
`Execute` returns `FRDGExecutionResult`, an alias for
`std::expected<void, FRDGExecutionError>`. Success has no diagnostic payload.
Only this boundary combines compile, preparation, and execution-state errors;
`GetRDGExecutionStatus()` derives CompileFailed, PreparationFailed, Recorded,
or InvalidState from the result and its active error alternative. There is no
independently stored execution status. `GetExecutionResult()` and Capture hold
an optional original result: absence means Execute has not started, including
when only the native test compiler has run. Diagnostic text never determines
success.
Recorded means CPU recording succeeded, not GPU completion.
A second or reentrant Execute returns InvalidState before allocations, commands,
callbacks, or extraction, without changing the original execution report or
compiler evidence. Supported exceptions propagate with terminal Failed state;
interrupted recording retains an InvalidState report and incomplete-recording
error. Fatal authoring assertions are not recovered.

Reentry from parameter/value constructors consumes the builder with CompileFailed
while storage construction is incomplete; storage cleanup still occurs exactly
once at builder destruction.

All declaration and mutation methods require Building through release-enabled
`requiref`, including parameter submission and allocation, resource/value/token
creation, extraction, uses, dependencies, roots, culling, and budget edits.
Callbacks and allocators cannot reopen authoring. The state gate does not make
concurrent calls thread-safe. A retry requires a fresh builder and fresh graph
declarations. External initial access must match the actual ordered entrance
state; a fresh builder cannot discover unrelated external uses or infer the
previous graph's final access.

Capture, Dump, statistics, budgets, and compiler records remain inspectable on
the builder. Before compilation or after compile failure, Capture has
`bCompiled = false`, empty compiler records, and the declared budget; Dump
reports no compiled plan. Partial compiler output is never published.
Successful compilation retains `bCompiled = true` evidence after preparation
failure. An owning `FRDGCapture` survives builder destruction. Native compiler
tests use a test-only friend accessor defined in the native fixture that returns only diagnostic
success/error evidence and seals the builder; production has no compile-only
entry point.

The graph is the declaration and scheduling authority. It emits existing
`FRHIBufferTransition` and `FRHITextureTransition` descriptors, while RHI and
the active backend remain authoritative for validating and committing actual
execution state. Compilation never mutates a command list.

## Resource Contract

- External resources registered through `RegisterExternalTexture` or
  `RegisterExternalBuffer` declare exact initial/final access and are retained
  strongly by physical identity. A missing final access is a compile error. Repeated registration
  of the same non-null physical texture or buffer returns the first graph
  handle when kind, physical description, initial access, and final access all
  agree. The first name and declaration order remain canonical. A conflicting
  repeat records one deterministic declaration error naming both stable
  contracts; null imports retain the ordinary missing-resource failure and do
  not become identity keys. CPU-authored buffers are rejected with
  `ExternalBufferContentModeInvalid` at declaration, because changing content
  versions do not satisfy this physical-identity access contract. Use
  graph-created buffers and owned graph uploads for graph-managed contents.
- Graph-created resources begin at `ERHIAccess::Discard`, require a stored
  producer before any read or load, and default to no final state (`ERHIAccess::None`). Renderer frame-local
  targets use that default; consumers declare their next access on the pass.
  Explicit final states remain for external boundaries and extraction.
- `QueueTextureExtraction` and `QueueBufferExtraction` make a resource an
  explicit terminal consumer (`RDG.Export`) of its complete byte or
  aspect/mip/layer range. This node requires valid stored texture contents in every
  cell, retains the final producers through ordinary value dependencies,
  extends allocation lifetimes through export, and applies the requested final
  access. It appears in scheduled passes, captures, and structural budgets;
  an output-free graph has no export node. External resources with discarded
  initial contents also require a stored producer. A counted reference is
  published to the destination only after complete successful execution. Compile, preparation, allocation, or recording failure leaves
  every destination unchanged. Duplicate resource or destination extraction
  is a deterministic declaration error.
- Every use declares one nonempty exact byte range or texture
  aspect/mip/layer range. Texture tracking uses fixed aspect/mip/layer indices;
  distinct subresources remain independent. Buffers use one resource-wide
  dependency and barrier state. Byte ranges remain authoritative for bindings
  and parameter authorization, but do not establish independent graph resources.
  Buffer initialization is checked separately using coalesced byte intervals.
  Reads require prior stored writes or imported contents covering every declared
  input byte; extraction requires complete buffer coverage. Same-pass writes
  cannot initialize that pass's inputs. This validates declared coverage, not
  shader execution or the values actually written by callbacks.
- Required access cannot contain `Discard`. Discard is producer intent and
  is carried separately from the expected-before access state.
- An attachment `Load` requires prior contents. A `DontCare` store invalidates
  its contents and cannot satisfy a later producer requirement.

## Compilation and Ordering

The executable sequence is the declaration sequence filtered by retention.
`AddPassDependency(Producer, Consumer)` is legal only while Building and requires
valid handles from this builder with `Producer.Index < Consumer.Index`. It may
be called after both passes are declared. Retaining the consumer retains its
producer; duplicate edges are idempotent. Invalid/foreign handles, self edges,
and backward edges produce deterministic compile errors even on unreachable
passes. Compilation failure consumes the builder and publishes no partial result.

Migrate `AddDependency(Consumer, Producer)` to
`AddPassDependency(Producer, Consumer)`; the old API is removed. A formerly
backward declaration must move the producer before the consumer, with resource
versions and callback capture lifetimes reviewed. Explicit edges cannot repair
a resource read declared before its producer.

Each texture subresource and whole buffer carries a produced-value version.
Value edges connect a producer to readers and read/write consumers; explicit edges also participate
in reachability. A separate minimal execution frontier preserves required RAW,
WAR, and WAW order without making overwritten values reachable. A discard
write starts a new version. Buffer writes conservatively retain the previous
producer for partial updates, including disjoint and partial discard writes.
A single full-buffer discard declaration can sever that value dependency,
while preserving required execution ordering for retained passes.
Same-pass buffer declarations combine
access masks and advance the resource version once. Every generated dependency
also points forward;
compilation performs no reordering. A same-range overwrite chain therefore
produces linear rather than all-pairs dependencies.

Compilation fails as one complete result for invalid or foreign handles,
unnamed or duplicate identities, missing producers, illegal access/use pairs,
overlapping declarations within a pass, invalid normalized ranges or usage,
pass-domain/access mismatch, and invalid dependency direction. No pass callback
runs and no transition records when compilation fails.

Logical tokens express compatibility-only ordering edges without transitions
or backend state. Graph-owned typed values use those same value versions,
producer/consumer edges, execution frontiers, culling closure, and logical
lifetimes for non-RHI outcomes.

Pass culling is opt-in and root-driven. Present, offscreen output, temporal
publication, readback, capture, timestamps, and other external effects mark an
explicit root reason. Compilation retains each root and its complete reverse
Value/Explicit predecessor closure; Execution edges do not propagate retention.
Other passes are reported as unreachable. Full declaration validation precedes
culling. Resource-use indexing stores one contiguous use-pointer array and a prefix-offset
table, preserving declaration order within each resource. Explicit inspection
reuses the same index builder. Predecessor slices use contiguous pass indices
and prefix offsets, built once from finalized edges, including any
Execution-to-retaining upgrades. Each pass is marked before enqueueing and
expanded at most once. Ordering costs O(P); retention indexing and traversal
cost O(P + E) after edge generation. With culling disabled all passes are retained
and predecessor lists are not allocated. These bounds do not describe the entire
compiler, range analysis, transitions, or capture generation.

Every resource reports its first/last retained scheduled pass. A resource used
only by unreachable passes reports a culled lifetime. These logical intervals
do not authorize physical aliasing.

## Transition and Execution Contract

Each compiled pass owns the buffer and texture transition batches that precede
its callback. State is tracked per texture subresource and whole buffer. A
discard producer
sets `bDiscardContents` and preserves the prior compiled access, including
after a `DontCare` store. Content validity and value reachability are separate
from execution dependencies and access state. Only a logical resource's first
use carries the compatibility `Discard` wildcard so the backend can recover
the physical allocation's prior accesses on pool reuse. Final transition batches restore each used imported
or explicitly finalized texture subresource or buffer used by retained passes.
Unused texture subresources receive no final transition. Partial buffer discards
never discard the whole resource; buffer barriers cover the complete allocation.

`FRDGBuilder::Execute` records each pre-pass batch, invokes the pass
callback with a pass-scoped resource view, and then records final batches.
`QueueBufferUpload` copies a byte span; `QueueBufferUploadOwned` moves a byte
buffer. Each creates a Copy recording pass with an exact `TransferWrite` byte
use and an RHI `UploadBuffer` command. The destination must declare
`DestinationCopy`. A single upload owns at most 16 MiB and a builder admits at
most 32 MiB of queued CPU payload capacity. Sources also share the process-wide
32 MiB buffer-upload budget with CPU-authored snapshots and native upload
commands. Invalid ranges, missing copy usage,
or excess payload fail compilation before any upload command is recorded.
`CreateStructuredBuffer` and `CreateStructuredBufferOwned` create a buffer
with structured, shader-resource, and copy-destination usage, then queue its
complete initial contents through the same path. Their byte size must be a
nonzero multiple of the element stride.
After culling and scheduling, consecutive upload helpers on the same logical
queue form one execution-plan submission and owned RHI command list, with
at most 64 uploads and 16 MiB of source bytes per list. Pass handles, exact
uses, dependencies, and barriers between overlapping writes remain distinct.
Batching stops at other callbacks, queue changes, and either limit. It reduces RHI
command-list batches without merging destination ranges or GPU copy commands.
The graph and recorded RHI command share one immutable source allocation.
Recording at a full admitted budget does not require another source copy.
Graph destruction or cancellation releases its ownership; bytes remain charged
until the final graph or command owner releases them.
`AddRecordingPass` instead receives a regular owned `FRHICommandList` and the
same frozen typed parameters/resolver. Its callback must close every render pass,
diagnostic region and timing query; the graph seals and queues the list only after
the callback returns successfully. The list owns captured command resources after
builder destruction. An exception destroys that callback's unpublished list and
leaves the graph failed. This does not roll back previously queued passes.
These callbacks default to serial recording. `ERDGRecordingPolicy::Parallel`
declares an independent CPU callback: it may read frozen parameters/resources and
access declared typed values, but cannot mutate renderer caches, global telemetry
or graph metadata, launch child tasks, or depend on RHI/owner-thread progress.
Eligible consecutive callbacks form waves of at most eight. Every compiled
dependency, including typed-value and explicit edges, cuts a wave before its
consumer; serial callbacks also cut waves. Single-pass waves and unavailable
schedulers run inline. Worker results are joined in pass order and the complete
wave must succeed before any of its lists is queued. Failure or cancellation
discards that wave and drains every launched task; earlier queued waves remain.
Task-handle storage is reserved before dispatch so allocation failure cannot
orphan work borrowing the graph. A failed graph remains consumed: later callbacks
and extraction publication do not run, and another `Execute` cannot replay its
accepted prefix. Queued command payloads retain their owners independently of
the graph and release them when replay storage retires.
Immediate-only `AddPass` callbacks remain supported. Barriers,
queue ownership and submission assembly remain on the immediate timeline; an
owned recording callback cannot use immediate allocation, readback or flush APIs.
Assembly follows `QueueCommandList` admission rules: the immediate list must not
be inside a render pass, diagnostic region or timing query when the owned list
is inserted. Migrating a caller with surrounding scopes requires moving those
scopes to a supported recording boundary first.
Lookup of a foreign, undeclared, incorrectly typed, or unavailable handle is
an unrecoverable authoring-contract failure. Graphics, compute, and copy passes
accept only their corresponding graphics/attachment, compute, and transfer
access families.

After successful private compilation, the execution allocator receives immutable `FRDGAllocationRequest`
records containing resource ID, kind, exact description, retained lifetime,
observation tag, and explicit `bExtracted` ownership intent. For independent
queue execution, execution-local copies additionally carry a shared
`FRDGAllocationRetirement` proof; compiled logical requests remain free of runtime sync points.
Allocators detach extracted allocations from reusable storage before returning
success; counted references then own the exported resource, with no implicit
return to the pool when those references expire. Renderer may promote an
existing compatible pool entry into an export. A later recording failure still
leaves that allocation detached and does not publish the extraction destination.
Ordinary allocations are borrowed for one ordered execution; preserving their
contents across executions requires extraction, not merely a strong reference.

Pool reuse on the immediate RHI timeline relies on ordered command recording
and backend transitions. Across independent timelines, reuse requires explicit
GPU completion or synchronization. Neither an `Execute` return nor extraction
publication proves GPU completion; external consumers must order their GPU uses
and RHI retains responsibility for native resource retirement.

Renderer opts into independent queues by retaining this proof with each pool
entry. Reuse and ordinary pressure eviction require the graph's explicit
terminal join sync point to be `Complete`; the join depends on both queue tails.
Unpublished, pending, canceled, failed, or device-lost sync points never authorize
reuse. Selection for eviction is captured before compaction so concurrent GPU
progress cannot remove an entry absent from eviction accounting. This is a
conservative whole-graph completion requirement, not graph-local aliasing.
If retained entries cannot be evicted within the pool's structural budget,
allocation fails before creating new resources. It neither exceeds the budget
nor recycles outstanding GPU uses; later attempts can succeed after completion
or explicit pool invalidation.

Allocation is atomic: returning false, omitting one resource, or
publishing an incompatible description records nothing, publishes no
extraction destination, and invokes no pass. Culled logical resources never
enter the batch. Tests and production use this same counted-reference allocator
contract; no raw-pointer backing publication path remains.

Render-pass bodies that already own validated attachment initial/final layouts
use the managed-attachment declaration. The graph records the attachment
intent and exit access, emits an entry handoff for prior accesses even when
the attachment is cleared or discarded, and continues state tracking from the
render pass's declared final access. The render pass still owns its internal
attachment transitions.

Compilation and lazy diagnostics share one traversal for range-state advancement
and transition events, including final boundaries. `FRDGTransitionCapture::Kind`
distinguishes `RHIBarrier` events from `PassManaged` events owned by the pass body.
Only actual RHI barriers enter executable batches and transition budgets; a
same-state read does not gain an artificial entry barrier in Capture. Managed
exit events retain the declared entry/result pair even when the accesses match.
Detailed range uses and event arrays are materialized only on explicit inspection;
compilation reuses its existing layout without retaining diagnostic history.
The layout is read-only to both phases. Dependency producer/reader state is
local to dependency analysis; barrier traversal starts with fresh access state.
Version counters belong only to lazy diagnostics. Texture uses are reported per
subresource; buffer uses retain declared byte ranges with resource-wide versions.

## Graph-Owned Typed Values

`CreateValue<T>(Name, StableTypeName, ConstructorArguments...)` constructs one
payload in aligned builder-owned storage and returns a graph-local
`TRDGValueHandle<T>`. The stable type name is explicit diagnostic
metadata rather than RTTI text. One graph cannot assign different stable names
to the same C++ type or reuse one stable name for different C++ types.

Each typed value requires exactly one declared writer; all consumers declare
reads. Production parameter members use `TRDGValueWrite<T>` or
`TRDGValueRead<T>`. Missing or duplicate writers, foreign handles,
wrong C++ types, reads before the producer, and invalid directions fail
deterministically before recording. Values lower to token-shaped compiler uses
and therefore do not create a second dependency scheduler.

Value payloads and destructor records remain owned by the builder throughout
compilation and recording. Builder destruction destroys each constructed payload
exactly once. Compile failure, preparation failure, recording success or
unwinding, and culling do not shorten storage lifetime.
`FRDGPassResources::ReadValue`/`WriteValue` and the corresponding
parameter-resolver methods enforce the executing pass's exact declared
direction. Parameter resolution additionally requires the exact submitted
wrapper address; copied, wrong-pass, wrong-direction, foreign, or wrongly typed
members are rejected. Captures expose the stable value type and parameter field
path without addresses or compiler-specific type spelling.

## Typed Pass Parameters

`AllocParameters<T>()` constructs one registered standard-layout parameter
object in builder-owned aligned storage. The metadata for `T` owns its stable
structure name, size, alignment, and ordered member descriptions. Supported
members are texture, buffer, token, typed-value read/write, color attachment,
depth/stencil attachment, and managed-texture wrappers; a member may be a fixed array, an
`std::optional` wrapper, or a nested registered parameter structure. Wrappers
store only graph-local handles and exact runtime ranges. Metadata stores the
invariant use, access, discard, attachment action, managed-transition, and
result-access intent.

The first typed use in each module compiles that constexpr-friendly metadata
into one function-local immutable `FRDGParameterLayout`. The layout owns
root-relative leaf and element offsets, deterministic field paths, compact
category indices, an offset-sorted authorization index, and a name-sorted
shader-binding index. A valid type reuses the same layout for every allocation;
a malformed type reuses its stable validation error. Allocations and compiled
passes carry the exact layout pointer, so no process-global metadata registry or
metadata-address lookup participates in graph lifetime.

The parameterized `AddPass` consumes the mutable reference, freezes the
allocation, and scans the layout elements once to lower engaged fields into
the canonical compiler use model. Submission is atomic: malformed
metadata, a foreign allocation or handle, an invalid range or access/domain
combination, overlapping fields, or a reused reference publishes neither a
pass callback nor a partial use set. The public `AddPass` requires a parameter
object and a typed callback; raw declarations and resource-view callbacks are
private test-accessor operations. Both test and production callbacks enter one
erased callback slot and one recording path. Test-only mixed-authority
injection is still rejected before compilation.

Parameter storage and destructor records remain owned by the builder. Objects
are destroyed in reverse allocation order when the builder dies, not when a
pass is culled or execution returns early. Compile, retained-backing, callback, and normal execution paths
therefore observe one stable immutable parameter object for its complete graph
lifetime.

A parameterized callback receives the const submitted object and a non-copyable
`FRDGParameterResolver`. The resolver accepts only the exact wrapper or
optional-member address declared by that pass and returns typed texture,
buffer, color-attachment, or depth/stencil-attachment views. Raw handles,
copied wrappers, wrong-kind fields, foreign optionals, and fields from another
pass are authoring failures. A disengaged optional resolves to absence only
when that exact optional is a declared member. Cull or incomplete retained
backing prevents the callback from running.

Resolver authorization normalizes the requested address against the immutable
parameter root and searches the layout's offset index. Submission records only
one compact alias for each engaged optional so both the optional object and its
contained wrapper remain valid call forms even when their addresses differ.
No resolver call recursively interprets parameter metadata.

Each lowered use owns a deterministic parameter path such as
`FGBufferPassParameters.Colors[0]`. Validation prefixes the existing normalized
reason with the pass and field path, dumps append `field=<Path>`, and owning
captures preserve the same string. Manual uses keep an empty path and retain
their previous dump text. Paths never contain allocation identity, addresses,
timestamps, or measured duration.

### Reflected Shader Composition

A texture or buffer parameter member may additionally declare one reflected
shader binding role with
`WithRDGShaderBinding`. That annotation names
the binding and its `Texture`, `StorageImage`, or `StorageBuffer` type on the
same member that owns graph use, access, range, optionality, and array extent.
It never appends a hidden graph use: graph lowering remains authoritative for
dependencies and transitions, while cached shader reflection remains
authoritative for set, binding, and descriptor-array coordinates.

During its callback, a pass obtains a non-copyable
`FRDGShaderParameterScope` scope from the exact submitted object and
resolver. Composed `SetShaderParameters` resolves only those exact members,
creates counted texture views for their declared subresources, carries exact
buffer byte ranges, and submits all selected graph and ordinary shader fields
in one RHI parameter command. A graphics shader requires a graphics pass and a
compute shader requires a compute pass. SRVs require graph read authority and
the matching shader-readable access; storage images and writable storage
buffers require graph write authority and the matching read/write access.

Fixed arrays bind in element order and must match reflection extent. A
disengaged optional is legal when the selected shader does not reflect its
binding, but fails before RHI recording when active reflection requires it.
Missing graph authority, copied or foreign owner objects, duplicate aliases,
wrong binding type, wrong pass domain, missing backing, or an incomplete array
also fail before an affected parameter command, draw, or dispatch. Attachments,
tokens, managed-transition wrappers, dynamic arrays, null descriptors, and
partially bound arrays cannot be shader-composed.

Use captures preserve `ShaderBindingName` and `ShaderBindingType`; dumps append
`shader-binding=<Name>` and `binding-type=<Type>` beside the stable graph field
path. Uncomposed and manual uses retain their previous capture form.

## Diagnostics and Budgets

Results use domain-specific `std::expected` aliases, including
`FRDGMetadataResult`, `FRDGCompileResult`, `FRDGAllocationResult`,
`FRDGPreparationResult`, and `FRDGExecutionResult`. There is no universal
`FRDGResult`, `FRDGError`, global `ERDGError`, or generic diagnostic wrapper.
Metadata, use, identity, dependency, and allocation failures have domain-local
reason enums with fixed context types. Single-meaning failures (limits, external
contract conflicts, missing backing, and incompatible backing) are concrete
error types without an additional code. Aggregation variants contain complete
errors rather than an independently selected code and context. Callers inspect
the relevant alternative or domain reason. There is no global error-category
classification or `GetCategory()` interface; diagnostic text is derived from
the concrete error at presentation boundaries.
Structural budget errors contain only the exceeded dimension,
actual count, and limit; helpers that construct them return an error value,
not a potentially successful result. Tracking-layout construction returns its
owned value with a budget error and has no output parameter.

Generic range iteration propagates its visitor's result type. Execution-state
traversal returns a continuation boolean; the compiler keeps any transition
budget diagnostic at the call site. Inspection of an already compiled plan
checks internal invariants locally instead of propagating recoverable errors.
Compilation aggregates declaration and dependency diagnostics only; allocation
errors and backend causes do not enter metadata validation or compiler results.

Parameter layout construction returns
`std::expected<std::unique_ptr<const FRDGParameterLayout>, FRDGMetadataError>`
and caches either the completed immutable layout or its validation error.
Error contexts retain names, indices, ranges, and expected/actual descriptions;
no diagnostic borrows builder metadata or physical resource pointers.
`ToString` overloads are declared beside their error types and implemented in
`RDG/RDGErrorStrings.cpp`. Enum overloads return static `std::string_view` text;
structured-error overloads return `std::string` with owned context and causes.
Callers check the result before formatting `Result.error()` at logs, assertions,
and UI boundaries; there is no formatter for `std::expected` itself.
A retained result remains usable after graph destruction.

`FRDGAllocator::Allocate` returns `FRDGAllocationResult`. Its error distinguishes
a byte-budget failure from `FRDGAllocationFailure`, which carries a resource
identity, allocation reason, and concrete `FRHICreationError` cause. Local
allocation steps use only the latter error; aggregation occurs on return from
the allocator. It does not carry unrelated Shader or RenderResource error
alternatives. Its output
table also carries allocation statistics on failure; the output parameter is
retained for that diagnostic contract. Preparation adds backing compatibility
and queue preparation failures, while preserving allocation causes and domain
reasons.
Renderer allocation retains native status even when a later attempt is
suppressed, and publishes a complete batch only after all resources validate.
The retry, rollback, retirement, extraction, and execution-state rules apply.
Native causes format through the RHI formatter at the RDG presentation boundary.
Capture dependency `Cause` strings, allocation
observation tags, and resource names describe graph identities; they are not
error codes or error transport. Console-command messages and Core modular-feature
retirement messages likewise remain owned by their separate contracts.

`GetExecutionPlan()` exposes immutable logical submission records. Each
retained pass occupies one batch, except bounded groups of consecutive upload
helpers on the same logical queue. These share one batch, followed by an
epilogue batch when the graph has work. Empty graphs create no synthetic batch.
Batch pass intervals index the compact scheduled pass array, not declaration
indices; culling therefore cannot leave a dangling submission reference.
`SetPassAsyncComputeEligible` marks only compute passes owned by the builder;
invalid or foreign handles produce declaration errors. `SetAsyncComputeEnabled`
controls the graph's scheduling policy and defaults to false. When enabled,
eligible retained passes receive the logical async-compute role. Other passes
retain the graphics role. Eligibility, policy and physical hardware availability
are separate inputs. Both setters are building-only declarations.
Dependencies preserve compiler causes, add FIFO edges within each logical
queue and join both terminal prefixes at the epilogue. Independent branches on
different logical queues receive no artificial consecutive-pass edge.
Resource handoffs identify exact transition indices in their
consumer prologue or epilogue without retaining a physical resource pointer.
`ConsumerPass` identifies the compiled pass owning a prologue transition even
when several passes share the consumer submission; epilogue transitions do not
use this field. Submission endpoints are remapped after grouping. Dependencies
within a batch remain ordered by pass barriers and do not create self edges.
Each handoff also records the submission containing the latest prior use of
that tracked range on each logical queue, omitting the consumer submission
itself after grouping. Queue FIFO makes each recorded use cover earlier uses on the
same queue; it never replaces a use on another queue. Cross-queue producers add
execution dependencies to the handoff consumer. Initial transitions have no
graph producer. Texture ranges use exact aspect/mip/layer cells; buffers retain
the compiler's conservative whole-resource tracking. Captures and dumps retain
these producer endpoints separately from runtime completion sync points.
Range ownership starts on the logical graphics queue. A queue change emits a
handoff even for equal read access, and records the source queue independently
of its producer endpoints. The epilogue returns ranges last used on async
compute to graphics, preserving the current access when no final access was
requested for a transient resource. Disabling async policy removes these
queue-only transitions. Logical ownership changes are conservatively ordered;
they do not claim concurrent cross-queue sharing of one range.

Preparation resolves all logical barriers into execution-local physical
transition arrays before any graph callback or command is recorded. Recording
traverses the batch intervals and emits RHI GPU submission scopes with owning
wait sync points. Physical async lowering requires enabled graph policy, an
independent queue capability, and either no allocation requests or an allocator
whose `SupportsAsyncCompute()` explicitly accepts multi-queue reuse. Other
graphs map both roles to graphics. The allocator default is false.
Preparation creates every cross-queue release/acquire object before graph
recording. These owning objects replace their ordinary barrier entries and are
recorded at the source producer's tail and consumer's prologue. A separate
graphics preamble releases initially graphics-owned ranges with no graph
producer; only affected consumers wait its sync point. Transfer creation failure
fails preparation before callbacks or graph commands are recorded.
The current single-queue Vulkan mapping coalesces same-queue batches into
native payloads and satisfies their dependencies through FIFO execution and
resource barriers, without a CPU wait between passes. The native test accessor
exposes runtime submission signals in batch order; these are separate from the
immutable plan and do not certify graph success or extraction publication.
`FRDGCapture::ExecutionPlan` owns a copy, and the dump includes stable
batch identities, queue roles, dependency causes and handoff locations. No
native completion values or physical queue-family indices enter this data.

`Dump()` reports stable scheduled pass identities, declaration indices,
parameter-structure names, domains, dependency kinds/causes, logical resources,
submitted parameter fields, normalized uses and versions, transition counts,
preparation disposition, and final-batch counts. The dump omits builder
identities, addresses, timestamps, and measured duration so equal declarations
produce equal text. `Capture()` copies that dump plus pointer-free
pass/resource/parameter/use/transition records, dependencies, lifetimes,
culling decisions, statistics, and the original `ExecutionResult` into an owning
value that remains valid after graph destruction. Rejected repeated execution
attempts do not replace this report. Before execution the optional report is
empty; compile and preparation failures retain their typed error
and owned context. Renderer capture publication includes compile failures as
well as preparation failures and successful recordings. Compile timing is
available through `GetStatistics().CompileMicroseconds`.

`AllocationStatistics` records active/retained resource counts and logical
bytes, peak active bytes, cumulative reuse hits/misses, evictions, and failures.
Allocated resource records carry the allocator's stable, nonzero pointer-free
allocation ID and hit/miss disposition. External resources carry the external
disposition with allocation ID zero; culled and failed resources also use ID
zero. A graph-local resource index is never presented as physical identity.
`ObservationTag` may attribute memory to a typed owner, but is excluded from
compatibility, selection, scheduling, eviction priority, and success.

Renderer allocation compatibility, retention, and failure transactions are
defined by [frame resource lifetimes](RendererFramePreparation.md#resource-lifetime-classes)
and [resource recovery](RendererResourceRecovery.md).

`FRDGCapture::Parameters` contains one record for every submitted leaf
field of every parameterized pass, including fields on a culled pass and a
disengaged optional. The pass declaration index and full field path form its
stable identity. A present field names the canonical resource ID and preserves
its declared member kind, use/access, exact range, discard/store intent,
managed-transition result, and optional shader binding. An absent field sets
`bPresent` false and uses no synthetic resource ID. `Uses` remains the
compiler-normalized evidence and may contain multiple exact cells for one
field; correlate it with `Parameters` by pass declaration index and field path.
`Dependencies` and `Transitions` remain compiler output and are never inferred
again by inspection tooling.

For an authoring investigation, first select the pass record and its parameter
structure, then inspect all matching field records, correlate present fields
with resources and normalized uses, and finally follow dependency causes and
transitions. This order makes a route-selected absent fallback distinguishable
from a missing compiler use and preserves the declaration/compiler boundary.

`FRDGBudget` separates structural safety limits from regression budgets.
`TextureTransitions` counts compacted executable entries;
`TextureTransitionSubresources` counts the original per-subresource barrier
events, including repeated transitions. Compaction merges consecutive layers,
then mip ranges, only within the same batch and with identical resource,
aspect, access, discard intent, source queue, and producer submissions. Detailed
capture events remain per-subresource. `RegressionMaxTextureTransitions` applies
to compacted entries; `MaxTextureTransitions` bounds pre-compaction events so
compaction cannot hide excessive compiler work.
`MaxRangeCells` and `MaxRangeCellCandidates` bound fixed layout cells, including
unused subresources of referenced textures. `MaxCellVisits` counts layout
construction, buffer coverage checks and interval merges, dependency analysis,
and execution-plan traversal.
The `Max*` structural limits are deliberately broad deterministic compile gates
that protect graph construction from catastrophic growth. Errors name the
exceeded dimension and include actual and limit values. `RegressionMax*`
thresholds describe the expected shape of a named production graph: statistics
report individual overages, captures preserve the selected budget, and a graph
remains executable when one is exceeded. Compile and execute CPU thresholds are
also observational; wall-clock or regression-budget observation never rejects
compilation, aborts execution, or changes renderer correctness.

CPU timing uses Core's monotonic `FTime` clock and scope timers. Statistics and
captures expose `Phases`: validation (including export construction, explicit
edges, and resource-use indexing), resource layout construction, hazard
dependencies,
culling, and execution-plan generation (barriers, lifetimes, allocation requests,
and publication). `CompileMicroseconds` covers the whole private compile call.
`ExecuteMicroseconds` retains its preparation-plus-recording meaning:
`PreparationMicroseconds` includes allocation, backing validation and adoption;
`RecordingMicroseconds` includes transition pointer resolution, RHI commands,
callbacks, final transitions and extraction publication. Neither total includes
caller-side graph authoring or GPU completion. Phase durations round down to
microseconds and need not sum to the total because of boundary and cleanup work.
Timers retain elapsed work on early return and supported exception unwinding;
unentered phases remain zero. Failed compilation still publishes no compiler
records. Invalid repeated execution leaves all timing evidence unchanged.

The foundation regression gate compiles a 128-pass same-range hazard chain
under 250 milliseconds in a Debug native test. Renderer migration plans must
freeze representative median and p95 budgets before using graph timing as a
production acceptance gate.

## Production Authoring Contract

New renderer work that crosses pass boundaries must use the graph path:

- Submit one typed parameter object and a typed callback for every production
  pass. The public API rejects raw authoring at compile time.
- Use `MakeRDGTextureReadMetadata`, `MakeRDGComputeTextureWriteMetadata`,
  `MakeRDGManagedTextureMetadata`, and `MakeRDGAttachmentMetadata` for common
  texture roles. These infer or fix wrapper category, range kind, use, access,
  and attachment discard intent. Shader bindings decorate the same declaration
  with `WithRDGShaderBinding`; unusual low-level metadata still receives full
  layout and submission validation.
- Declare every cross-pass texture/buffer range with exact access and use a
  graph-owned typed value for a non-RHI outcome. Tokens remain for unmigrated
  execution-only compatibility edges.
- Declare external effects such as presentation, offscreen output, readback,
  capture, publication, and timestamps as explicit roots when culling is on.
- Describe graph-created resources exactly and let the execution allocator
  acquire retained resources; do not allocate inside pass callbacks.
- Set named safety limits, structural regression budgets, and CPU budgets beside
  every production graph authoring site. Raising a regression budget requires
  explaining the new pass/resource relationship and extending its contract
  coverage; safety limits change only when the supported graph scale changes.
- Keep manual transitions out of migrated edges. A feature-owned render pass
  may describe attachment layout, but its graph declarations own the outer
  access handoff.
- Extend `RenderContractTests` for compiler semantics and the owning capture, then
  add a renderer integration test for production wiring and a Vulkan gate for
  transition/backend equivalence.

Bypass scheduling, hidden inter-pass resources, mutable string blackboards,
and observer-controlled execution are not supported authoring patterns.

### Low-Level Compatibility Boundary

Raw pass creation (`AddTestPass`) and manual `Use*` methods are private.
The native-only `RDGTestAccess.h` friend accessor retains raw compiler and RHI
transition oracles, plus existing synthetic allocator/renderer fixtures. It
also adapts old resource-view callbacks at submission time; recording never
dispatches between callback forms. Production code cannot call these methods.
The test injection boundary retains invalid-handle and mixed-authority checks
so malformed fixtures fail deterministically without corrupting frozen uses.

`RendererSceneContractTests` additionally scans production Renderer C++ source
for manual uses and non-parameterized pass authoring. Compiler tests keep raw
oracles independent of parameter lowering; typed callback tests exercise the
public API directly.
