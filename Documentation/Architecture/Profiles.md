# Profiles

This document summarizes the current `Profile` system in Durin.

For the relationship between workspace, projects, modules, and the workspace-global profile name, also read:

- `Documentation/Architecture/WorkspaceProjects.md`

## Overview

Durin separates three concepts:

- project
- profile
- CMake target

They are related, but they are not the same thing.

### Project

A project describes code and content ownership.

Current example:

- `Engine`

Project metadata is stored in:

- `Engine/Engine.dproject`

### Profile

A profile describes the workspace-wide host/runtime mode used for the build.

Current examples:

- `DurinEditor`
- `DurinGame`

Profile metadata is built into DurinHeaderTool:

- [profile_config.py](/G:/Workspace/Durin/Engine/Source/Programs/DurinHeaderTool/configs/profile_config.py)

### CMake target

A CMake target is the actual compilation unit.

Examples:

- `Core`
- `Launch`
- `DurinLauncher`

## Built-In Profiles

Durin currently defines profiles as built-in constants rather than per-project data files.

Current built-in values:

- `DurinEditor`
  - `WithEditor = true`
  - `AppConfigName = DurinEditorConfig.yaml`
- `DurinGame`
  - `WithEditor = false`
  - `AppConfigName = DurinGameConfig.yaml`

## Presets And Generated Metadata

Main project presets are defined in:

- `CMakePresets.json`

Current Win64 presets:

- `Win64-Debug-DurinEditor`
- `Win64-Release-DurinEditor`
- `Win64-Debug-DurinGame`
- `Win64-Release-DurinGame`
- `Win64-Shipping-DurinGame`

Each preset sets:

- `CMAKE_BUILD_TYPE`
- `DURIN_PROFILE_NAME`
- a dedicated build directory such as `Build/Win64-Debug-DurinEditor`

DurinHeaderTool resolves the active profile and emits generated project metadata into:

- `Engine/Intermediate/Build/<Platform>/<ProfileName>/...`

Examples:

- `Engine/Intermediate/Build/Win64/DurinEditor/...`
- `Engine/Intermediate/Build/Win64/DurinGame/...`

Relevant code:

- `Engine/Source/Programs/DurinHeaderTool/configs/profile_config.py`
- `CMake/Modules.cmake`

## Derived Build Behavior

### Module naming

Module output names use:

- `<ProfileName>-<ModuleName>`

Examples:

- `DurinEditor-Core.dll`
- `DurinGame-Core.dll`

### Launcher naming

The launcher target is still named:

- `DurinLauncher`

Its output filename matches the active profile:

- `DurinEditor.exe`
- `DurinGame.exe`

### Output layout

Profile runtime output currently goes to:

- `Engine/Binaries/<Platform>/<Config>/Runtime/<Profile>/`

Examples:

- `Engine/Binaries/Win64/Debug/Runtime/DurinEditor/`
- `Engine/Binaries/Win64/Debug/Runtime/DurinGame/`

### Compile definitions

Currently exposed semantic compile definitions include:

- `DURIN_PROFILE_NAME`
- `DURIN_APP_CONFIG_NAME`
- `DURIN_WITH_EDITOR`
- `DURIN_WITH_DEVELOPER_TOOLS`
- `DURIN_BUILD_DEBUG`
- `DURIN_BUILD_RELEASE`
- `DURIN_BUILD_SHIPPING`

`DURIN_WITH_EDITOR` is the primary semantic branch for code-level editor vs non-editor behavior.
`DURIN_WITH_DEVELOPER_TOOLS` is derived from the build configuration: editor builds and non-shipping game builds enable developer tools, while shipping game builds disable them.

In the current startup flow, `DURIN_WITH_EDITOR` also controls which concrete engine implementation `FEngineLoop::Init()` creates:

- editor builds create `DEditorEngine`
- non-editor builds create `DGameEngine`

## Runtime Rule

At runtime, module loading derives filenames from the configured profile name.

For example, `LoadModule("Core")` resolves to:

- `DurinEditor-Core.dll`
- `DurinGame-Core.dll`

Relevant code:

- `Engine/Source/Runtime/Core/Private/Modules/ModuleManager.cpp`

## Recommended Usage In Code

Prefer semantic branching:

```cpp
#if DURIN_WITH_EDITOR
// editor-only code
#endif
```

Avoid spreading direct profile-name checks unless there is a very specific low-level reason.

Use the profile name for:

- output naming
- launcher naming
- runtime module filename construction
- startup selection

Use `DURIN_WITH_EDITOR` for:

- choosing editor-only vs non-editor code paths
- deciding whether startup constructs `DEditorEngine` or `DGameEngine`

## Limitations

Profiles already control:

- naming
- build partitioning
- editor/non-editor compile-time branching

Build configuration controls developer tooling. `Debug` and `Release` game builds include developer tools; `Shipping` game builds exclude them.

Modules may declare required build features in their `.dmodule` metadata. For example, a module with:

```json
{
    "RequiredFeatures": ["DeveloperTools"]
}
```

is only added when the corresponding feature is enabled. Dependencies on disabled modules are filtered by the generated CMake metadata, so common build logic should not special-case concrete module names.

## Adding A New Profile

Minimum steps:

1. Add a new built-in profile entry in `Engine/Source/Programs/DurinHeaderTool/configs/profile_config.py`.
2. Add matching presets in `CMakePresets.json`.
3. Verify generated intermediate output under `Engine/Intermediate/Build/<Platform>/<ProfileName>/`.
4. Decide whether `WithEditor` should be true or false.
5. Verify launcher naming, module naming, and config naming.
