# Native Test Process Isolation Plan

Summary: Make aggregate native tests deterministic under CTest parallelism by giving every test process an isolated writable sandbox and reserving serialization for genuinely shared resources.

Last reviewed: 2026-07-28

Status: Archived
Completed: 2026-07-28

## Current Status

- Stage 0 is complete. The frozen inventory maps 91 source files, 120
  GoogleTest suites, and 647 cases to 29 cohesive feature/lifecycle targets
  with no cross-domain suite owner.
- Stage 1 is complete. `durin_discover_tests` now owns working directory,
  timeout, labels, target serialization, named legacy groups, explicit
  resources, and case-parallel opt-in for every native-test target.
- Stage 2 is complete. The seven module-era executables have been replaced by
  29 feature/lifecycle targets, while the isolation probe remains a separate
  characterization target.
- Stage 3 is complete. Every native-test executable now enters through
  `NativeTestSupport`, creates a unique process sandbox, and applies the common
  cleanup/retention policy.
- Stage 4 is complete. The Core utility, file-system, concurrency, and
  reflection/object domains now use the process sandbox and run without their
  temporary target serialization locks.
- The asset package, cook, derived-data, decoder, and import domains also use
  the process sandbox and run without temporary target serialization locks.
- The shader contract, cache, service, and CPU render-contract domains are
  case-parallel. Shader cache/service output is sandbox-local, and the
  temporary cross-process `shader-compiler` compatibility group was removed;
  no irreducible compiler resource was found.
- The editor asset-workflow target is case-parallel. Its upgrade models,
  destination/import validation, content browser, source-library contract, and
  source-reference index fixtures now use the process sandbox.
- The editor property target is case-parallel. Its reflected-revision package
  mount now resolves through the process sandbox; its local mount fixture and
  process-local package identifier do not retain cross-process state.
- The material target is case-parallel. Its mutable Vulkan thumbnail inputs
  and mount use the process sandbox, while the explicit `durin-gpu` lock and
  renderer-runtime constraint remain in place for real device lifecycle
  ownership.
- The static-mesh target is case-parallel. Import, source-repair, material-slot,
  derived-data, upgrade, and editor-details fixtures use the process sandbox;
  mount initialization is test-local, and the reusable upgrade fixture tracks
  initialization by sandbox rather than caching one static root.
- The texture target is case-parallel. Texture 2D, texture-cube, derived-data,
  source-ingest, failure, and static-model import fixtures use the process
  sandbox; reusable texture and cube mounts track initialization by sandbox,
  while single-case mounts are initialized by their owning tests.
- The thumbnail, world, viewport, spline, sky-box, editor-rendering,
  editor-shell, external-tool, cooked-texture, and Vulkan integration targets
  have completed their isolation audits. Their mutable fixtures use the
  process sandbox; GPU and renderer-runtime locks remain only on targets that
  own those irreducible resources.
- The rendered-thumbnail fixture cache is keyed by process sandbox, so direct
  target runs reuse live fixture objects without exposing paths or raw object
  pointers across sandbox lifetimes.
- Stage 5 is complete. Native-test configuration now rejects retired shared-root
  access, direct recursive deletion, unregistered resource locks, broad locks
  without rationale, duplicate suite ownership, module-mirror catch-all
  targets, and unexplained heavyweight runtime linkage. Named fixture cleanup,
  abandoned-success reclamation, and the authoritative workflow contract are
  implemented.
- Stage 6 is complete. The 1-, 2-, and 14-job aggregate matrix passed; after a
  randomized schedule exposed a stale Vulkan command-list pipeline during RHI
  teardown, the lifecycle fix passed ten focused stress runs and three
  consecutive 14-job randomized aggregates in 17.85, 17.98, and 18.16 seconds.
  All 30 targets passed directly, and representative Core, asset, shader,
  texture, thumbnail, and Vulkan cases passed both filtered direct and isolated
  CTest reruns. Machine-readable evidence is retained in
  `NativeTestProcessIsolationQualification.json`.
