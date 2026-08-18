# Terrain Heightmap Asset Foundation Plan

Summary: Add a dedicated lossless 16-bit heightmap asset with transactional import, derived data, Cook/load, and bounded editor inspection as the authority for later terrain rendering and collision.

Last reviewed: 2026-08-12

Status: Archived
Completed: 2026-08-12

## Current Status

T0 is implemented. `DTerrainHeightmap` is an Engine-owned asset with exact
top-left row-major `uint16` samples, a deterministic 64×64 regional min/max
hierarchy, strict grayscale16 PNG import, transactional StandardAssetImport
reimport, independent DDC and THPL Cook schemas, source-free cooked-runtime
load, source-reference integration, bounded reflected inspection, and a
Content Browser import action.

The lasting contract is
[Terrain Heightmap Asset](../../../Runtime/Terrain/TerrainHeightmapAsset.md). The
[Heightfield Terrain Roadmap](../../../Roadmaps/Archive/2026-08/HeightfieldTerrain.md) records T0 as
complete and makes T1/T2 ready for plan selection.

## Goal

Create one Engine-owned `DTerrainHeightmap` asset whose committed revision
preserves exact linear unsigned 16-bit height samples and bounded regional
min/max metadata across import, save/load, DDC restore, reimport, Cook, and
cooked-runtime load. Later Renderer, Aether, and editor plans can consume this
asset without reopening source files, routing through `DTexture2D`, or
inventing their own height authority.

## Scope

- A reflected `DTerrainHeightmap` Engine asset and forward declaration.
- One canonical row-major `uint16` sample plane with explicit width, height,
  orientation, validation limits, min/max facts, and monotonic revision.
- Import of lossless 16-bit single-channel grayscale PNG through the standard
  single-asset import framework.
- A deterministic regional min/max hierarchy suitable for later patch bounds
  and collision acceleration decisions.
- Independently versioned DDC object and cooked companion schemas, keys,
  checksums, allocation limits, encode/decode, and corruption rejection.
- Editor source provenance, import/reimport transactionality, asset-registry
  identity, source-reference indexing, and bounded inspection facts.
- Focused tests for fidelity, orientation, validation, DDC, Cook/load,
  transactionality, registry/import integration, and malformed inputs.
- Lasting asset contract documentation after implementation.

## Non-Goals

- Terrain Actor, Component, SceneProxy, SceneInfo, vertex factory, shader,
  render resource, thumbnail render, or viewport terrain drawing.
- RHI texture creation or a promise that the eventual GPU representation is
  identical to the cooked CPU payload layout.
- Aether HeightField geometry, BodySetup publication, traces, sweeps, overlaps,
  navigation, or gameplay height queries.
- 8-bit image promotion, RGB-to-luminance conversion, JPEG, ordinary texture
  import, RAW files requiring sidecar dimensions/endian metadata, EXR, floating
  point heights, or signed heights.
- Height resampling, cropping, erosion, smoothing, holes, sculpting, painting,
  layers, streaming, tiling multiple assets, or runtime mutation.
- Component world scale, Z offset, physical units, material mapping, patch size,
  render LOD, or collision cell policy; the roadmap assigns those decisions to
  their consuming child plans.
- Refactoring `DTexture2D` or generalizing all asset build coordination merely
  to share code with one new asset.

## Design Decisions and Invariants

### Canonical height semantics

- `DTerrainHeightmap` does not derive from `DTexture` and never advertises an
  `ETextureUsage`. It is an Engine asset backed by terrain-specific value,
  derived-data, and persistence types.
- A sample is an unsigned 16-bit normalized height in `[0, 65535]`. The asset
  stores no world-space Z unit. A later component computes world height from
  its authored Z scale and offset.
- Samples are tightly packed row-major. `(0, 0)` is the top-left imported pixel;
  X increases with column and Y increases with row in asset sample space.
  Import never flips, rotates, transposes, gamma-corrects, normalizes by the
  observed min/max, or color-converts samples.
- Width and height are independently validated and need not be square or
  `2^n + 1`. Each dimension is at least two. Stage 0 freezes the maximum
  dimensions, total sample count, encoded-source size, decoded bytes, hierarchy
  bytes, and peak builder allocation before implementation.
- The committed payload records exact global minimum and maximum sample values.
  Uniform maps are valid and retain equal minimum/maximum values.

### Regional metadata is deterministic and lossless

