# Native Test Execution Granularity Plan

Summary: Replace universal per-case native-test execution with an explicit hybrid policy that batches proven-safe targets while retaining case isolation for lifecycle and shared-resource risks.

Last reviewed: 2026-08-09

Status: Active
Completed:

## Current Status

The repository currently discovers every GoogleTest case as an independent
CTest entry and excludes whole-target direct registrations from the default
aggregate. This preserves strong isolation and scheduling flexibility, but the
default aggregate now pays one process bootstrap and teardown for nearly every
assertion-scale case. The existing direct registrations demonstrate the
opposite execution granularity, but they are qualification-only and do not
participate in the default aggregate.

This plan selects a hybrid default rather than a repository-wide switch. No
target changes granularity until its same-process lifecycle and order
independence are measured and qualified. Implementation has not started.

## Goal

Make the routine native-test aggregate use target-level processes for proven
batch-safe execution domains and case-level processes for tests whose failure,
lifecycle, concurrency, or shared-resource behavior still requires isolation.
The result must materially reduce process creation without weakening focused
diagnosis, resource locking, failure reporting, or the ability to run the
complete suite with case isolation.

## Scope

- Native-test discovery metadata and registration in
  `CMake/Project/ProjectTargets.cmake`.
- Per-target declaration of the default execution granularity.
- DurinDevTool selection of hybrid, case, and target native-test aggregates.
- Policy validation and unit coverage for CMake and DurinDevTool.
- Evidence-driven migration of existing native-test targets.
- Native-test workflow documentation and qualification evidence.

## Non-Goals

- Deleting functional coverage merely to reduce process count.
- Removing per-process sandboxes or allowing shared writes below a target's
  `Work` container.
- Making every target run in one process by default.
- Combining native-test executables that have different dependency, runtime,
  fixture, timeout, or resource-lock ownership.
- Changing focused `test --target <Target> --filter <Filter>` behavior.
- Hiding a failed batch by automatically rerunning it and reporting success.
- Replacing correctness tests with timing thresholds or treating one machine's
  wall-clock result as a permanent performance guarantee.

## Design Decisions and Invariants

### Execution modes

- `hybrid` is the eventual default for `test --target all`. Each target runs at
  its declared default granularity.
- `case` runs all discovered GoogleTest cases independently and preserves the
  current aggregate as the isolation and diagnosis fallback.
- `target` runs each eligible native-test executable once. It is an explicit
  qualification mode and does not include characterization-only targets.
- A focused single-target run continues to launch that executable once and
  forwards an optional GoogleTest filter; aggregate granularity does not alter
  this path.

### Target policy

- The safe configuration fallback is case execution. A new or undeclared
  target never becomes batched implicitly.
- A target may opt into target-level default execution only with an explicit
  CMake property and a non-empty rationale naming its owned initialization,
  teardown, mutable state, and fixture-reset boundary.
- Target-level default execution requires a direct lifecycle registration and
  is invalid for characterization-only targets.
- Case parallel safety and default execution granularity remain separate
  properties. Case parallel safety proves independent processes may overlap;
  target batching proves cases may share one process in one execution domain.
- Existing timeout and resource-lock metadata applies identically to the
  selected default registration. Batching never bypasses `durin-gpu`, legacy
  renderer serialization, or another registered resource.

### Registration and labels

- Every discovered case carries a stable `native-test-case` label.
- Every direct whole-target registration carries a stable
  `native-test-target` label.
- Exactly one registration family for each ordinary target also carries
  `native-test-default`: discovered cases for case-default targets, or the
  direct registration for target-default targets.
- `hybrid`, `case`, and `target` select these labels instead of reconstructing
  target lists in DurinDevTool.
- Existing `native-test-direct` remains a compatibility label during the
  migration. `--include-direct` runs only direct registrations not already
  executed by the hybrid phase, preventing duplicate work.
- Characterization labels remain authoritative and are excluded from every
  routine aggregate mode.

### Failure and diagnosis

- A failed target-level process reports the GoogleTest case names emitted by
  that process and preserves its process sandbox through the existing harness.