- Three consecutive 14-job aggregate runs passed all 720 CTest entries after
  the Core targets enabled case parallelism; their real times were 20.95,
  20.52, and 23.35 seconds.
- Three further 14-job aggregate runs passed all 720 CTest entries after the
  asset targets enabled case parallelism; their real times were 17.70, 16.63,
  and 17.56 seconds.
- The RenderCore targets passed every entry across five 14-job aggregate
  schedules. Three complete aggregates passed all 720 entries in 18.23, 19.13,
  and 20.57 seconds; the other two had unrelated failures in the Core
  direct-smoke atomic publication stress and the not-yet-migrated texture-cook
  direct smoke.
- After migrating the editor asset-workflow target, its 40 direct tests passed
  and three consecutive 14-job aggregate schedules passed all 720 entries in
  18.55, 16.92, and 17.15 seconds.
- After migrating the editor property target, its 25 direct tests passed and
  three consecutive 14-job aggregate schedules passed all 720 entries in
  17.50, 16.76, and 16.84 seconds.
- After migrating the material target, its 43 direct tests passed and three
  consecutive 14-job aggregate schedules passed all 720 entries in 19.78,
  18.40, and 18.90 seconds.
- After migrating the static-mesh target, its 42 direct tests passed and three
  consecutive 14-job aggregate schedules passed all 720 entries in 18.78,
  17.63, and 19.02 seconds.
- After migrating the texture target, its 51 direct tests passed and three
  consecutive 14-job aggregate schedules passed all 720 entries in 17.61,
  17.15, and 17.62 seconds.
- The final Stage 4 batch passed direct runs for ThumbnailTests (44 tests),
  WorldTests (35), ViewportTests (45), SplineTests (10), SkyBoxTests (8),
  EditorRenderingTests (9), TextureCookIntegrationTests (1),
  CoreFileSystemTests (30 passed and one platform skip), ExternalToolTests
  (5), EditorShellTests (27), and SkyBoxVulkanIntegrationTests (1).
- Focused stress also passed three consecutive direct MaterialTests runs
  (43 tests each) after draining deferred CPU-only texture releases before
  Vulkan startup, and three consecutive direct TextureCookIntegrationTests
  runs after making renderer/RHI teardown explicit.
- After the functional-target helper began applying the audited case-parallel
  policy before test discovery, three consecutive 14-job aggregate schedules
  passed all 720 entries in 16.01, 16.16, and 16.39 seconds. The only skipped
  entries were the known mount-link platform case and the two isolation
  characterization cases.
- The missing-source texture recovery test now owns a sandbox-local DDC; its
  prior global cache allowed the direct target smoke and discovered case to
  corrupt or replace the same cache object.
- `.\DevTool.bat test --target all` now schedules CTest-discovered GoogleTest
  cases with the Agent Build Profile job count; the current profile runs 14
  cases concurrently.
- Native-test executables and runtime DLLs retain the shared `Bin` layout, but
  each engine target binary and external runtime file now has one deployment
  target reused by every test that declares the dependency. Parallel test
  builds therefore no longer attach competing `POST_BUILD` writers to the same
  DLL destination. The complete build executed 19 deployment commands for 19
  unique destinations, and the aggregate rerun passed all 720 CTest entries at
  14 jobs.
- After rebasing onto `dev`, the Stage 3 aggregate passes all 720 CTest entries
  in 14.81 seconds at 18 jobs. Unique ownership is configured for all 94 native
  `.cpp` sources, including the two new feature-test sources and the isolation
  probe.
- No native-test source or header references `DURIN_TEST_WORK_DIR`, and
  `add_durin_test` no longer publishes that compile definition. Its internal
  target property remains the discovery-time Work-container contract.
## Goal

Preserve case-level CTest discovery and useful aggregate parallelism while
making every native test pass independently of execution order, process
overlap, prior artifacts, and process-global initialization performed by other
cases.

## Scope

