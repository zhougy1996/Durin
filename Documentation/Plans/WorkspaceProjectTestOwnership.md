# Workspace Project Test Ownership Plan

Summary: Declare workspace projects and project test roots once and use their ownership for bounded native-test impact analysis.

Last reviewed: 2026-09-07

Status: Completed
Completed: 2026-09-07

## Current Status

Implemented workspace membership, project test declarations, shared test output
paths, registry ownership and affected graph refresh. Project creation updates
the manifest transactionally; legacy workspaces without one retain their
existing scaffolding path. The actual configured RoadWeaver CMake-only impact
selects RoadGraphContractTests and RoadSceneIntegrationTests, never all.

Validation on Win64-Debug-DurinEditor:

- Configuration and 76 focused Python/schema regression tests passed, including
  19 workspace/ownership/freshness and transaction cases.
- Broader Python run: 429 passed, 3 skipped, 1 command-help snapshot failure.
  The identical snapshot mismatch was reproduced using an isolated export of
  unmodified HEAD 399cbecf5; it is a pre-existing validation limitation.
- Native aggregate built successfully on incremental retry. Initial large-build
  discovery timed out for 8 executables; retry resolved all discovery failures.
  Aggregate execution passed 80/83 targets. CoreConcurrencyTests failed a
  readiness assertion; VulkanRHIIntegrationTests and
  StaticMeshRenderPreparationVulkanTests crashed. All three passed the bounded
  serial CTest failed-target rerun (23.82 seconds). Do not describe the original
  parallel aggregate as an uninterrupted pass.
- Native build and aggregate diagnostics are retained in local Build/.agent-state
  logs dated 20260907-130717, 20260907-131433 and 20260907-131444.
- Documentation and all-plan validation passed. Changes are isolated from the
  pre-existing roads work, which was committed separately during this task.

## Goal

Load Engine, Sandbox, and RoadWeaver through Durin.dworkspace. Declare native
test roots in .dproject, preserve project CMake initialization, and select
project-owned tests for test-directory build changes without a global fallback.

## Decisions

- Workspace configuration owns project membership; project descriptors own
  native-test roots; target CMake continues to own sources and dependencies.
- Keep registered project metadata even when no test targets exist. Refresh
  configuration before executing affected selections whose test graph changed;
  explain remains read-only and reports stale metadata.
- Shared build/test infrastructure retains conservative aggregate coverage.
- Preserve pre-existing road implementation edits and isolate this task's commit.

## Implementation Stages

### Stage 0: Workspace and project declarations

- [x] Add validated workspace loading and project native-test declarations.
- [x] Migrate root project loading and project-creation tooling.
- [x] Supply shared native-test output paths through the build API.

Completion: configured projects and scaffolding consume the workspace file.

### Stage 1: Project-aware test selection

- [x] Publish project and target ownership in the configured registry.
- [x] Bound project-test edits, including additions and deletions.
- [x] Refresh test graph before execution and explain stale registry state.

Completion: project test CMake changes select project tests; shared changes
retain all; added tests cannot disappear behind an old registry.

### Stage 2: Validation and handoff

- [x] Add regression coverage for declarations, scaffolding, ownership and freshness.
- [x] Run relevant Python/CMake checks and native aggregate per agent testing guidance.
- [x] Update workspace/build/test documentation and validate plan/document links.
- [x] Review and commit isolated changes with Plan and Stage provenance.

Completion: acceptance checks pass or material blockers are explicitly recorded.
