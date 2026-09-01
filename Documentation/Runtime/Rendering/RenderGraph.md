# Render Graph

Summary: Define the deterministic frame-local graph compiler and its boundary with RHI execution state.

Modules: RenderCore, RHI

Last reviewed: 2026-08-30

## Ownership Boundary

`FRDGBuilder` owns declarations for one graph. Texture, buffer, and
pass handles are valid only for the builder that created them. A successful
compile transfers immutable resource views, pass callbacks, dependencies, and
transition batches into `FRDGCompiledGraph`; external owners retain the
physical RHI resources themselves. Graph-created textures and buffers use the
description-first `CreateTexture`/`CreateBuffer` API with
`FRDGTextureDesc`/`FRDGBufferDesc` and no
physical pointer. Compilation computes retained lifetimes first;
`FRDGCompiledGraph::Execute` passes one name-free batch of exact descriptors
to the `FRDGAllocator` in `FRDGExecutionContext`. The complete returned table
is held through counted RHI references for the compiled execution lifetime.

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
  explicit culling root, apply its requested final access, and publish a
  counted reference to the destination only after complete successful
  execution. Compile, preparation, allocation, or recording failure leaves
  every destination unchanged. Duplicate resource or destination extraction
  is a deterministic declaration error.
- Every use declares one nonempty exact byte range or texture
  aspect/mip/layer range. The compiler partitions partially overlapping
  declarations into exact buffer intervals and texture aspect/mip/layer cells;
  disjoint cells remain independent.
- Required access cannot contain `Discard`. Discard is producer intent and
  affects only the expected-before state.
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
uses `ERHIAccess::Discard` as its expected-before access; otherwise the prior
compiled access is exact. Final transition batches restore each used imported
or explicitly finalized range after the last pass.

`FRDGCompiledGraph::Execute` records each pre-pass batch, invokes the pass
callback with a pass-scoped resource view, and then records final batches.
Lookup of a foreign, undeclared, incorrectly typed, or unavailable handle is
an unrecoverable authoring-contract failure. Graphics, compute, and copy passes
accept only their corresponding graphics/attachment, compute, and transfer
access families.

An optional execution-preparation callback runs after successful compile.
The execution allocator then receives immutable `FRDGAllocationRequest`
records containing only resource ID, kind, exact description, and retained
lifetime. Allocation is atomic: returning false, omitting one resource, or
publishing an incompatible description records nothing, publishes no
extraction destination, and invokes no pass. Culled logical resources never
enter the batch. Tests and production use this same counted-reference allocator
contract; no raw-pointer backing publication path remains.

Render-pass bodies that already own validated attachment initial/final layouts
use the managed-attachment declaration. The graph records the attachment
intent and exit access, emits only an entry handoff needed for a load, and
continues state tracking from the render pass's declared final access. This
avoids a second explicit barrier competing with RHI render-pass state.

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

Successful compilation transfers value payloads and destructor records to the
compiled graph. Builder destruction and every compile/execute exit path destroy
each constructed payload exactly once. Culling does not shorten storage
lifetime, and no payload may outlive its builder or compiled graph.
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

Successful compilation transfers parameter storage and destructor records to
the compiled graph. Objects are destroyed in reverse allocation order when the
owning builder or compiled graph dies, not when a pass is culled or execution
returns early. Compile, retained-backing, callback, and normal execution paths
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

The Renderer allocator keys textures and buffers only by exact allocation
descriptions. Diagnostic names are stored outside compatibility, so renaming a
logical graph resource does not change reuse or its stable physical allocation ID.
Candidate reservation, generation-based failure suppression, rollback,
publication, and error reporting form one transaction for both resource kinds;
typed RHI creation remains at the boundary. Retained bytes are maintained
incrementally and inactive entries are evicted in stable creation order. The
named structural policy ceiling is 640 MiB; a requested batch above that ceiling
fails atomically, while observation tags cannot reserve or prioritize memory.

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
  for a new pass. Use the manual surface only while migrating an existing
  contributor, and never mix both authorities on one pass.
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
- Extend `RenderGraphTests` for compiler semantics and the owning capture, then
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
parameter object. This allows new parameter structures and contributors without
maintaining a filename allowlist while making the production prohibition
executable. The compatibility surface may be removed only when these compiler
and RHI transition oracles have parameterized replacements that remain
independent of the lowering behavior they validate and no external low-level
consumer has been admitted.

## Production Pilot Boundary

