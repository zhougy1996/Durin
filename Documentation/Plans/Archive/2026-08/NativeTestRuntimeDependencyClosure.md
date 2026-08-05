# Native Test Runtime Dependency Closure Plan

Summary: Derive each native test's deployable runtime closure from its final CMake target graph while preserving shared-destination single-writer deployment.

Last reviewed: 2026-08-05

Status: Archived
Completed: 2026-08-05

## Current Status

- Stages 0 through 2 are complete. The default Agent Build Profile contains 39
  native-test targets. Their deterministic linked runtime sets are recorded
  below and are now collected by reusable, alias-normalized, cycle-safe CMake
  traversal rather than by an audit-only source parser.
- A cold pre-fix `SplineTests` build reproduced a GoogleTest discovery failure
  with `0xc0000135`; the build deployed only `DurinEd`. Building and passing
  `RHICommandListTests` still left `SplineTests` undiscoverable. Building
  `EditorAssetWorkflowTests` populated the wider shared `Bin`, after which the
  unchanged `SplineTests` executable discovered and passed all ten cases. This
  complements the previously recorded stale-RHI `0xc0000139` reproduction and
  confirms a deployment-edge defect rather than a Spline assertion failure.
- The audit found widespread missing transitive modules and Slang files in
  AssetImport, editor, World, Viewport, Spline, rendering, and integration
  targets. It also found unnecessary manual extras: `Renderer` on
  `EditorAssetWorkflowTests`, and `StandardAssetImport` plus Assimp on
  `VulkanRHIIntegrationTests`.
- `VulkanRHI` is the only genuine unlinked runtime module. It is owned by the
  six Vulkan-backed targets listed below because `RHIInit()`/Renderer selects
  the backend dynamically. No current test needs a standalone runtime-only file
  exception; Assimp and Slang files are reached through introducing imported
  targets.
- Configure fixtures pass for aliases, private/public links, `LINK_ONLY`,
  `TARGET_NAME`, `TARGET_NAME_IF_EXISTS`, Debug/optimized selection, cycles,
  static/interface/object bridges, imported runtime files, runtime-only roots
  and files, unknown expressions, linked-exception rejection, missing targets,
  and destination collisions. The production graph currently contributes no
  other target-bearing generator-expression form.
- `durin_discover_tests()` now registers the derived closure before
  `gtest_discover_tests()`. All ordinary target/file deployment lists have been
  removed from native-test declarations. A cold direct `SplineTests` build
  deployed 11 engine DLLs and three Slang files exactly once, then discovered
  and passed all ten cases without another test target running first.
- Qualification is complete. Cold direct Core (filtered), RHI, Spline,
  Viewport, and Vulkan targets passed. An unchanged Spline rebuild emitted zero
  deployment commands; after a temporary binary-changing RHI edit, only RHI was
  redeployed and the runtime/test hashes matched. The edit was restored before
  aggregate validation.
- The original cold aggregate passed 931 registered tests and emitted 19
  deployment commands for 19 unique runtime destinations. After rebasing onto
  `dev`, the new World baseline case links `StandardAssetImport` so a cold DDC
  miss can rebuild `/Engine/Models/Box`; direct `WorldTests` passed 62 cases and
  the default aggregate passed 932 registrations. `--include-direct` now runs
  those cases first and starts the 38 whole-executable registrations only after
  the case phase passes, so duplicate modes never overlap. The two phases
  passed in 29 seconds, including all 44 isolated `StaticMeshTests` cases and
  its complete single-process lifecycle. The all-plan validator passes, and
  lasting authoring/diagnostic rules now live in the native-test build guide.

### Stage 0 Runtime Inventory

Each row is an exact sorted closure group. `Slang` means the three files
published by `Slang_Imported`; `Assimp` means the file published by
`assimp::assimp`. Runtime-only roots are shown separately from linked targets.

