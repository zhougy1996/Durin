# Material System Roadmap

Summary: Evolve authored materials from fixed PBR inputs to material-owned parameters, compiled layouts, reusable graphs, and runtime instances.

Last reviewed: 2026-09-11

Status: Active
Completed:

## Current Status

On 2026-09-11 the user selected the
[Reusable Material Functions plan](../Plans/ReusableMaterialFunctions.md) as M11:
typed function assets, texture-driven StandardPBR calls, Surface access, editor
authoring and explicit upgrades of the small existing asset set. The proposal is
recorded; Stage 0 is next and no implementation stage is complete.

On 2026-09-10 the user selected the
[Material Instance Shader Variants plan](../Plans/MaterialInstanceShaderVariants.md)
as M13: one root graph with per-field instance configurations and matching shared
compiled variants. Common ownership, publication, instance Cook and editor/import
integration are implemented; the plan remains active for final qualification. It
extends the M5/M6 compile and Cook foundations and precedes M8's runtime instance
API work; it does not introduce runtime shader compilation or complete M11.

The [Material Parameters and Compiled Layouts plan](../Plans/Archive/2026-09/MaterialParametersAndCompiledLayouts.md)
completed M10 on 2026-09-10: material-owned declarations, compiled layout v4, generic production
bindings, authored migration, DMAT v4 and the dual-layer rust fixture. The
fixed-v3 compiler and Renderer adapters are retired. On 2026-09-10 the user
cancelled the unavailable historical pre/post image and performance baseline;
current CPU/GPU correctness, migration, Cook and resource-bound checks remain
required. See the child plan for final receipts and unrelated macOS test exceptions.
M1-M7 retain their historical completion. M8 follows M10, and reusable functions
follow as M11; these future slices are not delivered by the M10 plan.

The compiled-layout material stack is production-capable. Material and material-
instance assets provide stable parameter identities, inheritance, serialization,
dependency tracking, asset-backed default material selection, an independent
error terminal, immutable render representations, stable render proxies, and
coalesced publication. StaticMesh and SplineMesh consume the same
validated material-specific representation and shared Renderer-private surface
material execution contract across forward, GBuffer, and shadow passes.

The Material Editor supports base materials and instances, Undo/Redo, grouped
typed controls, inherited-value provenance, parent and texture asset picking
with Content Browser drag/drop, live rendered previews, sphere/box selection,
orbit controls, persistent rendered thumbnails, and the completed M7 typed
material graph workflow. Canvas and structured callers share one command
surface with reflected presentation, atomic transactions, versioned clipboard,
diagnostic navigation, and open-document relocation/deletion handling. A
dedicated full parent-chain view remains useful editor polish but does not block
the material compiler.

Every `DMaterial` now owns a bounded typed program and deterministic compiled
shader-map identity. Compilation is generation-safe, cancelable, shared by
identity, last-known-good, and visible in MaterialEditor; Win64 Game Cook emits
a strict DMAT payload that loads without authored graph state or live
compilation. M7 command-driven authoring, human canvas, structured automation,
and diagnostic navigation are complete. Runtime-only dynamic material instances
and measured reuse/batching policy remain unlanded.

The [M5 material-program and compiler foundation plan](../Plans/Archive/2026-08/MaterialProgramAndCompilerFoundation.md)
is complete: bounded authored programs compile deterministically and render
through every production surface consumer. The
[M6 lifecycle plan](../Plans/Archive/2026-08/MaterialCompileLifecycleAndDerivedData.md) is
complete: Engine owns bounded Worker orchestration and Cook admission while
ShaderBuild now owns shader-artifact DDC behind RenderCore contracts. M7 graph authoring is
complete; M8's runtime instance API is selected, while advanced batching and
reuse remain evidence-gated on measurements after M10.

## Outcome

Artists can author bounded, typed surface programs; the engine validates and
compiles them deterministically; every supported geometry family consumes the
result through the existing material/pass boundary; editor and runtime updates
remain responsive; and failures select an explicit error
surface with actionable diagnostics.

Materials declare their own bounded typed inputs independently of surface
outputs. The compiler derives bindings, instances retain stable override
identities, and runtime consumers use matching code, layout and resource data.

## Scope

- Material-owned parameter declarations, compiled input layouts, identity-safe
  authoring and migration away from fixed role bindings, followed by reusable
  material functions with explicit inputs and outputs.
- Material-program schema, typed expression graph, surface outputs, parameters,
  deterministic validation, and normalized compiler IR.
