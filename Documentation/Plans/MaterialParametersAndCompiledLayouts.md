# Material Parameters and Compiled Layouts Plan

Summary: Replace fixed PBR input identities and bindings with material-owned parameters and compiled layouts across authoring, instances, rendering, migration, and Cook.

Last reviewed: 2026-09-09

Status: Active
Completed:

## Current Status

Selected by the user on 2026-09-09 as M10 of the
[Material System roadmap](../Roadmaps/MaterialSystem.md). Documentation only:
no implementation stages or qualification gates are complete. Another agent
is working in this checkout. This planning task modifies only this plan and
its roadmap; implementation must acquire the single-writer checkout or use a
separate worktree before modifying source, assets or build outputs.

The inspected baseline has 56 canonical GUIDs, exact-schema validation,
role-based StandardSurface generation and exact-v3 render bindings. Preserve
existing asynchronous compilation, instances, immutable proxies, stale-result
rejection and Cook. Geometry submission is already being generalized; reconcile
the current code rather than restoring historical per-family renderer paths.

## Goal

Author independently named numeric and Texture2D parameters, compile their
reachable bindings, override them in multiple asset instances and render/cook
them through all supported surface consumers without fixed role slots.

The acceptance fixture is a dual-layer rust material exposing at least
MetalColorTexture, MetalNormalTexture, RustColorTexture, RustNormalTexture,
RustMaskTexture, RustAmount and RustTiling. It mixes layers into the existing
surface outputs, with instance-specific values and textures.

## Scope and Non-Goals

Includes parameter CRUD and references, instance inheritance/orphans, editor
commands and clipboard, compiled layouts, render publication, authored migration,
Cook, default/error surfaces and import-created materials. Initial value types
are Float, Float2, Float3, Float4 and Texture2D with explicit sampling policy.
A built-in ordinary-graph PBR template replaces StandardSurface's hidden role
access during migration; general function assets belong to M11.

Excludes dynamic runtime instances (M8), general material functions (M11), static
switch parameters, new shading models, vertex displacement, arbitrary shader
source, bindless descriptors, other texture dimensions and material domains.
Do not add another compiler scheduler, DDC, RHI creation queue, asset mutation
service or geometry submission path.

## Selected Decisions

### Ownership and identity

- Engine owns declarations, validation, material IR, reachability, layout policy
  and accepted results. MaterialEditor owns UI over shared semantic commands.
  RenderCore owns generic shader/reflection contracts and shader maps;
  ShaderBuild owns live compilation and shader-artifact caching. Renderer owns
  pass adaptation and device resources through current RHI contracts.
- Each root definition has stable ParameterId, case-insensitive FName, type,
  default and metadata. NodeId identifies an expression occurrence. Arithmetic
  nodes have no parameter identity; outputs remain semantic surface properties.
- Same-name/same-type creation reuses the definition without replacing its
  default/metadata. Same-name/different-type creation fails. Rename preserves
  GUID and updates all referencing labels; renaming to an occupied name fails.
  Explicit parameter merge and in-place retyping are outside this slice. Type
  replacement creates a new GUID instead of reinterpreting existing overrides.
- Names, metadata, node GUIDs and presentation do not change executable identity.
  Declaration GUIDs may conservatively contribute to layout/program identity;
  deduplicating equivalent graphs across independently authored roots is not a
  requirement of this slice. Dynamic values stay outside shader/PSO keys.
- Root duplication preserves internal GUIDs under a new asset identity. Parent
  switching matches only GUID and type, not names. Unrelated roots with the same
  parameter names must not accidentally share overrides or values.

### Instances, layout and publication

- Instances own no graph or layout. Resolve child-to-parent-to-root active
  overrides by GUID/type. Deleted or unreachable definitions leave inspectable
  orphan overrides; explicit removal discards them. Restoring the same GUID
  restores eligibility; type-mismatched data never binds.
- Compilation produces a deterministic versioned layout: uniform offsets,
  texture indices, sampler information and per-pass reflected bindings. Remove
  role-to-slot switches from general resolution. Renderer consumes validated
  compact bindings without DObject or name lookups.
- Texture samples use explicit Texture2D inputs and UV expressions. UV
  transforms become ordinary numeric calculations. Sampling settings are bounded
  explicit data, not arbitrary floats encoding hidden roles. Stage 0 freezes
  their representation and invalidation behavior. Texture fallback is explicit,
  type-compatible and independent of parameter name; normal decoding is explicit.
- An accepted generation couples program, layout, resolution schema, static
  shader identity and counted resources. Keep the previous accepted schema during
  graph compilation. Compatible dynamic edits may update that schema; new-only
  inputs wait for new code. Old code must never consume the edited layout.
