# Stable Authored Companion Publication Plan

Summary: Publish authored DABK companions under stable names with hidden transactional backups and hash-verified crash recovery.

Last reviewed: 2026-08-24

Status: Archived
Completed: 2026-08-24

## Current Status

Complete. Authored DABK discovery and submission now use only
`<package-stem>.dabulk`; generation-named compatibility and migration paths were
removed after the two tracked companions moved to stable names. Single and
bundle saves durably back up a prior stable companion, restore it on synchronous
failure, and remove the backup only after the replacement closure verifies.
Live load selects final or backup bytes by the package descriptor hash while
construct-free inspection remains read-only. The asset-package domain tests,
30-package compatibility audit, two-companion construct-free corpus inventory,
documentation validation, and full Win64 Debug `all` build passed.

## Goal

Replace generation-named submitted authored companions with one stable
`<package-stem>.dabulk` path per package without weakening atomic publication,
crash recovery, integrity validation, relocation, deletion, or source-control
closure.

## Scope

- Resolve authored DABK companions independently of their container hash while
  retaining the hash in descriptors, trailers, and DABK headers.
- Make single-package and bundle saves preserve the prior stable companion in a
  hidden durable backup until package publication succeeds.
- Recover an interrupted replacement by selecting the final or backup
  companion whose container hash matches the published package.
- Migrate the two tracked generation-named companions and update lasting
  package, lifecycle, VolumeTexture, and source-control contracts.
- Cover stable naming, failure rollback, crash recovery, cleanup, and corpus
  closure with focused tests and construct-free inspection.

## Non-Goals

- Changing DAST v5, its trailer/footer wire, DABK v1, payload ids, or hashes.
- Persistent virtualization, remote hydration, deduplication, compression, or
  cross-package companion sharing.
- Preserving generation-named filenames as a supported production route after
  the tracked corpus is migrated.

## Design Decisions and Invariants

- The submitted companion is exactly `<package-stem>.dabulk`; transactional
  files are hidden/internal and never enter the submit closure.
- The package descriptor and trailer remain the authority for expected payload
  and container identity. A filename never establishes integrity.
- Companion replacement precedes package replacement. If a prior companion
  exists, a complete durable backup is published first. Any synchronous
  package failure restores it before returning.
- After a crash, loading an external payload validates the final companion
  against the package descriptor. A mismatch may be recovered only from a
  complete backup with the exact expected container hash; ambiguous or corrupt
  states fail closed.
- A package that matches the final companion commits recovery and removes a
  stale backup. Inspection stays read-only; recovery occurs only on live
  payload load or a new authorized save.
- Stable naming does not change package or DABK wire versions.

## Current Foundations and Gaps

- Atomic file publication already writes flushed hidden sibling temporaries and
  atomically replaces one destination.
- DABK parsing verifies its header container hash and every payload content
  hash against the package descriptor.
- Single and bundle saves already publish companions before packages and clean
  stale generation-named companions after success.
- The current generation filename prevents overwrite rollback from being
  necessary; switching to a stable name therefore requires explicit backup
  ownership in both save paths and load-time recovery.

## Implementation Stages

### Stage 0: Define stable publication and recovery

- [x] Confirm the unchanged DAST v5/DABK v1 wire and descriptor hash authority.
- [x] Select stable submitted names, hidden atomic temporaries, durable sibling
  backup ownership, synchronous rollback, and hash-directed crash recovery.

#### Acceptance Gate

- Scope, decisions, and validation requirements are explicit.

### Stage 1: Implement stable companion transactions

- [x] Change path discovery, orphan classification, and cleanup to the stable
  submitted filename while excluding internal backup/temporary files.
- [x] Add shared backup/publish/rollback/commit mechanics to single and bundle
  saves without weakening package-last publication.
- [x] Add descriptor-directed live-load recovery and fail closed when neither
  final nor backup matches the published package.

#### Acceptance Gate

- Ordinary saves expose only a complete old or new stable companion/package
  closure, and every injected failure restores the old closure.

### Stage 2: Qualify and migrate the repository closure

- [x] Cover stable naming, repeated replacement, no-prior-companion saves,
  package failure rollback, committed-backup cleanup, interrupted replacement
  recovery, corruption rejection, orphan inspection, relocation, and deletion.
- [x] Migrate both tracked companions to stable names and verify every package,
  descriptor, trailer, companion, Git/LFS addition, and deletion.
- [x] Update lasting documentation, validate the repository, complete the plan,
  and commit one isolated closure.

#### Acceptance Gate

- The tracked corpus has two reachable stable companions, no reachable
  generation-named companions, no internal transaction artifacts, and all
  focused package/content/documentation validation passes.

## Validation Matrix

| Area | Required evidence |
| --- | --- |
| Naming | Exact stable path; internal backup/temp exclusion |
| Integrity | Descriptor, trailer, DABK container, and payload hashes unchanged |
| Save | Single and bundle package-last publication with synchronous rollback |
| Recovery | Final/backup selection by published descriptor; corrupt states reject |
| Operations | Inspection, orphan cleanup, relocation, deletion, and resave |
| Corpus | Two reachable stable LFS companions; zero missing/orphan/internal files |
| Repository | Asset package/content tests and documentation validators |

## Definition of Done

- Stable submitted companion naming is the only ordinary authored DABK route.
- A failed or interrupted save cannot strand the published package without its
  exact descriptor-matching companion when a valid prior closure existed.
- The repository corpus and lasting documentation reflect the stable closure.

## Deferred Follow-ups

- Cross-process writer exclusion and persistent multi-package transaction
  journals remain part of the broader authored mutation-coordination design.
- Persistent virtualization and backend optimization retain their independent
  evidence gates.

## Related Documentation

- [Authored Package Storage Evolution](../../../Roadmaps/Archive/2026-08/AuthoredPackageStorageEvolution.md)
- [Asset Packages](../../../Runtime/Assets/AssetPackages.md)
- [Asset Data Lifecycle and Storage](../../../Runtime/Assets/AssetDataLifecycle.md)
- [Volume Textures](../../../Runtime/Assets/VolumeTextures.md)
- [Content Version Control](../../../Development/VersionControl/ContentVersionControl.md)

## Related Code

- `Engine/Source/Runtime/AssetCore/Public/Asset/EditorBulkDataStorage.h`
- `Engine/Source/Runtime/AssetCore/Private/EditorBulkDataStorage.cpp`
- `Engine/Source/Runtime/AssetCore/Private/AssetPackageOperations.cpp`
- `Engine/Tests/Native/AssetCoreTests/Private/PackageTests.cpp`
