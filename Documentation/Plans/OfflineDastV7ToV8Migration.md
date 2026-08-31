# Offline DAST v7-to-v8 Migration Plan

Summary: Build a bounded construct-free v7 converter, report every decision deterministically, and migrate the maintained authored corpus to canonical DAST v8.

Last reviewed: 2026-08-31

Status: Completed
Completed: 2026-08-31

## Current Status

All stages are complete. The bounded converter, deterministic plan/apply API,
offline command, rollback, stale admission, and report contract are covered by
focused tests. All 25 maintained packages are canonical v8; the eight external
companions retained exact bytes and hashes; repeated dry-runs are byte-identical
no-ops. CoreDObject, AssetRegistry, AssetMaintenance, DurinAssetTool, and Engine
consumers compile without executing Engine. P5 now owns live linker application,
the atomic Engine writer/reader cutover, and removal of the remaining v7 live path.

Post-completion cleanup on 2026-08-31 revalidated the complete v8 corpus and
removed the temporary converter, command route, AssetMaintenance orchestration,
and focused v7 fixtures. The implementation details below remain historical
evidence for the completed one-time transition.

## Maintained Corpus Migration Report

Applied 2026-08-31 with report schema 1. All 25 records reached `Converted`;
there were no blockers or recovery files. Eight external bulk companions retained
their exact byte extents and hashes. Two subsequent read-only reports were
byte-identical, contained 25 `AlreadyV8` records and no `Ready` records, and had
SHA-256 `16a659ecfb989238369668c6b38d5d10fbdc826bb769b4b00f03d6d59c1129ba`.

| Package | Source main XXH3-128 | Target main XXH3-128 | External bulk bytes / XXH3-128 |
| --- | --- | --- | --- |
| `/Engine/Materials/DefaultMaterial` | `138e9e6d52fc116477d24dc898f4def0` | `3e2e82e359e3a343a6209c32573677ad` | — |
| `/Engine/Materials/ImportedSurface` | `11fd53c40cebb94caca2fa01b4623fe2` | `0cbee46b745d5ab6c6d29e267714287f` | — |
| `/Engine/Models/Box` | `e9649f2f13d0f947014c30149aec1eeb` | `2fa01de682a271a4864a7575418b1281` | — |
| `/Engine/Models/Sphere` | `b0d61314a2acd8e7040f8e7ed8bd9844` | `200320b1227a00dda85aa010a46c3710` | — |
| `/Engine/Models/SplineBox` | `7792f46ca73c6b1d1c791f60cb5ce96e` | `e11edc3cb1f0e4a8367f5f8f270ba0ce` | — |
| `/Engine/Renderer/DefaultStudioEnvironment` | `cf99f75d09b1414098938ac4eace6feb` | `54686cbaa011a76d62f880876fc1fe42` | — |
| `/Game/Characters/RiggedSimple/Animations/Animation_0` | `9c5e7e36960a335c7ce75ac777c63a7f` | `34d31944af1f9924e27c8e24aa932386` | — |
| `/Game/Characters/RiggedSimple/Materials/Material_001_effect` | `5937ac441f1c259ba883907ea0f06fac` | `626c9b78d6b0c52ce827bce15c7919f6` | — |
| `/Game/Characters/RiggedSimple/Meshes/RiggedSimple` | `bc23ca48d958959187dc954acae3e9bc` | `481c7187659d6ee934fe485233fc8437` | — |
| `/Game/Characters/RiggedSimple/SkeletalMeshes/Cylinder` | `e9f40deae106814e20d5de4f13464c65` | `78a40a596d4e45dcc2eea7f7cf75941d` | — |
| `/Game/Characters/RiggedSimple/Skeletons/Armature` | `8c8da673c2aafda01cc617be43ed9269` | `3673b3b07199948a3871a18944fb2654` | — |
| `/Game/Levels/GrayboxStage15` | `278ea1b48834709150138721dc87e5f5` | `576d76fd2ac6bcd2028d6e4fb6badf32` | — |
| `/Game/Models/GrayboxPawn` | `05cbc6b658b4bb82569c3e60b2e985d0` | `98bb0ac1a891bf657482b49b860105a6` | — |
| `/Game/Models/VintageLighter/Materials/vintage_lighter` | `baa70fbda917fda720c50a9cf848b8ae` | `5f55d64a6be8d8b354f240a179629735` | — |
| `/Game/Models/VintageLighter/Materials/vintage_lighter_alpha` | `8e816f9b50d7b14677fa692051de35e1` | `8ff4fa0dddf1dee0c096f8b0a75960d9` | — |
| `/Game/Models/VintageLighter/Meshes/vintage_lighter_1k` | `cd323525438636241850bfb0d0c0ac39` | `813bd2470b27f4bee7e4cb3fccfcd31c` | — |
| `/Game/Models/VintageLighter/Textures/vintage_lighter_diff_BaseColor` | `4a6476ab49f623f2e9fabf8fc486cedd` | `bbde35958f5d89dcffb1cd1390a3cfa2` | `4194304 / a2a5b0f70a84a1963ce049ec1d31a90d` |
| `/Game/Models/VintageLighter/Textures/vintage_lighter_diff_Opacity` | `aa8405ad5483d2389e638fdce4eccd58` | `36ed05531c8a5cced9124411d18e7efb` | `4194304 / a7fa2fde863813638bbf51c6fa494e2f` |
| `/Game/Models/VintageLighter/Textures/vintage_lighter_metal_vintage_lighter_rough_Metallic` | `2a93d258533a311429994d223c26a42a` | `1cef9c7e86a9cc49f42763f36628b597` | `4194304 / 29ea02cbcc6f386914bc4cfc12c90863` |
| `/Game/Models/VintageLighter/Textures/vintage_lighter_metal_vintage_lighter_rough_Roughness` | `8541e7d8037a2f8cf06668fb63d01fc4` | `6a4fc987c7e4cc03ea845ee40fac1647` | `4194304 / 3cc72c9163546052a100c2daa240f69f` |
| `/Game/Models/VintageLighter/Textures/vintage_lighter_nor_gl_Normal` | `c2bc7f20244ce182b41a5a76fc5b18bc` | `79114b74c8289b54e62dc86a8b355f9c` | `4194304 / d7e919cfc6160175c88ff948ce118662` |
| `/Game/Textures/TEXCUBE_PureSky_512x512` | `a3be22fb0eaeebad91660477946a4d23` | `8718b0250fbf0a9ae115858596da875c` | `6291456 / fa0b83bfc6bc74b61b4065ff87c7641f` |
| `/Game/Textures/TEX_StoneHead` | `fe44ce6af797dd5d117b0c30779b47bb` | `4d7977c58183390099e805dcf195cae1` | `1048576 / 893fb8249da8e173c1313405c27a69b8` |
| `/Game/Volumes/VolumetricCloud/VT_Cloud_Base_Voronoi_128` | `8cf1ed4bda537cb6e58deff804ae51bd` | `b7dbaa28cc3d74c2b699080d9dfc3db4` | `2097152 / 082ef5bec38851fbdeb74b1390e2a348` |
| `/Game/Volumes/VolumetricCloud/VT_Cloud_Detail_Voronoi_64` | `326911b7ea3a87f16993d8123aa6c802` | `828a8101a503a8228fd727528927e52c` | — |