- The derived payload includes the complete canonical sample plane plus a
  hierarchy of conservative unsigned min/max pairs. Level 0 covers fixed-size
  sample regions selected in Stage 0; each later level combines exactly up to
  four children with edge clamping or bounded partial coverage defined in the
  frozen schema.
- The hierarchy never replaces samples, averages heights, or changes the
  canonical value used by a future render/collision consumer. Every node must
  contain the exact minimum and maximum of its covered source rectangle.
- Node ordering, level offsets, dimensions, coverage, alignment, and retained
  bytes are serialized facts and have golden-byte tests. Consumers query a
  read-only value interface rather than depending on builder vectors.

### Source, DDC, package, and cooked state stay distinct

- The authored package retains mounted source path, exact source-content hash,
  cheap source fingerprint, source dimensions/bit depth/channel facts, build
  version, and the descriptor needed to restore derived data. It does not
  serialize decoded source pixels into ordinary reflected fields.
- Editor DDC keys include exact source identity, canonical format/orientation,
  hierarchy policy, builder/schema versions, and target/profile identity where
  it can change bytes. A warm DDC hit reconstructs the same validated immutable
  CPU payload without decoding PNG.
- Cook publishes one terrain-height companion entry with a new stable payload
  ID and a descriptor independent from texture, static-mesh render, and
  collision payload IDs. Cooked packages strip editor-only source path and
  fingerprint fields.
- Cooked runtime accepts only the selected platform/profile/schema and validates
  dimensions, counts, offsets, non-overlap, alignment, min/max consistency,
  checksums, and allocation ceilings before publishing. Missing, corrupt,
  truncated, oversized, or incompatible required bulk is a hard asset-qualified
  failure; it never falls back to source, DDC, Texture2D, or a flat plane.
- DDC corruption or absence is a rebuildable editor miss only while valid
  source is available. Persistence failure cannot replace an already committed
  valid asset revision with partial state.

### Import and reimport publish atomically

- The StandardAssetImport provider owns `.png` routing only when the request
  explicitly targets `DTerrainHeightmap`; ordinary `.png` default routing
  remains `DTexture2D` and scene-import material textures are unchanged.
- Decode accepts only a PNG whose decoded source is one channel and exactly 16
  bits per sample. Unsupported bit depth/color type, interlacing policy,
  malformed chunks, excessive dimensions, non-finite metadata, or allocation
  overflow fails with a source-qualified diagnostic before creating a package.
- The importer builds and validates a detached candidate. Reimport exchanges
  the complete committed state only after decode, hierarchy build, payload
  validation, and required persistence succeed. Failure retains object identity,
  package Dirty state, source reference, revision, samples, metadata, and prior
  DDC/cooked descriptor.
- Reimport of byte-identical source is a semantic no-op: it does not advance the
  asset revision or dirty the package. A real committed content change advances
  the revision exactly once and updates the source-reference index atomically.
- Stage 0 freezes whether decode/build execution is synchronous inside the
  existing import transaction or admitted through the CPU task system. The
  selected path must include cancellation, memory admission, shutdown, and
  target-publication rules; it may not borrow `FTexture2DBuildCoordinator`
  unless that type is first generalized without texture policy leakage.

### Asset API is consumer-safe but renderer-neutral

- Public access exposes dimensions, sample lookup/copy or immutable snapshot,
  global/regional min/max query, revision, readiness/status, and exact retained
  bytes. It exposes no writable sample vector, decoder object, DDC path,
  platform file handle, or mutable hierarchy node.
- A consumer can retain an immutable payload snapshot across asset revision
  replacement without observing freed or half-updated memory. Stage 0 freezes
  shared ownership and destruction semantics consistent with Engine asset
  lifetime and expected later render-command publication.
- Object duplication copies the committed semantic state and source provenance
  according to existing asset rules but does not alias mutable proposal state,
  pending imports, or transient diagnostics.
- Load, import, reimport, exchange, destruction, and process shutdown are
  GameThread publication operations. Worker code, if selected, receives value
  snapshots only and never reads or writes a DObject.

## Current Foundations and Gaps

