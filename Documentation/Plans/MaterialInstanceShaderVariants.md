# Material Instance Shader Variants Plan

Summary: Let one base material graph produce inherited instance render configurations with shared compiled variants, atomic publication, and cooked runtime support.

Last reviewed: 2026-09-11

Status: Active
Completed:

## Current Status

The 2026-09-11 prerequisite refactor changes current compilation failure and
admission rejection to retire the owner's accepted renderable generation and
publish ErrorMaterial. Pending work retains a valid prior generation. This
updates the shared lifecycle contract without completing a stage of this plan.

Stages 0–2 are committed. Implementation of complete accepted instance
generations, shared DMAT v5 instance payloads, editor controls and import readiness
is present; final regression qualification is in progress. The plan remains
Active until the remaining qualification and scene reimport scope gates below
are resolved.

Execution resumed on 2026-09-10 at the user's request. Stage 2 commit is
`0cc812231`; its 140-test receipt is recorded below. Final-stage work continues
on 2026-09-11. Stage 3 and Stage 4 code are validated together to preserve the
existing cooked-instance tests while replacing parent-only publication.

Implementation findings: multi-asset Cook reachability seeded only the first
export and pruned independent top-level variants. All top-level exports now seed
reachability; nested graph-only subobjects remain subject to pruning. The new
nested-variant package roundtrip covers this seam. Material recipe version 3
invalidates old incremental DMAT output. Existing transient runtime compatibility
adopts precompiled parent code only; it does not implement M8.

The current Scene importer is creation-only under its owning architecture; there
is no supported reimport/reconciliation path to qualify. The requested scene
reimport-reuse gate remains unresolved rather than silently adding a second
import lifecycle. Win64 Game execution also needs a Windows lane; the local
macOS editor tests can verify Win64 payload metadata/codec and graph-stripped
loads but cannot supply native Win64 Game execution evidence.

## Goal

One `DMaterial` owns the graph and parameter declarations. Asset instances can
inherit from materials or other instances, override individual rendering
properties, and resolve matching compiled programs. Equal effective compiler
inputs share work and immutable results regardless of instance asset identity.
Dynamic parameter edits do not compile. Cooked runtime needs no graph or compiler.

For example, brick and metal instances share the opaque variant, foliage uses a
masked variant, and glass uses a translucent variant of the same compatible
surface graph. This does not promise that every graph/shading configuration is
valid; unsupported combinations retain explicit validation diagnostics.

## Scope and Non-Goals

- Include the existing blend mode, shading model, mask threshold, two-sided and
  depth-write properties; nested inheritance; authored migration; editor/import
  integration; compile lifecycle; render publication; and Win64 Game Cook/load.
- Preserve the current supported graph, geometry, shading and pass capabilities.
  Do not add static-switch nodes, material layers/functions, arbitrary shader
  code, new shading models, or new transparency algorithms.
- Do not add a second scheduler, persistent material artifact cache, or a global
  cooked shader library. Reuse current asset compilation, ShaderBuild DDC, BulkData
  and renderer boundaries.
- Runtime transient instance creation remains the roadmap's M8 work. This plan
  freezes its boundary: runtime dynamic edits cannot request shader compilation;
  configuration changes use precompiled assets/variants.
- Mask-threshold dynamic parameterization is deferred. Preserve its compiled
  behavior for Masked materials; record variant counts so later work is evidence
  based. Do not generate an asset for every threshold.

## Selected Design

### Authored ownership and effective configuration

The root material retains the single graph and declaration scope. Instances store
independent override flags and values, not a full inherited snapshot. Separate
shader configuration from pipeline state in the resolved contract:

| Property | Initial classification | Identity policy |
| --- | --- | --- |
| BlendMode | Shader and pass configuration | Included |
| ShadingModel | Shader configuration | Included |
| OpacityMaskThreshold | Masked shader configuration | Included for Masked; canonicalized to one value otherwise |
| bTwoSided | Current culling/pipeline semantics | Excluded from shader identity while no generated code depends on it |
| DepthWritePolicy | Pipeline state | Excluded from shader identity |

Stage 0 audits every production pass before locking this classification. If
two-sided behavior also changes generated normals/code, include that behavior in
shader configuration explicitly; do not silently expand semantics to match UE.
Use one canonicalization function for validation, compile identity, compatibility
and Cook. Preserve authored inactive cutoff values so switching to Masked restores
user intent; only the effective compiler input is canonicalized.

