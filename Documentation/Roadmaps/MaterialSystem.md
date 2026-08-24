# Material System Roadmap

Summary: Evolve the landed fixed PBR material stack into authored compiled materials, scalable runtime updates, and complete editor workflows.

Last reviewed: 2026-08-24

Status: Active
Completed:

## Current Status

The fixed-schema material stack is production-capable. Material and material-
instance assets provide stable parameter identities, inheritance, serialization,
dependency tracking, asset-backed default material selection, an independent
error terminal, immutable render representations, stable render proxies, and
coalesced publication. StaticMesh, SkeletalMesh, and Terrain consume the same
validated v3 PBR surface representation and shared Renderer-private surface
material execution contract across forward, GBuffer, and shadow passes.

The Material Editor supports base materials and instances, Undo/Redo, grouped
typed controls, inherited-value provenance, parent and texture asset picking
with Content Browser drag/drop, live rendered previews, sphere/box selection,
orbit controls, and persistent rendered thumbnails. Its earlier preview and
drag/drop backlog is therefore complete. Open-document relocation/deletion
handling, a dedicated full parent-chain view, and broader workflow automation
remain useful editor polish but do not block the material compiler.

The principal remaining product limitation is that every `DMaterial` still
uses the canonical built-in parameter schema and fixed surface shader. Users
cannot author a typed material program, compile expressions into a shader-map
identity, inspect compiler diagnostics, or persist and cook compiled material
artifacts. Runtime-only dynamic material instances and measured reuse/batching
policy have also not landed.

The M6 lifecycle plan is prepared but remains dependency-blocked on M5.
Milestone 5 has satisfied its own entry gate and remains the recommended next
implementation work.

## Outcome

Artists can author bounded, typed surface programs; the engine validates and
compiles them deterministically; every supported geometry family consumes the
result through the existing material/pass boundary; editor and runtime updates
remain responsive; and failures retain a last-known-good or explicit error
surface with actionable diagnostics.

## Scope

- Material-program schema, typed expression graph, surface outputs, parameters,
  deterministic validation, and normalized compiler IR.
- Generated shader source or modules, dependency-derived shader-map identity,
  pass/permutation integration, diagnostics, persistence, cooking, and reload.
- Material graph authoring, compiler feedback, preview, Undo/Redo, and asset
  lifecycle behavior in MaterialEditor.
- Transient runtime material instances, update batching, bounded resource reuse,
  lifetime, diagnostics, and profiling-driven scalability.
- Remaining material-specific editor workflow and end-to-end coverage where it
  is not already owned by shared editor or asset infrastructure.

## Non-Goals

- Replacing the metallic/roughness PBR surface ABI, material render proxy, or
  shared surface-pass executor merely to introduce compilation.
- Letting Renderer or RenderCore read reflected material objects or editor graph
  state.
- Arbitrary user shader source, custom render passes, post-process materials,
  decals, compute materials, ray tracing, or bindless resource indexing in the
  first compiler milestone.
- A runtime-polymorphic geometry-renderer hierarchy.
- Texture streaming or general asset residency; those remain owned by their
  asset and rendering domains.
- Speculative uniform deduplication, descriptor virtualization, or background
  compilation architecture without measured demand and explicit lifetime gates.

## Program Decisions and Invariants

- The current v3 PBR representation and shared surface execution contract are
  the compatibility baseline. Compiler work feeds that boundary before it is
  allowed to extend it.
- Authored graph data belongs to Engine material assets. Engine owns validation,
  normalization, and material-specific IR; RenderCore owns generic Slang
  compilation, reflection, cache storage, and shader resource primitives;
  Renderer owns pass integration and fallback selection; MaterialEditor owns
  authoring UI.
- The first graph domain is a bounded, acyclic, typed surface-expression DAG
  with explicit material outputs. It does not accept arbitrary source snippets
  or implicit type conversions that cannot be diagnosed deterministically.
- Parameter GUIDs remain persistent identity. Display names and graph-node IDs
  do not replace parameter identity in instances, serialization, dependency
  keys, or render publication.
- Compiler identity includes normalized program IR, reachable dependencies,
  static material properties, compiler environment, and the exact requested
  shader/pass contract. Dynamic values never enter shader-map or PSO keys.
- Workers operate on immutable, value-owned snapshots. They do not resolve or
  mutate `DObject` state. Game-thread publication and render-thread resource
  replacement retain the existing ownership boundaries.