| Area | Existing foundation | Gap owned by this plan |
| --- | --- | --- |
| Reflected asset | Engine DObject asset classes, serialization hooks, duplication, destruction | Add `DTerrainHeightmap`, metadata, revision/status, immutable payload ownership, and reflection registration |
| Source decode | Existing encoded image loading and RGBA8 texture decode | Preserve exact 16-bit single-channel PNG values with explicit color-type/bit-depth rejection and orientation fixtures |
| Derived data | Content-addressed object storage, versioned/checksummed payload patterns | Define terrain key, hierarchy builder, schema, encode/decode, ceilings, warm hit, corrupt miss, and diagnostics |
| Cook/runtime | Package companion payloads and descriptor validation | Add independent height payload ID/schema, transactional Cook contribution, stripped runtime metadata, and hard load failures |
| Import/reimport | AssetImportCore planning/execution and StandardAssetImport providers | Add explicit target routing, detached candidate, semantic no-op, atomic exchange, source index, and failure preservation |
| Editor presentation | Content Browser class facts, generic property/asset inspection | Register a clear asset identity and bounded height-specific facts without building a terrain renderer or dedicated editor workspace |
| Testing | Texture and StaticMesh asset/DDC/Cook/import fixtures | Add asymmetric 16-bit golden sources, malformed/oversized cases, exact sample/hierarchy bytes, and transactionality coverage |

## Implementation Stages

### Stage 0: Freeze format, limits, ownership, and baselines

- [x] Inventory `DTexture2D` asset lifecycle, source provenance, DDC key/payload,
  Cook/load, import/reimport provider, source-reference index, asset-registry
  presentation, duplication, and shutdown seams only as references; record all
  places where Terrain must remain separately identified.
- [x] Add asymmetric non-square 16-bit grayscale PNG fixture designs with
  distinct corners, row/column gradients, repeated extrema, odd dimensions,
  and uniform data. Record decoded golden samples without using production code.
- [x] Freeze PNG color type, interlace acceptance, byte order at the decoder
  boundary, row origin, sample axes, canonical native-memory encoding, and
  explicit error behavior for 8-bit, multichannel, palette, alpha, malformed,
  truncated, and oversized inputs.
- [x] Measure or derive exact decoded, hierarchy, payload, and peak-build bytes
  for representative and maximum candidates. Freeze per-dimension, total
  sample, encoded-source, payload, hierarchy, and allocation ceilings.
- [x] Freeze hierarchy base-region dimensions, odd-edge coverage, level ordering,
  node layout, alignment, and exact min/max query semantics.
- [x] Freeze class name, package fields, revision/status values, immutable
  payload ownership, duplicate/exchange semantics, DDC namespace/key fields,
  builder/schema/platform versions, cooked payload ID, descriptor, and binary
  layout with golden bytes.
- [x] Decide synchronous versus task-system build execution from measured
  representative/maximum decode and hierarchy costs. If asynchronous, freeze
  admission bytes, cancellation checkpoints, generation matching, completion
  pumping, shutdown, and save/Cook waiting behavior before implementation.
- [x] Record baseline focused EngineTests and documentation validation, plus any
  pre-existing failure, in the Stage 0 handoff.

#### Acceptance Gate

- Canonical sample semantics, source acceptance, coordinates, limits,
  hierarchy, public ownership, import publication, DDC, Cook, failure behavior,
  threading, and golden fixtures have no unresolved choice.
- Maximum-input peak memory is bounded by a named ceiling and the selected
  execution model cannot expose partial DObject state or deadlock save/Cook.
- Payload IDs and schemas cannot collide with Texture2D, TextureCube,
  StaticMesh render, or StaticMesh collision data.
- Existing focused asset/import/Cook baselines pass or pre-existing failures
  are recorded before implementation.

### Stage 1: Add the runtime asset and canonical payload builder

- [x] Add terrain heightmap public/private Engine files, forward declaration,
  reflection registration, build membership, asset class metadata, and a
  minimal reflected package representation.
- [x] Implement immutable canonical payload ownership, exact validation,
  dimensions, sample lookup/snapshot, global min/max, regional hierarchy query,
  revision/status, retained bytes, and finite allocation guards.
- [x] Implement the deterministic hierarchy builder with checked arithmetic,
  odd-dimension coverage, exact extrema, stable node order, and no sample
  averaging or mutation.
- [x] Implement duplicate, exchange, PostLoad, BeginDestroy/FinishDestroy, and
  shutdown behavior so retained payloads survive consumer snapshots and no
  transient proposal/build state is serialized or aliased.
- [x] Add focused unit tests for asymmetric orientation, non-square/odd/uniform
  inputs, exact global/regional extrema, invalid dimensions/counts, overflow,
  immutable revision replacement, duplication, destruction, and retained bytes.
