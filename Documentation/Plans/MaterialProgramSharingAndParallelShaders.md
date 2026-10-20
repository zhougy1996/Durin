# Material Program Sharing and Parallel Shaders Plan

Summary: Centralize semantic program subscriptions and submit material shader stages concurrently with bounded memory and deterministic assembly.

Last reviewed: 2026-10-20

Status: Completed
Completed: 2026-10-20

## Current Status

Both stages are complete. The manager owns
semantic subscriptions and independently dispatches completion callbacks. The
asset semantic map and `TryJoin` are removed. A two-request Shader window stores
results by index and drains submitted requests on failure/cancellation. Joining
subscribers inherit previously requested priority. Cancellation observers run
outside manager locks; weak captures avoid cycles through Shader handles.

Tested baseline: `8d21e6ab4933a3c00b5f6f1c5d07797b3f5ba227` plus this
implementation. Final per-file SHA-256 inventory:
`Build/MaterialProgramSharingAndParallelShaders/sources.json`, inventory digest
`d3b4e1b4aef414b6e844f4cbf042de26be87bfb5ecc0bddf0c520d4222530b3c`.
Only documentation changed after the final native receipts. All validation used
the `windows-msvc-x64` profile and established unsandboxed MSVC lane; Editor runs
used Debug with two CPU workers and two blocking-I/O workers.

Final receipts under `Build/MaterialProgramSharingAndParallelShaders/`:

- `compiler.xml`: MaterialCompilerTests, 100/100 passed.
- `lifecycle.xml`: material-compilation domain, lifecycle 18 cases and shutdown
  1 case passed. This includes canonical and semantic asset sharing and detached
  fanout cancellation/failure.
- `cook.xml`: MaterialCookTests, 7/7 passed.
- `cook-functional.xml`: six material/source-free Cook cases passed, including
  shuffled variant delivery and cancellation after result readiness.
- `isolated.xml`: all eight ownership cases plus admission saturation, cache-I/O
  cancellation and single-worker progress passed independently, 11/11, one slot.
- `inline-success.xml` and `inline-canceled.xml`: standalone successful and
  canceled callbacks passed with `InlineExecution=1` explicitly verified. Their
  callback paths are unchanged by the final editor-only priority propagation fix;
  final isolated execution also covers these cases.
- `qualification.xml` and `qualification.log`: the registered
  `ParallelProgramCompilationWorkloads` qualification passed. Raw final samples
  are in `samples.csv`; the initial pass remains in `samples-initial.csv`.
- Debug Game Engine build passed; build receipt
  `Build/.agent-state/logs/20261020-133713-625274-39440-cmake.log`.

The qualification collected five samples per workload without excluding a warm-up
sample. Single-material cold compilation median/p95 was 1.079/1.353 seconds;
four-material batch median/p95 was 2.766/2.838 seconds (1.446 materials/second at
the median); pixel-only edit median/p95 was 0.733/0.742 seconds. Cold single builds
compiled seven entries. Four compatible materials compiled nineteen entries and
reused nine vertex products; pixel edits compiled four fragment entries and reused
three vertex products in every sample. Per-program shader window peaked at two,
and retained/live output reservations peaked at 16 MiB. Process lifetime peak
working set reached 354.17 MiB; this includes runtime/compiler/cache allocations
and is not a per-request heap bound. These are Debug host observations, with no
old-implementation controlled timing baseline or speedup claim.

Affected selection expanded Engine ownership and the private test-target include
change to unrelated project tests. Bounded suites above cover every changed
caller and lifecycle boundary; Engine, Sandbox and RoadWeaver source/test roots
were audited. No public production ABI, persistent schema, GPU execution or
shared build graph changed, so an all-build/GPU gate is unnecessary. The only test
graph change adds private ShaderBuild test support to MaterialCompilerTests.
Long-lived behavior is documented in Material System and Material Shader Stages;
changed-document, all-plan and final diff audits passed.

## Goal

Keep workflow in the program manager and state in plain records. Asset services
retain object publication and canonical request coalescing; semantic sharing
belongs to the manager for every caller. Each subscription retains its own
cancellation, source attribution and configuration. A canceled subscriber cannot
cancel work still needed by another subscriber. Remove the reverse `TryJoin`
callback. Preserve force policy and completion/drain boundaries.

Submit a bounded number of shader stages per program, store results by canonical
request index and assemble only after submitted requests retire. Keep the 8 MiB
aggregate material result ceiling and explicitly bound temporary shader output
reservations. Avoid blocking waits inside continuations, including inline builds.
Persistent keys and payload formats remain unchanged.

## Implementation Stages

### Stage 0: Centralize program sharing and subscriptions

- [x] Move semantic identity lookup, subscriptions and fanout into the manager;
  remove `TryJoin`, the asset semantic map and consumer-transfer workflow.
- [x] Preserve per-subscriber cancellation, configuration and diagnostics/source
  mapping; cover shared builds across manager, synchronous and asset callers.

Completion: semantic sharing works without an asset-service callback, and canceling
either the first or a later subscriber preserves the remaining subscribers.

### Stage 1: Bound parallel shader submission and validate the final architecture

Depends on Stage 0.

- [x] Add bounded submission, indexed output slots, deterministic failure choice
  and cancellation/failure draining. Cover synchronous callback reentrancy and
  shuffled completion without multiplying the aggregate retained result limit.
- [x] Run compiler, lifecycle/shutdown, Cook and material Cook functional suites,
  isolated ownership regressions and the Debug Game Engine build. These cover the
  migrated callers and non-editor boundary; broaden only if evidence requires it.
- [x] Measure cold single-material and batch compilation, pixel-only reuse and
  output reservations. Record samples and limitations; do not infer a speedup from
  correctness-suite duration or contended diagnostic timings.
- [x] Update the owning material/shader documentation, validate changed documents
  and all plans, audit workspace consumers, and commit the validated result.

Follow the [build](../Agents/BuildAndRun.md), [test](../Agents/Testing.md) and
[qualification](../Development/Build/NativeTestQualification.md) workflows.
Use the established unsandboxed MSVC validation lane. An all-build is unnecessary
unless public ABI or build-graph changes broaden the affected boundary.
