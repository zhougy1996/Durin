# Native C++ Tests

This document explains how to configure, build, run, and extend Durin's native C++ test system.

## Overview

Durin's native tests are built with:

- `GoogleTest` as the test framework
- `CTest` as the test runner
- standalone test executables under `Engine/Source/Programs/Tests`

Tests are intentionally kept out of runtime/editor startup code. They are only added to the build when `BUILD_TESTING` is enabled.

Current test targets include:

- `CoreTests`
- `AssetCoreTests`
- `RenderCoreTests`

## Build Configuration

At the root CMake level:

- `include(CTest)` enables the standard CTest integration
- `BUILD_TESTING` controls whether test-only dependencies and test targets are added

When `BUILD_TESTING=OFF`:

- GoogleTest is not added to the build
- test targets are not added
- normal engine/editor builds continue to work without the test layer

Main Editor/Game presets explicitly keep `BUILD_TESTING=OFF`. Use a dedicated test preset when working on native tests.

## Configure

### Configure with tests enabled

```powershell
cmake --preset Win64-Debug-DurinEditor-Tests
```

### Configure with tests disabled

```powershell
cmake --preset Win64-Debug-DurinEditor
```

## Build Tests

Build a specific test target:

```powershell
cmake `
  --build Build/Win64-Debug-DurinEditor-Tests `
  --target CoreTests `
  -j 4
```

Native test binaries share one profile-level `Bin/` directory, while each test target keeps its own data and working directories:

- `Engine/Binaries/Win64/Debug/Tests/DurinEditor/Bin/CoreTests.exe`
- `Engine/Binaries/Win64/Debug/Tests/DurinEditor/CoreTests/Data/`
- `Engine/Binaries/Win64/Debug/Tests/DurinEditor/CoreTests/Work/`

If a test target links against engine module DLLs, copy the required runtime DLLs into the shared `Bin/`. `CoreTests` currently does this with:

```cmake
durin_test_deploy_target_binary(CoreTests Core)
```

Use `durin_test_deploy_directory_to_data(...)` or `durin_test_deploy_files_to_data(...)` for checked-in test inputs.

## Run Tests

### Run all discovered CTest tests

```powershell
ctest `
  --test-dir Build/Win64-Debug-DurinEditor-Tests `
  -C Debug `
  --output-on-failure
```

### Run one group of tests

```powershell
ctest `
  --test-dir Build/Win64-Debug-DurinEditor-Tests `
  -C Debug `
  --output-on-failure `
  -R FJsonDocumentTests
```

### Run the executable directly

```powershell
.\Engine\Binaries\Win64\Debug\Tests\DurinEditor\Bin\CoreTests.exe
```

Or run a single GoogleTest case:

```powershell
.\Engine\Binaries\Win64\Debug\Tests\DurinEditor\Bin\CoreTests.exe `
  --gtest_filter=FJsonDocumentTests.ParseObjectFromString
```

## Add a New Test Target

1. Create a new folder under `Engine/Source/Programs/Tests`, for example `AssetCoreTests`.
2. Add a `CMakeLists.txt` file for that target.
3. Use `add_durin_test(...)` instead of calling `add_executable(...)` directly.
4. Link the target against the engine modules it needs and `GTest::gtest_main`.
5. Deploy required engine DLLs into `Bin/`, and sample inputs into `Data/`.
6. Register the tests with `gtest_discover_tests(...)`, using the target's `DURIN_TEST_WORK_DIR`.

Example:

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
durin_test_deploy_directory_to_data(AssetCoreTests "${CMAKE_CURRENT_SOURCE_DIR}/TestData")

get_target_property(_durin_asset_core_tests_work_dir AssetCoreTests DURIN_TEST_WORK_DIR)

gtest_discover_tests(AssetCoreTests
    WORKING_DIRECTORY "${_durin_asset_core_tests_work_dir}"
)
```

## Add a New Test File

Recommended pattern:

- keep tests grouped by subsystem
- keep checked-in sample input files in a local `TestData` folder
- prefer deterministic tests for `Core`, parsing, asset transforms, and other non-UI logic

Example:

```cpp
#include <gtest/gtest.h>

TEST(FExampleTests, BasicExpectation)
{
    EXPECT_EQ(1 + 1, 2);
}
```

## Practical Notes

- On Windows, build and test commands should be run from the Visual Studio developer environment so standard library and SDK paths are available.
- If `cmake` is not on `PATH` on your machine, use the machine-local command documented in `AGENTS_LOCAL.md`.
- `gtest_discover_tests()` registers individual GoogleTest cases with CTest, so `ctest -R` filters match test case names, not only executable names.
- Keep the test working directory as `DURIN_TEST_WORK_DIR` so generated round-trip files and CTest discovery files stay out of `Bin/`.
- `DURIN_TEST_DATA_DIR` and `DURIN_TEST_WORK_DIR` are added as compile definitions by `add_durin_test(...)`.
- Test executables are separate programs. They should not depend on editor startup, real windows, or renderer boot unless that is explicitly the point of the test.
- The preferred first targets for native testing are low-level modules such as `Core` and `AssetCore`.

## Current Layout

- Test root: `Engine/Source/Programs/Tests`
- First suite: `Engine/Source/Programs/Tests/CoreTests`
- Shared helpers: `CMake/Project/ProjectTargets.cmake`, `CMake/Project/ProjectOutputs.cmake`
- Shared binary root: `Engine/Binaries/<Platform>/<Config>/Tests/<Profile>/Bin/`
- Per-target sandbox root: `Engine/Binaries/<Platform>/<Config>/Tests/<Profile>/<TestTarget>/`
- Test dependency source wrapper: `Engine/CMake/ThirdParty/googletest`
- Prepare test dependency source ahead of configure with: `python Engine/Scripts/Bootstrap/setup_third_party.py --libs googletest`
- Prepared test dependency source lives under: `Engine/External/Source/googletest`