Resolve the full parent chain on the GameThread with bounded cycle/depth checks.
Parent graph/declaration edits, inherited property changes and reparenting affect
the request dependency revision. Explicit overrides equal to the parent remain
authored overrides but share the same effective variant. Clearing a flag resumes
inheritance. Dynamic values and override flags themselves are not shader keys.

### Shared compilation, per-owner state

Extract a common material compilation owner/state boundary usable by both asset
types. Keep the synchronous compiler consuming detached values. Reuse the existing
material domain and single-flight mechanism; update finish/cancel/reload/offline
preparation and owner discovery rather than adding an instance-only path.

Build compiler input from the root graph/declarations and the instance's effective
shader configuration. A child need not wait for the parent's default variant to
compile: it can request its own effective configuration from the authored graph.
An exact compatible accepted/retained result may be adopted without compilation.
Cache identity remains the normalized compiler input plus dependencies, target,
compiler and pass/version envelope; object path, parent-chain shape and dynamic
values are excluded. Owner generation/dependency tokens remain separate from the
shared key and must still be checked on cache hits and mailbox admission.

Use the existing material dependency invalidation infrastructure. Do not add
strong reverse owner collections or Worker-side object traversal. Reparent,
Undo/Redo, root edits, reload, deletion and unload invalidate the correct consumers;
canceling one consumer must not cancel work still needed by another. Retain the
existing 64-flight/256-consumer and result/cache byte limits unless measured
evidence justifies a documented change. Define bounded deferred admission/retry so
a large instance fan-out cannot silently remain stale after capacity is released.

### Accepted rendering generation

Publish a compatible program, active parameter contract/layout, shader properties,
resolved values/resources and pass state as one accepted generation. Keep authored
intent, pending request and accepted state distinct. Pending changes retain a
valid complete prior generation; current compilation failure or admission
rejection retires it to ErrorMaterial. Cook must
reject stale/error results. Deletion or broken parent state cannot retain dangling
references and must expose an explicit diagnostic.

A variant must resolve inherited values by stable parameter ID/type against its
own accepted layout. Do not copy packed parent bytes, assume parent and child
active layouts are identical, or inherit the parent's ErrorMaterial as proof that
an independently valid child variant is invalid. Preserve dormant overrides.
Compatible dynamic edits may update the accepted layout while compilation is
pending; newly typed/added parameters cannot leak into an old generation. Root
declaration removal/retype and reparenting must retain any old resources required
by last-known-good state until replacement or retirement.

Pipeline-only edits reuse code and are validated against the accepted shader
configuration. During a pending shader change, derive automatic depth/pass state
from the accepted configuration. Any incompatible combined edit waits for the
new generation. Renderer remains a consumer of immutable values and shared
program handles; no material-object lookup moves onto the render thread.

### Persistence and compatibility

Authored packages save parent references, per-field override intent and dynamic
values. Migrate a legacy enabled full override to all corresponding flags enabled,
even where values equal the parent: that preserves its previous snapshot intent.
A disabled legacy override becomes all flags disabled. Invalid legacy data gets
bounded diagnostics under the package migration rules; never silently drop valid
shader differences. Retire the broad setter and old editor fields after callers
and migration readers are converted.

Each cooked material or instance carries its own effective variant payload through
a shared codec and existing package BulkData machinery. Keep parent dependencies
needed for parameter inheritance. Do not mutate a root package to append variants
discovered from children. This deliberately permits repeated payload bytes across
packages in the first implementation; runtime/in-process compiler result reuse is
separate from cross-package storage deduplication.

Extend the versioned cooked envelope to distinguish effective shader contract
from pipeline metadata and validate the instance's effective configuration and
parameter contract before publication. Stage 0 records the exact authored schema
and DMAT version transitions. Old incompatible cooked outputs require recook;
runtime does not repair them by compiling. No cooked result may depend on an
unloaded editor-only graph or a warm DDC.

## Implementation Stages

### Stage 0: Freeze contracts and qualification fixtures

- [x] Audit compile ownership, dependency invalidation, pass semantics, reflected
  editing, archive migration and offline preparation seams in the affected code.
- [x] Record concrete common owner/state types, effective-property resolver API,
  snapshot/admission tokens, deferred admission policy and payload schema/version.
- [x] Define fixtures from one graph: opaque, masked with two cutoffs, translucent,
  pipeline-only differences, equal explicit overrides, and nested/reparented instances.
- [x] Discover registered native targets through DevTool; record exact CPU, Cook,
  editor and Vulkan selections plus baseline counter observations for these fixtures.

