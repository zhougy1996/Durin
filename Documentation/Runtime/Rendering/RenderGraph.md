# Render Graph

Summary: Define the deterministic frame-local graph compiler and its boundary with RHI execution state.

Modules: RenderCore, RHI

Last reviewed: 2026-08-24

## Ownership Boundary

`FRenderGraphBuilder` owns declarations for one graph. Texture, buffer, and
pass handles are valid only for the builder that created them. A successful
compile transfers immutable resource views, pass callbacks, dependencies, and
transition batches into `FCompiledRenderGraph`; external owners retain the
physical RHI resources themselves. Graph-created textures and buffers may be
declared from `FRenderGraphTextureDesc`/`FRenderGraphBufferDesc` without a
physical pointer. Compilation computes retained lifetimes first; one backing
resolver receives only retained requests and must publish a complete candidate
table before recording starts.

The graph is the declaration and scheduling authority. It emits existing
`FRHIBufferTransition` and `FRHITextureTransition` descriptors, while RHI and
the active backend remain authoritative for validating and committing actual
execution state. Compilation never mutates a command list.

## Resource Contract

- Imported resources declare their exact initial and final access and remain
  externally owned. A missing final access is a compile error. The same
  physical texture or buffer cannot be imported twice into one graph.
- Graph-created resources begin at `ERHIAccess::Discard`, require a stored
  producer before any read or load, and may omit a final state when their
  contents do not cross the graph boundary.
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

Logical tokens express producer/consumer ordering and lifetimes for typed
non-RHI outcomes. They use the same value-version and execution-frontier rules
without inventing transitions or backend state.

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

`FCompiledRenderGraph::Execute` records each pre-pass batch, invokes the pass
callback with a pass-scoped resource view, and then records final batches.
Lookup of a foreign, undeclared, incorrectly typed, or unavailable handle is
an unrecoverable authoring-contract failure. Graphics, compute, and copy passes
accept only their corresponding graphics/attachment, compute, and transfer
access families.

An optional compatibility preparation callback runs after successful compile.
The retained-backing resolver then receives immutable requests containing
stable identity, logical description, backing class, and retained lifetime.
Publication is atomic: returning false or omitting one required backing records
nothing and invokes no pass. Culled logical resources never enter the request.

Render-pass bodies that already own validated attachment initial/final layouts
use the managed-attachment declaration. The graph records the attachment
intent and exit access, emits only an entry handoff needed for a load, and
continues state tracking from the render pass's declared final access. This
avoids a second explicit barrier competing with RHI render-pass state.

## Diagnostics and Budgets

`Dump()` reports stable scheduled pass identities, declaration indices,
domains, dependency kinds/causes, logical resources, normalized uses and
versions, transition counts, preparation disposition, and final-batch counts. The
dump omits builder identities, addresses, timestamps, and measured duration so
equal declarations produce equal text. `Capture()` copies that dump plus
pointer-free pass/resource/use/transition records, dependencies, lifetimes, culling decisions, and
statistics into an owning value that remains valid after graph destruction.

`FRenderGraphBudget` separates structural safety limits from regression budgets.
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

- Declare every cross-pass texture/buffer range with exact access and use a
  typed logical token only for a non-RHI outcome.
- Declare external effects such as presentation, offscreen output, readback,
  capture, publication, and timestamps as explicit roots when culling is on.
- Put graph-created resource acquisition in the retained-backing resolver; do
  not lazily allocate inside pass callbacks.
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

## Production Pilot Boundary

Contact-shadow visibility contributes work to the scene parent graph and no
longer constructs, compiles, or executes a child graph. Its current compute or
fragment body retains its bounded intra-pass pipeline handoffs while the parent
owns ordering against GBuffer, depth, deferred lighting, and final output.

## Scene Frame Graph

`FRenderGraphSceneFrameExecutor` owns the sole production graph's
compile/execute/capture boundary. `FSceneFrameExecutionPipeline` owns frame
preparation, topology selection, and commit or abort, while
`FSceneFrameGraphComposer` wires renderer-private feature contributors in a
fixed order. Each contributor owns its pass declarations, exclusive logical
textures, uses, and bounded callback. Present or offscreen output is the
explicit root. Stable compilation preserves declaration order between
independent optional producers.

The composer is the only boundary allowed to see the complete immutable
`FSceneRenderPlan`; it slices that plan into feature-specific recorder inputs
before invoking contributors. Contributors and their callbacks cannot receive
the complete plan or execution pipeline. `FSceneFrameFeatureRecorders` owns
feature command semantics and renderer services, but does not author, compile,
or execute graph structure.

Persistent geometry and feature pipeline preparation complete before graph
compile. Compute, fragment, disabled, and factor-one routes are therefore part
of the authored topology rather than callback-time choices. After culling, the
backing resolver derives target-family requirements solely from retained
logical resources and atomically publishes only those requested handles from
the existing transient pool. Only then do callbacks run. Scene failure prevents
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

## Related Documentation

- [Render Graph Architecture Roadmap](../../Roadmaps/Archive/2026-08/RenderGraphArchitecture.md)
- [RHI Resource Transitions](RHIResourceTransitions.md)
- [RHI Command Execution](RHICommandExecution.md)
- [Renderer Frame Preparation](RendererFramePreparation.md)
