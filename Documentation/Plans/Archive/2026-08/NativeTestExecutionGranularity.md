# Native Test Execution Granularity Plan

Summary: Make one-process-per-target execution the ordinary native-test default while retaining full case isolation as an explicit diagnostic and qualification mode.

Last reviewed: 2026-08-09

Status: Archived
Completed: 2026-08-09

## Current Status

The repository currently discovers every GoogleTest case as an independent
CTest entry and excludes whole-target direct registrations from the default
aggregate. This preserves strong isolation and scheduling flexibility, but the
default aggregate now pays one process bootstrap and teardown for nearly every
assertion-scale case. The existing direct registrations demonstrate the
opposite execution granularity, but they are qualification-only and do not
participate in the default aggregate.

This plan selects target-level execution as the final routine default. Hybrid
selection is a migration mechanism while targets prove same-process lifecycle
and order independence; it is not the intended steady state. Tests that cannot
share a process must first repair teardown or move into a dedicated execution
target. Only infrastructure probes whose purpose requires multiple processes,
intentional crashes, or abrupt exits remain exceptional. Stage 0 froze the
`windows-msvc-x64` baseline, qualified a low-risk pilot, and recorded the
remaining lifecycle repairs. Stage 1 added validated `CASE`/`TARGET` policy,
stable case/target/default labels, explicit migration metadata on every
ordinary target, and configuration probes for all rejection paths. Stage 2
added typed `target`, `case`, and `hybrid` aggregate selection, made hybrid the
rollout default, propagated reproducible GoogleTest shuffle seeds into batched
processes, and added actionable batched-failure diagnostics. Stage 3 migrated
21 qualified targets, reduced hybrid launches by 26.7 percent, and verified
retained batched-failure diagnosis. Stage 4 migrated every remaining ordinary
target, repaired shared process state and writable-data isolation, and made
target execution the repository default. The final 48-process aggregate cuts
launches by 96.1 percent from the Stage 0 case baseline and passes the timing,
case-compatibility, and diagnostic gates.

The Stage 0 handoff is
[Native Test Execution Granularity Stage 0 Evidence](../../../Development/Build/NativeTestExecutionGranularityStage0.md).

Stage 1 baseline commit: `63be6552`. Its working set was
`CMake/Project/ProjectTargets.cmake`, the native-test policy probes, and native
target `CMakeLists.txt` declarations. The migration fallback remains `CASE`;
all ordinary targets require direct lifecycle registration, while the process
isolation characterization target has no default family. Configure, policy
probes, registration-label/timeout/resource auditing, and a complete randomized
case aggregate passed. No Stage 1 questions remain open.

Stage 2 baseline commit: `0cb71947`. Its working set was the DurinDevTool build
request, parser, runtime, and their command tests. Aggregate modes select only
the declarative CTest labels; a case-name regex is accepted only in case mode,
and `--include-direct` runs only direct registrations absent from the hybrid
default. All 306 DurinDevTool tests passed. Root-wrapper integration passed for
case (1,242 registrations), hybrid (1,242), target (46), and hybrid plus the
46-registration compatibility phase, with no failures or duplicate work. No
Stage 2 questions remain open.

Stage 3 baseline commit: `d587e708`. Its working set was the declarative native
test policies, the ordinary isolation probe, and Stage 3 evidence. The 21
target-default registrations own their existing process sandbox and scoped
fixture lifecycle; no production lifecycle change was required. Three
randomized hybrid runs and two randomized case runs passed. Two randomized
all-target diagnostics left only the Stage 4 repair queue failing. The detailed
handoff is [Native Test Execution Granularity Stage 3 Evidence](../../../Development/Build/NativeTestExecutionGranularityStage3.md).

Stage 4 baseline commit: `055e05b6`. Its working set was the final native-test
declarations, shared object/death-child support, bounded object and task test
seams, asset/material fixtures, DurinDevTool defaults, and authoritative build
documentation. Three consecutive randomized target aggregates, a complete
case aggregate, hybrid compatibility, focused lifecycle qualification, CMake
policy configuration, and all 306 DurinDevTool tests passed. There are no
temporary ordinary `CASE` exceptions or open questions. The detailed handoff
is [Native Test Execution Granularity Stage 4 Evidence](../../../Development/Build/NativeTestExecutionGranularityStage4.md).