Contact-shadow visibility contributes work to the scene parent graph and no
longer constructs, compiles, or executes a child graph. Its current compute or
fragment body retains its bounded intra-pass pipeline handoffs while the parent
owns ordering against GBuffer, depth, deferred lighting, and final output.

## Scene Render Graph

`FSceneRenderPipeline` owns the sole production graph's preparation,
compile/execute/capture, and commit-or-abort boundary. One stack-owned
`FSceneFrameContext` separates logical preparation, resolved resources, feature
decisions, transaction state, and observation state, while
`FSceneRenderGraphComposer` wires renderer-private features in a fixed order.
Feature-owned `AddPasses` entries accept a feature-specific immutable input,
create their own typed completion value and logical textures, then return a
narrow typed output for the next contributor. Every production scene pass is
parameterized: one graph-owned parameter object supplies its exact texture,
attachment, value, token, and selected-route declarations to both compilation
and the bounded callback. Present or offscreen output is the explicit typed
root. Stable compilation preserves declaration order between independent
optional producers.

The composer is the only graph-authoring boundary allowed to see the complete
immutable `FSceneRenderPlan` and canonical `FSceneFrameFeaturePlan`; it slices
those plans into feature-specific inputs before invoking each feature.
Features and their callbacks cannot receive
the complete plan or execution pipeline. `FSceneRenderFeatureRecorders` owns
the remaining feature command semantics and renderer services during migration,
but does not author, compile, or execute graph structure. GBuffer and Contact
Visibility already own their complete metadata/setup/callback/recording path
and do not pass through that facade.

Production scene authoring has no frame-wide resource/channel bag, repeated
persistent-input declaration helper, or manual `Graph.Use*` supplement. A pass
may resolve only fields present in its immutable parameter object. Route and
fallback selection finishes before that object is assigned, so absent optional
fields mean the route cannot access those capabilities.

Persistent geometry and feature pipeline preparation complete before graph
compile. Compute, fragment, disabled, and factor-one routes are therefore part
of the published feature plan rather than callback-time choices. Callbacks
validate that physical capabilities still match the authored route and fail the
feature instead of selecting another route. After culling, the
descriptor-keyed Renderer allocator reserves a distinct physical entry for
every retained logical resource and atomically publishes the complete
strong-reference table. Diagnostic names and feature routes do not participate
in compatibility or selection. Only then do callbacks run. Scene failure prevents
final output work from publishing success, and the surrounding view-state
transaction commits only after the rooted final-output pass succeeds.

The scene graph observes regression ceilings of 12 declared passes, 28
dependencies, and 32 physical texture transitions. Its structural safety
limits are independently set to 256 passes and 4096 dependencies, buffer
transitions, and texture transitions. The current graph declares no cross-pass
buffers, so its buffer regression ceiling remains unbounded until production
measurements establish one.
Scene Color, depth, directional and cloud shadows, GBuffer, ambient occlusion,
contact visibility, cloud spatial/composite, isolated deferred, GBuffer debug,
and output all have graph identities. Consumers obtain physical textures only
from their pass-scoped resource views; typed tokens carry non-RHI outcome state.

The 2026-08-24 Win64 Debug Vulkan cloud fixture froze six disabled,
invalid-input, compute, fragment, offscreen, present, and resized captures. Each
scheduled all 11 declared passes, with 22--25 dependencies and 1 or 17 texture
transitions. The observational Debug CPU thresholds remain 5 milliseconds to
compile and 250 milliseconds to record the complete callback schedule; every
fixture capture stayed within both ceilings. These are regression ceilings,
not optimization targets.

`SetSceneRenderGraphCaptureSink` remains the development and test observer
authored after complete frame migration. An individual `RenderView` submission
may instead supply an explicit capture output; Engine uses that path to route a
one-shot owning capture back to the exact requesting `FSceneViewport`. With no
observer and no explicit output, no capture is constructed. Neither mechanism
can mutate resources, callbacks, scheduling, or frame commit state.
When both consumers are present, Renderer constructs one owning snapshot and
publishes that same value to the observer and explicit output.

## Related Documentation

- [Render Graph Architecture Roadmap](../../Roadmaps/Archive/2026-08/RenderGraphArchitecture.md)
- [RHI Resource Transitions](RHIResourceTransitions.md)
- [RHI Command Execution](RHICommandExecution.md)
- [Renderer Frame Preparation](RendererFramePreparation.md)
