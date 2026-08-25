# DAST v6 Baseline Cutover Plan

Summary: Freeze and convert the complete tracked DAST corpus to v6, switch package policy, and remove v5 plus the temporary converter.

Last reviewed: 2026-08-25

Status: Archived
Completed: 2026-08-25

## Current Status

This completed M3 child of the
[Durin Binary Envelope and DAST v6 roadmap](../../../Roadmaps/Archive/2026-08/DurinBinaryEnvelopeAndDastV6.md)
converted the frozen corpus, made DAST v6 the sole production package route,
qualified Engine and Sandbox, and removed v5 plus the temporary converter.

## Goal

Convert the complete tracked authored package and compatibility-fixture
baseline to DAST v6 in one bounded transition, select v6 for all production
policy, qualify Engine and Sandbox, and remove every v5/converter authority.

## Scope

- Freeze tracked `.dasset`, `.dasset.hex`, and package-companion closure with
  source fingerprints immediately before conversion.
- Add the smallest exact offline v5-to-v6 converter and dry-run report.
- Switch supported reader, ordinary writer, mutations, redirectors, Cook, and
  baseline policy to v6.
- Convert every tracked valid v5 package and supported fixture, preserving
  corrupt/incompatible fixture intent under v6.
- Validate Engine/Sandbox baselines and affected package/Cook/companion flows.
- Delete the converter, v5 codec, trailer/footer authority, and obsolete tests.

## Non-Goals

- Long-lived dual-format support, runtime migration, editor background rewrite,
  DABK/DBLK conversion, payload schema changes, or unrelated asset edits.

## Design Decisions and Invariants

- The frozen manifest, source SHA-256, dry-run semantic report, and companion
  closure are checked before any replacement bytes are published.
- Conversion is offline, deterministic, success-atomic per file, and accepts
  only exact validated v5 input. Corrupt fixtures are regenerated deliberately
  from named v6 mutations rather than treated as convertible packages.
- The policy switch and corpus rewrite land with v5/converter removal; no
  intermediate dual baseline is a completed state.
- All ordinary writes, relocation, redirectors, canonical resave, Cook package
  construction, cache fingerprints, and audit tools select v6 after cutover.
- M3 closes only with zero residual legacy `DAST` v5 prefixes and zero reachable
  DTRL/DTRF or converter code.

## Current Foundations and Gaps

M2 provides exact conversion logic and full v6 capabilities. Remaining work is
the bounded temporary tool surface, fixture strategy, policy switch, binary
replacement, complete project qualification, and legacy deletion.

## Implementation Stages

### Stage 0: Freeze manifest and cutover evidence

- [x] Freeze every tracked package, fixture, companion closure, size, v5 prefix,
  and SHA-256 in a plan-owned manifest/report.
- [x] Verify no unrelated tracked binary edits overlap the cutover window.
- [x] Define valid-package conversion and each corrupt/incompatible fixture's
  named v6 regeneration strategy.
- [x] Capture Engine/Sandbox baseline, package, Cook, bulk, and documentation
  pre-cutover results.

#### Acceptance Gate

- The exact source corpus and replacement strategy are reviewable and no binary
  write begins without complete closure.

### Stage 1: Implement and prove the temporary converter

- [x] Add a bounded offline command that dry-runs exact v5-to-v6 conversion and
  reports source/destination hashes plus semantic equality.
- [x] Prove deterministic repeated conversion, refusal of non-v5/corrupt input,
  and package-plus-companion closure equality.
- [x] Switch production policy and ordinary write paths to v6 behind the same
  change set while retaining the temporary v5 input converter only offline.
- [x] Update or replace v5-specific tests with v6 contract coverage.

#### Acceptance Gate

- Dry-run covers the frozen valid corpus exactly and every production operation
  selects v6 without silently accepting legacy bytes.

### Stage 2: Convert packages and fixtures

- [x] Convert every valid tracked `.dasset` atomically and verify semantic,
  descriptor, source-path, and deterministic-byte equality.
- [x] Regenerate seven compatibility fixtures with preserved named intent under
  v6 and update fixture expectations.
- [x] Verify companions are unchanged and every manifest destination begins
  with `DURF`/DAST v6 with no DTRF footer.
- [x] Run Engine and Sandbox `asset baseline` validation.

#### Acceptance Gate

- The complete frozen corpus is v6, fixture intent is preserved, both project
  baselines pass, and no companion or unrelated binary changed.

### Stage 3: Remove legacy authority and qualify

- [x] Delete the temporary converter, v5 codec, DTRL/DTRF trailer/footer code,
  and obsolete v5-only tests/policy names.
- [x] Prove zero residual v5 prefix, converter symbol, DTRL/DTRF authority, and
  unsupported dual-format reader path.
- [x] Run Core, package, Cook, bulk, catalog/mutation, Engine, Sandbox, and
  required broad validation after legacy removal.
