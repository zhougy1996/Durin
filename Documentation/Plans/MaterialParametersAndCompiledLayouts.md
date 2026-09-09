# Material Parameters and Compiled Layouts Plan

Summary: Replace fixed PBR input identities and bindings with material-owned parameters and compiled layouts across authoring, instances, rendering, migration, and Cook.

Last reviewed: 2026-09-09

Status: Active
Completed:

## Current Status

Execution started on 2026-09-09 as M10 of the
[Material System roadmap](../Roadmaps/MaterialSystem.md), from clean commit
`ccc55ddf472192f82d3be0a0a3d935d38b8fde5f`. This task is the source/build writer
in Durin-worker; the other active Durin task uses Durin-architect.
The baseline material domain builds and passes its registered routine selection.
On 2026-09-09 the user explicitly deferred environment-dependent qualification
and instructed implementation to continue. Stage 0 measurement gates therefore
remain open but do not block implementation; they still block final qualification.
No GPU timing baseline or image-parity qualification is claimed.

Stage 1 declaration foundations are implemented: bounded material-owned
declarations, Float4 persistence/editing, atomic definition-plus-graph changes,
identity-safe create/reuse/rename/delete, transactional structured replacement,
type-safe override resolution and orphan retention. Declaration CRUD widgets,
constant promotion and clipboard v3 now have shared commands and tests. Legacy surface-output promotion/texture helpers still need
explicit-expression migration, and the complete Stage 1 acceptance audit remains
open. Stage 2 now has deterministic typed layouts, custom numeric/texture
source generation, target resource limits and layout-aware reflection validation.
Stage 3 has initial accepted-layout payload population and common Forward,
GBuffer and masked-shadow binding. Explicit UV channel selection, sine/cosine,
Make Surface, and texture sampling controls now use ordinary graph/value data.
Extended admission validation is implemented; instance static permutations,
the full accepted-schema audit and the rust fixture remain open.
Stage 4 now serializes and validates custom compiled layouts in DMAT v4, with
package Cook/load tests for custom values and instance sampling overrides.
Authored migration, Cook execution without compiler/source access and Stage 5
qualification remain open. The existing constructor seed and fixed-v3 renderer are
temporary baseline adapters assigned to Stage 4 removal.
The declaration API follow-up replaces string out-parameters with typed errors,
parameter identities and complete graph diagnostics; this refines Stage 1
foundations without completing its remaining authoring workflows. Existing
program mutation and compiler snapshot APIs also return their validation result
directly, following the user-requested single-result convention.

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

#### Execution decisions and baseline

Audited on 2026-09-09:

- Engine role consumers are `Material`, `MaterialInterface`,
  `MaterialParameterTypes`, `MaterialProgramTypes`, `MaterialProgramGenerator`,
  `MaterialRenderTypes`, `MaterialRenderRepresentation`, `MaterialRenderProxy`
  and `MaterialCookedProgram`. Editor commands in `MaterialGraphOperations`
  and `AssetForgeBuiltins/ImportedSurfaceMaterial.cpp` also depend on them.
  Renderer reaches the fixed binding through `MaterialBindingResolution` and
  `FMaterialRenderBinding`; removing just the declaration validator is insufficient.
- Consume the existing common geometry submission/factory path. The geometry
  plan has implemented Stages 1-3; its performance qualification is still open.
  RHI creation remains synchronous through the current facade; its refactor
  has no implementation yet. Do not introduce speculative readiness APIs.
- DMAT already uses `FArchive`, `SerializeBoundedString`,
  `SerializeBoundedSequence` and `SerializeByteBuffer`, with an 8 MiB payload
  bound and 16-byte BulkData alignment. On 2026-09-09 the completed
  Payload Archive Serialization Refactor and the current bidirectional Archive /
  declared-extent interfaces were reconciled. DMAT continues through the existing
  Archive and BulkData owners; no parallel payload adapter was introduced.

Selected implementation contracts (implementation status is recorded above):

- Retain the existing serialized Scalar/Vector/Texture/Vector2 enum values;
  append Vector4. Definitions have unique nonzero GUIDs and unique non-None
  FNames, bounded text, valid metadata and finite active numeric defaults.
  Bound authored definitions to 128, including unreachable declarations.