Exit: contracts have one selected implementation each, test fixtures are mapped to
registered targets, and no schema/ownership decision remains implicit. Baselines
are current reproducible counter measurements, not unavailable historical images.

### Stage 0 contract decisions (2026-09-10)

The following names describe the selected implementation, not APIs already
available in the current source.

#### Audited seams

- `MaterialCompileLifecycle.cpp` owns the existing manager, detached requests,
  retained results and single flights. Submit/admit, completion pumping,
  selected finish/cancel and shader-reload discovery all require `DMaterial`.
  `AssetCompilingManager.cpp` also registers only `DMaterial::StaticClass()`.
  Generalizing only the submit signature would leave instances invisible to
  aggregate finish, cancellation and shutdown.
- `MaterialInterface.cpp` provides `GetLoadedMaterialDependents` and
  `GetLoadedDirectMaterialChildren` through GameThread live-object snapshots.
  `MarkRenderDataDirty` currently publishes only the current owner: it is not
  a compile invalidation mechanism. Reuse these queries for authored dependency
  fan-out; do not trigger child requests from acceptance publications.
- `MaterialRenderProxy.cpp` copies parent render data, then overlays local
  parameters and properties. It treats a parent error representation as a child
  failure and cannot populate inherited values newly active in a child layout.
  Both this path and `DMaterialInterface::GetRenderData` need the same complete
  generation contract.
- Generated forward, geometry and shadow entry points are built together in
  `MaterialProgramGenerator.cpp`. Blend, shading and cutoff become macros;
  neither culling nor depth policy becomes shader code. `SurfaceMaterial.slang`
  always applies `SV_IsFrontFace` normal orientation, independent of two-sided
  intent. Keep two-sided state pipeline-only; no new normal semantics are needed.
- Shared `StaticMeshRenderPreparation.cpp` selects culling and depth state for
  geometry factories, excludes translucent shadow draws and applies straight
  alpha blending. Automatic depth writing is false for translucent draws.
  GBuffer eligibility stays with the existing factory/pass contract.
  `MeshRendererShared.h` also hashes cutoff in renderer keys and must receive
  canonical accepted shader properties.
- `MaterialCook.cpp` owns only base-material BulkData/admission. The material
  Cook contributor and its ShaderBuild dependency declaration in
  `EngineCookContributors.cpp` are base-only. `PackageReload.cpp` admits both
  classes for private preparation but checks compile failure only on bases.
  Offline preparation invokes PostLoad before aggregate finish and must use the
  common owner path without requiring live published handles.
- `MaterialInstance.cpp` only validates Parent proposals; reflected static edits
  have no equivalent semantic path. `SceneDirectImport.cpp` copies a complete
  parent snapshot and ignores dynamic setter results. Material Editor compile
  status is read from a base material. Each caller must use instance owner state.

#### Selected owner, resolution and admission contracts

- `DMaterialInterface` becomes the compilation owner for both asset classes.
  It contains a non-reflected `FMaterialCompilationOwnerState` with status,
  diagnostics, request/dependency revisions, accepted generation and deferred
  admission intent. `DMaterial` alone retains the authored graph/declarations.
  `FMaterialCompilationLifecycle` consumes the common owner; there is one
  `Durin.Material` domain and one retained-result store.
- `FMaterialPropertyOverrides` stores five independent reflected enable flags
  and a `FMaterialStaticProperties` value. `SetPropertyOverrides` validates and
  applies the complete edit atomically; clearing one flag preserves its value.
  `ResolveMaterialProperties` returns a value-owned
  `FResolvedMaterialProperties`: root handle, authored effective properties,
  canonical shader properties and five supplying-owner handles for editor labels.
  Resolution is GameThread-only, iterative, and limited to 64 material owners.
  Repeated handles, missing roots and depth overflow return bounded diagnostics.
- `CanonicalizeMaterialShaderProperties` sets non-Masked cutoff to `0.333f`,
  normalizes Masked negative zero to positive zero, and clears culling/depth to
  their defaults in the compiler projection. Validate authored enum/float values
  before projection. Identity, request snapshots, compatibility, generated
  macros, renderer shader keys and Cook validation consume this projection.
  Stored inactive cutoff and pipeline values are never overwritten by it.
- Snapshot/admission retain the current handle generation, authored revision,
  request generation, exact identity and target checks. Add a separate
  `ParentChainRevision` token to requests/results and owner state; keep shader
  reload generation as `DependencyRevision`. Root semantic edits, inherited
  configuration edits and reparenting advance affected owners before requesting
  replacements. Dynamic edits do not advance shader revisions. Worker code
  consumes no object pointers or parent chains. Cache hits follow admission too.
