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

A profile describes how a project is built and hosted.

Current examples:

- `DurinEditor`
- `DurinGame`

Profile metadata is stored in:

- `Engine/Profiles/DurinEditor.dprofile`
- `Engine/Profiles/DurinGame.dprofile`

### CMake target

A CMake target is the actual compilation unit.

Examples:

- `Core`
- `Launch`
- `DurinLauncher`

## Profile Files

Example `DurinEditor.dprofile`:

```json
{
    "ProfileName": "DurinEditor",
    "WithEditor": true,
    "AppConfigName": "DurinConfig.yaml"
}
```

Example `DurinGame.dprofile`:

```json
{
    "ProfileName": "DurinGame",
    "WithEditor": false,
    "AppConfigName": "DurinConfig.yaml"
}
```

Current fields:

- `ProfileName`
- `WithEditor`
- `AppConfigName`

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

## Limitations

Profiles already control:

- naming
- build partitioning
- editor/non-editor compile-time branching

Build configuration controls developer tooling. `Debug` and `Release` game builds include developer tools; `Shipping` game builds exclude them.

## Adding A New Profile

Minimum steps:

1. Add a new `.dprofile` file under `Engine/Profiles`.
2. Add matching presets in `CMakePresets.json`.
3. Verify generated intermediate output under `Engine/Intermediate/Build/<Platform>/<ProfileName>/`.
4. Decide whether `WithEditor` should be true or false.
5. Verify launcher naming, module naming, and config naming.
