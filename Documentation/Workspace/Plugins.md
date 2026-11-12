# Plugin Metadata And Selection

Summary: Define source-plugin descriptors, project overrides, deterministic selection, and source-build integration before startup loading.

Modules: Projects, Core

Last reviewed: 2026-11-12

## Implemented Boundary

Projects depends only on Core and exposes the value-only `ResolvePlugins`
query. DHT provides `config.plugin_config.resolve_plugins`; DevTool exposes the
same Python implementation through `build.plugins.resolve_plugins`. Neither
resolver builds, loads native code, registers types, or publishes mounts.

Projects now owns project admission, the current-project context, history
persistence, and edit-session ownership in `Project/Project.h` and related
headers. The former Core `Misc/Project*.h` entry points have been removed and
their consumers now depend on Projects. The project manager/descriptor APIs are
introduced in `Interfaces/IProjectManager.h` and `ProjectDescriptor.h`.
Projects does not change `EnabledRootModules`. Launch has a
build dependency on Projects so the module belongs to both runtime variants;
plugin loading is not yet wired into Launch or host startup. Later integration
is tracked in the [implementation plan](../Plans/RestartBasedPluginSystem.md).

Core no longer parses project fields. Projects supplies validated mount values
through Core's mount mechanism. Ordinary project admission, status/history
inspection, and native plugin selection share FProjectDescriptor. Launch selects
interactive history restoration and executes deferred relaunch after shutdown;
Game/tool admission does not restore editor history. MainFrame retains the
interactive recording/unsaved-work policy and Projects persists history values.

## Ordinary Project Management

Principal interfaces follow UE's names and result semantics. FProjectDescriptor
offers bool Load/Read/Save with an output failure reason. IProjectManager::Get()
offers bool LoadProjectFile, QueryStatusForProject/CurrentProject (status output),
LoadModulesForProject (ELoadingPhase), SaveCurrentProjectToDisk,
SetPluginEnabled, and RemovePluginReference. Save/edit methods provide an output
failure reason; GetLastError supplies diagnostics for load/status/module failures.
Native strings and descriptor formats remain Durin-specific. Public principal
interfaces are not constrained to std::expected; internal helpers may use it.

GetCurrentProject on the manager returns the requested descriptor. The separate
Project/Project.h context snapshot supplies effective paths and admitted module
roots to existing consumers. Descriptor/plugin edits mark pending state dirty;
saving atomically persists configuration without changing the active snapshot.
The manager owns both states; the context accessors delegate to it. Typed
descriptor fields are the persistent authority, including module selections,
mounts, schema annotation, and native-test root. Save validates and serializes
those fields without retaining a second JSON document. Setting an unchanged
override does not mark the project dirty.
Explicit false and removal of an override have different persisted meanings.
Changing projects after ordinary root-module startup requires process restart.
ClearCurrentProject is for pre-startup/teardown safe points, not live switching.

Admission validates the complete descriptor, root selections, and mount values
before publishing path/context state. A failed load leaves the previous descriptor,
context, path, mount storage, and edit ownership intact. Queries parse detached
values and never replace registry storage or increment its revision. Core's pure
mount validator is shared with publication, which remains immutable. Manager
mutation/status operations are control-thread services; detached descriptor reads
do not require Engine, Renderer, editor UI, or Python.

Editor, Game, and the standalone asset tool delegate ordinary root loading to
LoadModulesForProject at their existing pre-content points. Ordinary project
roots use Providers; other phase calls do no ordinary-root work. Plugin phase
execution remains the later startup integration stage. RelaunchEditorForProject
returns void because it only queues a request and asks the host to exit. Launch
consumes it once and reports actual process-launch failures outside Projects.

## Descriptor v1

Discover `.dplugin` files beneath `Engine/Plugins` and the selected project's
`Plugins`. A descriptor directory owns one plugin; discovery stops there and
ignores descriptors below that boundary, following UE. Reject multiple descriptors
in one root, case-insensitive duplicate plugin/module IDs, and module names
colliding with supplied project modules. Source paths must stay beneath their
owner, including the final consumed module file. Discovery rejects symbolic
plugin search directories and junction aliases; unconsumed content/build trees
are not audited.

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
inactive plugin metadata, contribute to the discovery snapshot. Plugin module
files are read only for selected plugins with eligible host/platform modules;
disabled/excluded module bytes do not contribute. Native results expose typed
Plugins and Modules; ManifestJson is their portable artifact projection. Absolute paths are
returned separately in Locations; relocation preserves selection when descriptor
bytes stay the same. Callers supply every workspace project for ownership/graph
validation, in addition to the automatically included engine and active project.