- [x] Record the stage handoff and focused validation evidence.

#### Acceptance Gate

- A detached canonical candidate validates and publishes as one immutable
  revision; every sample and hierarchy answer matches independent golden data.
- No public API exposes mutable sample/hierarchy storage or a renderer,
  collision, decoder, DDC, or source-file dependency.
- Invalid or oversized candidates fail before allocation/publication and leave
  the previous asset state unchanged.

### Stage 2: Implement 16-bit PNG import and transactional reimport

- [x] Add a decoder boundary that returns exact single-channel `uint16` rows
  and source facts without passing through RGBA8, sRGB, BC compression, or
  observed-range normalization.
- [x] Register an explicit `DTerrainHeightmap` StandardAssetImport provider
  while preserving ordinary `.png` Texture2D default routing and scene import.
- [x] Build a detached import candidate from decoded samples and hierarchy,
  retain mounted source provenance/hash/fingerprint, and publish a new package
  only after complete validation.
- [x] Add same-path and moved-path reimport planning, semantic no-op detection,
  complete-state exchange, source-reference index updates, package Dirty and
  revision behavior, cancellation/failure preservation, and provider
  registration/unregistration safety.
- [x] Expose bounded import/build diagnostics including source facts, canonical
  bytes, hierarchy bytes, peak estimated bytes, elapsed phases, revision, and
  latest failure without retaining encoded/decoded proposals indefinitely.
- [x] Add focused tests for every accepted golden PNG and rejected bit depth,
  color type, malformed/truncated, dimension, allocation, path, reimport,
  no-op, provider-routing, and rollback case.
- [x] Record the stage handoff and focused validation evidence.

#### Acceptance Gate

- Accepted PNG samples are bit-identical to independent golden values and the
  asymmetric fixture proves row/column orientation.
- Default PNG import still creates Texture2D; only an explicit heightmap target
  creates `DTerrainHeightmap`; scene material imports remain unchanged.
- Failed/cancelled reimport and persistence never alter target identity,
  samples, metadata, source reference, revision, Dirty state, or diagnostics
  beyond the named failure record; byte-identical reimport is a no-op.

### Stage 3: Add DDC, Cook, and cooked-runtime loading

- [x] Implement the frozen terrain DDC key, namespace, payload encoder/decoder,
  checksums, checked byte-range validation, atomic persistence, warm restore,
  source fingerprint reuse, and corrupt/incompatible miss behavior.
- [x] Make editor PostLoad distinguish warm restore, rebuildable miss, missing
  source, active build, failure, and ready state without silently substituting
  a Texture2D or zero-height payload.
- [x] Contribute the independently identified height payload and descriptor to
  `FCookContext` transactionally; strip source-only fields and reject Cook when
  the committed revision is unavailable or invalid.
- [x] Implement cooked-runtime descriptor/bulk resolution and complete schema,
  platform/profile, dimension, count, offset, alignment, checksum, extrema,
  hierarchy, and allocation validation before publication.
- [x] Add golden-byte, warm/cold DDC, missing/corrupt/truncated/oversized/schema/
  platform mismatch, source-unavailable, Cook rollback, cooked-package
  round-trip, and runtime-without-source/DDC tests.
- [x] Record payload sizes, cache/build timings, peak memory, and the stage
  handoff with focused validation evidence.

#### Acceptance Gate

- Import, package reload, warm DDC, rebuild, Cook, and cooked-runtime load
  publish identical canonical samples and hierarchy queries.
- Editor rebuildable failures retain prior valid state or report an explicit
  unavailable state; cooked runtime rejects every required-payload defect with
  an asset-qualified error and has no source/DDC fallback.
- DDC and Cook bytes stay within frozen ceilings, use stable golden identities,
  and cannot be confused with ordinary texture or mesh payloads.

### Stage 4: Integrate inspection, qualify T0, and publish the contract

- [x] Register Content Browser/source-reference facts and generic inspection
  fields for dimensions, format, global range, source path/status, revision,
  sample bytes, hierarchy bytes, total retained bytes, DDC identity, payload
  versions, and latest bounded diagnostic.
- [x] Ensure save/reload, duplicate, rename/move, delete, source move, reimport,
  asset-registry refresh, and editor shutdown preserve the frozen asset and
  source-reference contracts without requiring a dedicated heightmap editor or
  rendered thumbnail.
- [x] Run the smallest affected native test targets, documentation validation,
  and any broader validation required by actual cross-target changes according
  to the repository build/test guidance.
