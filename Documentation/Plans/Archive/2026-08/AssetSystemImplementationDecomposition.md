# AssetSystem Implementation Decomposition Plan

Summary: Decompose the monolithic AssetCore private implementation into cohesive translation units without changing its public contracts or runtime behavior.

Last reviewed: 2026-08-21

Status: Archived
Completed: 2026-08-21

## Current Status

All stages are complete. Stateless facades, all three mutation workflows, catalog behavior, cache persistence, serialized/loaded reference traversal, package operations, and runtime residency now live in cohesive translation units. The former `AssetSystem.cpp` has been removed: package save/inspection/canonicalization lives in `AssetPackageOperations.cpp`, runtime loading/residency/lifecycle lives in `AssetRuntime.cpp`, and catalog/runtime private state declarations have narrow headers. Relocation and redirector fix-up share journal, byte staging, fingerprint, publication, and reference-projection primitives while retaining their distinct validation and compensation ordering. No-op Engine deletion contributors were removed; the owned-payload and move-observer extension points remain for concrete cross-module ownership boundaries. All six registered targets that directly declare `asset-core` passed, covering 162 tests, and the full `all` build completed successfully.

## Goal

- Reduce `AssetSystem.cpp` from a 7,500-line mixed-responsibility implementation into cohesive private translation units.
- Keep each change reviewable and leave AssetCore buildable after every stage.
- Preserve package formats, catalog behavior, mutation atomicity, residency behavior, and exported API signatures.

## Scope

- Private implementation and private-header decomposition within `AssetCore`.
- Relocation, redirector fix-up, deletion, catalog, package operations, reference processing, loading, residency, and public forwarding implementations currently located in `AssetSystem.cpp`.
- Consolidation of duplicated private mutation publication and compensation mechanics after mechanical decomposition is validated.

## Non-Goals

- No public asset API redesign or new public mutable manager.
- No package, registry-cache, reference-cache, or mutation-journal format changes.
- No semantic changes to loading, saving, mutation, Cook reachability, or redirect resolution.
- No unrelated coding-standard cleanup.

## Design Decisions and Invariants

- Public responsibility boundaries remain those documented by `AssetPackage.h`, `AssetCatalog.h`, `AssetLoad.h`, and `AssetMutation.h`.
- The first decomposition pass is mechanical: move definitions and the minimum private state they require without changing control flow.
- Cross-translation-unit private declarations live in narrowly named internal headers. `AssetSystemInternal.h` must not become a dumping ground for every helper.
- Transaction preparation, commit, undo, redo, recovery journals, observer ordering, and catalog publication retain their current ordering and failure behavior.
- Each stage is independently buildable and testable. Common mutation machinery is extracted only after the separated implementations pass their existing tests.
- Registration is reserved for real cross-module extension ownership. The reference-store registry remains because AssetImportCore and LevelEditor provide persistent reference owners without reversing module dependencies; no-consumer or no-op extension registrations are audited for removal after mechanical decomposition.

## Current Foundations and Gaps

- Public headers are already separated by package, catalog, load, mutation, and test-support responsibility.
- `AssetSystem.cpp` currently contains approximately 7,531 lines and combines reference codecs, persistent caches, package operations, catalog scanning, three mutation workflows, and runtime loading/residency.
- `FAssetCatalogStore` and `FAssetRuntimeState` are declared in one private header and are consumed by several existing AssetCore translation units.
- Relocation and redirector fix-up contain similar staging, recovery, compensation, and registry-publication mechanics, but extracting them before file separation would combine structural and behavioral risk.

## Implementation Stages

### Stage 0: Select boundaries and baseline

- [x] Inventory the implementation by responsibility and identify its private dependencies.
- [x] Preserve the existing public API and serialized formats as explicit invariants.
- [x] Select mechanical translation-unit decomposition before shared transaction abstraction.

#### Acceptance Gate

- The plan passes the repository plan validator and identifies bounded, independently verifiable stages.

### Stage 1: Separate asset mutation implementations

