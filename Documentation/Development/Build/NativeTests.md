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
.\DevTool.bat test --target CoreTests
.\DevTool.bat test --target CoreTests --filter FJsonDocumentTests.ParseObjectFromString
.\DevTool.bat test --target CoreTests --timeout 60
.\DevTool.bat test --target all
```

The first command runs the target's discovered tests. The second passes a GoogleTest filter. The test executable has a 300-second timeout by default; `--timeout <seconds>` changes it, and `--timeout 0` disables it for an intentionally long diagnostic run. The timeout starts after the target has finished building.

`--target all` builds the complete preset and then runs every test registered in
that build directory through CTest. Its timeout applies to each CTest-registered
test. GoogleTest `--filter` syntax is executable-specific and therefore cannot
be combined with `--target all`.

DurinDevTool clears build recovery state before launching the test executable. A failed assertion, crash, timeout, or interrupted test should be diagnosed and rerun with `test`; it does not require `rebuild`. Build ownership, recovery, and parallelism rules are documented in `Documentation/Development/Build/BuildAndRun.md`.

In the interactive shell, use the equivalent commands:

```text
DurinDevTool> preset Win64-Debug-DurinEditor-Tests
DurinDevTool> test --target CoreTests
DurinDevTool> test --target CoreTests --filter FJsonDocumentTests.ParseObjectFromString
DurinDevTool> test all
```

DurinDevTool rejects `test` when the selected preset does not enable `BUILD_TESTING`.

For diagnosis, the corresponding executable is under `Engine/Binaries/Win64/Debug/Tests/DurinEditor/Bin/` and may be run directly with normal GoogleTest arguments. CTest discovery state is in `Build/Win64-Debug-DurinEditor-Tests`.

## Output Layout

- Test executables: `Engine/Binaries/<Platform>/<Config>/Tests/<Profile>/Bin/`
- Per-target checked-in inputs: `<TestRoot>/Data/`
- Per-target generated and round-trip files: `<TestRoot>/Work/`

Test executables and their runtime DLLs share `Bin/`. Deployment helpers create
one build target per engine DLL or external runtime file, so every destination
has one writer even when many native-test targets require it. Do not add
target-owned `POST_BUILD` copies into the shared directory.

Do not write generated test files into `Bin/` or the checked-in data directory.

## Add A Test Target

1. Create a folder under `Engine/Tests/Native`.
2. Add a `CMakeLists.txt` using `add_durin_test(...)`.
3. Link the required engine modules and `GTest::gtest_main`.
4. Deploy required engine DLLs and checked-in input data.
5. Register discovery with `gtest_discover_tests(...)` and `DURIN_TEST_WORK_DIR`.

Minimal pattern:

```cmake
include(GoogleTest)

add_durin_test(AssetCoreTests
    Private/AssetImportTests.cpp
)

target_link_libraries(AssetCoreTests PRIVATE
    AssetCore
    GTest::gtest_main
)

durin_test_deploy_target_binary(AssetCoreTests AssetCore)

get_target_property(_work_dir AssetCoreTests DURIN_TEST_WORK_DIR)

gtest_discover_tests(AssetCoreTests
    WORKING_DIRECTORY "${_work_dir}"
)
```

`add_durin_test(...)` provides `DURIN_TEST_DATA_DIR` and `DURIN_TEST_WORK_DIR`. Use `durin_test_deploy_directory_to_data(...)` or `durin_test_deploy_files_to_data(...)` for checked-in inputs. Tests should remain independent executables and avoid editor startup or real window creation unless that behavior is under test.

`durin_test_deploy_target_binary(...)` and
`durin_test_deploy_runtime_files(...)` register dependencies on shared
deployment targets. Repeating either declaration across test targets reuses the
same deployment target instead of scheduling another copy. Two external files
with the same destination filename are rejected during configuration unless
they resolve to the same source file.

## Related Docs

- `Documentation/Development/Build/BuildAndRun.md`
- `Documentation/Development/Build/ThirdPartyBootstrap.md`