- [x] Because the new importable asset is user-visible, complete the
  repository-required full `all` build and validation-enabled editor smoke from
  one Agent Build Profile, covering import, inspection, save/reload, reimport,
  Cook, runtime load, and orderly shutdown.
- [x] Publish the implemented long-lived heightmap asset, format, DDC,
  Cook/load, source, failure, and diagnostic contracts under
  `Documentation/Runtime/`; update the Heightfield Terrain roadmap T0 status
  and the precise entry state for T1/T2.
- [x] Record final source revision, format/version identities, limits, memory/
  timing evidence, validation, verified editor executable, decisions, and open
  follow-ups in the final handoff.

#### Acceptance Gate

- A user can explicitly import, inspect, save, reload, reimport, Cook, and load
  a heightmap asset with exact 16-bit fidelity and clear failure diagnostics.
- Fidelity, orientation, hierarchy, limits, ownership, transactionality, DDC,
  Cook/runtime, registry/source reference, duplication, lifecycle, full build,
  and editor-smoke validation all pass.
- Lasting documentation is authoritative, T0 is closed in the roadmap, and
  T1/T2 can consume the immutable revisioned payload without reopening source
  or revisiting its semantics.

## Validation Matrix

| Scenario | Required behavior | Evidence owner |
| --- | --- | --- |
| Asymmetric golden PNG | Exact corner/interior `uint16` values and top-left row-major orientation | Heightmap builder/import tests |
| Odd and non-square dimensions | Exact sample count, hierarchy coverage, extrema, and checked layout | Heightmap builder tests |
| Uniform/extreme samples | `0`, `65535`, repeated extrema, and equal global min/max remain exact | Heightmap builder tests |
| Unsupported source | 8-bit, RGB/RGBA, palette, malformed, truncated, oversized, and disallowed interlace fail before publication | Decoder/import negative tests |
| Explicit routing | Default `.png` remains Texture2D; explicit target is Heightmap; scene texture import is unchanged | StandardAssetImport tests |
| Reimport transaction | Changed source publishes once; identical source is a no-op; failure/cancel preserves complete prior state | Import/reimport and source-index tests |
| Immutable lifetime | Snapshots survive revision replacement and asset teardown without mutable aliasing or leaks | Engine lifecycle tests |
| DDC | Cold build and warm restore match; missing/corrupt/incompatible data rebuilds only with valid source | Heightmap derived-data tests |
| Cook transaction | One stable descriptor/payload is contributed atomically; invalid/unready assets reject Cook | Heightmap Cook tests |
| Cooked runtime | Source/DDC-free load is exact; every descriptor/payload defect fails before publication | Cooked package tests |
| Duplication/package operations | Duplicate, save/load, rename/move, registry refresh, delete, and source references preserve identity rules | Engine/editor asset tests |
| Resource ceilings | Representative and maximum inputs stay within frozen encoded, decoded, hierarchy, payload, peak, and retained-byte limits | Builder diagnostics and characterization fixture |
| Shutdown | Pending or committed work drains/cancels according to the frozen execution model with no stale callback or DObject access | Engine/editor shutdown tests |

## Definition of Done

- `DTerrainHeightmap` is a registered Engine asset with one immutable,
  revisioned, exact `uint16` canonical payload and deterministic regional
  min/max hierarchy.
- Explicit 16-bit grayscale PNG import and reimport are transactional and do
  not alter ordinary Texture2D or scene-import routing.
- DDC, package, Cook, and cooked-runtime paths are independently versioned,
  bounded, checksummed, corruption-safe, and bit-identical on accepted data.
- Public API, diagnostics, Content Browser/source-reference integration,
  duplication, lifecycle, and shutdown provide the stable consumer boundary
  required by later rendering and collision plans.
- Focused tests, required aggregate validation, full build, editor smoke, and
  documentation validation pass; lasting contracts are published and roadmap
  T0 is complete.

## Final Handoff

- Completed on 2026-08-12 in the completion commit for this plan (the final
  commit identifier is reported with the handoff because a commit cannot
  contain its own stable identifier).
- The canonical contract is synchronous construction of immutable
  `shared_ptr<const FTerrainHeightmapPayload>` revisions from non-interlaced,
  16-bit grayscale PNG input. Samples use top-left, row-major `uint16` order;
  regional extrema use deterministic 64x64 base regions and 2x2 reduction.
