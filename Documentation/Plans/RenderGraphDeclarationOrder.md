# Render Graph Declaration Order Plan

Summary: Adopt forward-only AddPassDependency declarations, execute retained passes in declaration order, and index retention predecessors for linear culling.

Last reviewed: 2026-09-10

Status: Completed
Completed: 2026-09-10

## Current Status

All stages and acceptance gates are complete. RenderContractTests passed all 149 tests in
Win64-Debug-DurinEditor (2026-09-10). Forward validation is centralized before
edge deduplication; invalid handles retain deferred compile failure. Ordering
now directly filters declaration indices. Retention indexes finalized retaining
edges once and marks passes before enqueueing. Coverage includes empty/no-root
graphs, independent passes, chains through 8192 passes, disabled culling, shared
ancestors, multiple roots, read/write values, execution-to-value upgrades,
overwrite pruning, and existing extraction/transition/capture oracles.

Audit: all six RDG call sites are in RDGTests.cpp. The lifecycle death test
keeps its Building-state expectation; the typed-parameter self-edge and the
self/cycle rejection fixtures will expect self/backward declaration errors.
The dependency-budget fixture migrates `AddDependency(Read, Write)` to
`AddPassDependency(Write, Read)`. No production or sample caller moves a pass,
so resource versions and callback capture lifetimes do not change. The RHI
pipeline creation helper is unrelated and remains unchanged.

All generated edges pass through AddDependencyEdge: explicit prerequisites,
value producers, and execution frontiers. Resource analysis visits declaration
indices; RDG.Export is appended after user passes and reads final stored values.
Outgoing and indegree arrays are used only by the removed scheduler. The owning
registered target is RenderContractTests; no production/backend wiring changes
require GPU execution. The authoritative RenderGraph contract is the only live
contract with obsolete scheduling wording.

## Goal

Make declaration order the executable order of the graph. Expose
`AddPassDependency(Producer, Consumer)` for explicit forward dependencies, reject
invalid ordering before execution, and compute retention without per-node scans
of the complete dependency table. Preserve resource-value semantics, deterministic
diagnostics, and the exclusion of execution-only edges from retention propagation.

## Selected Decisions

- RenderCore and the existing thread-confined builder own this change. Dependency
  declarations remain legal only during Building; compilation failure still
  consumes the builder and publishes no partial compiled result.
- `AddPassDependency(Producer, Consumer)` requires valid handles from the same
  builder and `Producer.Index < Consumer.Index`. Callers may add the edge after
  both passes have been declared. Self-dependencies and backward dependencies
  produce deterministic compile diagnostics, including on unreachable passes.
- Preserve the current deferred declaration-error handling for invalid dependency
  handles. Do not introduce process-fatal assertions for these input errors.
- Explicit dependencies retain their producer when the consumer is retained,
  matching the current `Explicit` behavior. Do not introduce a second public
  execution-only dependency API in this work.
- Resource analysis continues to interpret reads, writes, discard, and stored
  contents in declaration order. An explicit edge cannot repair a read before
  its producer. Every final scheduling edge must point forward.
- The retained executable sequence is exactly the declaration sequence filtered
  by retention. Remove the general topological scheduler and its indegree state;
  do not add a heap fallback that silently accepts backward declarations.
- Retention follows `Value` and `Explicit` edges and excludes `Execution` edges.
  Build retention predecessor lists once after dependency construction finishes,
  so execution-to-retaining edge upgrades are reflected correctly.
- Keep the canonical typed dependency records, causes, endpoint deduplication,
  budgets, and deterministic capture ordering. Remove outgoing scheduling indexes
  only after confirming that no remaining consumer needs them.
- Migrate and remove `AddDependency(Pass, Prerequisite)` in this work. The new API
  reverses the old argument order; do not perform a name-only replacement. This
  is an intentional source and backward-dependency behavior change.
- The design is inspired by UE's rendering workflow, not a claim of exact parity
  with an audited UE version. No UE implementation or complexity claim is an
  acceptance requirement for this plan.

## Scope

The work covers the RenderCore declaration API, dependency validation, retained
pass ordering, retention indexing, affected callers and native contract tests,
and the implemented Render Graph documentation. It does not add asynchronous
compute, cross-queue fences, parallel recording, resource allocation policies,
or a new resource-version model. It does not promise linear complexity for the
entire RDG compiler; the target is ordering and retention after edge generation.