## Goal

Make the routine native-test aggregate launch every ordinary test target once,
allowing targets to run concurrently under their existing resource policy.
Retain complete case-isolated execution as an explicit diagnosis and
independence-qualification mode. The result must reduce routine process creation
to approximately the number of functional targets without weakening focused
filters, resource locking, failure reporting, or exceptional crash probes.

## Scope

- Native-test discovery metadata and registration in
  `CMake/Project/ProjectTargets.cmake`.
- Explicit classification of ordinary targets and exceptional multi-process
  characterization runners.
- DurinDevTool selection of target, case, and transitional hybrid native-test
  aggregates.
- Policy validation and unit coverage for CMake and DurinDevTool.
- Evidence-driven migration of existing native-test targets.
- Native-test workflow documentation and qualification evidence.

## Non-Goals

- Deleting functional coverage merely to reduce process count.
- Removing per-process sandboxes or allowing shared writes below a target's
  `Work` container.
- Forcing intentional crash, abrupt-exit, or concurrent-sandbox
  characterization into an ordinary same-process target.
- Combining native-test executables that have different dependency, runtime,
  fixture, timeout, or resource-lock ownership.
- Changing focused `test --target <Target> --filter <Filter>` behavior.
- Hiding a failed batch by automatically rerunning it and reporting success.
- Replacing correctness tests with timing thresholds or treating one machine's
  wall-clock result as a permanent performance guarantee.

## Design Decisions and Invariants

### Execution modes

- `target` is the eventual default for `test --target all`. Every ordinary
  native-test executable runs once, and different targets remain eligible for
  CTest parallelism under their existing resource locks.
- `case` runs all discovered GoogleTest cases independently and preserves the
  current aggregate as an explicit state-leak diagnosis and independence
  qualification mode. It is not a routine acceleration mode.
- `hybrid` exists only during migration. It runs qualified targets once and
  retains case execution for targets whose teardown or topology is still being
  repaired. It is removed or demoted to an internal compatibility mode after
  every ordinary target migrates.
- Random scheduling in target or hybrid mode randomizes both CTest target order
  and GoogleTest case order inside each selected target. Case mode retains CTest
  process-order randomization.
- A focused single-target run continues to launch that executable once and
  forwards an optional GoogleTest filter; aggregate granularity does not alter
  this path.

### Target policy

- Every ordinary target must support target-level execution. New ordinary
  targets receive target execution by default and must own their initialization,
  teardown, mutable state, and fixture-reset boundary.
- A temporary case-default declaration requires a non-empty migration rationale
  naming the unresolved teardown or topology defect and the stage that removes
  the exception. It is not a permanent risk classification.
- Intentional crash, abrupt-exit, and concurrent-process characterization uses
  a dedicated target or custom runner and is excluded from routine aggregates.
- Target-level execution requires a direct lifecycle registration and is the
  configuration fallback after migration completes.
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
  `native-test-default`. During migration this may be discovered cases for a
  temporary exception; at completion it is the direct target registration.
- `hybrid`, `case`, and `target` select these labels instead of reconstructing
  target lists in DurinDevTool.
- Existing `native-test-direct` remains a compatibility label during the
  migration. Once target execution is default, `--include-direct` is removed or
  retained only as a no-op compatibility alias; it never duplicates work.
- Characterization labels remain authoritative and are excluded from every
  routine aggregate mode.

### Failure and diagnosis

- A failed target-level process reports the GoogleTest case names emitted by
  that process and preserves its process sandbox through the existing harness.
- DurinDevTool prints the exact case-mode rerun form after a batched aggregate
  failure. It does not automatically convert a failed run into a passing run.
- Intentional process crashes and abrupt exits move to a dedicated target or
  custom runner. GoogleTest death tests are not case-default merely because
  they spawn their own controlled child process.
- Case mode remains the required confirmation when investigating order
  dependence, state leakage, or a failure seen only in a batched target.

### Qualification policy

