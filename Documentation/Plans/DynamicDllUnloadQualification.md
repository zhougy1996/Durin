# Dynamic DLL Unload Qualification Plan

Summary: Qualify real native-module unload and reload across synchronous, asynchronous, destructor, and injected-failure paths.

Last reviewed: 2026-08-15

Status: Active
Completed:

## Current Status

Milestones 1-4 are complete and the repository callback audit has no unresolved
owner path. Core already has in-process retirement and operation-group tests,
but those tests install module objects directly and cannot prove that
`FreeLibrary` occurs only after every old-generation callable and destructor is
gone. Stage 0 will freeze a minimal real-DLL fixture and host-observation ABI
before implementation.

## Goal

Prove with a physically loaded test DLL that successful unload removes the old
native image, reload creates a distinct owner generation, no old-generation
code or destructor executes afterward, and every injected quiescence failure
prevents native release with actionable diagnostics.

## Scope

- Build and deploy one test-only dynamic `IModuleInterface` fixture through the
  production module loading path.
- Exercise synchronous feature calls, specialized owner-gated callbacks,
  worker-to-Game-Thread task chains, retained typed results, queued callable
  destruction, and module-instance destruction.
- Repeat unload/reload with generation and native-image evidence.
- Inject invocation, resource, task, deferred-callable, result-handle, and
  reflected-object failures and prove the DLL remains mapped.
- Run deterministic stress under the strongest available Windows diagnostics.

## Non-Goals

- Do not turn the fixture into a general Plugin SDK or stable third-party ABI.
- Do not make production modules depend on test control surfaces.
- Do not use process exit or operating-system image cleanup as unload evidence.
- Do not qualify arbitrary user Plugin behavior outside the owner, task, and
  shutdown contracts implemented by Milestones 1-4.

## Design Decisions and Invariants

- The fixture is a real `MODULE`/DLL artifact loaded by
  `FModuleManager::LoadModule`; direct `InstallStartedModule` tests remain unit
  coverage and cannot satisfy this plan.
- Host observations live in the test executable or a process-resident support
  library. The DLL may publish POD events through a frozen test-only boundary,
  but the host never calls a function pointer after unload.
- Every event includes owner generation and fixture instance serial. The host
  rejects an event from a prior generation after successful reload.
- Destructor-sensitive captures emit their final event while mapped. A
  successful unload requires all expected destructor events before the native
  image disappears.
- A failure injection closes admission irreversibly. The expected result is
  `UnloadBlocked`, a still-mapped image, and categorized snapshot evidence; the
  test does not attempt to reactivate that generation.
- Tests serialize module-manager and DLL-image mutation and use bounded
  barriers/timeouts. No wall-clock sleep is synchronization evidence.

## Current Foundations and Gaps

| Foundation | Existing evidence | Qualification gap |
| --- | --- | --- |
| Module manager | Production `LoadLibrary`/`FreeLibrary` path and categorized unload results | Native tests mostly install in-process module objects |
| Modular features | Race, stale generation, self-wait, and retained-resource unit tests | No old-image execution proof after physical reload |
| Operation groups | Worker/GT drain, result, callable destruction, timeout tests | No callable compiled into a separately unloadable image |
| Specialized registries | Owner gates and persistent resource leases across audited registries | No real DLL registration/destructor fixture |
| Test tooling | Native target deployment, runtime dependency closure, qualification labels | No dedicated unload fixture artifact and serialized stress target |

## Implementation Stages

### Stage 0: Freeze the real-DLL fixture and observation boundary

- [ ] Select the smallest existing CMake/native-test deployment mechanism that
  produces and deploys a loadable test module beside its qualification target.
- [ ] Define the fixture control states, POD host event schema, synchronization
  barriers, generation/instance evidence, and native-image presence probe.
- [ ] Map every required success and failure scenario to one observable unload
  result and one pre-`FreeLibrary` destruction assertion.
- [ ] Record Windows serialization, timeout, crash-diagnostic, and artifact
  retention requirements.

#### Acceptance Gate

- The fixture uses the production loader and the host can distinguish mapped,
  unmapped, and reloaded generations without retaining Plugin code pointers.
- Every scenario has deterministic entry, release, and expected evidence.

### Stage 1: Implement the fixture and successful lifecycle qualification

- [ ] Add the real dynamic fixture, its process-resident host probe, runtime
  deployment dependency, and a dedicated native qualification target.
