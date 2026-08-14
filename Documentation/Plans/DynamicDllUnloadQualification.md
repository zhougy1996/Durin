# Dynamic DLL Unload Qualification Plan

Summary: Qualify real native-module unload and reload across synchronous, asynchronous, destructor, and injected-failure paths.

Last reviewed: 2026-08-15

Status: Completed
Completed: 2026-08-15

## Current Status

All stages are complete. The production loader repeatedly maps and unmaps a
standalone test DLL, successful retirement drains admitted synchronous and
Worker-to-Game-Thread execution before destructor and image release, and every
injected quiescence failure keeps the native image mapped with categorized
diagnostics. Thirty-two additional unload/reload cycles passed with increasing
owner generations and instance serials and no new events from earlier
generations. The successful qualification also passed under Application
Verifier with Heaps, Handles, Locks, and TLS checks enabled.

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

- [x] Select the smallest existing CMake/native-test deployment mechanism that
  produces and deploys a loadable test module beside its qualification target.
- [x] Define the fixture control states, POD host event schema, synchronization
  barriers, generation/instance evidence, and native-image presence probe.
- [x] Map every required success and failure scenario to one observable unload
  result and one pre-`FreeLibrary` destruction assertion.
- [x] Record Windows serialization, timeout, crash-diagnostic, and artifact
  retention requirements.

#### Completion Evidence

- `DynamicUnloadFixture` is a test-only `MODULE` target outside the scanned
  native-test source tree. `DynamicDllUnloadQualificationTests` declares it as
  a runtime-only target, so the standard derived runtime closure supplies one
  build/deploy writer and places the DLL beside the test executable without a
  target-owned copy command.
- A shared test contract declares a host feature and a fixture feature. The
  host feature lives in the executable, allocates monotonically increasing
  instance serials, stores POD lifecycle events, exposes bounded barriers, and
  never retains a DLL function pointer. The fixture registers only through its
  production `FModuleContext` and reports events by bounded invocation of the
  process-resident host feature.
- Every event carries the fixture instance serial and phase. Owner generation
  is read from the manager's module record, while `GetModuleHandleW` probes the
  exact deployed filename without incrementing the loader reference count.
- Success cases cover admitted-call release, drained Worker-to-Game-Thread
  publication, destructor events, unmap, reload, and old-generation silence.
  Failure cases map to invocation timeout, retained owner resource, active
  worker timeout, retained typed result/deferred storage, reflected rejection,
  shutdown exception, wrong thread, and recursive unload.
- The qualification target is excluded from ordinary aggregates, runs only via
  explicit qualification admission, and uses events/condition variables plus
  bounded manager timeouts. Failed runs retain the standard native-test sandbox
  and logs; no sleep is used for synchronization.

#### Acceptance Gate

- The fixture uses the production loader and the host can distinguish mapped,
  unmapped, and reloaded generations without retaining Plugin code pointers.
- Every scenario has deterministic entry, release, and expected evidence.

### Stage 1: Implement the fixture and successful lifecycle qualification

- [x] Add the real dynamic fixture, its process-resident host probe, runtime
  deployment dependency, and a dedicated native qualification target.
- [x] Exercise an admitted synchronous feature call racing unload and prove
  unload waits for return before destroying the instance and image.
- [x] Exercise a worker-to-Game-Thread chain plus destructor-sensitive queued
  captures and prove successful drain destroys all old-generation storage.
- [x] Unload and reload repeatedly; verify increasing owner generations,
  distinct instance serials, correct new-generation dispatch, and zero events
  from earlier generations.

#### Completion Evidence

- `DynamicUnloadFixture` is compiled as a separate DLL, loaded by the
  production filename convention, and deployed through the native-test runtime
  closure. The host verifies mapping with `GetModuleHandleW` and observes no
  image after every successful `UnloadModule`.
- A synchronous fixture invocation blocks through the process-resident host.
  Unload closes admission, a late invocation reports `Unavailable`, and the
  admitted call exits before shutdown, module destruction, and physical unmap.