| Native-test targets | Linked shared/module targets | External files | Runtime-only roots |
| --- | --- | --- | --- |
| `CoreUtilityTests`, `CoreFileSystemTests`, `CoreConcurrencyTests`, `NativeTestIsolationProbeTests`, `NativeTestIsolationCharacterizationTests` | `Core` | None | None |
| `CoreObjectTests` | `Core`, `CoreDObject` | None | None |
| `AssetPackageTests`, `AssetCookTests`, `AssetDerivedDataTests`, `AssetDecodeTests` | `AssetCore`, `Core`, `CoreDObject` | None | None |
| `AssetImportCoreTests` | `AssetCore`, `AssetImportCore`, `Core`, `CoreDObject` | None | None |
| `AssetImportTests` | `ApplicationCore`, `AssetCore`, `AssetImportCore`, `Core`, `CoreDObject`, `Engine`, `Mona`, `MonaCore`, `RHI`, `RenderCore`, `StandardAssetImport` | Assimp, Slang | None |
| `RHICommandListTests`, `RHIThreadTests`, `RHIInitializationTests` | `Core`, `RHI` | None | None |
| `RenderShaderContractTests`, `RenderShaderCacheTests`, `RenderShaderServiceTests`, `RenderContractTests` | `Core`, `RHI`, `RenderCore` | Slang | None |
| `VulkanRHIIntegrationTests` | `ApplicationCore`, `AssetCore`, `Core`, `CoreDObject`, `Engine`, `Mona`, `MonaCore`, `RHI`, `RenderCore` | Slang | `VulkanRHI` |
| `EditorPropertyTests`, `EditorAssetWorkflowTests`, `ViewportTests`, `SplineTests`, `EditorShellTests`, `ExternalToolTests` | `ApplicationCore`, `AssetCore`, `Core`, `CoreDObject`, `DurinEd`, `Engine`, `Mona`, `MonaCore`, `MonaImGui`, `RHI`, `RenderCore` | Slang | None |
| `WorldTests` | `ApplicationCore`, `AssetCore`, `AssetImportCore`, `Core`, `CoreDObject`, `DurinEd`, `Engine`, `Mona`, `MonaCore`, `MonaImGui`, `RHI`, `RenderCore`, `StandardAssetImport` | Assimp, Slang | None |
| `EditorHierarchyTests`, `EnvironmentLightingTests` | `ApplicationCore`, `AssetCore`, `Core`, `CoreDObject`, `Engine`, `Mona`, `MonaCore`, `RHI`, `RenderCore` | Slang | None |
| `MaterialTests`, `StaticMeshTests`, `TextureTests`, `SceneImportVulkanTests`, `EditorRenderingTests` | `ApplicationCore`, `AssetCore`, `AssetImportCore`, `Core`, `CoreDObject`, `DurinEd`, `Engine`, `Mona`, `MonaCore`, `MonaImGui`, `RHI`, `RenderCore`, `Renderer`, `StandardAssetImport` | Assimp, Slang | `VulkanRHI` for `MaterialTests` and `SceneImportVulkanTests` |
| `ThumbnailTests`, `SkyBoxTests`, `SkyBoxVulkanIntegrationTests` | `ApplicationCore`, `AssetCore`, `Core`, `CoreDObject`, `DurinEd`, `Engine`, `Mona`, `MonaCore`, `MonaImGui`, `RHI`, `RenderCore`, `Renderer` | Slang | `VulkanRHI` for `SkyBoxVulkanIntegrationTests` |
| `RendererResourceReloadVulkanTests`, `TextureCookIntegrationTests` | `ApplicationCore`, `AssetCore`, `Core`, `CoreDObject`, `Engine`, `Mona`, `MonaCore`, `RHI`, `RenderCore`, `Renderer` | Slang | `VulkanRHI` |

### Stage Handoffs

- Stage 0 baseline: `2c1161a7637ad03300a77c232ffffd3f4fe8bafc`
  (squashed task parent).
  Working set: the plan, native-test CMake declarations, module descriptors,
  shared deployment helpers, and isolated test outputs. Decision: retain only
  `VulkanRHI` as a runtime-only target; remove the other unlinked manual extras.
  Validation: cold Spline discovery failure, insufficient RHI-only refresh,
  shared-residue recovery, and configure audit for all 39 targets. Open
  questions: none for Stage 1.
