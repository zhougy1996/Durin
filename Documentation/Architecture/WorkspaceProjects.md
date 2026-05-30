# Workspace And Projects

This document explains how Durin currently models the workspace, projects, modules, and profiles.

## Overview

Durin currently has four important concepts:

- workspace
- project
- module
- profile

They are related, but they do not map 1:1 to each other.

## Workspace

The workspace is the repository root.

Current example:

- `G:/Workspace/Durin`

The workspace is the top-level CMake source directory and the place CLion should open for normal source development.

Typical workspace contents:

- `Engine/`
- game projects such as `SandBox/`
- shared root `CMakeLists.txt`
- shared root `CMakePresets.json`

## Project

A project is a top-level code/content owner inside the workspace.

Current examples:

- `Engine`
- `SandBox`

A project typically owns its own:

- `.dproject`
- `Source/`
- `Profiles/`
- `Configs/`
- `Intermediate/`
- `Binaries/`

Projects are registered with DurinHeaderTool through:

- `Engine/Source/Programs/DurinHeaderTool/Configs/RegisteredProjects.json`

`Engine` remains a built-in registered project. Additional projects such as `SandBox` are registered through that JSON file.

## Module

A module is the compilation and runtime loading unit.

Examples:

- `Core`
- `RenderCore`
- `SandBox`

Modules belong to a project, but module dependencies can cross project boundaries.

For example:

- `SandBox` can depend on `Engine`
- `SandBox` can depend on `Core`

The module system is the main extensibility boundary. Most new gameplay and editor code should start life as a module, not a new project.

## Profile

A profile describes the current host/runtime mode used for the build.

Current profiles:

- `DurinEditor`
- `DurinGame`

Important current rule:

- `DURIN_PROFILE_NAME` is workspace-global

That means one configure/build tree selects a single active profile name and all projects in that workspace build against it.

Examples:

- `Win64-Debug-DurinEditor` builds both `Engine` and `SandBox` in `DurinEditor` mode
- `Win64-Debug-DurinGame` builds both `Engine` and `SandBox` in `DurinGame` mode

## Why Project Profiles Still Use `DurinEditor` / `DurinGame`

Even though each project has its own `Profiles/` directory, profile names are currently shared across the workspace because the active profile is selected globally through CMake presets and `DURIN_PROFILE_NAME`.

So this is intentional and currently correct:

- `Engine/Profiles/DurinEditor.dprofile`
- `SandBox/Profiles/DurinEditor.dprofile`

The profile files belong to different projects, but the active profile name is the same workspace-wide host identity.

In other words:

- profile files are project-local
- profile names are currently workspace-global

## CLion Workflow

For normal development, open the workspace root in CLion:

- `G:/Workspace/Durin`

Do not treat `SandBox/` as the only root project unless the build system is later redesigned for an installed-engine or game-root workflow.

Current expected workflow:

- open the workspace root
- select a root preset such as `Win64-Debug-DurinEditor`
- build targets from both `Engine` and `SandBox` inside the same workspace

## What This Means For Naming

Because profiles are workspace-global today, game projects should continue to use:

- `DurinEditor`
- `DurinGame`

instead of project-specific names such as:

- `SandBoxEditor`
- `SandBoxGame`

Project-specific profile names only become natural if the architecture later changes so each project can choose its own independent profile namespace.

## Recommended Mental Model

Use this model for the current system:

- workspace = one source-development super-project
- project = one top-level code/content owner inside the workspace
- module = one build/runtime unit inside a project
- profile = one workspace-wide host mode

For a small self-developed engine, the most common shape is:

- one `Engine` project
- one primary game project
- many modules
- very few additional top-level projects

## Future Direction

If Durin later grows an installed-engine workflow, then a game-root model may become reasonable:

- game project opened as the IDE root
- engine treated as an external installed dependency
- project-specific host identities such as `SandBoxEditor`

That is not the current model.

