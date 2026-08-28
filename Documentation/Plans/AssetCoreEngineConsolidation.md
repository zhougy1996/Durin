# AssetCore Engine Consolidation Plan

Summary: Remove the AssetCore module boundary by consolidating its asset runtime into Engine while preserving package behavior and CoreDObject ownership.

Last reviewed: 2026-08-29

Status: Active
Completed:

## Current Status

Planning is selected. No source, module metadata, public API, package format,
or runtime behavior has changed yet. The first implementation stage is an
atomic module-boundary migration because partially moving implementation
cohorts would introduce either duplicate symbols or an `Engine`/`AssetCore`
dependency cycle.

## Goal

Remove `AssetCore` as a shared module and make its package, catalog, loading,
bulk-data, Cook, and authoring-tool implementation an Asset subsystem inside
`Engine`. Preserve the existing `Durin::Asset` namespace, `Asset/...` include
paths, DAST/DBLK/DABK bytes, public behavior, and `CoreDObject` ownership of the
managed-object foundations.

The consolidation must leave one buildable source owner, eliminate the
`AssetCore` binary/API boundary completely, and provide a simpler foundation
for later package-loading work without taking that redesign into this plan.

## Scope

- Move `AssetCore` public and private sources into cohesive `Engine/Public/Asset`
  and `Engine/Private/Asset` ownership.
- Transfer reflected-header registration and replace `ASSETCORE_API` exports
  with the appropriate `ENGINE_API` surface.
- Remove the `AssetCore` project mapping, module descriptor, CMake target, and
  every direct module or executable link dependency.
- Retarget native asset tests and private-source include paths to `Engine`
  without changing their behavioral coverage or fixture corpus.
- Preserve the standalone `DurinAssetTool` workflow, which already links
  `Engine`, and verify that audit and canonical-resave code remains available.
- Make implementation-only codec, transaction, catalog-store, and runtime-state
  headers private to `Engine` after the mechanical move where this can be done
  without changing behavior.
- Update lasting module-ownership and asset-package documentation in the same
  implementation commit.

## Non-Goals

- Moving `DPackage`, `FAssetPath`, object paths, Outer/GC behavior, reflection,
  or soft-object foundations out of `CoreDObject`.
- Changing DAST v6, DBLK v2, DABK v2, manifest formats, file suffixes, package
  fingerprints, canonical byte output, or compatibility policy.
- Adding asynchronous loading, range I/O, a Package Store, IoStore-style
  containers, load handles, priorities, cancellation, or hot reload.
- Changing the single-main-asset rule, cross-package main-asset-only references,
  object identity, redirect semantics, dependency eagerness, or unload policy.
- Moving mutation, relocation, deletion, canonical-resave, compatibility, or
  authored-bulk workflows into Editor/Developer modules during consolidation.
- Renaming `Durin::Asset`, existing `Asset/...` public includes, native test
  targets, or user-facing asset commands solely for directory aesthetics.
- Retaining an empty compatibility `AssetCore` module or forwarding library.

## Design Decisions and Invariants

- `Engine` owns the complete asset subsystem; folders and namespaces remain
  explicit even though the dynamic-library boundary is removed.
- `CoreDObject` continues to own `DPackage` and the object identity/lifetime
  primitives. `CoreDObject` must not acquire an `Engine` dependency.
- Existing public include spelling remains stable. The current overlap between
  `AssetCore/Public/Asset` and `Engine/Public/Asset` is resolved by co-location,
  not by a repository-wide include rename.
- The consolidation is behavior-preserving. Any required semantic fix found
  during migration is recorded separately unless it is necessary to restore
  the pre-move contract.
- Source relocation, export-macro replacement, reflected-header transfer,
  consumer dependency changes, tests, and removal of the old module occur as
  one build-boundary stage. There is no supported half-migrated graph.
- The implementation keeps one source/build writer as required by repository
  policy. No second checkout edits or builds this migration concurrently.
- Package fixtures and canonical byte comparisons remain unchanged and are the
  authority for detecting accidental serialization drift.
- A shipping/runtime size split may be considered later, but this plan does not
  replace one module boundary with several speculative asset modules.

## Current Foundations and Gaps

- `AssetCore` currently has only `Core` and `CoreDObject` as public dependencies,
  so its existing lower-layer boundary is technically clean.
- `Engine` publicly depends on `AssetCore`, while Engine asset headers expose
  `FEditorBulkData`, cooked-payload descriptors, Cook contexts, package
  inspection, and asset results. The effective public model is already shared.
- Nearly every direct `AssetCore` consumer also depends on `Engine`; the main
  offline host links both explicitly. The separate binary therefore provides
  little isolation to the current target graph.
- The module carries roughly twenty thousand private implementation lines,
  thirty public headers, and more than one hundred exported declarations. Many
  declarations are public primarily because of the shared-library boundary.