- Stage 1 baseline: `2c1161a7637ad03300a77c232ffffd3f4fe8bafc`
  (squashed task parent).
  Working set: `TargetDependencyClosure.cmake`, `ProjectOutputs.cmake`,
  `ProjectTargets.cmake`, and the runtime-closure configure fixtures. Key
  symbols: `durin_collect_target_dependency_closure()`,
  `durin_resolve_target_link_item()`, and
  `durin_test_collect_runtime_dependency_closure()`. Decision: imported targets
  contribute declared `DURIN_RUNTIME_DEPLOY_FILES` but not arbitrary developer
  machine binaries. Validation: configure succeeds and
  `Durin.NativeTestRuntimeClosure` passes. Open question for Stage 2: none.
- Stage 2 baseline: `2c1161a7637ad03300a77c232ffffd3f4fe8bafc`
  (squashed task parent). Working set: shared deployment registration,
  native-test helper declarations, runtime-only Vulkan owners, repository
  policy probes, and the closure fixtures. Key symbol:
  `durin_test_register_runtime_dependency_closure()`. Decisions: discovery owns
  finalization; linked manual duplicates fail configuration; only dynamic
  `VulkanRHI` selection uses an exception. Validation: migrated configuration,
  closure/policy contracts, and cold direct `SplineTests` all pass. Open
  question for Stage 3: qualify the wider cold, incremental, aggregate, random,
  and direct-lifecycle matrix.
- Stage 3 baseline: `2c1161a7637ad03300a77c232ffffd3f4fe8bafc`
  (squashed task parent). Working set: registered test outputs, deployment
  logs, the native-test build guide, and this plan. Decisions: retain the shared
  `Bin` model after confirming 19 commands equal 19 unique destinations; keep
  case isolation and whole-executable lifecycle qualification as separate
  sequential phases rather than concurrent duplicate registrations. Validation:
  cold representative targets, incremental RHI refresh, cold aggregate, two
  randomized aggregates, phased direct lifecycle, and all-plan validation pass.
  Open questions: none.

## Goal

Make every native-test target independently buildable and runnable from clean
registered outputs without duplicating its transitive runtime dependency list.
A production-module split or dependency change must propagate to consuming
tests through the CMake target graph rather than requiring edits to every test
that reaches the module transitively.

## Scope

- Native-test runtime binary deployment under the existing shared test `Bin`.
- Traversal of concrete CMake link dependencies after a test target has
  completed its link declarations.
- Deployment of repository shared/module targets and external runtime files
  published through `DURIN_RUNTIME_DEPLOY_FILES`.
- Explicit declarations for runtime-only modules or files that are loaded
  without a CMake link edge.
- Configure-time diagnostics and contract coverage for unsupported or
  incomplete runtime dependency metadata.
- Migration of all native-test targets to the derived-closure path.
- Cold-output direct-target, aggregate, and randomized validation.

## Non-Goals

- Changing per-process writable sandbox ownership, CTest scheduling, resource
  locks, or direct-lifecycle policy.
- Giving each test target a private DLL directory; native tests continue to
  share one `Bin` directory per configured runtime variant.
- Replacing CMake target dependencies with post-link PE/ELF/Mach-O scanning.
- Automatically deploying arbitrary data files, project assets, dynamically
  discovered plugins, or developer-machine libraries that have no declared
  target metadata.
- Changing production module boundaries or Spline runtime behavior.
- Removing the low-level single-target and single-file deployment primitives.

## Design Decisions and Invariants

- The final CMake target graph is the source of truth for linked runtime
  dependencies. Built-binary inspection may be used as validation evidence but
  does not own build ordering or deployment.
- `durin_discover_tests(<target>)` is the runtime-closure finalization point.
  Repository policy already requires every native test to call it after target
  creation, link declarations, and target policy setup.
- Closure traversal is deterministic, cycle-safe, and alias-normalized. It
  visits both `LINK_LIBRARIES` and `INTERFACE_LINK_LIBRARIES` edges that resolve
  to concrete CMake targets.
