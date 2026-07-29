# Native C++ Tests

This document covers running native tests and adding test targets. Native tests are enabled by the Agent driver's `Win64-Debug-DurinEditor-Tests` preset.

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
.\DevTool.bat test --target all --schedule-random --output-junit Build\NativeTestResults.xml
.\DevTool.bat test --target all --ctest-regex "^FJsonDocumentTests.ParseObjectFromString$"
.\DevTool.bat test --target all --include-direct
```

The first command runs the target's discovered tests. The second passes a GoogleTest filter. The test executable has a 300-second timeout by default; `--timeout <seconds>` changes it, and `--timeout 0` disables it for an intentionally long diagnostic run. The timeout starts after the target has finished building.

`--target all` builds the complete preset and then runs every test registered in
that build directory through CTest, excluding qualification-only registrations.
Its timeout applies to each CTest-registered test. GoogleTest `--filter` syntax
is executable-specific and therefore cannot be combined with `--target all`.
Use `--schedule-random` to randomize the CTest launch order and
`--output-junit <path>` to retain machine-readable aggregate results. Use
`--ctest-regex <regex>` for an isolated rerun of matching CTest-registered
names. Add `--include-direct` to also run each target as one whole-executable
lifecycle test after its cases have run in isolation. These options require
`--target all`.

The default aggregate excludes `native-test-direct` because those registrations
repeat the same functional cases in one process. Run them for qualification,
process-state leakage investigations, and changes to test-target lifecycle.
`native-test-characterization` is always excluded from the aggregate and runs
only through its owning custom target. Such a dedicated target sets
`DURIN_TEST_DIRECT_LIFECYCLE FALSE` because its custom runner, rather than a
routine whole-executable smoke, owns the required environment and scheduling.

Do not record a current test or registration total in repository documentation.
CTest discovery is the source of truth; use the command summary or
`--output-junit` when a review needs an auditable count.

DurinDevTool clears build recovery state before launching the test executable. A failed assertion, crash, timeout, or interrupted test should be diagnosed and rerun with `test`; it does not require `rebuild`. Build ownership, recovery, and parallelism rules are documented in `Documentation/Development/Build/BuildAndRun.md`.

In the interactive shell, use the equivalent commands:

```text
DurinDevTool> preset Win64-Debug-DurinEditor-Tests
DurinDevTool> test --target CoreUtilityTests
DurinDevTool> test --target CoreUtilityTests --filter FJsonDocumentTests.ParseObjectFromString
DurinDevTool> test all
```

DurinDevTool rejects `test` when the selected preset does not enable `BUILD_TESTING`.

For diagnosis, the corresponding executable is under
`Engine/Binaries/Win64/Debug/Tests/DurinEditor/Bin/` and may be run directly
with normal GoogleTest arguments. Direct and filtered runs use the same
process-isolation harness as CTest. Add `--durin-keep-test-work`, or set
`DURIN_TEST_KEEP_WORK=1`, to retain a successful run's files. CTest discovery
state is in `Build/Win64-Debug-DurinEditor-Tests`.

## Output Layout

- Test executables: `Engine/Binaries/<Platform>/<Config>/Tests/<Profile>/Bin/`
- Deployed read-only inputs: `<TestRoot>/Data/`
- Per-target sandbox parent: `<TestRoot>/Work/`
- Per-process writable sandbox: `<TestRoot>/Work/Runs/run-p<PID>-<nonce>/`

Test executables and their runtime DLLs share `Bin/`. Deployment helpers create
one build target per engine DLL or external runtime file, so every destination
has one writer even when many native-test targets require it. Do not add
target-owned `POST_BUILD` copies into the shared directory.

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

durin_test_deploy_target_binary(PackageRoundTripTests AssetCore)

set_target_properties(PackageRoundTripTests PROPERTIES
    DURIN_TEST_CASE_PARALLEL_SAFE TRUE
)

durin_discover_tests(PackageRoundTripTests)
```

`add_durin_test(...)` links `NativeTestSupport`, generates the harness entry
point, and provides `DURIN_TEST_DATA_DIR` for input lookup. Use
`durin_test_deploy_directory_to_data(...)` or
`durin_test_deploy_files_to_data(...)` for checked-in inputs. Always register
through `durin_discover_tests(...)`; direct `gtest_discover_tests(...)`
boilerplate bypasses repository policy and is rejected.

Case-level parallelism is the default pattern. If a target cannot use
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

`durin_test_deploy_target_binary(...)` and
`durin_test_deploy_runtime_files(...)` register dependencies on shared
deployment targets. Repeating either declaration across test targets reuses the
same deployment target instead of scheduling another copy. Two external files
with the same destination filename are rejected during configuration unless
they resolve to the same source file.

## Related Docs

- `Documentation/Development/Build/BuildAndRun.md`
- `Documentation/Development/Build/ThirdPartyBootstrap.md`
