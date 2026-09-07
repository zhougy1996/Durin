# StaticMesh Source Residency Plan

Summary: Separate canonical StaticMesh source storage from decoded recipe inputs and add explicit immutable geometry residency.

Last reviewed: 2026-09-07

Status: Archived
Completed: 2026-09-07

## Current Status

Stages 0-3 are complete. Source extraction, immutable residency, import/build/apply
migration and contracts are implemented. The default macOS Debug profile passed
all 30 affected native targets and the full `all` build. The migrated viewport
qualification target also passed. MaterialVulkanTests compiled, but GPU execution
was unavailable inside the agent sandbox; it is supplementary validation for
this CPU source-residency refactor, not a completion or dependency gate. The
source API is ready for the dependent plans. See the validation-scope decision
below for the confirmed environment cause and retained GPU coverage.

This is the foundation for
[StaticMesh authored compilation](StaticMeshAuthoredCompilation.md) and
[StaticMesh payload inspection](../../StaticMeshPayloadInspection.md).

## Goal

Give authored geometry one authority, reuse decoded input without repeated
serialization, and allow explicit release without invalidating existing readers.
Preserve rendering, collision, material reconciliation, source-independent
rebuilds, package compatibility, and metadata-only warm DDC loads.

## Selected Design

- Engine owns the source value, canonical codec, validation, content identity,
  and residency. StaticMeshBuild accepts only detached decoded geometry and
  recipe settings; it does not receive bulk storage, live assets, or DDC state.
- Retain FStaticMeshImportedData as the persistent reflected type initially,
  with the existing DStaticMesh::ImportedData declaring identity and wire fields.
  Extract declarations into a dedicated source header; separate decoded arrays
  into a non-reflected geometry value. A cosmetic persisted type rename is out
  of scope. Verify reflection compatibility before changing field visibility.
- Expose checked complete-value initialization and const metadata access.
  Validate a replacement before installing it. Failed initialization or asset
  application preserves prior source, materials, collision, and render state.
- A read handle owns immutable decoded geometry through shared ownership.
  Copies are cheap; source replacement and explicit memory release drop the
  source's reference without invalidating existing handles. New readers after
  replacement see only the new source. No borrowed source pointer may escape
  into worker input.
- Metadata validity and identity perform no payload I/O. Decode validates all
  counts, indices, channel lengths, arithmetic, archive end, and the existing
  1 GiB authored bound before publishing a resident value. Preserve underlying
  read/decode diagnostics and never cache failed partial geometry.
- Source capture belongs at the family build-input boundary. A fresh import
  creates canonical storage and seeds residency once; a warm DDC lookup uses
  only source identity; a miss acquires a decoded handle. Apply receives the
  authored replacement separately from the pure recipe product.
- Keep the current canonical geometry bytes, identity, and DDC key behavior
  unless Stage 0 demonstrates a correctness defect. Do not silently change
  hashing semantics or introduce compression, a generic mesh hierarchy, new
  LOD import capabilities, or a platform payload format in this refactor.
- Keep cooked-load generation, GPU resource revision, and collision revision.
  They qualify runtime state, not redundant authored-source generations.

## Implementation Stages

### Stage 0: Freeze consumers, compatibility, and memory evidence

- [x] Inventory standalone and Scene import/reimport, PostLoad, synchronous
  rebuild, cook, duplication, serialization, and tests using source arrays.
- [x] Verify existing bulk payload identity semantics and reflection field
  signatures; record the exact persisted fields that must remain readable.
- [x] Choose handle/acquire/release API names and synchronization rules, including
  simultaneous read, release, and source-copy behavior. Keep asset mutation on
  its owner thread and prohibit in-place edits of shared decoded geometry.
- [x] Select small and large representative fixtures and record baseline
  encode/decode counts, retained decoded bytes, and peak rebuild allocations.
  Define measurable gates before implementation; do not invent timing budgets.

Completion: recorded consumer map, compatible API design, and reproducible
baseline with no unresolved ownership or format decision.

### Stage 1: Separate source storage and decoded geometry

Depends on Stage 0.

- [x] Extract the persistent source and codec from StaticMesh.h/.cpp; introduce
  the value-only decoded geometry type and remove ambiguous Decode return state.
- [x] Encapsulate persistent fields and replace public array edits with complete
  validated initialization; migrate provider requests to decoded geometry only.
- [x] Implement immutable resident handles, lazy acquisition, explicit release,
  safe copy/replacement semantics, and deterministic failure behavior.
