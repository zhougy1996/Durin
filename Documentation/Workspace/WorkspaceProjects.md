# Workspace And Projects

This document explains the boundary between the workspace, projects, modules,
and runtime variants in the current Durin architecture.

## Overview

Use this mental model:

- workspace = the repository root
- project = a top-level code or content owner inside the workspace
- module = a build and runtime loading unit inside a project
- runtime variant = the workspace-wide host mode selected for one configure or build tree

## Workspace

The workspace is the repository root, opened as the top-level CMake source directory for normal development.

Typical workspace contents:

- `Engine/`
- game projects such as `Sandbox/`
- root `CMakeLists.txt`
- root `CMakePresets.json`

## Projects And Modules

Projects are top-level owners such as `Engine` or `Sandbox`. They typically own `.dproject`, `Source/`, `Configs/`, `Intermediate/`, and `Binaries/`.

At runtime, launch a specific project with
`DurinDevTool run --project <path-to-project.dproject>`. DurinDevTool accepts a
workspace-relative or absolute existing descriptor and forwards its normalized
absolute path through the launcher's `--project=<path>` contract. The project
root is the descriptor's parent directory, so projects may live outside the
engine workspace. `ProjectName` is display/build identity; the selected
project root plus its explicit `Content` path publishes the fixed logical
`/Game/` mount. The engine root plus `Content` publishes `/Engine/`.

CMake passes complete `.dproject` paths to DurinHeaderTool. Generated build metadata preserves the complete set of project descriptors needed to resolve cross-project module dependencies; there is no global project registry file.

### Descriptor Schemas And Validation

The repository-owned structural contracts are
[`durin-project.schema.json`](../../Engine/Source/Programs/DurinHeaderTool/schemas/durin-project.schema.json)
and
[`durin-module.schema.json`](../../Engine/Source/Programs/DurinHeaderTool/schemas/durin-module.schema.json).
Both use JSON Schema Draft 2020-12. DurinHeaderTool and DurinDevTool validate
descriptors against the same files before extracting their own model fields;
the VS Code settings template associates `*.dproject` and `*.dmodule` files
with them for completion and inline diagnostics. A descriptor may include a
non-empty `$schema` string for another editor, but it is optional and does not
select the runtime validator.

`ProjectName` and `ModuleName` are the only required construction fields.
Omitted project maps/lists are empty, except that omitting `BaseModules`
enables every `ModuleDirs` entry as a base root. Omitted module dependency and
reflection-header lists are empty, `LinkType` defaults to `Shared`, and `PCH`
defaults to `Self`. The supported link types are exactly `Shared` and `Static`.
The project schema covers the complete descriptor, including the runtime-owned
`Mounts` section described below, even though DHT retains only build fields in
its project model.

Descriptor loading is strict and does not coerce values. Unknown root or
nested fields, wrong scalar/container/element types, typed fields set to
`null`, empty required names/paths, exact duplicate list items, duplicate JSON
object members, and unsupported closed-set values fail configuration. Errors
identify the descriptor and JSON property path; malformed JSON additionally
reports its line and column. This makes misspellings such as
`PrivateDependecies` fail before generated CMake metadata is published instead
of silently selecting an empty dependency list.

Schemas own structural validation only. Case-insensitive project/module
uniqueness, project ownership, dependency graph validity, runtime-variant
references, filesystem existence, relative-path containment, canonical mount
overlap, and mount dependency rules remain explicit semantic checks in the
owning tool or runtime because they require workspace or filesystem context.

## Mounted Content And Sources

A logical mount has one owner `Root`, one configurable relative `ContentPath`,
and one effective physical directory returned by `GetContentDir()`. Both
extensionless `FAssetPath` package identities and complete `FSourcePath` file
identities resolve relative to that same directory. The path types retain
different validation and persistence rules, but never select different roots:

```text
/Game/Textures/T_Stone
  -> <project>/Content/Textures/T_Stone.dasset

/Game/Art/Stone.png
  -> <project>/Content/Art/Stone.png
```

The engine publishes `/Engine/`; the selected project publishes `/Game/` and
may reference `/Engine/`. Registry publication is immutable for the active
project lifetime.

An active project may declare plugin-shaped or manually scanned external
mounts:

```json
{
    "Mounts": [
        {
            "VirtualRoot": "/Plugins/PCG/",
            "Owner": "Extension",
            "Root": "Plugins/PCG",
            "ContentPath": "Content",
            "AutoScan": true,
            "ContentWritable": false,
            "Dependencies": ["/Engine/"]
        },
        {
            "VirtualRoot": "/Libraries/StudioArt/",
            "Owner": "ExternalSources",
            "Root": "Libraries/StudioArt",
            "ContentPath": ".",
            "AutoScan": false,
            "ContentWritable": false,
            "Dependencies": ["/Engine/"]
        }
    ]
}
```

Every entry requires exactly `VirtualRoot`, `Owner`, `Root`, `ContentPath`,
`AutoScan`, `ContentWritable`, and `Dependencies`. Only `Extension` and
`ExternalSources` owners are accepted. `Root` is descriptor-relative;
`ContentPath` is relative to that root. Neither may traverse or be absolute,
and custom mounts cannot override `/Engine/` or `/Game/`. `AutoScan` controls
recursive `.dasset` discovery only: a manual-scan mount still admits valid
asset and source identities and direct package loading. The active `/Game/`
mount automatically depends on every additional mount; each additional mount
declares its own outgoing dependencies.

