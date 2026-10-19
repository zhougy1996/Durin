# Editor Ownership Simplification Plan

Summary: Remove repeated editor selection from tool dispatch and ordinary UI transactions, and centralize World context lifecycle.

Last reviewed: 2026-10-19

Status: Archived
Completed: 2026-10-19

## Current Status

Implementation and qualification are complete. Tool services bind a
weak editor owner at construction; provider scopes retain only invocation
context. WorldOutliner actions use implicit editor scopes. World initialization
and shutdown own private context registration; previews no longer register
manually. Singleton refresh helpers are renamed across all consumers, and
retained history drops recording-only owner linkage.

Win64-Debug-DurinEditor validation outside the sandbox passed 910 tests across
17 targets, including EditorOperationTests (132), WorldTests (175), Python
tool/reflection integration (47/12), MCP integration/jobs (19/14), editor
host/property/shell, material graph/interaction, viewport, asset reload, level
mutation, RoadWeaver, Sandbox and Engine header coverage. World tests cover
automatic registration/removal, failed-initialization non-publication and
shutdown of an unselected preview before engine dependencies are retired.
Tested source is plan commit `3936774ec` plus this stage's implementation and
fixtures. After the final retirement-order change, World, editor and Python
suites were rerun; later changes only update documentation and evidence.

All production targets passed the final integration build. Preserved receipt:
`Build/EditorOwnershipSimplificationValidation/20261019-115003-728112-31956-cmake.log`.
Exact test receipt files/counts are recorded in
`Build/EditorOwnershipSimplificationValidation/receipts.md`, outside rolling log
retention. Changed documents and all plans validate; diff and removed-symbol
checks pass across all three workspace projects.

The affected-test explanation expands Engine ownership changes to unrelated
asset/render/GPU targets. Named CPU suites cover the changed lifecycle/tool/UI
behavior; the final production all build covers the shared header integration.

## Selected Decisions

- Bind a tool service to its editor once at construction. Keep its endpoint
  detached and thread-safe: only GameThread dispatch may resolve the weak owner;
  closed endpoints cannot reach a replacement editor.
- Callback binding tracks exact invocation context, not an editor selection.
  Preserve copied-context rejection, nested restoration and thread checks.
- Ordinary LevelEditor actions use the active-editor scoped transaction entry.
  Keep explicit transactors in reusable property/material change backends,
  independent buffer tests and suspended Python scopes where exact ownership is
  part of their contract.
- Engine and World alone maintain context registration. Successful World
  initialization registers its engine-owned World; shutdown unregisters it.
  Keep MainWorld, EditorWorld and PlayWorld role pointers and borrowed queries.
  Engine retirement shuts down remaining registered Worlds before engine
  services, including initialized Worlds that never became MainWorld.
- Rename singleton subsystem refresh helpers and drop recording-only owner
  linkage when a transaction becomes retained history.

## Implementation Stages

### Stage 0: Record scope and acceptance

- [x] Record ownership decisions, preserved safeguards and validation scope.
- [x] Validate and commit this plan before implementation.

### Stage 1: Simplify ownership and qualify consumers

Depends on Stage 0.

- [x] Bind tool ownership once and simplify exact callback context binding.
- [x] Migrate ordinary UI scopes, singleton refresh consumers and retained
  transaction linkage across Engine, Sandbox and RoadWeaver source/test roots.
- [x] Internalize World context mutation and cover automatic registration,
  failed initialization and shutdown cleanup.
- [x] Update owning contracts and verify affected-test selection.
- [x] Pass EditorOperationTests, WorldTests, Python integration, MCP, relevant
  property/material/viewport and project consumer tests.
- [x] Build all production targets once at final acceptance: Engine public
  lifecycle/API access changes and editor consumers cross module boundaries.
- [x] Validate changed documents and all plans, review the diff and removed
  symbols, record receipts and commit with plan/stage provenance.

Validation follows [build](../../../Agents/BuildAndRun.md) and
[test](../../../Agents/Testing.md) using Win64-Debug-DurinEditor. Application-hosted,
GPU and complete UE transaction serialization qualification are outside this
ownership-only change. The previous plan remains historical completion evidence;
lasting contracts stay in their owning documentation domains.