- Shared and module library targets contribute their target binary to the
  deployment set. Static, object, and interface libraries are traversed but do
  not contribute a binary. The native-test executable itself is never copied as
  a dependency.
- Every visited target contributes files from `DURIN_RUNTIME_DEPLOY_FILES`.
  This keeps external runtime ownership, such as Slang or Assimp files, on the
  target that introduces it.
- Runtime-only targets and files have a separate explicit property or helper.
  This exception is for plugin, delay-load, or data-driven loading without a
  link edge; it is not an alternate way to restate an ordinary link closure.
- Supported generator-expression wrappers are normalized explicitly. A
  target-bearing expression that cannot be resolved for the configured preset
  fails configuration with the owning target and expression in the diagnostic;
  it is never silently skipped.
- Derived deployment continues to call
  `durin_test_get_target_binary_deployment(...)` and
  `durin_test_get_runtime_file_deployment(...)`. Every shared destination
  therefore retains one deployment writer, collision checking, incremental
  copying, and reuse across consuming tests.
- Closure ordering affects diagnostics only. Build correctness comes from
  target dependencies, and destination identity continues to define deployment
  deduplication.
- A native-test target may not rely on a DLL or runtime file deployed solely by
  another test target. Aggregate success is insufficient evidence when the same
  target fails from clean outputs.
- Existing explicit deployment helpers remain available as low-level
  primitives and for staged migration, but final native-test definitions do not
  manually enumerate ordinary transitive link closures.

## Current Foundations and Gaps

### Foundations

- `durin_test_get_target_binary_deployment(...)` already resolves aliases,
  assigns a stable shared deployment target, depends on the production target,
  and copies incrementally.
- `durin_test_get_runtime_file_deployment(...)` already rejects conflicting
  sources for one destination and provides shared ownership for external files.
- `durin_discover_tests(...)` is the mandatory centralized finalization path for
  native-test registration and already validates target policy.
- `durin_assert_target_dependency_closure_excludes(...)` demonstrates a
  cycle-safe repository traversal of `LINK_LIBRARIES` and
  `INTERFACE_LINK_LIBRARIES` with alias and common generator-expression
  handling.
- All engine functional tests are declared through bounded CMake helpers, so a
  central migration does not require changes to test source files.

### Gaps

- The deployment layer consumes only explicitly named targets and files; it
  does not derive a test's runtime closure.
- Manual `DEPLOY_TARGETS` lists duplicate production dependency knowledge and
  become stale when modules are split or acquire new runtime dependencies.
- External runtime files are deployed only when a test repeats the introducing
  imported target explicitly.
- The shared test `Bin` preserves files across builds. Missing deployment edges
  can therefore pass after another test populates the directory and fail under
  a different build order or ABI revision.
- Configure validation checks source ownership, resource policy, and forbidden
  dependencies, but not whether a native test has a complete deployable runtime
  closure.
- Current documentation explains shared deployment ownership but leaves
  transitive closure maintenance to each test author.

## Implementation Stages

### Stage 0: Inventory the concrete runtime graph and lock the closure contract

- [x] Enumerate every native-test target, its direct CMake link roots, current
  target/file deployment declarations, and the derived concrete dependency
  closure for the default Agent Build Profile.
- [x] Classify every target type and generator-expression form encountered in
  native-test dependency traversal. Record explicit handling for aliases,
  `LINK_ONLY`, `TARGET_NAME_IF_EXISTS`, configuration selection, imported
  targets, and runtime-file properties.
- [x] Identify runtime-only modules/files that do not have a link edge and give
  each one an owning target plus rationale.
- [x] Report missing and redundant deployment declarations without mutating the
  build graph. The report must include the reproduced `SplineTests` RHI gap and
  any equivalent gaps in World, Viewport, editor, rendering, or integration
  targets.
- [x] Reproduce the `SplineTests` discovery failure and successful dependency
  refresh in isolated build/test outputs so shared files from another checkout
  cannot affect the evidence.

#### Acceptance Gate

- Every current native-test target has a deterministic expected set of engine
  DLLs and external runtime files.
