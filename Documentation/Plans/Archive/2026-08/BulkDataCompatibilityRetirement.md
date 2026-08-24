# BulkData Compatibility Retirement Plan

Summary: Upgrade the repository asset corpus and retire proven-unused authored BulkData, Archive residency, VolumeTexture migration, and reflection-alias compatibility paths

Last reviewed: 2026-08-22

Status: Archived
Completed: 2026-08-22

## Current Status

Stage 0 is complete. The repository corpus and call graph establish a bounded
retirement set. The Sandbox project audit covers all 29 tracked `/Engine` and
`/Game` packages: every package is compatible current DAST v4, no package has
deprecated-route evidence, and no tracked package is a VolumeTexture. Two
ImportRecord packages retain canonicalization evidence for the retired
`Durin::Asset::Import` namespace and must be resaved before those aliases are
removed. A complete production-source inventory finds exactly seven
`LegacyNames` declarations, all in `ImportRecord.h`; this plan retires all
seven. Generic reflection support and owner-scoped property-alias tests remain
because they are infrastructure, not a repository asset compatibility route.

Production authored package loading is eager: the V4 reader resolves and
verifies DABK bytes before publishing the object graph. Consequently
`FAuthoredBulkData::SetUnloaded`, its callback provider, and the facade's
synchronous-load API have no production caller.

Stage 1 is complete. The two ImportRecord packages were individually planned
and atomically resaved; their second plans report zero ready packages. SHA-256
changed from `00C34C...B2763` to `7FC6DA...1658E` and from
`84CC9B...B974` to `8D1F42...C1C8`. All seven production `LegacyNames`
declarations and the concrete migration-only test are removed.
`AssetImportCoreTests` passes 30/30 with rebuilt reflection metadata, and the
post-removal audit reports 29/29 compatible current DAST v4 packages with zero
canonicalization and deprecated-route evidence.

Stage 2 is complete. `FAuthoredBulkData` now exposes runtime residency, failure,
loading, and resident bytes only through `GetBulkData()`; its unused callback
provider and synthetic unloaded seam are gone. Archive bulk transfers now carry
only identity, placement, hashes, sizes, and verified immutable bytes. Focused
CoreUtilityTests (80/80), AssetPackageTests (107/107), and TextureTests (80/80)
pass, and a source inventory finds no retired Archive residency or authored
callback/facade names.

Stage 3 is complete. VolumeTexture now has only the current authored BulkData
voxel schema: the source custom version, deprecated byte Array/Blob fields,
post-deserialize migration trait, and two migration-only tests are removed.
TextureTests passes 78/78, TextureCookIntegrationTests passes 1/1, generated
and production source contain none of the retired symbols, and the corpus remains
29/29 compatible with a 29-package current DAST v4 baseline. Stage 4 is next:
document and qualify the supported baseline.

Stage 4 is complete. Lasting serialization, reflection, asset package/lifecycle,
VolumeTexture, and roadmap documentation now describe the current-only schema
and single runtime BulkData API. Focused Core, reflection, package, import,
texture, and Cook targets pass; the full native aggregate and Win64 Debug Editor
`all` build pass. Final asset audit/baseline remains 29/29 compatible and 29
current DAST v4 packages. Documentation lifecycle validation and final diff
checks complete the plan.

## Goal

Make the landed BulkData architecture honest and minimal: repository assets use
only current identities, authored bulk values expose the common `FBulkData`
read/residency surface, Archive transfers carry serialization data rather than
a duplicate runtime state machine, and VolumeTexture no longer retains two
historical voxel schemas that the tracked corpus does not contain.

## Scope

- Canonically resave the two tracked ImportRecord packages that still encode
  `Durin::Asset::Import` identities, then require a zero-evidence corpus audit.
- Remove all seven production `LegacyNames` declarations, which are the
  corpus-cleared ImportRecord entries for both retired namespaces, and delete
  tests whose only purpose was those concrete aliases.
- Remove the unused authored callback loader, `SetUnloaded`, private callback
  provider, and synthetic unloaded-facade test.
- Migrate VolumeTexture source access and the bulk property inspector to
  `FAuthoredBulkData::GetBulkData()` and remove redundant facade read/residency
  pass-through methods.
- Remove Archive-level bulk residency/failure fields and
  `EArchiveBulkDataResidency`; Archive writers/readers continue to accept and
  publish only complete verified byte candidates.