- Compile in GUID order. Give each numeric declaration one zero-padded,
  16-byte uniform slot, independent of vector width; use typed field metadata
  to distinguish Float2 from Float4. Keep the existing 16 KiB payload ceiling
  and 256-field structural bound. The compiler must additionally check the
  target descriptor budget including lighting, environment and shadows;
  the current 64-resource CPU bound is not evidence of device support.
  The current conservative target policy reserves four sampled images, two
  samplers and three uniform buffers for external resources, plus the material
  buffer. It permits at most 12 material textures under the 16-image policy.
  Vulkan now publishes descriptor/uniform limits through `FRHICapabilities`;
  compilation clamps this policy to the active device. Device-budget
  qualification and the full Stage 0 interface freeze remain open.
- Keep sampler min/mag/address enums as bounded typed data, associated with
  texture declarations and instance overrides. Sampler-only changes update
  resources without changing shader code. Sample nodes consume an explicit
  texture and Float2 UV; fallback policy is explicit white, black or flat-RG
  normal data, never inferred from names. Changing shader-affecting decode or
  sampling mode changes executable identity; ordinary sampler selection does not.
  Migrate each old packed sampler override to its texture GUID, retaining the
  old scalar declaration/value as unreachable authored data for inspection.
- Plan version transitions: authored program 3 to 4 (accept supported 2 via
  its existing upgrade), clipboard 2 to 3, DMAT 3 to 4, IR/generator 2 to 3,
  compiler envelope 4 to 5, pass contract 1 to 2 and render layout 3 to 4.
  Presentation remains version 2. These numbers must be rechecked against
  intervening commits before implementation changes the corresponding format.
  Stage 1 adds an independent declaration schema discriminator: missing/1
  retains legacy canonical validation; 2 enables material-owned declarations.
  Graph/clipboard/DMAT versions remain unchanged until their implementation
  stages. This prevents custom declarations from silently weakening validation
  of malformed legacy packages.
- Clipboard v3 carries source-root identity and referenced definitions.
  Same-root paste reuses GUID/type; foreign paste generates local GUIDs or
  reuses a same-name/type declaration only after default/metadata comparison.
  Conflicts reject the whole paste with a declaration diagnostic. Remap every
  reference before committing one definition-plus-graph transaction.
- An accepted generation retains its resolution schema and counted resources.
  Deletion/type replacement leaves its prior values available to old code;
  GUID/type-compatible edits update it, while new-only inputs wait. Parent
  switching rebuilds resolution from the new root; override eligibility uses
  GUID and type, never names. Static shader
  overrides require an existing-scheduler permutation with the effective
  properties; pending/failed results retain the whole previous generation.
- Binding failure selects a complete independent error generation, never a
  partially populated payload. Migration expands StandardSurface atomically
  using ordinary expressions and explicit surface construction. Preflight
  node/link/depth bounds; if expansion cannot fit, preserve the original data
  and report failure without saving or silently discarding unreachable nodes.

Baseline receipt: `.\DevTool.bat test "@material"` resolved MaterialTests,
MaterialThumbnailTests and MaterialVulkanTests. Build succeeded in 73.21 s;
routine CTest selection succeeded in 22.02 s. Logs:
`Build/.agent-state/logs/20260909-154812-754187-32048-cmake.log` and
`Build/.agent-state/logs/20260909-154925-962307-32048-ctest.log`.
Host profile is windows-msvc-x64 / Win64-Debug-DurinEditor; detected GPU is
NVIDIA GeForce GTX 1060 6GB, driver 582.66. Durations are diagnostic only.

Qualification preparation still required: capture reproducible pre-change and
post-change color/GBuffer/depth/masked-shadow images, pin scene inputs, establish
numeric tolerances and freeze CPU/upload/allocation/retention/GPU budgets.
Existing thumbnail difference tests do not provide that complete baseline.
Preserve the source baseline above for a later isolated comparison checkout.
Keep current compiler limits (64 requests, 256 consumers, 128 resident programs,
2 MiB request and 8 MiB result) and include new layout/schema storage in accounting.

### Stage 0: Freeze interfaces and qualification

Dependencies: exclusive source writer acquired and current related interfaces
reconciled. Outcome: selected wire/layout decisions and reproducible baseline.

- [x] Record baseline commit and dirty-state disposition; inventory Engine,
  Renderer, MaterialEditor and importer role-table consumers, current geometry
  pass interfaces, RHI readiness and payload serialization contracts.
