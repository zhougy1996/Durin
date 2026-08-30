# RDG Parameter Layout Flattening Plan

Summary: Compile typed RDG parameter metadata into one reusable flat layout so pass lowering, resolver authorization, capture, and shader composition stop recursively rediscovering the same fields.

Last reviewed: 2026-08-30

Status: Completed
Completed: 2026-08-30

## Current Status

All implementation and non-Tracy qualification work is complete. Each typed
parameter structure now owns a function-local immutable
`FRDGParameterLayout`; allocation reuses its cached validation result,
parameter submission scans flat elements, resolver authorization uses the
offset index plus engaged-optional aliases, and composed shader submission uses
the name-sorted binding table. Existing RenderCore graph and shader contract
targets pass in Debug and Release, and the registered Renderer scene-contract
target passes in Debug.

The same timed qualification workload was run against detached pre-change commit
`dc1d15492` and the final implementation in Release with 8 warm-ups and 64
samples. The final comparison ran without a competing repository build. The
machine was not reserved globally, so absolute timings remain diagnostic; the
same-workload relative comparison is the acceptance evidence:

| Elements | Metric | Legacy median / p95 | Flat median / p95 |
| --- | --- | --- | --- |
| 1 | Allocation | 0.4 / 0.5 us | 0.1 / 0.1 us |
| 1 | AddPass | 0.6 / 0.8 us | 0.5 / 0.7 us |
| 1 | Compile | 1.8 / 2.1 us | 1.8 / 2.3 us |
| 1 | Resolver | 0.0 / 0.1 us | 0.0 / 0.1 us |
| 1 | Composition | 0.4 / 0.5 us | 0.3 / 0.4 us |
| 32 | AddPass | 13.1 / 17.7 us | 8.7 / 15.9 us |
| 32 | Resolver | 1.5 / 1.6 us | 0.5 / 0.6 us |
| 32 | Composition | 4.0 / 9.7 us | 2.9 / 9.8 us |
| 128 | AddPass | 69.5 / 78.6 us | 39.8 / 57.0 us |
| 128 | Resolver | 11.0 / 11.4 us | 2.8 / 3.6 us |
| 128 | Composition | 19.6 / 42.9 us | 10.5 / 33.0 us |

The selected CPU gates are: single-element AddPass p95 at most 1.0 us; 32- and
128-element resolver median and p95 at least 50% below legacy; 32-element AddPass
median at least 20% below legacy with no material p95 regression; and
128-element AddPass median/p95 at least 25%/20% below legacy. Composition must
improve median by at least 20% at 32 elements and 35% at 128 elements, with no
more than 10% p95 regression at 32 and no p95 regression at 128. Every gate is
met by the final comparison.

The static layout retains 400, 3,648, and 14,140 logical bytes for the 1-, 32-,
and 128-element fixtures; the legacy implementation retained no type-level
layout. The compiled optional-alias table uses 16 fixed bytes and one exact
contiguous 8-byte record per engaged optional, below the 32-byte fixed gate,
and allocates no node per leaf. Internal uses borrow layout-owned paths, public
captures remain owning, and submission no longer allocates traversal paths.
The three legacy hot recursive consumers are reduced to zero; recursive
metadata interpretation remains in validation/layout construction only.

By explicit user direction, representative PostProcess/Deferred Tracy capture
is deferred and is not a closeout requirement for this plan. No Tracy evidence
is claimed.

## Goal

Replace repeated recursive interpretation of one RDG parameter type with a
single validated, flattened layout that all hot consumers can traverse or
index directly, while preserving observable graph scheduling, diagnostics,
capture order, shader binding, and callback authorization behavior.

## Scope

- Add an immutable type-level layout containing root-relative leaf and element
  offsets, category indices, stable field paths, and shader-binding indices.
- Build and validate that layout once per typed parameter structure through a
  function-local static accessor; do not introduce a process-global metadata
  registry.
- Carry the exact layout pointer from parameter allocation through pass
  declaration and compiled execution.
- Make parameter lowering and capture consume the flat layout in deterministic
  metadata order.
- Replace resolver tree traversal with offset-based authorization, including
  the existing optional-wrapper and contained-value call forms.
- Make composed shader submission consume the layout's binding index rather
  than rebuilding and linearly searching a composed-member list.
- Measure declaration, compile, execution, shader-composition, allocation, and
  retained-memory effects on synthetic and representative renderer workloads.

## Non-Goals

- Do not replace graph handles and value wrappers with UE-style resource or
  view objects.
- Do not add resource-owned `GetRHI()` access or remove
  `FRDGParameterResolver`.
- Do not merge `FRDGParametersMetadata` with shader metadata or change public
  parameter declaration macros beyond supplying the layout accessor.
- Do not weaken exact subresource/byte-range dependency analysis, optional
  semantics, managed-transition semantics, or callback capability checks.
- Do not add a global layout cache, RHI layout object, persistent shader/layout
  pair cache, or new renderer feature behavior.
- Do not treat Debug timing or a contended profiling session as an acceptance
  result.

## Selected Design

### Metadata And Layout Ownership

