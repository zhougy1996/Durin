# Texture Asset State Simplification Plan

Summary: Reduce texture assets to authored inputs, installed platform data, and render-resource lifetime while moving build/DDC diagnostics and request bookkeeping out of the objects.

Last reviewed: 2026-09-03

Status: Archived
Completed: 2026-09-03
- Stage 0: Freeze compatibility and field ownership (2026-09-03)
- Stage 1: Establish the minimal resource-update API (2026-09-02)
- Stage 2: Establish persistent source and transient build inputs (2026-09-03)
- Stage 3: Move operational state out of texture objects (2026-09-03)
- Stage 4: Retire publication-shaped object APIs (2026-09-03)
- Stage 5: Qualify behavior and update lasting contracts (2026-09-03)

## Current Status

All stages are complete. Texture assets now retain one persistent common source
authority per concrete family, authored settings, installed/cooked platform
data, and render-resource lifetime only. Texture2D request input is detached;
decoded recipe pixels are materialized only after a DDC miss or explicit
preview request. Build/DDC diagnostics and request bookkeeping are operation-
or manager-owned, and CPU/GPU readiness are queried from their distinct owners.

All texture object-level publication/status APIs in scope have been removed.
Mutation uses family source/settings/import-data setters, typed non-virtual
`SetPlatformData`, and the common non-virtual `DTexture::UpdateResource`.
Render-thread `FTextureResource::PublishTexture_RenderThread` remains the narrow
consumer-visible stable-reference publication boundary. Compatibility uses
load-only family migration shims, which are cleared immediately after PostLoad
so they do not retain duplicate bulk payloads during the asset lifetime.

## Goal

Make each texture asset contain only state that is authoritative for editing,
cooking, runtime use, or render-resource lifetime:

```text
editor-only authored source + editor-only build settings
    -> detached build/DDC operation
    -> SetPlatformData(complete validated value)
    -> UpdateResource()
    -> revision-qualified render-thread TextureReference switch
```

At completion:

- `FTexture2DImportedState`, `PublishImportedState`,
  `PublishDerivedDataLoad`, and `PublishUncookedLoadFailure` no longer exist;
- TextureCube and VolumeTexture no longer expose equivalent `ApplyBuildResult`
  or `PublishDerivedDataLoad` object APIs;
- texture platform data is installed through family-specific, non-virtual
  `SetPlatformData` functions and rendered through one common
  `DTexture::UpdateResource` template method;
- `DTexture` owns the common editor-source contract and each concrete texture
  stores exactly one editor-only `FTextureSource` authority, while family-
  specific imported/build-input values exist only for a build request;
- request ids, cache hit/miss origin, DDC keys, persistence messages, and
  operation errors are returned by or queried from the owning build operation
  instead of being durable texture-object state;
- CPU platform-data availability and revisioned GPU readiness remain distinct
  and are derived from their owning state rather than collapsed into
  `ETextureBuildStatus`;
- import/reimport remains failure-safe by preparing and validating every
  fallible value before the first object mutation, not by treating sequential
  GameThread assignments as an atomic transaction;
- DDC keys, texture payload bytes, cooked package behavior, supported formats,
  and the stable render-reference transition remain compatible.

## Scope

- Simplify `DTexture`, `DTexture2D`, `DTextureCube`, and `DVolumeTexture`
  platform-data and render-resource update APIs.
- Establish the persistent common `FTextureSource` contract, remove build-input residency and
  redundant Texture2D summary state.
- Classify every texture field as authored editor state, cooked/runtime state,
  transient resource state, or operation diagnostics; delete or relocate
  fields that have no asset-lifetime authority.
- Move Texture2D compilation identity and terminal diagnostics into
  `FTextureCompilingManager` and its existing bounded diagnostic history.
- Return synchronous TextureCube, VolumeTexture, post-load, and DDC failures to
  their callers without preserving narrative errors on the asset.
- Adapt TextureEditor and payload inspection to authoritative source/platform/
  render state and operation-owned diagnostics.
- Replace texture-family `PublishAssetImportData` methods with ordinary
  validated owned-child assignment, with dirtying and save sequencing owned by
  AssetForge/editor orchestration.
- Apply the same API vocabulary and ownership rules to all three texture
  families so the cleanup does not leave parallel lifecycle models.

## Non-Goals