- [x] Move stateless catalog forwarding code into `AssetCatalogFacade.cpp` and validate the new translation-unit boundary.
- [x] Move stateless runtime and mutation forwarding code into `AssetRuntimeFacade.cpp`.
- [x] Move relocation state and transaction implementation into `AssetRelocation.cpp`.
- [x] Move redirector fix-up state and transaction implementation into `AssetRedirectorFixup.cpp`.
- [x] Move deletion analysis and transaction implementation into `AssetDeletion.cpp`.
  - [x] Move the deletion token state machine, revalidation, residency transition, and registry projection operations.
  - [x] Move deletion analysis and the physical test-deletion path behind one narrow companion-inspection helper.
  - [x] Move deletion batch preparation after narrowing its registered reference-store and companion-owner dependencies.
- [x] Introduce only the narrowly shared private mutation declarations required by those translation units.
  - [x] Isolate the workflow-neutral mutation token state in `AssetMutationTransactionInternal.h`.
  - [x] Isolate shared mutation journal state, recovery lifetime, staging, fingerprint, and publication helpers in `AssetMutationJournalInternal.h`.
  - [x] Bound the mutation reference-projection dependency in `AssetMutationReferenceInternal.h`.
  - [x] Isolate the deletion token state in `AssetDeletionInternal.h`.
  - [x] Isolate the cross-module persistent-reference provider registry in `AssetMutationRegistryInternal.h`.
  - [x] Isolate relocation extension ownership and failure injection in `AssetRelocationExtensionsInternal.h`.
- [x] Preserve all mutation failure-injection seams and registration APIs.

#### Acceptance Gate

- AssetCore builds successfully.
- The registered AssetCore test target passes, including relocation, fix-up, and deletion cases.
- No public-header or persistent-format diff is introduced.

### Stage 2: Separate catalog and reference projection

- [x] Move registry and reference cache persistence into a dedicated implementation.
- [x] Move catalog scan, snapshot, path resolution, reference-index queries, and Cook reachability into a catalog implementation.
  - [x] Move snapshot, exact query, path resolution, reference-index query, Cook reachability, and catalog projection operations.
  - [x] Move scan, refresh, and persistent snapshot flushing after extracting cache persistence.
- [x] Move serialized and loaded reference traversal/rewrite helpers into a reference implementation.

#### Acceptance Gate

- AssetCore builds successfully.
- Catalog refresh, cache, redirect resolution, reference extraction, and Cook reachability tests pass.

### Stage 3: Separate package operations and runtime residency

- [x] Move package inspection, validation, serialization, and atomic save operations into a package-operations implementation.
- [x] Move create, load, soft-reference resolution, residency, unload, initialization, and shutdown into a runtime implementation.
- [x] Reduce `AssetSystem.cpp` to a small facade or remove it when no cohesive implementation remains.
- [x] Split `AssetSystemInternal.h` where consumers no longer require the combined declaration surface.

#### Acceptance Gate

- AssetCore builds successfully.
- Package, save/load, soft-reference, residency, and lifecycle tests pass.
- `AssetSystem.cpp` no longer contains multiple independent subsystem implementations.

### Stage 4: Consolidate private mutation mechanics

- [x] Inventory duplication exposed by the separated relocation, fix-up, and deletion implementations.
- [x] Extract shared staging, journal, publication, and compensation primitives only where ordering and recovery contracts are identical.
- [x] Keep workflow-specific validation and state explicit.
- [x] Audit no-op deletion-contributor registrations and the currently unconsumed owned-payload relocator API; retain extension points whose future ownership boundary is concrete and document that boundary clearly.
- [x] Evaluate the single-consumer move-observer registry against editor transaction ownership, retaining it when it remains the clearer Commit/Undo/Redo extension boundary.

The duplication audit confirmed that relocation and redirector fix-up already use
`AssetMutationJournal` for the identical durable state, byte staging,
fingerprinting, file publication, and journal-state persistence contracts.
Their remaining compensation loops differ in participant ordering, failure
injection, loaded-object handling, and external reference-store restoration, so
combining them would hide workflow invariants rather than reduce mechanism.
Deletion receives an editor-owned physical transition and has a deliberately
different transaction boundary. Engine registrations that contributed no
companions were removed. The unregistered-by-default owned-payload hook remains
the dependency boundary for future class modules that exclusively own sidecar
files, while the move observer remains the post-commit boundary used by
LevelEditor to synchronize transient state across Commit, Undo, and Redo.

