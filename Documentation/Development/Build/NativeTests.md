# Native C++ Tests

This document covers running native tests and adding test targets. Native tests
are available from every registered build preset; the default is
`Win64-Debug-DurinEditor`.

## Source Layout

- Native test targets: `Engine/Tests/Native/<TargetName>/`
- Target-owned checked-in inputs: `Engine/Tests/Native/<TargetName>/Data/`
- Checked-in inputs shared by multiple targets: `Engine/Tests/Data/<FixtureName>/`

Keep target-owned inputs beside the target that owns them. Promote an input to
the shared data directory only when multiple test targets depend on the same
fixture.

## Run Tests

Build and run a test executable through the root wrapper:

```powershell
.\DevTool.bat test --target CoreUtilityTests
.\DevTool.bat test --target CoreUtilityTests --filter FJsonDocumentTests.ParseObjectFromString
.\DevTool.bat test --target CoreUtilityTests --timeout 60
.\DevTool.bat test --target all
.\DevTool.bat test --target all --granularity hybrid
.\DevTool.bat test --target all --schedule-random --output-junit Build\NativeTestResults.xml
.\DevTool.bat test --target all --granularity case --ctest-regex "^FJsonDocumentTests.ParseObjectFromString$"
```

The first command runs the target's discovered tests. The second passes a GoogleTest filter. The test executable has a 300-second timeout by default; `--timeout <seconds>` changes it, and `--timeout 0` disables it for an intentionally long diagnostic run. The timeout starts after the target has finished building.

`--target all` builds the `DurinNativeTests` aggregate and then runs every
ordinary target once through CTest. This `target` granularity is the default.
Use `--granularity case` to run every discovered GoogleTest case in a separate
process for isolation diagnosis and independence qualification. Do not run
unfiltered `--target all --granularity case`; diagnose an ordinary aggregate
failure with `--target <failed-target> --filter <suite.case>`, or use a narrow
case-name `--ctest-regex` when CTest-level isolation is required. After
diagnosis, use default target granularity for the final full-suite validation
unless the change specifically targets case isolation. `hybrid` is a
transition-compatible spelling which currently selects the same registrations
as `target` because every ordinary target has migrated. Native-test executables and GoogleTest are excluded from CMake's
default `all` target, so routine `build` and `rebuild` commands do not compile
tests even when the selected preset enables `BUILD_TESTING`.
Its timeout applies to each CTest-registered test. GoogleTest `--filter` syntax
is executable-specific and therefore cannot be combined with `--target all`.
Use `--schedule-random` to randomize the CTest launch order and
`--output-junit <path>` to retain machine-readable aggregate results. In target
and hybrid modes the command also prints and forwards a GoogleTest shuffle seed
so order failures can be reproduced with `GTEST_RANDOM_SEED`. Use
`--ctest-regex <regex>` only with case granularity for an isolated rerun of a
matching case registration. `--include-direct` remains an accepted no-op
compatibility alias in target mode and never duplicates a process. These
options require `--target all`.

Use a focused `--target <Target> --filter <GoogleTestFilter>` command for the
fastest failing-case iteration. It launches one target process with the filter;
aggregate granularity does not change focused execution.

`native-test-characterization` is always excluded from the aggregate and runs
only through its owning custom target. Such a dedicated target sets
`DURIN_TEST_DIRECT_LIFECYCLE FALSE` because its custom runner, rather than a
routine whole-executable smoke, owns the required environment and scheduling.

Do not record a current test or registration total in repository documentation.
CTest discovery is the source of truth; use the command summary or
`--output-junit` when a review needs an auditable count.

DurinDevTool clears build recovery state before launching the test executable. A failed assertion, crash, timeout, or interrupted test should be diagnosed and rerun with `test`; it does not require `rebuild`. Build ownership, recovery, and parallelism rules are documented in `Documentation/Development/Build/BuildAndRun.md`.

`NativeCrashCharacterizationTests` is the separately isolated native-crash
target. Its parent process launches one runtime child per intentional fault,
waits no more than 15 seconds, and validates the native exit status plus the
context, dump, and completion marker before the retained sandbox can be
cleaned. The target carries a runtime-stack rationale because a native fault
cannot be characterized safely inside the GoogleTest process. Current
supported fixtures cover read, write, and execute access violations,
`std::terminate`, and a worker-thread access violation. Stack overflow and
simultaneous/recursive faults remain best-effort and stack overflow remains
deferred as documented in
[Native Crash Diagnostics](../../Runtime/Core/NativeCrashDiagnostics.md).

In the interactive shell, use the equivalent commands:

```text
DurinDevTool> preset Win64-Debug-DurinEditor
DurinDevTool> test --target CoreUtilityTests
DurinDevTool> test --target CoreUtilityTests --filter FJsonDocumentTests.ParseObjectFromString
DurinDevTool> test all
```