- Removing canonical editor source data required for rebuild, reimport,
  property editing, or cook.
- Removing `FTexturePlatformData`, family platform-data serializers,
  `CookedPlatformData`, or lazy cooked-payload materialization.
- Removing render revisions, stale-result rejection, deferred resource
  cleanup, `FTextureReference`, or render-thread texture publication.
- Changing mip generation, compression, panorama projection, DDC key schemas,
  payload schemas, producer versions, source formats, or target support.
- Introducing a generic asset-state container, typeless build result, shared
  asset-status enum, or repository-wide publication abstraction.
- Extending the cleanup to StaticMesh, skeletal, terrain, material, or package
  publication APIs merely because they currently use similar names.
- Persisting diagnostics elsewhere in authored or cooked packages.

## Selected Design

### Platform data and resource update are separate operations

Each concrete texture family exposes a non-virtual setter accepting exclusive
ownership of a complete platform-data value:

```cpp
auto SetPlatformData(
    std::unique_ptr<FTexturePlatformData> Data,
    std::string& OutError) -> bool;
```

TextureCube and VolumeTexture use their corresponding typed values. A setter
requires the GameThread, validates the complete candidate before mutation, and
only replaces CPU platform-data ownership. It does not update authored source,
record DDC provenance, mark a package Dirty, emit editor notifications, or
queue GPU work.

`DTexture::UpdateResource()` is the common public resource transition. It owns
revision advancement, stable-reference initialization, candidate replacement,
queued render initialization, and deferred release. It is non-virtual; the
existing virtual `CreateRenderResourceCandidate` remains the family-specific
customization point. Callers explicitly invoke `UpdateResource()` after a
successful setter, matching the separation between data installation and
resource recreation.

`UpdateResource()` rejects an absent or invalid family payload through a
family readiness hook or a precondition established by the setter. It must not
silently publish a fallback for an invalid CPU candidate. An obsolete render
completion cannot replace a newer revision, and releasing an older concrete
resource cannot reset a stable reference that already points at the newer
resource.

### Persistent source is distinct from build input

`DTexture` owns the common editor-source validation and access boundary,
following the UE separation between source art and platform data. Each concrete
texture stores exactly one editor-only reflected `FTextureSource Source` because
the current serializer routes legacy-field migration by exact declaring type;
physically moving the field to the base would prevent the old family fields
from migrating directly. `FTextureSource` is the persistent source-art
authority and contains bulk payload, dimensions, slice/depth layout, source
format, transparency/channel metadata where applicable, schema version, and
content identity. Lightweight source queries read this metadata without
materializing bulk bytes. Cooked packages strip `Source`.

`FTexture2DImportedData` is redefined as the detached Texture2D build input
created from an immutable `FTextureSource` snapshot. It owns normalized bytes
and the exact metadata required by TextureBuild, but it is request-local: the
texture object never retains it after submission or completion. Cube and
volume builds use equivalent family-specific detached inputs derived from the
same persistent source authority.

The current persistent `FTexture2DImportedData` payload migrates without a byte
or identity change into `FTextureSource`. `FTexture2DImportedData` then becomes
the detached request snapshot, while `FTextureSourceData` remains the decoded
TextureBuild recipe input created only after a DDC miss or explicit preview
materialization. This is an ownership correction, not permission to discard
source art.
The following Texture2D object members are removed after migration:

- resident build-input `SourceData`;
- `ImportedData` once its persistent payload has migrated to `DTexture::Source`;
- `SourceWidth` and `SourceHeight`;
- `SourceChannelCount` and `bSourceHasTransparency`.

TextureCube and VolumeTexture likewise retain no family build input after a
request. Their authored pixels/faces/voxels live in `DTexture::Source`; family
layout and build settings remain separate authored metadata only where they
cannot be expressed by the common source descriptor.

Authored setters accept already prepared valid values and do not initiate a
build. Import/reimport creates canonical imported-data candidates, validates
settings and platform data, and prepares owned import-data children before it
modifies the live object. Once mutation begins, the GameThread commit contains
no expected recoverable failure or cancellation point. Dirtying, editor change
notification, save, and any compensation remain orchestration responsibilities.

### Runtime and editor fields are classified explicitly

The target ownership is:

