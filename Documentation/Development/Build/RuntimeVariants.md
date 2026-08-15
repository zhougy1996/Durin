# Runtime Variants

This document explains runtime variants, how presets select them, which compile
definitions they expose, and what must be updated when adding one.

For workspace, project, module, and runtime-variant boundaries, also read
`Documentation/Workspace/WorkspaceProjects.md`.

## Overview

Durin separates three concepts:

- project
- runtime variant
- CMake target

A runtime variant is the workspace-wide host mode used for the current build.
Current built-in runtime variants are:

- `DurinEditor`
- `DurinGame`

Runtime-variant definitions live in
`Engine/Source/Programs/DurinHeaderTool/durin_header_tool/config/runtime_variant_config.py`.

## Presets And Generated Metadata

`CMakePresets.json` is the source of truth. Each main preset selects:

- `CMAKE_BUILD_TYPE`
- `DURIN_RUNTIME_VARIANT`
- a dedicated CMake build directory

Testing and PCH options are preset behavior, not runtime-variant semantics.
Every registered build preset enables native-test configuration, and tests are
built on demand rather than by the default `all` target. Multiple presets can
map to the same runtime variant and final-output directories; their operational
roles are documented in `Documentation/Development/Build/BuildAndRun.md`.

Profiling is also preset behavior rather than a runtime variant. The dedicated
Release profiling presets keep `DurinEditor` or `DurinGame` as the runtime
variant and use `DURIN_PRESET_ROLE=Profiling` for output isolation. See
`Documentation/Development/Build/Profiling.md`.

DurinHeaderTool emits configuration-independent metadata under
`Engine/Intermediate/Build/<Platform>/<RuntimeVariant>/`. Debug, Release, and
Profiling presets for the same runtime variant intentionally share this
metadata within a worktree, including each module's persistent `DHTState`.
Phase-state records still validate their platform, runtime variant, DHT and
native-libclang fingerprints, schema, parser/generator context, and semantic
inputs before reuse. Preset sharing therefore reuses only compatible entries,
and all presets remain subject to the checkout/module single-writer locks.

## Derived Build Behavior

Runtime variants determine:

- module output naming such as `DurinEditor-Core.dll`
- launcher output naming such as `DurinEditor.exe`
- runtime output directories such as
  `Engine/Binaries/<Platform>/<Config>/Runtime/<RuntimeVariant>/`
- semantic compile definitions including `DURIN_WITH_EDITOR`

Prefer `DURIN_WITH_EDITOR` for code-level branching. Use the runtime variant for
output naming and runtime module filename construction.

## Module Selection

Each `.dproject` file defines `BaseModules` plus optional per-runtime-variant
`ExtraModules.<RuntimeVariant>.Modules`.

DurinHeaderTool treats those entries as the root module set for the active
runtime variant and resolves transitive dependencies from there.

The `developer` module-creation kind is not a runtime variant. It defaults a
new module to the `DurinEditor` root and `Source/Developer`, while the project
descriptor roots and dependency closure still decide whether that module is
present. Developer modules required by headless programs are selected
explicitly by those programs; `DurinGame` does not gain a Developer branch.
The Engine editor currently selects `AssetBuildCore`, `TextureBuild`, and
`GeometryBuild`; package audit and canonical-resave tools select none and use
ordinary AssetCore loading for current-format content.

## Adding A New Runtime Variant

Minimum steps:

1. Add the built-in runtime-variant entry in
   `Engine/Source/Programs/DurinHeaderTool/durin_header_tool/config/runtime_variant_config.py`.
2. Add or update matching `ExtraModules.<RuntimeVariant>.Modules` entries in the
   relevant `.dproject` files when the module set should differ.
3. Add matching presets in `CMakePresets.json`.
4. Verify generated output under
   `Engine/Intermediate/Build/<Platform>/<RuntimeVariant>/`.
5. Decide the `WithEditor` value and verify launcher naming, module naming, and config file naming.