- Remove `FVolumeTextureSourceVersion`, historical Array/Blob fields,
  `PostDeserialize` migration, and their compatibility tests after the corpus
  gate proves no tracked package depends on them.
- Update lasting reflection, asset-package, lifecycle, and volume contracts.

## Non-Goals

- Changing DAST v4, DABK v1, DBLK, TXPL, payload ids, hashes, Cook manifests,
  DDC keys, or package publication ordering.
- Removing `FAuthoredBulkData` itself; it remains the reflected authored value
  and owns replacement plus DAST/DABK storage policy around `FBulkData`.
- Removing `FAuthoredBulkDataDescriptor`; placement and container identity are
  still required by authored package storage and inspection.
- Removing `FCookedPackagePayload`, `LoadCookedPackagePayload`, DBLK container
  decode/resolve APIs, or migrating the remaining cooked consumers. Seven
  production consumers remain, and StaticMesh depends on selecting multiple
  payloads from one decoded container.
- Adding deferred authored IO. A future real DABK provider must be designed from
  package lifetime and transaction requirements rather than preserving the
  unused callback seam.
- Preserving load compatibility for external VolumeTexture packages that still
  contain the retired Array/Blob voxel fields or for external packages using the
  removed ImportRecord names. This cleanup intentionally advances the supported
  baseline to the audited repository corpus.
- Broad removal of unrelated deprecated properties, aliases, package fixtures,
  or compatibility infrastructure without matching corpus evidence.
- Removing generic reflection `LegacyNames` capability or owner-scoped property
  alias tests; future bounded schema evolution still requires that mechanism.

## Design Decisions and Invariants

- Asset mutation precedes reader removal. The canonical-resave apply must
  publish and verify the two ImportRecord packages atomically; only a subsequent
  audit with zero canonicalization/deprecated evidence authorizes alias removal.
- `Sandbox/Sandbox.dproject` is the corpus authority because its mount set
  includes the six tracked Engine packages and 23 tracked Game packages. The
  standalone Engine descriptor does not provide a runnable audit context and is
  not treated as a second incomplete corpus.
- Checked-in package diffs are reviewed by exact path and size/hash evidence.
  No source import, reimport, derived-data rebuild, or Cook runs during the
  canonical resave.
- `FBulkData` exclusively owns runtime `Unloaded`, `Resident`, and `Failed`
  state. `FArchiveBulkDataTransfer` contains logical identity, storage metadata,
  and an immutable buffer only; saving rejects an absent/unverified buffer and
  loading commits only verified resident bytes.
- Authored V4 loading remains eager and transactional. Removing the callback
  seam does not turn external DABK access into inline storage and does not alter
  when package publication occurs.
- `FAuthoredBulkData` retains only authored capabilities and inspection:
  construction/replacement, `GetDescriptor`, `GetBulkData`, `Serialize`, and
  `Identical`. Consumers read residency/failure/bytes from `GetBulkData()`.
- Removing the VolumeTexture custom version and deprecated members does not
  change current package bytes because deprecated properties are absent from
  current schemas and no tracked VolumeTexture package exists. Historical test
  packages become explicitly unsupported rather than silently misdecoded.
- ImportRecord current qualified names and package values do not change; only
  stored legacy names are rewritten and then removed from reflection metadata.
- Current authored/cooked corruption, bounds, hash, and last-known-good behavior
  remains mandatory. Cleanup may reduce branches but may not weaken validation.

## Current Foundations and Gaps

| Area | Evidence | Cleanup |
| --- | --- | --- |
| Tracked corpus | Sandbox audit finds 29 compatible current DAST v4 packages and zero deprecated-route evidence. | Resave two ImportRecords with namespace alias evidence; require zero evidence afterward. |
| Authored residency | V4 reader eagerly calls `LoadAuthoredBulkPayload`; `SetUnloaded` has no production caller. | Remove callback/provider seam and facade load pass-through. |
| Archive transfer | All production save/load paths require or produce verified resident bytes. | Remove duplicate Archive residency/failure state. |
| Volume compatibility | Only synthetic tests populate deprecated Array/Blob fields; no tracked VolumeTexture package exists. | Remove custom version, fields, migration trait, and tests. |
| Import aliases | Two tracked packages use `Durin::Asset::Import`; no corpus evidence uses `Durin::AssetImport`. | Resave packages, then remove both alias sets from ImportRecord reflection declarations. |
| Cooked compatibility | Seven Engine consumers still use `FCookedPackagePayload`; StaticMesh selects multiple entries from one container. | Retain; a later shared-container provider plan owns migration. |

