# Third-Party Libraries

This document summarizes the current third-party library workflow in Durin. It is intended as a reusable reference for future coding sessions.

## Goals

- Keep third-party source, build output, install output, and runtime deployment clearly separated.
- Avoid rebuilding the same library once per `DurinEditor` or `DurinGame` build tree.
- Keep `Engine/Binaries` disposable for normal cleanup.
- Make bootstrap scripts idempotent so `Setup.bat` can be re-run safely.

## Directory Layout

### Source

Third-party source or package contents live under:

- `Engine/Source/ThirdParty/<Library>`

Current examples:

- `Engine/Source/ThirdParty/slang`
- `Engine/Source/ThirdParty/glm`
- `Engine/Source/ThirdParty/googletest`
- `Engine/Source/ThirdParty/spdlog`
- `Engine/Source/ThirdParty/glfw`
- `Engine/Source/ThirdParty/rapidyaml`
- `Engine/Source/ThirdParty/assimp`

Notes:

- `slang` is treated as a downloaded prebuilt package.
- `glm` is treated as source prepared ahead of configure and consumed directly from `Engine/Source/ThirdParty/glm`.
- `googletest` is treated as source prepared ahead of configure and consumed directly from `Engine/Source/ThirdParty/googletest` when tests are enabled.
- `spdlog` is treated as source prepared ahead of configure and then built into a shared install tree.
- `assimp`, `glfw`, and `rapidyaml` are treated as source code cloned by `git clone`.

### Third-Party Build Trees

Dedicated third-party build trees live under:

- `Build/ThirdParty/Build/<Platform>-<Config>-<Library>`

Current example:

- `Build/ThirdParty/Build/<Platform>-Debug-spdlog`
- `Build/ThirdParty/Build/<Platform>-Release-spdlog`
- `Build/ThirdParty/Build/<Platform>-Debug-glfw`
- `Build/ThirdParty/Build/<Platform>-Release-glfw`
- `Build/ThirdParty/Build/<Platform>-Debug-rapidyaml`
- `Build/ThirdParty/Build/<Platform>-Release-rapidyaml`
- `Build/ThirdParty/Build/<Platform>-Debug-assimp`
- `Build/ThirdParty/Build/<Platform>-Release-assimp`

These build trees are independent from the main project profile build trees such as:

- `Build/Win64-Debug-DurinEditor`
- `Build/Win64-Debug-DurinGame`

### Shared Install Trees

Shared install outputs live under:

- `Build/ThirdParty/Install/<Platform>/<Config>/<Library>`

Current example:

- `Build/ThirdParty/Install/<Platform>/Debug/spdlog`
- `Build/ThirdParty/Install/<Platform>/Release/spdlog`
- `Build/ThirdParty/Install/<Platform>/Debug/glfw`
- `Build/ThirdParty/Install/<Platform>/Release/glfw`
- `Build/ThirdParty/Install/<Platform>/Debug/rapidyaml`
- `Build/ThirdParty/Install/<Platform>/Release/rapidyaml`
- `Build/ThirdParty/Install/<Platform>/Debug/assimp`
- `Build/ThirdParty/Install/<Platform>/Release/assimp`

This is the path the main project consumes when importing installed third-party targets.

### Runtime Deployment

Runtime DLL deployment still goes to:

- `Engine/Binaries/ThirdParty/...`

This path is for copied runtime binaries only. It is not the install prefix for shared third-party builds.

## Current Library Patterns

### Slang

Relevant bootstrap script:

- `Engine/Scripts/Bootstrap/Setup_slang.bat`

Behavior:

- Checks whether the expected files already exist under `Engine/Source/ThirdParty/slang`
- Downloads and extracts a prebuilt package only if missing
- Logs normalized absolute paths

Use this pattern for libraries that are primarily consumed as prebuilt SDKs or binary packages.

### Direct Source Libraries

Relevant files:

- `Engine/Scripts/Bootstrap/Setup_glm.bat`
- `Engine/Scripts/Bootstrap/Setup_googletest.bat`
- `Engine/CMake/ThirdParty/glm/CMakeLists.txt`
- `Engine/CMake/ThirdParty/googletest/CMakeLists.txt`

Behavior:

- `Setup_glm.bat` clones source into `Engine/Source/ThirdParty/glm`.
- `Setup_googletest.bat` clones source into `Engine/Source/ThirdParty/googletest`.
- Main project consumes `glm` directly from that prepared source directory.
- Test builds consume `googletest` directly from that prepared source directory when `BUILD_TESTING` is enabled.
- No shared install tree is needed because `glm` is header-only.

Use this pattern for header-only or otherwise lightweight source dependencies that should still be prepared ahead of configure.

### Shared Source-Build Libraries

Relevant files:

- `Engine/Scripts/Bootstrap/Setup_spdlog.bat`
- `Engine/Scripts/Bootstrap/Setup_glfw.bat`
- `Engine/Scripts/Bootstrap/Setup_rapidyaml.bat`
- `Engine/Scripts/Bootstrap/Setup_assimp.bat`
- `Engine/Scripts/Bootstrap/Setup_installed_thirdparty.bat`
- `Engine/CMake/ThirdParty/DurinThirdParty.cmake`
- `Engine/CMake/ThirdParty/spdlog/CMakeLists.txt`
- `Engine/CMake/ThirdParty/glfw/CMakeLists.txt`
- `Engine/CMake/ThirdParty/rapidyaml/CMakeLists.txt`
- `Engine/CMake/ThirdParty/assimp/CMakeLists.txt`