## Frozen Conversion and Corpus Contract

- Maintained corpus is exactly 25 tracked `.dasset` files (six under
  `Engine/Content`, 19 under `Sandbox/Content`) and the eight tracked `.dbulk`
  companions bound by those packages. Generated, test, backup, recovery, `Saved`,
  `Build`, and `Binaries` trees are excluded.
- v7 scalar, enum, intrinsic, struct, fixed/variable array, map, hard/soft reference,
  bytes, BulkData, custom-version, import/export, redirect, provenance, and topology
  facts map to the corresponding detached v8 linker facts. An unmaterialized struct
  descriptor may remain schema-less; a materialized struct value may not.
- v7 inline/external BulkData retains payload bytes and placement. External alignment
  is retained; inline alignment is one; migrated element size is one byte because v7
  carries no element-size fact. Descriptor identity, per-value hash, range, overlap,
  padding, and complete segment binding are validated before output exists.
- Retained unknown payloads, unknown provenance, invalid topology/table/value data,
  malformed or ambiguous BulkData, unsupported versions, identity mismatch, limits,
  truncation, non-canonical v8 output, and stale source closures are terminal. Output
  vectors and source files remain unchanged; publication failure restores both main
  and companion snapshots or reports `RecoveryRequired`.
- Reports use schema version 1, sort by virtual package path, and contain source and
  target main/bulk extent plus XXH3-128 hash, stable status, diagnostic code, and
  diagnostic. Plan is read-only; apply revalidates exact closure hashes, reconverts,
  compares planned target hashes, publishes with rollback, and rereads/re-emits v8.
