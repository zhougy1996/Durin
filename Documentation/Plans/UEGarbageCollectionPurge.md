# UE Garbage Collection Purge Plan

Summary: Separate GC requests, Engine scheduling, synchronous reachability, and destruction draining.

Last reviewed: 2026-11-10

Status: Completed
Completed: 2026-11-10

## Current Status

All scoped stages are complete from `c904b5e77`. Final focused validation passed
for 15 reflection, GC, gameplay, and RoadWeaver targets, the independent Vulkan
mesh-fence case, normal exit smoke, and the final all-build check. The implemented
contracts live in [Garbage Collection](../Runtime/ObjectSystem/GarbageCollection.md)
and [Runtime Lifecycle](../Runtime/Application/RuntimeLifecycle.md).

The broad `fast-all` audit is not green: 87/99 targets passed. Eleven failing
targets reproduced the same failing cases when substituting the original
`c904b5e77` mark/sweep body, adapted only to the current signature, request slot,
and completion telemetry. This was a controlled algorithm comparison, not a clean
checkout baseline. The final collector was then restored and SHA256-verified.
The remaining RDG thread-placement failure passed both the comparison run and a
focused rerun with the final collector. These broader fixture/asset/scheduling
failures are recorded below rather than expanding this GC change into unrelated
asset-pipeline repairs.

The original SceneImportVulkan scenario stops at its asset-catalog assertion
before reaching the changed GC section. A separate case in the same target now
initializes real StaticMesh/Vulkan resources, blocks release behind a render
command, then verifies purge-only finalization without another reachability scan.

## Goal

Requests carry full-purge intent only. Execution chooses KeepFlags explicitly.
Engine owns policy and checked-frame accounting; Core owns collection telemetry
and pending destruction. Preserve synchronous, single-threaded reachability.
Do not add incremental reachability, concurrent GC, or speculative write barriers.
Asset residency remains an explicit lifetime edge independent of editor KeepFlags.

## Implementation Stages

### Stage 0: Align request and scheduling contracts

- [x] Separate request intent and execution KeepFlags; migrate consumers.
- [x] Move scheduling/configuration to Engine and distinguish checked and collected frames.
- [x] Correct object-handle write-barrier comments.

### Stage 1: Separate destruction advancement and draining

Depends on Stage 0.

- [x] Keep pending destruction independently of reachability scans.
- [x] Add bounded advancement and synchronous full drain; migrate shutdown and module release.
- [x] Cover request preservation, retention flags, frame semantics, and lifecycle callbacks.

### Stage 2: Validate and document

Depends on Stage 1.

- [x] Update the owning runtime contract.
- [x] Run affected discovery and relevant Core, Engine, asset, editor, and project tests.
- [x] Build all: public GC signatures and cross-module scheduling ownership affect foundational consumers.
- [x] Run one bounded normal shutdown smoke and documentation validators.

Follow [build](../Agents/BuildAndRun.md) and [test](../Agents/Testing.md) workflows.

## Validation Evidence

Environment: Windows, `windows-msvc-x64`, `Win64-Debug-DurinEditor`, 14 build/test
slots. Tested tree: `c904b5e77` plus this plan's changes. No incremental reachability
or concurrent collector is enabled.

- Final 15-target selection: `@domain=reflection+garbage-collection+gameplay+road-graph+road-scene`.
  Receipt: `Build/.agent-state/logs/20261110-203659-886329-26424-ctest.log`.
- Broad audit: `test fast-all`; receipt
  `Build/.agent-state/logs/20261110-202852-522821-28604-ctest.log`.
- Original-algorithm comparison: receipt
  `Build/.agent-state/logs/20261110-203428-630833-28952-ctest.log`.
  Reproduced failures: AssetEditorFamilyTests, EditorPropertyTests,
  EditorRenderingTests, MaterialCookTests, MaterialFunctionTests,
  MaterialPackageTests, PhysicsSceneTests, SkyBoxTests, SplineTests,
  StaticMeshMaterialTests, and TextureTests. New purge-contract tests are expected
  to fail with the old collector and are not comparison regressions.
- RDG rerun: `RenderContractTests FRDGTests.ParallelRecordingRetainsPayloadsAcrossWavesAndGraphDestruction`;
  receipt `Build/.agent-state/logs/20261110-204245-367623-34632-RenderContractTests.log`.
- Unrelated SceneImport catalog failure: receipt
  `Build/.agent-state/logs/20261110-203842-208933-39000-SceneImportVulkanTests.log`.

The expanded build also exposed two small existing compilation issues: a missing
ObjectLifecycle include in ContentBrowser's standalone test compilation and an
obsolete `.Message` access on a bool-returning material-test operation. Both were
corrected without changing runtime behavior.

Final acceptance receipts:

- `SceneImportVulkanTests FSceneImportVulkanTests.PendingMeshDestructionDrainsAfterRenderFenceWithoutAnotherMark`:
  `Build/.agent-state/logs/20261110-204324-629643-5224-SceneImportVulkanTests.log`.
- `LaunchStartupCharacterizationTests FLaunchStartupCharacterizationTests.BoundedTickExitUsesNormalApplicationShutdown --mode characterization`:
  `Build/.agent-state/logs/20261110-204327-500441-39804-ctest.log`.
- `build all`: `Build/.agent-state/logs/20261110-204330-701888-48264-cmake.log`.
- Changed-document validation, all-plan validation, and `git diff --check` passed.

Only documentation/status updates followed these final runtime receipts. Broad
`fast-all` and the original import scenario remain explicitly non-green as described
above; no unrelated asset-pipeline or thread-scheduler repair is claimed.
