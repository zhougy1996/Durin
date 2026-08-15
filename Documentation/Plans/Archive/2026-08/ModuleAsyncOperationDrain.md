# Module Async Operation Drain Plan

Summary: Add explicit module-owned asynchronous operation groups, abort semantics, Game Thread drain, and retained-callable destruction proof.

Last reviewed: 2026-08-15

Status: Archived
Completed: 2026-08-15

## Current Status

All stages are complete. Core now provides owner-bound asynchronous operation
groups, irreversible drain/cancel close, stable abort reasons, inherited task
scope ownership, selected Game Thread processing, and separate active-task,
typed-result, deferred-callable, and Worker-wrapper diagnostics. Module shutdown
closes groups before reflected-object teardown, permits mapped-library cleanup
through `FModuleShutdownContext`, then performs an owner-wide async audit before
the final feature audit and native release gate.

Six focused asynchronous retirement tests cover closed descendant admission,
stable cancellation reason, self-wait, selected Game Thread execution and
cancellation, destruction-sensitive Worker and Game Thread captures, retained
typed results, successful module cancellation, and fail-closed module timeout.
The complete `CoreConcurrencyTests` target passes 136 tests. The lasting Task
System and modular retirement contracts now own the implemented behavior, and
the parent roadmap activates the Engine authoring feature migration.

## Goal

Require every unload-relevant asynchronous operation to have explicit owner,
admission, completion, cancellation, continuation, and retained-callable
accounting so module shutdown can prove quiescence before native release.

## Scope

- Add Core-owned asynchronous operation groups associated with module owners.
- Define explicit drain and cancel policies plus stable abort reasons.
- Attribute worker tasks, descendants, and Game Thread continuations.
- Reject new work after group closure and diagnose recursive/self waits.
- Prove terminal execution and destruction of retained Plugin callables.
- Integrate operation diagnostics into `FModuleShutdownContext` and the existing
  fail-closed unload audit.

## Non-Goals

- Do not use module or feature availability as an operation cancellation token.
- Do not migrate Runtime Engine authoring feature families in this plan.
- Do not generalize specialized provider selection or registration semantics.
- Do not qualify repeated real DLL reload; that remains Milestone 5.

## Design Decisions and Invariants

- Operation state owns abort semantics; module state only closes ownership and
  authorizes or rejects native unload.
- An accepted root operation owns all inherited worker descendants and
  continuations until terminal state and callable destruction are both proven.
- Game Thread drain selects only the closing owner/scope and never pumps
  unrelated deferred work.
- A wait from execution owned by the same closing group returns a categorized
  self-wait result rather than blocking.
- Timeout is irreversible for the closing module generation and retains the
  native library with diagnostic counts.
- No task, continuation, queue, callback wrapper, custom deleter, or captured
  Plugin object may remain in Core storage after successful drain.

## Current Foundations and Gaps

- Core task scopes and cancellation tokens track execution but do not expose
  module-owner attribution or callable-destruction completion.
- `GameThreadDeferred` has bounded pumping, but current waits cannot select one
  module owner without processing unrelated work.
- `FModuleShutdownContext` now provides the correct mapped-library callback
  boundary but has no operation-group facade or async audit snapshot.
- Asset import and build systems have local drain mechanisms that will inform
  tests but are not migrated by this plan.

## Implementation Stages

### Stage 0: Freeze operation identity, state, and drain policy

- [x] Define operation-group identity, owner binding, state, abort reason,
  result, and diagnostic snapshot types.
- [x] Specify inheritance across root tasks, child tasks, and continuations.
- [x] Define Game Thread selected-drain ordering and self-wait rules.
- [x] Inventory Core callable-storage sites participating in the audit.

#### Acceptance Gate

- Feature retirement and operation abort remain distinct state machines.
- Terminal execution and callable destruction are independently observable.
- Every timeout and recursive path maps to an existing or extended fail-closed
  module result without rollback to `Active`.

### Stage 1: Implement Core operation groups

- [x] Add closing-aware root admission, descendant inheritance, and exact
  outstanding-operation accounting.