- Native-test target creation and GoogleTest discovery helpers.
- Decomposition of module-era test executables into feature-owned execution
  domains with explicit dependency, fixture, lifecycle, and resource
  ownership.
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
- Creating one executable per test case or splitting targets without a
  documented feature, dependency, lifecycle, fixture, or resource boundary.
- Treating target decomposition as a replacement for process sandboxes; cases
  within one functional target must still be safe under CTest process overlap.
- Running multiple build or test drivers concurrently in one checkout; the
  existing checkout ownership lock remains authoritative.
- Making production asset, shader-cache, or file APIs tolerate destructive
  races created only by the test harness.

## Design Decisions and Invariants

- CTest-discovered GoogleTest cases remain the aggregate scheduling and
  reporting unit.
- A native-test executable is a test execution domain, not a production-module
  mirror. A target contains tests that share a cohesive feature surface,
  dependency closure, process bootstrap/teardown contract, fixture ownership,
  and irreducible resource policy.
- Split a target when tests require materially different process-global
  initialization, runtime mode, renderer/device lifecycle, external tools,
  mutable fixture ownership, dependency closure, timeout class, or CTest
  resource locks. Do not split solely by source-file size or individual test
  count.
- The target topology must make direct executable runs meaningful: running all
  cases in one target may reuse only state intentionally owned and reset by
  that functional domain.
- Target boundaries and process sandboxes are complementary. A target boundary
  isolates dependency declarations, deployed data, work containers, and
  lifecycle policy; test binaries share one runtime directory whose DLLs have
  unique deployment owners. A process sandbox isolates concurrent CTest cases
  within that target.
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
- Functional-target CTest serialization and named legacy serialization groups
  are compatibility mechanisms during migration. A target becomes
  case-parallel only after all of its writable paths and process-global
  resources pass the isolation audit; multiple newly split targets retain a
  shared compatibility group while they still touch the same legacy external
  state.
- After migration, CTest resource locks describe only irreducible shared
  resources such as one physical GPU queue, a fixed external port, or a
  deliberately shared external service. Filesystem output receives isolation,
  not a lock.
- Repeated runs may reuse deterministic logical fixture names inside a process
  sandbox. Uniqueness is provided at the sandbox boundary rather than scattered
  through individual tests.

## Current Foundations and Gaps

### Foundations

- `add_durin_test` owns distinct root, `Data`, and `Work` locations per target.
  Test executables and runtime DLLs share `Bin`, with one deployment target per
  destination rather than one copy command per consuming test.
- Every native-test target uses `gtest_discover_tests`, so registration policy
  can be centralized without changing individual test names.
- CMake source lists already expose natural feature clusters within the larger
  targets, and `TextureCookTests` demonstrates that a lifecycle-specific
  executable can coexist with tests from the same production module.
- DurinDevTool already owns aggregate concurrency and the checkout lock.
- Most generated paths are derived from `DURIN_TEST_WORK_DIR`, so a common
  runtime replacement has a bounded migration surface.

### Gaps

- `durin_discover_tests` centralizes discovery metadata, applies one
  target-scoped lock by default, and supports named compatibility groups,
  explicit resources, and an audited case-parallel opt-in.
- Most target boundaries mirror production modules. `EngineTests` in particular
  has one oversized dependency and lifecycle domain, so unrelated features
  inherit renderer, editor, asset, and tooling state and cannot declare narrow
  resource policies.
- Target-owned `Data`, support code, runtime DLL deployment, and direct-run
  expectations have not been assigned to a proposed functional topology.
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

- [x] Record the native-test targets, all writable path call sites, static
  fixture caches, mount registrations, fixed ports, GPU/runtime dependencies,
  and other process-external resources.
- [x] Classify every test suite into a proposed functional execution domain and
  mark it filesystem-isolatable, explicitly resource-constrained, or requiring
  a separate integration-test executable.
- [x] Record the proposed target topology, including each target's feature
  owner, source files, support code, direct dependencies, deployed `Data`,
  process bootstrap/teardown contract, timeout class, and resource locks.
