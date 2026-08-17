# Native Test Application Host Simplification Plan

Summary: Reduce authoring and maintenance complexity around native-test execution hosts without changing the validated LaunchServices process model, CTest ownership, result fidelity, or cleanup guarantees.

Last reviewed: 2026-08-18

Status: Active
Completed:

## Current Status

The macOS native-test application host is implemented and committed in
`026eefca`. It provides the required LaunchServices admission, internal-volume
artifact layout, exact child-result propagation, cancellation, bounded
retention, and CTest/DevTool integration. Those behaviors are validated and are
not candidates for removal.

The remaining complexity is concentrated in four implementation seams:

- test authors must set `DURIN_TEST_EXECUTION_HOST` separately and before
  `durin_finalize_native_test`, creating an avoidable ordering rule;
- `durin_finalize_native_test` owns metadata validation, platform resolution,
  output relocation, runtime layout, and registry state in one function;
- the controller/host protocol exposes positional string vectors and repeated
  status strings instead of typed request and result boundaries;
- DurinDevTool's exact-target path contains an execution-host special case
  instead of delegating to one target execution strategy.

This plan simplifies those seams incrementally. Each stage must preserve the
existing wire behavior or provide an explicit compatibility transition, and
must pass direct and application-hosted regression gates before the next stage.

## Goal

Make the application-host framework easier to declare, read, test, and extend
while preserving all observable behavior and safety properties of the current
implementation. A test author should express the execution-host requirement in
the same structured finalization call as other native-test metadata, and host
maintainers should work with typed protocol and process abstractions rather
than positional fields and repeated cleanup branches.

## Scope

- Add `EXECUTION_HOST direct|application` to
  `durin_finalize_native_test` and make it the canonical authoring surface.
- Preserve a bounded compatibility path for existing target-property
  declarations while migrating repository targets and helpers.
- Split execution-host resolution and application artifact-layout setup out of
  native-test metadata finalization.
- Replace positional request access and free-form result states with typed
  protocol structures and validated conversions.
- Introduce small macOS-local RAII wrappers for file descriptors, spawn
  actions, and owned child processes where they remove duplicated cleanup.
- Centralize DurinDevTool's choice between focused executable execution and a
  launcher-aware CTest registration.
- Reassess the misleading `Durin.NativeTestDirect.<Target>` registration name
  and change it only with an explicit compatibility and migration decision.
- Preserve or improve focused unit, policy, concurrency, cancellation, report,
  qualification, and aggregate validation.

## Non-Goals

- Removing the controller/Host process split or accepting `open` status as the
  test result.
- Replacing CTest, GoogleTest, NativeTestSupport, or DurinDevTool selection.
- Removing nonce, canonical-path, PID, environment, atomic-publication,
  retention, timeout, signal-forwarding, or exact-exit validation.
- Moving fixture ownership into the generic host; tests continue to own
  ApplicationCore, `GApp`, windows, surfaces, RHI, and renderer setup.
- Moving application-hosted artifacts back to the external workspace volume.
- Designing a general-purpose cross-platform process or serialization library.
- Fixing `EditorGridVulkanTests` renderer failures, GUI-session provisioning,
  Windows qualification, or the open gates in the original host plan.
- Changing product application packaging, signing, notarization, or launch.

## Design Decisions and Invariants

### One canonical authoring call

- `durin_finalize_native_test(... EXECUTION_HOST application ...)` becomes the
  preferred declaration.
- Omitted `EXECUTION_HOST` continues to mean `direct`.
- A pre-existing `DURIN_TEST_EXECUTION_HOST` property may be read during a
  documented transition, but specifying both surfaces with different values
  is a target-specific configuration error.
- Final target properties remain available for registry generation and CTest
  discovery; the simplification changes authorship, not configured metadata.

### Separate metadata from platform layout

- Metadata normalization remains platform-neutral.
- Execution-host semantic validation, platform resolution, and macOS artifact
  layout become named internal functions with focused inputs and outputs.
- Only the resolved macOS application host may select the `/private/tmp`
  layout and `@loader_path` behavior.
- Generator expressions must be evaluated exactly once; probes must reject
  generated paths containing a literal `$<CONFIG>`.

### Typed protocol with stable behavior

- Introduce `FRequest` and typed result stage/status representations.
- Positional binary fields may remain wire-compatible during the refactor, but
  field ordering is contained inside encode/decode functions.
- Controller and Host code consume validated structures and do not index raw
  request vectors or compare arbitrary result strings.
- Unknown wire values, embedded NULs, stale nonces, inconsistent exit/signal
  combinations, and trailing data remain hard launcher failures.

### Small local process abstractions

- RAII wrappers remain private to the macOS native-test host implementation.
- Wrappers expose ownership and cleanup, not policy: bounded grace periods,
  exit mapping, evidence retention, and escalation decisions remain explicit.
- No wrapper may kill by name, process group, or unvalidated PID.