- Target readiness is based on repeated evidence, not test names, target size,
  use of GPU APIs, or use of process-global subsystems. Qualification includes
  normal order, randomized order, repeated target execution, case-mode
  execution, and retained-sandbox inspection on injected failure.
- If a case relies on another case's initialization, leaves global state
  unreconciled, consumes an irreversible singleton, or changes runtime mode,
  repair its lifecycle or split it into a cohesive dedicated target. Do not use
  permanent routine case isolation to conceal the defect.
- Resource locks control overlap between target processes; they do not imply
  case execution. GPU, renderer, object-system, GC, scheduler, and external-tool
  targets may use target execution when their owned teardown is deterministic.
- A target that later exposes order dependence or state leakage temporarily
  returns to hybrid case execution with a tracked repair requirement; case mode
  remains available immediately for diagnosis.

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

- Discovery policy does not yet make whole-target registration the ordinary
  default or distinguish temporary migration exceptions from characterization.
- The default aggregate excludes every direct registration and therefore
  launches one process per discovered case.
- Direct lifecycle success is not yet a sufficient target-readiness audit;
  targets lack repeated randomized same-process gates and tracked repair paths
  for lifecycle defects.
- DurinDevTool has no named granularity option and no diagnostic guidance from
  a failed batched target to a case-isolated rerun.
- CMake policy tests do not verify mutually exclusive default labels or reject
  invalid batching declarations.
- Current documentation describes case-level execution as the universal
  default rather than a diagnostic and independence-qualification mode.

## Implementation Stages

### Stage 0: Freeze the baseline and classify exceptional process requirements

Dependencies: none.

- [x] Capture machine-readable baseline results for the current case aggregate:
  process launches, wall-clock duration, accumulated process time, skipped and
  disabled cases, failures, and the slowest execution domains.
- [x] Run repeated randomized case aggregates and repeated whole-target direct
  qualification without changing registration policy.
- [x] Inventory every native-test target against initialization, teardown,
  mutable global state, fixture writes, process termination, timeout, and
  registered resource locks.
- [x] Classify intentional crash, abrupt-exit, and concurrent-process probes as
  exceptional runners. Verify that GoogleTest death tests and ordinary
  `RHIExit` teardown do not incorrectly enter this category.
- [x] Select a small pilot set of deterministic low-cost targets, then order the
  remaining ordinary targets into migration groups. Renderer, GPU,
  external-process, object-system, and concurrency usage affects ordering but
  does not create a permanent case-default category.
- [x] Record a concrete teardown repair or target split for every ordinary
  target that fails direct qualification. Unknown behavior is a Stage 0 gap,
  not an accepted long-term exception.
- [x] Store raw counts and timings in test/JUnit evidence rather than copying a
  current suite total into long-lived documentation.

#### Acceptance Gate

- The baseline can be reproduced through documented DurinDevTool entrypoints.
- Every target is classified as an ordinary migration candidate or a narrowly
  justified exceptional runner.
- The pilot set passes normal, randomized, and repeated direct execution with
  clean teardown and no retained successful sandboxes.
- Every ordinary target that does not pass direct qualification has a bounded
  repair or target-split action.

### Stage 1: Add declarative CMake execution policy

Dependencies: Stage 0.

- [x] Add a validated migration property for default execution granularity.
  Begin with explicit `CASE` and `TARGET` values, then make `TARGET` the
  fallback after all ordinary targets migrate.
- [x] Require a valid direct lifecycle for every ordinary target and a temporary
  rationale plus repair stage for every `CASE` exception.
- [x] Generate `native-test-case`, `native-test-target`, and exactly one
  `native-test-default` registration family per ordinary target.
- [x] Preserve timeouts, resource locks, existing target labels, working
  directories, and process-sandbox behavior on both registration families.
- [x] Keep characterization registrations outside ordinary default selection
  and reject missing repair metadata, contradictory declarations, or
  unsupported property values during configure.
- [x] Extend CMake policy probes to cover label selection, compatibility labels,
  resource propagation, and every rejection path.

#### Acceptance Gate

- Configuration tests prove that every ordinary target contributes one and
  only one default registration family.
- Before the first migration, hybrid selection is behaviorally identical to the
  current case aggregate; after the final migration, hybrid and target select
  the same ordinary registrations.