## Implementation Stages

### Stage 0: Define the implementation boundary

- [x] Audit the tracked Engine/Game package corpus through the Sandbox project.
- [x] Identify canonicalization and deprecated-route evidence by exact package.
- [x] Inventory every production `LegacyNames` declaration and distinguish the
  seven concrete ImportRecord aliases from generic reflection infrastructure.
- [x] Prove the authored callback loader and `GetBulkData` call graph.
- [x] Classify VolumeTexture historical fields as corpus-retirable rather than
  current migration debt.
- [x] Prove cooked compatibility APIs still have production consumers and
  exclude them from this cleanup.

#### Acceptance Gate

- The mutation order, supported baseline, exact retirement set, retained cooked
  boundary, failure semantics, and validation gates are explicit.

### Stage 1: Upgrade tracked assets and retire cleared reflection aliases

Depends on Stage 0.

- [x] Capture pre-resave hashes and audit evidence for the two ImportRecord
  packages.
- [x] Apply canonical resave only to
  `/Game/Characters/RiggedSimple/RiggedSimple_Import` and
  `/Game/Models/VintageLighter/vintage_lighter_1k_Import`.
- [x] Review exact package diffs, rerun audit/baseline, and prove zero
  canonicalization and deprecated-route evidence across all 29 packages.
- [x] Remove cleared ImportRecord `LegacyNames` declarations and update focused
  reflection/import compatibility tests.
- [x] Re-run the corpus audit after rebuilding metadata and prove all tracked
  packages remain current and compatible without the aliases.

#### Acceptance Gate

- Only the two authorized packages change; a second resave is a no-op; all 29
  packages load under current names with zero alias/deprecated evidence after
  the legacy registrations are removed.

### Stage 2: Collapse authored and Archive compatibility surfaces

Depends on Stage 1.

- [x] Remove `FLoadFunction`, `SetUnloaded`, the callback-backed authored
  provider, and their synthetic test.
- [x] Migrate VolumeTexture and editor inspection to `GetBulkData()` and remove
  redundant authored read/residency/load pass-through methods.
- [x] Remove `EArchiveBulkDataResidency` plus transfer `Residency`/`Failure`
  fields; simplify Core and AssetCore readers/writers around verified buffers.
- [x] Preserve authored replacement, copy sharing, semantic equality, inline and
  external save/load, corruption failure, relocation, deletion, recovery, and
  reimport behavior.
- [x] Add compile-time/call-graph guards or focused tests that prevent the
  retired names and methods from returning unnoticed.

#### Acceptance Gate

- There is one runtime bulk residency model, no authored callback-loader seam,
  current DAST/DABK bytes remain stable, and focused Core/AssetCore/editor
  consumers compile and pass.

### Stage 3: Retire historical VolumeTexture voxel schemas

Depends on Stages 1-2.

- [x] Remove the VolumeTexture source custom version, deprecated byte Array and
  Blob fields, and `PostDeserialize` conversion trait.
- [x] Remove historical migration-only tests and retain current BulkData source,
  import/reimport, 128-cubed, DDC, Cook, and runtime coverage.
- [x] Confirm DHT no longer emits the deprecated fields/custom version and
  current VolumeTexture package bytes remain deterministic in synthetic
  save/reload tests.
- [x] Re-run the complete tracked corpus baseline with the compatibility code
  absent.

#### Acceptance Gate

- Current volume authoring/cook workflows are green, no production or generated
  source names the retired schemas, and all tracked assets remain current.

### Stage 4: Document and qualify the supported baseline

Depends on Stages 1-3.

- [x] Update reflection, serialization, asset package/lifecycle, VolumeTexture,
  and roadmap documentation to state the new baseline and retained boundaries.
- [x] Run focused Core, CoreDObject, AssetCore, AssetImport, and texture/cook
  tests using repository guidance.
- [x] Run the full native aggregate and Win64 Debug Editor `all` build.
- [x] Run final corpus audit/baseline plus changed/all documentation, plan, and
  roadmap validation.
- [x] Verify the final diff contains only the two authorized asset rewrites,
  bounded source/test cleanup, and lasting documentation updates.

#### Acceptance Gate

- All validation is green, the audit has zero compatibility debt, current wires
  are unchanged, and the repository no longer advertises or tests retired
  compatibility paths.

## Validation Matrix