- A failed edit or compile never publishes a partial representation. The last
  valid compiled result remains active when available; otherwise the existing
  ErrorMaterial is the terminal fallback, with asset-qualified diagnostics.
- Authored material data remains package state; rebuildable compiler artifacts
  remain derived data or cooked payloads. A plan must lock the exact boundary
  before adding persistent cache or cook formats.
- New resource fields or surface outputs require an explicit versioned layout
  transition and qualification across StaticMesh, SkeletalMesh, Terrain,
  preview, thumbnail, forward, GBuffer, and relevant shadow paths.

## Current Foundations and Gaps

### Landed foundations

- Runtime-owned canonical material parameters, stable GUIDs, base/instance
  inheritance, static properties, package references, and strict schema
  validation.
- Stable material render proxies with lazy parent resolution, publication
  coalescing, stale rejection, startup replay, and counted texture references.
- Exact v3 immutable PBR representation with 48 uniform fields, eight texture
  roles, UV transforms, sampler state, and deterministic per-role fallbacks.
- Asset-backed DefaultMaterial and asset-independent ErrorMaterial with shared
  whole-material fallback diagnostics.
- Shared surface uniform/resource resolution and fragment/pass execution for
  StaticMesh, SkeletalMesh, and Terrain.
- RenderCore Slang compilation, reflection, dependency manifests,
  content-addressed shader artifacts, in-process coalescing, bounded caches,
  development reload, and transactional renderer resource replacement.
- A bounded CPU task system with typed results, cancellation, owner scopes,
  GameThread publication seams, shutdown contracts, and attribution.
- A live Material Editor with inherited source labels, drag/drop-capable asset
  pickers, retained preview meshes, orbit preview, Undo/Redo, and thumbnails.

### Material-specific gaps

- `DMaterial` definitions are required to equal the built-in canonical schema;
  there is no authored expression or output graph.
- Material shader-map identity describes fixed static properties rather than a
  compiled material program and its dependencies.
- Production surface shaders are fixed Renderer source; there is no generated
  material module or material-program binding seam.
- There is no material compile state machine, request generation, cancellation,
  diagnostic model, last-known-good asset state, or cook contract.
- MaterialEditor has no graph canvas, node/pin editing, compiler result panel,
  or source-linked diagnostic navigation.
- There is no transient non-asset material instance API. Existing proxy
  coalescing handles ordinary asset edits, but runtime batching, allocation,
  reuse, and stress limits have not been measured.
- Open material documents are keyed by their original resource ID and do not
  yet participate in the asset relocation/deletion observer path.

## Milestone Map

| Milestone | State | Dependencies | Deliverable | Entry gate | Exit gate |
| --- | --- | --- | --- | --- | --- |
| 1. Material asset and editor foundation | Complete | None | Runtime-owned parameters and instances, stable mesh binding, MaterialEditor, preview, and thumbnails | Historical | Lasting contracts and archived plans record the landed behavior |
| 2. Versioned render representation | Complete | M1 | Immutable validated renderer-facing material layouts and proxy publication | Historical | Renderer consumes compact values without reflected-object lookup |
| 3. Metallic/roughness PBR surface | Complete | M2 | Canonical v3 PBR inputs, texture roles, tangent-space normals, direct lighting, and studio IBL | Historical | Level, preview, and thumbnail output pass focused and Vulkan qualification |
| 4. Material passes and shared execution | Complete | M3 | Opaque/masked/translucent policy plus shared forward, GBuffer, and shadow material execution across production geometry families | Historical | StaticMesh, SkeletalMesh, and Terrain pass the shared execution matrix |
| 5. Material program and synchronous compiler foundation | Ready; recommended next | M4; landed Shader Cache and Shader Parameters contracts | Persisted bounded program schema, typed validation/IR, deterministic dependency identity, and one synchronous compiled surface vertical slice through the existing v3 boundary | Fixed surface ABI and multi-family execution are stable; generic compiler/cache infrastructure is available | Authored program round-trips, invalid graphs fail deterministically, two materially distinct programs compile and render, dependency edits invalidate identity, and fixed-schema content retains explicit fallback/transition behavior |
| 6. Asynchronous compilation, derived data, and cooking | Plan prepared; blocked on M5 | M5; CPU task and asset lifecycle contracts | Cancelable generation-safe compilation, last-known-good publication, persistent diagnostics, derived artifacts, cook/load path, bounded retention, and shutdown handling | M5 identifies immutable inputs/outputs, timings, artifact size, and synchronous failure modes | Editor remains responsive under compile load; stale results cannot publish; warm/miss/cancel/failure/cook/reload/shutdown paths are qualified |
| 7. Material graph authoring workflow | Blocked on M5 and M6 contracts | M5 schema; M6 request/diagnostic model | Graph canvas, node/pin operations, parameters, compiler diagnostics, preview integration, Undo/Redo, copy/paste, and asset lifecycle behavior | Stable serialized schema and compiler diagnostic locations exist | Representative authoring workflows survive save/reload, relocation, deletion, compile failure/recovery, and multi-document editing |
| 8. Runtime dynamic materials and scalability | Evidence-gated | M5 compiled path; preferably M6 lifecycle | Transient non-asset instances plus measured batching/reuse/lifetime policy and stress diagnostics | Profiles identify update frequency, allocation, upload, descriptor, and cache bottlenecks | Runtime updates are bounded, do not mutate assets, preserve proxy/resource lifetime, and meet plan-defined stress budgets |
| 9. Remaining Material Editor lifecycle polish | Conditional; independently selectable | Shared asset mutation APIs | Relocation/deletion synchronization, explicit parent-chain inspection, and missing end-to-end workflow coverage | Shared editor/asset ownership can expose the required notifications without MaterialEditor-local catalog mirrors | Open documents and references respond deterministically to move/delete, and focused workflow tests cover the selected behavior |