#### Acceptance Gate

- AssetCore builds successfully.
- Mutation tests pass with failure injection at preparation, publication, compensation, undo, and redo boundaries.
- The shared abstraction reduces duplication without weakening workflow-specific invariants.

### Stage 5: Broad validation and completion

- [x] Run the bounded AssetCore validation lane and all directly affected dependent test targets discovered through the test registry.
- [x] Run the repository-required broader build gate for the completed cross-file refactor.
- [x] Update the owning Asset documentation only if implementation reveals a lasting contract change.
- [x] Record validation evidence and complete this plan.

#### Acceptance Gate

- All required builds and tests pass.
- Documentation and plan validators pass.
- No public API, serialized-format, or runtime-behavior change remains undocumented.

## Validation Matrix

| Area | Minimum validation |
| --- | --- |
| Plan lifecycle | `doc plan validate --scope all` |
| Translation-unit changes | AssetCore target build |
| Mutation separation | Registered AssetCore mutation tests |
| Catalog/reference separation | Catalog, package, reference, and Cook test cases |
| Runtime separation | Load, save, residency, unload, and lifecycle test cases |
| Final integration | Affected dependent targets and repository-required broader build gate |

Use the repository agent build and testing workflows to discover and execute exact commands; this plan does not redefine those procedures.

## Validation Evidence

Completed on 2026-08-21 with the `Win64-Debug-DurinEditor` profile:

- `.\DevTool.bat build --target AssetCore`
- `.\DevTool.bat build --target Engine`
- `.\DevTool.bat test AssetPackageTests`: 95 passed
- `.\DevTool.bat test AssetCookTests`: 13 passed
- `.\DevTool.bat test AssetDerivedDataTests`: 3 passed
- `.\DevTool.bat test AssetImportCoreTests`: 31 passed
- `.\DevTool.bat test AssetImportTests`: 17 passed
- `.\DevTool.bat test AssetMountedSourceTests`: 3 passed
- `.\DevTool.bat build`: full `all` target completed
- `.\DevTool.bat doc plan validate --scope all`
- `.\DevTool.bat doc validate --scope all`

The test registry identified exactly those six configured targets as directly
declaring the `asset-core` module. No lasting public contract, serialized
format, or runtime behavior change was introduced, so the owning Asset
documentation required no contract update.

## Definition of Done

- `AssetSystem.cpp` is removed or reduced to one cohesive facade responsibility.
- Major private responsibilities have independently understandable translation units and narrow internal dependencies.
- Existing public contracts and persistent formats are unchanged unless a separately documented decision explicitly supersedes this invariant.
- Focused and broad validation gates pass, and the plan records their evidence.

## Deferred Follow-ups

- Reconsider whether `FAssetRuntimeState` should be decomposed into separately owned state objects only after translation-unit boundaries expose stable ownership seams.
- Compile-time and incremental-build measurements may justify additional include-surface work after functional decomposition.

## Related Documentation

- [Asset packages](../../../Runtime/Assets/AssetPackages.md)
- [Asset catalog and mutation](../../../Runtime/Assets/AssetCatalogAndMutation.md)
- [Asset data lifecycle](../../../Runtime/Assets/AssetDataLifecycle.md)
- [C++ coding standards](../../../Development/Standards/CodingStandards.md)
- [Agent build and run workflow](../../../Agents/BuildAndRun.md)
- [Agent testing workflow](../../../Agents/Testing.md)

## Related Code

- `Engine/Source/Runtime/AssetCore/Private/AssetPackageOperations.cpp`
- `Engine/Source/Runtime/AssetCore/Private/AssetRuntime.cpp`
- `Engine/Source/Runtime/AssetCore/Private/AssetCatalogStoreInternal.h`
- `Engine/Source/Runtime/AssetCore/Private/AssetRuntimeStateInternal.h`
- `Engine/Source/Runtime/AssetCore/Public/AssetCatalog.h`
- `Engine/Source/Runtime/AssetCore/Public/AssetLoad.h`
- `Engine/Source/Runtime/AssetCore/Public/AssetMutation.h`
- `Engine/Tests/Native/AssetCoreTests/Private/PackageTests.cpp`
