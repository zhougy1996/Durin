# Cooked Bulk Data Field Migration Plan

Summary: Unify DDC and Cook on family-owned PlatformData and migrate cooked payloads from DBLK descriptors to lazy `FBulkData` fields.

Last reviewed: 2026-08-30

Status: Completed
Completed: 2026-08-30

## Current Status

Stage 0 and Stage 1 are complete. `FEditorBulkData` now atomically publishes one
immutable tagged memory/package snapshot, and editor/runtime BulkData share the
bounded `FPackageResourceRange` storage value without sharing authored identity
or runtime residency. Cooked object capture dispatches `SerializeCooked` through
the existing DAST object stream, preserves explicit target context, uses the
same dispatch during NoDelta planning, and accepts detached `FBulkData` without
mutating the live value.

Stage 2 is complete. The production-neutral package path: a synthetic
PlatformData projection publishes DAST v7 plus a headerless raw `.dbulk`, marks
the manifest entry as `PackageBulk`, registers the cooked package resource, and
loads an attached `FBulkData` with zero range reads until first access. The
cooked-field bit now lives on the package manifest record, so inline-only and
metadata-only projections dispatch `SerializeCooked` without requiring a
companion. Golden wire, malformed segment, mixed legacy manifest, publication
failure, relocation/cleanup, lazy reread, retirement, and shutdown coverage use
the shared placement/publication layers plus the raw-field integration fixture.
Legacy DBLK v2 decoding remains available only for the selected compatibility
window; production writers and family descriptor callbacks were retired in
Stage 3.

Stage 3 texture migration is complete for Texture2D, TextureCube, and
VolumeTexture. Their existing TXPL `PlatformData::Serialize` remains the DDC
and Cook schema; `SerializeCooked` projects that value as a package
`FBulkData` field, small values remain inline, and large values use the raw
segment. Cook no longer mutates or saves a `CookedPayload` descriptor for these
families. Cooked object load retains attached metadata with zero range reads;
the first `GetPlatformData` or render-resource request locks, validates, and
transactionally publishes the decoded TXPL value. The 78-test Texture suite
passes; the Vulkan Cook qualification reaches the platform-data consumer gate
but cannot initialize MoltenVK because Metal is unavailable in this execution
environment.

Stage 3 is complete across the remaining families. StaticMesh owns lazy
`RenderData` and `CollisionData` fields; SkeletalMesh, AnimationClip,
TerrainHeightmap, and EnvironmentLighting own lazy `PlatformData`; Material
owns lazy `ProgramData`; and Skeleton uses the same cooked package projection
without a companion. Existing family canonical serializers remain the shared
DDC/Cook schema and first access transactionally publishes decoded runtime
state. Terrain World region and manifest output now uses manifest-owned,
headerless opaque raw segments instead of DBLK containers. Descriptor-aware
Cook overloads and the family descriptor loader have been retired from the
compiled production API; DBLK v2 decoding and construct-free legacy inspection
remain isolated for the recorded compatibility window.

Validation: `AssetPackageTests` focused cooked projection/raw-field cases and
the 139-test baseline pass; `AssetBulkContainerTests` passes 11/11.

Stage 4 qualified the common loose-field boundary and published the lasting
contracts. The 4 MiB fixture recorded 9.95 ms metadata load, 19.77 ms first
access through one exact 4 MiB read, zero resident field bytes, one registered
resource, 175.81 ms save, and 235.81 ms canonical resave (16.96 MiB/s). Family
tests cover their bounded schema shapes, DDC hit/miss and Cook reuse, lazy first
access, publication, corruption, unload, shutdown, and source/DDC independence.
The registered `fast-all` matrix builds and passes after repairing three
independently exposed stale contract tests. Texture's application-hosted Vulkan
qualification reaches its Cook/platform-data consumer boundary but cannot
initialize MoltenVK on this host because Metal is unavailable; no code-path
failure was observed before that environmental boundary.