- Every encountered target-bearing generator expression is supported or has a
  recorded runtime-only declaration; none is silently ignored.
- The audit distinguishes linked, runtime-only, static/interface-only, and
  external-file dependencies.
- The `SplineTests` stale-RHI failure is recorded as a deployment-edge defect,
  not a Spline assertion failure or missing production build dependency.

### Stage 1: Implement deterministic runtime-closure collection

- [x] Add one reusable target-closure traversal utility rather than duplicating
  the existing forbidden-dependency walk.
- [x] Normalize aliases and supported generator-expression wrappers before
  queueing dependencies, and preserve a visited set to terminate cycles.
- [x] Classify visited targets by CMake type and collect only deployable shared
  or module binaries while continuing through static, object, interface, and
  imported metadata targets.
- [x] Collect `DURIN_RUNTIME_DEPLOY_FILES` from every visited target and merge
  explicitly registered runtime-only roots into the same traversal.
- [x] Sort and deduplicate collected targets/files before registering their
  shared deployment targets, while retaining existing destination-collision
  failures.
- [x] Emit a bounded configure diagnostic for unsupported target-bearing
  expressions, missing runtime-only targets, or targets whose deployable binary
  cannot be resolved.
- [x] Add configuration-level contract coverage for alias traversal, public and
  private links, cycles, static-to-shared traversal, imported runtime files,
  runtime-only roots, unknown expressions, and destination collisions.

#### Acceptance Gate

- A synthetic module split added below a linked root appears in the test's
  derived deployment closure without editing that test target.
- Static/interface nodes are traversed without copy commands; shared/module
  nodes and declared external runtime files each receive exactly one shared
  deployment target.
- Unsupported dependency metadata fails configuration before compilation or
  GoogleTest discovery.
- Existing single-writer and incremental-deployment contract coverage remains
  green.

### Stage 2: Finalize closures through native-test discovery and migrate targets

- [x] Invoke derived runtime-closure registration from
  `durin_discover_tests(...)` before `gtest_discover_tests(...)` creates the
  discovery command.
- [x] Add a narrowly named target property or helper for runtime-only roots and
  files, with configuration validation that rejects ordinary linked targets in
  the exception list.
- [x] Update common native-test helper functions so target authors declare link
  dependencies and genuine runtime-only exceptions, not duplicated transitive
  `DEPLOY_TARGETS` lists.
- [x] Migrate Core, CoreDObject, AssetCore, RHI, RenderCore, Vulkan, Engine, and
  editor functional targets. Remove a manual deployment declaration only after
  its derived replacement is visible in configure evidence.
- [x] Correct the `SplineTests` closure through the general mechanism, including
  RHI, RenderCore, Mona layers, ApplicationCore, and Slang runtime files reached
  through its linked roots.
- [x] Add a configure check that rejects target-owned native-test `POST_BUILD`
  runtime copies and reports manual declarations that merely duplicate the
  derived ordinary link closure.

#### Acceptance Gate

- From clean outputs, building and running `SplineTests` deploys its complete
  runtime closure and passes without building `RHICommandListTests` or another
  native-test target first.
- Representative Core-only, editor-model, viewport/render-contract, RHI, and
  Vulkan targets run directly with no DLL supplied solely by a prior test.
- A production module dependency can be split or inserted below an existing
  root without editing transitive consumer test definitions.
- Every remaining explicit runtime-only declaration has an owner and rationale.

### Stage 3: Qualify clean, incremental, and aggregate behavior

- [x] Validate representative single targets from cold registered outputs,
  including filtered direct runs and GoogleTest discovery.
- [x] Modify or rebuild one transitive runtime module and confirm every consuming
  test copies the updated binary before discovery; unchanged deployments remain
  incremental.
- [x] Run the complete native-test aggregate with the Agent Build Profile's
  normal parallel schedule and randomized scheduling, then run the direct
  lifecycle qualification required by the native-test guide.
- [x] Confirm the complete build emits one deployment command per unique
  destination and never schedules competing writers.
- [x] Update the native-test build documentation with automatic closure,
  runtime-only exception, diagnostics, and test-authoring rules. Move durable
  behavior out of this plan before completion.