- Capacity rejection becomes explicit deferred intent only for transient
  flight/consumer capacity. Keep 64 flights, 256 consumers, 128 retained programs,
  2 MiB/request, 8 MiB/result and 256 MiB total retained results. Store one retry
  bit and force intent per owner, without retaining detached requests or strong
  references. A fair GameThread live-owner scan selects at most 256 deferred
  owners per pump and rotates its handle cursor; rebuild each snapshot on retry.
  Selected finish/all finish include deferred owners, pump capacity, and continue
  admission until terminal. Cancel/delete clears intent, shutdown stops retries,
  and invalid/oversized requests remain terminal with diagnostics.
- `FMaterialLocalRenderLayer` owns the immutable compiler result, accepted static
  properties and complete parameter values
  as `FMaterialLocalRenderParameter` entries. Build values on GameThread by stable
  ID/type against that result's active layout, including native texture references.
  Admission prepares the complete candidate before swapping. Removed/retyped
  authored definitions retain old native values/resources until retirement.
  Compatible dynamic edits refresh this contract and dependent publications.
- Both render-data entry points consume a complete accepted layer; instances do
  not copy a parent's packed representation or depend on its compile success.
  Pipeline-only edits are admitted against accepted shader properties. Combined
  shader/pipeline changes wait together; automatic depth/pass routing is derived
  from the accepted blend mode. Missing roots retire invalid state explicitly.

#### Selected schema transitions

- Keep DAST v9 and base declaration schema 2. Add the reflected
  `PropertyOverrides` field to instances. Retain
  `bOverrideStaticProperties_DEPRECATED` and
  `StaticPropertiesOverride_DEPRECATED` with their original logical types and
  declaring type for the repository's deprecated-field routes. Convert in
  PostLoad only when loaded deprecated-property evidence is present; this avoids
  repeating migration during duplication or subsequent PostLoad calls. Enabled
  legacy snapshots enable all five flags, including equal values; disabled
  snapshots enable none. Invalid legacy snapshots emit bounded diagnostics.
  Normal save emits only current fields; no DAST custom version is required for
  this tagged-field shape change.
- Advance material program identity schema 2 to 3 and compiler envelope 5 to 6
  when canonical compiler properties land. IR 3, generator 4 and pass contract 2
  remain unchanged because their code/layout semantics are unchanged.
- Advance DMAT payload schema 4 to 5 in Stage 4. Its bounded envelope explicitly
  serializes canonical shader properties and pipeline metadata separately,
  followed by the existing immutable runtime program, layout and active contract.
  Encode validates the accepted contract; decode checks exact version, target,
  checksum, canonical properties and parameter ID/type contract before swapping.
  Old DMAT outputs require recook; no compatibility compiler is added.
- Move cooked `ProgramData` ownership to `Durin::DMaterialInterface` and share
  the codec/admission implementation. Every instance has its own BulkData field;
  retain parent package dependencies for dynamic values. Register both families
  with ShaderBuild recipe inputs. Repeated package bytes are intentional and
  measured independently from retained in-process result sharing.

#### Qualification fixture and registered lanes

Use one PBR graph with stable parameter IDs and these effective configurations:
opaque default, opaque with inactive cutoff `0.25`, Masked `0.25`, Masked `0.75`,
translucent, opaque two-sided/depth-disabled, explicit-equal opaque and an
inheriting root/child/grandchild chain. Reparent the grandchild between opaque
and Masked owners, reset its flag, then remove/retype a used parameter during a
pending replacement. Expected final unique shader identities: four. Record
legacy behavior separately before changing canonicalization.

Registry discovery on `Win64-Debug-DurinEditor` returned the following concrete
targets. Existing source suites do not yet prove the new behavior; extend them
in their owning stage.

| Gate | Registered selection | Fixture responsibility |
| --- | --- | --- |
| CPU identity, inheritance, lifecycle, codec and editor commands | `MaterialTests` | Compiler projections, shared consumers, accepted layout, migration and transactions |
| Thumbnail CPU lifecycle | `MaterialThumbnailTests` | Preview refresh and resource lifetime |
| Material Vulkan | `MaterialVulkanTests` | Rendered variants and preview/thumbnail parity |
| Import CPU and Vulkan | `SceneImportTests`, `SceneImportVulkanTests` | Alpha modes, cutoff, one graph, reimport and double-sided output |
| Cook packages and cold process | `AssetCookTests`, `StandaloneCookProcessTests` | Instance payload admission and compiler-free Game load |
| Spline CPU | `SplineTests` | Existing shared geometry submission contract |