- [ ] Freeze declaration/layout/pass records, alignment and numeric/resource
  bounds against actual target capabilities. Select explicit sampler and fallback
  representation, including sampler-only invalidation behavior and preservation
  of existing per-instance packed sampler overrides.
- [x] Specify authored/clipboard/DMAT transitions and cross-root paste remapping.
  Same-root paste reuses definitions; foreign declarations use validated remaps
  with name/type/default conflict diagnostics, not unexamined foreign GUIDs.
- [x] Freeze accepted-schema behavior during deletion/type replacement/parent
  edits, static-instance permutations, binding failures and template expansion
  near graph size limits. Resolve blocking design choices before Stage 1.
- [ ] Select registered CPU/GPU tests; capture fixed scene inputs and baseline
  images. Record numeric image tolerances, CPU/upload/allocation/retention/GPU
  budgets and sampling conditions before changing rendering behavior.

Completion: concrete decisions, schema versions, bounds and baseline receipts
are recorded here. This draft does not count as passing the baseline gate.

### Stage 1: Add declarations and identity-safe editing

Dependencies: Stage 0. Outcome: custom declarations persist and resolve.

- [x] Implement bounded declarations and atomic definition-plus-graph mutations;
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

Stage 1 foundation receipt (2026-09-09): MaterialTests passes 118 tests after
adding declaration conflicts/no-ops, atomic reference deletion, override
inheritance/orphan restoration, Float4 save/reload/duplication, one-transaction
Undo/Redo and old accepted values during failed custom compilation. The
synchronous read path now consumes the same local layers as RenderThread.
The final affected selection passed 82/82 targets in 130.49 s after the
type-mismatched PostLoad orphan regression was added:
`Build/.agent-state/logs/20260909-160810-196431-32476-ctest.log`.
Full `all` build succeeded:
`Build/.agent-state/logs/20260909-160746-331070-32844-cmake.log`.
Changed-document, all-plan and all-roadmap validation passed. The old-code regression
fixture deliberately relies on Stage 1's unsupported custom generator; replace
that trigger with explicit compiler failure injection when Stage 2 lands.

Stage 1 result-API follow-up receipt (2026-09-09): declaration validation and
editing now return typed results without string out-parameters. Program mutation
and compiler snapshot validation return their result directly. Updated tests
assert typed conflicts, parameter identities, full graph diagnostics, no-op
revisions and failure atomicity. Final affected selection passed 82/82 targets:
`Build/.agent-state/logs/20260909-170052-041542-21636-ctest.log`.
Full `all` build passed:
`Build/.agent-state/logs/20260909-170316-737512-16536-cmake.log`.
Changed-document validation passed. Qualification remains deferred as above.

Stage 1 authoring follow-up (2026-09-09): root parameter CRUD widgets and
shared commands, Float4 constant promotion, unreachable default editing and
clipboard v3 remapping are implemented. Clipboard textures have counted strong
retention and the source-root identity is weak and generation-safe. Foreign
external node links are rejected even on coincident GUIDs. Foreign legacy
StandardSurface/TextureCoordinate paste is deliberately rejected until Stage 4
expands hidden role dependencies; same-root paste remains supported. This is a
temporary adapter, not custom GPU support. Focused MaterialTests passed 122/122,
including source collection, retained texture defaults, atomic history, conflict
rejection and stale same-root declarations. The subsequent affected selection
passed 81/81 targets (including the unreachable-default panel regression):
`Build/.agent-state/logs/20260909-173747-262940-29432-ctest.log`.
Full `all` build passed:
`Build/.agent-state/logs/20260909-174009-922390-26736-cmake.log`.
Changed-document validation passed. No GPU performance qualification is claimed.

### Stage 2: Compile layouts and complete results

Dependencies: Stage 1. Outcome: reachable custom inputs generate validated code
and bindings independent of their names or PBR role.

- [x] Normalize reachable declarations and explicit texture/UV/sampler inputs;
  generate deterministic layouts/source, including Float4 and resource-free graphs.
- [x] Validate reflected forward, GBuffer and masked-shadow bindings against the
  layout; support optimized-out bindings and resource-free opaque shadows. Reject
  overlapping, duplicate, wrong-type and oversized layouts.
- [ ] Carry layout compatibility and immutable schema through compiler results,
  shared requests, cancellation, stale admission and bounded caches.