- [ ] Exercise an admitted synchronous feature call racing unload and prove
  unload waits for return before destroying the instance and image.
- [ ] Exercise a worker-to-Game-Thread chain plus destructor-sensitive queued
  captures and prove successful drain destroys all old-generation storage.
- [ ] Unload and reload repeatedly; verify increasing owner generations,
  distinct instance serials, correct new-generation dispatch, and zero events
  from earlier generations.

#### Acceptance Gate

- Repeated successful cycles prove the old image is absent between cycles and
  only the new generation can publish or execute afterward.

### Stage 2: Qualify fail-closed unload diagnostics

- [ ] Inject an admitted synchronous invocation timeout and retained specialized
  registry resource; verify categorized failure and mapped image.
- [ ] Inject worker, Game Thread deferred callable, typed-result handle, and
  callable-destruction retention failures; verify exact async snapshots.
- [ ] Inject reflected-object drain rejection and shutdown callback failure;
  verify module-instance destruction and `FreeLibrary` do not run.
- [ ] Cover wrong-thread and recursive self-unload requests without deadlock.

#### Acceptance Gate

- Every injected failure leaves the native image mapped, exposes actionable
  owner/group evidence, and executes no post-failure reactivation path.

### Stage 3: Stress, diagnostics, and roadmap completion

- [ ] Run deterministic repeated unload/reload stress under available Windows
  loader diagnostics, Application Verifier, ASan, or the strongest supported
  equivalent and record the selected environment.
- [ ] Run the dedicated qualification target, affected Core tests, full
  `fast-all`, documentation validation, and a full build.
- [ ] Move final fixture and diagnostic rules into the Runtime Core contract,
  complete the parent roadmap, and retain this plan as provenance.

#### Acceptance Gate

- Stress completes without stale-generation events, use-after-unload,
  loader-lock faults, leaked callable storage, or unexplained mapped images.
- All roadmap completion criteria and affected regression gates pass.

## Validation Matrix

| Concern | Required evidence |
| --- | --- |
| Physical unload | Native image absent after successful `UnloadModule` |
| Reload identity | Owner generation and fixture instance serial both change |
| Invocation race | Unload waits for admitted call; late call is rejected |
| Async chain | Worker and selected Game Thread publication drain deterministically |
| Destructor safety | All Plugin capture/provider/module destructors report before unmap |
| Failure closure | Each injected audit failure returns its category and keeps image mapped |
| Old-generation isolation | No event or dispatch from an earlier generation after reload |
| Stress | Repeated serialized cycles pass under selected Windows diagnostics |
| Regression | Core concurrency/utility, `fast-all`, docs, and full build pass |

## Definition of Done

- A real test DLL covers the complete successful unload/reload lifecycle.
- Every roadmap-named failure family demonstrably prevents `FreeLibrary`.
- Old-generation executable and destructor storage cannot run after reload.
- Stress and repository regressions pass, the lasting contract records the
  qualified boundary, and the parent roadmap is completed.

## Deferred Follow-ups

- Cross-version third-party Plugin ABI compatibility remains outside this
  roadmap.
- Platform-specific qualification beyond Windows requires a separate plan when
  another supported dynamic-loader environment exists.

## Related Documentation

- [Parent roadmap](../Roadmaps/ModularFeatureAndDllUnloadSafety.md)
- [Modular features and module retirement](../Runtime/Core/ModularFeaturesAndModuleRetirement.md)
- [Task System](../Runtime/Core/TaskSystem.md)
- [Native C++ Tests](../Development/Build/NativeTests.md)
- [Agent Testing Workflow](../Agents/Testing.md)
- [Agent Build and Run Workflow](../Agents/BuildAndRun.md)

## Related Code

- [Module manager interface](../../Engine/Source/Runtime/Core/Public/Modules/ModuleManager.h)
- [Module manager implementation](../../Engine/Source/Runtime/Core/Private/Modules/ModuleManager.cpp)
- [Module test context](../../Engine/Source/Runtime/Core/Public/Modules/ModuleTestContext.h)
- [Modular feature tests](../../Engine/Tests/Native/CoreTests/Private/ModularFeatureTests.cpp)
- [Async operation tests](../../Engine/Tests/Native/CoreTests/Private/AsyncOperationGroupTests.cpp)
- [Native test configuration](../../Engine/Tests/Native/CMakeLists.txt)