### Behavior before naming cleanup

- Direct targets retain their focused executable path and ordinary aggregate
  membership.
- Application targets retain launcher-aware CTest execution for exact and set
  selections.
- Renaming the whole-target CTest registration is optional until Stage 0
  proves that CI, saved commands, and tooling can migrate without ambiguity.

## Current Foundations and Gaps

- Registry schema 3 already records semantic and resolved execution hosts.
- CMake probes cover execution-host validation and `TEST_LAUNCHER` behavior.
- Protocol/controller tests cover round trips, malformed records, result
  consistency, ordinary failure, crash, cancellation, and concurrency.
- End-to-end application cases cover environment transfer, reportable success,
  isolation, stress, and concurrent CTest case execution.
- `durin_finalize_native_test` is currently the only safe point at which final
  target metadata and the application output layout are both known.
- Raw `Request[index]` access and string status values make protocol evolution
  harder to review than necessary.
- Controller and Host repeat descriptor, spawn-action, wait, signal, and
  cleanup mechanics across several failure paths.
- DurinDevTool correctly routes application-hosted exact targets, but the
  decision is embedded in the top-level execution flow.

## Implementation Stages

### Stage 0: Freeze behavior and simplification boundaries

- [ ] Record golden configured registry records and generated CTest commands
  for one direct and one application-hosted target.
- [ ] Add a generated-source/build-command assertion that no native-test path
  contains a literal `$<CONFIG>`.
- [ ] Inventory all repository declarations of
  `DURIN_TEST_EXECUTION_HOST`, `APPLICATION_HOST`, and direct-registration
  names.
- [ ] Decide whether `Durin.NativeTestDirect.<Target>` is renamed in this plan
  or explicitly deferred, including CI and command compatibility impact.
- [ ] Record baseline focused direct and application-hosted execution times for
  comparison after structural changes.

#### Acceptance Gate

- The plan has executable golden tests for the behavior that must not change,
  and the CTest registration-name decision is explicit before API changes.

### Stage 1: Simplify the CMake authoring and layout API

- [ ] Add optional `EXECUTION_HOST` parsing to
  `durin_finalize_native_test`, defaulting to `direct`.
- [ ] Define compatibility and conflict rules for the legacy target property,
  then migrate repository-owned declarations and helper options.
- [ ] Extract semantic validation/platform resolution into a focused internal
  function with synthetic Apple and non-Apple probes.
- [ ] Extract macOS application artifact relocation and Engine runtime-layout
  staging into a focused internal function or module.
- [ ] Keep deployment helpers operating on finalized Data, Work, and runtime
  paths without imposing application behavior on direct targets.
- [ ] Extend CMake probes for defaulting, conflicts, invalid values, platform
  resolution, late mutation compatibility, and generator-expression output.

#### Acceptance Gate

- A new application target needs only one structured finalization call; direct
  declarations stay unchanged; registry and generated CTest behavior match the
  Stage 0 goldens; and direct output paths contain the concrete configuration.

### Stage 2: Introduce typed protocol boundaries

- [ ] Add `FRequest` encode/decode functions and remove positional request
  indexing from the Host.
- [ ] Replace free-form result status/stage construction with enums or closed
  typed values and centralized wire conversion.
- [ ] Move all request/result consistency checks to validation functions that
  return actionable field-specific diagnostics.
- [ ] Preserve the existing binary framing unless a versioned migration is
  objectively smaller and safer than compatibility.
- [ ] Expand unit tests for every request field, unknown status/stage values,
  stale nonces, embedded NULs, invalid PID/exit/signal combinations, truncation,
  trailing data, and maximum field limits.

#### Acceptance Gate

- Controller and Host logic use validated typed records exclusively; malformed
  and stale inputs retain the same launcher-failure semantics; and valid
  records remain wire-compatible or have a tested version transition.

### Stage 3: Consolidate macOS resource ownership

- [ ] Add minimal scoped wrappers for file descriptors and
  `posix_spawn_file_actions_t`.
- [ ] Add a local owned-child abstraction only if it eliminates repeated wait,
  liveness, terminate, grace-period, and kill code without hiding PID policy.
- [ ] Convert early-return paths in Controller and Host to deterministic scoped
  cleanup.
- [ ] Keep signal-handler work async-signal-safe and keep policy decisions in
  the normal control flow.
- [ ] Add or retain focused tests for spawn failure, output-open failure, PID
  publication failure, controller disappearance, interruption before and after
  child publication, escalation, concurrent execution, and retention limits.

#### Acceptance Gate

- Cleanup branches are materially reduced, every owned descriptor/action/child
  has one visible lifetime, cancellation remains bounded, and process audits
  find no orphan after success, failure, crash, or interruption.

### Stage 4: Simplify DurinDevTool routing and diagnostics

- [ ] Move exact-target execution choice into one target execution strategy
  function driven by configured registry metadata.