- [x] Add focused tests for malformed geometry, metadata-only reads, repeated
  acquisition sharing, release/reload, outstanding-handle survival, and failed
  replacement preserving the original source and identity.

Completion: source bytes have one authority, callers cannot leave public arrays
out of sync with it, and decoded readers have explicit ownership.

### Stage 2: Integrate import, build, application, and cook

Depends on Stage 1.

- [x] Migrate standalone and Scene import/reimport to initialize source once
  and seed its resident geometry; preserve provenance and save failure behavior.
- [x] Remove unconditional CaptureDecodedData from orchestration; use metadata
  for DDC lookup and acquire geometry only when the recipe actually needs it.
- [x] Migrate PostLoad, rebuild, cook, and application paths; separate authored
  replacement from recipe output without breaking material reconciliation.
- [x] Release temporary source residency at documented operation boundaries;
  retain only explicit handles and avoid deep array copies in request/result
  transfer. Do not drop unsaved authoritative bulk bytes.
- [x] Verify a warm metadata-only load with unreadable authored bulk still hits
  DDC, while a miss reports the source read failure. Verify corrupt DDC falls
  back to valid canonical geometry without reopening the physical import file.

Completion: fresh import avoids encode/decode round trips, warm loads avoid
source reads, and no asset retains an accidental second mutable geometry copy.

### Stage 3: Qualify package compatibility and publish contracts

Depends on Stage 2.

- [x] Validate existing authored fixtures, new save/load, duplicate/copy and
  applicable transaction behavior, reimport failure, material mapping, render
  and collision parity, and cook with the physical import source unavailable.
- [x] Verify cooked projection strips source and cooked loading needs neither
  source acquisition nor a build provider. Preserve runtime readiness behavior.
- [x] Compare Stage 0 fixtures: one decode per resident source between releases,
  no warm-hit source read, and release removes source-owned decoded retention
  after outstanding handles expire. Record peak allocation changes separately
  from wall-clock measurements.
- [x] Run affected native tests and required target checks under the repository
  workflows below. Record actual targets and evidence. GPU execution is
  supplementary for this CPU source-residency change; unavailable GPU coverage
  is recorded without implying a pass or blocking completion.
- [x] Update asset data lifecycle/bulk and StaticMesh rendering contracts with
  implemented semantics; close this plan only when all gates pass.

Completion: compatibility, behavior, and memory evidence are recorded and the
source API is stable for the dependent compilation plan.

## Validation And Contract Owners

Follow [build workflow](../../../Agents/BuildAndRun.md) before target operations and
[testing workflow](../../../Agents/Testing.md) before selecting native tests.
Relevant existing coverage includes StaticMeshDerivedDataContractTests,
StaticMeshPayloadCodecTests, StaticMesh collision tests, package/import tests,
and cook tests; resolve current registered targets at execution time.
Contracts: [asset lifecycle](../../../Runtime/Assets/AssetDataLifecycle.md),
[bulk data](../../../Runtime/Assets/BulkData.md), and
[StaticMesh rendering](../../../Runtime/Rendering/StaticMeshRendering.md).

## Related Code

- `Engine/Source/Runtime/Engine/Public/StaticMesh/StaticMesh.h`
- `Engine/Source/Runtime/Engine/Private/StaticMesh/StaticMesh.cpp`
- `Engine/Source/Runtime/Engine/Public/StaticMesh/StaticMeshBuildProvider.h`
- `Engine/Source/Runtime/Engine/Private/StaticMesh/StaticMeshBuildProvider.cpp`
- `Engine/Source/Runtime/Engine/Private/StaticMesh/StaticMeshBuild.cpp`
- `Engine/Source/Runtime/Engine/Private/StaticMesh/StaticMeshCook.cpp`
- `Engine/Source/Developer/StaticMeshBuild/`
- `Engine/Source/Editor/AssetForgeBuiltins/Private/StaticMeshImport.cpp`
- `Engine/Source/Editor/AssetForgeBuiltins/Private/SceneImport.cpp`

## Stage 0 Evidence And Decisions

Consumer map: `StaticMeshImport.cpp` and `SceneDirectImport.cpp` convert Assimp
scene values through `StaticMeshImportAdapter.h`; `SceneImport.cpp` prepares
scene requests but does not build StaticMesh recipes itself. Engine
`StaticMeshBuildProvider.cpp` captures, looks up DDC, decodes and transfers the
source in its result. `StaticMeshBuild.cpp` applies and synchronously rebuilds;
`StaticMeshCook.cpp` rebuilds during authored PostLoad and projects runtime cook
products. Reflection serialization/duplication transfers the source value.
Native array writers also exist in spline, physics, material Vulkan, viewport
picking and thumbnail fixtures; cache tests manually clear decoded arrays.