`ContentWritable` is the canonical descriptor key. The runtime loader accepts
legacy `AuthoringWritable` only as an input migration path for unversioned
project descriptors and rejects entries containing both spellings. Schemas,
checked-in descriptors, examples, and generated output use only the canonical
key.

A declared root may be a directory, junction, or symbolic link. Canonical
containment requires the effective content directory to remain beneath `Root`
and rejects a nested link that escapes it. Effective content directories may
not canonically overlap, although distinct mounts may share an owner root when
their content subdirectories are disjoint. A missing content directory remains
registered as unavailable so packages can load from valid derived
data and report a repairable diagnostic; it never falls back to another
physical directory. Workstation-specific absolute paths do not belong in
committed descriptors.

Modules are the compilation and runtime loading units. They belong to a project, but their dependencies can cross project boundaries.

## Project Game Settings

Engine owns the gameplay projection of `<project>/Configs/Project.yaml` under
one `Game` section:

```yaml
Game:
  DefaultLevel: /Game/Levels/NewLevel
  NativeModule: Sandbox
  GameModeClass: Durin::Sandbox::ADefaultGameMode
```

`FProjectGameSettingsStore` is the shared reader and default-level writer for
standalone startup, PIE, the Level Editor, and external asset-reference
contribution. `DefaultLevel` is optional. `NativeModule` and `GameModeClass`
must either both be absent/empty for lifecycle-only play or both be non-empty
scalars. Updates to the default level preserve the native pair and unrelated
YAML settings; the obsolete `Editor.DefaultLevel` route is not read as an
alias.

`NativeModule` is the logical name passed to `FModuleManager` after reflected
object initialization. `GameModeClass` must be the exact fully qualified
reflection identity of a constructible `AGameMode` derived class registered by
that module. Resolution never searches short names, loads every project module,
or falls back after a configured error. PIE and standalone surface the same
module/class/settings-route diagnostic and leave the World stopped if
resolution or gameplay bootstrap fails.

Most new gameplay or editor work should start as a module, not a new project.

Use `DurinDevTool create module` to create and register one. By default the command
writes runtime, editor, and developer modules beneath `Source/Runtime`,
`Source/Editor`, and `Source/Developer`, respectively. Runtime defaults to a
base root; editor and developer default to the `DurinEditor` root. These are
generator and root-selection conventions rather than architectural
categories: `--path <ProjectRelativePath>` may place a module anywhere inside
its owning project, for example `Source/Game/Combat`,
`Source/Features/Inventory`, or `Source/Tools/WorldTools`.

`ModuleDirs` is the authoritative mapping from module name to project-relative
module root. `BaseModules` and `ExtraModules.<RuntimeVariant>.Modules`, not the physical
directory name, determine where the module is enabled. Within each module root,
ordinary source discovery still uses its recursive `Public/` and `Private/`
trees as the visibility boundary.

Developer is not a third runtime variant and a `Source/Developer` path does
not make a module editor-only by itself. Programs such as Cook explicitly
select the Developer modules they require, while a game root that does not
select them never receives them through directory discovery. For asset
authoring, editor roots select `DerivedDataCache` and the needed
`TextureBuild`/`StaticMeshBuild`/`SkeletalBuild`/`TerrainBuild` recipes explicitly; package audit,
canonical-resave, and game roots select none of them.

Creation adds the selected relative directory to `ModuleDirs` and appends the
selected enablement roots without reordering existing entries. Module names and
CMake targets are case-insensitively unique across the workspace; module roots
must not overlap, and dependencies may cross project boundaries but must already
exist. The full command syntax and defaults are documented in
`Documentation/Development/Build/BuildAndRun.md`.

When a separate top-level owner is required, use
`DurinDevTool create project <Name> --path <Path>`. In the current workflow the
path must be a new direct child of the workspace root. The command creates the
project descriptor and CMake entrypoints, `Configs/` and `Content/`, and a
same-named runtime module enabled in `BaseModules`; it also adds the explicit
root `add_subdirectory(...)` registration in the same transaction. Workspace
project and initial module names remain case-insensitively unique. External,
installed-engine, and nested project creation remain outside this workflow.

## Runtime Variants

Current runtime variants are `DurinEditor` and `DurinGame`.

Important rule: `DURIN_RUNTIME_VARIANT` is workspace-global. One configure or
build tree selects a single active runtime variant, and all projects in that
tree build against it.

Examples:

- `Win64-Debug-DurinEditor` builds both `Engine` and `Sandbox` in `DurinEditor` mode
- `Win64-Debug-DurinGame` builds both `Engine` and `Sandbox` in `DurinGame` mode

Because runtime variants are workspace-global today, game projects should
continue using shared runtime-variant names such as `DurinEditor` and
`DurinGame` rather than project-specific names.

## Workflow Note

For normal development, open the workspace root rather than a single project subdirectory. Build targets from `Engine` and game projects inside the same workspace.
