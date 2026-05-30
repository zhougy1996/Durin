# Build System

This document summarizes how the Durin build is organized and where generated build metadata fits into the pipeline.

## Overview

The repository is driven by CMake at the root and delegates into:

- `Engine/CMakeLists.txt`

Most engine and editor code is built as loadable modules rather than a monolithic executable.

## Key CMake Entry Points

The most important build logic lives in:

- `CMake/Modules.cmake`

Important helpers:

- `durin_add_project(...)`
- `durin_add_module(...)`
- `durin_add_test_target(...)`

### `durin_add_project(...)`

This helper:

- invokes DurinHeaderTool to prepare generated project metadata
- imports generated project metadata from `Engine/Intermediate/Build/<Platform>/<Profile>/...`
- resolves active profile-derived values used by the rest of the build
- adds module subdirectories for the current project

Relevant tool:

- `Engine/Source/Programs/DurinHeaderTool/main.py`

### `durin_add_module(...)`

This helper:

- imports generated per-module CMake metadata
- wires reflection-generated sources and export files
- sets precompiled headers
- builds a shared or static library based on module metadata
- emits shared library outputs named like `DurinEditor-<ModuleName>`

### `durin_add_test_target(...)`

This helper is the common path for native C++ tests under:

- `Engine/Source/Programs/Tests`

It applies the shared test output layout and common compile definitions used by native tests.

## Generated Metadata

Durin relies on generated project and module metadata under:

- `Engine/Intermediate/Build/<Platform>/<Profile>/`

Common examples:

- generated project CMake metadata
- generated module CMake metadata
- reflection export files
- generated reflection source files

If a module appears incomplete from static source inspection alone, check the generated intermediate metadata before assuming the source tree is missing something.

## Module-Based Build Shape

Durin currently uses runtime-loaded modules for much of the engine/editor stack.

Examples:

- `Core`
- `RenderCore`
- `RHI`
- `VulkanRHI`
- `MainFrame`

The launcher executable loads modules dynamically at runtime through the module system rather than linking every subsystem directly into a single host binary.

## Output Naming

Shared module naming matters because runtime loading depends on it.

Current pattern:

- `DurinEditor-Core.dll`
- `DurinEditor-RenderCore.dll`
- `DurinGame-Core.dll`

The runtime module loader expects the `<ProfileName>-<ModuleName>` naming convention established by the build system.

## Related Docs

- Build and run workflow: `Documentation/Setup/BuildAndRun.md`
- Profile system: `Documentation/Architecture/Profiles.md`
- Runtime architecture: `Documentation/Architecture/RuntimeArchitecture.md`

