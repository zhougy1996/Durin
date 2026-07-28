# Static Mesh LOD Resources Refactor Plan

Summary: Reshape static-mesh render resources around Unreal Engine-compatible buffer and vertex-factory names while preserving Durin's cooked payload, RHI buffer model, and rendered output.

Last reviewed: 2026-07-29

Status: Active
Completed:

## Current Status

Stage 0 is complete. The existing two-stream vertex layout, Slang input order,
uint32 indices, section draw ranges, fallback UV/color materialization,
resource readiness, canonical derived-data payloads, and a Vulkan rendered
fixture are now pinned by focused tests. Both renderer paths obtain their
otherwise unchanged declaration elements from
`GetStaticMeshVertexDeclarationElements()`, which is explicitly transitional
until `FLocalVertexFactory` assumes ownership in Stage 2.

The deterministic Vulkan baseline imports `MultiSection.gltf`, whose data now
contains an angled normal, negative tangent handedness, authored UVs, and
non-white vertex colors. Its 64-by-64 RGBA output hash is
`52fdb5113401075fabb77a111012afd1`.

Stage 1 is next. `FStaticMeshLODResources` still owns the legacy semantic arrays
and raw RHI references; no UE-named buffer-resource types have landed yet.

## Goal

Establish a static-mesh render-resource model whose names and responsibility
boundaries follow Unreal Engine closely enough that engineers can transfer
their existing mental model directly:

- `FStaticMeshLODResources` owns one LOD's geometry resources and sections;
- `FStaticMeshVertexBuffers` groups position, static-mesh attribute, and color
  vertex buffers;
- `FRawStaticIndexBuffer` owns index storage and its RHI resource;
- `FStaticMeshRenderData` owns parallel LOD-resource and LOD-vertex-factory
  arrays;
- `FStaticMeshVertexFactories` contains the `FLocalVertexFactory` instances
  configured from one LOD;
- `FLocalVertexFactory` owns vertex-fetch interpretation, declaration creation,
  stream binding metadata, and the shader-side vertex-factory contract.

The completed refactor must preserve existing static-mesh assets, material-slot
behavior, rendering output, and the unified `FRHIBuffer` RHI abstraction.

## Scope

- Introduce the UE-compatible render-resource and vertex-factory type names
  needed by the current static-mesh renderer.
- Move CPU vertex/index storage and RHI creation into their corresponding
  buffer resource types.
- Separate vertex color from tangent/UV data at the semantic ownership layer
  and, after validation, at the vertex-stream layer.
- Move static-mesh vertex declaration and vertex stream selection out of
  `RendererModule.cpp` into `FLocalVertexFactory`.
- Keep the index buffer selected by the mesh draw rather than owned or bound by
  the vertex factory.
- Add a Slang vertex-factory module that defines the static-mesh vertex-input
  contract without moving material or pass ownership into it.
- Preserve the current derived-data payload schema unless a measured format
  requirement makes a version change unavoidable.
- Add focused resource, vertex-layout, derived-data, and rendered-path
  validation.

## Non-Goals

- Reproducing Unreal Engine's complete RenderCore, mesh draw command, PSO
  precache, Nanite, ray-tracing, spline-mesh, manual vertex fetch, or LOD
  streaming implementations.
- Adding skeletal meshes, instanced static meshes, mesh shaders, GPU scene, or
  runtime mesh deformation.
- Implementing depth-only, shadow-depth, reversed, wireframe, adjacency, or
  16-bit index buffers in this refactor. The ownership model must leave room
  for them, but they require separate feature work.
- Redesigning the static-mesh importer, material-slot model, or material
  parameter system.
- Replacing Durin's unified `FRHIBuffer` with distinct RHI vertex-buffer and
  index-buffer object hierarchies.
- Adopting every historical Unreal Engine field or preserving exact binary
  layout compatibility with Unreal Engine.
- Introducing general Slang material interfaces, shader graph compilation, or
  pass permutations owned by the Material System plan.

## Design Decisions and Invariants

