# macOS Native Test Application Host Plan

Summary: Add a declarative native-test application host that preserves GoogleTest, CTest, and DurinDevTool policy while giving Cocoa-dependent macOS targets a LaunchServices-owned process lifecycle with exact result propagation and cleanup.

Last reviewed: 2026-08-18

Status: Active
Completed:

## Current Status

Stages 0 through 3 are implemented, and the available macOS portion of Stage 4
passes. Native-test authors declare `EXECUTION_HOST application` through
`durin_finalize_native_test`; CMake serializes the resulting execution-host
metadata, CTest applies one generic LaunchServices application host through
`TEST_LAUNCHER`, and DurinDevTool preserves target/case selection, filters,
isolation, stress, report, qualification, and direct execution behavior.

The controller and application host use nonce-bound binary control records,
mode-restricted environment transfer, exact PID ownership, atomic result
publication, bounded signal escalation, output replay, bounded failed-run
retention, and exact child-result mapping. Application-hosted executables,
runtime dependencies, control files, and retained evidence stay under the
owning target's Bin/Data/Work root. They are not relocated to `/private/tmp`;
an unauthorized external-volume checkout fails LaunchServices admission until
the user grants the ordinary macOS permission.

Evidence on the macOS arm64 Debug editor profile includes:

- the checked-in CMake launcher/discovery probe passes on CMake 4.4.2;
- all 376 DurinDevTool tests complete with 374 passes and two expected skips;
- all 19 protocol/controller native tests pass, including ordinary failure,
  crash retention, and bounded cancellation;
- application-host pass/report/isolation/stress paths pass, two case
  registrations run concurrently in about 0.10 seconds each, successful
  control directories are removed, and a process audit finds no orphan host,
  controller, or test child;
- the complete `VulkanRHIIntegrationTests` qualification target and a filtered
  case pass through ordinary DevTool commands;
- the complete ordinary `test all` aggregate passes after building the shared
  native-test target set.

The macOS regression matrix now also passes focused direct, filtered
application, isolation, stress, report, concurrent-case, crash
characterization, Vulkan/window qualification, and ordinary aggregate runs.
Windows remains unavailable to provide measured direct-host regression
evidence, so cross-platform completion stays open.

## Goal

Make every native-test execution mode reliably run targets that require Cocoa,
AppKit, or a real macOS presentation window while preserving exact test exit
status, output, timeout, cancellation, resource locking, sandboxing, and
GoogleTest filtering. Test authors declare the lifecycle requirement once;
CMake and the platform launcher select the correct host.

## Scope

- Add a structured native-test execution-host requirement independent of test
  kind, domain, backend, resource locks, and direct-lifecycle registration.
- Provide a macOS LaunchServices application host for targets that declare an
  application lifecycle requirement.
- Keep CTest as the scheduler and authority for target/case registration,
  labels, resource locks, timeout, report mode, and aggregate selection.
- Preserve GoogleTest arguments, filters, shuffle seeds, exit status, stdout,
  stderr, and per-process NativeTestSupport sandboxes across the host boundary.
- Provide bounded termination and orphan cleanup when tests fail, crash, time
  out, or are interrupted.
- Migrate the Vulkan RHI and Cocoa lifecycle targets that require the host and
  inventory other AppKit/GLFW-dependent native targets.
- Keep Windows and non-application native-test execution unchanged.

## Non-Goals

- Creating a macOS-specific replacement for GoogleTest, CTest, NativeTestSupport,
  DurinDevTool selection, or the native-test registry.
- Moving primary-window, RHI, renderer, or scene setup into the generic host;
  those fixtures remain owned by the test target exercising them.
- Building the product `.app`, signing, notarization, packaging, installation,
  or distribution; the test host is an internal build artifact only.
- Making visual screenshot qualification reliable while the login session is
  locked, or claiming unattended GUI support on a host without an active
  graphical session.
- Treating missing LaunchServices admission as a skip, falling back silently to
  direct execution, or accepting `open`'s exit status as the test result.
- Changing ordinary console-only tests or broadening qualification targets into
  the routine aggregate.

## Design Decisions and Invariants

### One test framework, multiple execution hosts

- `durin_finalize_native_test` remains the structured target declaration, and
  `durin_discover_tests` remains the only GoogleTest/CTest registration path.
- `durin_finalize_native_test(... EXECUTION_HOST direct|application ...)` owns
  the declaration. The default is `direct`; `application` describes a semantic
  lifecycle requirement, not a test kind or an Apple-only feature.
- `DURIN_TEST_DIRECT_LIFECYCLE` remains an independent property: it decides
  whether the ordinary whole-target CTest registration exists. It must not be
  overloaded to select the platform process host.