- CLI selection is exactly one of `--all` or one-or-more canonical package/folder
  scopes. Cancellation is checked during discovery, planning, and before each apply
  admission. `migrate-v8` exits before `DObjectInit`, reflection/module loading,
  editor services, Engine loop, Cook, or GPU initialization.

## Goal

Provide one offline, bounded, non-constructing conversion route from a validated
v7 main package plus optional raw segment to canonical v8 main/bulk outputs.
Expose it through deterministic AssetMaintenance planning/reporting and a narrow
`DurinAssetTool migrate-v8` command, then convert every maintained authored
package. Unsupported retained values, custom payload ambiguity, malformed bulk
descriptors, stale inputs, or any failed verification leave the source closure
unchanged and appear explicitly in the report.

## Scope

- Complete v7 envelope/object-stream decode and existing v7-to-linker adaptation
  without `DObject` construction, dependency loading, `PostLoad`, or reflection
  mutation.
- Exact translation of v7 inline/external BulkData descriptors into v8 value
  bytes, element/alignment policy, placement, and raw-segment binding.
- Deterministic per-package plan/apply records with source/target hashes,
  terminal status, stable diagnostics, and atomic `.dasset`/`.dbulk` publication.
- Explicit mounted package selection, stale-input rejection, cancellation, dry
  run, and full post-write v8 read/re-emit verification.
- Migration of version-controlled `.dasset`/`.dbulk` files under
  `Engine/Content` and `Sandbox/Content`; generated `Saved`, Build, Binaries,
  test-work, backup, and recovery files are not maintained corpus.
- Focused native tests and compile-only tool/runtime consumers; no Engine,
  editor, game, Cook, or application-hosted execution.

## Non-Goals

- Applying v8 linker exports to live objects or making Engine load v8; P5 owns
  that cutover immediately after corpus migration.
- Preserving retained unknown values, unsupported custom serializers, ambiguous
  searchable-name inference, compatibility evidence, or deprecated migration
  routes that cannot be represented exactly in the linker model.
- In-place best-effort edits, partial package closure publication, permanent
  dual-format authoring, or conversion of untracked/generated outputs.
- Keeping the migration command as a permanent general version graph after the
  v7 baseline is retired.

## Program Decisions

- The converter accepts exact virtual package identity and complete detached
  main/bulk byte spans. It validates v7 completely, adapts into
  `ObjectPackage::FLinkerTables`, resolves every BulkData payload by checked
  descriptor bounds and digest, and calls only CoreDObject's v8 writer.
- A v7 BulkData field is preserved as raw bytes. Inline placement stays inline;
  external placement stays external with its validated power-of-two alignment.
  The migration element-size policy is byte-addressed (`1`) because v7 did not
  serialize element size; P5 reflection application remains authoritative for
  typed runtime interpretation.
- The converter publishes outputs only after `ReadPackageV8`, canonical
  write-read-write identity, exact main/bulk digests, and package-identity
  validation all succeed.
- AssetMaintenance snapshots source main/bulk closures before planning. Apply
  rechecks exact content hashes and writes one package closure atomically with
  rollback on any stage or verification failure.
- Reports sort by virtual package path and use stable enum names/diagnostics.
  Dry-run and apply serialize the same plan facts; apply adds terminal output
  hashes and changed paths.
- P4 removes ordinary writer selection for v7 only where the offline capability
  can replace it without live application. Engine's current live writer/reader
  implementation is removed during P5's atomic cutover, not prematurely while
  Engine cannot yet apply v8 exports.

## Implementation Stages

### Stage 0: Freeze conversion and corpus contracts

- [x] Inventory v7 decoded features, retained/custom failure classes, authored
  bulk descriptor bytes, companion naming, and all tracked corpus versions.
- [x] Freeze conversion input/output, byte-addressed BulkData policy, limits,
  diagnostic taxonomy, atomicity, and canonical verification requirements.
- [x] Freeze deterministic report schema, selection grammar, dry-run/apply
  semantics, stale checks, cancellation, and maintained-corpus definition.
- [x] Add synthetic supported/unsupported fixtures before file orchestration.

#### Acceptance Gate

Every representable v7 fact has one v8 mapping, every ambiguity has a terminal
failure, and the maintained corpus boundary is reviewable before files change.

### Stage 1: Implement the bounded construct-free converter

- [x] Add a temporary AssetRegistry legacy-conversion capability that validates
  v7 main/bulk bytes and adapts them into detached linker tables atomically.