### Unreal Engine Names Are the Default Vocabulary

Repository-owned types use the Unreal Engine names below when their
responsibilities match:

| Durin type | Responsibility |
| --- | --- |
| `FPositionVertexBuffer` | Position CPU storage, metadata, and vertex-buffer RHI resource. |
| `FStaticMeshVertexBuffer` | Aggregate of tangent-basis and texture-coordinate storage and RHI resources. |
| `FStaticMeshVertexBuffer::FTangentsVertexBuffer` | Independently bindable packed tangent-basis stream owned by `FStaticMeshVertexBuffer`. |
| `FStaticMeshVertexBuffer::FTexcoordVertexBuffer` | Independently bindable texture-coordinate stream owned by `FStaticMeshVertexBuffer`. |
| `FColorVertexBuffer` | Vertex-color storage and its RHI resource. |
| `FStaticMeshVertexBuffers` | Aggregate of the three vertex-buffer resources. |
| `FIndexBuffer` | Generic RenderCore render-resource base for an index-buffer RHI allocation. |
| `FRawStaticIndexBuffer` | Static-mesh index storage, stride metadata, and index-buffer RHI resource. |
| `FVertexFactory` | Generic RenderCore base for vertex-fetch policy and RHI declarations. |
| `FLocalVertexFactory` | Vertex factory for explicit local-space vertex streams. |
| `FStaticMeshVertexFactories` | Per-LOD container of static-mesh vertex factories. |

Durin-specific names are allowed only when Durin intentionally has different
semantics. Such differences must be documented at the declaration rather than
encoded as near-synonyms.

### LOD Resources and Vertex Factories Are Parallel Ownership

`FStaticMeshRenderData` owns:

```text
LODResources[LODIndex]       -> FStaticMeshLODResources
LODVertexFactories[LODIndex] -> FStaticMeshVertexFactories
```

The arrays have identical length whenever resources are initialized. A vertex
factory may reference the stable buffer resource objects in its corresponding
LOD, but it does not own them. Release ordering is vertex factories first,
then LOD buffers. Initialization is LOD buffers first, then vertex factories.

`FStaticMeshLODResources` does not contain `FLocalVertexFactory`. This mirrors
the separation between geometry ownership and vertex-fetch interpretation and
avoids making serialized or streamable LOD state own renderer policy.

### Vertex and Index Buffers Remain Distinct Above RHI

Vertex and index buffers receive different RenderCore resource wrappers because
they have different CPU data, stride, binding, and lifecycle contracts.
Both wrappers continue to create and retain `FRHIBuffer` objects with the
appropriate `EBufferUsageFlags`; no backend-specific resource split is
introduced.

`FLocalVertexFactory` owns vertex declarations and vertex stream descriptions.
It never owns or binds an index buffer. `FRawStaticIndexBuffer` is selected by
the mesh batch or draw submission together with section ranges.

### Initial Vertex Layout

The target semantic model is:

```text
FStaticMeshVertexBuffers
  PositionVertexBuffer
  StaticMeshVertexBuffer
    TangentsVertexBuffer
    TexCoordVertexBuffer
  ColorVertexBuffer
```

`FStaticMeshVertexBuffer` follows UE's public shape: tangent and texture
coordinate data have separate CPU storage, metadata, RHI allocations, and
binding components, while the parent object presents their shared static-mesh
attribute API.

The target physical streams are mandatory in this plan:

| Stream | Data | Required by current base pass |
| --- | --- | --- |
| 0 | Position | Yes |
| 1 | Packed normal and tangent basis | Yes |
| 2 | UV channels | Yes |
| 3 | Color | Yes |

The current payload always materializes white colors and zero-filled missing UV
channels when authored data is absent. The refactor preserves those fallbacks,
so the first `FLocalVertexFactory` declaration has a stable set of streams and
does not introduce optional-attribute permutations.

Position remains separately bindable so future depth-only and shadow-depth
passes can consume it without fetching tangent, UV, or color data. Splitting
tangent, UV, and color streams must be validated with a rendered equivalence
test. A backend limitation blocks Stage 3 acceptance and requires a recorded
design revision; it must not silently collapse Tangent and TexCoord back into
one physical stream.

