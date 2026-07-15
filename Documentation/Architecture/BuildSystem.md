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

`add_durin_project(...)` invokes DurinHeaderTool, imports generated project metadata from `Engine/Intermediate/Build/<Platform>/<Profile>/...`, resolves active profile-derived values, and then adds module subdirectories for the current project.

This generated metadata path is shared by every preset with the same target platform and runtime profile. A build identifier currently isolates binary outputs but not DHT project metadata, generated reflection files, or incremental manifests. Do not concurrently configure or build presets that share this path; separate CMake build trees do not make those generated writes independent.

Project entry scripts such as `Engine/CMake/EngineSetup.cmake` and `SandBox/CMake/SandBoxSetup.cmake` run before that helper and may perform project-specific setup such as third-party registration.

`add_durin_module(...)` imports generated per-module CMake metadata, wires reflection-generated sources and export files, applies shared PCH settings, and builds the resulting shared or static library.

## Build Output Identifiers

`DURIN_BUILD_IDENTIFIER` optionally isolates the complete binary output tree without changing build semantics or profile behavior. When set, the output configuration directory changes from `<Config>` to `<Config>-<Identifier>`; for example, the `Win64-Debug-DurinEditor-Agent` preset writes to `Engine/Binaries/Win64/Debug-Agent/` while remaining a normal Debug `DurinEditor` build with tests enabled.

Build identifiers affect binary output paths only. They do not currently affect `Engine/Intermediate/Build/<Platform>/<Profile>/`, so they do not permit concurrent presets that share a DHT metadata path. Do not use identifiers for compile-time feature selection or runtime module naming.

## Module Output Naming

Shared module naming is part of the runtime contract. Current outputs follow `<ProfileName>-<ModuleName>`, for example:

- `DurinEditor-Core.dll`
- `DurinEditor-RenderCore.dll`
- `DurinGame-Core.dll`

The runtime module loader expects that naming convention.

## Related Docs

- `Documentation/Architecture/ReflectionRoadmap.md`
- `Documentation/Setup/BuildAndRun.md`
- `Documentation/Architecture/Profiles.md`
- `Documentation/Architecture/RuntimeArchitecture.md`
