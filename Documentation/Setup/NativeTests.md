# Native C++ Tests

This document covers the native test presets, execution flow, and the minimum pattern for adding new test targets.

## Configure And Build

Native tests are enabled only when `BUILD_TESTING=ON`. Use the dedicated test preset:

```powershell
cmake --preset Win64-Debug-DurinEditor-Tests
```

The preset above is available for human test builds. Agents use the Agent build script:

```powershell
.\BuildTool Configure
```

The Agent build profile uses the same `Win64-Debug-DurinEditor-Tests` preset and output paths. Its dedicated worktree provides isolation from other branches and IDE sessions.

Build a specific test target:

```powershell
cmake --build Build/Win64-Debug-DurinEditor-Tests --target CoreTests --parallel
```

Agents use:

```powershell
.\BuildTool Test --target CoreTests
```

Both examples use machine-appropriate parallelism without prescribing a fixed count. See `Documentation/Setup/BuildAndRun.md` for Agent job detection and local overrides.

Normal editor/game presets keep `BUILD_TESTING=OFF`.

## Run Tests

Run all discovered tests:

```powershell
ctest --test-dir Build/Win64-Debug-DurinEditor-Tests -C Debug --output-on-failure
```

Agents use the script's `Test` action to build and run a specific native test target.

Run one test group:

```powershell
ctest --test-dir Build/Win64-Debug-DurinEditor-Tests -C Debug --output-on-failure -R FJsonDocumentTests
```

Run the executable directly:

```powershell
.\Engine\Binaries\Win64\Debug\Tests\DurinEditor\Bin\CoreTests.exe
```

The equivalent Agent command is:

```powershell
.\BuildTool Test --target CoreTests
```

Run a single GoogleTest case:

```powershell
.\Engine\Binaries\Win64\Debug\Tests\DurinEditor\Bin\CoreTests.exe --gtest_filter=FJsonDocumentTests.ParseObjectFromString
```

The equivalent Agent command is:

```powershell
.\BuildTool Test --target CoreTests --filter FJsonDocumentTests.ParseObjectFromString
```

## Output Layout

- Test executables: `Engine/Binaries/<Platform>/<Config>/Tests/<Profile>/Bin/`
- Per-target data and work roots follow the same configuration directory.

Keep generated round-trip files and discovery outputs in the target work directory, not in `Bin/`.

## Add A New Test Target

Minimum steps:

1. Create a new folder under `Engine/Source/Programs/Tests`.
2. Add a `CMakeLists.txt` that uses `add_durin_test(...)`.
3. Link the target against the required engine modules and `GTest::gtest_main`.
4. Deploy required engine DLLs to `Bin/` and checked-in inputs to `Data/`.
5. Register tests with `gtest_discover_tests(...)` using `DURIN_TEST_WORK_DIR`.

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

## Important Rules

- `DURIN_TEST_DATA_DIR` and `DURIN_TEST_WORK_DIR` are provided by `add_durin_test(...)`.
- Use `durin_test_deploy_directory_to_data(...)` or `durin_test_deploy_files_to_data(...)` for checked-in test inputs.
- Test executables should remain separate programs and should not depend on editor startup or real window creation unless that is the point of the test.

## Related Docs

- `Documentation/Setup/BuildAndRun.md`
- `Documentation/Setup/ThirdPartyBootstrap.md`
