# Render Graph

Summary: Define the deterministic frame-local graph compiler and its boundary with RHI execution state.

Modules: RenderCore, RHI

Last reviewed: 2026-08-24

## Ownership Boundary

`FRenderGraphBuilder` owns declarations for one graph. Texture, buffer, and
pass handles are valid only for the builder that created them. A successful
compile transfers immutable resource views, pass callbacks, dependencies, and
transition batches into `FCompiledRenderGraph`; external owners retain the
physical RHI resources themselves.

The graph is the declaration and scheduling authority. It emits existing
`FRHIBufferTransition` and `FRHITextureTransition` descriptors, while RHI and
the active backend remain authoritative for validating and committing actual
execution state. Compilation never mutates a command list.

## Resource Contract

- Imported resources declare their exact initial and final access and remain
  externally owned. A missing final access is a compile error.
- Graph-created resources begin at `ERHIAccess::Discard`, require a stored
  producer before any read or load, and may omit a final state when their
  contents do not cross the graph boundary.
- Every use declares one nonempty exact byte range or texture
  aspect/mip/layer range. Exact matching ranges and disjoint ranges are tracked
  independently. Partially overlapping declarations are rejected until the
  compiler owns interval splitting.
- Required access cannot contain `Discard`. Discard is producer intent and
  affects only the expected-before state.
- An attachment `Load` requires prior contents. A `DontCare` store invalidates
  its contents and cannot satisfy a later producer requirement.

## Compilation and Ordering

Passes retain declaration order when dependencies leave them independent.
Explicit prerequisites and overlapping resource hazards add directed edges;
RAW, WAR, and WAW hazards preserve declaration order. Stable topological
compilation rejects cycles and never performs performance reordering.

Compilation fails as one complete result for invalid or foreign handles,
unnamed or duplicate identities, missing producers, illegal access/use pairs,
overlapping declarations within a pass, partial overlaps across passes,
invalid RHI ranges or usage, and dependency cycles. No pass callback runs and
no transition records when compilation fails.

Logical tokens express producer/consumer ordering and lifetimes when a typed
feature result crosses passes but physical RHI ownership remains inside a
closed renderer callback. Tokens participate in RAW/WAR/WAW dependencies,
culling, and diagnostics without inventing transitions or backend state.

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
callback with graph-bounded resource lookup, and then records final batches.
Callbacks can resolve only handles owned by that compiled graph. Callback
policy and rendering algorithms remain feature-owned.

An optional execution-preparation callback runs once after successful compile
and before the first transition or pass callback. Renderer uses this gate to
acquire the complete transient target bundle from its existing pool. A false
result records nothing, invokes no pass, and preserves pool and temporal abort
policies.

## Diagnostics and Budgets

`Dump()` reports stable scheduled pass identities, declaration indices,
domains, transition counts, dependency causes, and final-batch counts. The
dump omits builder identities, addresses, timestamps, and measured duration so
equal declarations produce equal text. `Capture()` copies that dump plus
pointer-free pass records, dependencies, lifetimes, culling decisions, and
statistics into an owning value that remains valid after graph destruction.

`FRenderGraphBudget` freezes pass, dependency, and transition ceilings as
deterministic compile gates. Errors name the exceeded dimension and include
actual and limit values. Compile and execute CPU thresholds are different:
statistics report whether they were exceeded, but wall-clock observation never
rejects compilation, aborts execution, or changes renderer correctness.

The foundation regression gate compiles a 128-pass same-range hazard chain
under 250 milliseconds in a Debug native test. Renderer migration plans must
freeze representative median and p95 budgets before using graph timing as a
production acceptance gate.

## Production Authoring Contract

New renderer work that crosses pass boundaries must use the graph path:

- Declare physical texture/buffer ranges with exact access, or use a logical
  token when a typed result crosses callbacks but RHI ownership stays inside a
  closed feature implementation.
- Declare external effects such as presentation, offscreen output, readback,
  capture, publication, and timestamps as explicit roots when culling is on.
- Put resource acquisition in execution preparation when it must complete as
  one frame bundle; do not lazily allocate inside pass callbacks.
- Set a named structural and CPU budget beside every production graph authoring
  site. Raising a structural limit requires explaining the new pass/resource
  relationship and extending its contract coverage.
- Keep manual transitions out of migrated edges. A feature-owned render pass
  may describe attachment layout, but its graph declarations own the outer
  access handoff.
- Extend `RenderGraphTests` for compiler semantics and the owning capture, then
  add a renderer integration test for production wiring and a Vulkan gate for
  transition/backend equivalence.

Bypass scheduling, hidden inter-pass resources, mutable string blackboards,
and observer-controlled execution are not supported authoring patterns.

## Production Pilot Boundary

Contact-shadow visibility is the first production graph slice. Route and
resource preparation select compute, fragment, or factor-one before graph
construction. Compute declares five imported GBuffer/depth inputs, one
submission-local uniform range, and one transient visibility output. Fragment
adds the persistent fullscreen vertex/index buffers and declares visibility as
a cleared color attachment. Both routes restore visibility and all imported
inputs to graphics-readable boundary state.

The contact-visibility render-pass layout keeps its attachment in
`ColorAttachmentReadWrite` across begin/end. The graph owns the discard-to-
attachment and attachment-to-sampling transitions outside that render pass.
Feature code contains no manual buffer or texture transition for the migrated
slice, and callbacks resolve all declared resources through
`FRenderGraphPassResources`.

## Scene Frame Graph

`FRenderGraphSceneFrameExecutor` is the sole production scene scheduler. It
declares logical result tokens and dependencies for directional shadow,
GBuffer, ambient occlusion, contact visibility, cloud shadow, deferred
lighting, Scene Color, and final output. Present or offscreen output is the
explicit root. Stable compilation preserves declaration order between
independent optional producers.

Persistent geometry and feature preparation complete before graph compile.
Frame target requirements are immutable graph input; the existing transient
pool resolves every requested bundle through the execution-preparation gate.
Only then do callbacks run. Scene failure prevents final output work from
publishing success, and the surrounding view-state transaction commits only
after the rooted final-output pass succeeds.

The scene graph freezes ceilings of 12 declared passes, 24 dependencies, and
zero physical transitions because its current cross-feature values are logical
tokens. Its observational Debug CPU thresholds are 5 milliseconds to compile
and 250 milliseconds to record the complete callback schedule. Contact compute
and fragment graphs each allow one pass and zero dependencies; their exact
transition ceilings are 2 buffer/12 texture and 0 buffer/2 texture,
respectively. These are regression ceilings, not optimization targets.

`SetSceneRenderGraphCaptureSink` is the first feature authored after complete
frame migration. When installed, it receives an owning capture after execution.
With no sink, no capture is constructed. The observer cannot mutate resources,
callbacks, scheduling, or frame commit state.

## Related Documentation

- [Render Graph Architecture Roadmap](../../Roadmaps/RenderGraphArchitecture.md)
- [RHI Resource Transitions](RHIResourceTransitions.md)
- [RHI Command Execution](RHICommandExecution.md)
- [Renderer Frame Preparation](RendererFramePreparation.md)