Use `test MaterialTests "FMaterialCompileLifecycleTests.*"` for the initial bounded
baseline and `test "@domain=material"` for the material domain gate. Run the
registered Cook/import targets as a bounded positional set when their stages
land. GPU execution and cold Win64 Game loading remain mandatory final gates;
target discovery alone is not evidence of execution.

Stage 0 validation: `.\DevTool.bat test MaterialTests
"FMaterialCompileLifecycleTests.*"` passed both lifecycle/codec cases on
2026-09-10. The lifecycle case now measures the shared-graph fixture before its
existing single-flight/supersession/shutdown assertions. Receipt:
`Build/.agent-state/logs/20260910-202752-211639-3956-MaterialTests.log`.

| Measured legacy baseline | Value |
| --- | --- |
| Instance owners | 8 |
| Effective normalized identities | 5 (inactive opaque cutoff unnecessarily splits one) |
| Instances accepting parent code | 4 |
| Instance compile requests | 0 |
| Total accepted/completed requests | 1 / 1 |
| Retained programs / bytes | 1 / 165931 |
| Root DMAT bytes | 130749 |
| In-flight / consumers / publications after drain | 0 / 0 / 0 |

Instance payload storage is unavailable in the baseline; Stage 4 must measure
all fixture packages after implementing that payload. The existing lifecycle
case checks zero retained programs/bytes after shutdown. GPU and Game execution
were not performed in Stage 0.

The first baseline build failed because `EnvironmentLightingResources.cpp`
still used the removed `MakeRDGShaderResourceParameterMemberMetadata` helper.
Its two declarations now use the current semantic compute read/write helpers
with `WithRDGShaderBinding`, preserving accesses, discard and descriptor types.
The subsequent MaterialTests build passed. A first suite-name filter selected
zero cases; only the corrected wildcard execution above counts as test evidence.

During Stage 1, the user requested rebasing onto local `dev`. Rebased onto
`dddb2d69e`; the duplicate sky-light fix was resolved by retaining the upstream
`70429fefd` version. Stage 0 is now commit `284d1217f` and changes only its
qualification fixture and plan evidence. Uncommitted Stage 1 changes were
preserved through a dedicated stash and restored successfully.

### Stage 1: Introduce per-field configuration and migration

Depends on Stage 0.

- [x] Add reflected per-field shader/pipeline overrides and one effective resolver;
  distinguish authored properties from canonical compiler properties.
- [x] Migrate legacy override snapshots and update property editing validation,
  duplication, transactions, serialization and parent-chain invalidation hooks.
- [x] Route root identity construction through the same canonicalization; version
  affected identities so stale artifacts cannot be reused.
- [x] Test independent inherit/reset, same-value explicit intent, inactive cutoff,
  nested overrides, cycles and old asset round trips.

Exit: one authoritative property resolution path exists. Until Stage 3, differing
instance shader configurations still fail safely rather than use mismatched code.

Stage 1 handoff (2026-09-10):

- Added `FMaterialPropertyOverrides`, `SetPropertyOverrides`, the 64-owner
  `ResolveMaterialProperties` resolver and supplying-owner handles. Reflected
  edit validation, committed parent edits, Undo/Redo and duplication preserve
  independent flags. Instance proxy layers carry sparse flags so subsequent
  parent changes still propagate inherited pipeline fields.
- Authored DAST stays at v9. Real serialized package fixtures exercise both
  enabled and disabled legacy snapshots through deprecated-field loading,
  canonical resave and explicit-equal override persistence. Repeated PostLoad
  does not replay the migration. Tests also cover inactive cutoff preservation,
  reparent/reset, source labels, 64-owner limits and corrupt cycles.
- Compiler identity schema is now 3 and compiler envelope 6. Identity generation,
  cutoff macros, root setter/reflected edit decisions and parent compatibility
  use the common canonicalization. Root cooked metadata compares its canonical
  renderable properties and serializes current pipeline state. DMAT remains 4;
  the shared instance payload/schema-5 transition belongs to Stage 4.
- `.\DevTool.bat test MaterialTests` passed 139 cases in 17 suites, including
  real render-thread rejection of incompatible parent code and reflected
  override transactions. Receipt:
  `Build/.agent-state/logs/20260910-204549-416299-1092-MaterialTests.log`.
  The updated fixture measures four effective identities, five compatible
  parent-code instances and zero instance compile requests. Independent
  compilation is deliberately absent until Stage 2.