M4 inventory is 25 checked-in DAST v7 packages and eight raw `.dbulk`
companions, with no tracked `.dabulk` companions or unknown legacy
`CookedPayload` fields. One logical 259-byte DBLK v2 regression fixture remains
in `AssetCoreTests` until M4 retires the compatibility decoder. Win64/Game is
the production-qualified family target/profile; other pairs remain explicit
unsupported inputs except the EditorValidation compatibility decoder profile.
M3 may now generalize Cook capture and publication without reopening
PlatformData ownership or schema.

## Frozen Stage 0 Contract

Ownership is singular: `FEditorBulkData` owns authored instance/content
identity; each family PlatformData owns target schema and validation; DDC owns
only its rebuildable envelope; `SerializeCooked` owns an immutable save
projection; DAST owns field order and package placement; `FPackageResourceRange`
owns bounded stored-range facts; `FBulkData` owns runtime residency and locks.
No lower layer carries a DDC key, physical path, asset schema, or target.

`Serialize` transfers authored/editor state. `SerializeCooked` receives a
persistent Cook Archive with explicit platform/profile and editor filtering,
builds only stack-local or detached projections, and never changes source
fields, dirty state, diagnostics, build revisions, or residency. A family
PlatformData `Serialize` is the only schema used by DDC and Cook; outer framing
may differ. Discovery and NoDelta capture call the same selected serializer.

| Family | Inline PlatformData | `FBulkData` fields and access |
| --- | --- | --- |
| Texture2D/Cube/Volume | dimensions, format, mip/slice layout, schema/target | one field per mip/slice payload; requested by resource upload |
| StaticMesh | bounds, sections, vertex/index layout | vertex/index streams; requested by render publication |
| BodySetup collision | shape/count metadata and collision schema | cooked geometry stream; requested by physics publication |
| SkeletalMesh | skeleton/bone/section/layout metadata | vertex, index, weight and skin streams; requested by render publication |
| AnimationClip | duration, rate, track tables and codec metadata | compressed track/key streams; requested by animation evaluation setup |
| TerrainHeightmap | dimensions, scale and sample format | height samples; requested by terrain render/collision consumers |
| Material | dependency, layout, stage and reflection metadata | shader bytecode per stage; requested by material resource publication |
| EnvironmentLighting | dimensions, format and mip layout | irradiance/radiance payload fields; requested by lighting upload |
| Skeleton | references and bone metadata only | none; package remains companion-free |

Every payload schema retains its existing family limit until measurements
justify a change; fields are required unless the current family format already
defines their absence. Schema version, target platform, and target profile are
validated before publication. Byte-dependent family validation occurs after an
explicit field lock and publishes only a complete detached runtime candidate.

The retained legacy surface is `FCookedPayloadDescriptor`,
`FCookedBulkPayload`, the DBLK v2 fixture encoder/decoder and container,
CMNF `CookedBulk` record decoding, construct-free legacy inspection, and
`AssetCoreTests/Data/CookedBulk`. `FCookedPackagePayload`,
`LoadCookedPackagePayload`, descriptor-aware Cook overloads, Terrain World
descriptors, and all family `CookedPayload` fields/loaders are retired. DBLK v2
is read-only in production after every family moves to `PackageBulk`; the
encoder exists only to generate the bounded compatibility fixture and cannot be
reached by current Cook output. The decoder remains until the recorded
fixture/corpus regeneration gate.
Unsupported DBLK versions fail in the isolated compatibility decoder; raw-field
publication keeps companion-before-package-before-manifest ordering and the
existing rollback/cleanup boundary.

## Goal

Give every payload-bearing asset family one target-specific PlatformData value
that is produced once, cached by DDC, projected into a cooked package, and
consumed by runtime. DDC and Cook share the PlatformData schema while retaining
different outer ownership envelopes. Cooked packages store large PlatformData
fields as lazy `FBulkData` ranges in the same headerless raw `.dbulk` segment
model as authored packages, without DDC, source, descriptor, or physical-path
dependencies at runtime.

Before that migration, make `FEditorBulkData` publish one immutable coherent
state snapshot so concurrent retrieval, update, copy, and serialization cannot
observe mixed identity, size, memory, or package-source state.

## Scope

- Replace `FEditorBulkData`'s parallel memory/source members and selector bool
  with one immutable snapshot containing instance identity, content identity,
  logical size, and exactly one explicit memory or package-resource source.
