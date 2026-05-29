# Profiles

This document summarizes the current `Profile` system in Durin. It is intended as a reusable reference for future coding sessions.

## Why Profiles Exist

Durin separates three concepts:

- project
- profile
- CMake target

They are not the same thing.

### Project

A project describes code and content ownership.

Current example:

- `Engine`

Project metadata is stored in:

- `Engine/Engine.dproject`

### Profile

A profile describes how a project is built and run.

Current examples:

- `DurinEditor`
- `DurinGame`

Profile metadata is stored in:

- `Engine/Profiles/DurinEditor.dprofile`
- `Engine/Profiles/DurinGame.dprofile`

### CMake target

A CMake target is the actual compilation unit such as:

- `Core`
- `Launch`
- `DurinLauncher`

## Current Profile Files

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

## Current Build Presets

Main project presets are defined in:

- `CMakePresets.json`

Current Win64 presets:

- `<Platform>-Debug-DurinEditor`
- `<Platform>-Release-DurinEditor`
- `<Platform>-Debug-DurinGame`
- `<Platform>-Release-DurinGame`

Each preset sets:

- `CMAKE_BUILD_TYPE`
- `DURIN_PROFILE_NAME`
- a dedicated build directory such as `Build/<Platform>-Debug-DurinEditor`

## How Profile Data Flows

### 1. CMake preset selects the active profile

Example:

- `DURIN_PROFILE_NAME=DurinEditor`

### 2. DHT loads project + profile metadata

Relevant code:

- `Engine/Source/Programs/DurinHeaderTool/configs/profile_config.py`

Profile lookup rule:

- `<ProjectDir>/Profiles/<ProfileName>.dprofile`

### 3. Generated project metadata is emitted into the project intermediate directory

Current pattern:

- `Engine/Intermediate/Build/<Platform>/<ProfileName>/...`

Example:

- `Engine/Intermediate/Build/<Platform>/DurinEditor/...`

### 4. CMake imports generated metadata

Relevant code:

- `CMake/Modules.cmake`

This is where profile-derived values become CMake variables and compile definitions.

## Current Derived Behavior

### Module naming

Module output names use:

- `<ProfileName>-<ModuleName>`

Example:

- `DurinEditor-Core.dll`
- `DurinGame-Core.dll`

Relevant code:

- `CMake/Modules.cmake`
- `Engine/Source/Runtime/Core/Private/Modules/ModuleManager.cpp`

### Launcher naming

The editor/game launcher output name matches the active profile name.

Examples:

- `DurinEditor.exe`
- `DurinGame.exe`

The launcher target itself is still named:

- `DurinLauncher`

But its output name is set from the active profile.

### Intermediate build layout

Intermediate generated files are partitioned by profile.

Example:

- `Engine/Intermediate/Build/Win64/DurinEditor/...`
- `Engine/Intermediate/Build/Win64/DurinGame/...`

### Semantic compile definitions

Currently exposed compile definitions include:

- `DURIN_PROFILE_NAME`
- `DURIN_APP_CONFIG_NAME`
- `DURIN_WITH_EDITOR`

`DURIN_WITH_EDITOR` is the main semantic macro currently intended for code branching.

## Recommended Usage in Code

### Preferred

Use semantic branching:

```cpp
#if DURIN_WITH_EDITOR
// editor-only code
#endif
```

### Avoid overusing

Do not spread direct profile-name checks everywhere unless there is a very specific reason.

Prefer:

- `DURIN_WITH_EDITOR`

Over:

- hand-written branches that directly encode `DurinEditor` vs `DurinGame`

The profile name is still useful for:

- module naming
- launcher naming
- startup selection
- very low-level host decisions

## Current Runtime Rule

At runtime, module loading derives filenames from the configured profile name.

For example, `LoadModule("Core")` resolves to something like:

- `DurinEditor-Core.dll`
- `DurinGame-Core.dll`

This logic is implemented in:

- `Engine/Source/Runtime/Core/Private/Modules/ModuleManager.cpp`

## Current Limitations

Profiles currently control naming and build partitioning, but not the entire runtime behavior surface.

Known state:

- naming and build output separation are in place
- `DURIN_WITH_EDITOR` is in place
- startup behavior is not yet fully profile-driven

For example:

- `DurinGame` exists as a profile
- but not every runtime/init path has been fully split into editor/game behavior yet

## When Adding a New Profile

Minimum steps:

1. Add a new `.dprofile` file under `Engine/Profiles`
2. Add matching root CMake presets in `CMakePresets.json`
3. Ensure generated/intermediate output paths are exercised for the new profile
4. Decide whether `WithEditor` should be true or false
5. Verify launcher naming, module naming, and config naming

Potential future examples:

- `DurinPak`
- `DurinServer`

## Naming Conventions

Current naming convention favors the profile name as the host identity.

Examples:

- profile name: `DurinEditor`
- module prefix: `DurinEditor-`
- launcher output: `DurinEditor.exe`
- intermediate directory segment: `DurinEditor`

This keeps naming rules consistent and avoids introducing redundant host-name variables.

## Summary

Use these mental models:

- project = whose code/content this is
- profile = how this project is built and hosted
- CMake target = what is actually compiled

Current stable rule:

- `ProfileName` drives naming
- `DURIN_WITH_EDITOR` drives the most important code-level semantic split
