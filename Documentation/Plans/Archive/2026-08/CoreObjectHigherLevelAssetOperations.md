# Core Object Higher-Level Asset Operations Plan

Summary: Qualify justified higher-level asset workflows on exact DAST v8 closures and remove object-stream compatibility variants.

Last reviewed: 2026-08-31

Status: Archived
Completed: 2026-08-31

- 2026-08-31: Stage 0 classified the 36 failures into retained relocation /
  fix-up, v8 inspection/BulkData, Cook, identity/Registry, stale v7 assertion,
  and retired compatibility groups. Production call matrices show identity is
  lost at admission header reads, Cook byte-only canonicalization, mutation
  rewrite contexts, and several physical-path inspection consumers.
- 2026-08-31: Stage 1 made v8 admission and physical inspection identity-aware,
  projected recursive values and BulkData descriptors directly from linker
  tables, and qualified bounded header reads, explicit admission, Registry
  cache/duplicate handling, soft-reference inspection, and exact bulk closures.
- 2026-08-31: Stage 2 carried package identity and bulk closure through linker
  reference rewriting, rebuilt hard and soft Registry metadata after fix-up,
  repaired redirector publication metadata, and qualified 22 relocation,
  redirector, rollback, alias, and Registry lifecycle cases on v8.
- 2026-08-31: Stage 3 confirmed deletion is already package-index driven and
  companion ownership is derived from exact v8 inspection. Nine deletion,
  blocker, redirector-closure, rollback, and bulk-companion cases pass without
  v7 descriptor parsing or dependency loads.
- 2026-08-31: Stage 4 carried identity and exact main/bulk closures through
  Cook serialization, canonicalization, reachability, pruning, publication,
  admission, and runtime linker application. Cook no longer manufactures a v7
  payload-directory segment; the complete `AssetPackageTests` suite passes
  109/109 after retiring one compatibility-only canonical-resave case.
- 2026-08-31: Stage 5 deleted the dormant Engine object-stream byte
  inspection/schema/mutation surface, raw bulk-segment fallback, identity-free
  package-header overloads, and disabled v7 tests. Exact v8 operations pass
  `AssetPackageTests` 108/108, Registry 3/3, linker adapter 8/8, Cook 10/10,
  editor workflow 34/34, and Material 111/111; Engine compiles successfully.

## Current Status

P0 through P6 are complete. Every retained higher-level operation now consumes
package-level candidates and exact v8 main/bulk closures. The former 36-failure
baseline is closed: retained relocation, fix-up, deletion, Cook, inspection,
and Registry behavior passes, while dormant object-stream and v7-only surfaces
have been deleted. P7 owns lasting contract publication and repository-wide
final qualification.

## Goal

Every surviving relocation, deletion, redirector/fix-up, inspection, Cook, and
maintenance workflow consumes package-level Registry candidates and exact v8
main/bulk closures. Remove dormant object-stream mutation/schema APIs and tests
whose only purpose was live v7 compatibility.

## Scope

- Exact package identity and main/bulk closure propagation through header,
  validation, inspection, mutation, Registry refresh, and Cook boundaries.
- Recursive v8 inspection projection only for concrete runtime/editor/tool
  consumers, including BulkData metadata without reconstructing v7 descriptors.
- Relocation, redirector fix-up, and deletion transactions using v8 codec
  mutations and package-level candidate sets.
- Canonical v8 Cook output and reference reachability, or explicit removal of
  unsupported legacy variants.
- Deletion of dormant Engine object-stream byte mutation/schema readers and
  stale v7-named qualification tests.

## Non-Goals

- Restoring v7 runtime loading/writing, global persistent occurrence routes, or
  compatibility/deprecated-route reports without a current consumer.
- Changing the DAST v8 wire format or CoreDObject linker semantics.
- Launching an editor/game application or running GPU qualification.

## Operation Dispositions