- Generated shader source or modules, dependency-derived shader-map identity,
  pass/permutation integration, diagnostics, persistence, cooking, and reload.
- Command-driven material graph operations, a human canvas and structured
  automation surface, compiler feedback, preview, Undo/Redo, and asset lifecycle
  behavior in MaterialEditor.
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

- Compiled layout v4 replaces the historical fixed input table while retaining
  the eight surface outputs and shared pass execution. Legacy authored content
  migrates before compilation. Current correctness establishes the retained
  acceptance scope; historical baseline comparison was cancelled by the user.
- Authored graph data belongs to Engine material assets. Engine owns validation,
  normalization, material-specific IR and layout policy; RenderCore owns generic
  shader contracts and resource primitives; ShaderBuild owns live Slang
  compilation, dependency manifests and shader-artifact DDC;
  Renderer owns pass integration and fallback selection; MaterialEditor owns
  authoring UI.
- The first graph domain is a bounded, acyclic, typed surface-expression DAG
  with explicit material outputs. It does not accept arbitrary source snippets
  or implicit type conversions that cannot be diagnosed deterministically.
- `DMaterial` remains the asset and package object. Its `FMaterialProgram`,
  nodes, links, surface outputs, and graph presentation remain reflected bounded
  values rather than per-node `DObject` subobjects. Stable GUIDs, not object
  addresses or canvas coordinates, identify authored nodes and connections.
- MaterialEditor canvas code, structured automation, and tests must share one
  UI-independent graph inspection and command surface. Screen-coordinate input
  is never the authority for graph mutation or verification.
- Node presentation is authored package state but remains separate from graph
  semantics and is excluded from normalized IR, compiler identity, compile
  snapshots, derived data, and cooked payloads.
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
transition and qualification across StaticMesh, SplineMesh,
  preview, thumbnail, forward, GBuffer, and relevant shadow paths.

## Current Foundations and Gaps

### Landed foundations

- Material-owned bounded declarations, stable GUIDs, base/instance inheritance,
  typed texture sampling/fallback data, static properties, package references,
  bounded legacy migration, and strict schema validation.
- Stable material render proxies with lazy parent resolution, publication
  coalescing, stale rejection, startup replay, and counted texture references.
- Deterministic compiled layout v4 with typed numeric fields, compact texture /
  sampler indices, explicit fallbacks, reflected pass validation, and DMAT v4.
- Asset-backed DefaultMaterial and asset-independent ErrorMaterial with shared
  whole-material fallback diagnostics.
- Shared surface uniform/resource resolution and fragment/pass execution for
  StaticMesh and SplineMesh.
- RenderCore Slang compilation, reflection, dependency manifests,
  content-addressed shader artifacts, in-process coalescing, bounded caches,
  development reload, and transactional renderer resource replacement.
- A bounded CPU task system with typed results, cancellation, owner scopes,
  GameThread publication seams, shutdown contracts, and attribution.
- A live Material Editor with inherited source labels, drag/drop-capable asset
  pickers, retained preview meshes, orbit preview, Undo/Redo, and thumbnails.

### Material-specific gaps

- Shader-affecting instance overrides now request matching accepted
  variants, including scene-import overrides. Final platform/workflow qualification
  remains active.
  M13 owns per-field inheritance, instance compilation/publication, Cook and the
  editor/import workflow. Its detailed acceptance gates belong to the child plan.
- Reusable explicit-input functions remain absent and belong to M11. Legacy
  role knowledge is confined to authored migration; the independent error
  terminal uses the resource-free compiled-layout boundary.
- There is no transient non-asset material instance API. Existing proxy
  coalescing handles ordinary asset edits, but runtime batching, allocation,
  reuse, and stress limits have not been measured.

## Milestone Map