- `.\DevTool.bat build` completed target `all` for
  `Win64-Debug-DurinEditor`. Receipt:
  `Build/.agent-state/logs/20260910-204712-654075-27708-cmake.log`.
  Changed-document and all-plan validation passed. The bounded Stage 1
  `MaterialTests` selection supplies runtime validation; the module-wide
  `test affected --explain` expands to unrelated Engine domains and was not run.
  GPU and cold Game instance-load gates remain outstanding for later stages.
- Lasting implemented property/migration rules are documented in
  [Material System](../Runtime/Rendering/MaterialSystem.md). The broad wrapper,
  base-only compile state and parent-layout inheritance remain explicit future
  work. The user supplied the continuation on 2026-09-10; Stage 2 now proceeds.

### Stage 2: Generalize compilation ownership and variant reuse

Depends on Stage 1.

- [x] Refactor base-only lifecycle APIs/state and aggregate filters to accept both
  material asset kinds, preserving the base-material behavior.
- [x] Snapshot effective instance inputs and implement exact-key reuse, per-owner
  status/admission, dependency fan-out, cancellation, reload and bounded retry.
- [x] Include bootstrap/tooling and offline preparation in the same owner contract.
- [x] Test identical-input single-flight/cache reuse, masked cutoff differences,
  zero compilation for dynamic/pipeline edits, supersession, capacity recovery,
  parent deletion/reparenting and shutdown without retained object ownership.

Exit: instances obtain matching results with asset-qualified status; equal inputs
share compiler work while generations remain independently validated. Results are
not exposed to production rendering until Stage 3's publication boundary is ready.

Stage 2 validation: `./DevTool test MaterialTests` passed 140/140 tests on
2026-09-10; receipt `Build/.agent-state/logs/20260910-234312-661178-48221-MaterialTests.log`.
The expanded lifecycle fixture covers common owner status, selected finish,
retained reuse, distinct Masked identity, nested and reparented owners, inactive
root cutoff changes affecting Masked descendants, zero dynamic/pipeline requests,
and 264 consumers with deferred recovery and cancellation. Existing lifecycle
coverage retains single-flight sharing, stale admission and shutdown checks.
The shared eight-instance fixture now measures four effective identities, four
retained programs / 670644 bytes, and zero consumers/flights after drain. The
root payload remains 130749 bytes; instance payload measurement awaits Stage 4.
Production instance rendering intentionally remains behind Stage 3.

### Stage 3: Publish complete instance rendering generations

Depends on Stage 2.

- [x] Replace parent-code-only acceptance with the common accepted variant state.
- [x] Resolve parameter layers against the accepted variant contract; remove packed
  parent-layout assumptions and shader-identity mutation through local static layers.
- [x] Implement last-known-good retention, compatible dynamic edits during compile,
  atomic pass configuration changes and correct error/resource retirement behavior.
- [ ] Qualify StaticMesh/SplineMesh (and the current shared geometry submission
  contract), forward/GBuffer/shadow, preview and thumbnail publication paths.

Exit: successful variants render with matching state; failed/pending/reordered
results cannot mix code, layout, values or routing. A child with a valid distinct
variant does not depend on successful compilation of the parent's default variant.

### Stage 4: Cook and load effective instance variants

Depends on Stage 3; follow the accepted shared archive/BulkData interfaces.

- [x] Generalize Cook admission and codec use to instances; serialize each effective
  payload and required parent dependencies without modifying shared root assets.
- [x] Enforce current target/dependency/request freshness and strict version,
  checksum, effective configuration and parameter-contract checks.
- [ ] Load nested instance fixtures in Win64 Game without authored graphs, shader
  sources, editor DDC or a live compiler; test corrupted/missing/wrong payloads.
- [x] Measure repeated payload storage and document the retained duplication tradeoff.

Exit: all fixture configurations survive Cook/load with matching identities and
rendering; stale/failed Cook cannot succeed by substituting last-known-good/error.

### Stage 5: Integrate import and Material Editor workflow

Depends on Stage 4, so the public workflow produces shippable assets.

- [x] Expose independent rendering override controls, inherit/reset and effective
  source labels; show instance compile status, diagnostics and last-known-good state.
- [x] Route UI, reflected edits, Undo/Redo and structured callers through shared
  mutation/compile semantics; apply related property edits as one transaction/request.