## Child Plan Boundaries

| Proposed or completed plan | Milestone | Boundary | Activation |
| --- | --- | --- | --- |
| [Material Parameter Domain Refactor](../Plans/Archive/2026-07/MaterialParameterDomainRefactor.md) | M1 | Runtime-owned parameter schema and editor integration | Complete |
| [Static Mesh Material Slots](../Plans/Archive/2026-07/StaticMeshMaterialSlots.md) and [Static Mesh Indexed Material Overrides](../Plans/Archive/2026-08/StaticMeshIndexedMaterialOverrides.md) | M1 | Mesh slot identity, reconciliation, and component overrides | Complete |
| [Material Render-Proxy Invalidation](../Plans/Archive/2026-08/MaterialRenderProxyInvalidation.md) | M2 | Stable proxy publication and scalable inherited invalidation | Complete |
| [Material Render Representation](../Plans/Archive/2026-08/MaterialRenderRepresentation.md) | M2 | Versioned renderer-facing layout and compact binding | Complete |
| [PBR Material Surface](../Plans/Archive/2026-08/PBRMaterialSurface.md) | M3 | Canonical metallic/roughness surface contract | Complete |
| [Default Material and Error Fallback](../Plans/Archive/2026-08/DefaultMaterialAndErrorFallback.md) | M3 | Valid unassigned default and independent invalid-state terminal | Complete |
| [Material Render Pass Policies](../Plans/Archive/2026-08/MaterialRenderPassPolicies.md) | M4 | Blend, depth, culling, mask, and translucent ordering | Complete |
| [Surface Material Pass Execution](../Plans/Archive/2026-08/SurfaceMaterialPassExecution.md) | M4 | Shared material resource and pass execution across geometry families | Complete |
| Material Program and Compiler Foundation | M5 | One bounded persisted program domain and synchronous end-to-end compiled surface slice; excludes async orchestration and graph canvas | Create only when selected for implementation |
| [Material Compile Lifecycle and Derived Data](../Plans/MaterialCompileLifecycleAndDerivedData.md) | M6 | Async requests, cancellation, diagnostics, last-known-good publication, cache/cook, reload, and shutdown; excludes graph UI | Prepared; implementation begins after M5 exit evidence satisfies Stage 0 |
| Material Graph Editor | M7 | Authoring interaction and compiler feedback over the landed schema/lifecycle; excludes compiler architecture changes | Create after M5 schema and M6 diagnostic contracts stabilize |
| Runtime Dynamic Material Instances | M8 | Non-asset instances and profiling-selected scalability work; excludes authored graph compilation | Create only from measured compiled-path evidence |
| Material Editor Asset Lifecycle | M9 | Move/delete synchronization and selected workflow coverage; excludes graph/compiler design | May be selected independently when editor lifecycle is the priority |

The M5 plan should begin with a characterization and design stage that locks
the smallest useful expression/output domain, serialized ownership, transition
from canonical fixed materials, generated-module boundary, and shader identity.
It should not pre-design M6's asynchronous orchestration or M7's canvas.

## Program Validation Matrix

