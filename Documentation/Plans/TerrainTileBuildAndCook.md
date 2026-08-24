# Terrain Tile Build and Cook Plan

Summary: Implement normalized Terrain tile inputs, product codecs, DDC, Cook, manifests, and source-free loading

Last reviewed: 2026-08-25

Status: Active
Completed:

## Current Status

T0 completed through the [Terrain World Data Contract
Plan](TerrainWorldDataContract.md). The authoritative [Terrain World Data
contract](../Runtime/Terrain/TerrainWorldData.md) freezes all input domains,
wire values, product magics/ceilings, build-key fields, atomic generation,
8×8-region packages, compatibility rejection, outcomes, and qualification
vectors. Legacy Sandbox Terrain content is gone; existing Terrain code remains
only as an oracle until T2.

## Goal

Implement deterministic normalized Terrain tile composition and five immutable
product codecs, integrate them with AssetBuildCore/DDC and AssetCore Cook region
packages, and prove source- and DDC-free loading of a complete tile generation.

## Scope

- New Terrain World authored values and normalized value-only build inputs.
- Height/source composition, border evidence, five product codecs, validation,
  independent build functions/keys, cancellation, and atomic generation.
- World manifests, 8×8 region package Cook, reachability, partial installation,
  checksums, compatibility inspection, and source-free load.
- Golden, malformed, determinism, cache, Cook, memory, and shutdown tests.

## Non-Goals

- Engine objects, renderer resources, collision-scene publication, queries,
  streaming, World Partition, programmable surfaces, or editor sculpting.
- Reading, converting, or preserving any legacy Terrain package or payload.
- Replacing old production Terrain code before T2 qualifies the new tile.

## Design Decisions and Invariants

- Implement the Runtime contract literally; format changes require updating T0
  rather than adding hidden interpretation in T1.
- Workers consume immutable copied values and never reflected objects or files.
- Cache hits and local builds pass the same codec and dependency validation.
- Five products build independently, but only a complete checked generation is
  publishable. Failure retains the prior generation.
- Authored intent, DDC products, cooked packages/bulk, and runtime values remain
  distinct. Runtime tests remove source and DDC access.
- Legacy magic/class/build keys reject before body decode or dependency lookup.

## Current Foundations and Gaps

AssetBuildCore provides versioned build functions, canonical XXH3-128 identities,
cache validation, cancellation terminals, and timing. AssetCore provides DAST,
cooked bulk, manifests, compatibility inspection, reachability, and atomic file
publication. GeometryBuild and AssetForgeBuiltins own current heightmap examples
but not new compatibility. Missing work is the new family of values, codecs,
builders, package directory, authored bridge, and tests.

## Implementation Stages

### Stage 0: Implement values and codecs

- [ ] Add world/tile/layer/generation identities, checked coordinate helpers,
  floor division, extents, and canonical encoders.
- [ ] Add authored definition validation and immutable normalized tile inputs.
- [ ] Implement `TWHT`, `TWMD`, `TWCV`, `TWCL`, and `TWQY` codecs with exact
  headers, limits, XXH3-128, byte order, and compatibility inspection.
- [ ] Add golden/asymmetric, round-trip, malformed, overflow, and legacy rejects.

#### Acceptance Gate

- Every contract vector passes; malformed/legacy data fails before publication
  with bounded allocation and one exact terminal.

### Stage 1: Implement composition and product builds

- [ ] Normalize ordered height and coverage sources without reflected-worker access.
- [ ] Implement border/halo evidence and bit-identical neighbor validation.
- [ ] Register five versioned build functions and deterministic keys.
- [ ] Implement independent cache validation, cancellation, supersession, and
  complete-generation publication retaining the prior generation on failure.
- [ ] Qualify cold/warm/reordered/parallel builds and the 768 MiB task ceiling.

#### Acceptance Gate

- Identical normalized input produces identical keys and bytes across scheduling;
  intended source/neighbor/layer/schema/platform changes invalidate only the
  declared products, and no partial generation publishes.

### Stage 2: Implement manifests and Cook

- [ ] Implement sorted world manifests and 8×8 signed region lookup.
- [ ] Cook independently addressable product bulk with exact ranges/checksums,
  selected-region reachability, package ceilings, and unload blockers.
- [ ] Validate DDC reuse/local fallback identically and reject corruption.
- [ ] Prove partial installation and source/DDC-free cooked loading.

#### Acceptance Gate

- F0 package counts and byte ceilings reconcile; manifest, corruption, missing
  region/product, compatibility, reachability, and unload cases are deterministic.

### Stage 3: Integrate authored build lifecycle and hand off T2

- [ ] Connect compact authored definitions to normalization/build/Cook through
  existing AssetForge and async-operation ownership.
- [ ] Add diagnostics and conservation for bytes, phases, origins, products,
  requests, cancellations, retained generations, and shutdown.
- [ ] Run the smallest registered targets plus required asset/Cook integration
  domains selected through `DevTool test list/explain`.
- [ ] Publish implemented contract details, update the roadmap, and create
  `Terrain Runtime Tile Reference` only after the gate passes.

#### Acceptance Gate

- One complete finite tile builds, caches, cooks, and loads without source/DDC;
  failure and shutdown conserve all terminals and bytes within T0 budgets.

## Validation Matrix

| Area | Evidence |
| --- | --- |
| Coordinates/codecs | Golden signs, boundaries, seams, byte determinism, round trip, malformed and one-past-limit rejection |
| Builds | cold/warm, worker/order determinism, invalidation matrix, cancel/supersede, prior-generation retention |
| Packages/Cook | manifest order, signed regions, reachability, partial install, range/hash corruption, unload blocking, source/DDC-free load |
| Compatibility | every old class, magic, key, and payload rejects before body decode |
| Budgets/lifecycle | product/package/peak bytes, exactly-once terminals, failure and shutdown conservation |
| Documentation | changed docs, all active plans, and roadmaps validate |

## Definition of Done

- All stage gates and Runtime-contract qualifications pass.
- The five product owners, build functions, DDC values, region packages,
  manifests, Cook, and runtime load have no legacy dependency.
- T0 budgets and failure/diagnostic vocabulary are enforced in code and tests.
- Lasting implementation contracts and the roadmap agree; T2 is activated only
  after a source-free complete tile is available.

## Deferred Follow-ups

- T2 owns runtime Engine/renderer/collision/query publication.
- T3 owns programmable surface evaluation.
- T4 owns multi-tile interest mapping and residency.
- T9 owns optional non-destructive authoring operations.

## Related Documentation

- [Terrain World Data](../Runtime/Terrain/TerrainWorldData.md)
- [Terrain World System Roadmap](../Roadmaps/TerrainWorldSystem.md)
- [Asset Data Lifecycle and Storage](../Runtime/Assets/AssetDataLifecycle.md)
- [Asset Packages](../Runtime/Assets/AssetPackages.md)
- [Agent Build and Run Workflow](../Agents/BuildAndRun.md)
- [Agent Testing Workflow](../Agents/Testing.md)

## Related Code

- `Engine/Source/Developer/AssetBuildCore`
- `Engine/Source/Developer/GeometryBuild`
- `Engine/Source/Editor/AssetForgeBuiltins`
- `Engine/Source/Runtime/AssetCore`
- `Engine/Source/Runtime/Engine/Public/Terrain`
