# Build System

This document explains where Durin's CMake entrypoints live, how generated metadata flows through the build, and where module output naming is defined.

## Overview

The repository configures from the root `CMakeLists.txt` and delegates into `Engine/CMakeLists.txt`. Most engine and editor code is built as loadable modules rather than a monolithic executable.

The main build entrypoints are:

- `CMake/DurinWorkspaceSetup.cmake`
- `CMake/DurinBuildApi.cmake`
- `CMake/Project/ProjectSetup.cmake`
- `CMake/Project/ProjectTargets.cmake`

Important helper APIs:

- `add_durin_project(...)`
- `add_durin_module(...)`
- `add_durin_test(...)`

## Generated Metadata Flow

`add_durin_project(...)` invokes DurinHeaderTool, imports generated project metadata from `Engine/Intermediate/Build[-Identifier]/<Platform>/<Profile>/...`, resolves active profile-derived values, and then adds module subdirectories for the current project.

DHT uses atomic replacement and cross-process locks scoped by build identifier, platform, profile, project, and module. Conflicting writers serialize while independent modules remain parallel. Debug and Release intentionally share configuration-independent metadata; identifier-specific workflows use independent roots.

Ninja schedules build-time DHT commands through the `durin_dht` job pool. Each
command receives an explicit parser-worker limit, and module-internal parallelism
scales with the number of headers requiring parsing: fewer than 8 uses one worker,
8-15 uses at most two, 16-31 uses at most four, and larger sets may use the
configured limit. The defaults balance large-module incremental builds with
Ninja-level module and compiler scheduling: at most two DHT commands run at once,
and each uses at most four parser workers. `DURIN_DHT_JOB_POOL_SIZE` and
`DURIN_DHT_WORKERS` are cache settings intended for measured preset or CI tuning;
worker count is constrained to 1-8.

DHT emits one INFO summary per module export/reflection command. Per-header timing,
cache, dependency-loading, and worker details are DEBUG-only. Set the
`DURIN_DHT_LOG_LEVEL` cache setting to `DEBUG` for diagnostics or `WARNING` for
Ninja-only progress unless DHT reports a problem.

Project entry scripts such as `Engine/CMake/EngineSetup.cmake` and `Sandbox/CMake/SandboxSetup.cmake` run before that helper and may perform project-specific setup such as third-party registration.

`add_durin_module(...)` imports generated per-module CMake metadata, wires reflection-generated sources and export files, applies shared PCH settings, and builds the resulting shared or static library.

Ordinary module sources under `Public` and `Private` are discovered by per-module
CMake `GLOB_RECURSE CONFIGURE_DEPENDS` rules. Adding or removing a supported C/C++
source or header therefore updates the Ninja graph during a normal build. DHT
metadata only describes module configuration, reflection inputs, and generated
outputs; it does not freeze the ordinary source list at configure time.

During configuration, CMake hashes the tracked DHT Python package together with
`requirements.txt` into `DHT.fingerprint`. Those files are configure dependencies,
and export/reflection build commands depend on the resulting fingerprint. The
fingerprint is also passed into DHT's private manifests, so a tool implementation
change invalidates both CMake's build edge and DHT's internal cache.

## Build Output Identifiers

`DURIN_BUILD_IDENTIFIER` optionally isolates binary and generated metadata outputs without changing build semantics. Binary configurations and DHT intermediate roots append the identifier. Normal builds leave it empty and use separate worktrees when workflows need concurrency.

Do not use identifiers for feature selection or runtime module naming. Presets with the same identifier, platform, and profile share one DHT path because generated metadata does not vary by Debug, Release, or Shipping.

Build identifiers are also independent of the engine release version defined in `Engine/Build/Build.version`. The release version describes product compatibility and distribution; a build identifier only isolates workflow-owned outputs.

A preset's `binaryDir` isolates CMake, object, and Ninja state only—not Durin's final outputs or DHT metadata. This is why the IDE and Agent may keep separate CMake trees but still must follow the single-writer workflow in `BuildAndRun.md`.

## Module Output Naming

Shared module naming is part of the runtime contract. Current outputs follow `<ProfileName>-<ModuleName>`, for example:

- `DurinEditor-Core.dll`
- `DurinEditor-RenderCore.dll`
- `DurinGame-Core.dll`

The runtime module loader expects that naming convention.

## Related Docs

- `Documentation/Architecture/Versioning.md`
- `Documentation/Architecture/ReflectionSystem.md`
- `Documentation/Architecture/GarbageCollection.md`
- `Documentation/Setup/BuildAndRun.md`
- `Documentation/Architecture/Profiles.md`
- `Documentation/Architecture/RuntimeArchitecture.md`