Behavior:

- `Setup_installed_thirdparty.bat` owns shared install flow such as config parsing, build/install directory layout, `cmake -S/-B/-DCMAKE_INSTALL_PREFIX`, and install verification.
- `Setup_spdlog.bat`, `Setup_glfw.bat`, and `Setup_rapidyaml.bat` clone source into `Engine/Source/ThirdParty/<Library>` if missing, then install from their third-party CMake wrapper directories into `Build/ThirdParty/Install/...`.
- `Setup_assimp.bat` clones source into `Engine/Source/ThirdParty/assimp` if missing, then uses the same shared install helper.
- Main project imports installed targets from the shared install tree instead of building them once per profile.
- Current imported targets include `spdlog::spdlog`, `glfw`, `ryml::ryml`, and `assimp::assimp`.

Use this pattern for libraries that are built from source and shared across multiple main project profiles.

## Bootstrap Conventions

Bootstrap scripts live under:

- `Engine/Scripts/Bootstrap`

Current scripts:

- `Setup_slang.bat`
- `Setup_glm.bat`
- `Setup_googletest.bat`
- `Setup_spdlog.bat`
- `Setup_glfw.bat`
- `Setup_rapidyaml.bat`
- `Setup_assimp.bat`
- `Setup_installed_thirdparty.bat`

General expectations:

- Scripts should be safe to run multiple times.
- Scripts should check for already-installed content and return quickly when possible.
- Scripts should print absolute paths in logs.
- Scripts should own third-party preparation, not profile-specific build trees.

`Setup.bat` calls:

- `Engine/Scripts/Bootstrap/Bootstrap.bat`

And `Bootstrap.bat` currently calls:

- `Setup_slang.bat`
- `Setup_glm.bat`
- `Setup_googletest.bat`
- `Setup_spdlog.bat`
- `Setup_glfw.bat`
- `Setup_rapidyaml.bat`
- `Setup_assimp.bat`

## CMake Consumption Rules

### Main Project

The main project should not directly `FetchContent` third-party libraries that are meant to be shared across profiles.

Instead:

- prepare the dependency once via bootstrap
- build/install it into `Build/ThirdParty/Install/...`
- import it from there

For current shared libraries, imported targets are:

- `spdlog::spdlog`
- `glfw`
- `ryml::ryml`
- `assimp::assimp`

### Imported Library Expectations

An imported library should define:

- `IMPORTED_LOCATION`
- `IMPORTED_IMPLIB`
- `INTERFACE_INCLUDE_DIRECTORIES`

And should fail configuration with a clear message if the installed dependency is missing.

## Cleanup Rules

### Safe to delete routinely

- `Engine/Binaries/...`
- profile-specific build trees such as `Build/Win64-Debug-DurinEditor`

### Keep unless intentionally rebuilding third-party installs

- `Engine/Source/ThirdParty/assimp`
- `Engine/Source/ThirdParty/glm`
- `Engine/Source/ThirdParty/googletest`
- `Engine/Source/ThirdParty/spdlog`
- `Build/ThirdParty/Build/...`
- `Build/ThirdParty/Install/...`

### Legacy cleanup

Old profile-local or `_deps`-style third-party build directories such as:

- `_deps/spdlog-build`
- `_deps/spdlog-src`
- `_deps/spdlog-subbuild`
- `_deps/glfw-build`
- `_deps/glfw-src`
- `_deps/glfw-subbuild`
- `_deps/rapidyaml-build`
- `_deps/rapidyaml-src`
- `_deps/rapidyaml-subbuild`
- `_deps/assimp-build`
- `_deps/assimp-src`
- `_deps/assimp-subbuild`

have been removed from the current workflow and should not be reintroduced for shared libraries meant to be reused across profiles.

## Recommendations for New Libraries

When adding a new third-party dependency, choose one of these patterns:

### Prebuilt package pattern

Use this for SDK-like libraries:

- place package contents in `Engine/Source/ThirdParty/<Library>`
- add a bootstrap script that downloads and extracts the package
- deploy required runtime files into `Engine/Binaries/ThirdParty/...` as needed

### Source build pattern

Use this for compile-from-source libraries:

- clone or otherwise prepare source into `Engine/Source/ThirdParty/<Library>`
- create a dedicated `Engine/CMake/ThirdParty/<Library>` entry
- wire imported-target path discovery through `Engine/CMake/ThirdParty/DurinThirdParty.cmake`
- reuse `Engine/Scripts/Bootstrap/Setup_installed_thirdparty.bat` for the common install flow
- build in `Build/ThirdParty/Build/...`
- install in `Build/ThirdParty/Install/...`
- import the installed target from the main project

Use direct source consumption instead of a shared install when the dependency is header-only and does not benefit from a separate install tree.

## Current Assumptions

- Shared third-party installs are keyed by platform and configuration, not by profile.
- `DurinEditor` and `DurinGame` should reuse the same third-party install when ABI-compatible.
- Third-party source should remain outside main project build trees.
- Shared third-party installs are driven by bootstrap scripts and direct `cmake -S/-B/-DCMAKE_INSTALL_PREFIX` commands, not by per-library CMake presets.
