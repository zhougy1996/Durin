# Profiles

This document explains what a Durin profile is, how presets map onto profiles, which compile definitions they expose, and what must be updated when adding a new profile.

For workspace, project, module, and profile boundaries, also read `Documentation/Workspace/WorkspaceProjects.md`.

## Overview

Durin separates three concepts:

- project
- profile
- CMake target

A profile is the workspace-wide host or runtime mode used for the current build. Current built-in profiles are:

- `DurinEditor`
- `DurinGame`

Profile definitions live in `Engine/Source/Programs/DurinHeaderTool/durin_header_tool/config/profile_config.py`.

## Presets And Generated Metadata

`CMakePresets.json` is the source of truth. Each main preset selects:

- `CMAKE_BUILD_TYPE`
- `DURIN_PROFILE_NAME`
- a dedicated CMake build directory

Testing and PCH options are preset behavior, not profile semantics. Multiple presets can map to the same profile and final-output directories; their operational roles are documented in `Documentation/Development/Build/BuildAndRun.md`.

`DURIN_BUILD_IDENTIFIER` is optional workflow isolation, not a profile or build configuration. Normal builds leave it empty. DurinHeaderTool emits configuration-independent metadata under `Engine/Intermediate/Build[-Identifier]/<Platform>/<ProfileName>/`; identifier and locking details belong to `Documentation/Development/Build/BuildSystem.md`.

Debug and Release presets for the same identifier and profile intentionally share this metadata.

## Derived Build Behavior

Profiles determine:

- module output naming such as `DurinEditor-Core.dll`
- launcher output naming such as `DurinEditor.exe`
- runtime output directories such as `Engine/Binaries/<Platform>/<Config>/Runtime/<Profile>/`
- semantic compile definitions including `DURIN_WITH_EDITOR`

Prefer `DURIN_WITH_EDITOR` for code-level branching. Use the profile name for output naming and runtime module filename construction.

## Module Selection

Each `.dproject` file defines `BaseModules` plus optional per-profile `ExtraModules.<ProfileName>.Modules`.

DurinHeaderTool treats those entries as the root module set for the active profile and resolves transitive dependencies from there.

## Adding A New Profile

Minimum steps:

1. Add the built-in profile entry in `Engine/Source/Programs/DurinHeaderTool/durin_header_tool/config/profile_config.py`.
2. Add or update matching `ExtraModules.<ProfileName>.Modules` entries in the relevant `.dproject` files when the module set should differ.
3. Add matching presets in `CMakePresets.json`.
4. Verify generated output under `Engine/Intermediate/Build[-Identifier]/<Platform>/<ProfileName>/`.
5. Decide the `WithEditor` value and verify launcher naming, module naming, and config file naming.