| Milestone | State | Dependencies | Deliverable | Entry gate | Exit gate |
| --- | --- | --- | --- | --- | --- |
| 1. Material asset and editor foundation | Complete | None | Runtime-owned parameters and instances, stable mesh binding, MaterialEditor, preview, and thumbnails | Historical | Lasting contracts and archived plans record the landed behavior |
| 2. Versioned render representation | Complete | M1 | Immutable validated renderer-facing material layouts and proxy publication | Historical | Renderer consumes compact values without reflected-object lookup |
| 3. Metallic/roughness PBR surface | Complete | M2 | Canonical v3 PBR inputs, texture roles, tangent-space normals, direct lighting, and studio IBL | Historical | Level, preview, and thumbnail output pass focused and Vulkan qualification |
| 4. Material passes and shared execution | Complete | M3 | Opaque/masked/translucent policy plus shared forward, GBuffer, and shadow material execution across production geometry families | Historical | StaticMesh and SplineMesh pass the shared execution matrix |
| 5. Material program and synchronous compiler foundation | Complete | M4; landed Shader Cache and Shader Parameters contracts | Persisted bounded program schema, typed validation/IR, deterministic dependency identity, and one synchronous compiled surface vertical slice through the existing v3 boundary | Fixed surface ABI and multi-family execution are stable; generic compiler/cache infrastructure is available | Authored program round-trips, invalid graphs fail deterministically, two materially distinct programs compile and render, dependency edits invalidate identity, and fixed-schema content retains explicit fallback/transition behavior |
| 6. Asynchronous compilation, derived data, and cooking | Complete | M5; CPU task and asset lifecycle contracts | Cancelable generation-safe compilation, last-known-good publication, bounded diagnostics, non-duplicative cache ownership, cook/load path, bounded retention, and shutdown handling | M5 identifies immutable inputs/outputs, timings, artifact size, and synchronous failure modes | Editor remains responsive under compile load; stale results cannot publish; warm/miss/cancel/failure/cook/reload/shutdown paths are qualified |
| 7. Material graph authoring workflow | Complete | M5 schema; M6 request/diagnostic model | Shared graph inspection/command surface, reflected presentation data, human canvas, structured automation, node/pin operations, compiler diagnostics, preview integration, Undo/Redo, copy/paste, and asset lifecycle behavior | Stable serialized schema and compiler diagnostic locations exist | Equivalent canvas and structured authoring workflows survive save/reload, relocation, deletion, compile failure/recovery, and multi-document editing without coordinate-based automation or semantic/identity drift |
| 8. Runtime dynamic materials and scalability | Required API; optimization evidence-gated | M10 and M13 | Transient instances, atomic multi-parameter updates and measured lifetime/cost bounds | Stable compiled layouts and accepted variant contract; representative workloads defined before optimization | Independent objects update without asset mutation or compilation; measurements justify batching/reuse |
| 9. Remaining Material Editor lifecycle polish | Conditional; independently selectable | Shared asset mutation APIs | Explicit parent-chain inspection and any newly selected end-to-end workflow polish | A concrete user workflow remains unserved after M7 | The selected workflow has focused coverage without duplicating shared editor infrastructure |

## Refactor Milestones

| Milestone | State | Dependencies and entry gate | Deliverable and completion condition |
| --- | --- | --- | --- |
| 10. Material-owned parameters and compiled layouts | Complete; historical baseline cancelled by user | Landed M5-M7; current correctness and resource bounds | Custom numeric/Texture2D inputs through editor, instances, renderer and Cook; rust-material vertical slice, migrated-content correctness, fixed-role production bindings retired |
| 11. Reusable material functions | Selected; plan active | M10's parameter/layout contract is stable | Explicit typed function calls, dependency lifecycle and standard PBR templates; shared edits invalidate callers safely and separate calls keep independent inputs |
| 12. Context expressions and output extensions | Conditional | Concrete effect selected after M10; M11 where useful | Selected time/world/view inputs, or a separately scoped vertex/shading/domain extension; geometry, shadow, Cook and recovery qualification for that effect |
| 13. Material instance shader variants | Implementation landed; qualification active | M10 complete; reuse M5/M6 lifecycle and current geometry interfaces | One graph with per-field instance configurations, shared matching programs, atomic publication, authored migration, Cook and editor/import qualification |

Parameter GUIDs identify declarations within a root material; node GUIDs identify
expression occurrences. Names are case-insensitive lookup keys in one root scope.
Same-name/same-type creation reuses a declaration; conflicting types fail. Rename
preserves identity; merge/retype cannot silently reinterpret overrides. Opcode
types stay enums until a concrete plugin-extension requirement justifies change.

A renderable generation owns compatible program, layout, resolved values and
counted resources. Pending edits retain that complete generation; failed edits retire it and display
ErrorMaterial. Neither path can pair old code with new layout.
Dynamic values remain outside program/PSO identity. Input declarations are
independent of the eight surface outputs; M10 does not expand shading models.