## Implementation Stages

### Stage 0: Audit dependency callers and freeze migration cases

- [x] Search source, tests, samples, and relevant active documentation for the
  RDG `AddDependency` API and ordering expectations; distinguish unrelated APIs.
- [x] Identify every backward explicit dependency and every test expecting
  topological reordering or cycle errors. Record the intended forward replacement
  or new rejection expectation before changing those cases.
- [x] For callers requiring declaration movement, record resource read/write
  versions and callback capture lifetimes affected by the move. Do not treat
  successful index validation as proof of equivalent rendering behavior.
- [x] Confirm handling of generated export/sentinel passes and every edge creation
  path, and identify the smallest owning native test selection using repository
  testing guidance.

Completion condition: affected callers and tests have explicit migration
dispositions, and no unresolved ordering requirement is hidden by a fallback.
If a caller fundamentally requires backward ordering, record the conflict and
resolve the design before proceeding with that caller.

### Stage 1: Establish forward dependency declarations

Depends on Stage 0.

- [x] Introduce `AddPassDependency(Producer, Consumer)` and migrate all RDG callers
  with the correct argument order; remove the old public API.
- [x] Validate ownership, bounds, self-dependency, and forward direction with
  deterministic error messages. Preserve duplicate-edge idempotence.
- [x] Enforce the forward-edge invariant for generated dependencies as well as
  explicit declarations, retaining structural dependency-budget enforcement.
- [x] Add focused contract coverage for valid forward edges, duplicate edges,
  invalid/foreign handles, self edges, backward edges, and invalid unreachable
  declarations. Preserve read-before-producer rejection.

Completion condition: all accepted graphs satisfy the forward-edge invariant,
all migrated callers compile, and the owning declaration contract tests pass.

### Stage 2: Replace scheduling and retention scans

Depends on Stage 1.

- [x] Remove `BuildStablePassOrder` and unused indegree/emitted/outgoing state.
  Build the executable sequence directly from retained declaration indices.
- [x] Build per-consumer retention predecessor lists from finalized non-Execution
  edges. Skip this allocation when culling is disabled.
- [x] Traverse from roots with a visited/retained mark set before enqueueing,
  ensuring each retained node's predecessors are expanded at most once.
- [x] Preserve full declaration validation before culling, dependency filtering,
  extraction roots, lifetime/barrier processing, and canonical diagnostics.
- [x] Verify empty graphs, all-independent passes, long chains, shared ancestors,
  multiple roots, no roots, disabled culling, and generated exports.
- [x] Verify overwrite pruning, read/write value retention, explicit retention,
  and an Execution edge upgraded to a retaining edge for the same endpoints.

Completion condition: retained passes equal the retained declaration subsequence;
ordering costs O(P), and retention indexing plus traversal costs O(P + E), with
no per-retained-node scan of the full edge table. Captures and resource behavior
remain equivalent for previously valid forward graphs.

### Stage 3: Validate migration and publish the contract

Depends on Stage 2.

- [x] Run the owning native contract tests and affected caller checks selected
  through the build/test guides. Add renderer or backend checks only where the
  caller audit identifies changed production wiring or transition behavior.
- [x] Compare representative forward-graph captures for scheduled passes,
  retention, dependencies, resource lifetimes, and barriers. Document intentional
  diagnostic changes for formerly accepted backward declarations.
- [x] Check increasing independent-pass and sparse-chain graph sizes, including
  culling enabled and disabled. Record graph sizes, configuration, and observed
  timings; use structural/code evidence for complexity rather than a flaky
  wall-clock pass/fail threshold. Do not attribute all compile time to traversal.
- [x] Update the authoritative Render Graph contract with declaration ordering,
  the new API's argument order and retention semantics, failure behavior, and
  migration examples. Remove obsolete stable-topological/cycle wording where it
  describes the replaced implementation; retain unrelated historical evidence.
- [x] Validate changed documentation and plan lifecycle metadata, record actual
  validation evidence and limitations, and mark the plan complete only after all
  required gates pass.

Completion condition: implementation, callers, tests, and authoritative contract
agree; the removed API has no live RDG callers; all required validation is recorded.