[`durin-plugin-build.schema.json`](../../Engine/Source/Programs/DurinHeaderTool/schemas/durin-plugin-build.schema.json)
is the separate inventory contract: Stage 2 produces it and Stage 3 admits it.
It requires SelectionFingerprint and native Identity fields EngineBuild,
Platform, Architecture, Configuration, RuntimeVariant, and ToolchainABI. Each
binary records Module, Owner, a contained relative Path, and RuntimeDependencies.
Paths resolve beneath the selected build/staging root; physical lookup remains
machine-local. The build produces this inventory. The read-only Projects API
`AdmitPluginBuild` compares its selection fingerprint and six identity fields
against the caller-supplied resolved selection and host identity, verifies exact
module/owner membership, and checks binary and runtime-dependency files beneath
the inventory directory (including canonical containment). It returns paths in
selection order only after every check passes; failure preserves the output and
reports a configure/build requirement. This API does not load libraries or
authenticate binary contents. Startup integration and host identity derivation
remain pending.

## Source Build Integration

DHT resolves each workspace project's selection and builds the union of required
module targets. `owning_plugin` remains independent from `owning_project`; plugin
modules do not enter the project's authored ModuleDirs or ordinary startup roots.
Foreign workspace projects provide module ownership and dependency context;
only Engine and the active project's roots participate in that project's
selection validation. Runtime plugins can require ordinary non-root modules;
the workspace build closure includes those modules under their original owners.

Plugin modules use the existing add_durin_module, dependency/export metadata,
reflection generator, PCH, include, and linking paths. DHT emits a
`<Module>API.h` using the standard module export define. Native plugin modules
currently require Shared linkage. Generated metadata is under
`<Project>/Intermediate/Build/<Platform>/<RuntimeVariant>/Plugins/<Id>/<Module>`.
It is configuration-independent, like ordinary DHT metadata. Native binaries,
import libraries, and symbols additionally separate configuration and variant
under `<Project>/Binaries/<Platform>/<Configuration>/Plugins/<Id>/<RuntimeVariant>`.

CMake watches discovered directory membership up to plugin boundaries and the
consumed descriptors. A normal DevTool build reconfigures when descriptors are
created/deleted or enablement changes. Disabled modules disappear from generated
targets; neither existing metadata nor stale DLLs imply selection.

`<Project>PluginBuild` builds the selected native closure and publishes
`PluginBuild.json`, `PluginSelection.json`, and the declared shared-library
dependency files beneath
`Build/<preset>/PluginBuild/<Project>/<Configuration>/`. It participates in all
when plugin discovery is active; a direct individual module build need not
publish the full inventory. Paths in the inventory are relative to this root.
EngineBuild currently combines the engine semantic version and built Core
binary SHA-256; ToolchainABI records compiler ID/version and pointer size.
Platform, architecture, configuration, runtime variant, and selection fingerprint
are explicit. Publication is a build artifact operation, without startup loading,
content cooking, or a binary compatibility guarantee.

The repository-owned [fixture](../../Engine/Tests/Fixtures/Plugins/README.md)
qualifies a reflected asset, a dependent plugin subclass, an Editor companion,
and a content-only plugin with exported calls across plugin DLLs.

## Validation

Python and native tests consume shared read-only Cases.json in
`Engine/Tests/Native/PluginResolutionTests/Data`, comparing golden canonical
success manifests and expected error/chain fragments. Python also checks schema
constraints, existing Engine/Sandbox/RoadWeaver roots, descriptor-reader parity,
relocation, and fingerprint invalidation. CoreFileSystemTests covers active-project
override capture and invalid input rejection. Follow the
[testing workflow](../Agents/Testing.md) for execution.

Projects.json additionally supplies common ordinary-project fixtures for native
and Python readers. Native coverage checks query/admission failure isolation,
mount-storage identity, pending-versus-active edits, atomic save failure, and
case-insensitive override removal. Process coverage verifies Browser/explicit
project admission, history protection, edit-session exclusion, and relaunch
request consumption. Native platform qualification is recorded in the plan.
