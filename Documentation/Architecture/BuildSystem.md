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

DHT writes use atomic replacement and cross-process locks rooted by build identifier, target platform, and runtime profile. Project metadata has a project lock, and each module has its own lock shared by export and reflection generation. Project preparation additionally takes every affected module lock before cleaning or regenerating module metadata. This keeps conflicting writers serialized without blocking independent module generation, allowing Ninja to schedule DHT work in parallel. A profile lock remains reserved for operations that truly mutate profile-wide indexes or cleanup state. Debug and Release intentionally share the same configuration-independent metadata, while identifier-specific workflows use independent roots and locks.

Project entry scripts such as `Engine/CMake/EngineSetup.cmake` and `SandBox/CMake/SandBoxSetup.cmake` run before that helper and may perform project-specific setup such as third-party registration.

`add_durin_module(...)` imports generated per-module CMake metadata, wires reflection-generated sources and export files, applies shared PCH settings, and builds the resulting shared or static library.

## Build Output Identifiers

`DURIN_BUILD_IDENTIFIER` optionally isolates workflow-owned binary and generated metadata outputs without changing build semantics or profile behavior. Binary outputs append the identifier to the configuration, while DHT metadata appends it to the intermediate root. Normal Agent builds do not need an identifier because they run in dedicated worktrees; the option remains available for specialized workflows that must coexist in one checkout.

Do not use identifiers for compile-time feature selection or runtime module naming. Presets using the same identifier, platform, and profile share one DHT path and lock. Build configuration is deliberately absent because current generated metadata does not vary between Debug, Release, and Shipping.

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