### CPU Data and Cooked Payload

The existing `FStaticMeshPayloadData` schema remains the authored derived-data
contract. Decode constructs the named buffer resources from its position,
normal, tangent, UV, color, and index arrays. Encode reads the same semantic
data back from those resources.

Moving vectors into named resource objects is not by itself a payload format
change and must not invalidate existing cached static meshes. Any unavoidable
payload schema change requires:

- a recorded reason and version decision in this plan;
- compatibility behavior for existing payloads;
- derived-data key/version updates;
- old/new round-trip and corruption tests.

CPU storage may be released only through an explicit `NeedsCPUAccess` or
streaming policy. This refactor does not silently discard arrays immediately
after upload because current editor and test consumers still inspect LOD data.

### Render-Resource Lifecycle

All new GPU-owning buffer and vertex-factory types participate in the existing
`FRenderResource` lifecycle and are initialized and released on the rendering
thread. They do not rely on public mutation of raw `FBufferRHIRef` fields.

Resource initialization is idempotent. Partial initialization failure leaves
the LOD not ready and permits a later retry without leaking initialized
resources. `FStaticMeshRenderData::IsReadyForRendering()` requires the
corresponding LOD resources and vertex factory to be ready.

### Vertex Factory Owns the CPU/GPU Shader Boundary

`FLocalVertexFactory` is responsible for:

- translating buffer resources into `FVertexElement` declarations;
- recording the stream index and offset for every input semantic;
- exposing the vertex declaration used by PSO creation;
- binding or returning the vertex streams required by a draw;
- validating compatible vertex counts and stream strides;
- identifying its shader-side vertex-factory implementation.

The renderer chooses the LOD, section, material, pass, pipeline, and index
buffer. It must not reconstruct the static-mesh vertex declaration.

### Slang Module Boundary

Introduce a module named and located consistently with
`VertexFactory/LocalVertexFactory.slang`. It owns:

- the local vertex-factory input structure;
- conversion from packed vertex inputs to a pass-neutral vertex-interpolant
  input;
- vertex-fetch helpers shared by static-mesh passes.

It does not own transforms, material evaluation, lighting, fragment outputs,
or pass entry points. `StaticMesh.slang` initially remains the entry-point
module and imports the vertex-factory module.

The first module extraction preserves existing Vulkan locations, formats,
bindings, entry-point names, and reflection. Slang interfaces and generics are
deferred until a second concrete vertex factory or a real shader-composition
requirement exists.

## Current Foundations and Gaps

### Foundations

- `FRenderResource` and `FVertexBuffer` already provide a render-thread
  lifecycle foundation in RenderCore.
- `FRHIBufferCreateDesc` already distinguishes vertex and index usages without
  requiring different backend resource classes.
- Static meshes already upload positions separately from packed attributes and
  bind an independent index buffer.
- `FStaticMeshRenderData` already owns a vector of per-LOD resources.
- Static-mesh derived data already stores semantic arrays independently of the
  current GPU packing.
- The Slang compiler already loads a module, composes it with entry points, and
  records transitive source dependencies in shader-cache identity.

### Gaps

- RenderCore has `FVertexBuffer` but no matching `FIndexBuffer` or
  `FVertexFactory` foundation.
- `FStaticMeshLODResources` exposes raw CPU vectors and raw RHI references
  together.
- `FStaticMeshPackedVertex` combines tangent/UV/color ownership and forces
  every current draw to fetch all attributes together.
- Static-mesh declaration construction is duplicated for the main renderer and
  texture-cube thumbnail path.
- Vertex stream binding is hard-coded in renderer draw code.
- Readiness covers only three raw buffer references and does not validate a
  vertex factory or declaration.
- The Slang static-mesh entry point owns both vertex-fetch layout and material
  surface logic.
- Existing tests cover payload and selected packing behavior but do not pin the
  complete C++ declaration-to-Slang input contract.

