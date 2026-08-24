# DAST v6 Baseline Cutover Plan

Summary: Freeze and convert the complete tracked DAST corpus to v6, switch package policy, and remove v5 plus the temporary converter.

Last reviewed: 2026-08-25

Status: Active
Completed:

## Current Status

This is the active M3 child of the
[Durin Binary Envelope and DAST v6 roadmap](../Roadmaps/DurinBinaryEnvelopeAndDastV6.md).
M1 and M2 are complete. DAST v6 has full detached capability parity, but v5 is
still the supported repository baseline and ordinary writer. Stage 0 must
regenerate and freeze the exact Git manifest before any binary conversion.

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

- [ ] Freeze every tracked package, fixture, companion closure, size, v5 prefix,
  and SHA-256 in a plan-owned manifest/report.
- [ ] Verify no unrelated tracked binary edits overlap the cutover window.
- [ ] Define valid-package conversion and each corrupt/incompatible fixture's
  named v6 regeneration strategy.
- [ ] Capture Engine/Sandbox baseline, package, Cook, bulk, and documentation
  pre-cutover results.

#### Acceptance Gate

- The exact source corpus and replacement strategy are reviewable and no binary
  write begins without complete closure.

### Stage 1: Implement and prove the temporary converter

- [ ] Add a bounded offline command that dry-runs exact v5-to-v6 conversion and
  reports source/destination hashes plus semantic equality.
- [ ] Prove deterministic repeated conversion, refusal of non-v5/corrupt input,
  and package-plus-companion closure equality.
- [ ] Switch production policy and ordinary write paths to v6 behind the same
  change set while retaining the temporary v5 input converter only offline.
- [ ] Update or replace v5-specific tests with v6 contract coverage.

#### Acceptance Gate

- Dry-run covers the frozen valid corpus exactly and every production operation
  selects v6 without silently accepting legacy bytes.

### Stage 2: Convert packages and fixtures

- [ ] Convert every valid tracked `.dasset` atomically and verify semantic,
  descriptor, source-path, and deterministic-byte equality.
- [ ] Regenerate seven compatibility fixtures with preserved named intent under
  v6 and update fixture expectations.
- [ ] Verify companions are unchanged and every manifest destination begins
  with `DURF`/DAST v6 with no DTRF footer.
- [ ] Run Engine and Sandbox `asset baseline` validation.

#### Acceptance Gate

- The complete frozen corpus is v6, fixture intent is preserved, both project
  baselines pass, and no companion or unrelated binary changed.

### Stage 3: Remove legacy authority and qualify

- [ ] Delete the temporary converter, v5 codec, DTRL/DTRF trailer/footer code,
  and obsolete v5-only tests/policy names.
- [ ] Prove zero residual v5 prefix, converter symbol, DTRL/DTRF authority, and
  unsupported dual-format reader path.
- [ ] Run Core, package, Cook, bulk, catalog/mutation, Engine, Sandbox, and
  required broad validation after legacy removal.
- [ ] Update lasting package, lifecycle, versioning, serialization, and content
  version-control contracts to v6-only authority.

#### Acceptance Gate

- DAST v6 is the sole supported/emitted baseline and all affected validation
  passes without legacy or converter code.

### Stage 4: Close the roadmap

- [ ] Record exact manifest, conversions, tests, project baselines, residual
  searches, and binary diff evidence in this plan.
- [ ] Mark M3 and the parent roadmap completed and validate changed/all plans,
  roadmaps, and documentation.
- [ ] Commit the isolated cutover with plan/stage provenance.

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

- [Durin Binary Envelope and DAST v6](../Roadmaps/DurinBinaryEnvelopeAndDastV6.md)
- [DAST v6 Package Codec](DastV6PackageCodec.md)
- [Asset Packages](../Runtime/Assets/AssetPackages.md)
- [Asset Data Lifecycle](../Runtime/Assets/AssetDataLifecycle.md)
- [Versioning](../Runtime/Assets/Versioning.md)
- [Content Version Control](../Development/VersionControl/ContentVersionControl.md)

## Related Code

- `Engine/Source/Runtime/AssetCore/Private/AssetPackageCodec.cpp`
- `Engine/Source/Runtime/AssetCore/Private/AssetPackageV6Codec.cpp`
- `Engine/Tests/Native/AssetCoreTests/Data/Compatibility/`
- `Engine/Content/`
- `Sandbox/Content/`