`FRDGParametersMetadata` remains a constexpr-friendly non-owning description.
`GetRDGParameterLayout<T>()` owns a function-local static build result for the
typed metadata. The result stores either one immutable layout or one stable
validation error, so valid and malformed typed declarations are each processed
once without relying on metadata-pointer lifetime in a global registry.

Parameter allocation records the exact layout pointer obtained for `T`.
`FGraphPass`, compiled pass runtime, `FRDGParameterResolver`, and
`FRDGShaderParameterScope` propagate that pointer directly; no later consumer
looks it up by metadata address. A module may own its own copy of an inline
type's layout, matching the existing metadata module boundary. Cross-module
process-wide uniqueness is not required.

### Flat Records

The layout owns one leaf record for each non-nested member occurrence after
outer nested arrays are expanded. A leaf's own array remains one group for
shader-binding extent validation. A contiguous element table expands every
leaf array into records containing root-relative byte offsets, leaf indices,
and array-element indices.

Category tables contain compact indices into the element table for textures,
buffers, values, tokens, and attachments rather than duplicate descriptors.
Stable diagnostic paths and a name-sorted shader-binding table are owned once
by the layout. Submission-specific handles, presence, ranges, lowered uses,
and physical resources remain outside the shared layout.

### Resolver Authorization

The resolver normalizes a requested member address to a root-relative offset
and queries a sorted layout index while retaining expected/alternate kind and
optional-form validation. Present `std::optional<T>` members must authorize
both the optional object passed to an optional overload and its contained value
passed to a non-optional overload. Because the contained-value offset is not a
portable static property of `std::optional`, parameter submission records only
the compact instance aliases needed for engaged optionals. Foreign, copied,
wrong-kind, wrong-optional-form, and absent contained-value requests continue
to fail before resource resolution.

### Lowering, Capture, And Shader Composition

Parameterized-pass submission performs one linear value-dependent pass over
the layout elements. That pass reads optional engagement and wrapper values,
normalizes exact uses, and produces the existing scheduling input. Internal
records reference layout leaf/element indices instead of rebuilding metadata
paths where ownership permits; public pointer-free captures materialize their
owned strings at the capture boundary.

Composed shader submission searches the layout's validated shader-binding
index and uses precomputed element offsets. It does not recursively collect
members, count duplicate names, or invoke a full-tree resolver search per
bound element. Cached shader reflection remains authoritative for descriptor
coordinates, and all current domain, type, array-extent, authority, optional,
and exact-view checks remain in force.

## Implementation Stages

### Stage 0: Freeze Cost And Contract Baselines

- [x] Add or extend focused RenderCore characterization fixtures with flat,
  nested, array, optional, value, attachment, and shader-composed parameter
  structures at representative leaf counts.
- [x] Measure allocation/validation, parameterized `AddPass`, compile,
  resolver-heavy execution, and composed shader submission independently in a
  Release-capable CPU benchmark path; record warm-up, sample count, median,
  p95, parameter depth/count, resolve count, and shader-binding count.
- [x] Record the explicit disposition of representative PostProcess and
  Deferred Tracy capture. It is deferred by user direction and no profiling
  evidence is claimed by this plan.
- [x] Record the current allocation count and retained bytes attributable to
  parameter uses, parameter captures, per-submission traversal temporaries,
  and compiled runtime records.
- [x] Freeze semantic oracles for use order, complete parameter capture
  including absent optionals, diagnostic field paths, malformed metadata,
  resolver rejection, composed binding arrays, and copied/foreign parameter
  rejection.
- [x] Select numeric CPU and memory acceptance thresholds from the stable
  baseline. Require no statistically meaningful regression for single-leaf
  passes and a material improvement for resolver-heavy 32- and 128-leaf
  cases; record the selected values here before Stage 1 implementation.

#### Completion Condition

The plan records reproducible baseline evidence, selected numeric gates, and
semantic fixtures sufficient to distinguish traversal savings from allocator,
RHI, or Debug-build noise.

### Stage 1: Introduce The Immutable Parameter Layout

- [x] Add the internal layout, leaf, element, category-index,
  shader-binding-index, and build-result types with bounded offsets and
  deterministic metadata order.
- [x] Build the layout by recursively validating metadata once, enforcing the
  existing maximum nesting, stable layout, member uniqueness, kind/layout,
  graph/shader compatibility, and shader-binding uniqueness rules.
- [x] Generate stable root-relative field paths and expand nested and leaf
  arrays without changing existing capture naming or array ordering.
- [x] Add the type-level function-local static accessor while leaving
  `FRDGParametersMetadata` constexpr-friendly and supporting deterministic
  cached validation failures.
- [x] Propagate the exact layout pointer through allocation, pass declaration,
  compiled runtime, resolver, and shader-parameter scope ownership.
- [x] Prove layout sharing for repeated allocations of the same type and
  isolation for distinct parameter types without a global registry.

#### Completion Condition

Every typed parameter allocation has one validated immutable layout, existing
behavior still uses the legacy consumers, and layout construction and failure
tests pass without changing public authoring semantics.

### Stage 2: Linearize Submission And Capture

