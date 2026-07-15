# Profiles

This document explains what a Durin profile is, how presets map onto profiles, which compile definitions they expose, and what must be updated when adding a new profile.

For workspace, project, module, and profile boundaries, also read `Documentation/Architecture/WorkspaceProjects.md`.

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

Main presets are defined in `CMakePresets.json`. Common Win64 presets include:

- `Win64-Debug-DurinEditor`
- `Win64-Release-DurinEditor`
- `Win64-Debug-DurinGame`
- `Win64-Release-DurinGame`
- `Win64-Shipping-DurinGame`

Each preset sets:

- `CMAKE_BUILD_TYPE`
- `DURIN_PROFILE_NAME`
- a dedicated build directory such as `Build/Win64-Debug-DurinEditor`

Presets may also set `DURIN_BUILD_IDENTIFIER` to isolate outputs produced by a particular build workflow. This is not a profile or build configuration: `Win64-Debug-DurinEditor-Agent` remains `Debug` + `DurinEditor`, but writes to `Engine/Binaries/Win64/Debug-Agent/`.

DurinHeaderTool resolves the active profile and emits generated project metadata under `Engine/Intermediate/Build/<Platform>/<ProfileName>/...`.

That path does not currently include the preset name or `DURIN_BUILD_IDENTIFIER`. Presets such as human Debug, Tests, and Agent `DurinEditor` builds therefore share DHT-generated CMake files, reflection outputs, and incremental manifests even though their CMake build trees and binary outputs differ. Do not configure or build presets sharing the same platform and profile concurrently.

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
4. Verify generated output under `Engine/Intermediate/Build/<Platform>/<ProfileName>/`.
5. Decide the `WithEditor` value and verify launcher naming, module naming, and config file naming.
