# Authored Package Storage Strategic Activation Plan

Summary: Supersede the measured Retain disposition with an explicit strategic Proceed directive and select the bounded package and companion route.

Last reviewed: 2026-08-24

Status: Archived
Completed: 2026-08-24

## Current Status

The measured
[Authored Package Trailer Qualification](AuthoredPackageTrailerQualification.md)
correctly recorded Retain for the present 32-package corpus. The user has now
directed Codex to generate and execute the remaining plans until the
[Authored Package Storage Evolution](../../../Roadmaps/Archive/2026-08/AuthoredPackageStorageEvolution.md)
roadmap is complete. This is a strategic investment decision, not evidence that
the original corpus thresholds fired.

This plan records the explicit supersession boundary before production format
work starts. The qualification measurements, Retain scorecard, and diagnostic
timings remain historical evidence. Strategic Proceed accepts the additional
implementation and compatibility cost while preserving every durability,
source-control, domain-ownership, and rollback gate.

Strategic Proceed is now recorded. DAST v5 with a versioned EOF trailer/footer,
external DABK v1 placement in Git LFS, companion-first publication, complete
package replacement, dual v4/v5 reads, and canonical v4 rollback are selected.
The roadmap includes Milestone 0.5 and activates only
[Authored Package Trailer Foundation](AuthoredPackageTrailerFoundation.md).

## Goal

Record one truthful strategic Proceed decision, select the lowest-risk package
boundary and initial placement from the qualified candidates, update Milestone
ordering, and activate exactly one bounded Package Trailer Foundation plan.

## Scope

- Distinguish user-directed program activation from a measurement-triggered
  qualification result.
- Select the package boundary, trailer discovery boundary, initial placement,
  publication baseline, compatibility behavior, and rollback authority.
- Update the roadmap without rewriting or weakening the completed
  qualification evidence.
- Create the next implementation plan with no unresolved wire or placement
  branch.

## Non-Goals

- Implementing a trailer/footer codec, DAST v5 reader/writer, dual-read package
  loading, production publication, or asset migration in this plan.
- Changing DAST v4, DABK v1, `.gitattributes`, tracked assets, domain schemas,
  DDC, Cook, runtime resource contracts, or source-control history.
- Claiming that current corpus cost, current-computer timing, or deduplication
  evidence independently justified Proceed.
- Activating virtualization, optimization, or legacy-retirement milestones
  whose separate evidence gates have not passed.

## Design Decisions and Invariants

- Strategic Proceed is authorized by the user's explicit whole-roadmap
  completion directive. It supersedes the disposition, not the facts, of the
  completed Retain qualification.
- The selected package boundary is **DAST v5**, dispatched by the existing
  `DAST` preamble and version field. An unmodified DAST v4 reader rejects v5
  before body interpretation; no trailing bytes are appended to a v4 file.
- DAST v5 retains the canonical Name, Type, Schema, Object, and Value logical
  sections, then owns a separately versioned trailer and fixed EOF footer. The
  foundation plan freezes exact bytes, bounds, hashes, and canonical order.
- The initial placement is **External DABK v1 companion** in Git LFS. DAST v5
  package-local authored payload bytes, referenced/virtualized states,
  compression, deduplication, and a new companion format are out of scope.
- The object stream retains logical `PayloadId`, logical size, and logical
  content integrity. The trailer owns the required external placement,
  stored size, DABK container identity, and lookup authority. Any transitional
  duplicate physical fact must agree exactly or the package is corrupt.
- Publication remains companion-first followed by complete-file atomic package
  replacement. Tail rewrite and append generation remain rejected.
- New readers must retain DAST v4/DABK v1 support. New writes remain opt-in
  until the selected-local publication plan passes. Rollback requires a
  canonical DAST v4 package plus every DABK v1 companion it names.
- The only foundation placement enum is `ExternalDabkV1`; unknown required
  values fail. Extensibility is versioned but carries no reserved behavior.
- Domain-owned schemas and `FEditorBulkData` semantic ownership do not change.
  DDC remains non-authoritative.

## Current Foundations and Gaps