## Validation Evidence

- Final owning selection: `.\DevTool.bat test RenderContractTests`,
  Win64-Debug-DurinEditor, 149/149 tests passed in 3.31 seconds; build 5.45 seconds.
  Receipt: `Build/.agent-state/logs/20260910-141834-888600-8196-RenderContractTests.log`.
- `test affected --explain` expands the shared Engine test project to unrelated
  asset/editor/GPU targets. The plan's explicit owning-target gate was used
  instead. No production caller moved, no backend transition implementation
  changed, and GPU/application execution was not required or run.
- Before/after scheduler captures: two complete deterministic dumps (culling on
  and off) of a buffer/token graph with independent work, multiple roots, shared
  predecessors and an Execution-to-Value upgrade matched byte for byte. This
  compares scheduled passes, dependencies, culling, lifetimes, uses and barriers.
  Receipts: `20260910-141545-557198-29964-RenderContractTests.log` and
  `20260910-141735-405178-33644-RenderContractTests.log` under the same log directory.
  The baseline run exposed a new test's incorrect assumption that public edges
  retain insertion order; the test now finds endpoints in canonical order. Final
  coverage also ensures the upgraded edge alone retains its producer.
- Existing extraction, overwrite-pruning, lazy capture, normalized range/version,
  barrier, budget and terminal-builder tests remain green. Added tests cover
  invalid unreachable edges, foreign/default handles on both endpoints, duplicate
  edges at the structural limit, read-before-producer with an explicit edge,
  read/write retention, shared ancestors and declaration subsequences.
- Removed API scan finds only the unrelated private RHI pipeline helper.
  BuildStablePassOrder and its outgoing/indegree/emitted state are absent.
- Changed-document and all-plan lifecycle validators passed; diff whitespace
  validation passed. The authoritative RenderGraph contract contains the API,
  migration, failure and retention rules.

### Diagnostic Scaling Samples

Single compile samples in milliseconds on this Windows MSVC Debug profile,
with the last pass rooted. Each row also ran without roots, and empty graphs
were checked. Both independent and chain graphs contain no resources; a sparse
chain has P - 1 explicit edges. Construction and capture are outside the timer;
all compiler work is inside it. These observations are not performance thresholds
or an attribution of all compile time to ordering/retention.

| Shape | Culling | Passes | Before | After |
| --- | --- | --- | --- | --- |
| Independent | Off | 128 | 3.418 | 1.116 |
| Independent | Off | 1024 | 139.670 | 8.941 |
| Independent | Off | 8192 | 8371.478 | 75.000 |
| Chain | Off | 128 | 3.968 | 1.837 |
| Chain | Off | 1024 | 143.622 | 14.765 |
| Chain | Off | 8192 | 8269.497 | 120.107 |
| Independent | On | 128 | 2.514 | 0.522 |
| Independent | On | 1024 | 130.228 | 3.635 |
| Independent | On | 8192 | 8265.360 | 31.288 |
| Chain | On | 128 | 4.187 | 2.014 |
| Chain | On | 1024 | 148.806 | 16.173 |
| Chain | On | 8192 | 8427.674 | 133.639 |

Structural evidence: one declaration-index loop emits retained passes; one
final-edge loop builds predecessor lists; the root loop and mark-before-enqueue
worklist expand each pass at most once and each retaining edge at most once.
Disabled culling returns before predecessor allocation. Canonical edge storage,
kind upgrades, sorting for diagnostics and resource analysis are unchanged.

## Execution and Handoff

Read [Build and Run](../Agents/BuildAndRun.md) before configuring, building, or
running repository targets, and [Testing](../Agents/Testing.md) before selecting
or running native tests. Follow [Documentation Workflow](../Agents/Documentation.md)
for document validation. Keep each stage's status and evidence in its implementation
commit, with exact Plan and Stage trailers under the repository handoff rules.

## Related Code and Contract

- [Render Graph contract](../Runtime/Rendering/RenderGraph.md)
- [RDG public API](../../Engine/Source/Runtime/RenderCore/Public/RDG.h)
- [RDG compiler](../../Engine/Source/Runtime/RenderCore/Private/RDG.cpp)
- [RDG native tests](../../Engine/Tests/Native/RenderCoreTests/Private/RDGTests.cpp)