- [x] Keep one imported-surface graph; apply source alpha mode/cutoff/two-sided
  intent to instances through the new API. Remove the full-static setter usage.
- [x] Check parameter application outcomes and compilation readiness explicitly;
  integrate variant failure into the existing import candidate/commit lifecycle.
  Finish shared work in batches where synchronous import publication requires it.
- [ ] Verify save/reload, reimport reuse, texture alpha, masked threshold boundaries,
  double-sided imports, preview/thumbnail refresh and parent edits after import.

Exit: the selected one-graph workflow works through editor and scene import, has
actionable failures, and does not create base material assets per configuration.

### Stage 6: Complete regression evidence and retire obsolete contracts

Depends on Stage 5.

- [ ] Run the bounded affected CPU/Cook suites and the explicit GPU matrix below;
  record commands, results, unavailable lanes and resource counter receipts.
- [x] Remove obsolete broad override APIs, parent-only compatibility branches and
  base-only compilation assumptions; retain only required legacy migration readers.
- [x] Document implemented contracts in MaterialSystem, asset compilation and the
  relevant editor/import authority, then update the parent roadmap.
- [x] Record remaining threshold/deduplication follow-ups as deferred scope without
  claiming runtime M8 or reusable-function M11 completion.

Exit: every required gate has evidence, migration and clean runtime loading pass,
and durable documentation describes the implemented behavior. Complete this plan
only after these gates; planning approval is not implementation completion.

## Execution Evidence (2026-09-11)

The implementation now publishes complete instance generations, routes cooked
assets through shared DMAT v5 admission, removes broad override setters and
parent render-layer inheritance, and exposes transactional per-field editor
controls. Disabled controls display effective inherited values while preserving
dormant authored overrides. Scene import validates parameter application and
finishes all candidate variants before commit. Cooked mode rejects all shader
compile requests, including base construction and setter paths.

All receipts below are under `Build/.agent-state/logs/`. The host profile is
`macos-xcode-arm64`, preset `MacOS-arm64-Debug-DurinEditor`.

| Command | Result | Receipt |
| --- | --- | --- |
| `./DevTool build` | Target `all` passed, including editor integration | `20260911-001451-649038-51970-cmake.log` |
| `./DevTool test MaterialTests` | 140/140 passed | `20260911-001031-094226-51425-MaterialTests.log` |
| `./DevTool test MaterialThumbnailTests` | 8/8 passed | `20260911-000455-968106-51030-MaterialThumbnailTests.log` |
| `./DevTool test SceneImportTests` | 4/4 passed | `20260910-235655-659292-49694-SceneImportTests.log` |
| `./DevTool test AssetCookTests` | 21/21 passed | `20260911-000159-708252-50765-AssetCookTests.log` |
| `./DevTool test StandaloneCookProcessTests` | 2/2 passed, including three cross-package nested variants and incremental reuse | `20260911-001236-302109-51717-StandaloneCookProcessTests.log` |
| `./DevTool test SplineTests` | 42/42 passed | `20260911-001312-231326-51776-SplineTests.log` |
| `./DevTool test MaterialVulkanTests --mode qualification` | Passed on Apple M4 | `20260911-000524-909441-51107-ctest.log` |
| `./DevTool test SceneImportVulkanTests --mode qualification` | Passed on Apple M4 | `20260911-000727-839763-51319-ctest.log` |
| `./DevTool test StaticMeshRenderPreparationVulkanTests --mode qualification` | Passed; instance blend/cull/depth, StaticMesh/SplineMesh and multi-batch geometry | `20260911-001056-895257-51574-ctest.log` |

The eight-instance fixture measures four effective identities, eight accepted
instances, 13 instance requests / 14 total completed owner requests, four retained
programs / 670644 bytes, 1062434 total instance DMAT bytes and 130749 root DMAT
bytes. Shutdown and saturation fixtures verify drain and bounded retry. Payload
bytes deliberately repeat across packages; threshold parameterization and global
cooked deduplication remain deferred, and this work does not complete M8 or M11.

Two broader qualification selections failed and are not counted as passing:

- `./DevTool test GBufferQualificationTests --mode qualification`: the combined
  renderer fixture expected 150 volumetric-cloud timing queries but received zero
  with clouds disabled, then aborted during cleanup. Instance receipt:
  `20260911-001131-177541-51603-ctest.log`. Repeating with its original
  base-material fixture reproduced the same failure:
  `20260911-001425-030805-51919-ctest.log`.