- [ ] Keep direct exact targets on the focused executable path and application
  exact targets on their launcher-aware CTest registration.
- [ ] Preserve filter, compact/full output, shuffle seed, isolation, stress,
  report, characterization, qualification, timeout, and environment behavior.
- [ ] Implement or defer the Stage 0 registration-name decision with explicit
  compatibility tests.
- [ ] Update list/explain and failure diagnostics only where the simpler model
  makes the execution path clearer to operators.

#### Acceptance Gate

- Top-level DevTool execution no longer contains a platform-host special case;
  all modes select the same targets and commands as the Stage 0 goldens; and
  stale registry/schema failures remain explicit.

### Stage 5: Regression, documentation, and completion

- [ ] Run the shared CMake policy suite, complete DurinDevTool Python suite,
  NativeTestSupport host/controller targets, and application execution probes.
- [ ] Run focused direct, filtered application, isolation, stress, report,
  concurrent-case, Vulkan qualification, and ordinary aggregate validation.
- [ ] Audit generated paths, control-directory retention, sandboxes, resource
  locks, process exit, and measured launcher overhead against Stage 0.
- [ ] Obtain Windows direct-host evidence before claiming cross-platform
  completion, without imposing macOS artifacts or dependencies on Windows.
- [ ] Update the native-test authoring contract to show only the canonical API
  and remove transitional guidance after repository declarations migrate.
- [ ] Record final evidence, complete this plan only after all gates pass, and
  archive it through the repository plan workflow.

#### Acceptance Gate

- The framework has fewer author-visible steps and fewer duplicated internal
  mechanisms, while direct/application behavior, exact results, cancellation,
  cleanup, selection, reporting, and cross-platform policy remain unchanged.

## Validation Matrix

| Boundary | Required evidence |
| --- | --- |
| CMake authoring | One-call application declaration, direct default, conflict diagnostics, synthetic platform resolution, and concrete generated paths. |
| Configured policy | Golden registry and CTest commands retain host, labels, locks, timeout, working directory, and aggregate membership. |
| Protocol | Typed round trip plus malformed, stale, unknown, NUL, size, PID, exit, signal, and trailing-data rejection. |
| Process ownership | Spawn/open/publication failures, success, ordinary failure, crash, timeout, interruption, escalation, concurrency, retention, and no orphans. |
| DevTool | Exact direct/application, filter, compact/full, isolation, stress, report, characterization, qualification, and stale-registry behavior. |
| Regression | macOS application targets, ordinary native aggregate, Windows direct targets, generated path audit, and launcher-overhead comparison. |

Build, test selection, and documentation validation follow the repository
[agent build workflow](../Agents/BuildAndRun.md),
[agent testing workflow](../Agents/Testing.md), and
[documentation workflow](../Agents/Documentation.md).

## Definition of Done

- Test authors declare a non-default execution host in the structured finalize
  call without a separate ordered property mutation.
- Native-test metadata finalization, execution-host resolution, and macOS
  artifact layout have distinct internal ownership.
- Controller and Host consume typed validated request/result records rather
  than positional vectors and free-form status strings.
- macOS resource ownership is scoped and duplicated cleanup logic is reduced
  without weakening exact-PID or bounded-termination policy.
- DurinDevTool has one registry-driven exact-target execution strategy.
- Direct and application-hosted behavior passes the full validation matrix,
  lasting guidance names one canonical authoring workflow, and all plan gates
  are evidence-backed before completion.

## Deferred Follow-ups

- A reusable platform-neutral process library remains deferred until another
  subsystem demonstrates the same ownership and cancellation requirements.
- A new protocol encoding remains deferred unless typed boundaries expose a
  concrete limitation in the current versioned binary framing.
- Linux display-server application hosting remains deferred until a real
  target requires it.
- Renderer qualification failures, GUI-session provisioning, and product app
  distribution remain owned by their existing plans and investigations.

## Related Documentation

- [macOS Native Test Application Host Plan](MacOSNativeTestApplicationHost.md)
- [Native C++ Tests](../Development/Build/NativeTests.md)
- [Build And Run](../Development/Build/BuildAndRun.md)
- [Agent Testing Workflow](../Agents/Testing.md)

## Related Code

- `CMake/Project/ProjectTargets.cmake`
- `CMake/Tests/NativeTestDiscoveryPolicyTests.cmake`
- `CMake/Tests/Fixtures/NativeTestLauncher/`
- `Engine/Tests/NativeTestSupport/Private/MacOS/`
- `Engine/Tests/Native/NativeTestApplicationHostTests/`
- `Engine/Tests/Native/NativeTestApplicationExecutionTests/`
- `Tools/DurinDevTool/durin_dev_tool/build/core.py`
- `Tools/DurinDevTool/durin_dev_tool/build/native_test_registry.py`
- `Tools/DurinDevTool/durin_dev_tool/build/runtime.py`