DurinDevTool rejects `test` for an IDE-only or custom preset that does not
enable `BUILD_TESTING`.

For diagnosis, the corresponding executable is under
`Engine/Binaries/Win64/Debug/Tests/DurinEditor/Bin/` and may be run directly
with normal GoogleTest arguments. Direct and filtered runs use the same
process-isolation harness as CTest. Add `--durin-keep-test-work`, or set
`DURIN_TEST_KEEP_WORK=1`, to retain a successful run's files. CTest discovery
state is in `Build/Win64-Debug-DurinEditor`.

## Output Layout

- Test executables: `Engine/Binaries/<Platform>/<Config>/Tests/<Profile>/Bin/`
- Deployed read-only inputs: `<TestRoot>/Data/`
- Per-target sandbox parent: `<TestRoot>/Work/`
- Per-process writable sandbox: `<TestRoot>/Work/Runs/run-p<PID>-<nonce>/`

Test executables and their runtime DLLs share `Bin/`. Deployment helpers create
one build target per engine DLL or external runtime file, so every destination
has one writer even when many native-test targets require it. Do not add
target-owned `POST_BUILD` copies into the shared directory.

`durin_discover_tests(...)` derives each test's deployable runtime closure from
the final `LINK_LIBRARIES` and `INTERFACE_LINK_LIBRARIES` target graph before it
registers GoogleTest discovery. Shared and module libraries contribute their
binaries; static, object, and interface targets are traversed without a copy.
Imported targets contribute files published through
`DURIN_RUNTIME_DEPLOY_FILES`, so the target which introduces Assimp, Slang, or
another external runtime owns its deployment metadata. Test declarations do
not repeat ordinary transitive DLL or external-file lists.

`Data` is input-only. Never create, update, rename, or delete files there.
`Work` is only a parent managed by the test harness; tests must not write
directly beneath it. Every process receives a canonical unique directory under
`Work/Runs`, returned by `Durin::Testing::GetTestWorkDirectory()`. Logical
fixture names may repeat across processes because they resolve below different
sandboxes.

Successful runs delete their sandbox. Failed runs and runs using
`--durin-keep-test-work` print and retain it for diagnosis. The harness
periodically removes successful directories left behind by cleanup failures
after 24 hours, but skips the current process and every PID that may still be
alive. Crash directories and intentionally retained directories have no
success marker and are never removed by this periodic cleanup.

GoogleTest death-test children allocate a unique nested directory below their
parent run. A successful parent removes those child directories; a failed
parent retains the complete tree with the ordinary diagnostic sandbox.

Create a clean named fixture and perform recursive cleanup through the support
library:

```cpp
const std::filesystem::path Root =
    Durin::Testing::CreateTestFixtureDirectory("PackageRoundTrip");

// Use Root for every generated file.

Durin::Testing::RemoveTestWorkDirectory(Root);
```

`CreateTestFixtureDirectory` rejects absolute paths and traversal outside the
current process sandbox. `RemoveTestWorkDirectory` rejects the sandbox root,
checked-in `Data`, shared `Work`, and every other outside path. Do not call
`std::filesystem::remove_all` in native-test code; repository configuration
checks enforce use of the contained cleanup API.

## Add A Test Target

Create a feature/lifecycle target, not a target that mirrors a production
module. A target should own suites that share a coherent setup and dependency
stack. Every `.cpp` and every GoogleTest suite has exactly one target owner;
configuration rejects unowned sources, duplicate source or suite registration,
and the retired module-wide catch-all target names.

Minimal pattern:

```cmake
include(GoogleTest)

add_durin_test(PackageRoundTripTests
    Private/AssetImportTests.cpp
)

target_link_libraries(PackageRoundTripTests PRIVATE
    AssetCore
)

set_target_properties(PackageRoundTripTests PROPERTIES
    DURIN_TEST_CASE_PARALLEL_SAFE TRUE
)

durin_set_native_test_target_execution(PackageRoundTripTests)
durin_discover_tests(PackageRoundTripTests)
```

Complete every target link declaration before calling
`durin_discover_tests(...)`. The discovery call is the runtime-closure
finalization point as well as the test-policy registration point. Configuration
fails when a target-bearing link expression cannot be resolved for the active
preset, a referenced runtime target is missing, two external files claim the
same destination name from different sources, or a manual deployment repeats
an ordinary derived dependency.

Register a test target only in configurations that provide every capability it
exercises. In particular, targets that link editor-only modules or consume
editor-only offline build services must be guarded by `DURIN_WITH_EDITOR`.
When a mixed-capability integration target remains useful in runtime-only
builds, keep its runtime cases registered and explicitly skip only the case
whose documented prerequisite is unavailable.