- [x] Replace the recursive `AddParameterizedPass` traversal with one ordered
  scan of layout elements that preserves optional presence, owner/type
  validation, exact range normalization, use order, and atomic publication.
- [x] Generate compact engaged-optional address aliases during the same scan;
  do not allocate a node-based address map or duplicate the complete leaf
  descriptor per pass.
- [x] Reference layout indices from internal use/capture state where lifetime
  permits and materialize owned public diagnostic strings only at the capture
  boundary.
- [x] Remove superseded submission-time path construction and metadata
  recursion after equivalence fixtures prove identical scheduling and capture
  semantics.
- [x] Measure per-pass allocation count and retained bytes against Stage 0 and
  keep the single-leaf path within the selected non-regression gate.

#### Completion Condition

Parameterized submission performs one value-dependent linear layout scan,
produces semantically equivalent uses and captures, and adds no unbounded or
node-per-leaf instance structure.

### Stage 3: Index Resolver Authorization

- [x] Replace `FRDGParameterResolver::FindMember` tree recursion with the
  sorted root-offset index plus engaged-optional aliases.
- [x] Preserve exact wrapper identity, expected/alternate kind, optional call
  form, executing-pass identity, and value read/write direction checks.
- [x] Cover first/middle/last fields, nested arrays, leaf arrays, engaged and
  disengaged optionals, attachments, managed resources, typed values, and
  foreign/copied/wrong-kind negative cases.
- [x] Remove resolver-owned recursive traversal and confirm one callback that
  resolves `R` fields no longer performs `R` complete metadata-tree scans.
- [x] Pass the Stage 0 resolver-heavy CPU and memory gates.

#### Completion Condition

Resolver authorization is offset-indexed, all capability death tests retain
their contract, and resolver-heavy workloads meet the frozen improvement gate.

### Stage 4: Reuse Layout For Shader Composition

- [x] Replace execution-time `ComposedMembers` construction with lookup in the
  layout's validated shader-binding table.
- [x] Resolve binding arrays from their leaf group and element offsets without
  invoking full-tree member lookup for each texture or buffer.
- [x] Preserve reflection-owned set/binding coordinates, graph-owned access
  authority, exact texture/buffer views, optional reflection behavior, array
  extent validation, and graphics/compute domain rejection.
- [x] Remove duplicate execution-time binding-name counting and path building
  after positive and negative composition coverage passes.
- [x] Pass the Stage 0 shader-composition CPU and retained-memory gates.

#### Completion Condition

Composed submission consumes cached shader bindings and the shared graph
layout, performs no recursive graph-parameter traversal, and preserves all
current command-recording and failure boundaries.

### Stage 5: Qualify And Document The Unified Path

- [x] Pass focused RenderCore graph-parameter, capture, value, transition, and
  shader-foundation coverage according to the repository testing workflow.
- [x] Pass the smallest registered Renderer scene-contract selections covering
  PostProcess, Deferred, attachments, managed transitions, and composed shader
  submission; expand only when evidence requires it.
- [x] Rerun the frozen synthetic and representative Release CPU measurements
  with the same workload, warm-up, sample count, and machine-contention rules.
- [x] Compare source complexity, allocation count, static layout bytes,
  per-pass retained bytes, median/p95 declaration time, and median/p95
  resolver/composition execution time against Stage 0.
- [x] Update the lasting Render Graph and Shader Parameters contracts to
  describe layout ownership, enumeration, resolver indexing, and binding reuse
  without preserving implementation-stage narrative.
- [x] Pass changed-document and all-plan lifecycle validation.

#### Acceptance Gate

- No hot consumer recursively traverses `FRDGParametersMetadata`; recursive
  interpretation remains only in one type-layout build and explicit cold-path
  diagnostics.
- Scheduling, culling, transition, capture, optional, managed-transition,
  shader-composition, and resolver authorization contracts remain equivalent.
- Resolver-heavy and shader-composed representative cases meet the numeric
  Stage 0 CPU gates, single-leaf cases meet the non-regression gate, and
  per-pass memory meets the frozen retained-byte gate.
- The implementation introduces no global metadata registry, node-based
  per-field lookup map, direct-resource callback API, or RHI ownership change.

## Related Documentation

- [Render Graph](../Runtime/Rendering/RenderGraph.md)
- [Shader Parameters](../Runtime/Rendering/ShaderParameters.md)
- [CPU Profiling](../Development/Build/Profiling.md)
- [Agent Testing Workflow](../Agents/Testing.md)

## Related Code

- `Engine/Source/Runtime/RenderCore/Public/RDG.h`
- `Engine/Source/Runtime/RenderCore/Private/RDG.cpp`
- `Engine/Source/Runtime/RenderCore/Public/Shader/Shader.h`
- `Engine/Source/Runtime/RenderCore/Private/Shader/Shader.cpp`
- `Engine/Tests/Native/RenderCoreTests/Private/RDGTests.cpp`
- `Engine/Tests/Native/RenderCoreTests/Private/ShaderFoundationTests.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/PostProcessRendering.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/DeferredDirectionalLightingRendering.cpp`