- On macOS, `application` resolves to the LaunchServices host. On platforms
  where an ordinary process already has the required application lifecycle, it
  may resolve to direct execution without changing target declarations.
- The configured native-test registry records the execution-host requirement
  so `test list`, `test explain`, CI diagnostics, and stale-registry validation
  report the real target contract.

### CTest retains scheduling and policy ownership

- Application-hosted targets use CMake's `TEST_LAUNCHER` integration rather
  than custom target commands or target-local `POST_BUILD` launch scripts.
- Whole-target registrations name the executable target rather than expanding
  only `$<TARGET_FILE:...>`, so CMake can apply the declared test launcher.
- Application-hosted GoogleTest discovery uses `DISCOVERY_MODE PRE_TEST`.
  Building the executable must not require an active GUI session merely to
  enumerate cases; discovery and execution acquire the same host when CTest is
  intentionally run.
- Target and case registrations retain their existing labels, resource locks,
  timeouts, working directory, and NativeTestSupport sandbox contract.
- Characterization targets with custom lifecycle owners remain explicit and do
  not acquire this host implicitly.

### The application host is a process adapter, not a fixture owner

- CMake produces one generic internal `.app` host for the active build
  configuration. The bundle executable starts the requested native-test binary
  as a child instead of `exec`-replacing itself, so it can capture normal,
  failed, and signal-derived child status.
- A command-line controller is the CTest `TEST_LAUNCHER`. It creates one unique
  control directory per invocation, asks LaunchServices for a new hidden host
  instance, streams or republishes captured output, reads the host's atomic
  completion record, and exits with the exact test result.
- `open` is transport only. A zero status from `open` never means the test
  passed; a missing, malformed, stale, or mismatched completion record is a
  launcher failure.
- The control protocol carries a nonce, requested executable, arguments,
  working directory, environment file, host PID, child PID, stdout/stderr
  paths, and atomic completion status. Every path is canonicalized and confined
  to the invocation's control directory or configured test output roots.
- The environment is transferred through a mode-restricted file rather than
  serialized into shell source or exposed wholesale on the process command
  line. The host reconstructs exact name/value pairs without evaluation.
- The host does not create `GApp`, windows, Vulkan surfaces, RHI devices, or
  rendering state. `VulkanRHIIntegrationTests` retains its registered hidden
  window fixture because that is the behavior under test.

### Failure, timeout, and cancellation are first-class

- The application host traps termination requests, forwards them to the test
  child, waits for a bounded grace period, escalates only to the exact retained
  PIDs, and publishes its final status before exit when possible.
- The controller handles CTest interruption and timeout by terminating the
  recorded child and host. It never uses an unvalidated process-name match or a
  broad kill command.
- A native test crash must fail CTest and retain the ordinary NativeTestSupport
  sandbox/crash evidence. Launcher diagnostics identify whether failure
  occurred before host admission, before child start, during the test, or while
  publishing completion.
- Concurrent case isolation uses independent control directories and
  LaunchServices `new instance` admission. Existing CTest resource locks remain
  the authority for GPU or target serialization.
- Successful launcher control directories are removed. Failed, crashed,
  timed-out, or cleanup-failed invocations are retained with a bounded path and
  actionable diagnostic.

## Current Foundations and Gaps

- `durin_discover_tests` already centralizes deployment closure, GoogleTest
  discovery, case policy, whole-target registration, resource locks, and
  timeout metadata.
- CMake 4.4 provides the `TEST_LAUNCHER` target property needed to prefix test
  execution without replacing CTest registration.
- DurinDevTool already selects whole-target and case registrations by labels
  and passes the configured environment to CTest; that selection model should
  remain unchanged.
- NativeTestSupport already owns per-process writable sandboxes, success/failure
  retention, GoogleTest arguments, and process return codes.
- The current registry schema records direct lifecycle but has no execution-host
  field, so selection diagnostics cannot distinguish console and application
  admission requirements.
- CTest currently registers `Durin.NativeTestDirect.<Target>` with a raw target
  file path. That bypasses CMake test-launcher semantics and causes Cocoa
  initialization to run as an unregistered command-line process.
- Temporary `.app` wrappers prove the lifecycle requirement but do not provide
  exact CTest status, cancellation cleanup, concurrency isolation, or a
  repository-owned reproducible command.

## Implementation Stages

### Stage 0: Freeze the execution-host contract and control protocol

- [x] Reproduce command-line and direct-bundle Cocoa admission failure with a
  native crash report rooted in `_RegisterApplication`.
- [x] Prove a LaunchServices-owned bundle shell can spawn the native test while
  retaining Cocoa admission and can capture the child result.