Persisted compatibility: `/Cpp/Engine`, `Durin::FStaticMeshImportedData` retains
`Geometry: FEditorBulkData` (BulkData property), `MaterialSlotCount: uint32`,
`MeshCount: uint32`, `SchemaVersion: uint32`, all with flags None. The declaring
`Durin::DStaticMesh::ImportedData` remains EditorOnly. Generated DHT statics are
friends, so private access does not change signatures. Geometry payload schema
stays 1; FEditorBulkData hashes canonical bytes with XXH3-128. Its instance GUID
is excluded from the existing source identity, which hashes schema, slot count,
mesh count and payload content ID in that order. No key/version bump is planned.

Selected API: `FStaticMeshDecodedGeometry`, `FStaticMeshGeometryReadHandle`,
`Initialize`, `AcquireGeometry`, `ReleaseGeometry`. Each source copy has its own
residency lock/reference but can share the immutable geometry allocation. Acquire,
release and copying a stable source synchronize on its residency mutex. Complete
source mutation, reflection load and asset application require owner-thread or
external exclusive access; handles may outlive these operations. Cache publication
also checks identity so reflected replacement cannot reuse stale residency.
Recipes own a shared const geometry handle, never a source reference. Apply takes
its authored replacement separately from the recipe/build result.

Reproducible baseline: `./DevTool test StaticMeshTests
FStaticMeshSourceResidencyTests.RepresentativeGeometry`, run in isolation on the
macOS Debug profile. Fixtures contain 1 and 100,000 disconnected triangles,
position/index channels and one material. The test explicitly captures/decodes,
then builds with DDC persistence disabled: two encodes and two decodes by the
executed call paths (fresh orchestration alone performs one of each). Observed
position/index capacity retention: 48 and 6,291,456 bytes; canonical sizes: 195
and 4,800,147 bytes. Identity low/high: 4982799754724307949 /
10298414200299834774 and 17565407108445809865 / 892654471079648671.
One-millisecond sampling of malloc default-zone live allocation reports process
peaks of 126,408,384 and 259,922,736 bytes, including test/runtime overhead and
explicit retained decoded baseline input. These are sampled allocation peaks,
not RSS or exact allocator high-water marks; `max_size_in_use` returns zero on
this host and is unusable. Log: `Build/.agent-state/logs/
20260907-015750-900623-85862-StaticMeshTests.log`. Wall-clock test duration was
2.088 seconds and is diagnostic only.

Gates: preserve canonical sizes/identities for these fixtures; fresh initialization
encodes once and seeds residency without decode; repeated acquisition shares one
allocation until release; warm hits issue no source acquisition; release drops
only this source's decoded reference and the allocation expires after its last
handle/source-copy owner. Compare sampled allocation peaks separately from time,
without timing thresholds. Decode must reject truncated/oversized arrays before
allocation and reject nonempty channels whose lengths differ from positions.

## Stages 1 And 2 Handoff

Source extraction and consumer migration form one compilable change: removing
public arrays necessarily updates all native fixtures and the Engine/provider
seam together. The new source header is explicitly registered in `Engine.dmodule`.
DHT regenerated the same four field signatures with private access. Source copy
retains immutable bulk/decoded ownership under independent residency locks;
reflection-driven identity changes invalidate cached geometry on acquisition.
Initialization preflights exact wire size and reserves it once before encoding.
Standalone/Scene application receives source separately from build products.
Assets release their decoded reference on accepted source publication; request
and import-operation copies release by scope exit.

The focused StaticMesh lane passed its pre-existing tests after migration.
Added coverage exercises 16 simultaneous readers, release/reload, independent
copy release, outstanding handles, replacement, failed initialization, malformed
bytes and channel lengths, metadata-only warm DDC reads, retryable source errors,
and checked-in Box package loading/duplication. Required validation is complete;
supplementary GPU execution remains recorded below.

## Stage 3 Validation Evidence

