# Plugin Metadata And Selection

Summary: Define source-plugin descriptors, project overrides, deterministic selection, source-build integration, and frozen native startup.

Modules: Projects, Core, Engine, ShaderBuild, MainFrame

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
Launch admits the frozen native selection before mount and renderer setup;
the host executes its module phases before their consumers. Content mounts,
early authored shader roots, and layered defaults are integrated.
MainFrame provides restart-required plugin management. Cooked plugin deployment
integration remains tracked in the [implementation plan](../Plans/RestartBasedPluginSystem.md).

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
execution follows the startup contract below. RelaunchEditorForProject
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
An explicitly disabled required dependency leaves its requested consumers
inactive, including transitive consumers; their true overrides remain saved.
The selection manifest reports these requests in optional `InactivePlugins`
entries with an ID and reason, and startup logs a warning. Unrelated valid
plugins remain eligible. Even disabled explicit IDs must refer to discovered
descriptors. An enabled plugin
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

## Frozen Native Startup

Launch calls IProjectManager::PreparePluginStartup before default mount
publication and renderer/shader initialization. Projects resolves the source
workspace and active project's on-disk descriptors, admits the inventory, and
freezes native membership. This is independent of later descriptor edits and
does not add another readiness state machine. The existing hosts execute these
dependency-ordered phases:

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
module hooks run at the points above. Projects prepares descriptor-derived resource roots in the same admission pass.
Content roots join the single immutable mount publication; ShaderBuild consumes
shader roots before freezing its registry. Pending descriptor edits cannot
replace these active roots.

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
is the available-artifact contract, independently of project enablement. Schema
version 2 replaces the selection-bound version 1; startup rejects version 1 with
a configure/build diagnostic. `Inputs` records logical descriptor paths and
hashes for discovered plugin metadata, eligible plugin modules, and ordinary
module descriptors. Project descriptor bytes and startup selection fingerprints
are excluded, so override-only edits require neither a build nor republication.

Native `Identity` contains EngineBuild, Platform, Architecture, Configuration,
RuntimeVariant, and ToolchainABI. Each binary records Module, Owner, a contained
relative Path, and RuntimeDependencies. Paths resolve beneath the inventory root.
The read-only `AdmitPluginBuild` API compares the six identity fields against
host-supplied values, then validates current selected plugin descriptor/module
hashes and ordinary module descriptor hashes against build inputs. Ordinary
module validation is conservative across the workspace; project override bytes
and inactive plugin metadata do not invalidate selected artifacts.

The inventory may contain more modules than startup selects. Admission checks
selected ownership, paths, required files and runtime dependency closure, and
returns only selected paths in dependency/phase order. Unused binary files may
be absent; extra entries do not grant loading permission. Duplicate module/path
entries, path escape and selected dependencies on unselected plugin binaries are
rejected. Publication occurs only after every check passes; failure preserves
the previous output. Admission does not authenticate binary contents or rebuild
missing/stale artifacts.

Source startup finds the inventory under the configured build directory's
`PluginBuild/<Project>/<Configuration>` directory. The host derives EngineBuild
from its engine version and the SHA-256 of its loaded Core library;
architecture/toolchain come from its compiled CMake configuration. The candidate
inventory never supplies the expected identity. Startup selection remains a
separate canonical artifact describing process policy.

Core freezes ordinary workspace module names and exact selected plugin paths
once per process. A selected plugin becomes loadable only when Projects admits
its phase; unknown, disabled, and not-yet-admitted names cannot use fallback DLL
lookup or override registration. Native dependencies come from the verified
inventory closure. Plugin records retain native code for process lifetime,
including factory/startup failures after native loading. Runtime unload is
rejected even if a plugin opts into dynamic reload. Existing module shutdown
calls run in reverse startup completion order after host consumers retire;
partial StartupModule failures call the module's own cleanup once.

