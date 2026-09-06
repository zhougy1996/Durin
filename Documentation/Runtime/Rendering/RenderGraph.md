# Render Graph

Summary: Define the deterministic frame-local graph compiler and its boundary with RHI execution state.

Modules: RenderCore, RHI

Last reviewed: 2026-09-07

## Ownership Boundary

`FRDGBuilder` owns declarations, parameters, typed values, callbacks, compiled
records, and retained resource references for one graph execution. Handles are
valid only for their originating builder. There is no public compile operation
or independently owned executable graph. Graph-created textures and buffers
use description-first `CreateTexture`/`CreateBuffer` declarations.

Non-const `Execute(CommandList, ExecutionContext)` compiles, prepares retained
resources, records passes, and publishes extraction outputs. The optional
context pointer supplies `FRDGAllocator`; omitting it is valid only when no
retained logical RHI resource requires allocation. The allocator receives one
name-free batch of exact retained descriptors. The builder holds the complete
returned reference table through its own lifetime; allocator borrowing and RHI
retirement rules still apply.

The thread-confined lifecycle is Building -> Compiling -> Preparing -> Recording
-> Recorded, with terminal Failed on supported failures or C++ unwinding. Every
execution attempt consumes Building, including compile and preparation failure.
`FRDGExecutionResult` distinguishes CompileFailed, PreparationFailed, Recorded,
and InvalidState. Recorded means CPU recording succeeded, not GPU completion.
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
  not become identity keys.
- Graph-created resources begin at `ERHIAccess::Discard`, require a stored
  producer before any read or load, and may omit a final state when their
  contents do not cross the graph boundary.
- `QueueTextureExtraction` and `QueueBufferExtraction` make a resource an
  explicit terminal consumer (`RDG.Export`) of its complete byte or
  aspect/mip/layer range. This node requires valid stored contents in every
  cell, retains the final producers through ordinary value dependencies,
  extends allocation lifetimes through export, and applies the requested final
  access. It appears in scheduled passes, captures, and structural budgets;
  an output-free graph has no export node. External resources with discarded
  initial contents also require a stored producer. A counted reference is
  published to the destination only after complete successful execution. Compile, preparation, allocation, or recording failure leaves
  every destination unchanged. Duplicate resource or destination extraction
  is a deterministic declaration error.
- Every use declares one nonempty exact byte range or texture
  aspect/mip/layer range. The compiler partitions partially overlapping
  declarations into exact buffer intervals and texture aspect/mip/layer cells;
  disjoint cells remain independent.
- Required access cannot contain `Discard`. Discard is producer intent and
  is carried separately from the expected-before access state.
- An attachment `Load` requires prior contents. A `DontCare` store invalidates
  its contents and cannot satisfy a later producer requirement.

## Compilation and Ordering

Passes retain declaration order when dependencies leave them independent.
Each normalized range carries a produced-value version. Value edges connect a
producer to readers and read/write consumers; explicit edges also participate
in reachability. A separate minimal execution frontier preserves required RAW,
WAR, and WAW order without making overwritten values reachable. A discard
write starts a new version. Stable topological compilation rejects cycles and
never performs performance reordering. A same-range overwrite chain therefore
produces linear rather than all-pairs dependencies.

Compilation fails as one complete result for invalid or foreign handles,
unnamed or duplicate identities, missing producers, illegal access/use pairs,
overlapping declarations within a pass, invalid normalized ranges or usage,
pass-domain/access mismatch, and dependency cycles. No pass callback runs and
no transition records when compilation fails.

Logical tokens express compatibility-only ordering edges without transitions
or backend state. Graph-owned typed values use those same value versions,
producer/consumer edges, execution frontiers, culling closure, and logical
lifetimes for non-RHI outcomes.

Pass culling is opt-in and root-driven. Present, offscreen output, temporal
publication, readback, capture, timestamps, and other external effects mark an
explicit root reason. Compilation retains each root and its complete reverse
dependency closure; other passes are reported as unreachable. Declaration and
cycle validation still cover the complete graph before culling.

Every resource reports its first/last retained scheduled pass. A resource used
only by unreachable passes reports a culled lifetime. These logical intervals
do not authorize physical aliasing.

## Transition and Execution Contract

Each compiled pass owns the buffer and texture transition batches that precede
its callback. State is tracked per exact declared range. A discard producer
sets `bDiscardContents` and preserves the prior compiled access, including
after a `DontCare` store. Content validity and value reachability are separate
from execution dependencies and access state. Only a logical resource's first
use carries the compatibility `Discard` wildcard so the backend can recover
the physical allocation's prior accesses on pool reuse. Final transition batches restore each used imported
or explicitly finalized range after the last pass.