| Concern | Required evidence |
| --- | --- |
| Mutation safety | Pre/post hashes, exact two-package selection, atomic canonical-resave reports, second-run no-op, and reviewed binary diffs. |
| Corpus baseline | Sandbox audit/baseline covers 29 Engine/Game packages with zero incompatible, unsupported, failed, stale, canonicalization, or deprecated-route evidence. |
| Reflection aliases | DHT/reflection and import tests resolve only current ImportRecord names; removed names have no registration or fixture. |
| Unified API | Production authored consumers read through `GetBulkData`; retired facade methods and callback types have zero source/generated references. |
| Archive | Inline/Skip/External policy and verified transfer tests pass without a second residency enum or failure string. |
| Authored storage | DAST/DABK deterministic bytes, threshold placement, corruption, move/delete/recovery, and reimport remain green. |
| Volume baseline | Current BulkData source/import/build/DDC/Cook/runtime tests pass; historical Array/Blob symbols are absent. |
| Cooked boundary | Existing cooked consumers and low-level DBLK tests remain unchanged and green. |
| Aggregate | Focused targets, full native aggregate, Debug Editor build, corpus gates, and documentation lifecycle validation pass. |

## Definition of Done

- The two tracked ImportRecord packages use current qualified identities and no
  repository package requires a removed alias or deprecated route.
- Authored BulkData has no unused callback provider or duplicate facade
  residency/load API; `FBulkData` is the sole runtime state owner.
- Archive bulk transfer has no runtime residency/failure state and retains exact
  verified serialization behavior.
- VolumeTexture supports only the current authored BulkData source schema.
- Cooked compatibility APIs with real consumers remain intact and documented as
  deferred rather than mislabeled dead code.
- All focused, aggregate, build, asset-corpus, and documentation gates pass.

## Deferred Follow-ups

- A real deferred authored DABK provider tied to package lifetime, cancellation,
  and memory budgets.
- A shared cooked-container provider/cache that can migrate StaticMesh and other
  remaining consumers without decoding one DBLK multiple times.
- Removal of `FCookedPackagePayload` and low-level public cooked access only
  after all production callers use that shared provider.
- Any broader legacy-property or reflection-alias retirement backed by a fresh
  repository corpus audit and separately reviewed asset mutation set.

## Related Documentation

- [Unified BulkData API](UnifiedBulkDataAPI.md)
- [Large Asset Payload Architecture Roadmap](../../../Roadmaps/Archive/2026-08/LargeAssetPayloadArchitecture.md)
- [Asset Packages](../../../Runtime/Assets/AssetPackages.md)
- [Asset Data Lifecycle and Storage](../../../Runtime/Assets/AssetDataLifecycle.md)
- [Volume Textures](../../../Runtime/Assets/VolumeTextures.md)
- [Generated Reflection System](../../../Runtime/Core/ReflectionSystem.md)
- [Serialization](../../../Runtime/Core/Serialization.md)
- [Canonical Resave](../../../Editor/Guides/CanonicalResave.md)
- [Testing](../../../Agents/Testing.md)
- [Build and Run](../../../Agents/BuildAndRun.md)

## Related Code

- `Engine/Source/Runtime/Core/Public/Serialization/Archive.h`
- `Engine/Source/Runtime/Core/Private/Serialization/Archive.cpp`
- `Engine/Source/Runtime/AssetCore/Public/Asset/BulkData.h`
- `Engine/Source/Runtime/AssetCore/Public/Asset/AuthoredBulkData.h`
- `Engine/Source/Runtime/AssetCore/Private/AuthoredBulkData.cpp`
- `Engine/Source/Runtime/AssetCore/Private/AssetPackageV4ArchiveAdapter.cpp`
- `Engine/Source/Editor/AssetImportCore/Public/ImportRecord.h`
- `Engine/Source/Editor/DurinEd/Private/Editor/PropertyView.cpp`
- `Engine/Source/Runtime/Engine/Public/Texture/VolumeTexture.h`
- `Engine/Source/Runtime/Engine/Private/Texture/VolumeTexture.cpp`
- `Engine/Tests/Native/CoreTests/Private/ArchiveTests.cpp`
- `Engine/Tests/Native/AssetCoreTests/Private/PackageTests.cpp`
- `Engine/Tests/Native/AssetCoreTests/Private/BulkDataTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/Texture/TextureBuildTests.cpp`
- `Sandbox/Content/Characters/RiggedSimple/RiggedSimple_Import.dasset`
- `Sandbox/Content/Models/VintageLighter/vintage_lighter_1k_Import.dasset`
