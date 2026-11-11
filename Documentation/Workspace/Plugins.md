# Plugin Metadata And Selection

Summary: Define source-plugin descriptors, project overrides, and deterministic metadata resolution before build or startup integration.

Modules: Projects, Core

Last reviewed: 2026-11-11

## Implemented Boundary

Projects depends only on Core and exposes the value-only `ResolvePlugins`
query. DHT provides `config.plugin_config.resolve_plugins`; DevTool exposes the
same Python implementation through `build.plugins.resolve_plugins`. Neither
resolver builds, loads native code, registers types, or publishes mounts.

Core retains project admission and the current project's explicit
`PluginOverrides`. Projects does not change `EnabledRootModules`. Launch has a
build dependency on Projects so the module belongs to both runtime variants;
plugin loading is not yet wired into Launch or host startup. Later integration
is tracked in the [implementation plan](../Plans/RestartBasedPluginSystem.md).

## Descriptor v1

Discover `.dplugin` files beneath `Engine/Plugins` and the selected project's
`Plugins`. A descriptor directory owns one plugin; discovery stops there. A
separate ownership audit rejects nested descriptors. Reject multiple descriptors
in one root, case-insensitive duplicate plugin/module IDs, and module names
colliding with supplied project modules. Source paths must stay beneath their
owner. Symbolic links and junction aliases within plugin trees are unsupported
in v1, including loops and escaping entries.

```json
{
  "SchemaVersion": 1,
  "Id": "Water",
  "DisplayName": "Water",
  "Version": "1.0",
  "Dependencies": ["Environment"],
  "Content": "Content",
  "Shaders": "Shaders",
  "Configs": "Configs",
  "Modules": [
    {"Name": "Water", "Path": "Source/Water", "Host": "Runtime", "Phase": "Providers"},
    {"Name": "WaterEditor", "Path": "Source/WaterEditor", "Host": "Editor", "Phase": "EditorReady"}
  ]
}
```

SchemaVersion and Id are required. Absent/empty Modules declares a content-only
plugin. Optional DisplayName and Version are nonempty metadata strings. Content,
Shaders, and Configs are optional contained relative directories; absent roots
declare no corresponding data. Declaration does not create or publish roots.
Path names a directory containing `<Name>.dmodule`, whose ModuleName must match.

Host defaults to Runtime; Phase defaults to Providers. Plugin/module Platforms
is an optional identifier list: absent/empty means unrestricted, otherwise match
the requested platform exactly. Target variants are DurinEditor and DurinGame;
Editor modules are excluded from Game. Runtime cannot use EditorReady.

The structural authority is
[`durin-plugin.schema.json`](../../Engine/Source/Programs/DurinHeaderTool/schemas/durin-plugin.schema.json).
Semantic checks additionally enforce ownership, dependency, and phase rules.

## Project Overrides And Dependencies

```json
{"ProjectName": "Example", "Plugins": {"Water": true, "UnusedTools": false}}
```

Absent Plugins and an empty object mean no explicit choices. Values must be
boolean. Unmentioned plugins are inactive unless required by an enabled plugin.
Explicit false conflicts with required activation and produces an error. Even
disabled explicit IDs must refer to discovered descriptors. An enabled plugin
with an incompatible platform is an error. An unused module filtered by host or
platform is excluded; a required edge to that module is an error.

Plugin Dependencies activates required plugins; `.dmodule` owns module/link
edges. A module edge to another plugin requires that plugin in its owner's
direct Dependencies. Transitive paths are checked edge by edge. Runtime plugin
modules cannot reach Editor plugin modules or existing project modules reachable
only from Editor roots. Existing project modules retain conditional editor
composition. Optional module dependencies never activate targets: they apply
only to independently enabled, eligible modules, matching existing DHT rules.
Required project-module paths into disabled plugins fail without activating them.
Diagnostics include the descriptor and traversed module/plugin chain.

## Startup Phase Contract For Later Integration

The resolver validates these ordered phases. Stage 3 will insert hooks in the
existing startup sequence; it does not add another readiness state machine.

| Phase | Editor insertion point | Game insertion point |
| --- | --- | --- |
| Providers | Synchronous editor Init, before project roots and InitializeProjectContent | StartGame, before project roots and native gameplay settings resolution |
| EngineReady | Synchronous editor Init, after provider/project registration and before InitializeProjectContent | StartGame, after provider/project registration and before InitializeProjectContent |
| EditorReady | Synchronous workspace initialization, after LoadWorkspaceModules, before AddRegisteredEditorSubsystems/InitializeWorkspaces | Not admitted |

Engine services already exist at these host admission points. Modules supplying
types/subsystems needed earlier must choose Providers. Consumers cannot depend
on later-phase plugin providers, including through intermediate modules.
Existing project roots are Providers consumers for this check.

Shader-root declaration is descriptor-driven, before the initialization-time
shader registry freezes; it is independent of module startup callbacks. These
are selected integration boundaries, not claims that startup hooks already run.

## Canonical Selection And Build Inventory

[`durin-plugin-selection.schema.json`](../../Engine/Source/Programs/DurinHeaderTool/schemas/durin-plugin-selection.schema.json)
defines target platform/variant, dependency-first plugins with Explicit/RequiredBy
reasons, dependency/phase-ordered modules with plugin owner, logical descriptor
inputs and hashes, and a fingerprint.

Canonical JSON uses UTF-8, sorted object keys, no optional whitespace, standard
JSON control-character escaping, and literal non-ASCII characters. Arrays have
resolver-defined deterministic order. Input Hash is 64-bit FNV-1a over exact
descriptor bytes, rendered as 16 lowercase hexadecimal digits. Fingerprint is
the same hash over canonical selection with Fingerprint omitted. Input-byte
changes, including line endings, intentionally invalidate selection. These are
change detectors, not native compatibility or authenticity checks.

Logical inputs are `Project/<ProjectName>`, `Project/<ProjectName>/<Module>`,
`Plugin/<Id>`, and `Plugin/<Id>/<Module>`. All inspected descriptors, including
inactive metadata, contribute to the discovery snapshot. Absolute paths are
returned separately in Locations; relocation preserves selection when descriptor
bytes stay the same. Callers supply every workspace project for ownership/graph
validation, in addition to the automatically included engine and active project.

[`durin-plugin-build.schema.json`](../../Engine/Source/Programs/DurinHeaderTool/schemas/durin-plugin-build.schema.json)
is the separate inventory contract: Stage 2 produces it and Stage 3 admits it.
It requires SelectionFingerprint and native Identity fields EngineBuild,
Platform, Architecture, Configuration, RuntimeVariant, and ToolchainABI. Each
binary records Module, Owner, a contained relative Path, and RuntimeDependencies.
Paths resolve beneath the selected build/staging root; physical lookup remains
machine-local. Stage 1 defines/tests the schema, not artifact production or
compatibility admission.

## Validation

Python and native tests consume shared read-only Cases.json in
`Engine/Tests/Native/PluginResolutionTests/Data`, comparing golden canonical
success manifests and expected error/chain fragments. Python also checks schema
constraints, existing Engine/Sandbox/RoadWeaver roots, descriptor-reader parity,
relocation, and fingerprint invalidation. CoreFileSystemTests covers active-project
override capture and invalid input rejection. Follow the
[testing workflow](../Agents/Testing.md) for execution.