- [x] Identify shared support or fixtures that need a small test-support
  library instead of compiling unrelated feature suites into the same
  executable.
- [x] Capture a machine-readable aggregate baseline containing the failing test
  names and collision signatures from an 18-job run.
- [x] Add a focused harness regression with two discovered cases that use the
  same logical filename and overlap in time; it must fail under the legacy
  shared root and pass under distinct process sandboxes.
- [x] Decide the portable process-id/nonce implementation and failed-artifact
  naming format before exposing the test-support API.

#### Acceptance Gate

- Every current use of the shared work root and every non-filesystem shared
  resource has an owner and migration classification.
- Every existing GoogleTest suite maps to exactly one proposed execution
  domain, with intentional multi-target support code called out explicitly.
- Proposed target boundaries are justified by feature/lifecycle ownership and
  allow narrow dependency and resource declarations; none is merely a
  one-case workaround.
- The focused regression reproduces the collision without relying on timing
  from unrelated engine tests.

#### Stage 0 Handoff

- Baseline commit: `88e54d76`.
- Evidence: `Documentation/Plans/Archive/2026-07/NativeTestProcessIsolationBaseline.json`.
- Working set entering Stage 1: `CMake/Project/ProjectTargets.cmake`, the seven
  legacy native-test CMake files, the native-test root CMake file, and
  `Tools/DurinDevTool/tests/test_build_core.py`.
- Key decisions: 29 functional/lifecycle domains; one low-level
  `NativeTestSupport`; a named compatibility group distinct from irreducible
  `durin-gpu`; run directories named
  `run-p<PID>-<128-bit-lowercase-hex-nonce>`.
- Open question: select the final CMake helper name and target property names
  while preserving all 647 existing CTest names.
- Validation: plan validation passed; `NativeTestIsolationProbeTests` built and
  skipped without an explicit mode; the focused two-process characterization
  produced one legacy collision failure and two isolated-control passes.

### Stage 1: Contain aggregate concurrency behind one discovery helper

- [x] Add a repository CMake helper that wraps `gtest_discover_tests` and
  centralizes working directory, timeout, labels, resource locks, and future
  harness properties.
- [x] Migrate every native-test target from direct discovery calls to the
  helper.
- [x] Apply a target-scoped CTest resource lock by default so cases from one
  legacy target cannot concurrently mutate its shared `Work`; retain
  cross-target parallelism because target work roots are already distinct.
- [x] Support an explicit named legacy serialization group so newly split
  targets that still share an external cache, runtime, or fixture remain safe
  until that ownership is removed.
- [x] Add an explicit opt-in target property for process-isolated case
  parallelism. Reject contradictory declarations during configuration.
- [x] Make the two missing-registry thumbnail tests register their own mount or
  use a path whose mount is part of their explicit setup.
- [x] Add CMake/DurinDevTool tests covering default serialization, parallel-safe
  opt-in, and explicit shared-resource locks.

#### Acceptance Gate

- `test --target all` has no same-target filesystem collision failures with the
  compatibility locks enabled.
- Every discovered case still has its original CTest name and can be run
  individually.
- The missing-registry tests pass as isolated CTest processes without another
  thumbnail test running first.

#### Stage 1 Handoff

- Baseline commit: `754a6ee5`.
- Working set entering Stage 2: the Stage 0 functional source map, the eight
  current native-test CMake files, and private implementation sources compiled
  directly by `RenderCoreTests` and `EngineTests`.
- Key symbols: `durin_discover_tests`,
  `durin_resolve_native_test_discovery_policy`,
  `DURIN_TEST_CASE_PARALLEL_SAFE`,
  `DURIN_TEST_LEGACY_SERIALIZATION_GROUP`,
  `DURIN_TEST_RESOURCE_LOCKS`, and `DURIN_TEST_TIMEOUT`.
- Decisions: legacy targets receive `durin-test-target-<target>` by default;
  Engine/TextureCook share `durin-test-legacy-renderer-runtime`; GPU-bearing
  targets also request `durin-gpu`; only the characterization target opts into
  case parallelism before sandbox migration.