## Implementation Stages

### Stage 0: Lock the Existing Contract and Baseline

Outcome: establish exact compatibility expectations before moving ownership.

- [x] Record the current static-mesh vertex semantics, Vulkan locations,
  element formats, strides, fallback UV/color behavior, index stride, and
  section draw-range rules in focused tests.
- [x] Add a resource-readiness test covering empty, malformed, partially
  initialized, initialized, and released LOD resources.
- [x] Add a derived-data fixture test proving an existing payload decodes and
  re-encodes without semantic changes.
- [x] Add or identify one deterministic rendered static-mesh baseline that
  exercises non-default position, normal, tangent handedness, UV0, and vertex
  color.
- [x] Inventory direct consumers of `FStaticMeshLODResources` CPU arrays and
  classify each as import/build, serialization, editor inspection, test, or
  render-thread use.
- [x] Confirm that no current consumer depends on the address stability of the
  exposed vectors or raw RHI reference fields.

#### Acceptance Gate

- The current CPU payload, GPU input layout, draw ranges, and rendered output
  are covered well enough that an ownership or packing regression fails a
  focused test.
- Every direct consumer has an assigned migration path and no unresolved
  lifetime dependency remains.

#### Stage 0 Handoff

- Baseline commit: `8434ff46` (`docs(rendering): plan static mesh LOD resource refactor`).
- Working set:
  `StaticMeshResources.h`, `StaticMesh.cpp`, `RendererModule.cpp`,
  `StaticMeshPayloadCodecTests.cpp`, `StaticModelImportVulkanTests.cpp`, and
  `MultiSection.gltf`.
- Key symbols:
  `FStaticMeshPackedVertex`,
  `GetStaticMeshVertexDeclarationElements()`,
  `FStaticMeshRenderData::InitResources()`, and
  `FStaticMeshRenderData::IsReadyForRendering()`.
- Decision: the declaration helper is a Stage 0 test seam, not the final owner.
  Stage 2 must move the same contract into `FLocalVertexFactory`.
- Decision: readiness now uses the same complete geometry validation as
  initialization, so populated RHI references cannot make a malformed LOD
  renderable.

Direct consumer inventory and migration paths:

| Classification | Current consumers | Migration path |
| --- | --- | --- |
| Import/build | `StaticMesh.cpp` debug, import, tangent/fallback, bounds, and upload paths | Stage 1 initializes and reads the named semantic buffer resources. |
| Serialization | `StaticMeshDerivedData.cpp` payload conversion | Stage 1 uses semantic buffer accessors and preserves the payload schema and hashes. |
| Editor inspection | `LevelEditorViewportClient.cpp` triangle picking | Stage 1 uses const position/index accessors; CPU access remains enabled. |
| Render-thread use | `RendererModule.cpp` draw validation, vertex-buffer binding, and index selection; `StaticMeshComponent.cpp` render-data eligibility | Stage 1 uses buffer RHI/count accessors; Stage 2 moves declaration and stream binding to the local vertex factory. |
| Tests | Static-mesh payload, material, cache, rendering, and editor smoke tests | Migrate alongside each owning production path; retain the Stage 0 assertions as compatibility gates. |

No consumer stores a pointer or reference to a vector object, vector element,
or raw RHI field across mutation. Consumers either build/serialize
synchronously, inspect immutable render data, or copy the RHI reference into an
immediate render command binding.

Validation:

- `StaticMeshTests`: 44 tests passed.
- `FStaticMeshPayloadCodecTests.*`: 10 focused tests passed.
- `FStaticModelImportVulkanTests.RendersReloadedSrgbTextureAndBaseColorFactor`:
  Vulkan rendered baseline passed.
- `FEditorTextureSmokeTests.*`: editor inspection smoke test passed.

### Stage 1: Add UE-Named Buffer Resource Types

Outcome: one LOD owns named vertex and index buffer resources instead of raw
vectors plus raw RHI references.

- [ ] Add RenderCore `FIndexBuffer` beside `FVertexBuffer`, using the same
  `FRenderResource` lifecycle and unified `FRHIBuffer` storage.
