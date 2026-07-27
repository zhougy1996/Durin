# Native Test Process Isolation Plan

Summary: Make aggregate native tests deterministic under CTest parallelism by giving every test process an isolated writable sandbox and reserving serialization for genuinely shared resources.

Last reviewed: 2026-07-28

Status: Active
Completed:

## Current Status

- `.\DevTool.bat test --target all` now schedules CTest-discovered GoogleTest
  cases with the Agent Build Profile job count; the current profile runs 18
  cases concurrently.
- Every case from a test target currently receives the same
  `DURIN_TEST_WORK_DIR`. Tests in separate processes therefore create, delete,
  mount, and rewrite the same files concurrently.
- A 2026-07-28 aggregate run reproduced 31 failures out of 647 tests. The
  failures included `remove_all` sharing violations, missing files during copy
  and load, failed package and shader-cache publication, and thumbnail fixture
  setup failures. The same thumbnail suites passed when run in one process,
  including 100 shuffled repetitions.
- The repository currently has 138 `DURIN_TEST_WORK_DIR` references in 42
  native-test source/header files, and 32 native-test files call
  `std::filesystem::remove_all`.
- `FTextureCubeAssetThumbnailTests.ProviderRejectsMissingRegistryData` also has
  an independent order dependency: it constructs a path under
  `/RenderedThumbnailFixtures/` without registering that mount in its own
  process. It passes in a combined executable only when an earlier test creates
  the fixture mount.
- `FMaterialAssetThumbnailTests.InvalidInstancePublishesOneStableDiagnostic`
  creates fixtures in the shared `RenderedAssetThumbnailFixtures` directory.
  Its diagnostic assertions are stable in isolation, but fixture creation and
  loading race with sibling CTest processes that delete and rebuild the same
  directory.

## Goal

Preserve case-level CTest discovery and useful aggregate parallelism while
making every native test pass independently of execution order, process
overlap, prior artifacts, and process-global initialization performed by other
cases.

## Scope

- Native-test target creation and GoogleTest discovery helpers.
- Ownership and lifecycle of generated test files under each target's `Work`
  directory.
- A common native-test process harness and runtime work-directory API.
- Migration of repository native tests away from the shared compile-time work
  directory.
- Explicit CTest resource declarations for hardware, ports, caches, or other
  resources that cannot be isolated by filesystem layout.
- Aggregate-test validation through DurinDevTool.
- Updates to the authoritative native-test documentation.

## Non-Goals

- Changing checked-in fixture contents or moving target-owned `Data`
  directories without a separate ownership reason.
- Treating `-j 1` as the permanent fix.
- Splitting test executables solely to hide shared-directory races.
- Running multiple build or test drivers concurrently in one checkout; the
  existing checkout ownership lock remains authoritative.
- Making production asset, shader-cache, or file APIs tolerate destructive
  races created only by the test harness.

## Design Decisions and Invariants

- CTest-discovered GoogleTest cases remain the aggregate scheduling and
  reporting unit.
- `<TestTarget>/Data` is deployed, read-only input shared by every process.
- `<TestTarget>/Work` is a container, not a writable test sandbox. Each process
  writes only below a unique run directory such as
  `<TestTarget>/Work/Runs/<process-id>-<nonce>/`.
- The common test harness creates the run directory before GoogleTest executes,
  exposes it through a runtime API, and never deletes another process's
  directory.
- Successful run directories are removed by the harness. Failed or crashed run
  directories are retained and reported for diagnosis. An explicit keep-work
  option preserves successful output when requested.
- Tests receive the runtime sandbox through a test-support API. The
  `DURIN_TEST_WORK_DIR` compile definition is removed after migration so new
  direct writes to the shared target root fail at compile time. CMake may retain
  an internal `DURIN_TEST_WORK_ROOT` target property for deployment and harness
  configuration, but ordinary tests do not use it.
- A test registers every virtual mount and initializes every process-global
  subsystem it requires. No case may depend on another case having run in the
  same process.
- Target-scoped CTest serialization is a compatibility mechanism during
  migration. A target becomes case-parallel only after all of its writable
  paths and process-global resources pass the isolation audit.
- After migration, CTest resource locks describe only irreducible shared
  resources such as one physical GPU queue, a fixed external port, or a
  deliberately shared external service. Filesystem output receives isolation,
  not a lock.
- Repeated runs may reuse deterministic logical fixture names inside a process
  sandbox. Uniqueness is provided at the sandbox boundary rather than scattered
  through individual tests.

## Current Foundations and Gaps

### Foundations

- `add_durin_test` already owns distinct root, `Bin`, `Data`, and `Work`
  locations per target.
- Every native-test target uses `gtest_discover_tests`, so registration policy
  can be centralized without changing individual test names.
- DurinDevTool already owns aggregate concurrency and the checkout lock.
- Most generated paths are derived from `DURIN_TEST_WORK_DIR`, so a common
  runtime replacement has a bounded migration surface.