An owning test helper may expose an `EDITOR_ONLY` option when the prerequisite
is not represented by an editor-module link edge, such as source decoding or
offline cooking. The option must still register the target's exact source list
as a configuration exclusion in runtime-only builds.

The source-ownership validator still covers configuration-excluded `.cpp`
files. In the unavailable branch, register each exact source through
`durin_exclude_native_test_sources(RATIONALE ... SOURCES ...)`; directory or
pattern exclusions are unsupported, and the rationale must name the missing
configuration capability.

Use a runtime-only exception only for a plugin, delay-loaded module, or file
which is selected without a CMake link edge. Declare the owner and rationale
before discovery:

```cmake
durin_test_register_runtime_only_dependencies(RendererIntegrationTests
    RATIONALE "RHIInit selects the Vulkan backend dynamically at runtime."
    TARGETS VulkanRHI
)
```

The helper also accepts `FILES` for a genuinely unlinked runtime file. It
rejects a target already present in the ordinary link closure; add or correct
the link edge instead. Repository policy rejects target-owned `POST_BUILD`
runtime copies. The lower-level explicit deployment helpers remain build-system
primitives, not native-test authoring requirements.

`add_durin_test(...)` links `NativeTestSupport`, generates the harness entry
point, and provides `DURIN_TEST_DATA_DIR` for input lookup. Use
`durin_test_deploy_directory_to_data(...)` or
`durin_test_deploy_files_to_data(...)` for checked-in inputs. Always register
through `durin_discover_tests(...)`; direct `gtest_discover_tests(...)`
boilerplate bypasses repository policy and is rejected.

Every ordinary target uses `durin_set_native_test_target_execution(...)` and
must reset mutable process-global state between tests and release owned
resources at its target boundary. If suites cannot safely share those
lifecycles, split them into cohesive targets instead of selecting routine case
execution. Checked-in data is read-only; generated assets, caches, databases,
and external-tool outputs must resolve below the current process sandbox.
Intentional multi-process, crash, and abrupt-exit characterization belongs in
a separately registered characterization target with a concrete rationale.

Case-level parallel safety remains required for the explicit case diagnostic
mode. If a target cannot use
`DURIN_TEST_CASE_PARALLEL_SAFE TRUE`, set
`DURIN_TEST_TARGET_LOCK_RATIONALE` to a concrete reviewed reason. Broad
`durin-test-target-*` locks without that rationale are rejected. Explicit
shared resources must come from the central registry in
`CMake/Project/ProjectTargets.cmake`:

- `durin-gpu`: exclusive ownership of the physical GPU/device lifecycle.
- legacy group `renderer-runtime`: temporary serialization for the
  process-global renderer lifecycle, emitted as
  `durin-test-legacy-renderer-runtime`.

Use `DURIN_TEST_RESOURCE_LOCKS durin-gpu` only when a target owns that physical
resource. Do not invent lock names. Targets that directly link `Renderer`,
`VulkanRHI`, `DurinEd`, `Mona`, or `MonaImGui` must also provide a
`DURIN_TEST_HEAVY_RUNTIME_RATIONALE`; this keeps feature targets narrow and
makes their dependency cost reviewable.

Tests should avoid editor startup or real window creation unless that behavior
is under test. When it is, keep the target scoped to that lifecycle and declare
the corresponding rationale and resource ownership explicitly.

## Qualified Parallel Baseline

The `windows-msvc-x64` Agent Build Profile qualified the native suite at
14 jobs on 2026-07-28. Three consecutive randomized aggregates completed in
17.85, 17.98, and 18.16 seconds. The same suite took 74.30 seconds at one job
and 39.74 seconds at two jobs. Whole-target direct lifecycle qualification also
passed.

The remaining aggregate critical path is explicit resource ownership:
`MaterialTests`, `VulkanRHIIntegrationTests`,
`SkyBoxVulkanIntegrationTests`, and `TextureCookIntegrationTests` use
`durin-gpu`; the renderer-backed owners also use
`durin-test-legacy-renderer-runtime`. In the final measured schedule these
locks covered 50 CTest entries and 17.24 process-seconds. Do not relax them
without separating the physical-device or renderer lifecycle they protect.

Incremental `all` dependency checks took 0.88-0.96 seconds in the qualification
matrix. Whole-target startup and multi-case execution accounted for 34.36
process-seconds in that historical run; the slowest direct entries were
`CoreUtilityTests` (5.42 seconds), `AssetPackageTests` (4.54 seconds), and
`TextureTests` (4.14 seconds).

Derived closures register dependencies on shared deployment targets. Multiple
tests consuming the same module or external file therefore reuse one writer;
unchanged deployments remain incremental. Two external files with the same
destination filename are rejected during configuration unless they resolve to
the same source file.

## Related Docs

- `Documentation/Development/Build/BuildAndRun.md`
- `Documentation/Development/Build/ThirdPartyBootstrap.md`