- Independent-case fixes: both missing-thumbnail-registry tests register their
  mount; the material parent-hook test initializes the object system; the
  texture reflected-build test releases transaction history before deleting
  its asset.
- Open question: while moving the mixed `FMaterialTests` suite, either retain
  one material target through Stage 2 or preserve case names while separating
  its Vulkan-backed source into an integration lifecycle domain.
- Validation: plan validation passed; the policy CMake test passed; focused
  material and texture cases passed directly; the characterization reproduced
  one legacy collision and two isolated passes; the final 18-job aggregate
  passed all 650 registrations in 25.25 seconds.

### Stage 2: Replace module-era targets with functional execution domains

- [x] Split `CoreTests` and `CoreDObjectTests` into utility/file-system,
  concurrency, and reflection/object-lifecycle domains where their bootstrap
  and dependencies differ.
- [x] Split `AssetCoreTests` into package/registry, derived-data, decoding, and
  import domains, sharing support libraries only where ownership is explicit.
- [x] Split `RenderCoreTests` into shader compiler/service, shader cache/store,
  and render-contract domains with independent timeout and resource policies.
- [x] Decompose `EngineTests` into focused editor-model, asset-workflow,
  material, static-mesh, texture, thumbnail, world, viewport, rendering, and
  external-tool domains. Keep GPU-backed cases separate from CPU-only model and
  serialization cases.
- [x] Retain or refine dedicated cooked-runtime and Vulkan integration targets;
  do not merge them into feature targets whose direct-run lifecycle is
  incompatible.
- [x] Give every new target its own `Data` deployment, `Work` container,
  dependency closure, labels, timeout, and temporary compatibility/resource
  group declarations.
- [x] Preserve every existing GoogleTest/CTest case name and add generated
  metadata checks that no suite is omitted or registered more than once.
- [x] Delete superseded module-era targets after all cases have moved.

#### Acceptance Gate

- Every native test belongs to one feature/lifecycle execution domain and no
  oversized module-mirror target remains.
- CPU-only targets do not link or initialize renderer, Vulkan, editor, or
  external-tool stacks unless their feature contract requires them.
- Every functional target passes as a direct executable, proving that its
  combined in-process lifecycle is coherent.
- Aggregate execution passes with compatibility groups enabled, and the
  remaining serialization cost is recorded before process-sandbox migration.

#### Stage 2 Handoff

- Baseline commit: `131879aa`.
- Working set entering Stage 3: `add_durin_test`,
  `durin_discover_tests`, the 29 functional target declarations under
  `Engine/Tests/Native`, and the Stage 0 isolation probe.
- Key decisions: source ownership is derived dynamically from every native
  `.cpp` passed to `add_durin_test`; configuration fails for an unowned,
  duplicate, or stale source; each target also receives one
  `Durin.NativeTestDirect.<target>` whole-executable smoke test.
- Target topology: 4 Core/object, 5 asset, 4 render-contract/shader, 14
  Engine/editor feature, and 2 dedicated Vulkan/cooked-runtime integration
  targets. The characterization probe remains outside that count.
- Dependency boundary: Core, asset, and CPU render-contract targets do not
  link editor, renderer, Vulkan, or external-tool stacks. Engine/editor targets
  link those stacks only where their feature sources or private implementations
  require them.
- Temporary serialization cost: all discovered cases retain their per-target
  compatibility lock; shader targets share `shader-compiler`; GPU lifecycle
  targets share `durin-gpu`, and renderer-startup targets additionally share
  `renderer-runtime`. The final run accumulated 112.33 process-seconds of
  native-test work and completed in 12.22 wall-clock seconds; direct lifecycle
  smokes accounted for 18.72 process-seconds.
- Validation: after the `dev` rebase, unique ownership covers 94 native
  sources; the complete `all` build passes; all 720 Stage 3 CTest entries pass
  at 18 jobs; all 30 direct executable smokes pass; and the
  legacy-collision/isolated-control probe still reproduces its expected
  outcomes.

