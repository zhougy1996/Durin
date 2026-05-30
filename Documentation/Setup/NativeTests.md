# Native C++ Tests

This document explains how to configure, build, run, and extend Durin's native C++ test system.

## Overview

Durin's native tests are built with:

- `GoogleTest` as the test framework
- `CTest` as the test runner
- standalone test executables under `Engine/Source/Programs/Tests`

Tests are intentionally kept out of runtime/editor startup code. They are only added to the build when `BUILD_TESTING` is enabled.

The first test target is:

- `CoreTests`

Current JSON coverage lives in:

- `Engine/Source/Programs/Tests/CoreTests/Private/JsonTests.cpp`

## Build Configuration

At the root CMake level:

- `include(CTest)` enables the standard CTest integration
- `BUILD_TESTING` controls whether test-only dependencies and test targets are added

When `BUILD_TESTING=OFF`:

- GoogleTest is not added to the build
- test targets are not added
- normal engine/editor builds continue to work without the test layer

## Configure

### Configure with tests enabled

```powershell
cmake `
  -S . `
  -B Build/Win64-Debug-Tests `
  -G Ninja `
  -DCMAKE_BUILD_TYPE=Debug `
  -DBUILD_TESTING=ON
```

### Configure with tests disabled

```powershell
cmake `
  -S . `
  -B Build/Win64-Debug-NoTests `
  -G Ninja `
  -DCMAKE_BUILD_TYPE=Debug `
  -DBUILD_TESTING=OFF
```

## Build Tests

Build a specific test target:

```powershell
cmake `
  --build Build/Win64-Debug-Tests `
  --target CoreTests `
  -j 4
```

The test executable is emitted into the dedicated test output directory:

- `Engine/Binaries/Win64/Debug/Tests/CoreTests.exe`

If a test target links against engine module DLLs, copy the required runtime DLLs beside the test executable. `CoreTests` currently does this with:

```cmake
durin_copy_target_binary_to_output_dir(CoreTests Core)
```

## Run Tests

### Run all discovered CTest tests

```powershell
ctest `
  --test-dir Build/Win64-Debug-Tests `
  -C Debug `
  --output-on-failure
```

### Run one group of tests

```powershell
ctest `
  --test-dir Build/Win64-Debug-Tests `
  -C Debug `
  --output-on-failure `
  -R FJsonDocumentTests
```

### Run the executable directly

```powershell
.\Engine\Binaries\Win64\Debug\Tests\CoreTests.exe
```

Or run a single GoogleTest case:

```powershell
.\Engine\Binaries\Win64\Debug\Tests\CoreTests.exe `
  --gtest_filter=FJsonDocumentTests.ParseObjectFromString
```

## Add a New Test Target

1. Create a new folder under `Engine/Source/Programs/Tests`, for example `AssetCoreTests`.
2. Add a `CMakeLists.txt` file for that target.
3. Use `durin_add_test_target(...)` instead of calling `add_executable(...)` directly.
4. Link the target against the engine modules it needs and `GTest::gtest_main`.
5. Copy any required engine DLLs beside the test executable.
6. Register the tests with `gtest_discover_tests(...)`.

Example:

```cmake
include(GoogleTest)

durin_add_test_target(AssetCoreTests
    Private/AssetImportTests.cpp
)

target_link_libraries(AssetCoreTests PRIVATE
    AssetCore
    GTest::gtest_main
)

durin_copy_target_binary_to_output_dir(AssetCoreTests AssetCore)

gtest_discover_tests(AssetCoreTests
    WORKING_DIRECTORY $<TARGET_FILE_DIR:AssetCoreTests>
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
- If `cmake` is not on `PATH` on your machine, use the machine-local command documented in `LOCAL_ENV.md`.
- `gtest_discover_tests()` registers individual GoogleTest cases with CTest, so `ctest -R` filters match test case names, not only executable names.
- Keep the test working directory as `$<TARGET_FILE_DIR:...>` so `gtest_discover_tests()` runs against the deployed test output, not the build-tree executable path.
- Test executables are separate programs. They should not depend on editor startup, real windows, or renderer boot unless that is explicitly the point of the test.
- The preferred first targets for native testing are low-level modules such as `Core` and `AssetCore`.

## Current Layout

- Test root: `Engine/Source/Programs/Tests`
- First suite: `Engine/Source/Programs/Tests/CoreTests`
- Shared helper: `CMake/Modules.cmake`
- Test dependency source wrapper: `Engine/CMake/ThirdParty/googletest`
- Prepare test dependency source ahead of configure with: `python Engine/Scripts/Bootstrap/setup_third_party.py --libs googletest`
- Prepared test dependency source lives under: `Engine/External/Source/googletest`