- [x] Update lasting package, lifecycle, versioning, serialization, and content
  version-control contracts to v6-only authority.

#### Acceptance Gate

- DAST v6 is the sole supported/emitted baseline and all affected validation
  passes without legacy or converter code.

### Stage 4: Close the roadmap

- [x] Record exact manifest, conversions, tests, project baselines, residual
  searches, and binary diff evidence in this plan.
- [x] Mark M3 and the parent roadmap completed and validate changed/all plans,
  roadmaps, and documentation.
- [x] Commit the isolated cutover with plan/stage provenance.

## Completion Evidence

- The manifest was frozen immediately after M2 commit `b9f995cf`: 27 valid v5
  packages, seven historical compatibility fixtures, and two DABK companions.
  No tracked binary had an overlapping pre-cutover edit.
- The temporary converter dry-ran all 27 packages, reported source and
  destination XXH3-128 identities, proved repeat-output equality and exact
  v6-to-logical-v5 semantic round trips, and refused a `.dabulk` non-v5 input.
  Atomic apply then converted the same 27-path manifest.
- All 27 tracked `.dasset` files now begin `DURF`. The seven fixtures were
  regenerated from a valid v6 package with named current, corrupt-magic,
  truncated, unknown-class, unknown-field, incompatible-signature, and
  invalid-object-graph mutations; fixture tests verify each intent.
- The two companions have no binary diff and retain SHA-256
  `fbfc0dbce4f7888b0e73f4e9ebd80df9b994d7e773ffe0fc0b246033bcfb64d2`
  and `0d7a7142d06ac2aefcd502d11f82e24fb1c8ac46ebdabe06101b91f244e7d7a3`.
- `DevTool asset baseline` reports 6 current v6 packages for Engine and 27 for
  Sandbox. Focused results are AssetPackage 122/122, CoreFileSystem 41/41,
  AssetCook 13/13, AssetBulkContainer 11/11, Material 84/84, Texture 86/86,
  and StaticMesh 74/74. The post-removal `fast-all` gate built and passed all
  61 selected contract, feature, and infrastructure targets. In the completion
  audit, 390 DevTool tests passed and two were intentionally skipped.
- Production/source searches find no v5 codec, package-trailer, DTRL/DTRF,
  converter, or `EncodeV5ObjectStream` symbol. The temporary converter target
  and source, v5 codec, trailer/footer implementation, and obsolete trailer
  tests were deleted before final validation.
- The completion audit also removed the remaining M2 transition labels from
  the production codec and tests. DevTool storage qualification now consumes
  protocol v3, treats DURF/DAST v6 plus DABK v1 as the retained current
  boundary across 27 packages, two payloads, and 2,359,296 reachable bytes,
  and leaves the frozen v2 protocol only as historical evidence.
- Lasting package, lifecycle, versioning, serialization, and content-version
  control contracts now define v6 as the sole supported/emitted authority.

## Validation Matrix

| Area | Required evidence |
| --- | --- |
| Manifest | Exact tracked files, companions, sizes, source hashes, no overlap |
| Conversion | Dry-run, deterministic bytes, semantic/descriptor equality, atomic replacement |
| Fixtures | Seven named v6 fixtures preserve current/corrupt/incompatible intent |
| Policy | v6-only reader/writer/mutation/Cook/cache; legacy fails closed |
| Projects | Engine and Sandbox deterministic asset baseline passes |
| Removal | No v5 codec/converter/DTRL/DTRF or residual v5 tracked bytes |
| Repository | Focused and broad registered tests plus documentation validators |

## Definition of Done

- Every tracked package/fixture is v6 and both project baselines pass.
- v6 is the sole supported reader and ordinary writer.
- Converter, v5 codec, trailer/footer authority, and obsolete coverage are gone.
- Lasting docs are authoritative and the roadmap is completed.

## Deferred Follow-ups

- DABK, DBLK, and standalone payload adoption remain separately triggered work.

## Related Documentation

- [Durin Binary Envelope and DAST v6](../../../Roadmaps/Archive/2026-08/DurinBinaryEnvelopeAndDastV6.md)
- [DAST v6 Package Codec](DastV6PackageCodec.md)
- [Asset Packages](../../../Runtime/Assets/AssetPackages.md)
- [Asset Data Lifecycle](../../../Runtime/Assets/AssetDataLifecycle.md)
- [Versioning](../../../Runtime/Assets/Versioning.md)
- [Content Version Control](../../../Development/VersionControl/ContentVersionControl.md)

## Related Code

- `Engine/Source/Runtime/AssetCore/Private/AssetPackageCodec.cpp`
- `Engine/Source/Runtime/AssetCore/Private/AssetPackageV6Codec.cpp`
- `Engine/Tests/Native/AssetCoreTests/Data/Compatibility/`
- `Engine/Content/`
- `Sandbox/Content/`