- [x] Prove `open -W` status is insufficient by observing success from `open`
  after a launched native-test crash.
- [x] Add a minimal CMake probe that records how `TEST_LAUNCHER` interacts with
  `gtest_discover_tests`, `DISCOVERY_MODE PRE_TEST`, whole-target registration,
  and generated case commands on the supported CMake version.
- [x] Freeze the control-directory file names, nonce checks, environment wire
  format, state transitions, result schema, and termination grace periods.
- [x] Decide the exact internal output root and retention budget without using
  checked-in `Data` or writing directly into a target's shared `Work` parent.

#### Acceptance Gate

- A checked-in probe demonstrates the selected CMake hook for both whole-target
  and case execution, and the written protocol distinguishes admission failure,
  test failure, crash, timeout, cancellation, malformed state, and cleanup
  failure without relying on `open` status.

### Stage 1: Add declarative execution-host metadata and CMake policy

- [x] Validate `EXECUTION_HOST` during target finalization and reject missing,
  unknown, or late mutations with target-specific diagnostics.
- [x] Extend the deterministic native-test registry schema and DurinDevTool
  reader with the execution-host field; expose it through list/explain output.
- [x] Generate the macOS application-host artifact and attach its controller
  through `TEST_LAUNCHER` only for application-hosted targets.
- [x] Change whole-target CTest registration to preserve target identity so the
  launcher property is honored, while retaining existing labels, locks,
  working directories, and timeouts.
- [x] Use pre-test GoogleTest discovery for application-hosted targets and keep
  post-build discovery for direct targets.
- [x] Add CMake policy/metadata probes and DurinDevTool registry tests for
  defaults, valid application declarations, invalid values, deterministic
  serialization, platform resolution, and stale schema rejection.

#### Acceptance Gate

- Synthetic direct and application targets configure deterministically; their
  generated CTest commands use the expected host without changing policy
  labels or locks; registry/list/explain output reports the requirement; and
  direct targets retain byte-for-byte-equivalent execution behavior where
  practical.

### Stage 2: Implement the macOS application host and controller

- [x] Implement the generic `.app` bundle executable with argument-preserving
  child launch, exact result capture, PID publication, signal forwarding,
  bounded termination, and atomic completion.
- [x] Implement the command-line controller with unique control directories,
  secure environment transfer, LaunchServices admission, output forwarding,
  result validation, cleanup, and exact exit-code mapping.
- [x] Reject control paths, executable paths, PIDs, nonces, or status files that
  escape or conflict with the active invocation.
- [x] Preserve compact/full output, GoogleTest brief mode, filters, shuffle
  seeds, report output, NativeTestSupport work retention, and Vulkan SDK
  environment values.
- [x] Add deterministic unit tests for command construction, quoting-free
  environment round trips, status validation, stale/malformed files, signal
  mapping, timeout escalation, cleanup, and concurrent invocation isolation.
- [x] Add native probe cases for pass, GoogleTest assertion failure, ordinary
  nonzero exit, intentional crash, timeout, controller interruption, and
  output/report publication.

#### Acceptance Gate

- CTest receives the real result for every probe, produces the expected output
  and report artifacts, leaves no live host/test process after completion or
  cancellation, and retains only bounded failed-invocation evidence.

### Stage 3: Migrate Cocoa- and presentation-dependent native targets

- [x] Declare the application host for `VulkanRHIIntegrationTests` and
  `MacOSWindowLifecycleTests`, then inventory other targets that initialize
  GLFW, AppKit, Cocoa dialogs, Metal layers, or native presentation.
- [x] Keep target-owned application/window fixtures explicit and remove only
  temporary or duplicated process-launch workarounds.
- [x] Qualify focused target, filtered case, case isolation, stress, report,
  characterization where applicable, and qualification modes through ordinary
  DevTool commands without manual `.app` assembly or direct `open` commands.
- [ ] Verify locked-session and missing-GUI-session failures are bounded and
  diagnostic; do not convert them to passes or platform skips.
- [x] Update the native-test contract, build/run guidance, macOS platform plan,
  and MoltenVK investigation with the lasting host declaration and operator
  workflow.

#### Acceptance Gate

- Every migrated target runs through its documented DevTool command, filters
  and reports behave identically to direct targets, Cocoa work remains on the
  main thread, and no manual LaunchServices wrapper is required for normal
  development or CI qualification.

### Stage 4: Cross-platform regression and rollout

- [ ] Prove direct targets on Windows and macOS retain their existing CTest
  commands, environment, results, and aggregate membership.