- DurinDevTool prints the exact case-mode rerun form after a batched aggregate
  failure. It does not automatically convert a failed run into a passing run.
- Crashes, intentional abrupt exits, death-style behavior, process-global
  lifecycle probes, and tests that cannot reliably identify the failing case
  remain case-default or move to a dedicated target.
- Case mode remains the required confirmation when investigating order
  dependence, state leakage, or a failure seen only in a batched target.

### Qualification policy

- Batch eligibility is based on repeated evidence, not test names or target
  size. Qualification includes normal order, randomized order, repeated direct
  execution, case-mode execution, and retained-sandbox inspection on injected
  failure.
- A target is ineligible while any case relies on another case's initialization,
  leaves process-global state unreconciled, consumes an irreversible singleton,
  changes runtime mode, intentionally terminates, or owns a shared resource not
  covered by its declared lock.
- A migrated target reverts to case-default immediately if later evidence finds
  order dependence or same-process state leakage.

## Current Foundations and Gaps

### Foundations

- `durin_discover_tests` centralizes discovered-case labels, timeouts, resource
  locks, and direct lifecycle registration.
- Every ordinary native-test process receives a unique writable sandbox and
  keeps failed output for diagnosis.
- Functional targets already describe coherent dependency and lifecycle
  domains rather than mirroring production modules.
- Whole-target direct registrations already exercise same-process execution and
  have historical qualification coverage.
- DurinDevTool already owns aggregate selection, CTest concurrency, randomized
  scheduling, regex filtering, JUnit output, and the optional direct phase.

### Gaps

- Discovery policy has no property expressing which registration family is the
  routine default.
- The default aggregate excludes every direct registration and therefore
  launches one process per discovered case.
- Direct lifecycle success is not yet a sufficient batch-eligibility audit;
  targets lack explicit rationale and repeated randomized same-process gates.
- DurinDevTool has no named granularity option and no diagnostic guidance from
  a failed batched target to a case-isolated rerun.
- CMake policy tests do not verify mutually exclusive default labels or reject
  invalid batching declarations.
- Current documentation describes case-level execution as the universal
  default rather than one selectable risk tier.

## Implementation Stages

### Stage 0: Freeze the baseline and classify execution risks

Dependencies: none.

- [ ] Capture machine-readable baseline results for the current case aggregate:
  process launches, wall-clock duration, accumulated process time, skipped and
  disabled cases, failures, and the slowest execution domains.
- [ ] Run repeated randomized case aggregates and repeated whole-target direct
  qualification without changing registration policy.
- [ ] Inventory every ordinary native-test target against initialization,
  teardown, mutable global state, fixture writes, process termination, timeout,
  and registered resource locks.
- [ ] Select a small pilot set containing only low-cost CPU targets with
  deterministic direct runs and no renderer, GPU, external-process, or runtime
  variant lifecycle.
- [ ] Record case-default reasons for every non-pilot target; an unknown reason
  remains case-default rather than becoming an open-ended exception.
- [ ] Store raw counts and timings in test/JUnit evidence rather than copying a
  current suite total into long-lived documentation.

#### Acceptance Gate

- The baseline can be reproduced through documented DurinDevTool entrypoints.
- Every target has a selected pilot or case-default classification with an
  evidence-backed rationale.
- The pilot set passes normal, randomized, and repeated direct execution with
  clean teardown and no retained successful sandboxes.

### Stage 1: Add declarative CMake execution policy

Dependencies: Stage 0.

- [ ] Add a validated target property for default execution granularity with
  `CASE` as the fallback and `TARGET` as the only opt-in value.
- [ ] Require an explicit batching rationale and a valid direct lifecycle for
  every `TARGET` declaration.
- [ ] Generate `native-test-case`, `native-test-target`, and exactly one
  `native-test-default` registration family per ordinary target.
- [ ] Preserve timeouts, resource locks, existing target labels, working
  directories, and process-sandbox behavior on both registration families.
- [ ] Reject target batching for characterization registrations and reject
  missing, contradictory, or unsupported property values during configure.