### Stage 3: Introduce the native-test process sandbox

- [x] Add a low-level test-support target usable by Core tests without an
  Engine dependency.
- [x] Replace `GTest::gtest_main` with a common Durin test entry point that
  creates one unique process sandbox below the owning target's `Work/Runs`
  directory before invoking GoogleTest.
- [x] Expose a runtime `GetTestWorkDirectory()`-style API and helpers for
  creating fixture subdirectories without exposing the shared work root.
- [x] Add a GoogleTest listener that removes successful sandboxes, preserves
  failed/crashed sandboxes, prints preserved paths, and honors an explicit
  keep-work diagnostic option.
- [x] Make initialization idempotent within a process and safe when a target is
  run directly with a GoogleTest filter outside CTest.
- [x] Cover concurrent creation, process-id reuse, cleanup failure, long-path
  limits, non-ASCII paths, crash retention, and keep-work behavior.

#### Acceptance Gate

- Concurrent processes from one test target receive different canonical
  writable roots and cannot remove or overwrite each other's files.
- Direct target runs, filtered runs, CTest case runs, and retained failure
  artifacts all report the resolved sandbox consistently.

#### Stage 3 Handoff

- Baseline commit: `b0af550f`.
- Working set entering Stage 4: `NativeTestSupport`,
  `add_durin_test`, the 44 native-test source/header files that still reference
  `DURIN_TEST_WORK_DIR`, and the functional target discovery policies.
- Key symbols: `RunNativeTests`, `GetTestWorkDirectory`,
  `CreateTestWorkSubdirectory`, `IsTestWorkDirectoryKept`, and
  `Private::CreateUniqueRunDirectory`.
- Decisions: every executable compiles the common main and links one
  Core-only static support library; run directories use
  `Work/Runs/run-p<PID>-<32 lowercase hex>` and are allocated with atomic
  directory creation; `--durin-keep-test-work` and
  `DURIN_TEST_KEEP_WORK=1` retain successful output; successful CTest
  discovery is covered by a post-run cleanup fallback because GoogleTest does
  not dispatch program listeners for `--gtest_list_tests`.
- Lifecycle policy: passed runs are deleted; failed runs, keep-work runs,
  crashes, and cleanup failures retain and report their resolved directory.
  Harness initialization failures are reported without invoking an interactive
  Debug CRT dialog.
- Open work: migrate the 156 shared-root references, replace sandbox-capturing
  static fixtures, and remove temporary target locks only after focused stress
  runs establish case-parallel safety.
- Validation: the sandbox characterization passed concurrent-process,
  keep-work, cleanup-failure, and abrupt-exit retention probes; four focused
  API tests passed; after the `dev` rebase, the complete `all` build and all
  720 CTest entries passed at 18 jobs in 14.81 seconds; all 30 `Runs` roots
  were empty after the original Stage 3 aggregate.

### Stage 4: Migrate functional targets and remove shared-root access

- [x] Migrate the Core utility/file-system, concurrency, and reflection/object
  domains; audit helpers that initialize paths before a test body.
- [x] Migrate the asset package/registry, derived-data, decoder, and import
  domains; isolate package roots, registry files, derived data, cook output,
  and companion files.
- [x] Migrate the shader compiler/service, shader cache/store, and
  render-contract domains; declare only genuine compiler or device resource
  constraints.
- [x] Migrate the editor, material, static-mesh, texture, thumbnail, world,
  viewport, rendering, and external-tool domains; isolate every mutable
  feature fixture.
- [x] Migrate cooked-runtime and Vulkan integration domains, retaining explicit
  resource locks where hardware or runtime lifecycle requires them.
- [x] Replace process-static fixture caches that capture a sandbox path or raw
  object pointers with sandbox-keyed fixtures or per-test RAII ownership.
- [x] Enable case parallelism target by target only after its isolation audit
  and focused stress run pass.
- [x] Remove the public `DURIN_TEST_WORK_DIR` compile definition and make any
  remaining direct use a build failure.