Coordinate with [Geometry Submission Refactor](../Plans/GeometrySubmissionRefactor.md),
[RHI Resource Creation Refactor](../Plans/Archive/2026-09/RHIResourceCreationRefactor.md) and
`Documentation/Plans/Archive/2026-09/PayloadArchiveSerializationRefactor.md` (currently in the
coordinating checkout).
Consume their accepted interfaces without introducing a second geometry dispatch,
resource scheduler or payload publication mechanism.

## Child Plan Boundaries

M10 is complete; M11 is selected for function and Import refactoring. M13 establishes the
accepted variant boundary before M8. M11 remains independently selectable after
M10, but overlapping material source changes must be sequenced or integrated in
separate writer checkouts. M12 creates only the bounded plan demanded by an actual
effect. M13 keeps runtime dynamic edits outside shader compilation.

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
| [Material Program and Compiler Foundation](../Plans/Archive/2026-08/MaterialProgramAndCompilerFoundation.md) | M5 | One bounded persisted program domain and synchronous end-to-end compiled surface slice; excludes async orchestration and graph canvas | Complete |
| [Material Compile Lifecycle and Derived Data](../Plans/Archive/2026-08/MaterialCompileLifecycleAndDerivedData.md) | M6 | Async requests, cancellation, diagnostics, last-known-good publication, cache/cook, reload, and shutdown; excludes graph UI | Complete |
| [Material Graph Editor](../Plans/Archive/2026-08/MaterialGraphEditor.md) | M7 | Command-driven authoring, reflected presentation, human canvas, structured automation, and compiler feedback over the landed schema/lifecycle; excludes compiler architecture changes and per-node object graphs | Complete |
| [Material Parameters and Compiled Layouts](../Plans/Archive/2026-09/MaterialParametersAndCompiledLayouts.md) | M10 | Declarations, compiled bindings, instances, editor, migration and Cook | Complete; historical baseline cancelled by user |
| [Material Instance Shader Variants](../Plans/MaterialInstanceShaderVariants.md) | M13 | Per-field configuration, shared variant lifecycle, rendering, migration, Cook and editor/import integration | Implemented; final qualification in progress |
| Runtime Dynamic Material Instances | M8 | Non-asset instances and measured updates; advanced reuse requires profiling evidence | Create after M13 |
| [Reusable Material Functions](../Plans/ReusableMaterialFunctions.md) | M11 | Typed function assets, Surface access, texture-driven PBR library, editor workflow and asset migration | Selected; Stage 0 next |
| Material Context and Output Extensions | M12 | One selected effect or output domain per bounded plan | Conditional on concrete effect and stage contract |
| Remaining Material Editor Polish | M9 | Explicit parent-chain inspection and any newly selected workflow coverage; excludes graph/compiler design | Select only when a concrete post-M7 workflow is unserved |

M5 locked the smallest useful expression/output domain, serialized ownership,
transition from canonical fixed materials, generated-module boundary, shader
identity, and synchronous Renderer publication. M6 landed the asynchronous
lifecycle. M7 landed the shared semantic editing boundary and projects the same
program into a human canvas and structured automation workflow.

## Program Validation Matrix

| Area | Required evidence |
| --- | --- |
| Asset/schema | Deterministic round trip, bounded counts/depth, stable IDs, malformed/cyclic/type-invalid rejection, dependency enumeration, duplication, and strict compatibility behavior |
| Compiler/IR | Schedule-independent normalization, stable keys, source/IR determinism, compiler/reflection failure diagnostics, dependency invalidation, and no reflected-object access outside the owning thread |
| Renderer | Exact layout/binding validation, pending last-known-good and failed ErrorMaterial fallback, StaticMesh/SplineMesh parity, forward/GBuffer/shadow coverage, reload, and device recovery |
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
  Preserve last-known-good state while compiling, retire failed generations to
  ErrorMaterial, separate authored dirty state from compiled readiness, and reject
  stale generations before publication.
- **Premature scalability machinery:** existing proxy coalescing may already
  cover many update cases. M8 remains evidence-gated and adds only mechanisms
  justified by measured compiled-material workloads.

## Completion Criteria

- Historical M5-M7 and required M10, M13, M8 and M11 have passed their exit gates;
  M9 and M12 are completed or explicitly dispositioned with remaining work
  routed to an owning roadmap or plan.
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
- [Asset Thumbnails](../Editor/Architecture/AssetThumbnails.md)
- [Reflected Property Editing](../Editor/Architecture/ReflectedPropertyEditing.md)
- [Material Graph Operations](../Editor/Architecture/MaterialGraphOperations.md)
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