- GameThread snapshots and admits value-owned work; workers retain no reflected
  objects. RenderThread applies versioned immutable data. Preserve owner/request/
  dependency checks, coalescing, lazy parent invalidation, cache bounds, shutdown,
  last-known-good and error behavior.
- Layout and shader-affecting settings enter compatibility identities; values
  and texture selections do not. Existing static instance overrides must use
  compatible permutations and cannot silently reuse incompatible parent code.

### Migration and persisted data

- New materials have no mandatory 56-definition seed. PBR templates create
  ordinary declarations and expressions with familiar names.
- Version authored schemas explicitly. Upgrade supported older authored content
  deterministically, preserving parameter GUIDs, defaults, unreachable values,
  instance references and usable presentation. Expand StandardSurface into
  ordinary expressions/explicit Surface construction without hidden role access.
- Preserve baseline formulas and channels: additive emissive; metallic B,
  roughness G, AO/mask R, independent opacity A; RG normal decode/RNM, roughness
  clamp, UV scale-rotation-offset, sampler behavior and texture fallbacks.
- Importers and new-material creation use the same template/declaration owner.
  DefaultMaterial remains authored content; ErrorMaterial remains an independent
  valid terminal compatible with the new binding boundary.
- New DMAT stores complete validated code/layout metadata through the current
  BulkData/archive APIs. Old cooked formats are rejected with a recook diagnostic;
  Game loading needs neither live compilation nor authored graphs. Freeze actual
  version numbers after Stage 0 audits the baseline. Migration never auto-saves
  packages or overwrites malformed authored content.

## Implementation Stages

### Stage 0: Freeze interfaces and qualification

Dependencies: exclusive source writer acquired and current related interfaces
reconciled. Outcome: selected wire/layout decisions and reproducible baseline.

- [ ] Record baseline commit and dirty-state disposition; inventory Engine,
  Renderer, MaterialEditor and importer role-table consumers, current geometry
  pass interfaces, RHI readiness and payload serialization contracts.
- [ ] Freeze declaration/layout/pass records, alignment and numeric/resource
  bounds against actual target capabilities. Select explicit sampler and fallback
  representation, including sampler-only invalidation behavior and preservation
  of existing per-instance packed sampler overrides.
- [ ] Specify authored/clipboard/DMAT transitions and cross-root paste remapping.
  Same-root paste reuses definitions; foreign declarations use validated remaps
  with name/type/default conflict diagnostics, not unexamined foreign GUIDs.
- [ ] Freeze accepted-schema behavior during deletion/type replacement/parent
  edits, static-instance permutations, binding failures and template expansion
  near graph size limits. Resolve blocking design choices before Stage 1.
- [ ] Select registered CPU/GPU tests; capture fixed scene inputs and baseline
  images. Record numeric image tolerances, CPU/upload/allocation/retention/GPU
  budgets and sampling conditions before changing rendering behavior.

Completion: concrete decisions, schema versions, bounds and baseline receipts
are recorded here. This draft does not count as passing the baseline gate.

### Stage 1: Add declarations and identity-safe editing

Dependencies: Stage 0. Outcome: custom declarations persist and resolve.

- [ ] Implement bounded declarations and atomic definition-plus-graph mutations;
  add create/reuse/rename/delete and explicit orphan removal via shared commands.
- [ ] Implement reachability, defaults, multi-level overrides, orphan retention
  and parent-cycle rejection without canonical-role schema validation.
- [ ] Extend details, catalog, promotion, transactions, clipboard and diagnostics;
  one semantic operation owns one transaction and compile request.
- [ ] Verify conflicts, rename, type replacement, duplication, parent switching,
  cross-root paste, Undo/Redo and save/reload. Assign any temporary adapter its
  Stage 4 removal; never map custom parameters secretly to canonical slots.

Completion: identity/persistence checks pass and canvas/structured operations
agree. Custom-parameter GPU support is not claimed before Stages 2-3.

### Stage 2: Compile layouts and complete results

Dependencies: Stage 1. Outcome: reachable custom inputs generate validated code
and bindings independent of their names or PBR role.

- [ ] Normalize reachable declarations and explicit texture/UV/sampler inputs;
  generate deterministic layouts/source, including Float4 and resource-free graphs.
- [ ] Validate reflected forward, GBuffer and masked-shadow bindings against the
  layout; support optimized-out bindings and resource-free opaque shadows. Reject
  overlapping, duplicate, wrong-type and oversized layouts.
- [ ] Carry layout compatibility and immutable schema through compiler results,
  shared requests, cancellation, stale admission and bounded caches.
- [ ] Verify value/name/presentation stability versus graph/type/layout identity
  changes. Inject invalid reflection, failed compilation and stale completion
  while an older accepted layout remains visible.

Completion: deterministic compiler/lifecycle tests pass; partial pass sets and
mismatched accepted schemas cannot publish.

### Stage 3: Bind custom materials in production rendering