| Area | Required evidence |
| --- | --- |
| Asset/schema | Deterministic round trip, bounded counts/depth, stable IDs, malformed/cyclic/type-invalid rejection, dependency enumeration, duplication, and strict compatibility behavior |
| Compiler/IR | Schedule-independent normalization, stable keys, source/IR determinism, compiler/reflection failure diagnostics, dependency invalidation, and no reflected-object access outside the owning thread |
| Renderer | Exact layout/binding validation, last-known-good or ErrorMaterial fallback, StaticMesh/SkeletalMesh/Terrain parity, forward/GBuffer/shadow coverage, reload, and device recovery |
| Async lifecycle | Coalescing, supersession, cancellation, stale-result rejection, bounded queues/storage, GameThread publication, module unload, and engine shutdown |
| Cook/runtime | Warm and miss paths, artifact corruption recovery, cooked load without authored-only state, deterministic missing-artifact policy, and package dependency correctness |
| Editor | Graph operations, Undo/Redo, save/reload, multi-document preview, diagnostic navigation, asset move/delete, and failure recovery |
| Scalability | Measured compile latency and artifact size before M6; update, allocation, upload, descriptor, and cache profiles before M8; plan-owned budgets for selected workloads |

Validation selection and execution follow the repository [build and run](../Agents/BuildAndRun.md)
and [testing](../Agents/Testing.md) workflows. Each child plan owns its exact
targets, fixtures, profiles, budgets, and final evidence.

## Risks and Control Gates

- **Schema overreach:** a general-purpose graph can make the first plan
  unbounded. M5 must freeze a minimal surface-only node/output set and reject
  unsupported constructs explicitly.
- **Pass ABI fragmentation:** generated materials could fork geometry-family
  shaders. M5 exits only through the shared surface contract; any ABI extension
  requires one versioned transition and the complete geometry/pass matrix.
- **Identity ambiguity:** graph edits, includes, static properties, and compiler
  environment can otherwise reuse stale artifacts. Normalized IR and complete
  dependency fingerprints are gates before cache persistence.
- **Editor/runtime ownership leaks:** graph nodes or `DObject` pointers must not
  cross to Worker or rendering threads. Compilation inputs are immutable owned
  snapshots and publication returns through explicit owner-thread seams.
- **Failure flicker or data loss:** async work can supersede a valid material.
  M6 must preserve last-known-good state, separate authored dirty state from
  compiled readiness, and reject stale generations before publication.
- **Premature scalability machinery:** existing proxy coalescing may already
  cover many update cases. M8 remains evidence-gated and adds only mechanisms
  justified by measured compiled-material workloads.

## Completion Criteria

- Every required milestone M5 through M8 has passed its exit gate; M9 is either
  complete or explicitly dispositioned with its remaining work routed to an
  owning roadmap or plan.
- Authored compiled materials render through every supported production
  geometry/pass path without bypassing the shared representation and fallback
  contracts.
- Compile, cache, cook, reload, cancellation, failure, recovery, and shutdown
  behavior has bounded diagnostics and acceptance evidence.
- Runtime dynamic updates have measured budgets and no asset mutation or
  lifetime violations.
- Lasting contracts have moved into the owning Runtime and Editor documentation,
  and all child plans retain completion provenance.

## Related Documentation

- [Material System](../Runtime/Rendering/MaterialSystem.md)
- [Shader Cache](../Runtime/Rendering/ShaderCache.md)
- [Shader Parameters](../Runtime/Rendering/ShaderParameters.md)
- [Static Mesh Rendering](../Runtime/Rendering/StaticMeshRendering.md)
- [Skeletal Mesh Rendering](../Runtime/Rendering/SkeletalMeshRendering.md)
- [Terrain Rendering](../Runtime/Rendering/TerrainRendering.md)
- [Asset Thumbnails](../Editor/Architecture/AssetThumbnails.md)
- [Reflected Property Editing](../Editor/Architecture/ReflectedPropertyEditing.md)
- [CPU Task System](../Runtime/Core/TaskSystem.md)
- [Asset Packages](../Runtime/Assets/AssetPackages.md)
- [Asset Data Lifecycle](../Runtime/Assets/AssetDataLifecycle.md)

## Related Code

- `Engine/Source/Runtime/Engine/Public/Materials`
- `Engine/Source/Runtime/Engine/Private/Materials`
- `Engine/Source/Runtime/RenderCore/Private/Shader`
- `Engine/Source/Runtime/Renderer/Private/Renderers`
- `Engine/Source/Editor/MaterialEditor`
- `Engine/Tests/Native/EngineTests/Private/Materials`
