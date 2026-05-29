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
- `Engine/Source/ThirdParty/assimp`

Notes:

- `slang` is treated as a downloaded prebuilt package.
- `assimp` is treated as source code cloned by `git clone`.

### Third-Party Build Trees

Dedicated third-party build trees live under:

- `Build/ThirdParty/Build/<Platform>-<Config>-<Library>`

Current example:

- `Build/ThirdParty/Build/<Platform>-Debug-assimp`
- `Build/ThirdParty/Build/<Platform>-Release-assimp`

These build trees are independent from the main project profile build trees such as:

- `Build/Win64-Debug-DurinEditor`
- `Build/Win64-Debug-DurinGame`

### Shared Install Trees

Shared install outputs live under:

- `Build/ThirdParty/Install/<Platform>/<Config>/<Library>`

Current example:

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

### Assimp

Relevant files:

- `Engine/Scripts/Bootstrap/Setup_assimp.bat`
- `Engine/CMake/ThirdParty/assimp/CMakeLists.txt`
- `Engine/CMake/ThirdParty/assimp/CMakePresets.json`

Behavior:

- Clones source into `Engine/Source/ThirdParty/assimp` if missing
- Builds with dedicated third-party presets such as `<Platform>-Debug-assimp`
- Installs into `Build/ThirdParty/Install/...`
- Main project imports the installed library via `assimp::assimp`

Use this pattern for libraries that are built from source and shared across multiple main project profiles.

## Bootstrap Conventions

Bootstrap scripts live under:

- `Engine/Scripts/Bootstrap`

Current scripts:

- `Setup_slang.bat`
- `Setup_assimp.bat`

General expectations:

- Scripts should be safe to run multiple times.
- Scripts should check for already-installed content and return quickly when possible.
- Scripts should print absolute paths in logs.
- Scripts should own third-party preparation, not profile-specific build trees.

`Setup.bat` calls:

- `Engine/Scripts/Bootstrap/Bootstrap.bat`

And `Bootstrap.bat` currently calls both:

- `Setup_slang.bat`
- `Setup_assimp.bat`

## CMake Consumption Rules

### Main Project

The main project should not directly `FetchContent` third-party libraries that are meant to be shared across profiles.

Instead:

- prepare the dependency once via bootstrap
- build/install it into `Build/ThirdParty/Install/...`
- import it from there

For `assimp`, the imported target is:

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
- `Build/ThirdParty/Build/...`
- `Build/ThirdParty/Install/...`

### Legacy cleanup

Old `FetchContent`-style `assimp` directories such as:

- `_deps/assimp-build`
- `_deps/assimp-src`
- `_deps/assimp-subbuild`

have been removed from the current workspace and should not be reintroduced for shared libraries like `assimp`.

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
- build in `Build/ThirdParty/Build/...`
- install in `Build/ThirdParty/Install/...`
- import the installed target from the main project

## Current Assumptions

- Shared third-party installs are keyed by platform and configuration, not by profile.
- `DurinEditor` and `DurinGame` should reuse the same third-party install when ABI-compatible.
- Third-party source should remain outside main project build trees.