- Extract a bounded package-resource range value shared by editor and runtime
  BulkData without adding content identity, DDC keys, schema, target, or paths
  to `FBulkData`.
- Establish asset-owned `Serialize` and `SerializeCooked` responsibilities and
  one family-owned PlatformData serialization schema used by both DDC values
  and cooked save/load.
- Convert Texture2D, TextureCube, VolumeTexture, StaticMesh and BodySetup
  collision, SkeletalMesh, AnimationClip, TerrainHeightmap, Material cooked
  programs, and EnvironmentLighting to PlatformData values whose large runtime
  fields are `FBulkData`.
- Make the existing explicit Cook contributors emit DAST v7 packages plus raw
  `.dbulk` segments and make cooked runtime load those fields through the
  package-resource manager.
- Retain only a bounded, explicitly tested DBLK v2 compatibility read window
  until the migrated fixtures and supported cooked corpus can be regenerated.
- Update Cook/runtime inspection, manifests, failure diagnostics, tests, and
  lasting asset-data contracts for the implemented M2 behavior.

## Non-Goals

- Changing imported canonical source schemas or making `FEditorBulkData` own
  PlatformData, a DDC key, Cook target/profile, or cooked placement.
- Requiring DDC value bytes to equal complete cooked package bytes. DDC keeps
  its rebuildable cache envelope; Cook keeps DAST and package-segment framing.
- Creating project-wide Cook discovery, scheduling, incremental Cook state,
  generic save-plan aggregation, remote output stores, or Cook On The Fly; M3
  owns those workflows.
- Adding archive/install-chunk resources, remote editor virtualization,
  optional or memory-mapped fields, new compression policy, or physical range
  deduplication.
- Resaving the remaining authored DAST v6/DABK corpus or removing authored v6
  compatibility; M4 owns repository-wide legacy retirement.
- Changing renderer, physics, animation, or material payload semantics beyond
  adapting their PlatformData ownership and serialization boundary.

## Design Decisions and Invariants

- `FEditorBulkData` remains authored input only. Its state is one immutable
  shared snapshot containing `InstanceId`, `ContentId`, `LogicalSize`, and a
  tagged memory/package source. `GetPayload`, copy, and serialization capture
  one snapshot; `UpdatePayload` validates and atomically publishes a complete
  replacement while retaining an existing valid instance identity.
- Empty editor data is a valid memory-backed snapshot with the canonical empty
  content hash. A package-backed snapshot cannot also retain memory, and no bool
  may be required to interpret which source is authoritative.
- A common `FPackageResourceRange` may carry only a resource handle, segment
  offset, stored size, storage flags, and alignment. `FEditorBulkData` adds
  authored identity around it; `FBulkData` adds logical size and runtime state
  around it. The range owns no path, target, schema, hash, GUID, or DDC key.
- Each asset family owns one PlatformData logical model for a target
  platform/profile. Build produces that value once. A validated DDC hit decodes
  it; Cook consumes the same value; cooked runtime reconstructs the same value.
- `PlatformData::Serialize` owns the single stable family schema and validation
  used at DDC and cooked boundaries. Target/profile and producer/schema versions
  are explicit context, never inferred from a physical path.
- Asset `Serialize` owns ordinary authored/editor state. Asset
  `SerializeCooked` owns an immutable target-specific save projection and calls
  the same PlatformData schema; it does not mutate reflected source fields,
  PlatformData, package dirty state, build revisions, or diagnostics.
- DDC and Cook share PlatformData field meaning, ordering, limits, and codecs,
  but not outer framing. DDC may retain cache-only producer metadata. Cook uses
  DAST field metadata and raw package-segment placement and persists no DDC key.
- Large PlatformData payloads are `FBulkData` fields. Build/DDC decoding creates
  detached resident values; cooked package loading creates attached unloaded
  ranges. Small metadata remains ordinary bounded PlatformData fields.
- Cooked package load validates the complete raw segment before object
  publication but performs no field-range read or payload-sized allocation.
  Family validation that requires bytes occurs on explicit access and publishes
  runtime resources only after a detached candidate is complete.