- [ ] Extend CMake policy probes to cover label selection, compatibility labels,
  resource propagation, and every rejection path.

#### Acceptance Gate

- Configuration tests prove that every ordinary target contributes one and
  only one default registration family.
- With no target opt-ins, the hybrid selection is behaviorally identical to the
  current case aggregate.
- Case and target registrations retain identical resource locks and timeout
  policy for the same execution domain.

### Stage 2: Expose hybrid, case, and target aggregate modes

Dependencies: Stage 1.

- [ ] Add a typed DurinDevTool aggregate granularity option with
  `hybrid`, `case`, and `target` values; reject it for a focused target where it
  has no meaning.
- [ ] Make hybrid the selected default only after the compatibility and command
  tests in this stage pass.
- [ ] Select aggregate registrations by CTest labels while preserving timeout,
  random scheduling, regex, job count, compact output, and JUnit behavior.
- [ ] Define regex behavior explicitly: a case-name regex against a batched
  target requires case mode; reject an ambiguous empty selection with an
  actionable command rather than silently running nothing.
- [ ] Update `--include-direct` so the second phase runs only direct lifecycle
  registrations not already executed by hybrid mode.
- [ ] On batched failure, print a deterministic case-mode rerun command and the
  preserved sandbox location already emitted by the harness.
- [ ] Extend DurinDevTool parser, configuration, runtime-command, interruption,
  JUnit, and error-message tests for all modes.

#### Acceptance Gate

- Command tests prove the exact CTest label expressions for all modes and option
  combinations.
- Hybrid with no batched targets matches the current aggregate registration set.
- Case mode runs every discovered ordinary case, target mode runs every eligible
  ordinary direct registration, and neither includes characterization tests.
- `--include-direct` introduces no duplicate target process after hybrid mode.

### Stage 3: Pilot target-level defaults

Dependencies: Stage 2.

- [ ] Opt only the Stage 0 pilot targets into target-default execution and add
  their reviewed batching rationales beside their target declarations.
- [ ] Add or repair explicit reset/teardown in test support when pilot evidence
  exposes state leakage; do not change production behavior to accommodate test
  batching.
- [ ] Run repeated hybrid, case, and target aggregates with randomized order and
  retain JUnit evidence for process count, duration, and failures.
- [ ] Inject one controlled assertion failure and one retained-work diagnostic
  in a probe target to verify batched reporting and case-mode rerun guidance.
- [ ] Compare pilot failures and skips across all three modes; any semantic
  difference blocks migration of that target.

#### Acceptance Gate

- Every pilot target passes repeated target-level and case-level runs in normal
  and randomized schedules.
- Hybrid reduces aggregate process launches by at least 25 percent from the
  Stage 0 baseline without increasing median wall-clock time by more than 10
  percent.
- Failure output identifies the owning target and failing GoogleTest case, keeps
  the failed sandbox, and gives a working case-mode rerun command.

### Stage 4: Expand by risk tier and qualify the default

Dependencies: Stage 3.

- [ ] Migrate additional CPU-only targets in bounded groups, repeating the
  pilot gate for each group.
- [ ] Review object-system, asset, renderer, GPU, external-tool, and dedicated
  lifecycle targets separately; keep case-default unless same-process evidence
  closes every recorded risk.
- [ ] Keep intentional crash/exit probes and characterization targets outside
  routine batching.
- [ ] Run consecutive randomized hybrid aggregates, a complete case aggregate,
  the target qualification aggregate, and direct lifecycle qualification on
  the selected Agent Build Profile.
- [ ] Compare process launches, median and worst wall-clock duration, aggregate
  process time, failure diagnostics, and retained artifacts with Stage 0.
- [ ] Update the native-test and build/run documentation with the implemented
  modes, authoring rules, fallback workflow, and final risk-tier policy.

#### Acceptance Gate

- The hybrid default reduces process launches by at least 70 percent from the
  Stage 0 baseline.
- Hybrid median wall-clock time does not regress from baseline, and its worst
  qualified run is no more than 10 percent slower than the baseline worst run.