- `DynamicUnloadFixture.Drained` owns a Worker result and
  `GameThreadDeferred` publisher. Module shutdown releases its result handles;
  the manager pumps the publisher, destroys the Plugin capture, audits zero
  async storage, destroys the module instance, and only then frees the image.
- Three physical load generations complete in one process. Both owner
  generation and host instance serial increase, while the old feature is never
  invoked after retirement.
- `DynamicDllUnloadQualificationTests --mode qualification` and
  `CoreConcurrencyTests` passed after a fresh configure.

#### Acceptance Gate

- Repeated successful cycles prove the old image is absent between cycles and
  only the new generation can publish or execute afterward.

### Stage 2: Qualify fail-closed unload diagnostics

- [x] Inject an admitted synchronous invocation timeout and retained specialized
  registry resource; verify categorized failure and mapped image.
- [x] Inject worker, Game Thread deferred callable, typed-result handle, and
  callable-destruction retention failures; verify exact async snapshots.
- [x] Inject reflected-object drain rejection and shutdown callback failure;
  verify module-instance destruction and `FreeLibrary` do not run.
- [x] Cover wrong-thread and recursive self-unload requests without deadlock.

#### Completion Evidence

- The failure target uses a distinct executable from successful physical-unmap
  qualification. Each irreversible scenario has a unique logical module record
  pointing at the same real fixture DLL, so one deliberately blocked generation
  cannot mask or reactivate another.
- An admitted synchronous barrier produces
  `FeatureInvocationDrainTimeout` with one in-flight invocation; a retained
  specialized-registry lease produces `OutstandingFeatureAudit` with one
  retained owner resource.
- An uncancelable fixture Worker produces `AsyncOperationDrainTimeout` with one
  active task. A completed typed task whose result handle remains in the mapped
  module produces the same category with one retained result. A queued
  `GameThreadDeferred` publisher drained without Game Thread identity produces
  `AsyncOperationUnsupportedThread` with retained callable storage.
- Reflected drain rejection and an injected shutdown exception preserve the
  module instance and omit its destructor event. Wrong-thread unload leaves the
  module active and is followed by a successful control-thread cleanup;
  recursive unload reports `RecursiveOwnedExecution` without deadlock or
  module destruction.
- Every failed record is `UnloadBlocked` with non-null native handle and module
  instance, and `GetModuleHandleW` still finds the DLL. The isolated
  `DynamicDllUnloadFailureQualificationTests --mode qualification` target
  passes alongside the successful unload target.

#### Acceptance Gate

- Every injected failure leaves the native image mapped, exposes actionable
  owner/group evidence, and executes no post-failure reactivation path.

### Stage 3: Stress, diagnostics, and roadmap completion

- [x] Run deterministic repeated unload/reload stress under available Windows
  loader diagnostics, Application Verifier, ASan, or the strongest supported
  equivalent and record the selected environment.
- [x] Run the dedicated qualification target, affected Core tests, full
  `fast-all`, documentation validation, and a full build.
- [x] Move final fixture and diagnostic rules into the Runtime Core contract,
  complete the parent roadmap, and retain this plan as provenance.

#### Completion Evidence

- The success target adds 32 serialized physical load/unload cycles after its
  three scenario generations. Every cycle observes a higher owner generation
  and fixture instance serial, an unmapped DLL after unload, and unchanged
  event counts for all earlier scenario instances.
- `DynamicDllUnloadQualificationTests --mode qualification` passed under
  Windows Application Verifier with Heaps, Handles, Locks, and TLS enabled.
  The settings were disabled after the run and verified absent.
- Both dedicated qualification targets, affected Core tests, the ordinary
  native-test aggregate, `fast-all`, a full build, and all documentation
  validators passed on the final tree.
- The Runtime Core contract now records the physical-DLL qualification and
  fail-closed evidence, and the parent roadmap is completed.

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
