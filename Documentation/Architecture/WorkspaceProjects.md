# Workspace And Projects

This document explains the boundary between the workspace, projects, modules, and profiles in the current Durin architecture.

## Overview

Use this mental model:

- workspace = the repository root
- project = a top-level code or content owner inside the workspace
- module = a build and runtime loading unit inside a project
- profile = the workspace-wide host mode selected for one configure or build tree

## Workspace

The workspace is the repository root, opened as the top-level CMake source directory for normal development.

Typical workspace contents:

- `Engine/`
- game projects such as `SandBox/`
- root `CMakeLists.txt`
- root `CMakePresets.json`

## Projects And Modules

Projects are top-level owners such as `Engine` or `SandBox`. They typically own `.dproject`, `Source/`, `Configs/`, `Intermediate/`, and `Binaries/`.

Projects are registered through `Engine/Configs/RegisteredProjects.json`.

Modules are the compilation and runtime loading units. They belong to a project, but their dependencies can cross project boundaries.

Most new gameplay or editor work should start as a module, not a new project.

## Profiles

Current profiles are `DurinEditor` and `DurinGame`.

Important rule: `DURIN_PROFILE_NAME` is workspace-global. One configure or build tree selects a single active profile, and all projects in that tree build against it.

Examples:

- `Win64-Debug-DurinEditor` builds both `Engine` and `SandBox` in `DurinEditor` mode
- `Win64-Debug-DurinGame` builds both `Engine` and `SandBox` in `DurinGame` mode

Because profiles are workspace-global today, game projects should continue using shared profile names such as `DurinEditor` and `DurinGame` rather than project-specific profile names.

## Workflow Note

For normal development, open the workspace root rather than a single project subdirectory. Build targets from `Engine` and game projects inside the same workspace.

## Future Direction

An installed-engine or game-root workflow may eventually justify project-specific host identities, but that is not the current model.
