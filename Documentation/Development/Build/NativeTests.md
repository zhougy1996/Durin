# Native C++ Tests

This document covers running native tests and adding test targets. Native tests are enabled by the Agent driver's `Win64-Debug-DurinEditor-Tests` preset.

## Run Tests

Build and run a test executable through the root wrapper:

```powershell
.\BuildTool test --target CoreTests
.\BuildTool test --target CoreTests --filter FJsonDocumentTests.ParseObjectFromString
.\BuildTool test --target CoreTests --timeout 60
```

The first command runs the target's discovered tests. The second passes a GoogleTest filter. The test executable has a 300-second timeout by default; `--timeout <seconds>` changes it, and `--timeout 0` disables it for an intentionally long diagnostic run. The timeout starts after the target has finished building.

BuildTool clears build recovery state before launching the test executable. A failed assertion, crash, timeout, or interrupted test should be diagnosed and rerun with `test`; it does not require `rebuild`. Build ownership, recovery, and parallelism rules are documented in `Documentation/Development/Build/BuildAndRun.md`.

In the interactive shell, use the equivalent commands:

```text
BuildTool> preset Win64-Debug-DurinEditor-Tests
BuildTool> test CoreTests
BuildTool> test CoreTests FJsonDocumentTests.ParseObjectFromString
```

BuildTool rejects `test` when the selected preset does not enable `BUILD_TESTING`.

For diagnosis, the corresponding executable is under `Engine/Binaries/Win64/Debug/Tests/DurinEditor/Bin/` and may be run directly with normal GoogleTest arguments. CTest discovery state is in `Build/Win64-Debug-DurinEditor-Tests`.

## Output Layout

- Test executables: `Engine/Binaries/<Platform>/<Config>/Tests/<Profile>/Bin/`
- Per-target checked-in inputs: `<TestRoot>/Data/`
- Per-target generated and round-trip files: `<TestRoot>/Work/`

Do not write generated files into `Bin/` or the checked-in data directory.

## Add A Test Target

1. Create a folder under `Engine/Source/Programs/Tests`.
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

## Related Docs

- `Documentation/Development/Build/BuildAndRun.md`
- `Documentation/Development/Build/ThirdPartyBootstrap.md`