- Both modules own headers below `Public/Asset`, which obscures source ownership
  and makes include spelling independent of the actual module boundary.
- AssetCore-specific test build metadata reaches into the module's private
  source directory. That access must move with the implementation rather than
  being replaced with a temporary compatibility path.

## Implementation Stages

### Stage 0: Define the implementation boundary

- [ ] Capture the complete `AssetCore` source, reflected-header, public-header,
  module-dependency, executable-link, test-private-include, generated-code, and
  documentation reference inventory.
- [ ] Classify every file into `Engine/Public/Asset`, `Engine/Private/Asset`, or
  unchanged `CoreDObject` ownership; record any ambiguous item before moving it.
- [ ] Confirm that no supported executable or module consumes `AssetCore`
  without already linking or publicly receiving `Engine`.
- [ ] Establish the pre-move validation baseline and confirm the configured
  test registry entries for `AssetBulkContainerTests`, `AssetCookTests`,
  `AssetPackageTests`, `AssetImportTests`, and `TerrainWorldBuildTests`.
- [ ] Freeze format and fixture expectations: no baseline regeneration and no
  accepted canonical-byte changes are part of this plan.

#### Acceptance Gate

- Every source and consumer has one selected destination or explicit unchanged
  owner; no circular dependency or unsupported AssetCore-only host remains.
- Scope, invariants, fixture policy, and validation requirements are explicit.

### Stage 1: Consolidate the module boundary atomically

- [ ] Move public AssetCore headers into `Engine/Public/Asset`, resolving the
  existing shared include subtree without changing include spelling.
- [ ] Move codecs, object-stream adapters, runtime services, catalog storage,
  bulk containers, Cook support, mutation transactions, and tooling
  implementation into structured `Engine/Private/Asset` subdirectories.
- [ ] Replace `ASSETCORE_API` with `ENGINE_API` only where a declaration remains
  a public cross-module API; remove export decoration from Engine-private types.
- [ ] Transfer `Asset/CookedAsset.h` and `Asset/Redirector.h` reflection inputs
  to `Engine.dmodule` and verify generated-header ownership.
- [ ] Update Engine, editor, developer, program, and test build metadata to link
  only the new owner; retarget AssetCore test private include paths to Engine.
- [ ] Remove `AssetCore` from `Engine.dproject`, delete its `.dmodule` and CMake
  target, and remove the Engine-to-AssetCore dependency.
- [ ] Keep `Durin::Asset`, public function signatures, initialization order,
  package fixtures, and command-line behavior unchanged.

#### Acceptance Gate

- The configured editor target graph generates and links without an
  `AssetCore` target, library, import library, generated-code owner, or direct
  dependency.
- `CoreDObject` remains independent of `Engine`; no new lower-level dependency
  points from Core, CoreDObject, RHI, or RenderCore into Engine asset services.
- Focused package, bulk-container, Cook, import, and Terrain Cook/build tests
  pass with their existing fixtures and registered target names.

### Stage 2: Contract the in-Engine implementation surface

- [ ] Audit the migrated public headers and move codec registries, V6 policy,
  logical object-stream readers/writers, catalog persistence, runtime state,
  and mutation journal internals behind `Engine/Private/Asset`.
- [ ] Remove module-boundary-only forwarding and export plumbing where direct
  Engine-internal calls are clearer, while retaining capability-named public
  entry points used by editor, build, and program modules.
- [ ] Keep the explicit Asset subsystem structure; do not collapse catalog,
  package serialization, residency, Cook, and mutation into one implementation
  file or global facade.
- [ ] Verify that public Engine asset headers include their direct requirements
  and do not rely on the removed module's former transitive include behavior.
- [ ] Audit runtime-variant linkage so authored tooling remains unavailable by
  policy where required, without introducing a new module split in this plan.

#### Acceptance Gate

- No public header exposes a codec, store, transaction, or singleton solely
  because the former shared-library implementation needed an exported seam.
- All external consumers compile using the preserved `Asset/...` includes and
  `Durin::Asset` names, with no compatibility module or include redirect tree.
- Asset package bytes, catalog results, load reports, mutation outcomes, and
  Cooked payload behavior remain unchanged under the focused suites.

### Stage 3: Validate integration and publish ownership

- [ ] Run changed-document validation after the final documentation edits.
- [ ] Build the complete configured non-application editor target graph,
  including `DurinAssetTool` and all modules that previously linked AssetCore.
- [ ] Run the focused Asset package/Cook/integration targets, then `test all`
  because this changes a shared runtime and build-module boundary. Application-
  hosted tests remain excluded unless separately authorized by their gate.
- [ ] Run the documented editor startup smoke test to cover module discovery,
  reflection registration, Asset runtime initialization, and shutdown.
- [ ] Confirm targeted searches find no active `AssetCore` source directory,
  module mapping, dependency, link target, API macro, or generated ownership;
  historical archive text is not rewritten solely for this migration.