- Consecutive randomized hybrid aggregates and the complete case aggregate pass
  with the same expected skips and disabled tests.
- Every target-default declaration has a current rationale and qualification
  evidence; every remaining case-default target has a concrete retained risk.
- The authoritative native-test documentation, CMake policy tests, and
  DurinDevTool command tests agree on the final behavior.

## Validation Matrix

| Area | Focused validation | Aggregate validation | Required evidence |
| --- | --- | --- | --- |
| CMake policy | Discovery policy and failure probes | Configure the ordinary test preset | Default-family uniqueness, invalid declarations rejected, locks/timeouts preserved |
| DurinDevTool | Parser, request, command construction, JUnit, and failure-message tests | Exercise each aggregate mode through the root wrapper | Exact labels, no characterization leakage, no duplicate direct phase |
| Pilot targets | Normal, shuffled, repeated direct, and case-isolated runs | Randomized hybrid and case aggregates | Same pass/skip set, clean teardown, no retained successful work |
| Failure path | Controlled assertion and retained-work probe | Failed batched aggregate followed by case rerun | Case name, target, sandbox, and actionable rerun are visible |
| Shared resources | Selected lock-bearing targets in case mode | Hybrid under normal parallelism | GPU and legacy renderer locks remain serialized |
| Performance | Stage 0 and per-stage JUnit/timing capture | Consecutive final hybrid and case aggregates | Relative launch reduction and wall-clock gates pass |

All configure, build, and test execution follows
[Build And Run](../Development/Build/BuildAndRun.md). Native-test authoring and
runtime sandbox rules remain owned by
[Native C++ Tests](../Development/Build/NativeTests.md).

## Definition of Done

- Hybrid is the documented default aggregate and retains explicit case and
  target modes.
- Process launch and wall-clock acceptance gates pass on one recorded Agent
  Build Profile.
- Case isolation remains available for the complete ordinary suite and for
  focused diagnosis.
- Batch declarations are explicit, validated, evidence-backed, and preserve
  resource policy.
- Failed batches remain diagnosable without automatic success masking.
- Lasting behavior is moved into the authoritative build/test documentation,
  all affected automated tests pass, and the plan is marked completed.

## Deferred Follow-ups

- Automatic sharding inside one large target; add it only if a qualified target
  becomes the hybrid critical path.
- Historical timing dashboards or CI trend storage beyond the JUnit artifacts
  needed for this migration.
- Automatic failed-case reruns. A later plan may add explicitly reported flaky
  retry policy, but this plan keeps failure semantics single-attempt and strict.
- Release and Shipping parity qualification unless a stage changes
  configuration-dependent registration behavior.

## Related Documentation

- [Native C++ Tests](../Development/Build/NativeTests.md)
- [Build And Run](../Development/Build/BuildAndRun.md)
- [Native Test Process Isolation](Archive/2026-07/NativeTestProcessIsolation.md)
- [Native Test Process Isolation Stage 0 Evidence](../Development/Build/NativeTestProcessIsolationStage0.md)
- [Native Test Process Isolation Stage 2 Evidence](../Development/Build/NativeTestProcessIsolationStage2.md)
- [Native Test Process Isolation Stage 3 Evidence](../Development/Build/NativeTestProcessIsolationStage3.md)

## Related Code

- `CMake/Project/ProjectTargets.cmake`
- `CMake/Tests/NativeTestDiscoveryPolicyTests.cmake`
- `CMake/Tests/NativeTestPolicyFailureProbe.cmake`
- `Engine/Tests/Native/CMakeLists.txt`
- `Engine/Tests/Native/*/CMakeLists.txt`
- `Engine/Tests/Native/NativeTestIsolationProbeTests/`
- `Tools/DurinDevTool/durin_dev_tool/build/config.py`
- `Tools/DurinDevTool/durin_dev_tool/build/handler.py`
- `Tools/DurinDevTool/durin_dev_tool/build/runtime.py`
- `Tools/DurinDevTool/tests/test_build_core.py`