- The cooked `.dbulk` is a raw package segment with zero padding and field
  ranges only. DAST owns extent, digest, target-compatible schema, and field
  placement; no DBLK header, payload table, or per-family physical resolver is
  emitted by the new writer.
- Existing per-asset `AddToCook` entrypoints may adapt to the new projection in
  M2, but orchestration remains explicit. M3 later generalizes capture,
  scheduling, output-store, and incremental publication without changing the
  M2 PlatformData or `FBulkData` contracts.
- DBLK v2 compatibility is read-only, separately routed, and cannot be emitted
  by new Cook output after the last family migrates. Removal requires a finite
  fixture/corpus inventory and regenerated supported Cook output.

## Implementation Stages

### Stage 0: Freeze editor snapshots and PlatformData serialization contracts

- [x] Freeze `FEditorBulkData` snapshot states, atomic publication mechanism,
  copy/move behavior, empty state, concurrent request/update ordering, failed
  update behavior, and serialization capture semantics.
- [x] Freeze `FPackageResourceRange` validation and ownership and prove its use
  does not merge editor identity with runtime residency.
- [x] Inventory every `FCookedPayloadDescriptor`,
  `LoadCookedPackagePayload`, `FCookedBulkPayload`, DBLK codec, Cook manifest,
  inspection, and family loader/writer consumer, including compatibility-only
  fixtures.
- [x] Freeze function ownership: ordinary asset `Serialize`, target-specific
  asset `SerializeCooked`, family PlatformData `Serialize`, Archive purpose and
  context, immutable save projection, and reflection/save-override interaction.
- [x] Freeze PlatformData field boundaries for every migrated family, including
  which values remain inline metadata and which become `FBulkData`, plus schema,
  target/profile, limits, required/optional, and access timing.
- [x] Freeze DBLK v2 compatibility routing, new-write cutoff, manifest changes,
  rollback/recovery behavior, and unsupported-version diagnostics.
- [x] Add contract tests that fail on mixed editor snapshots, DDC/Cook schema
  drift, cooked eager reads, raw-segment headers, and live-object mutation.

#### Acceptance Gate

- One reviewed contract assigns source identity, PlatformData schema, cache
  envelope, cooked projection, package placement, and runtime residency to
  exactly one layer; all families and legacy consumers have a finite migration
  entry and no unresolved dual-format writer remains.

### Stage 1: Publish coherent editor state and the cooked serialization boundary

- [x] Replace `FEditorBulkData` memory/source/bool state with the selected
  immutable tagged snapshot and atomic whole-state publication while preserving
  DAST v7 wire bytes and existing content/instance identity semantics.
- [x] Extract and adopt the common bounded package-resource range in
  `FEditorBulkData` and `FBulkData`; preserve retirement, cancellation, copy,
  unload, shutdown, and exactly-once request behavior.
- [x] Add focused editor BulkData tests for memory/package variants, empty data,
  copy snapshots, concurrent get/update, failed update, save during update,
  package retirement, and v6/v7 load/resave compatibility.
- [x] Add Archive and package-writer support for the selected `SerializeCooked`
  dispatch and immutable per-object Cook projection without adding a second
  object-stream grammar.
- [x] Add generic tests proving authored `Serialize` and cooked
  `SerializeCooked` select the correct fields, filter editor-only state, reject
  invalid target context, and never mutate the live graph.

#### Acceptance Gate

- Every editor payload operation observes one coherent immutable state, current
  authored packages remain byte-compatible, and a synthetic asset can project
  one PlatformData value with `FBulkData` through the cooked Archive boundary
  without a descriptor or live mutation.

### Stage 2: Replace cooked DBLK placement with DAST v7 raw field placement

- [x] Extend the current Cook package path to capture cooked PlatformData
  `FBulkData` fields, assign deterministic raw-segment ranges, and publish
  `.dbulk`, `.dasset`, and manifest state with the existing recoverable ordering.
- [x] Register cooked packages with the package-resource manager and load
  external fields as attached unloaded ranges after whole-segment validation.
- [x] Convert Cook manifest and construct-free inspection from DBLK entries to
  DAST v7 field closure while retaining explicitly labeled legacy DBLK reads.