| Area | Foundation | Gap owned by next plan |
| --- | --- | --- |
| Version dispatch | Package codec policy already rejects unsupported versions before construction | Register a read-only DAST v5 foundation codec without enabling production writes |
| Logical stream | DAST v4 has canonical five-section models and independent reference fixtures | Reuse the logical model under v5 while changing physical EOF ownership explicitly |
| Companion | DABK v1 is immutable, bounded, hashed, inspectable, and LFS-backed | Define trailer entry authority and cross-validation against DABK v1 |
| Inspection | Construct-free package and authored-bulk inspection exists | Discover and validate v5 trailer/footer without object construction |
| Compatibility | v4 canonical resave and exact rejection behavior exist | Add v5 golden/corruption fixtures and an explicit v4/v5 matrix |
| Publication | Companion-first plus atomic package replacement is qualified | Production dual-read/write integration remains Milestone 2, not foundation work |

## Implementation Stages

### Stage 0: Record strategic authority

- [x] Record that the user-directed completion mandate supersedes the Retain
  disposition without altering qualification measurements.
- [x] Preserve correctness, durability, source-control, rollback, and
  domain-ownership gates as non-waivable.

#### Acceptance Gate

- The activation rationale is explicit and cannot be mistaken for a telemetry
  threshold or performance claim.

### Stage 1: Select the implementation boundary

- [x] Select DAST v5 over an outer envelope and record why single-preamble
  version dispatch is the smaller compatibility surface.
- [x] Select trailer-indexed external DABK v1 in Git LFS and reject
  package-local, virtualized, compressed, tail, and append variants.
- [x] Freeze old-reader rejection, new-reader dual-read, full-file rollback,
  companion-first publication, and unsupported-state behavior.

#### Acceptance Gate

- Exactly one package boundary, trailer discovery model, placement,
  publication baseline, and rollback route is selected.

### Stage 2: Update sequencing and activate foundation work

- [x] Add the strategic activation milestone and exact selected route to the
  roadmap while preserving the completed Retain qualification.
- [x] Create and link one Authored Package Trailer Foundation plan whose stages
  stop before production writer/default placement integration.
- [x] Run changed-document, all-plan, and all-roadmap validation.

#### Acceptance Gate

- The roadmap, this plan, the completed qualification, and the activated
  foundation plan agree on authority, wire, placement, compatibility,
  publication, and deferred scope.

## Validation Matrix

| Area | Evidence |
| --- | --- |
| Authority | Retain evidence remains unchanged; strategic Proceed cites the explicit user directive |
| Selection | DAST v5 + versioned trailer/footer + `ExternalDabkV1` is the only activated route |
| Compatibility | v4 reader rejects v5; new reader retains v4/v5 dispatch; rollback bytes are explicit |
| Source control | `.dasset` remains ordinary Git and `.dabulk` remains Git LFS |
| Durability | Companion-first and whole-package replacement remain mandatory |
| Documentation | Changed, all-plan, and all-roadmap lifecycle validation |

## Validation Evidence

- Changed-document validation passed for the strategic plan, foundation plan,
  and roadmap update.
- All-plan validation passed with one active foundation plan and seven
  completed plans.
- All-roadmap validation passed with no lifecycle or link diagnostics.

## Definition of Done

- Strategic Proceed is recorded without falsifying qualification evidence.
- One complete route is selected and every alternative is explicitly deferred
  or rejected.
- The roadmap contains an activated foundation milestone/plan and no later
  milestone is prematurely activated.
- Documentation validation passes and the plan is completed in one isolated
  commit with exact plan/stage provenance.

## Deferred Follow-ups

- Trailer/footer codec and fixtures: Package Trailer Foundation.
- Production dual-read and opt-in publication: Selected Local Authored Payload
  Publication.
- Consumer migration, default writer, virtualization, optimization, and legacy
  DABK retirement remain in their roadmap milestones.

## Related Documentation

- [Authored Package Storage Evolution](../../../Roadmaps/Archive/2026-08/AuthoredPackageStorageEvolution.md)
- [Authored Package Trailer Qualification](AuthoredPackageTrailerQualification.md)
- [Asset Packages](../../../Runtime/Assets/AssetPackages.md)
- [Content Version Control](../../../Development/VersionControl/ContentVersionControl.md)

## Related Code

- `Engine/Source/Runtime/AssetCore/Private/AssetPackageCodec.h`
- `Engine/Source/Runtime/AssetCore/Private/AssetPackageV4Reader.cpp`
- `Engine/Source/Runtime/AssetCore/Private/Asset/PackageV4Writer.h`
- `Engine/Source/Runtime/AssetCore/Private/EditorBulkDataStorage.cpp`
- `Engine/Tests/Native/AssetCoreTests/Private/PackageV4WireContractTests.cpp`