- [ ] Verify value/name/presentation stability versus graph/type/layout identity
  changes. Inject invalid reflection, failed compilation and stale completion
  while an older accepted layout remains visible.

Implementation checkpoint (2026-09-09): compiled layout v4 sorts GUIDs, uses
16-byte numeric slots after one reserved view-control slot and assigns texture /
sampler pairs from binding 32. IR/generator/envelope/pass versions are 3/3/5/2.
Typed validation rejects invalid identities, fields, resource budgets and
reflection; tests include all numeric widths, custom texture sampling, empty
resources and malformed reflection/IR. The former unsupported-custom failure
fixture now injects a failed completion explicitly. Accepted payloads carry
counted texture references, sampler state and explicit fallback data. Common
render binding is connected while the legacy-v3 adapter remains for Stage 4.
Admission and shader-map creation now validate complete shader sets and active
layout agreement; legacy DMAT decoding reconstructs its fixed layout explicitly.
The broader schema/static-permutation acceptance audit and Stage 3 qualification
remain open.
Validation receipt: MaterialTests passes 125 tests
(`Build/.agent-state/logs/20260909-184356-096420-30140-MaterialTests.log`);
`test affected` passes 82/82 routine targets including material Vulkan, common
geometry preparation and Vulkan RHI integration
(`Build/.agent-state/logs/20260909-184535-376521-20828-ctest.log`). Full `all`
build passes (`Build/.agent-state/logs/20260909-184809-876257-31748-cmake.log`).
Earlier binding failures were fixed; an intermediate independent Vulkan RHI
crash did not reproduce in the final selection. Timings remain diagnostic.


Explicit-input follow-up (2026-09-09): program schema 4 appends UVChannel,
Sine, Cosine and MakeSurface without reordering old opcode values. UVChannel
rounds/clamps an explicit scalar to mesh channels 0-3; MakeSurface consumes the
eight typed surface properties. Catalog creation supplies ordinary defaults.
Supported schema 2/3 graphs upgrade structurally; hidden-role expansion still
belongs to Stage 4. Texture values preserve sampler/fallback data through root
and instance assignment, editor edits, Undo/Redo and texture replacement. Invalid
sampling enums are rejected. Sampling changes leave compiled program identity
unchanged; the legacy packed sampler migration is still outstanding.
Validation: MaterialTests passes 127 tests; `test affected` passes 82/82 routine
targets (`Build/.agent-state/logs/20260909-185928-555104-4132-ctest.log`), including
runtime sampling inheritance and editor Undo/Redo sampling coverage. Full `all` build passes
(`Build/.agent-state/logs/20260909-190212-832848-23960-cmake.log`).

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
- [x] Implement DMAT layout/code serialization and strict validation through
  current archive/BulkData APIs. Reject corrupt, trailing, incompatible and old
  cooked payloads; require current successful results for Cook.
- [ ] Verify migrated base/instance chains, orphan overrides and save/reload;
  compare baseline images including masked shadows and translucent behavior.
- [ ] Render new and migrated cooked content with ShaderBuild and authored-source
  access unavailable; compare authored/cooked results.
- [ ] Remove temporary adapters, exact-v3 production validators and hidden role
  lookup from general compilation/rendering. Retain only bounded authored-format
  migration knowledge needed to load supported older packages.

Cook format checkpoint (2026-09-09): DMAT v4 stores layout identity, field types,
offsets, compact indices and counts alongside the active declaration contract and
complete compiled stage set. A trailing 128-bit checksum covers the payload;
layout validation, shader hashes, reflected dynamic-buffer names, compiler/target
checks and archive extent checks all precede publication. Old DMAT requires
recooking. Source and IR remain excluded. Package Cook/load coverage verifies
custom Float4 and texture layouts, graph stripping and instance sampler/fallback
overrides; failed decode leaves the caller's previous program intact. This does
not qualify compiler-unavailable Game execution or migrated-content image parity.
Validation: MaterialTests passes 128 tests
(`Build/.agent-state/logs/20260909-190949-930747-29900-MaterialTests.log`);
`test affected` passes 82/82 routine targets
(`Build/.agent-state/logs/20260909-191118-649495-24780-ctest.log`), and full `all`
build passes (`Build/.agent-state/logs/20260909-191428-035747-27232-cmake.log`).

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
`Documentation/Plans/PayloadArchiveSerializationRefactor.md` (currently present
only in the coordinating checkout).
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