| State | Lifetime and owner |
| --- | --- |
| Stable texture reference, render resource, revision/completion | Transient `DTexture` resource lifetime |
| Installed family PlatformData | Transient CPU runtime value, reconstructed from DDC or cooked bulk |
| Cooked PlatformData bulk | Cooked/runtime package field |
| Texture usage and sRGB when consumed by material/runtime validation | Authored runtime metadata |
| `DTexture::Source` source art and physical-source import data | Editor-only authored package state |
| Family imported/build input | Detached build request only |
| Compression, maximum-resolution, alpha-mip, panorama-projection, and volume-build settings | Editor-only authored build settings unless Stage 0 finds a runtime consumer |
| DDC key, cache origin, persistence message, decoder-invocation fact | One build/DDC operation result |
| Request serial/id, cancellation, supersession, queue/build timing | Texture2D compiling manager |
| GPU ready/failed/released state | `FTextureResourceCompletion` for the current revision |

Changing a reflected field to `EditorOnly` or removing a reflected mirror must
preserve loading of existing authored packages and the required cooked runtime
projection. Stage 0 verifies unknown-field tolerance and captures representative
pre-change packages before any schema-affecting edit. A concrete incompatibility
must be resolved with an explicit migration/version decision rather than an
incidental format break.

### Diagnostics describe operations, not assets

Remove `DerivedDataDiagnostic`, `bLoadedFromDerivedDataCache`, `BuildStatus`,
and `LastBuildError` from texture assets after their consumers migrate.
`DerivedDataKey` is also removed unless Stage 0 identifies a correctness use
that cannot be recomputed from canonical inputs and provider identity.

Texture2D asynchronous status comes from the compiling manager:

- active request phase and scheduling/build/DDC detail come from
  `GetDiagnostic`;
- terminal failure remains in the manager's bounded history and completion
  result;
- CPU readiness is the presence of valid installed PlatformData for the
  accepted generation;
- GPU readiness and upload failure come from the current render completion and
  revision.

Synchronous TextureCube, VolumeTexture, cooked-load, and post-load operations
return bounded typed results or `OutError` directly. The editor presents that
result while the operation is active; it does not require every successfully
loaded texture to retain cache provenance and formatted messages indefinitely.

Live payload inspection derives source, decoded CPU, cooked, and GPU states
from their actual owners. It may join an available manager diagnostic for an
active or recent Texture2D operation, but lack of historical DDC provenance is
reported as unobserved rather than inferred as a miss or failure. Read-only
inspection must not trigger DDC access, rebuild, or resource update.

### Publication terminology remains narrow

The word `Publish` remains for externally visible state transitions that use a
stable indirection or a real multi-object/package transaction. In this scope,
`FTextureResource::PublishTexture_RenderThread` remains unchanged. Ordinary
owned-child assignment, platform-data replacement, error reporting, and
GameThread result application use `Set`, `Apply`, `Complete`, or
`UpdateResource` according to their actual behavior.

## Implementation Stages

### Stage 0: Freeze compatibility and field ownership

- [x] Enumerate production and test consumers of every removed API and field
  for Texture2D, TextureCube, and VolumeTexture.
- [x] Prove whether each reflected build/source field is required in authored,
  Cook, and runtime variants; record the final EditorOnly/runtime split before
  editing declarations.
- [x] Verify that existing package loading tolerates removal of the redundant
  Texture2D summary fields and capture authored/cooked compatibility fixtures.
- [x] Confirm whether any correctness path requires a stored `DerivedDataKey`;
  otherwise select operation-only ownership.
- [x] Record focused test targets and existing golden DDC/payload/package
  identities without changing producer or schema versions.

Completion condition: every current field and public mutation API has one
recorded target owner or an explicit removal decision, and compatibility
fixtures exist for the serialized fields affected by the cleanup.

Stage 0 evidence and correction (2026-09-03):

- The production consumer inventory covers Engine texture implementation and
  build providers, AssetForgeBuiltins import/reimport and scene import,
  TextureEditor inspectors and thumbnails, MaterialEditor thumbnails, cook
  contributors, and live/construct-free payload inspection. The matching test
  inventory is owned by `TextureTests`, `TextureThumbnailTests`,
  `SceneImportTests`, and `TextureCookIntegrationTests`.