Browser admission uses the build-generated ordinary module list and starts no
project plugins. Opening a project uses the existing unsaved-work/relaunch path;
LoadProjectFile and ClearCurrentProject cannot replace a frozen admission.
No-plugin staged Game layouts retain source-independent ordinary startup using
the same generated list. Plugin-bearing staged deployment is not admitted by
this source-startup path and remains cook/deploy integration work.

## Source Build Integration

DHT uses `config.plugin_config.enumerate_plugin_build` to discover all plugins
under Engine and every declared workspace project's plugin roots. It enumerates
all platform/host-eligible modules regardless of enablement overrides, validates
the build dependency graph, and emits a build catalog with Modules and Inputs.
It does not synthesize an all-enabled startup selection. Invalid eligible module
metadata fails a build even when its plugin is disabled. Startup still resolves
only Engine and the active project's plugin roots and applies project overrides.

`owning_plugin` remains independent from `owning_project`; plugin modules do not
enter authored ModuleDirs or ordinary startup roots. Plugin dependencies can
bring ordinary non-root modules into the workspace build closure under their
original owners. Game filters out Editor plugin modules. Enablement conflicts
remain startup errors and do not prune compilation targets.

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
created/deleted or project descriptors change. Override-only edits preserve
plugin target membership; startup does not require this reconfiguration. Neither
existing metadata nor stale DLLs imply startup selection.

`<Project>PluginBuild` builds the available workspace native closure and publishes
`PluginBuild.json`, `PluginCatalog.json`, and the declared shared-library
dependency files beneath
`Build/<preset>/PluginBuild/<Project>/<Configuration>/`. It participates in all
when plugin discovery is active; a direct individual module build need not
publish the full inventory. Paths in the inventory are relative to this root.
EngineBuild currently combines the engine semantic version and built Core
binary SHA-256; ToolchainABI records compiler ID/version and pointer size.
Platform, architecture, configuration, runtime variant, and descriptor build
inputs are explicit. Publication is a build artifact operation, without startup
loading, content cooking, or a binary compatibility guarantee.

The repository-owned [fixture](../../Engine/Tests/Fixtures/Plugins/README.md)
qualifies a reflected asset, a dependent plugin subclass, an Editor companion,
and a content-only plugin with exported calls across plugin DLLs.

## Plugin Content, Shaders, And Configuration

`GetActivePluginResources` exposes the resource snapshot frozen by
`PreparePluginStartup`. `ResolvePluginResources` prepares a value-only candidate;
invalid/missing resource directories, canonical escapes, or unselected required
dependencies preserve the caller's previous output. Declared Content, Shaders
and Configs paths are relative to the plugin descriptor directory and must name
contained directories. Resource paths may change physically without changing
logical plugin/module/asset identity.

Content uses `/Plugins/<Id>/`, is auto-scanned and read-only, and joins project
mount definitions before the existing single publication transaction. The
combined default/project/plugin definitions receive the existing collision,
canonical overlap and containment validation. `/Game/` can reference every
additional mount. A plugin mount can reference `/Engine/` and the content roots
of its required dependency closure, including transitive dependencies through
content-free plugins; it cannot reference arbitrary sibling plugins or `/Game/`.
Catalog discovery and direct package loading use those same mount definitions.

`Shaders` names the actual Slang source directory, without an implicit suffix.
ShaderBuild registers `/Plugins/<Id>/` before its initialization freeze. The
existing mounted-source resolver, source/dependency hashes and reload generation
control compiler/cache invalidation. Mount-specific derived outputs live under
the host's DerivedDataCache, never the plugin source directory. Game consumes
cooked Shader data; plugin Shader staging remains Stage 6 work.

