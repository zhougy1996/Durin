# Material Compile Manager Flow Plan

Summary: Remove per-build Operation objects and centralize material compilation flow behind manager submission and completion interfaces.

Last reviewed: 2026-10-20

Status: Completed
Completed: 2026-10-20

## Current Status

Both stages are complete. The program manager admits opaque handles and advances
plain records; asset subscriptions, synchronous callers, Cook and test consumers
have migrated. Operation files and inheritance are removed. Asset result
processing owns its counter updates, including exceptional publication exits.
Finish-all drains callback-generated submissions as well as its initial set.

Tested baseline: `833ebc4755cc22b6db0c680be9e8344b3082df19` plus this
implementation. Final source fingerprint:
`019ada420200d8bf00b39115ae7c7f9f72c175149c329482aa138b4154abc9f8`.
The per-file inventory (including removed files) is retained in
`Build/MaterialCompileManagerFlow/sources.json`. Only documentation changed
after native validation.

Validation used the `windows-msvc-x64` profile, Debug Editor unless noted:

- `test MaterialCompilerTests`: 97/97 passed; receipt
  `Build/MaterialCompileManagerFlow/compiler.xml`.
- `test '@domain=material-compilation'`: both lifecycle (18 cases) and shutdown
  (1 case) targets passed; receipt `Build/MaterialCompileManagerFlow/lifecycle.xml`.
- `test MaterialCookTests`: 7/7 passed; receipt
  `Build/MaterialCompileManagerFlow/cook.xml`.
- `test CookFunctionalTests 'FCookFunctionalTests.*Material*:FCookFunctionalTests.CookedOnlyPreparationSelectsAvailableVariantsWithoutProducers'`:
  6/6 passed, including concurrent variants, shuffled delivery, cancellation after
  readiness and source-free preparation; receipt
  `Build/MaterialCompileManagerFlow/cook-functional.xml`.
- All five `FMaterialProgramOwnershipTests` cases plus admission saturation,
  cancellation during cache I/O and single-worker progress ran independently
  with `--isolate --test-jobs 1`: 8/8 passed; receipt
  `Build/MaterialCompileManagerFlow/isolated.xml`.
- Standalone `FMaterialProgramOwnershipTests.CanceledSubmissionSupportsInlineAndTaskExecution`
  passed and its `InlineExecution=1` property was verified; receipt
  `Build/MaterialCompileManagerFlow/inline.xml`. The full suite exercises the
  task-backed branch of the same case.
- `build --preset Win64-Debug-DurinGame Engine` passed; receipt
  `Build/.agent-state/logs/20261020-130958-386674-28152-cmake.log`.

Affected selection expanded the Engine module to unrelated asset, world, physics
and GPU targets. The bounded suites above cover all migrated caller paths and
their lifecycle boundaries; source/test roots in Engine, Sandbox and RoadWeaver
were audited for remaining consumers. No shared public API, Shader generation,
persistent schema or GPU execution changed, so an all-build/GPU gate is not
required. All native validation used the established unsandboxed MSVC lane.
Changed-document validation, all-plan validation and final diff/symbol audits
passed. Lasting ownership and interface rules are recorded in
[Material System](../Runtime/Rendering/MaterialSystem.md#compile-lifecycle-and-cooked-programs).

## Goal

Delete `FMaterialBuildOperation` and its inheritance hierarchy. A process-wide
program compiling manager owns admission, translation, Shader submission,
assembly, cancellation, priority and retirement. Opaque compile handles identify
plain state records; records have no workflow methods or virtual dispatch.
Editor asset subscriptions, synchronous tooling and Cook use `SubmitJobs`,
`FinishCompilation`, `CancelCompilation` and completion processing. The asset
service keeps object-local qualifiers/publication separate and accounts for
consumed results internally. Preserve resource ceilings, canonical/semantic
sharing, force policy, source mapping, inline completion, and callback drain
boundaries. Shader submission order and persistent formats remain unchanged.

## Implementation Stages

### Stage 0: Replace operations with manager-owned compilation records

- [x] Implement plain records, opaque handles and manager-owned submission,
  continuation, cancellation, waiting and retirement.
- [x] Migrate asset subscriptions, synchronous calls, Cook and every test
  consumer in the workspace; remove Operation types/files and inheritance.
- [x] Centralize result accounting and align asset service interface vocabulary
  with submission, processing, cancellation and finish operations.

Completion: callers cannot construct, reserve or start a per-build workflow
object; all production paths use manager submission and records store state only.

### Stage 1: Validate completion contracts and document the final interfaces

Depends on Stage 0.

- [x] Exercise admission saturation/recovery, cancellation before submission and
  during I/O, isolated regressions, shared consumers, inline completion and drain.
- [x] Run MaterialCompilerTests, MaterialCompileLifecycleTests,
  MaterialCompileShutdownTests, MaterialCookTests and the affected material Cook
  functional cases. These cover all migrated production and test call sites.
- [x] Build Debug Game Engine to verify non-editor stubs and dependencies.
- [x] Update the runtime contract and validate changed documents, all plans,
  final diff and remaining symbol references.

Use the [build](../Agents/BuildAndRun.md) and
[test](../Agents/Testing.md) workflows. Inspect affected coverage before execution;
no all-build or GPU gate is required unless new evidence broadens the scope.
Native MSVC validation uses the previously established unsandboxed lane for C1902.