- [ ] Add `FPositionVertexBuffer`, `FStaticMeshVertexBuffer`, and
  `FColorVertexBuffer` with initialization, semantic CPU accessors, metadata,
  `InitRHI()`, `ReleaseRHI()`, readiness, and diagnostic names.
- [ ] Give `FStaticMeshVertexBuffer` UE-named
  `FTangentsVertexBuffer TangentsVertexBuffer` and
  `FTexcoordVertexBuffer TexCoordVertexBuffer` members; each member owns its
  own CPU storage metadata and RHI buffer.
- [ ] Add `FRawStaticIndexBuffer` with uint32 initialization, index access,
  stride/count metadata, `InitRHI()`, `ReleaseRHI()`, and readiness.
- [ ] Add `FStaticMeshVertexBuffers` as the aggregate of the three UE-named
  vertex-buffer types.
- [ ] Replace `FStaticMeshLODResources` CPU arrays and raw RHI references with
  `VertexBuffers`, `IndexBuffer`, `Sections`, `LocalBounds`,
  `NumTexCoords`, and `bHasColorVertexData`.
- [ ] Keep compatibility accessors only where a direct consumer cannot migrate
  atomically; mark each accessor with its removal stage and do not preserve raw
  writable vector fields.
- [ ] Move packing and fallback materialization into the owning buffer
  initialization paths.
- [ ] Update import/build and derived-data encode/decode code to use semantic
  buffer APIs without changing the payload schema.
- [ ] Update render-resource initialization, release, and retry behavior to
  operate through the named resources.

#### Acceptance Gate

- All Stage 0 CPU, payload, and lifecycle tests pass.
- No production code reads `PositionVertexBufferRHI`,
  `StaticMeshVertexBufferRHI`, or `IndexBufferRHI` directly from
  `FStaticMeshLODResources`.
- Existing payload fixtures remain compatible and no derived-data version is
  changed solely because of in-memory ownership.

### Stage 2: Introduce the Local Vertex Factory

Outcome: the vertex layout and stream interpretation have a reusable owner
outside `RendererModule.cpp`.

- [ ] Add the minimal `FVertexFactory` RenderCore base needed for declaration
  lifetime, readiness, type identity, and vertex-stream descriptions.
- [ ] Add `FLocalVertexFactory::FDataType` using UE-compatible terminology for
  position, tangent basis, texture coordinates, color, and vertex-count
  metadata.
- [ ] Implement `FLocalVertexFactory::SetData()` and resource initialization
  from `FStaticMeshVertexBuffers`.
- [ ] Make `FLocalVertexFactory` build and own the current static-mesh
  `FVertexDeclarationRHIRef`.
- [ ] Add a draw-facing API that supplies or binds the required vertex streams
  without accepting an index buffer.
- [ ] Add `FStaticMeshVertexFactories` containing one primary
  `FLocalVertexFactory`; leave override-color and spline factories absent until
  their features exist.
- [ ] Add `FStaticMeshRenderData::LODVertexFactories` and enforce one factory
  container per LOD.
- [ ] Initialize LOD buffers before factories and release factories before
  buffers.
- [ ] Move the main static-mesh renderer and texture-cube thumbnail path to the
  same local vertex-factory declaration contract.
- [ ] Remove static-mesh declaration construction and hard-coded per-stream
  binding from `RendererModule.cpp`.

#### Acceptance Gate

- Static-mesh PSOs obtain their vertex declaration from the corresponding
  `FLocalVertexFactory`.
- Static-mesh draws select `FRawStaticIndexBuffer` independently while all
  vertex streams come from the vertex factory.
- Main viewport and texture-cube thumbnail paths use the same declaration
  source and pass the Stage 0 rendered baseline.
- LOD/factory array mismatch, invalid stream counts, or incompatible strides
  fail deterministically before issuing a draw.

### Stage 3: Split the Physical Attribute Streams

Outcome: the UE-named semantic resources correspond to independently bindable
position, tangent, texture-coordinate, and color streams.

