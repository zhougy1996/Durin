# Runtime Dynamic Material Instances Plan

Summary: Separate persistent material instance editing from transient runtime parameter overrides.

Last reviewed: 2026-09-18

Status: Completed
Completed: 2026-09-18

## Current Status

Completed the bounded lifecycle/API slice after the user waived the remaining M13
qualification gates. Existing asset serialization and editor APIs remain compatible.
The runtime contract is published in [Material System](../Runtime/Rendering/MaterialSystem.md#persistent-and-dynamic-instance-lifetimes).
M8 remains open for atomic batches and measured scalability; this plan does not
claim those deliverables.

## Selected Design

Keep DMaterialInstance with an immutable native lifecycle selected by the
CreateDynamic factory. Ordinary construction remains the asset editing entry.
Dynamic instances have transient object flags, a fixed valid asset parent, and
only numeric/texture overrides. Reject persistence, duplication, reflected edits,
static configuration edits, reparenting, and asset-parent references to dynamic
instances. Callers retain instances through ordinary strong object references;
Outer is naming context, not an automatic GC root.

Dynamic instances reuse the parent's accepted program and complete render state,
including pending/failure transitions, in Editor and cooked Game. They never
schedule compilation or mark a package dirty. Parameter admission follows the
accepted compiled contract. Reuse existing typed storage, GC reference traversal,
dependency queries, and proxy publication. Do not introduce a second renderer
path or change persisted instance schemas. Dynamic-to-dynamic parenting and
atomic batch APIs/performance optimization are deferred from this bounded slice
of M8.

## Implementation Stages

### Stage 1: Separate instance lifecycles and qualify runtime behavior

- [x] Add explicit dynamic creation, mutation and persistence boundaries.
- [x] Integrate parent accepted state, dependency updates and compiler exclusion.
- [x] Cover independent overrides, asset cleanliness, lifetime, invalid operations,
  parent changes/failure and cooked parameter updates with focused native tests.
- [x] Run affected CPU tests and the required all build; record actual results.
- [x] Publish the runtime contract and update the material roadmap.

Validation follows [Testing](../Agents/Testing.md) and
[Build and run](../Agents/BuildAndRun.md). Old M13 qualification is not reopened.
GPU algorithms are unchanged; no broad GPU qualification is required for this slice.

## Validation Evidence (2026-09-18)

- `DevTool.bat test MaterialRuntimeTests FMaterialInstanceTests.Dynamic*`: 3/3
  passed, including independent values, clean owning packages, forbidden edits and
  duplication, parent accepted generation changes through a bound mesh proxy,
  broken-chain fallback, and parent/texture GC retention and release.
  Receipt: `20260918-180218-341441-23704-MaterialRuntimeTests.log`.
- `DevTool.bat build --target all`: passed for Win64-Debug-DurinEditor, including
  workspace project targets. Receipt: `20260918-180235-064282-9172-cmake.log`.
- `DevTool.bat test affected --explain` selected the broad Engine dependency
  closure, including unrelated GPU integrations. Used the bounded affected
  `DevTool.bat test "@domain=material,kind=feature" --test-jobs 4 --report`
  selection instead: all 11 targets passed (compilation, compiler, Cook, editing,
  functions, graph, packages, runtime, thumbnails and static mesh materials).
  Receipt: `20260918-180326-940400-30124-ctest.log`.
- After adding a transient child to the authored Cook fixture and explicit cooked
  package-cleanliness assertions, `DevTool.bat test MaterialCookTests`: 7/7 passed.
  Receipt: `20260918-180502-529009-22028-MaterialCookTests.log`. Cooked coverage
  uses the native test process with cooked asset configuration and graph-stripped
  Win64 Game payloads; a standalone Game application was not launched.
- Changed-document, all-plan and all-roadmap validation passed. Build/test receipts
  are under `Build/.agent-state/logs/` in this checkout. Old M13 qualification
  remains waived; no broad GPU or performance qualification was run.