#### Acceptance Gate

- No native-test source or header writes through a compile-time shared work
  directory.
- Every target marked parallel-safe passes repeated case-parallel runs without
  file-sharing, missing-file, stale-registry, or mount-order failures.
- Static fixture lifetime is bounded by its owning process sandbox and object
  lifecycle.

#### Stage 4 Handoff

- Baseline commit: `c4e6c2fe`.
- Working set entering Stage 5: `NativeTestSupport`, `add_durin_test`,
  native-test functional-target discovery policies, and the 32 source/header
  files that still perform direct `std::filesystem::remove_all`.
- Key symbols: `Testing::GetTestWorkDirectory`,
  `CreateRenderedAssetThumbnailFixtures`,
  `durin_add_core_functional_test`, `durin_add_engine_functional_test`, and
  `durin_discover_tests`.
- Decisions: repository test code no longer receives a compile-time work-root
  macro; Core and Engine functional targets apply their audited case-parallel
  policy before discovery; sandbox-keyed reusable fixtures preserve meaningful
  direct executable runs; `durin-gpu` and `renderer-runtime` remain explicit
  locks for device/runtime lifecycle owners.
- Open work: make sandbox containment enforceable for direct cleanup calls,
  centralize resource names, and add topology/policy checks in Stage 5.
- Validation: all focused direct targets passed, the complete `all` build
  succeeded after removal of the compile definition, and three consecutive
  14-job aggregate runs passed all 720 CTest entries in 16.01, 16.16, and
  16.39 seconds with the three known skips.

### Stage 5: Make isolation and resource ownership enforceable

- [x] Add repository checks that reject writes into `Data`, direct use of the
  retired work macro, and unreviewed direct `remove_all` calls outside the
  current process sandbox.
- [x] Add APIs for named per-process fixture directories and safe sandbox-local
  cleanup so tests do not rebuild path and containment checks ad hoc.
- [x] Require CTest resource names to come from a documented central registry;
  prevent broad target locks from being added after migration without an
  explicit rationale.
- [x] Add topology checks that reject unowned test suites, duplicate suite
  registration, module-mirror catch-all targets, and feature targets that link
  heavyweight runtime stacks without an allowlisted rationale.
- [x] Update native-test documentation with the Data/Work/Runs contract,
  functional-target boundary rules, direct-run behavior, failure artifact
  retention, resource-lock policy, and examples for new targets and fixtures.
- [x] Add periodic cleanup for abandoned successful run directories without
  deleting a directory owned by a live process.

#### Acceptance Gate

- A new test that attempts to write shared `Work` or checked-in `Data` is
  rejected by build-time checks or a focused harness test.
- The documented new-target pattern is parallel-safe by default and contains
  no manual CTest registration boilerplate.

#### Stage 5 Handoff

- Baseline: `dd44ad16ecb9fb3af3a7bd2a823afdcd9d74971c`.
- Working set: `NativeTestSupport`, `ProjectTargets.cmake`, native-test
  infrastructure policy probes, the 32 direct-cleanup source/header files,
  Engine feature-target rationales, and `NativeTests.md`.
- Key symbols: `CreateTestFixtureDirectory`, `RemoveTestWorkDirectory`,
  `CleanupAbandonedSuccessfulRunDirectories`,
  `durin_validate_native_test_repository_policy`,
  `durin_validate_native_test_source_ownership`,
  `DURIN_NATIVE_TEST_RESOURCE_LOCK_REGISTRY`, and
  `DURIN_TEST_HEAVY_RUNTIME_RATIONALE`.
- Decisions: recursive deletion is available only below the current process
  sandbox; successful cleanup failures receive a marker and become eligible
  for age/PID-safe reclamation after 24 hours; resource names and legacy groups
  are centrally registered; broad target locks and heavyweight runtime links
  require reviewable rationales; feature targets own each suite exactly once.
- Open work: execute and record the Stage 6 qualification matrix, including
  1-, 2-, and full-job aggregates, randomized schedules, direct targets, and
  filtered reruns.