- [ ] Replace `FStaticMeshPackedVertex` as persistent upload ownership with
  explicit packed tangent-basis, UV, and color element/storage types.
- [ ] Require separate RHI allocations and vertex streams for
  `TangentsVertexBuffer` and `TexCoordVertexBuffer`; neither is an interleaved
  view into the other.
- [ ] Preserve the current normalized tangent precision, full-precision UVs,
  and normalized 8-bit color behavior.
- [ ] Upload position, tangent basis, texture coordinates, and color as the
  target physical streams selected in this plan.
- [ ] Update `FLocalVertexFactory::FDataType` and declaration generation
  without exposing stream indices to renderer call sites.
- [ ] Preserve white color and zero UV fallbacks so current shader permutations
  remain stable.
- [ ] Remove transitional packed-attribute accessors and upload code after all
  consumers migrate.
- [ ] Measure buffer count, total uploaded bytes, static-mesh initialization
  time, and draw submission behavior against the Stage 0 baseline; record any
  meaningful regression before continuing.

#### Acceptance Gate

- Vertex layout tests prove every semantic maps to the intended buffer, format,
  offset, stride, and Slang input location.
- Rendered output remains equivalent within the established image tolerance.
- Position-only binding is possible through the vertex factory without
  touching tangent, UV, or color buffers.
- No persistent `FStaticMeshPackedVertex` ownership remains.

### Stage 4: Extract the Slang Local Vertex Factory Module

Outcome: shader vertex-fetch ownership matches the C++ vertex-factory
boundary.

- [ ] Add `VertexFactory/LocalVertexFactory.slang` using Slang
  `module`/`import`, public access only for the pass-facing contract, and
  internal visibility for packing helpers.
- [ ] Move the static-mesh input structure and packed-value decoding out of
  `StaticMesh.slang`.
- [ ] Keep transform, material, lighting, and pass entry points outside the
  vertex-factory module.
- [ ] Update static-mesh entry points to import and consume the local
  vertex-factory contract.
- [ ] Verify shader dependency resolution and cache invalidation when the
  imported module changes.
- [ ] Add reflection/ABI tests for entry-point names, vertex inputs, descriptor
  bindings, and pipeline layout before and after extraction.
- [ ] Document how a future vertex-factory type identifies its Slang module or
  composition component without adding unused generic composition machinery.

#### Acceptance Gate

- Editing the imported vertex-factory module invalidates and recompiles every
  dependent shader artifact.
- Static-mesh shader reflection and descriptor bindings remain compatible.
- No pass entry point directly duplicates the C++ local vertex-factory stream
  layout.

### Stage 5: Integration, Cleanup, and Contract Documentation

Outcome: the new ownership model is the only supported path and its lasting
contracts are documented.

- [ ] Remove obsolete raw buffer fields, compatibility accessors, duplicate
  declaration builders, and stale packing helpers.
- [ ] Confirm release/reinitialize behavior across renderer shutdown, static
  mesh replacement, reimport, and failed resource initialization.
- [ ] Run focused Engine, RenderCore, Renderer, shader-cache, static-mesh
  derived-data, material rendering, and viewport tests.
- [ ] Run the repository-prescribed full `all` build because the change crosses
  Engine, RenderCore, RHI-facing declarations, Renderer, and shaders.
- [ ] Run the editor static-mesh import/render/reimport smoke workflow and
  inspect representative vertex-colored and textured meshes.
- [ ] Move stable ownership, lifecycle, and vertex-factory shader contracts
  into the appropriate Runtime rendering documentation.
- [ ] Update the Material System plan only where its vertex-factory requirement
  now has a landed dependency; do not absorb material-pass work into this
  plan.

#### Acceptance Gate

- Focused tests, full build, and editor smoke validation succeed.
- Existing static-mesh packages and cached payloads load without migration.
- Static-mesh rendering has no declaration or stream-layout knowledge in
  `RendererModule.cpp`.
- Lasting runtime contracts are documented outside the plan and all required
  checklists contain evidence.