- `./DevTool test affected`: 30/30 native targets passed. This includes package
  save/load and import/reimport, Scene import, material mapping, collision,
  source-independent cook, cooked-load recovery, thumbnails and viewport contracts.
  Targets: AssetBulkContainerTests, AssetCompilingManagerTests, AssetCookTests,
  AssetImportDataTests, AssetImportTests, AssetPackageTests, AssetSaveReadinessTests,
  CookedMeshLoadingTests, DerivedDataCacheTests, EditorRenderingTests,
  EngineViewportHeaderTests, EnvironmentLightingTests, MaterialTests,
  MaterialThumbnailTests, MonaCoreBoundaryTests, MonaViewportTests, PhysicsSceneTests,
  RendererSceneContractTests, SandboxGameplayTests, SceneImportTests, SkyBoxTests,
  SplineTests, StaticMeshTests, StaticMeshThumbnailTests, TextureTests,
  TextureThumbnailTests, ThumbnailTests, ViewportTests,
  VolumetricCloudSceneContractTests, WorldTests.
  Log: `Build/.agent-state/logs/20260907-021139-447683-93298-ctest.log`.
- `./DevTool build --target all`: passed in 14.59 seconds on
  MacOS-arm64-Debug-DurinEditor. Log:
  `Build/.agent-state/logs/20260907-021241-066193-95086-cmake.log`.
- The bounded qualification set
  `@kind=qualification,domain=material+viewport,module=static-mesh-build+level-editor`
  built both changed fixture targets. ViewportQualificationTests passed 3/3 cases.
  MaterialVulkanTests failed before mesh work, during Vulkan instance creation:
  `VK_ERROR_INCOMPATIBLE_DRIVER`; MoltenVK reported Metal unavailable inside the
  agent sandbox. This supplementary execution did not pass and provides no GPU
  correctness or timing result; it does not block this plan or its dependents.
  Log: `Build/.agent-state/logs/20260907-021528-518749-98645-ctest.log`.
- Existing checked-in `/Engine/Models/Box` source loaded and duplicated with its
  identity and indices intact. New package round trips passed. Source storage is
  not an editable transaction property; reflected duplication and the affected
  editor transaction/material tests cover applicable copy behavior.
- Corrupt DDC rebuilt after deleting the physical import file. Cooked collision
  capture passed with that file removed. Both cooked consumer tests explicitly
  unloaded StaticMeshBuild before runtime loading, then restored the provider.
  Cooked sources remain invalid/nonresident while render/collision loading and
  readiness/retry behavior pass.
- Both baseline fixtures preserve exact payload sizes and source identities.
  The fresh-input harness now performs one encode and zero decodes; seeded
  acquisitions return the same handle. Package-backed concurrent readers issue
  one read/decode between releases, with the next acquisition after release
  issuing the second read. Warm DDC against an intentionally unreadable range
  issued zero reads; repeated misses each reported the original read diagnostic.
  Weak ownership tests confirm decoded allocation expiry after source/copy/handle
  references are released, without discarding canonical bytes.
- Independent final allocation sampling using the Stage 0 command reported
  126,410,144 bytes (small) and 235,299,648 bytes (large), versus baseline
  126,408,384 and 259,922,736 bytes. The large sampled process peak decreased
  24,623,088 bytes; the small fixture increased by 1,760 bytes. Both are
  process-level samples with no exact attribution or acceptance threshold. Explicitly retained fixture arrays still occupy 48 and
  6,291,456 capacity bytes while handles live. Final test duration was 1.770 seconds;
  no wall-clock acceptance threshold or exact allocator high-water claim is made.
  Log: `Build/.agent-state/logs/20260907-021330-045000-95889-StaticMeshTests.log`.
- Asset lifecycle, bulk ownership and StaticMesh source contracts are updated.
  macOS application smoke and other host/runtime profiles were not run; no pass
  is claimed for them. No cooked-load/GPU/collision revision semantics changed.

## Validation Scope Decision

On 2026-09-07, the user requested that GPU access restrictions in agent test
sessions not block this source-residency plan or subsequent work. A minimal
read-only Metal probe confirmed the cause: `MTLCopyAllDevices` returned zero
devices and `MTLCreateSystemDefaultDevice` returned nil inside the sandbox; the
same executable outside the sandbox returned one device, Apple M4. The host
supports Metal; the failed test process could not access it.

For this plan, required acceptance is the source/codec/residency, compatibility,
import/build/cook and CPU render/collision contract coverage, affected native
tests, and compilation of changed production and qualification fixtures. These
checks have passed. Actual GPU execution is supplementary because this change
does not alter shaders, RHI behavior or GPU resource revision semantics. The
previous blanket unsupported-host gate is superseded by this scoped decision.

Keep MaterialVulkanTests registered for GPU-capable validation; neither delete
it nor convert initialization failures into unconditional passes. Its sandbox
failure remains in the evidence. Plans that change GPU behavior must define
appropriate GPU-capable validation separately. No GPU test execution is needed
to unblock the source API dependencies of the compilation and inspection plans.