### Gaps

- Test discovery is repeated in each target CMake file and has no shared
  isolation or resource policy.
- The work path is a compile-time absolute string shared by every process of a
  target.
- Fixture helpers commonly perform uncoordinated `remove_all` on fixed
  target-relative directories.
- Some tests rely on mounts, object-system state, render state, or cached static
  fixture pointers initialized by earlier cases.
- Aggregate validation currently demonstrates concurrency failures but does not
  include a focused regression that proves two cases cannot observe or delete
  each other's output.
- Failed-run artifact retention and cleanup have no repository-wide contract.

## Implementation Stages

### Stage 0: Freeze the isolation inventory and acceptance baseline

- [ ] Record the native-test targets, all writable path call sites, static
  fixture caches, mount registrations, fixed ports, GPU/runtime dependencies,
  and other process-external resources.
- [ ] Classify each target as filesystem-isolatable, explicitly
  resource-constrained, or requiring a separate integration-test executable.
- [ ] Capture a machine-readable aggregate baseline containing the failing test
  names and collision signatures from an 18-job run.
- [ ] Add a focused harness regression with two discovered cases that use the
  same logical filename and overlap in time; it must fail under the legacy
  shared root and pass under distinct process sandboxes.
- [ ] Decide the portable process-id/nonce implementation and failed-artifact
  naming format before exposing the test-support API.

#### Acceptance Gate

- Every current use of the shared work root and every non-filesystem shared
  resource has an owner and migration classification.
- The focused regression reproduces the collision without relying on timing
  from unrelated engine tests.

### Stage 1: Contain aggregate concurrency behind one discovery helper

- [ ] Add a repository CMake helper that wraps `gtest_discover_tests` and
  centralizes working directory, timeout, labels, resource locks, and future
  harness properties.
- [ ] Migrate every native-test target from direct discovery calls to the
  helper.
- [ ] Apply a target-scoped CTest resource lock by default so cases from one
  legacy target cannot concurrently mutate its shared `Work`; retain
  cross-target parallelism because target work roots are already distinct.
- [ ] Add an explicit opt-in target property for process-isolated case
  parallelism. Reject contradictory declarations during configuration.
- [ ] Make the two missing-registry thumbnail tests register their own mount or
  use a path whose mount is part of their explicit setup.
- [ ] Add CMake/DurinDevTool tests covering default serialization, parallel-safe
  opt-in, and explicit shared-resource locks.

#### Acceptance Gate

- `test --target all` has no same-target filesystem collision failures with the
  compatibility locks enabled.
- Every discovered case still has its original CTest name and can be run
  individually.
- The missing-registry tests pass as isolated CTest processes without another
  thumbnail test running first.

### Stage 2: Introduce the native-test process sandbox

- [ ] Add a low-level test-support target usable by Core tests without an
  Engine dependency.
- [ ] Replace `GTest::gtest_main` with a common Durin test entry point that
  creates one unique process sandbox below the owning target's `Work/Runs`
  directory before invoking GoogleTest.
- [ ] Expose a runtime `GetTestWorkDirectory()`-style API and helpers for
  creating fixture subdirectories without exposing the shared work root.
- [ ] Add a GoogleTest listener that removes successful sandboxes, preserves
  failed/crashed sandboxes, prints preserved paths, and honors an explicit
  keep-work diagnostic option.
- [ ] Make initialization idempotent within a process and safe when a target is
  run directly with a GoogleTest filter outside CTest.
- [ ] Cover concurrent creation, process-id reuse, cleanup failure, long-path
  limits, non-ASCII paths, crash retention, and keep-work behavior.

#### Acceptance Gate

- Concurrent processes from one test target receive different canonical
  writable roots and cannot remove or overwrite each other's files.
- Direct target runs, filtered runs, CTest case runs, and retained failure
  artifacts all report the resolved sandbox consistently.

### Stage 3: Migrate targets and remove shared-root access

- [ ] Migrate `CoreTests` and `CoreDObjectTests`; audit helpers that initialize
  paths before a test body.
- [ ] Migrate `AssetCoreTests`; isolate package roots, registry files, derived
  data, cook output, and companion files.
- [ ] Migrate `RenderCoreTests`; isolate shader cache/store/compiler artifacts
  and declare only genuine compiler or device resource constraints.
- [ ] Migrate `EngineTests`; isolate texture imports, rendered-thumbnail
  fixtures, static-mesh fixtures, source-reference work, editor fixtures, and
  cook output.
- [ ] Migrate `VulkanRHITests` and `TextureCookTests`, retaining explicit
  resource locks where hardware or runtime lifecycle requires them.
- [ ] Replace process-static fixture caches that capture a sandbox path or raw
  object pointers with sandbox-keyed fixtures or per-test RAII ownership.
- [ ] Enable case parallelism target by target only after its isolation audit
  and focused stress run pass.