- Validation: configuration, the focused isolation harness, six negative policy
  probes, and plan validation passed; a full `all` build and final 14-job
  aggregate passed all 722 registered entries in 17.72 seconds with the three
  known skips.

### Stage 6: Restore and qualify full aggregate parallelism

- [x] Run the complete native suite at 1, 2, and the Agent Build Profile's full
  job count.
- [x] Repeat full-concurrency aggregate runs with randomized CTest scheduling
  and retain machine-readable results.
- [x] Run every test target directly once to cover the multi-case single-process
  path.
- [x] Verify filtered direct runs and isolated CTest reruns for representative
  Core, asset, shader, texture, thumbnail, and Vulkan failures.
- [x] Compare elapsed time against the serialized compatibility baseline and
  module-era baseline; record target startup cost, dependency/build impact, and
  the remaining resource-lock critical path.
- [x] Move lasting test-layout and isolation rules into
  `Documentation/Development/Build/NativeTests.md`.

#### Acceptance Gate

- Three consecutive full-job aggregate runs pass with no collision signature
  and no order-dependent failure.
- Single-job and direct-target runs also pass, proving correctness is not tied
  to concurrency.
- Remaining serialization is limited to documented irreducible resources, and
  aggregate elapsed time is recorded as the performance baseline.

#### Stage 6 Handoff

- Baseline: `fa3cbd84a6dff81a344596728d27501416bf7242`.
- Working set: DurinDevTool aggregate-test options and tests,
  `VulkanTextureSamplingTests.cpp`, `NativeTests.md`, and qualification
  evidence.
- Key symbols: `test_schedule_random`, `test_output_junit`,
  `test_ctest_regex`, `run_all_native_tests`, and
  `FRHICommandListImmediate::SwitchPipeline`.
- Decisions: randomized and isolated CTest qualification remains behind the
  DurinDevTool entrypoint; JUnit paths are caller-selected; the immediate
  command list releases its Vulkan pipeline before device teardown; only the
  registered GPU and renderer-runtime resources remain serialized.
- Open work: none. Deferred refinements remain listed below.
- Validation: all 722 registrations passed at 1 and 2 jobs; three consecutive
  randomized 14-job runs passed with the three known skips; all 30 targets
  passed directly; six representative domains passed filtered direct and
  isolated CTest reruns; the all-plan validator passed.

## Validation Matrix

| Layer | Validation | Required result |
| --- | --- | --- |
| Target topology | Generated suite-to-target and dependency metadata checks | Every suite has one feature owner; heavyweight dependencies and resource policies are narrow and justified |
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
- Native-test executables represent coherent feature/lifecycle domains rather
  than production-module mirrors, and every suite has exactly one target owner.
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

- Further shard an already coherent feature target only when lifecycle,
  dependency, resource, or measured critical-path evidence justifies a new
  execution domain.
- Add CI artifact upload for preserved failed sandboxes after the local
  retention format is stable.
- Consider per-case disk and time budgets after deterministic isolation is in
  place.

## Related Documentation

- [Build and Run](../../../Development/Build/BuildAndRun.md)
- [Native C++ Tests](../../../Development/Build/NativeTests.md)
- [18-job Failure Baseline](NativeTestProcessIsolationBaseline.json)
- [Stage 6 Qualification](NativeTestProcessIsolationQualification.json)

## Related Code

- `CMake/Project/ProjectTargets.cmake`
- `Engine/Tests/Native/*/CMakeLists.txt`
- `Engine/Tests/Native/NativeTestIsolationProbeTests/*`
- `Tools/DurinDevTool/durin_dev_tool/build/core.py`
- `Tools/DurinDevTool/tests/test_build_core.py`
- `Engine/Tests/Native/EngineTests/Private/Thumbnail/RenderedAssetThumbnailTestFixtures.h`
- `Engine/Tests/Native/EngineTests/Private/MaterialAssetThumbnailTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/TextureCubeAssetThumbnailTests.cpp`