- Frozen limits are 16,384 per dimension, 268,435,456 samples, 512 MiB encoded
  input, 513 MiB payload, 512 KiB hierarchy, and 2,560 MiB admitted peak memory.
  At the maximum square extent the retained canonical data is 537,220,652
  bytes: 536,870,912 sample bytes plus 349,524 hierarchy bytes and nine level
  records.
- Derived data uses `TerrainHeightmap/Objects`, the terrain-specific builder
  identity, THPL magic `0x4C504854`, schema/build version 1, and Cook bulk GUID
  `{7d0d1524-69ba42a9-91f70da3-47bc2861}`. Payloads are aligned, checksummed,
  bounded, and structurally revalidated before publication.
- Focused validation passed: `TerrainHeightmapTests` (6/6, 372 ms),
  `TerrainHeightmapCookTests` (1/1, 121 ms), `TextureTests` (74 passed and two
  intentional skips), and `EditorAssetWorkflowTests` (80 passed and one
  intentional skip). The complete `all` Agent build passed in 7.70 seconds,
  followed by a validation-enabled hidden editor smoke in 2.61 seconds.
- Documentation plan and roadmap validation passed. The verified executable is
  `Engine/Binaries/Win64/Debug/Runtime/DurinEditor/DurinEditor.exe` from the
  same Agent Build Profile.
- There are no open T0 blockers. T1 may consume the immutable revisioned
  payload for rendering, and T2 may consume it for collision without reopening
  source decoding, ownership, or persistence semantics. Remaining work is
  listed below and on the Heightfield Terrain roadmap.

## Deferred Follow-ups

- Terrain Actor/Component, patch render resources, R16 GPU upload, vertex
  factory/shader, PBR material mapping, and visible thumbnail/preview (T1).
- Aether HeightField geometry, query algorithms, BodySetup/BodyInstance
  publication, collision payload, and debug rendering (T2).
- Terrain patch LOD, error metric, neighbor resolution, skirts/stitching,
  conservative LOD bounds, and render diagnostics (T3).
- Dedicated heightmap/terrain editor UX, placement, viewport picking, and final
  end-to-end qualification beyond generic inspection (T4).
- RAW16 with explicit dimensions/endian metadata, floating-point EXR, signed or
  physical-height import, resampling, multiple tiled assets, and streaming,
  each behind a concrete consumer and format decision.
- Writable sample regions, partial DDC/Cook updates, sculpting, holes, painted
  layers, runtime deformation, and network replication.

## Related Documentation

- [Heightfield Terrain Roadmap](../../../Roadmaps/Archive/2026-08/HeightfieldTerrain.md)
- [Texture System](../../../Runtime/Rendering/TextureSystem.md)
- [Asset Data Lifecycle and Storage](../../../Runtime/Assets/AssetDataLifecycle.md)
- [Asset Packages](../../../Runtime/Assets/AssetPackages.md)
- [Asset Versioning](../../../Runtime/Assets/Versioning.md)
- [CPU Task System](../../../Runtime/Core/TaskSystem.md)
- [Rendering Capability Expansion](../../../Roadmaps/Archive/2026-08/RenderingCapabilityExpansion.md)
- [Aether Physics Evolution](../../../Roadmaps/Archive/2026-08/AetherPhysicsEvolution.md)
- [Build and Run](../../../Development/Build/BuildAndRun.md)

## Related Code

- `Engine/Source/Runtime/Engine/Engine.dmodule`
- `Engine/Source/Runtime/Engine/Public/EngineFwd.h`
- `Engine/Source/Runtime/Engine/Public/Texture/Texture2D.h`
- `Engine/Source/Runtime/Engine/Public/Texture/TextureDerivedData.h`
- `Engine/Source/Runtime/Engine/Private/Texture/Texture2D.cpp`
- `Engine/Source/Runtime/Engine/Private/Texture/TextureDerivedData.cpp`
- `Engine/Source/Runtime/AssetCore`
- `Engine/Source/Editor/AssetImportCore`
- `Engine/Source/Editor/StandardAssetImport/Public/StandardAssetImportProviders.h`
- `Engine/Source/Editor/StandardAssetImport/Private/StandardAssetImportProviders.cpp`
- `Engine/Source/Editor/LevelEditor`
- `Engine/Tests/Native/EngineTests/Private/Texture`
- `Engine/Tests/Native/EngineTests/Private/SourceReferenceIndexTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/Editor/ContentBrowserModelTests.cpp`