- [ ] Remove the public `DURIN_TEST_WORK_DIR` compile definition and make any
  remaining direct use a build failure.

#### Acceptance Gate

- No native-test source or header writes through a compile-time shared work
  directory.
- Every target marked parallel-safe passes repeated case-parallel runs without
  file-sharing, missing-file, stale-registry, or mount-order failures.
- Static fixture lifetime is bounded by its owning process sandbox and object
  lifecycle.

### Stage 4: Make isolation and resource ownership enforceable

- [ ] Add repository checks that reject writes into `Data`, direct use of the
  retired work macro, and unreviewed direct `remove_all` calls outside the
  current process sandbox.
- [ ] Add APIs for named per-process fixture directories and safe sandbox-local
  cleanup so tests do not rebuild path and containment checks ad hoc.
- [ ] Require CTest resource names to come from a documented central registry;
  prevent broad target locks from being added after migration without an
  explicit rationale.
- [ ] Update native-test documentation with the Data/Work/Runs contract,
  direct-run behavior, failure artifact retention, resource-lock policy, and
  examples for new targets and fixtures.
- [ ] Add periodic cleanup for abandoned successful run directories without
  deleting a directory owned by a live process.

#### Acceptance Gate

- A new test that attempts to write shared `Work` or checked-in `Data` is
  rejected by build-time checks or a focused harness test.
- The documented new-target pattern is parallel-safe by default and contains
  no manual CTest registration boilerplate.

### Stage 5: Restore and qualify full aggregate parallelism

- [ ] Run the complete native suite at 1, 2, and the Agent Build Profile's full
  job count.
- [ ] Repeat full-concurrency aggregate runs with randomized CTest scheduling
  and retain machine-readable results.
- [ ] Run every test target directly once to cover the multi-case single-process
  path.
- [ ] Verify filtered direct runs and isolated CTest reruns for representative
  Core, asset, shader, texture, thumbnail, and Vulkan failures.
- [ ] Compare elapsed time against the serialized compatibility baseline and
  record the remaining resource-lock critical path.
- [ ] Move lasting test-layout and isolation rules into
  `Documentation/Development/Build/NativeTests.md`.

#### Acceptance Gate

- Three consecutive full-job aggregate runs pass with no collision signature
  and no order-dependent failure.
- Single-job and direct-target runs also pass, proving correctness is not tied
  to concurrency.
- Remaining serialization is limited to documented irreducible resources, and
  aggregate elapsed time is recorded as the performance baseline.

## Validation Matrix

| Layer | Validation | Required result |
| --- | --- | --- |
| CMake helper | Configure-time and generated CTest metadata tests | Default lock, parallel opt-in, labels, timeouts, and explicit resources are correct |
| Test harness | Unit tests with concurrent child processes | Unique roots, containment, cleanup, retention, and keep-work behavior are deterministic |
| Test independence | Run each formerly order-dependent case alone | Mounts and process-global setup are owned by the case |
| Target migration | Repeated target-local CTest runs at full jobs | No target-local file or registry collisions |
| Aggregate correctness | `test --target all` at 1, 2, and full jobs | All registered tests pass |
| Stress | Three full-job runs with randomized scheduling | No intermittent collision or order failure |
| Direct workflow | Direct target and filtered DurinDevTool runs | Existing developer diagnosis workflow remains usable |
| Performance | Compare compatibility-lock and final elapsed times | Parallelism gain and remaining locked resources are documented |

## Definition of Done

- Every native-test process owns a unique writable sandbox.
- Checked-in test data is never mutated.
- No test relies on another case's mount, registry, object, renderer, or cache
  initialization.
- Case-level CTest parallelism is enabled for every isolatable target.
- Genuine shared resources use narrow, documented locks.
- The two reported thumbnail failures and all other reproduced aggregate
  collision failures pass under repeated full concurrency.
- Authoritative native-test documentation describes the implemented contract,
  and plan validation passes.

## Deferred Follow-ups

- Shard exceptionally long test targets only when timing data shows that
  process sandboxing and resource-lock reduction leave a meaningful critical
  path.
- Add CI artifact upload for preserved failed sandboxes after the local
  retention format is stable.
- Consider per-case disk and time budgets after deterministic isolation is in
  place.

## Related Documentation

- [Build and Run](../Development/Build/BuildAndRun.md)
- [Native C++ Tests](../Development/Build/NativeTests.md)

## Related Code

- `CMake/Project/ProjectTargets.cmake`
- `Engine/Tests/Native/*/CMakeLists.txt`
- `Tools/DurinDevTool/durin_dev_tool/build/core.py`
- `Tools/DurinDevTool/tests/test_build_core.py`
- `Engine/Tests/Native/EngineTests/Private/Thumbnail/RenderedAssetThumbnailTestFixtures.h`
- `Engine/Tests/Native/EngineTests/Private/MaterialAssetThumbnailTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/TextureCubeAssetThumbnailTests.cpp`