- [x] Add explicit drain/cancel request and stable abort-reason propagation.
- [x] Track retained callable storage through destruction completion.
- [x] Add deterministic worker concurrency, dynamic-child, timeout, and
  self-wait tests.

#### Acceptance Gate

- Closing rejects new roots while every previously accepted descendant reaches
  a terminal state.
- Successful drain reports zero operations and zero retained callables.
- Cancellation reason is operation-owned and never inferred from feature state.

### Stage 2: Integrate selected Game Thread continuation drain

- [x] Attribute deferred continuations to their operation group and owner.
- [x] Add owner/scope-selected pumping or cancellation without running unrelated
  deferred work.
- [x] Detach and destroy canceled callable captures before reporting success.
- [x] Test worker-to-Game-Thread chains, recursive pumping, supersession, and
  destruction-sensitive captures.

#### Acceptance Gate

- Selected drain reaches quiescence without reentering unrelated systems.
- A canceled continuation cannot run and its callable storage is destroyed
  while the owning library remains mapped.

### Stage 3: Integrate module shutdown and validate

- [x] Expose operation closure and diagnostics through
  `FModuleShutdownContext`.
- [x] Insert async drain/audit before the existing final feature audit and
  native release gate.
- [x] Add module-manager tests for cancel and timeout plus group-level drain,
  self-wait, retained-result, and retained-callable failure tests.
- [x] Build affected targets, run focused native tests, publish the lasting
  Core contract, and update the parent roadmap.

#### Acceptance Gate

- `FreeLibrary` is unreachable until synchronous features, asynchronous
  operations, Game Thread continuations, reflected objects, and retained
  callables all pass their audits.
- Injected async failure leaves the module `UnloadBlocked` with actionable
  owner and operation evidence.

## Validation Matrix

| Area | Validation | Evidence required |
| --- | --- | --- |
| Root admission | Core concurrency tests | Closing rejects later roots and accounts earlier roots |
| Inheritance | Barrier-controlled task tests | Children and continuations cannot escape their owner |
| Abort semantics | Core unit tests | Explicit stable reason reaches all canceled work |
| Game Thread drain | Selected deferred-executor tests | Only matching owner work is pumped or destroyed |
| Callable lifetime | Destructor-sensitive fixtures | Zero retained captures before success |
| Module audit | Module-manager tests | Timeout/self-wait retain mapping and diagnostic state |
| Regression | Focused task and module tests | Existing task cancellation and process shutdown remain valid |

## Definition of Done

- Core exposes explicit owner-bound operation groups with drain/cancel policy.
- Worker descendants and Game Thread continuations inherit ownership.
- Successful drain proves both terminal execution and callable destruction.
- Module shutdown consumes the async audit without weakening synchronous
  feature retirement or reflected-object ordering.
- Focused concurrency, failure-injection, build, and documentation validation
  gates pass.

## Deferred Follow-ups

- Runtime Engine authoring feature migration remains in
  `Documentation/Plans/Archive/2026-08/EngineAuthoringModularFeatureMigration.md` after this
  plan completes.
- Specialized registry audit and real DLL qualification remain Milestones 4
  and 5 of the parent roadmap.

## Related Documentation

- [Parent roadmap](../../../Roadmaps/Archive/2026-08/ModularFeatureAndDllUnloadSafety.md)
- [Modular feature and retirement contract](../../../Runtime/Core/ModularFeaturesAndModuleRetirement.md)
- [Task System](../../../Runtime/Core/TaskSystem.md)
- [Runtime Lifecycle](../../../Runtime/Core/RuntimeLifecycle.md)
- [Agent Testing Workflow](../../../Agents/Testing.md)
- [Agent Build and Run Workflow](../../../Agents/BuildAndRun.md)

## Related Code

- [Module manager interface](../../../../Engine/Source/Runtime/Core/Public/Modules/ModuleManager.h)
- [Modular feature interface](../../../../Engine/Source/Runtime/Core/Public/Modules/ModularFeature.h)
- [Task system interface](../../../../Engine/Source/Runtime/Core/Public/Threading/Task.h)
- [Task system implementation](../../../../Engine/Source/Runtime/Core/Private/Threading/Task.cpp)