## Validation Matrix

| Area | Required validation |
| --- | --- |
| Buffer CPU storage | Counts, semantic accessors, default UV/color materialization, tangent packing, clear/reinitialize behavior. |
| Buffer RHI lifecycle | Init, repeated init, release, repeated release, partial failure, retry, and renderer-wide RHI recreation. |
| Index data | uint32 stride, count, bounds validation, section first/count ranges, and independent draw selection. |
| Vertex declaration | Stream, offset, type, attribute location, stride, and vertex-count compatibility for every semantic. |
| Vertex factory | Data setup, readiness, declaration ownership, stream binding, LOD association, and release ordering. |
| Derived data | Existing fixture decode, semantic round trip, cache hit, rebuild, corruption rejection, and unchanged schema identity. |
| Shader modules | Import dependency discovery, cache invalidation, compile diagnostics, reflection, and binding stability. |
| Materials | Base color texture, vertex color, normal/tangent lighting, UV0 sampling, mirrored transform handedness, and material slots. |
| Renderer | Main static-mesh path, texture-cube thumbnail path, wireframe pipeline compatibility, and no draw on invalid resources. |
| Editor | Import, display, material assignment, reimport, resource replacement, shutdown, and restart from existing derived data. |
| Performance | GPU bytes, buffer count, resource initialization time, and representative draw submission comparison. |

Build, test, and runtime commands must use the repository entrypoint and
profiles defined in [Build and Run](../Development/Build/BuildAndRun.md).

## Definition of Done

- `FStaticMeshLODResources` owns `FStaticMeshVertexBuffers`,
  `FRawStaticIndexBuffer`, sections, bounds, and LOD metadata without exposing
  raw RHI references.
- `FStaticMeshRenderData` owns parallel `LODResources` and
  `LODVertexFactories` arrays with explicit initialization and release order.
- Static meshes use `FLocalVertexFactory` through
  `FStaticMeshVertexFactories`.
- Vertex declaration and stream binding are absent from static-mesh renderer
  orchestration code.
- Index-buffer ownership and selection remain separate from the vertex
  factory.
- The Slang local vertex-factory module owns vertex-input decoding while
  materials and passes retain their own responsibilities.
- Existing static-mesh assets and derived-data payloads remain compatible.
- Focused tests, the full `all` build, and editor smoke validation pass.
- Long-lived contracts are published under `Documentation/Runtime/Rendering/`.

## Deferred Follow-ups

- Depth-only and shadow-depth index buffers and position-only shader passes.
- Optional 16-bit index storage selected by mesh size.
- Override-color vertex factories for mesh painting or component-local colors.
- Spline, instanced, skeletal, and procedural vertex factories.
- Manual vertex fetch and SRV-based mesh access.
- LOD streaming, CPU-data discard, and resource replacement batching.
- Reversed and wireframe index buffers where native rasterizer behavior is
  insufficient.
- Mesh shader, ray-tracing geometry, and GPU-scene integration.
- Slang interfaces/generics for vertex-factory and material composition after
  at least two real implementations exist.

## Related Documentation

- [Material System Plan](MaterialSystem.md)
- [Shader Cache](../Runtime/Rendering/ShaderCache.md)
- [Shader Parameters](../Runtime/Rendering/ShaderParameters.md)
- [Build and Run](../Development/Build/BuildAndRun.md)

## Related Code

- `Engine/Source/Runtime/Engine/Public/StaticMesh/StaticMeshResources.h`
- `Engine/Source/Runtime/Engine/Public/StaticMesh/StaticMeshVertexData.h`
- `Engine/Source/Runtime/Engine/Private/StaticMesh/StaticMesh.cpp`
- `Engine/Source/Runtime/Engine/Private/StaticMesh/StaticMeshDerivedData.cpp`
- `Engine/Source/Runtime/RenderCore/Public/RenderResource.h`
- `Engine/Source/Runtime/Renderer/Private/RendererModule.cpp`
- `Engine/Shaders/Slang/StaticMesh.slang`