- Case and target registrations retain identical resource locks and timeout
  policy for the same execution domain.

### Stage 2: Expose target, case, and transitional hybrid aggregate modes

Dependencies: Stage 1.

- [x] Add a typed DurinDevTool aggregate granularity option with `target`,
  `case`, and transitional `hybrid` values; reject it for a focused target where
  it has no meaning.
- [x] Use hybrid as the temporary rollout default, then switch the default to
  target in Stage 4. Case is always explicit.
- [x] Select aggregate registrations by CTest labels while preserving timeout,
  random scheduling, regex, job count, compact output, and JUnit behavior.
- [x] In target and hybrid modes, propagate random scheduling into each test
  process through GoogleTest shuffle configuration as well as CTest
  `--schedule-random`; record or print the seed needed for reproduction.
- [x] Define regex behavior explicitly: a case-name regex against a batched
  target requires case mode; reject an ambiguous empty selection with an
  actionable command rather than silently running nothing.
- [x] Deprecate `--include-direct`: during migration it runs only direct
  registrations not already selected by hybrid mode; after target becomes the
  default it is removed or accepted as a no-op compatibility alias.
- [x] On batched failure, print a deterministic case-mode rerun command and the
  preserved sandbox location already emitted by the harness.
- [x] Extend DurinDevTool parser, configuration, runtime-command, interruption,
  JUnit, and error-message tests for all modes.

#### Acceptance Gate

- Command tests prove the exact CTest label expressions for all modes and option
  combinations.
- Hybrid with no batched targets matches the current aggregate registration set.
- Case mode runs every discovered ordinary case, target mode runs every
  ordinary direct registration, and neither includes characterization tests.
- `--include-direct` introduces no duplicate target process after hybrid mode.

### Stage 3: Pilot target-level execution and repair lifecycle defects

Dependencies: Stage 2.

- [x] Opt the Stage 0 pilot targets into target-default execution and record
  their owned initialization, reset, and teardown boundaries.
- [x] Add or repair explicit reset/teardown in test support when pilot evidence
  exposes state leakage; do not change production behavior to accommodate test
  batching.
- [x] Run repeated hybrid, case, and target aggregates with randomized order and
  retain JUnit evidence for process count, duration, and failures.
- [x] Inject one controlled assertion failure and one retained-work diagnostic
  in a probe target to verify batched reporting and case-mode rerun guidance.
- [x] Compare pilot failures and skips across all three modes; any semantic
  difference blocks migration of that target.
- [x] Confirm that single-target runs remain one process and do not introduce
  per-case execution as an acceleration path.

#### Acceptance Gate

- Every pilot target passes repeated target-level and case-level runs in normal
  and randomized schedules.
- Hybrid reduces aggregate process launches by at least 25 percent from the
  Stage 0 baseline without increasing median wall-clock time by more than 10
  percent.
- Failure output identifies the owning target and failing GoogleTest case, keeps
  the failed sandbox, and gives a working case-mode rerun command.

### Stage 4: Migrate every ordinary target and qualify the target default

Dependencies: Stage 3.

- [x] Migrate all remaining ordinary targets in bounded groups, repeating the
  pilot gate for each group.
- [x] Repair teardown or split cohesive execution domains when object-system,
  asset, renderer, GPU, external-tool, concurrency, or runtime lifecycle tests
  fail same-process qualification. Do not accept permanent routine case
  execution as the fix.
- [x] Keep only intentional crash/exit and concurrent-process characterization
  probes outside ordinary target execution.
- [x] Run consecutive randomized target aggregates, a complete case aggregate,
  and focused repeated target qualification on the selected Agent Build
  Profile.
- [x] Compare process launches, median and worst wall-clock duration, aggregate
  process time, failure diagnostics, and retained artifacts with Stage 0.
- [x] Make target mode the default, make the CMake fallback `TARGET`, and remove
  every temporary ordinary `CASE` exception.
- [x] Update the native-test and build/run documentation with the implemented
  modes, authoring rules, focused workflow, and case-diagnostic fallback.

#### Acceptance Gate