| Operation | Disposition |
| --- | --- |
| Relocation | Preserve one exact v8 transaction with main/bulk staging and rollback. |
| Redirector fix-up | Preserve through v8 linker reference rewriting and package-level candidates. |
| Deletion | Preserve package-level blocking analysis and exact companion deletion. |
| Exact occurrence inspection | Compute on demand from v8 linkers; remove persistent/object-stream variants. |
| Cook | Preserve canonical v8 closure publication; remove v7 payload-directory canonicalization. |
| Compatibility/deprecated-route tooling | Remove unless a remaining compiled consumer proves necessity. |
| Offline v7 converter | Retain below Engine through P7 qualification only. |

## Implementation Stages

### Stage 0: Freeze the P6 operation matrix

- [x] Classify all 36 baseline failures by retained operation, identity/closure
  defect, stale expectation, or retired behavior.
- [x] Map every surviving production caller to package identity, main bytes,
  bulk bytes, Registry candidate source, and transaction owner.
- [x] Freeze removal boundaries for dormant object-stream inspection/mutation
  APIs and compatibility-only tests.

#### Acceptance Gate

Every failing surface has one explicit retain/fix or retire/delete disposition;
no v7 compatibility behavior is reintroduced.

### Stage 1: Make v8 inspection and admission closure-exact

- [x] Require or derive canonical package identity at every v8 header,
  validation, scan, cache, and inspection boundary.
- [x] Project recursive struct/array/map/reference/BulkData values from linker
  tables for concrete consumers without an Engine package-wire model.
- [x] Qualify Registry refresh, duplicate-path, cache, admission, and large
  front-matter behavior.

#### Acceptance Gate

Ordinary scans remain front-matter bounded, exact inspections validate complete
closures, and all retained inspection consumers pass on v8.

### Stage 2: Qualify relocation and redirector fix-up

- [x] Pass exact source identity and main/bulk closure into relocation and
  reference rewrite codec calls.
- [x] Preserve staging, publication, registry, authored-state, and rollback
  invariants for loaded and unloaded packages.
- [x] Qualify redirector creation, fix-up, retained-alias behavior, and failure
  injection using package-level candidates.

#### Acceptance Gate

Retained relocation/fix-up tests pass without object-stream occurrence indexes
or v7 companion inference.

### Stage 3: Qualify deletion and companion ownership

- [x] Use package-level dependency state for blockers and exact v8 inspection
  only when a concrete companion or occurrence decision requires it.
- [x] Delete main/bulk closures transactionally and preserve loaded-package,
  registry, cache, and failure semantics.
- [x] Remove unsupported legacy deletion variants and tests.

#### Acceptance Gate

All retained deletion cases pass and no deletion path parses v7 descriptors or
loads dependencies solely to discover persistent occurrence routes.

### Stage 4: Qualify Cook and maintenance

- [x] Carry package identity and exact closure through Cook canonicalization,
  reference reachability, editor-only pruning, and bulk publication.
- [x] Emit canonical v8 main/bulk output and validate it through CoreDObject.
- [x] Remove legacy raw/payload-directory variants that lack a maintained
  consumer.

#### Acceptance Gate

Retained Cook tests pass on v8 without v7 table/value/wire helpers.

### Stage 5: Retire dormant seams and publish P6

- [x] Delete unused Engine object-stream byte inspection/schema/mutation code,
  compatibility reports, stale v7 tests, and obsolete public APIs.
- [x] Run focused operations, `AssetPackageTests`, bounded Registry/Cook/editor
  consumers, and compile Engine without launching an application.
- [x] Complete P6 and hand exact final-search/documentation gates to P7.

#### Acceptance Gate

Every former higher-level workflow is qualified on v8 or deleted with callers
and documentation; `AssetPackageTests` has no expected legacy failures.

## Validation Baseline

On 2026-08-31, `AssetPackageTests` ran 110 tests: 74 passed and 36 failed. The
complete output is recorded in
`Build/.agent-state/logs/20260831-045301-492183-59797-AssetPackageTests.log`.

## Related Code and Documentation

- [CoreDObject Engine Cutover](CoreDObjectEngineCutover.md)
- [Core Object Package Linker roadmap](../../../Roadmaps/Archive/2026-08/CoreObjectPackageLinker.md)
- [Asset Packages](../../../Runtime/Assets/AssetPackages.md)
- [Asset Catalog and Mutation](../../../Runtime/Assets/AssetCatalogAndMutation.md)
- [Testing Workflow](../../../Agents/Testing.md)