- [x] Remove descriptor callbacks and physical companion resolution from the
  new Cook/runtime path; keep compatibility adapters isolated from production
  family APIs.
- [x] Add golden, malformed, mixed-generation, failure-injection, relocation,
  cleanup, manifest, unload, and shutdown tests for cooked raw segments.

#### Acceptance Gate

- A synthetic cooked package publishes deterministic DAST v7 plus a headerless
  raw `.dbulk`, metadata-loads with zero payload-range reads, serves exact lazy
  ranges, survives every publication failure boundary, and requires neither a
  DBLK table nor a family-owned companion path.

### Stage 3: Migrate every cooked PlatformData family

- [x] Migrate Texture2D, TextureCube, and VolumeTexture to one DDC/Cook
  PlatformData schema with mip payloads represented by `FBulkData`.
- [x] Migrate StaticMesh render data and BodySetup collision, SkeletalMesh, and
  AnimationClip to family PlatformData with explicit metadata and bulk fields.
- [x] Migrate TerrainHeightmap, Material cooked programs, and
  EnvironmentLighting; qualify Skeleton as a metadata-only Cook contributor.
- [x] Make each DDC hit decode the same PlatformData model used by Cook and
  prove Cook reuses a valid target product without reading authored payload
  bytes or rebuilding it.
- [x] Make runtime resource creation request only the required `FBulkData`
  ranges, validate family schema after retrieval, and publish transactionally
  without source or DDC fallback.
- [x] Remove `LoadCookedPackagePayload`, `FCookedPackagePayload`, descriptor-aware
  Cook APIs, and all DBLK production use once the final family migrates; retain
  `FCookedPayloadDescriptor`, `FCookedBulkContainer`, and the DBLK v2 codec only
  inside the selected decoder/inspection compatibility surface.
- [x] Add family tests for build, DDC miss/hit, Cook, cooked metadata load,
  first access, render/physics publication, corruption, cancellation, unload,
  shutdown, and runtime source/DDC independence.

#### Acceptance Gate

- Every supported payload-bearing cooked family uses one PlatformData model at
  DDC, Cook, and runtime boundaries; all large fields are lazy `FBulkData`;
  new Cook output contains no DBLK or cooked descriptor; and metadata-only
  packages remain companion-free.

### Stage 4: Qualify M2 and publish lasting contracts

- [x] Measure DDC encode/decode, Cook reuse, raw segment bytes, cooked metadata
  load, first required range access, resident bytes, range count/bytes, resource
  handles, runtime publication, unload, and Cook throughput using bounded
  fixtures for each payload shape.
- [x] Run the smallest registered Core, CoreDObject, AssetCore, Engine family,
  Cook, runtime, renderer/physics consumer, and application smoke targets
  selected through repository guidance; use supported failure-injection and
  sanitizer modes.
- [x] Update Asset Data Lifecycle, Package Bulk Data, Asset Packages,
  Serialization, File I/O, Cook/manifest, inspection, and family contracts to
  own the implemented PlatformData and cooked field behavior.
- [x] Record the remaining DBLK package/fixture inventory, compatibility removal
  gate, measured budgets, M3 entry evidence, and any intentionally unsupported
  legacy target/profile.

#### Acceptance Gate

- The documented production path builds one PlatformData value, reuses its
  schema across DDC and Cook, loads cooked runtime fields lazily from raw package
  resources, passes every required target within recorded budgets, and gives M3
  sufficient evidence to generalize Cook publication without reopening M2 data
  ownership or serialization decisions.

## Validation Matrix