- [x] Run the shared CMake policy suite, DurinDevTool Python suite,
  NativeTestSupport infrastructure targets, migrated macOS application targets,
  and the complete ordinary native aggregate required for shared harness
  changes.
- [x] Exercise repeated and concurrent application-hosted runs while auditing
  process, control-directory, sandbox, GPU-lock, and result cleanup.
- [x] Record measured launcher overhead and ensure application hosting is
  opt-in rather than imposed on console-only targets.
- [x] Transfer lasting authoring and runtime rules to the native-test contract,
  remove superseded temporary guidance, and complete/archive this plan only
  after all gates land.

#### Acceptance Gate

- Shared test infrastructure and ordinary aggregates pass on supported hosts;
  migrated macOS targets have repeatable clean process exit; direct targets
  show no functional regression; and the repository documentation names one
  authoritative native-test workflow.

## Validation Matrix

| Boundary | Required evidence |
| --- | --- |
| CMake declaration | Valid/invalid execution-host probes, deterministic registry generation, launcher-aware whole-target registration, and pre-test case discovery. |
| Controller protocol | Canonical path and nonce validation, environment round trip, atomic status, stale/malformed state rejection, bounded cleanup, and concurrency isolation. |
| Process lifecycle | LaunchServices admission, main-thread Cocoa initialization, exact success/failure/crash status, timeout, interruption, signal forwarding, and no orphan processes. |
| Test semantics | GoogleTest filters, shuffle seeds, case isolation, brief/full output, JUnit report mode, NativeTestSupport sandbox retention, and target resource locks. |
| Vulkan/macOS integration | Registered primary window, Metal surface/device admission, swapchain fixture behavior, repeated RHI init/exit, and clean application teardown. |
| Cross-platform | Unchanged direct execution on Windows and console-only macOS targets plus the shared native-test infrastructure and ordinary aggregate gates. |

Build, test selection, and documentation validation follow the repository
[agent build workflow](../Agents/BuildAndRun.md),
[agent testing workflow](../Agents/Testing.md), and
[documentation workflow](../Agents/Documentation.md).

## Definition of Done

- Test authors declare application lifecycle once and do not write target-local
  LaunchServices wrappers.
- CTest remains the sole registration, scheduling, timeout, resource-lock, and
  result authority for ordinary native tests.
- macOS application-hosted tests return exact pass, assertion-failure, crash,
  timeout, and cancellation outcomes with bounded output and cleanup.
- Whole-target, filtered-case, isolation, stress, report, and qualification
  modes work through ordinary DevTool commands.
- Direct targets remain direct and retain cross-platform behavior.
- Lasting execution-host rules are documented in the native-test contract, all
  stage gates pass, and no manual temporary `.app` workflow remains required.

## Deferred Follow-ups

- Product `.app` assembly, signing, notarization, packaging, installation, and
  distribution remain owned by the macOS platform roadmap's distribution
  milestone.
- Remote GUI-session provisioning and a supported macOS CI machine matrix
  remain rollout work after the local host protocol is qualified.
- Automated visual-difference testing, screenshot capture under an unlocked
  session, and MoltenVK argument-buffer requalification remain separate
  rendering-quality work.
- A platform-neutral application host for future Linux display-server tests is
  deferred until a concrete target requires it; the semantic declaration must
  leave that extension possible.

## Related Documentation

- [Native C++ Tests](../Development/Build/NativeTests.md)
- [Build And Run](../Development/Build/BuildAndRun.md)
- [Agent Testing Workflow](../Agents/Testing.md)
- [macOS Platform Runtime Plan](MacOSPlatformRuntime.md)
- [macOS Platform Enablement Roadmap](../Roadmaps/MacOSPlatformEnablement.md)
- [macOS MoltenVK argument-buffer instability](../Investigations/MacOSMoltenVKArgumentBufferInstability.md)

## Related Code

- `CMake/Project/ProjectTargets.cmake`
- `CMake/Tests/NativeTestDiscoveryPolicyTests.cmake`
- `CMake/Tests/Fixtures/NativeTestMetadata/`
- `Engine/Tests/NativeTestSupport/`
- `Engine/Tests/Native/NativeTestIsolationProbeTests/`
- `Engine/Tests/Native/VulkanRHITests/`
- `Engine/Tests/Native/EngineTests/CMake/LaunchTests.cmake`
- `Engine/Tests/Native/EngineTests/Private/Launch/MacOSWindowLifecycleTests.cpp`
- `Tools/DurinDevTool/durin_dev_tool/build/native_test_registry.py`
- `Tools/DurinDevTool/durin_dev_tool/build/runtime.py`
- `Tools/DurinDevTool/tests/test_build_actions.py`