- The target default reduces process launches by at least 90 percent from the
  Stage 0 baseline and routine launch count is bounded by ordinary target and
  infrastructure registration count rather than GoogleTest case count.
- Target-default median wall-clock time does not regress from baseline, and its
  worst qualified run is no more than 10 percent slower than the baseline worst
  run.
- Consecutive randomized target aggregates and the complete case aggregate pass
  with the same expected skips and disabled tests.
- Every ordinary target runs at target granularity; remaining multi-process
  registrations are characterization infrastructure with a concrete reason.
- The authoritative native-test documentation, CMake policy tests, and
  DurinDevTool command tests agree on the final behavior.

## Validation Matrix

| Area | Focused validation | Aggregate validation | Required evidence |
| --- | --- | --- | --- |
| CMake policy | Discovery policy and failure probes | Configure the ordinary test preset | Default-family uniqueness, invalid declarations rejected, locks/timeouts preserved |
| DurinDevTool | Parser, request, command construction, JUnit, and failure-message tests | Exercise each aggregate mode through the root wrapper | Exact labels, no characterization leakage, no duplicate direct phase |
| Pilot targets | Normal, shuffled, repeated target, and case-isolated runs | Randomized hybrid and case aggregates | Same pass/skip set, clean teardown, no retained successful work |
| Failure path | Controlled assertion and retained-work probe | Failed batched aggregate followed by case rerun | Case name, target, sandbox, and actionable rerun are visible |
| Shared resources | Selected lock-bearing targets in target and case modes | Target aggregate under normal parallelism | GPU and legacy renderer locks remain serialized without requiring case execution |
| Performance | Stage 0 and per-stage JUnit/timing capture | Consecutive final target and case aggregates | Relative launch reduction and wall-clock gates pass |

All configure, build, and test execution follows
[Build And Run](../../../Development/Build/BuildAndRun.md). Native-test authoring and
runtime sandbox rules remain owned by
[Native C++ Tests](../../../Development/Build/NativeTests.md).

## Definition of Done

- Target is the documented default aggregate; case remains an explicit complete
  diagnostic and independence-qualification mode.
- Process launch and wall-clock acceptance gates pass on one recorded Agent
  Build Profile.
- Case isolation remains available for the complete ordinary suite and for
  focused diagnosis.
- Every ordinary target owns deterministic reset and teardown, while exceptional
  multi-process characterization remains separately registered and documented.
- Failed batches remain diagnosable without automatic success masking.
- Lasting behavior is moved into the authoritative build/test documentation,
  all affected automated tests pass, and the plan is marked completed.

## Deferred Follow-ups

- Bounded sharding inside one large target; consider it only when repeated
  measurement shows that target is the aggregate critical path and a small
  fixed shard count improves wall time by at least 20 percent. One process per
  case is not an acceleration strategy.
- Historical timing dashboards or CI trend storage beyond the JUnit artifacts
  needed for this migration.
- Automatic failed-case reruns. A later plan may add explicitly reported flaky
  retry policy, but this plan keeps failure semantics single-attempt and strict.
- Release and Shipping parity qualification unless a stage changes
  configuration-dependent registration behavior.

## Related Documentation

- [Native C++ Tests](../../../Development/Build/NativeTests.md)
- [Build And Run](../../../Development/Build/BuildAndRun.md)
- [Native Test Execution Granularity Stage 0 Evidence](../../../Development/Build/NativeTestExecutionGranularityStage0.md)
- [Native Test Execution Granularity Stage 3 Evidence](../../../Development/Build/NativeTestExecutionGranularityStage3.md)
- [Native Test Execution Granularity Stage 4 Evidence](../../../Development/Build/NativeTestExecutionGranularityStage4.md)
- [Native Test Process Isolation](../2026-07/NativeTestProcessIsolation.md)
- [Native Test Process Isolation Stage 0 Evidence](../../../Development/Build/NativeTestProcessIsolationStage0.md)
- [Native Test Process Isolation Stage 2 Evidence](../../../Development/Build/NativeTestProcessIsolationStage2.md)
- [Native Test Process Isolation Stage 3 Evidence](../../../Development/Build/NativeTestProcessIsolationStage3.md)

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