- `DTexture::Source` and physical-source `AssetImportData` are editor-authored
  state. Current family imported/source structs are migration inputs: their
  persistent bulk payload moves to `Source`, while normalized family build
  values become request-local. Texture2D usage and sRGB and TextureCube sRGB
  remain runtime metadata.
  Texture2D compression, alpha-mip, threshold, and maximum-resolution;
  TextureCube source-layout/panorama/source-dimension metadata; and
  VolumeTexture build settings are offline-only and may be `EditorOnly`.
- DAST v9 preserves unrecognized fields, as qualified by
  `FPackageAssetTests.SoftInspectionRejectsMalformedPayloadsAndPreservesUnknownFields`.
  Existing authored texture round-trip and cooked-strip coverage supplies the
  compatibility fixtures; removed summary fields therefore load as preserved
  unknown fields while `Source` supplies current metadata after migration.
- The initial plan incorrectly treated the current persistent
  `FTexture2DImportedData` type as the desired long-lived authority. The
  corrected model uses one `FTextureSource` per concrete asset, creates
  `FTexture2DImportedData` as a detached request snapshot, and materializes
  `FTextureSourceData` only for the recipe. Load-only deprecated family fields
  plus `FTextureSourceObjectVersion` preserve exact authored-package migration.
- Stored `DerivedDataKey` has no correctness consumer: providers compute keys
  from canonical source/settings/provider identity before lookup, while object
  consumers use it only for diagnostics and tests. It moves to operation-owned
  build products and manager history.
- Baseline identity coverage is
  `TextureDerivedDataTests` (key/payload schemas), texture import/cache tests
  (warm/cold and property invalidation), texture cook tests (editor stripping
  and cooked payloads), and cube/volume build tests. No producer version,
  derived-data key schema, TXPL payload schema, or imported-payload schema is
  changed by this plan.

### Stage 1: Establish the minimal resource-update API

- [x] Promote the common render-resource queue operation to public non-virtual
  `DTexture::UpdateResource()` while retaining virtual typed candidate
  construction below it.
- [x] Add non-virtual typed `SetPlatformData` functions to Texture2D,
  TextureCube, and VolumeTexture with GameThread and complete-value validation.
- [x] Keep installation and resource update separately testable; neither
  operation records DDC diagnostics or dirties packages.
- [x] Preserve revision ordering, stale upload rejection, stable-reference
  replacement, upload failure, retry, and deferred old-resource cleanup.
- [x] Migrate cooked lazy materialization to setter plus resource update without
  changing first-access behavior or payload validation.

Completion condition: every texture family can install valid CPU platform data
and update its resource without using an import/build publication API, while
the existing render lifecycle tests retain their behavior.

Stage 1 evidence (2026-09-02): `DTexture::UpdateResource()` now owns the
unchanged revisioned render transition, each concrete family validates and
installs its typed CPU value through `SetPlatformData`, and cooked lazy loads
use the same setter/update sequence. `./DevTool test affected` passed all 34
selected non-qualification native targets after rebuilding their dependency
closure.

### Stage 2: Establish persistent source and transient build inputs

- [x] Add the common `DTexture` source contract and one concrete editor-only
  `FTextureSource Source` storage field per family, with
  lightweight metadata/identity access that does not materialize bulk bytes.
- [x] Migrate existing Texture2D, TextureCube, and VolumeTexture authored bulk
  payloads into `Source` without changing payload bytes or identities.
- [x] Redefine family `ImportedData` values as detached request-local build
  inputs created from immutable `Source` snapshots; never store them on a
  texture after request submission or completion.
- [x] Remove Texture2D resident build-input `SourceData`, persistent
  `ImportedData`, and the four reflected source-summary mirrors.
- [x] Remove the corresponding persistent Cube/Volume family build-input state
  after their authored payload has migrated to `Source`.
- [x] Mark offline-only build settings EditorOnly after Stage 0 compatibility
  qualification, retaining only metadata with a concrete cooked/runtime
  consumer.
- [x] Update editor widgets and thumbnail renderers to use canonical metadata
  without forcing payload materialization merely to display dimensions or
  transparency.

Completion condition: each texture family has one persistent `Source`
authority, build inputs are request-local, and a warm DDC hit does not create
or require a decoded build-input allocation.

Stage 2 evidence (2026-09-03): all three families expose common source metadata
through `DTexture`, migrate their former authored payload through load-only
deprecated fields, and construct detached family inputs from immutable
`FEditorBulkData` snapshots. The Texture2D and Volume external-bulk tests prove
that warm DDC loading and input identity access perform zero source-range reads;
the first explicit decoded recipe access performs one.