- [x] Record validation evidence, close every acceptance gate, and complete the
  plan lifecycle according to `Documentation/Plans/AGENTS.md`.

#### Acceptance Gate

- Cold direct targets, incremental ABI-change coverage, aggregate CTest, random
  scheduling, and direct lifecycle qualification all pass.
- No native test depends on shared `Bin` residue or aggregate build order.
- Deployment command count equals the unique runtime destination count.
- Repository documentation contains the lasting source-of-truth and exception
  rules, and all-plan validation passes.

## Validation Matrix

| Area | Validation | Required result |
| --- | --- | --- |
| Closure traversal | Configure fixtures for aliases, public/private edges, cycles, static/interface bridges, and imported targets | Deterministic complete closure or an actionable configure failure |
| Shared ownership | Multiple tests consume the same DLL and external file | One deployment target and one writer per destination |
| Runtime files | RenderCore/Slang and asset-import/Assimp paths | Introducing target metadata deploys every required external file |
| Runtime-only exception | Unlinked plugin fixture | Explicit exception deploys it; linked targets are rejected from the exception list |
| Spline regression | Cold direct `SplineTests` build and run | Discovery succeeds and all cases pass without prior RHI test deployment |
| Editor/render stack | Cold direct Viewport or equivalent render-contract target | Complete ApplicationCore, Mona, RenderCore, RHI, and external runtime closure |
| Incremental ABI change | Rebuild one transitive DLL, then one consuming test | Updated DLL is copied before test discovery; unchanged files are not recopied |
| Aggregate | Full native-test CTest schedule and randomized repetitions | All registered entries pass with no deployment races |
| Direct lifecycle | Native-test direct qualification | Every target starts from its declared closure and shuts down cleanly |
| Documentation | All-plan validator and native-test guide review | Plan metadata valid and durable authoring rules documented |

## Definition of Done

- Native-test runtime deployment is derived from the final concrete CMake target
  graph during centralized test discovery.
- Runtime-only exceptions are explicit, rare, validated, and documented.
- Adding or splitting a transitive production runtime module does not require
  edits to unaffected consumer test definitions.
- Every native-test target runs from clean registered outputs without relying on
  another target's deployment residue.
- Shared destinations retain one writer, collision protection, and incremental
  copy behavior.
- `SplineTests` and the other previously incomplete targets pass cold direct
  validation.
- Configure contract coverage, representative direct tests, incremental ABI
  coverage, aggregate CTest, randomized scheduling, and direct lifecycle
  qualification pass.
- Lasting behavior is documented in the native-test build guide and this plan
  is marked completed with evidence.

## Deferred Follow-ups

- Generalizing runtime-closure metadata for non-test executable packaging or
  installed-engine distribution.
- Cross-platform built-binary dependency scanning as a diagnostic comparison
  against the declared CMake graph.
- Per-test private DLL directories if future plugin-version isolation makes the
  shared `Bin` model unsuitable.
- Runtime asset/plugin manifest generation beyond linked modules and declared
  external files.

## Related Documentation

- [Native C++ Tests](../../../Development/Build/NativeTests.md)
- [Build and Run](../../../Development/Build/BuildAndRun.md)
- [Native Test Process Isolation](../2026-07/NativeTestProcessIsolation.md)

## Related Code

- [`CMake/Project/ProjectOutputs.cmake`](../../../../CMake/Project/ProjectOutputs.cmake)
- [`CMake/Project/ProjectTargets.cmake`](../../../../CMake/Project/ProjectTargets.cmake)
- [`Engine/Tests/Native/EngineTests/CMakeLists.txt`](../../../../Engine/Tests/Native/EngineTests/CMakeLists.txt)
- [`Engine/Tests/Native/RenderCoreTests/CMakeLists.txt`](../../../../Engine/Tests/Native/RenderCoreTests/CMakeLists.txt)
- [`Engine/Tests/Native/VulkanRHITests/CMakeLists.txt`](../../../../Engine/Tests/Native/VulkanRHITests/CMakeLists.txt)