Dependencies: Stage 2 and reconciled geometry/RHI contracts.

- [ ] Replace fixed-table proxy resolution with accepted-layout population,
  retained parent schemas, counted resources and a complete error terminal.
- [ ] Adapt common mesh-pass bindings and pipelines through current factory/pass
  interfaces. Cover StaticMesh, SplineMesh, registered independent geometry,
  Material Preview and thumbnails.
- [ ] Render the rust fixture with independent asset instances and varied parameter
  names/types/resource counts. Verify edits reuse code and unrelated same-named
  roots retain independent values; test explicit missing-texture fallbacks.
- [ ] Exercise reload, device invalidation, unavailable resources, rapid edits,
  parent changes and teardown. Assert no old-code/new-layout combination during
  pending, failed or superseded compilation.

Completion: production pass tests meet Stage 0 tolerances and resource budgets;
all supported consumers use the same binding contract.

### Stage 4: Migrate content and Cook; retire fixed bindings

Dependencies: Stage 3. Outcome: one production path handles new/migrated content.

- [ ] Implement bounded authored migration and ordinary-graph PBR templates,
  including StandardSurface, UV/sampler overrides, default assets and importers.
- [ ] Implement DMAT layout/code serialization and strict validation through
  current archive/BulkData APIs. Reject corrupt, trailing, incompatible and old
  cooked payloads; require current successful results for Cook.
- [ ] Verify migrated base/instance chains, orphan overrides and save/reload;
  compare baseline images including masked shadows and translucent behavior.
- [ ] Render new and migrated cooked content with ShaderBuild and authored-source
  access unavailable; compare authored/cooked results.
- [ ] Remove temporary adapters, exact-v3 production validators and hidden role
  lookup from general compilation/rendering. Retain only bounded authored-format
  migration knowledge needed to load supported older packages.

Completion: migration parity and cooked tests pass; fixed-role runtime binding
is unnecessary for default, error, imported and graph-authored materials.

### Stage 5: Qualify and publish contracts

Dependencies: Stage 4.

- [ ] Run selected tests and compare baseline CPU/upload/allocation/retention/GPU
  measurements at equivalent scene counts. Investigate regressions against frozen
  budgets rather than silently relaxing them.
- [ ] Exercise bounded malformed inputs, repeated compile/edit/load/unload,
  cache eviction and shutdown; record commands, hardware and receipts.
- [ ] Update implemented Material System and Material Graph Operations contracts
  and affected Cook/shader documentation. Update M10 status and hand off stable
  interfaces to future M8/M11 plans.

Completion: every preceding gate passes with recorded evidence and lasting
contracts are updated. Dynamic instances/functions are not claimed as delivered.

## Validation and Coordination

Follow [agent build/run](../Agents/BuildAndRun.md) and
[agent testing](../Agents/Testing.md) before implementation validation. Select
tests from the current registry; do not start another build in a checkout with
an active process tree. Documentation validation closes no implementation box.

Coordinate with [Geometry Submission Refactor](GeometrySubmissionRefactor.md),
[RHI Resource Creation Refactor](RHIResourceCreationRefactor.md), and
[Payload Archive Serialization Refactor](PayloadArchiveSerializationRefactor.md).
Consume their accepted interfaces; do not edit their code/plans incidentally.
Record changed interface decisions here before continuing dependent stages.
Implementation commits use this exact plan path and actual Stage title as
Plan/Stage trailers under repository handoff rules.

## Related Code

- `Engine/Source/Runtime/Engine/Public/Materials/MaterialTypes.h`
- `Engine/Source/Runtime/Engine/Public/Materials/MaterialProgramTypes.h`
- `Engine/Source/Runtime/Engine/Private/Materials/MaterialParameterTypes.cpp`
- `Engine/Source/Runtime/Engine/Private/Materials/MaterialProgramCompiler.cpp`
- `Engine/Source/Runtime/Engine/Private/Materials/MaterialProgramGenerator.cpp`
- `Engine/Source/Runtime/Engine/Private/Materials/MaterialCompileLifecycle.cpp`
- `Engine/Source/Runtime/Engine/Private/Materials/MaterialRenderProxy.cpp`
- `Engine/Source/Runtime/Engine/Private/Materials/MaterialCookedProgram.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers`
- `Engine/Source/Runtime/RenderCore/Public/Shader`
- `Engine/Source/Developer/ShaderBuild`
- `Engine/Source/Editor/MaterialEditor/Private/Graph/MaterialGraphOperations.cpp`
- `Engine/Source/Editor/AssetForgeBuiltins/Private/ImportedSurfaceMaterial.cpp`
- `Engine/Tests/Native/EngineTests/Private/Materials`
- [Material System](../Runtime/Rendering/MaterialSystem.md)
- [Material Graph Operations](../Editor/Architecture/MaterialGraphOperations.md)