### Stage 3: Move operational state out of texture objects

- [x] Move Texture2D request serial/id/failure bookkeeping into
  `FTextureCompilingManager::FAssetState` and generation-qualified bounded
  history.
- [x] Replace `ETextureBuildStatus` consumers with distinct CPU platform-data,
  compilation, and render-resource queries.
- [x] Return DDC origin/key/persistence information in build and completion
  results and expose it through manager diagnostics only where the UI needs it.
- [x] Make synchronous family and cooked-load failures caller-owned; remove
  persistent per-texture narrative diagnostics and duplicate cache-origin
  flags.
- [x] Rewrite payload inspection so each stage is derived from authoritative
  source, platform, cooked-bulk, manager, or render-completion state.
- [x] Remove `RefreshBuildStatus` after upload state and diagnostic presentation
  no longer depend on copying render failure into asset fields.

Completion condition: successful idle texture objects retain no build-operation
diagnostic payload, and failures remain actionable through the operation or
revision that produced them.

Stage 3 evidence (2026-09-03): texture headers contain no stored DDC key,
cache-origin flag, build status, narrative build error, or Texture2D request
bookkeeping. The compiling manager owns active and bounded recent records;
payload inspection and editor UI join source, installed CPU platform data,
manager diagnostics, and revisioned render completion without causing work.

### Stage 4: Retire publication-shaped object APIs

- [x] Replace `FTexture2DImportedState` application with explicit prepared
  authored assignment, typed platform-data installation, and resource update.
- [x] Migrate DDC hit, local build, post-load, import/reimport, property editing,
  scene materialization, and cook preparation for all texture families.
- [x] Replace texture `PublishAssetImportData` functions with ordinary validated
  owned-child assignment and make dirty/save ownership explicit at editor call
  sites.
- [x] Delete texture `PublishImportedState`, `ApplyBuildResult`,
  `PublishDerivedDataLoad`, and `PublishUncookedLoadFailure` declarations,
  implementations, tests, and obsolete state envelopes.
- [x] Ensure cancellation and stale-generation checks occur before the first
  live-object mutation and no callback observes a half-applied authored edit.
- [x] Remove compatibility wrappers rather than maintaining two mutation
  vocabularies.

Completion condition: repository searches find no retired texture publication
API, and all object mutation paths use the same setter/update vocabulary and
thread/failure rules.

Stage 4 evidence (2026-09-03): import/reimport, scene materialization, property
editing, post-load, DDC recovery, cook preparation, and tests use explicit
source/settings/import-data setters, typed platform-data installation, and the
common resource update. Texture-scoped searches find no retired publication or
status APIs; render-thread stable-reference publication remains unchanged.

### Stage 5: Qualify behavior and update lasting contracts

- [x] Run focused texture build, import/cache, failure, cook, thumbnail,
  material-binding, scene-import, and render-resource lifecycle tests selected
  through the repository testing workflow.
- [x] Prove supersession, cancellation, object destruction, provider retirement,
  upload failure/retry, and old-resource release cannot publish stale state.
- [x] Compare DDC keys, canonical platform payload bytes, authored compatibility
  fixtures, and cooked outputs with the Stage 0 baseline.
- [x] Build the affected Editor closure and qualify the Game/runtime closure
  with a registered Game preset or the platform's cooked-runtime contract tests;
  confirm cooked runtime retains no editor source, build settings, DDC, or
  diagnostics.
- [x] Update `TextureSystem.md`, `AssetDataLifecycle.md`, and
  `AsyncAssetOperations.md` to describe installed platform data, operation-owned
  diagnostics, and the narrow render-thread publication boundary.
- [x] Complete and archive this plan only after the lasting contracts and exact
  validation evidence are recorded.

Completion condition: all behavioral, compatibility, module-closure, and
documentation gates pass with no texture-object diagnostic or publication API
reintroduced under another name.

Stage 5 evidence (2026-09-03):

- `./DevTool test TextureTests` passed 80 tests, including golden DDC/payload
  identities, authored/cooked source stripping, warm external-bulk read counts,
  invalid resource-update rejection, supersession, cancellation, destruction,
  provider retirement, and revisioned render failure/release behavior.