- `./DevTool test DirectionalShadowBaselineVulkanTests --mode qualification`:
  frozen image hashes and exact motion counts differed on Apple M4. Instance
  receipt: `20260911-001156-514666-51647-ctest.log`. The original base-material
  fixture reproduced the failure (`20260911-001347-250554-51859-ctest.log`);
  all 150 reported actual/expected hash entries were identical between runs.
  The variant fixtures were restored after these control runs; no frozen
  expectations or required assertions were weakened.

Remaining completion gates: native Win64 Game execution without compiler/DDC;
resolution of the unsupported scene reimport requirement; and successful required
GPU qualification evidence. The macOS standalone process test proves standalone
Cook and editor-hosted graph-stripped loads, not a native Win64 Game launch.
The reimport scope question has been sent to the user; no scope waiver is assumed.

## Validation Gates

Follow [agent testing](../Agents/Testing.md) and
[build/run](../Agents/BuildAndRun.md). Discover targets rather than treating the
`EngineTests` source directory as a runnable executable. Source suites below are
starting points for Stage 0, not invented command names.

| Area | Required evidence |
| --- | --- |
| Identity | Same effective input shares one flight/result while retained; path/flags/dynamic values excluded; only Masked cutoff differences change identity; pipeline changes preserve identity |
| Inheritance | Three-level chains, override/reset, parent changes and reparenting; no stale completion after root/chain revisions; explicit equal overrides survive reload |
| Publication | Pending retains complete accepted configuration; current failure retires it to ErrorMaterial; child layout resolves all inherited values by ID/type; missing parent/no accepted result is explicit; compatible dynamic edits still work |
| Lifecycle | Shared consumer cancellation, deletion/unload, shader reload, stale mailbox, capacity saturation/retry and shutdown; existing bounds respected and counts return to baseline after drain |
| Cook | Cold load of opaque/masked/translucent nested assets without compiler/DDC; strict corruption/version/contract rejection; current target only; no stale fallback cook |
| Import/editor | All alpha modes, two cutoffs, two-sided state, one graph, reimport, Undo/Redo, save/reload, diagnostics, preview and thumbnails |
| GPU | StaticMesh/SplineMesh fixture parity; opaque/Masked forward and GBuffer where eligible; relevant masked shadows; translucent forward blending and ordering; accepted depth/culling during transitions |

GPU qualification is explicitly required for this renderer behavior change and
must run in a capable environment before completion. Do not require translucent
GBuffer/shadow behavior unsupported by the current pass contract. Record compile
count, unique identities, retained bytes and cooked size for the fixture set;
the gate is reuse and bounded lifetime, not an invented timing threshold.

## Related Documentation and Code

- [Material System roadmap](../Roadmaps/MaterialSystem.md)
- [Material System contract](../Runtime/Rendering/MaterialSystem.md)
- [Asset Compilation](../Runtime/Assets/AssetCompilation.md)
- [Asset Packages](../Runtime/Assets/AssetPackages.md)
- [Shader Cache](../Runtime/Rendering/ShaderCache.md)
- [Serialization](../Runtime/Core/Serialization.md)
- [Geometry Submission Refactor](GeometrySubmissionRefactor.md): consume its current
  accepted interface; this plan adds no geometry dispatch mechanism.
- `Engine/Source/Runtime/Engine/Public/Materials/MaterialInterface.h`
- `Engine/Source/Runtime/Engine/Public/Materials/MaterialInstance.h`
- `Engine/Source/Runtime/Engine/Public/Materials/MaterialCompileLifecycle.h`
- `Engine/Source/Runtime/Engine/Private/Materials/Material.cpp`
- `Engine/Source/Runtime/Engine/Private/Materials/MaterialInstance.cpp`
- `Engine/Source/Runtime/Engine/Private/Materials/MaterialCompileLifecycle.cpp`
- `Engine/Source/Runtime/Engine/Private/Materials/MaterialProgramCompiler.cpp`
- `Engine/Source/Runtime/Engine/Private/Materials/MaterialRenderProxy.cpp`
- `Engine/Source/Runtime/Engine/Private/Materials/MaterialCook.cpp`
- `Engine/Source/Runtime/Engine/Private/Materials/MaterialCookedProgram.cpp`
- `Engine/Source/Editor/MaterialEditor/Private/Widgets`
- `Engine/Source/Editor/AssetForgeBuiltins/Private/SceneDirectImport.cpp`
- `Engine/Tests/Native/EngineTests/Private/Materials`
- `Engine/Tests/Native/EngineTests/Private/Texture/SceneImportTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/Texture/SceneImportVulkanTests.cpp`