| Area | Evidence |
| --- | --- |
| Editor state | Tagged memory/package source, atomic update, concurrent request capture, copy snapshot, identity retention, failure conservation, unchanged DAST v7 bytes |
| Serialization ownership | Authored `Serialize`, cooked `SerializeCooked`, one PlatformData schema, target context, editor-only filtering, immutable save projection |
| DDC and Build | Stable key inputs, validated hit decode, miss build, Cook reuse without source read, cache-envelope independence |
| Cooked package | DAST v7 field metadata, deterministic raw range layout, zero padding, extent/digest binding, no DBLK header/table |
| Runtime field | Metadata-only load, exact lazy reads, lock/unlock/unload, cancellation, retirement, source/DDC-free failure policy |
| Families | Texture2D/Cube/Volume, StaticMesh/collision, SkeletalMesh, AnimationClip, TerrainHeightmap, Material, EnvironmentLighting, metadata-only Skeleton |
| Publication | Package/segment/manifest ordering, rollback, recovery, cleanup, mixed generation, corruption, missing segment, shutdown |
| Compatibility | Read-only DBLK v2 route, no new DBLK writer, finite fixtures/corpus, explicit removal and unsupported-version gates |
| Budgets | DDC codec latency, Cook throughput, metadata latency, first-access latency, reads/bytes, residency, handles, segment size |

## Definition of Done

- `FEditorBulkData` has one coherent immutable tagged state and retains its
  authored-only identity and retrieval responsibilities.
- Each payload-bearing asset family has one target-specific PlatformData model
  and one stable schema shared by DDC and Cook.
- Asset `Serialize` and `SerializeCooked` have distinct documented ownership;
  Cook projection is immutable and never persists a DDC reference.
- Cooked DAST v7 packages use `FBulkData` fields and headerless raw `.dbulk`
  segments; cooked runtime metadata load is lazy and package-resource-backed.
- New Cook output emits no DBLK v2 container or `FCookedPayloadDescriptor`, and
  the remaining compatibility inventory and removal gate are explicit.
- Family, package, publication, runtime, failure, unload, shutdown, and
  application qualification pass within recorded budgets.
- Lasting contracts own the implemented behavior and the roadmap records M2
  completion and exact M3 entry evidence.

## Deferred Follow-ups

- M3 generalizes Cook capture, scheduling, incremental state, output-store,
  manifest, and publication after every family expresses PlatformData through
  the M2 serialization boundary.
- M4 resaves the repository corpus and removes DAST v6/DABK, DBLK v2,
  compatibility fixtures, and migration-only APIs.
- M5 selects archive, install-chunk, memory-mapped, optional, or remote stores
  only from measured post-migration evidence.

## Related Documentation

- [Package Bulk Data System Roadmap](../Roadmaps/PackageBulkDataSystem.md)
- [Package Bulk Data](../Runtime/Assets/BulkData.md)
- [Asset Data Lifecycle and Storage](../Runtime/Assets/AssetDataLifecycle.md)
- [Asset Packages](../Runtime/Assets/AssetPackages.md)
- [Serialization](../Runtime/Core/Serialization.md)
- [File I/O](../Runtime/Core/FileIO.md)
- [Agent Build and Run Workflow](../Agents/BuildAndRun.md)
- [Agent Testing Workflow](../Agents/Testing.md)

## Related Code

- `Engine/Source/Runtime/Core/Public/Serialization/Archive.h`
- `Engine/Source/Runtime/Engine/Public/Asset/BulkData.h`
- `Engine/Source/Runtime/Engine/Public/Asset/EditorBulkData.h`
- `Engine/Source/Runtime/Engine/Public/Asset/CookedAsset.h`
- `Engine/Source/Runtime/Engine/Public/Asset/Cook.h`
- `Engine/Source/Runtime/Engine/Private/Asset`
- `Engine/Source/Runtime/Engine/Public/Texture`
- `Engine/Source/Runtime/Engine/Private/Texture`
- `Engine/Source/Runtime/Engine/Public/StaticMesh`
- `Engine/Source/Runtime/Engine/Private/StaticMesh`
- `Engine/Source/Runtime/Engine/Public/SkeletalMesh`
- `Engine/Source/Runtime/Engine/Private/SkeletalMesh`
- `Engine/Source/Runtime/Engine/Public/Animation`
- `Engine/Source/Runtime/Engine/Private/Animation`
- `Engine/Source/Runtime/Engine/Public/Terrain`
- `Engine/Source/Runtime/Engine/Private/Terrain`
- `Engine/Source/Runtime/Engine/Public/Materials`
- `Engine/Source/Runtime/Engine/Private/Materials`
- `Engine/Tests/Native/AssetCoreTests`
- `Engine/Tests/Native/EngineTests`