`FRDGBuilder::Execute` records each pre-pass batch, invokes the pass
callback with a pass-scoped resource view, and then records final batches.
Lookup of a foreign, undeclared, incorrectly typed, or unavailable handle is
an unrecoverable authoring-contract failure. Graphics, compute, and copy passes
accept only their corresponding graphics/attachment, compute, and transfer
access families.

After successful private compilation, the execution allocator receives immutable `FRDGAllocationRequest`
records containing only resource ID, kind, exact description, and retained
lifetime, observation tag, and explicit `bExtracted` ownership intent.
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

## Graph-Owned Typed Values

`CreateValue<T>(Name, StableTypeName, ConstructorArguments...)` constructs one
payload in aligned builder-owned storage and returns a graph-local
`TRDGValueHandle<T>`. The stable type name is explicit diagnostic
metadata rather than RTTI text. One graph cannot assign different stable names
to the same C++ type or reuse one stable name for different C++ types.

Each typed value requires exactly one declared writer; all consumers declare
reads. `UseValue` is the bounded manual compatibility declaration. Typed
parameter members use `TRDGValueWrite<T>` or
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
the same canonical use model as the manual `Use*` APIs. Submission is atomic: malformed
metadata, a foreign allocation or handle, an invalid range or access/domain
combination, overlapping fields, or a reused reference publishes neither a
pass callback nor a partial use set. A parameterized pass cannot accept manual
uses or a second parameter object. The legacy `AddPass` and `Use*` surface
remains a compatibility path only for passes not yet migrated.

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
`MakeRenderGraphShaderResourceParameterMemberMetadata`. That annotation names
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

`Dump()` reports stable scheduled pass identities, declaration indices,
parameter-structure names, domains, dependency kinds/causes, logical resources,
submitted parameter fields, normalized uses and versions, transition counts,
preparation disposition, and final-batch counts. The dump omits builder
identities, addresses, timestamps, and measured duration so equal declarations
produce equal text. `Capture()` copies that dump plus pointer-free
pass/resource/parameter/use/transition records, dependencies, lifetimes,
culling decisions, and statistics into an owning value that remains valid after
graph destruction.

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
The `Max*` structural limits are deliberately broad deterministic compile gates
that protect graph construction from catastrophic growth. Errors name the
exceeded dimension and include actual and limit values. `RegressionMax*`
thresholds describe the expected shape of a named production graph: statistics
report individual overages, captures preserve the selected budget, and a graph
remains executable when one is exceeded. Compile and execute CPU thresholds are
also observational; wall-clock or regression-budget observation never rejects
compilation, aborts execution, or changes renderer correctness.

The foundation regression gate compiles a 128-pass same-range hazard chain
under 250 milliseconds in a Debug native test. Renderer migration plans must
freeze representative median and p95 budgets before using graph timing as a
production acceptance gate.

## Production Authoring Contract

New renderer work that crosses pass boundaries must use the graph path:

- Prefer one typed parameter object as the declaration and callback capability
  for a new pass. Use the manual surface only for the bounded low-level
  compatibility oracles, and never mix both authorities on one pass.
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

The non-parameterized `AddPass` overload and manual `UseTexture`, `UseBuffer`,
attachment, managed-resource, token, and typed-value declarations remain only
as an independent oracle for canonical compiler semantics and backend
transition qualification. Their bounded repository consumers are
`RDGTests.cpp` and `VulkanResourceTransitionTests.cpp`. They are not a
production Renderer or Renderer contract-fixture authoring option.

`RendererSceneContractTests` scans all production Renderer C++ source and
rejects a manual `Use*` call or an `AddPass` whose third argument is not a moved
parameter object. This allows new feature-owned parameter structures without
maintaining a filename allowlist while making the production prohibition
executable. The compatibility surface may be removed only when these compiler
and RHI transition oracles have parameterized replacements that remain
independent of the lowering behavior they validate and no external low-level
consumer has been admitted.

## Renderer Integration

The production scene schedule, feature ownership, route preparation, resource
lifetimes, output transaction, and scene budgets are defined by
[Renderer Frame Preparation](RendererFramePreparation.md). Features contribute
parameterized passes to that single caller-owned graph. Contact visibility
uses the same boundary; bounded intra-pass handoffs do not create child graphs.

## Related Documentation

- [Render Graph Architecture Roadmap](../../Roadmaps/Archive/2026-08/RenderGraphArchitecture.md)
- [RHI Resource Transitions](RHIResourceTransitions.md)
- [RHI Command Execution](RHICommandExecution.md)
- [Renderer Frame Preparation](RendererFramePreparation.md)