- [x] Decode and verify inline/external BulkData descriptors, payload ranges,
  per-value hashes, alignment, whole-segment binding, and duplicate/overlap rules.
- [x] Emit v8 only through CoreDObject and require complete read/re-emit identity
  before returning detached outputs.
- [x] Cover scalar/container/struct/reference/custom-version topology, redirects,
  inline/external bulk, limits, truncation, retained values, and output atomicity.

#### Acceptance Gate

Supported v7 bytes convert deterministically without objects; malformed or
unsupported inputs leave sentinel outputs unchanged with stable diagnostics.

### Stage 2: Add deterministic offline orchestration

- [x] Add AssetMaintenance migration plan/apply records and deterministic JSON
  serialization using mounted package snapshots and exact closure fingerprints.
- [x] Implement atomic main/bulk staging, stale revalidation, cancellation,
  rollback, post-write v8 verification, and failure injection tests.
- [x] Add `DurinAssetTool migrate-v8 --project=... [--all|scope...] [--apply]
  [--json]` and the matching DurinDevTool route without initializing Engine.
- [x] Prove dry-run is read-only, report ordering is enumeration-independent,
  and partial/failed apply never corrupts a package closure.

#### Acceptance Gate

One narrow offline command can audit or atomically convert selected v7 packages
with deterministic, machine-readable evidence and no live object lifecycle.

### Stage 3: Migrate and verify the maintained corpus

- [x] Run a dry-run over `Engine/Content` and `Sandbox/Content`, resolve or
  explicitly fail every package, and review the deterministic report.
- [x] Apply conversion to every supported tracked package and companion, keeping
  exact before/after hashes and terminal status in a checked-in migration report.
- [x] Prove every maintained `.dasset` is canonical v8, every external `.dbulk`
  matches its v8 binding, and no untracked backup/temporary file remains.
- [x] Prove a second dry-run is a deterministic no-op and repository searches
  find no maintained v7 package bytes.

#### Acceptance Gate

The complete maintained authored corpus is canonical v8, failures are absent or
explicitly dispositioned, and rerunning migration changes no bytes.

### Stage 4: Publish P4 and hand off the cutover

- [x] Compile CoreDObject, AssetRegistry, AssetMaintenance, DurinAssetTool, and
  Engine consumers without executing an Engine binary.
- [x] Run focused converter, migration orchestration, canonical v8, Registry,
  malformed-input, and corpus-audit tests.
- [x] Publish the temporary offline capability, report, corpus baseline, and P5
  handoff in owning Runtime/Development/Workspace documentation.
- [x] Complete the plan, update P4, pass documentation validation, and record
  exact plan/stage provenance in the implementation commit.

#### Acceptance Gate

P4 is reproducible from checked-in evidence, all maintained assets are v8, and
P5 can remove the remaining Engine v7 live path without another corpus rewrite.

## Validation Matrix

| Concern | Required evidence |
| --- | --- |
| Decode boundary | Complete v7 main/bulk validation is bounded and construct-free. |
| Value parity | Scalars, structs, containers, hard/soft references, maps, provenance, and versions match linker facts. |
| BulkData | Inline/external payload bytes, digests, alignment, bounds, and v8 segment binding match exactly. |
| Failure | Retained/ambiguous/custom/malformed inputs return stable terminal diagnostics and unchanged outputs/files. |
| Determinism | Repeated conversion, reports, and post-migration dry runs are byte-identical. |
| Atomicity | Stale inputs and injected stage/verify failures restore the complete source closure. |
| Corpus | Every tracked authored package is v8; generated and recovery trees remain outside scope. |
| Runtime boundary | No `DObject`, dependency load, `PostLoad`, Engine loop, editor, Cook, or GPU execution occurs. |

## Related Code and Documentation

- Retired historical path:
  `Engine/Source/Runtime/AssetRegistry/Private/PackageLinkerV7Adapter.h`
- [CoreDObject package format](../../Engine/Source/Runtime/CoreDObject/Public/DObject/PackageFormat.h)
- [AssetMaintenance](../../Engine/Source/Developer/AssetMaintenance)
- [DurinAssetTool](../../Engine/Source/Programs/DurinAssetTool)
- [Asset Packages](../Runtime/Assets/AssetPackages.md)
- [Asset Catalog and Mutation](../Runtime/Assets/AssetCatalogAndMutation.md)
- [Core Object Package Linker roadmap](../Roadmaps/CoreObjectPackageLinker.md)
- [Build and Run Workflow](../Agents/BuildAndRun.md)
- [Testing Workflow](../Agents/Testing.md)