`Configs` names a directory containing optional `Project.yaml`, `DurinEditor.yaml`
and `DurinGame.yaml` defaults. Project settings readers merge Engine's
`Configs/Project.yaml`, enabled plugin Project.yaml files in resolved dependency
order, then the project's Configs/Project.yaml. `LoadProjectConfig` only applies
those defaults to the active project's settings path; unrelated standalone stores
retain their own inputs. Module application settings merge the deployed immutable
`Templates/TP_<RuntimeVariant>.yaml`, ordered plugin `<RuntimeVariant>.yaml`
files, the project's matching config, and the existing saved user application
config last. Bootstrap logging retains its earlier saved-config initialization;
subsequent module consumers see the final merged document. Existing command-line
policies remain with their consumers.

`LoadConfigLayers` merges maps recursively; a later scalar or sequence replaces
the earlier value. Empty scalars/sequences are explicit overrides. Missing files
are optional; malformed YAML or I/O failure preserves the previous output and
reports the source file. No merge writes defaults into authored project files.
Game/default-Level and AssetManager readers consume effective project settings;
editing/saving continues to operate on the raw project file, preserving unrelated
fields and preventing inherited defaults from being materialized on disk.

Unavailable plugin types fail package loading before publication, with an
enable/restart/provider diagnostic. Serialized plugin dependency mounts are
checked before removed-field filtering, so unresolved plugin references cannot
be silently discarded as ordinary schema evolution. Failed loads retain source
bytes and publish no resident package; ordinary saves enforce read-only mounts.

## Editor Plugin Management

Open **Edit > Plugins** to review plugin names and next-start checkboxes. Search
matches the display name or stable plugin ID. A changed running-versus-pending
state is marked **Enabled after restart** or **Disabled after restart**. Hover
a name for its ID/version, running state and compatibility/build diagnostics.
Long names and dependency warnings wrap within each row. The list shows plugin
and requested-selection counts, distinguishes an empty catalog from no search
matches, and leaves fixed space below it for status and Save/Cancel/Restart.

Changes stay local until **Save**. Checkboxes record requests independently of
effective enablement: a checked plugin with an unchecked required dependency
stays checked and displays **Inactive: requires ...**. It can be saved and remains
inactive after restart until that dependency is enabled. Disabling a prerequisite
leaves consumer checkboxes unchanged. Checkbox actions never toggle other plugins.
Opening materializes the initial effective defaults as independent checkbox
values without dirtying or writing the descriptor; the next save persists those
values so unchecking a consumer cannot prune another checked plugin.
**Cancel** restores the last saved checkbox snapshot using cached metadata;
it neither scans nor reads or writes project/plugin files. **Refresh**, beside
Search, rescans plugin directories and reloads descriptor/build state from disk,
discarding unsaved checkbox edits as its tooltip explains. Closing and reopening
also reloads from disk. Failed refreshes retain the previous usable session and
display the error. There are no cascade controls or review modals.

Saving validates the next-start dependency/resource/mount closure and uses the
existing project-edit lease and atomic descriptor replacement. It rejects a
descriptor changed since the window loaded, unrelated pending project edits,
read-only files and ownership conflicts without partially replacing the file.
Only plugin overrides change; active modules, mounts, Shader roots and effective
configuration retain their startup snapshot. A missing or incompatible build
does not prevent saving a valid selection, but blocks restart with a build
diagnostic. No management action builds binaries or republishes an inventory.

Saved selections differing from the running selection display a restart-required state.
**Restart editor...** rechecks the saved descriptor and artifact admission,
then uses the normal all-document exit workflow. Pending document operations
and failed saves retain the editor; Cancel leaves documents open and plugin
settings saved for a later restart. Relaunch is deferred until shutdown and
preserves the explicit project path. Compatible override-only changes require
no rebuild. Build and restart remain separate user actions.

`FPluginEditSession` owns the value-only draft and cached metadata/diagnostics.
Checkbox changes only evaluate the in-memory graph; they do not read files,
hash descriptors, resolve the workspace or admit binaries. Opening/refreshing
captures metadata and build availability; save and restart revalidate disk state
through the shared startup services. The project manager owns persistence and
`MainFrame` owns presentation and restart routing.

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