- `./DevTool test affected` built and passed all 35 selected direct targets,
  covering import/cache, failure, Cook, thumbnails, materials, scene import,
  editor workflows, and resource lifecycle.
- Host-GPU qualification passed for `TextureCookIntegrationTests` and
  `VolumetricCloudQualificationTests`; the latter compiles and exercises the
  migrated VolumeTexture scene helper. The initial sandboxed Cook run could not
  create a MoltenVK instance because Metal is unavailable inside the sandbox;
  the identical host-access run passed.
- `./DevTool build` completed the full registered macOS Debug DurinEditor `all`
  target. This host profile exposes no DurinGame preset. The registered
  Win64/Game cooked projection was instead exercised by the focused and Vulkan
  Cook tests, which verify editor source/settings/diagnostics are absent and
  cooked platform data loads without source or DDC fallback. No runtime module
  dependency changed.
- Texture-scoped repository searches contain no retired object publication,
  copied build-status, or persistent diagnostic API. The three lasting contract
  documents now record source/request separation, setter/resource-update
  ownership, manager diagnostics, and render-thread-only publication.

## Acceptance Gates

| Gate | Required evidence |
| --- | --- |
| Object shape | Headers show only authored, installed/cooked platform, and render-lifetime state; removed diagnostics have no replacement fields on texture assets. |
| Source authority | Texture2D and TextureCube rebuild from one canonical editor source; UI metadata does not require a duplicate decoded source allocation. |
| Mutation boundary | All fallible build/import preparation precedes GameThread mutation; setters do not dirty, save, notify, or access DDC implicitly. |
| Resource lifecycle | Matching revisions alone update the stable texture reference; stale and failed candidates preserve the last valid reference or documented fallback behavior. |
| Diagnostics | Async diagnostics are manager/operation-owned and bounded; synchronous errors are returned; CPU and GPU readiness are queried separately. |
| Compatibility | Golden DDC keys and platform payloads are unchanged, existing authored fixtures load, and cooked Win64/Game output remains valid. |
| Dependency closure | Game/runtime builds retain no editor source, DDC, TextureBuild recipe, or diagnostic dependency introduced by the refactor. |
| Documentation | Lasting texture, asset-data, and async-operation contracts match the implemented ownership model. |

## Related Documentation

- [Texture System](../../../Runtime/Rendering/TextureSystem.md)
- [Asset Data Lifecycle and Storage](../../../Runtime/Assets/AssetDataLifecycle.md)
- [Async Asset Operations](../../../Editor/Architecture/AsyncAssetOperations.md)
- [Render Resource Lifecycle](../../../Runtime/Rendering/RenderResourceLifecycle.md)
- [Texture Build Object Boundary Completion Plan](TextureBuildObjectBoundaryCompletion.md)
- [Texture Build DDC Decoupling Plan](TextureBuildDdcDecoupling.md)

## Related Code

- `Engine/Source/Runtime/Engine/Public/Texture/Texture.h`
- `Engine/Source/Runtime/Engine/Public/Texture/Texture2D.h`
- `Engine/Source/Runtime/Engine/Public/Texture/TextureCube.h`
- `Engine/Source/Runtime/Engine/Public/Texture/VolumeTexture.h`
- `Engine/Source/Runtime/Engine/Private/Texture/Texture.cpp`
- `Engine/Source/Runtime/Engine/Private/Texture/Texture2D.cpp`
- `Engine/Source/Runtime/Engine/Private/Texture/TextureCube.cpp`
- `Engine/Source/Runtime/Engine/Private/Texture/VolumeTexture.cpp`
- `Engine/Source/Runtime/Engine/Private/Texture/Texture2DCompilation.cpp`
- `Engine/Source/Runtime/Engine/Private/Texture/TexturePayloadInspection.cpp`
- `Engine/Source/Editor/AssetForgeBuiltins/Private/Texture2DImport.cpp`
- `Engine/Source/Editor/AssetForgeBuiltins/Private/TextureCubeImport.cpp`
- `Engine/Source/Editor/AssetForgeBuiltins/Private/VolumeTextureImport.cpp`
- `Engine/Source/Editor/AssetForgeBuiltins/Private/SceneDirectImport.cpp`
- `Engine/Source/Editor/TextureEditor/Private/Texture2DPropertyEditing.cpp`
- `Engine/Source/Editor/TextureEditor/Private/Widgets/MTextureEditor.cpp`