- [ ] Update module routing and lasting Asset package/data-lifecycle ownership
  documents from `AssetCore` to the Engine Asset subsystem.
- [ ] Record validation evidence and complete every stage checklist in the same
  isolated commit as the implementation.

#### Acceptance Gate

- All required builds, focused tests, full non-application native tests,
  startup smoke coverage, documentation validators, and stale-reference audits
  pass.
- The repository has one Engine-owned Asset implementation and no live
  `AssetCore` module artifact or ownership claim.
- The final commit uses the repository commit format and includes exact
  `Plan: Documentation/Plans/AssetCoreEngineConsolidation.md` and stage
  provenance.

## Validation Matrix

| Change surface | Required validation | Evidence sought |
| --- | --- | --- |
| Documentation and lifecycle metadata | `doc validate --scope changed`, then plan validation required by the final status update | Valid links, active-plan structure, and current ownership names |
| Module graph and generated reflection | Complete configured `Win64-Debug-DurinEditor` build following the Build and Run guide | No AssetCore target; Engine owns redirected symbols and reflected headers |
| Package format and live loading | `AssetPackageTests` | Identical fixtures, canonical bytes, dependency rollback, residency, compatibility, and mutation behavior |
| Bulk and Cook containers | `AssetBulkContainerTests`, `AssetCookTests` | Unchanged DBLK/DABK/manifest bytes, validation, and publication |
| Cross-module authoring integration | `AssetImportTests`, `TerrainWorldBuildTests` | Editor/build providers resolve Engine-owned Asset APIs and runtime adapters |
| Shared runtime/build boundary | `test all` with application-hosted tests disabled | No missed consumer, registration, initialization, or link-order regression |
| Executable composition | Full build of `DurinAssetTool` and the configured editor, plus editor startup smoke | Offline tool and editor initialize and shut down without the removed module |
| Stale ownership | Targeted source/module/document searches excluding historical archives | No active module mapping, dependency, API macro, target, or source owner remains |

## Definition of Done

- `AssetCore` no longer exists in `Engine.dproject`, source module metadata,
  generated reflection ownership, CMake targets, link dependencies, API macros,
  or active source ownership documentation.
- Engine contains a cohesive `Public/Asset` and `Private/Asset` subsystem; the
  old include spelling and `Durin::Asset` namespace remain valid.
- `DPackage`, `FAssetPath`, soft-object foundations, reflection, GC, and Outer
  behavior remain owned by `CoreDObject`, with no Engine dependency introduced.
- Existing DAST, bulk-container, catalog, load, save, mutation, Cook, audit, and
  canonical-resave behavior is preserved without fixture regeneration.
- The focused validation matrix and full shared-runtime test gate pass, and the
  editor/offline-tool compositions build successfully.
- Lasting module and Asset documentation names Engine as the owner; this plan
  records final evidence and has no incomplete checklist before completion.
- The isolated implementation commit contains the required plan and stage
  provenance.

## Deferred Follow-ups

- Asynchronous package loading, request coalescing, priorities, cancellation,
  and load handles.
- Range-based package and cooked-payload I/O plus a Package Store abstraction.
- Multiple public exports, full cross-package object paths, and external
  packages for Actor/Level partitioning.
- Live residency ownership that does not infer current references from the
  persisted catalog, plus batched or budgeted GC.
- Cooked archive/TOC/chunk storage, compression, encryption, patching, and
  install groups.
- A later evidence-based split of editor-only mutation/audit/publication code
  if runtime binary size or target composition demonstrates a real need.
- Renaming AssetCore-derived native test directories, targets, domains, and
  historical fixtures after the module migration is stable.

## Related Documentation

- [Asset Packages](../Runtime/Assets/AssetPackages.md)
- [Asset Data Lifecycle](../Runtime/Assets/AssetDataLifecycle.md)
- [Code Modules](../Workspace/CodeModules.md)
- [Agent Build and Run Workflow](../Agents/BuildAndRun.md)
- [Agent Testing Workflow](../Agents/Testing.md)
- [Documentation Workflow](../Agents/Documentation.md)

## Related Code

- [`AssetCore` module descriptor](../../Engine/Source/Runtime/AssetCore/AssetCore.dmodule)
- [`Engine` module descriptor](../../Engine/Source/Runtime/Engine/Engine.dmodule)
- [`DPackage`](../../Engine/Source/Runtime/CoreDObject/Public/DObject/Package.h)
- [AssetCore public surface](../../Engine/Source/Runtime/AssetCore/Public)
- [AssetCore implementation](../../Engine/Source/Runtime/AssetCore/Private)
- [Engine Asset public surface](../../Engine/Source/Runtime/Engine/Public/Asset)
- [`DurinAssetTool` build composition](../../Engine/Source/Programs/DurinAssetTool/CMakeLists.txt)
- [AssetCore native-test build composition](../../Engine/Tests/Native/AssetCoreTests/CMakeLists.txt)
