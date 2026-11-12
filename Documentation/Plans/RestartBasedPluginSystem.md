# Restart-Based Plugin System Plan

Summary: Add project-selected source and content plugins with deterministic build, startup, cooking, and restart-required enablement.

Last reviewed: 2026-11-12

Status: Active
Completed:

## Current Status

Stages 0, 1, 1.1, 2, and 3 are complete for the selected Win64 source-build
milestone. Projects now admits and freezes the native plugin set before mount/
renderer setup; Editor and Game start its modules at the existing host phases.
Core enforces frozen names/paths and retains plugin code through process exit.
Stage 4 is next. Plugin content/shader/configuration consumption, management UI,
and cooked plugin deployment remain Stages 4-7 work. Native macOS startup
qualification remains an explicit outstanding platform gate.

Stage 3 admission checkpoint (2026-11-12, not accepted):
- Added read-only native inventory admission and a focused regression covering
  success, stale selection, incompatible identity, missing file, path escape,
  wrong owner and unselected module; failure preserves the previous output.
- Changed-document and all-plan validation passed. Outside-sandbox
  `test PluginResolutionTests` passed 8/8 on Win64-Debug-DurinEditor with the
  final admission source and fixture changes on base `caede8f7f6a5ab92aa29c3d73e933b07fde90ca6`. Build log:
  `Build/.agent-state/logs/20261112-152152-509746-14984-cmake.log`;
  native log:
  `Build/.agent-state/logs/20261112-152207-054854-14984-PluginResolutionTests.log`.
  The earlier sandbox C1902 failure was resolved by the authorized external run.
  This validates the standalone admission API; host integration and process
  qualification remain open. No Stage 3 acceptance checkbox is closed.

Stage 3 acceptance (2026-11-12, Win64 Debug, windows-msvc-x64):

- Tested base `2e508c939207c993e165839443290cd8d5bd570f` plus the final
  Stage 3 source/fixture changes. Input SHA-256 hashes are retained under ignored
  `Build/PluginStage3/final-inputs.json`; later documentation edits do not alter
  these inputs. Temporary fixture workspace membership was restored.
- Both runtime variants passed `build all` with the fixture and again with
  ordinary Engine/Sandbox/RoadWeaver membership. Final ordinary build logs:
  `Build/.agent-state/logs/20261112-163825-838185-31252-cmake.log` and
  `Build/.agent-state/logs/20261112-163912-963944-3016-cmake.log`.
  This covers the shared Core layout and all direct project API consumers.
- 46 independent fixture processes passed: 13 admission/startup probes per
  variant, four persisted-edit/rebuild processes per variant, five complete
  Editor/Game host runs, six later-provider/stale/missing-file host failures,
  and one existing no-plugin staged Game run. Normal hosts exit after three
  ticks with code 0; expected failures exit 1 and record bounded teardown.
  Browser starts no plugins. Runtime reflection is available at its phase.
  Editor starts Runtime, dependent EngineReady, then EditorReady modules; Game
  omits the Editor module. Shutdown reverses completed startup order.
- Probes reject all six incompatible native identity fields, stale selection,
  missing inventory/DLL/runtime dependency, unknown explicit registration,
  early name-based loading and runtime unload. Provider failure cleans its
  partial state once; later-provider failure also stops the earlier provider.
  Saving disabled overrides leaves current module membership and mount revision
  unchanged; the next process rejects stale artifacts, then observes no native
  plugins after rebuild despite stale DLL files. Re-enable/build restores startup.
- Raw case results, traces and logs are in `Build/PluginStage3/`:
  `DurinEditor-probes.json`, `DurinGame-probes.json`, both `*-cycle.json`,
  both `*-hosts.json`, and `host-extra.json`. The reproduction scripts are
  `qualify.py`, `cycle.py`, `hosts.py`, and `extra.py` there. The tracked
  fixture/probe lives under `Engine/Tests/Fixtures/Plugins/PluginFixture`.
- Final source regression: PluginResolutionTests 8/8, CoreConcurrencyTests
  `FModule*` 11/11 (including owned worker drain and failure retention), and
  LaunchProcessBoundaryTests 4/4. Final native logs use the
  `20261112-162755`, `20261112-162805`, and `20261112-162810` prefixes in
  `Build/.agent-state/logs/`. Broader affected selection is bounded by both
  all-builds, lifecycle regressions, and real host/DLL process coverage; GPU
  rendering behavior and the native test framework are unchanged.
- A supplemental three-case LaunchStartupCharacterizationTests selection built,
  but did not execute: its corrected characterization-mode invocation was
  blocked before creation by disconnected automatic approval. It contributes
  no passing evidence. Corresponding Browser/ordinary-host behavior is covered
  by the direct host cases above; no whole-suite claim is made.
- Lasting startup and source/staged boundaries are documented in
  [Plugins](../Workspace/Plugins.md#frozen-native-startup) and
  [Runtime lifecycle](../Runtime/Application/RuntimeLifecycle.md).
  Missing/stale artifacts never trigger an implicit build. The existing Browser
  relaunch/unsaved-work owner is retained. Plugin-bearing staged deployment
  requires Stage 6; no-plugin staged startup remains source-independent.
  Native macOS execution remains unqualified.

The 2026-11-12 convergence keeps these stages unchanged: typed descriptor fields
are the persistent authority, the manager owns both editable and active state,
and public bool operations report failures directly. Discovery treats a plugin
descriptor directory as a traversal boundary, matching UE; descriptors below
that boundary are ignored rather than audited. Module files are consumed only
for selected, host/platform-eligible plugins. Selection exposes typed values
alongside its canonical artifact projection.

Convergence acceptance (2026-11-12, Win64 Debug, windows-msvc-x64):

- Tested base `55ee89820` plus the convergence source/fixture changes. Final
  source hashes are retained in ignored
  `Build/PluginStage1_1/convergence-inputs.json`; documentation-only edits do not
  change these inputs. No stage was added and Stages 2-7 remain open.
- `build all` passed for Win64-Debug-DurinEditor and Win64-Debug-DurinGame after
  the public interface changes. Final build logs are
  `Build/.agent-state/logs/20261112-121153-586846-28620-cmake.log` and
  `Build/.agent-state/logs/20261112-121255-360278-39460-cmake.log`.
- PluginResolutionTests passed 6/6, including 49 shared plugin cases and the
  typed edit/save, unchanged-override, active-state isolation, and typed-result
  projection checks. Python descriptor/plugin/workspace/configuration/document
  regressions passed 213 tests with 2 symbolic-link privilege skips.
- CoreFileSystemTests passed 57 with 2 symbolic-link privilege skips.
  LaunchProcessBoundaryTests passed 4/4. The selected bounded-exit,
  missing-explicit-project, automated-history, and empty-default-Level startup
  characterization cases passed 4/4. Reports use the `convergence-` prefix under
  `Build/PluginStage1_1`; process-boundary coverage precedes only internal
  manager/consumed-path refinements and remains applicable to its unchanged
  relaunch and lease behavior.
- The Game smoke refreshed staged binaries from the final Game build, reused
  the existing complete Cook resources and EmptyGame fixture, and exited after
  3 ticks with exit code 0 and no error/fatal/validation diagnostics. Log:
  `Build/PluginStage1_1/convergence-game.log`.
- Document and all-plan validation passed. Native macOS and privileged symbolic
  link/junction execution remain unqualified; plugin native loading is still
  outside this acceptance.

Stage 1 acceptance (2026-11-11, Win64 Debug, windows-msvc-x64):

- Tested base `170364896642edb210e0fe3ce60ed06a2e010dee` plus the Stage 1
  working-tree changes. Final relevant inputs are recorded with SHA-256 hashes
  under ignored `Build/PluginStage1/inputs.json`. Subsequent status/documentation
  edits do not change the validated source, fixtures, schemas, or environment.
- PluginResolutionTests passed 2/2 native tests, including all 46 shared
  canonical-selection/error fixtures. Editor and Game are resolver query inputs
  within this Win64 Editor native executable; this is not Game-process loading
  qualification. Raw native output: `Build/PluginStage1/native.log`.
- Python plugin/descriptor/workspace/scaffolding/configuration regression:
  156 passed, 1 skipped. Includes both readers, build/selection schemas, existing
  Engine/Sandbox/RoadWeaver roots, relocated/reordered inputs, fingerprint
  invalidation, and a Runtime plugin consuming the real Engine descriptor graph
  in Editor/Game selections. Report: `Build/PluginStage1/python.xml`.
- CoreFileSystemTests: 57 passed, 2 skipped, including active-project override
  capture and invalid-override rejection. Raw output:
  `Build/PluginStage1/core-filesystem.log`. All three skipped tests require
  symbolic-link creation privileges unavailable on this host; lexical path
  escape and ordinary ownership validation passed. Native macOS execution and
  privileged link/junction execution remain unqualified.
- DurinLauncher, DurinAssetTool, Sandbox, and RoadWeaverEditor built successfully
  with Win64-Debug-DurinEditor. Their relevant shared project API remained
  unchanged during the final resolver-only refinements. Build logs are retained
  under `Build/.agent-state/logs/20261111-161439-532335-36040-cmake.log`,
  `20261111-161628-858099-46412-cmake.log`,
  `20261111-161753-947340-43172-cmake.log`, and
  `20261111-161827-170599-26960-cmake.log` in that directory.
- Affected-test analysis expands to all because Engine's project descriptor and
  native-test registration changed. Bounded source-plugin and Core tests plus
  direct shared-API consumers cover Stage 1; no generated plugin target
  selection/startup is wired yet. The explicit all-build gate stays in Stage 7.
- Lasting Stage 1 rules, phase insertion points, and schema ownership are in
  [Plugin metadata and selection](../Workspace/Plugins.md). Documentation and
  all-plan validators pass; final verification receipts are retained under
  `Build/PluginStage1/documentation.log`.

Existing foundations include `FModuleManager`, `.dmodule` generation, DHT,
extension content mounts, initialization-time shader mounts, modular features,
and World subsystem registration. They are reusable mechanisms, not evidence
that plugin discovery or enablement already works.

## Goal

A project can enable a plugin containing Runtime and Editor modules, content,
shaders, and configuration. Build tools and the executable agree on the selected
set. Enablement edits affect the next process; the current process keeps its
original plugin set. A cooked Game runs with only its declared plugin closure.

## Selected Scope And Decisions

- Support repository/source builds first, on the engine's supported platforms.
  First executable qualification is Win64; qualify platform-independent metadata
  everywhere covered by existing CI. macOS native execution remains an explicit
  outstanding gate until measured there.
- Discover `Engine/Plugins` and `<ActiveProject>/Plugins` recursively by
  `.dplugin`; stop descending at each plugin root. Reject case-insensitive
  plugin/module identity collisions; ignore descriptors beneath a plugin root. Do not provide
  implicit project-over-engine replacement or arbitrary search paths initially.
- A plugin is an optional distribution/enablement container. A module remains
  the compilation, reflection, and native loading unit. Existing project modules
  continue to work without becoming plugins.
- Introduce a small runtime `Projects` module depending on `Core`, with no
  Engine, Renderer, or editor dependencies. It owns plugin descriptors,
  project/plugin descriptors, selection, and manifest admission. Keep generic
  loading in Core's `FModuleManager`; Core must not depend back on Projects.
  Stage 1 retained ordinary-project management in Core as an interim boundary.
  Stage 1.1 moves descriptor/management policy to Projects while preserving
  Core's low-level path context and mount/module mechanisms.
- `.dplugin` owns schema version, stable ID, display/version metadata, contained
  module paths and host/platform eligibility, required plugin dependencies,
  optional content/shader/config roots, and module startup phases. `.dmodule`
  remains authoritative for link dependencies and reflection headers. Version
  metadata does not promise cross-version native compatibility.
- `.dproject` owns explicit plugin enable/disable overrides. Unmentioned plugins
  are disabled unless required by the enabled closure. Explicit disable wins
  over dependency activation by producing an error, never by silently breaking
  the dependency. Do not add optional dependency/version solving in this scope.
- Use versioned schemas, common fixtures, and canonical resolved manifests to
  keep Python build-time and C++ startup implementations equivalent. Do not
  require embedding Python in Game or sharing implementation across languages.
- Freeze a process's plugin identity/selection at project admission, before
  plugin-dependent content or shader consumption. Project Browser can remain
  usable without a selected project. Switching to a different plugin set
  requires restart; do not retrofit project-switch hot loading.
- Native plugins target the exact engine build identity, platform, architecture,
  configuration, runtime variant, and toolchain/ABI profile. Validate before
  loading plugin libraries. Source plugins may require configure/build before
  restart; UI must distinguish this from restart alone.
- Content-only plugins are supported. Plugin content uses stable virtual roots
  such as `/Plugins/Water/`; physical relocation must not change asset identity.
- Initial phases are provider registration before project content, engine-ready,
  and editor-ready. Shader root declaration is descriptor-driven and may need
  to occur earlier than provider callbacks. Phase names and exact hook mapping
  are frozen in Stage 1 against the actual startup sequence.
- The active set is immutable. No live enable/disable, DLL unloading, hot reload,
  reflected-type replacement, object migration, or live mount changes are added.
  Normal process-exit cleanup and partial-startup cleanup are still required.
- Binary-only third-party SDK distribution, marketplace/install/update services,
  sandboxing, general Renderer extension APIs, and Water implementation are out
  of scope. Water motivates the integration fixture but is not a prerequisite.

## Ownership And Related Work

| Area | Primary owner | Integration owners |
| --- | --- | --- |
| Discovery, descriptors, selection, manifest admission | New `Projects` runtime module | Core project/path primitives; DevTool and DHT Python descriptor readers |
| Ordinary-project descriptors, current project, status queries, edit-session ownership | Projects after Stage 1.1 | Core mechanisms; Launch lifecycle; editor history/restart policy |
| Module graph, reflection, generated targets | DurinHeaderTool and CMake | DurinDevTool; `.dproject`/`.dmodule` schemas |
| Process/project startup and exit | Launch, Engine, DurinEd | Projects; Core ModuleManager |
| Content mounts, catalogs, packages | Core, AssetRegistry, Engine | Projects supplies immutable mount definitions |
| Shader roots, source builds, cooked programs | ShaderBuild, RenderCore, Engine | Projects supplies roots and selection identity |
| Plugin configuration | Core configuration mechanisms | Projects defines ordered plugin inputs; Engine applies settings |
| Enablement UI | DurinEd, MainFrame | Projects; existing project-edit ownership and transactions |
| Cook and deploy closure | Engine, DurinDevTool | DHT/CMake binary inventory; ShaderBuild |

Coordinate startup hooks with the active
[Runtime Startup And Scene Activation plan](RuntimeStartupAndSceneActivation.md).
Its existing startup/readiness ownership must not be replaced by a second
plugin-specific startup state machine. The implemented
[Runtime lifecycle](../Runtime/Application/RuntimeLifecycle.md) remains the
starting contract until that plan changes it.

The [DurinEditor SDK Packaging plan](DurinEditorSdkPackaging.md) owns installed
SDK layout, imported engine targets, and external C++ consumer distribution.
This plan must expose reusable identity/inventory metadata without making the
source-plugin milestone depend on completing that SDK plan.

Follow [Workspace and projects](../Workspace/WorkspaceProjects.md),
[Build system](../Development/Build/BuildSystem.md),
[Build and run](../Agents/BuildAndRun.md), and
[Testing](../Agents/Testing.md) when selecting implementation coverage.

## Implementation Stages

### Stage 0: Define scope and stage acceptance

Dependencies: None.

Outcome: A reviewable implementation sequence with explicit restart semantics.

- [x] Select source/content plugin scope and deferred capabilities.
- [x] Assign descriptor, module, startup, asset, shader, and UI ownership.
- [x] Define dependencies and observable acceptance gates for Stages 1-7.

Acceptance: This plan states the selected behavior and existing-plan boundaries;
completion here does not claim an implemented plugin subsystem.

### Stage 1: Implement descriptors and deterministic plugin resolution

Dependencies: Stage 0.

Modules/tools: New Projects; Core project descriptor integration;
DurinHeaderTool schemas/readers; DurinDevTool descriptor/workspace services.

Outcome: A project resolves to one validated, explainable plugin selection.

- [x] Register Projects in Engine's project/module graph without introducing
  Core-to-Projects or Projects-to-Engine dependencies.
- [x] Add `.dplugin` schema and project overrides; preserve existing descriptors
  with no Plugins field. Define absent versus explicitly empty values.
- [x] Implement discovery, containment checks, duplicate detection, host/platform
  filtering, required dependency closure, cycle checks, and stable ordering.
- [x] Validate cross-plugin `.dmodule` references against declared plugin
  dependencies. Reject dependencies on disabled/ineligible plugin modules and
  Runtime-to-Editor links, including transitive violations.
- [x] Freeze phase ordering and its mapping to startup hooks, including a rule
  rejecting an earlier-phase consumer of a later-phase provider.
- [x] Define canonical selection/build manifests with schema version, logical
  owners, reasons for activation, inputs/fingerprints, and binary identity.
  Separate machine-local resolved paths from portable identity.

Acceptance gate:

- [x] Shared fixtures produce equivalent canonical selections in Python and C++:
  empty project, content-only plugin, required closure, Editor/Game selection,
  explicit-disable conflict, cycle, duplicate ID/module, missing descriptor,
  path escape, invalid phase edge, and unknown schema version.
- [x] Existing Engine, Sandbox, and RoadWeaver project descriptors retain their
  existing effective module roots when no plugins are enabled.
- [x] Errors identify the descriptor and complete failing dependency chain.

### Stage 1.1: Unify project management and descriptor ownership

Dependencies: Stage 1.

Status: Completed on 2026-11-12.

Modules/tools: Projects, Core, Launch, Engine, DurinEd, MainFrame; all direct
project API consumers, including DurinAssetTool and project/test targets in
Durin.dworkspace; DHT and DevTool descriptor readers and plan-context tooling.

Outcome: Projects manages ordinary projects and plugins through one descriptor
model and an explicit current-project service, without introducing live project
switching or changing existing startup/readiness ownership.

#### UE Alignment And Selected Boundaries

Use [UE Projects](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Projects)
and [IProjectManager](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Projects/IProjectManager)
as naming and responsibility references. Match principal concepts and observable
semantics, including bool results, status output parameters, and explicit failure
reason outputs where UE uses them. Retain Durin's `.dproject`/`.dmodule` formats
and native string/value vocabulary; internal helpers may use structured errors.
There is no requirement to retain `std::expected` or Durin error enums on these
principal public APIs. This alignment does not promise UE source/binary compatibility.

| Surface | Selected Stage 1.1 behavior |
| --- | --- |
| FProjectDescriptor | Typed project metadata, shared read/load/validation, module roots, plugin references, and mount declarations; independent of current-project mutation |
| IProjectManager::Get | Single service entry for ordinary-project management; Projects owns its implementation |
| LoadProjectFile / GetCurrentProject | Load and validate a descriptor, then publish the current descriptor; separate descriptor data from derived paths/session state |
| QueryStatusForProject / QueryStatusForCurrentProject | Read-only project status using the same descriptor validation; do not load modules, change current project, or equate valid metadata with compatible binaries |
| LoadModulesForProject | Route existing ordinary-project module loading through the manager at the existing host startup point; Core still executes native loading |
| SaveCurrentProjectToDisk / IsCurrentProjectDirty | Explicit descriptor editing/save contract, preserving unaffected fields and using atomic publication; failed save retains the dirty state |
| SetPluginEnabled / RemovePluginReference | Edit requested project configuration only; removing an override differs from writing false; no change to the process's admitted active plugin set |

The descriptor model may share module eligibility/loading-phase vocabulary with
plugin metadata, following UE's FModuleDescriptor concept. Do not import every
UE host type, phase, settings API, or IPluginManager method without a Durin need.
Plugin module phase execution and native compatibility admission remain Stage 3;
dependency-aware plugin UI and restart handling remain Stage 5.

- Projects owns project descriptors, status queries, current-project context,
  descriptor edit state, and project edit-session ownership. Keep descriptor
  inspection usable without initializing Engine, Renderer, editor UI, or Python.
- Core owns JSON/file/process primitives, generic FModuleManager, FPaths queries,
  immutable mount registry, and containment/dependency validation. Projects
  supplies validated path context and mount definitions; Core must not interpret
  `.dproject` fields or acquire a dependency on Projects.
- Projects may own recent-project record/persistence values. DurinEd/MainFrame
  owns interactive history ordering, auto-restore choice, and unsaved-work policy;
  Launch owns startup argument selection and deferred process relaunch. Game
  and command-line tools must not accidentally inherit interactive editor policy.
- Build tools retain a Python representation of the same contract. Share schemas
  and fixtures with C++; do not embed Python in the runtime or require C++ tools
  merely to inspect project metadata.

#### Implementation Tasks

- [x] Add the typed project descriptor and UE-aligned project manager interface,
  separating persistent descriptor state from derived context and session policy.
- [x] Move applicable APIs/data from Core's Project/ProjectError/ProjectHistory
  facilities to their selected owners. Migrate all source and test consumers in
  every workspace project; add explicit `.dmodule` dependencies where required.
  Do not leave a permanent Core facade depending back on Projects.
- [x] Make current-project admission, status/history inspection, and plugin
  resolution reuse the same project reader. Remove duplicate `.dproject` field
  interpretation from Core path/mount code and the native plugin resolver.
- [x] Preserve one validated project/mount snapshot: failed inspection/admission
  cannot publish partial current-project, path, or mount state. Retain the
  existing immutable mount lifetime and restart-required project switching.
- [x] Define descriptor edit/save semantics, unknown/schema-field handling,
  project ownership checks, and failure recovery. Keep requested descriptor
  edits distinct from the active process selection and preserve unrelated data.
- [x] Keep project module load order, reflected-provider readiness, initial
  content scan, and shutdown/relaunch placement equivalent to existing behavior;
  coordinate with RuntimeStartupAndSceneActivation rather than adding a new loop.
- [x] Align Python/C++ absent-versus-empty behavior: omitted BaseModules uses
  ModuleDirs; explicit empty BaseModules means no base roots; omitted/empty
  Plugins means no explicit overrides. Preserve existing checked-in project roots.
- [x] Extend plan-context stage discovery to recognize dotted numbers so Stage
  1.1 can be selected independently and its exact provenance is preserved.
- [x] Publish lasting project-management ownership/API rules in Workspace and
  relevant lifecycle documents; revise the interim boundary recorded by Stage 1.

#### Acceptance Gate

- Initial tooling checkpoint: focused Python regression covers dotted-stage
  identifiers (including distinct 1.1/1.10), completed-parent task boundaries,
  JSON/Markdown context, and omitted/empty BaseModules in DHT/DevTool. Raw report:
  `Build/PluginStage1_1/python.xml`: 172 passed, 2 skipped because directory
  symbolic-link creation is unavailable. Tested base `3563d2bbd` plus this
  checkpoint's working-tree changes. The repository's plan-context command independently
  selects Stage 1.1; captured JSON is `Build/PluginStage1_1/context.json`.
  Document and all-plan validation passed. This is not native project-manager acceptance;
  the shared native descriptor fixtures, ownership migration, process smokes,
  and Editor/Game all-build gates below remain open.

- Second checkpoint (2026-11-12): PluginResolutionTests passed 3/3, including
  the shared 46 plugin fixtures and new descriptor/status-query/atomic-save/
  failed-save-dirty-state coverage. CoreFileSystemTests passed 57 with 2
  symbolic-link privilege skips. Raw logs:
  `Build/.agent-state/logs/20261112-103551-552573-20308-PluginResolutionTests.log`
  and `Build/.agent-state/logs/20261112-103242-938457-33672-CoreFileSystemTests.log`.
  DurinLauncher also built successfully after the DLL export/header migration;
  final build output: `Build/.agent-state/logs/20261112-103552-940354-28780-cmake.log`.
  This checkpoint does not close the transactional admission, shared schema,
  lifecycle-policy, or Editor/Game all-build acceptance gates.

#### Final Acceptance (2026-11-12)

- Tested base `cfd97de477086d2cd22462596cc90cee1e5900bc` plus this stage's final
  working-tree inputs. Source/fixture hashes and configuration/artifact identities
  are retained under ignored `Build/PluginStage1_1/final-inputs.json`.
  Final documentation/status edits do not alter the relevant validated inputs.
- Win64 Debug Editor and Game `all` builds passed, including the standalone
  asset-tool consumer. Final logs in `Build/.agent-state/logs/`:
  `20261112-110554-437143-31484-cmake.log` and
  `20261112-110616-063599-17068-cmake.log`.
- Python regression: 210 passed, 2 directory-symlink privilege skips;
  `Build/PluginStage1_1/final-python.xml`. Native project/manager tests: 5/5,
  including 18 common project fixtures and all 46 unchanged plugin fixtures;
  `Build/PluginStage1_1/native-projects.xml`. Core file/project/mount regression:
  57 passed, 2 symlink privilege skips; `Build/PluginStage1_1/native-core.xml`.
- Launch process boundaries: 4/4, including exclusive project ownership and
  consume-once deferred relaunch arguments; `Build/PluginStage1_1/native-launch.xml`.
  Selected characterization: Browser bounded exit, missing explicit project,
  automated history preservation, and empty-default-level startup all passed
  (4/4); `Build/PluginStage1_1/native-startup.xml`. Unrelated command-handler
  characterization was not expanded into this selection.
- Sandbox and RoadWeaver Editor each completed 3 ticks and clean shutdown, with
  no error/fatal/Vulkan validation diagnostics. Logs under the same log directory:
  `20261112-110620-118661-28068-DurinEditor.log` and
  `20261112-110624-584498-34824-DurinEditor.log`.
- Game completed 3 ticks and clean shutdown from an isolated staged runtime with
  valid cooked default material, ShaderLibrary and application configuration;
  `Build/PluginStage1_1/staged-game.log`. Cook retained one package and published
  a validated manifest-last generation under `Build/PluginStage1_1/CookedGame`.
  An initial uncooked Game smoke returned zero but logged missing deployment
  resources and a Vulkan acquisition diagnostic; it was not accepted as a pass.
  Repeating with complete staged inputs produced no error/fatal/validation
  diagnostics. No RHI behavior or user asset was changed to mask that failure.
- Manager queries preserve actual mount-storage addresses as well as revision,
  path/context and ownership. Failed loads retain the active snapshot; failed
  atomic saves retain pending dirty state. Principal APIs now match UE bool and
  output-parameter conventions, with native strings and private structured errors.
- Core/Projects dependency guards pass; Projects remains Core-only. Native
  descriptor tests run without Engine/editor/Python. Dotted-stage selection and
  existing project roots remain covered. Affected-test analysis expands to all
  due to declaration changes; bounded behavior tests plus both explicit all-build
  gates cover this migration. GPU performance qualification is not claimed.
- Native execution is qualified on this Win64 host only. macOS execution and
  privileged symlink/junction paths remain unqualified; these do not imply a
  completed cross-platform Stage 7 gate. Document and all-plan validation pass;
  receipts are in `Build/PluginStage1_1/final-documentation.log`.

- [x] Shared fixtures establish C++/Python agreement on ordinary project fields,
  malformed/duplicate fields, omitted/empty values, plugin references, mounts,
  and Editor/Game root selection. Stage 1 plugin canonical fixtures remain valid
  unless an intentional, documented correction requires updating expectations.
- [x] Status queries leave current context, paths, mounts, module state, and
  history unchanged. Descriptor edits/atomic save preserve unrelated fields;
  failed writes preserve disk contents and pending edit state. Explicit false
  and removed plugin overrides remain observably different after reload.
- [x] Project Browser, explicit project opening, history/auto-restore, edit-session
  exclusion, and deferred relaunch retain their existing contracts. Game/tool
  project admission does not apply interactive history policy. Failed admission
  and normal exit leave no partial context or leaked ownership.
- [x] Engine, Sandbox, and RoadWeaver retain expected project roots and startup
  behavior; non-project Browser startup remains supported. Run focused project,
  plugin, mount, startup/history/relaunch regression coverage and the corresponding
  bounded process smokes, with evidence and platform skips recorded explicitly.
- [x] Dependency checks prove Core has no Projects/Engine/editor dependency and
  Projects remains Core-only. Ordinary project inspection works in a minimal
  native consumer. Dotted-stage plan-context discovery has focused regression
  coverage and selects Stage 1.1 independently of completed Stage 1.
- [x] Because public project types move across DLL boundaries and many owners
  gain direct dependencies, run `all` builds for Editor and Game at this stage's
  final acceptance. This gate covers the migration's ABI/build-graph risk; reuse
  unchanged passing results in Stage 7 rather than rerunning by commit identity.
- [x] Plan state and lasting documentation match the implemented boundary;
  document and all-plan validation pass. Only then make Stage 2 executable.

### Stage 2: Integrate plugin modules into build and reflection

Dependencies: Stages 1 and 1.1.

Modules/tools: DurinHeaderTool; CMake workspace/project/target generation;
DurinDevTool configure/build services; CoreDObject integration fixtures.

Outcome: Enabled plugin modules compile and expose reflected types through the
existing toolchain, without hand-editing Engine's ModuleDirs for each plugin.

- [x] Feed resolved plugin modules to CMake and DHT using the selected workspace
  membership; preserve module ownership independently from project ownership.
- [x] Generate export headers, reflection inputs, targets, include paths, and
  dependencies through existing module mechanisms.
- [x] Namespace generated state and outputs by plugin ownership and relevant
  target/configuration inputs. Reconfigure when discovery or enablement changes.
- [x] Produce a build manifest containing exact binary paths, build identity,
  selection fingerprint, and required native runtime dependencies.
- [x] Create a small repository-owned PluginFixture and a consumer fixture:
  Runtime module with a reflected component/asset, Editor companion, dependent
  plugin, and content-only plugin. Keep tests/fixtures under their owning test
  roots or ignored Build outputs; do not depend on Water development.

Acceptance gate:

- [x] Enabled fixture builds in DurinEditor and DurinGame; Editor companion is
  absent from the Game graph. Its reflected headers and API exports work across
  the fixture/consumer DLL boundary.
- [x] Disable the fixture, reconfigure, and prove its targets are absent and
  stale DLLs do not re-enter the selection. Re-enable and verify regeneration.
- [x] Configuration/variant outputs do not overwrite one another; no-plugin
  project builds remain unchanged in effective membership.

Stage 2 acceptance (2026-11-12, windows-msvc-x64):

- Tested base `e122ede14` plus this stage's final source/fixture inputs. SHA-256
  hashes and preset identities are retained under ignored
  `Build/PluginStage2/final-inputs.json`; final documentation edits do not affect
  those inputs. The repository fixture lives under
  `Engine/Tests/Fixtures/Plugins/PluginFixture` and was copied into ignored Build
  storage for temporary workspace registration. Durin.dworkspace was restored.
- The fixture inventory and cross-DLL consumer built and ran in
  Win64-Debug-DurinEditor, Win64-Debug-DurinGame, and
  Win64-Release-DurinEditor. Runtime and dependent plugin asset classes share
  reflected inheritance across DLLs. The Editor companion is absent from the
  Game graph. Content-only selection requires no native binary. Probes exited 0;
  manifests satisfy the shared inventory schema and every declared file exists.
- Disabling both overrides automatically reconfigured the Editor graph, removed
  the plugin targets, and published an empty binary list despite existing DLLs.
  Re-enabling regenerated targets and the consumer ran successfully. Final
  receipts and probe logs are in `Build/PluginStage2/acceptance-final.json` and
  the same directory. The first isolation check used a pre-reenable Debug hash
  baseline and detected a normal re-link; after establishing a post-reenable
  baseline, a separate Release build left every Debug DLL hash unchanged.
- Ordinary workspace `build all` passed for both Win64 Debug variants. The
  fixture targets and inventory targets disappear when temporary membership is
  removed; ordinary Engine/Sandbox/RoadWeaver targets remain. Final logs:
  `Build/.agent-state/logs/20261112-142153-937431-33440-cmake.log` and
  `Build/.agent-state/logs/20261112-142358-326193-39336-cmake.log`. Release fixture
  build log: `Build/.agent-state/logs/20261112-140140-758751-6840-cmake.log`.
- Full DHT Python tests plus DevTool workspace/build-config/document regressions:
  628 passed, 2 symbolic-link privilege skips. Native PluginResolutionTests:
  7/7 passed, including foreign-workspace root isolation; CoreObjectTests:
  181/181 passed. Reports: `Build/PluginStage2/python.xml`, `native.xml`, and
  `reflection.xml`. Affected selection expands to all because CMake infrastructure
  and unregistered fixture inputs changed. Both all-builds, direct native
  consumers, real DLL fixture execution, and the full DHT suite cover this stage;
  no native test execution framework, GPU behavior, or host startup changed.
- Principal plugin modules use Shared linkage with generated API headers;
  configuration-independent DHT data retains platform/variant/plugin ownership,
  while native outputs also separate configuration. Implemented rules are in
  [Plugin source build integration](../Workspace/Plugins.md#source-build-integration).
  Document and all-plan validation passed. Native macOS remains unqualified;
  runtime admission/loading and complete cooked deployment remain later stages.

### Stage 3: Admit and start the frozen plugin set

Dependencies: Stage 2.

Modules: Projects, Launch, Engine, DurinEd; narrowly scoped Core ModuleManager
and CoreDObject integration changes only where existing hooks are insufficient.

Outcome: Editor and Game load exactly the admitted plugin module set before
their consumers run, and report actionable startup failures.

- [x] Resolve project plugin inputs early enough to prepare immutable roots,
  validate build compatibility, and freeze selection before consumption.
- [x] Check manifests against current descriptors and binary identities before
  native loading. Missing or stale source-plugin artifacts report configure/
  build requirements; never build implicitly inside startup.
- [x] Register exact module paths and execute dependency-aware phase order.
  Ensure plugin reflection/providers are ready before catalog/package loading
  and before World subsystem membership is captured.
- [x] Enforce admitted module membership at loading boundaries so a disabled
  plugin cannot be loaded by guessing its name or by stale output discovery.
- [x] Keep Project Browser admission independent of project plugins. Define the
  restart path for opening a project with a different set, including plugins
  whose shaders require roots unavailable after renderer initialization.
- [x] Integrate partial-failure and normal-exit cleanup with existing startup/
  exit owners; stop callbacks and drain work before their providers disappear.
  Retain native code through the lifetime of plugin objects/reflection metadata.

Acceptance gate:

- [x] Process fixtures verify phase/dependency ordering, reflected-type
  availability, and frozen selection in both Editor and Game.
- [x] Missing DLL, incompatible identity, stale selection, provider-start failure,
  and attempted disabled-module loading fail at the correct boundary with an
  error result and clean teardown. No fallback silently omits required plugins.
- [x] A no-project Browser starts without project plugin initialization; opening
  a different-set project follows the documented restart path.
- [x] Editing overrides during a process does not change its modules or mounts;
  a new process observes the new set after any required build.

### Stage 4: Integrate content, shaders, and plugin configuration

Dependencies: Stage 3; Stage 1 supplies early root declarations.

Modules: Projects, Core mount/configuration mechanisms, AssetRegistry, Engine,
RenderCore, ShaderBuild; ContentBrowser only for mount presentation.

Outcome: Enabled plugin data works in authored projects without mutable mounts
or late shader-root registration.

- [ ] Merge enabled plugin mount declarations into the existing immutable mount
  publication transaction; validate collisions, outgoing dependencies, read-only
  policy, and canonical physical containment.
- [ ] Include eligible plugin assets in catalog discovery and direct package
  loading. Preserve logical identities through physical relocation.
- [ ] Feed descriptor-derived shader roots before ShaderBuild's initialization
  freeze. Include plugin source/dependency fingerprints in shader build/cache
  inputs, and keep derived outputs outside read-only plugin installations.
- [ ] Define deterministic configuration precedence: engine defaults, enabled
  plugin defaults in resolved dependency order, project overrides, then existing
  user/command-line overrides where supported. Plugin defaults must not write
  into project files or replace unrelated existing precedence rules.
- [ ] Diagnose references to disabled plugin types/assets before destructive
  editing/saving can discard unresolved data. Do not silently substitute or
  resave stripped representations.

Acceptance gate:

- [ ] Fixture asset resolves via its plugin virtual path, survives plugin-root
  relocation, and enforces read-only and cross-mount dependency policies.
- [ ] Fixture shader compiles through a plugin root and invalidates after source
  changes. Late-root registration is unnecessary; derived caches remain writable.
- [ ] Plugin defaults apply only when enabled; project overrides win
  deterministically. Disabled referenced content/types yield actionable errors
  and cannot be silently lost through ordinary save flows.

### Stage 5: Add restart-required plugin management

Dependencies: Stage 4.

Modules: Projects, DurinEd, MainFrame; reuse existing project-edit ownership,
property/UI infrastructure, and project admission services.

Outcome: Users can review and change the next-start plugin selection safely.

- [ ] Show discovered plugins, current effective state, requested next-start
  state, dependency reasons, compatibility, and build availability.
- [ ] Preview dependency effects and reject disable requests that leave required
  consumers enabled; offer an explicit cascade selection rather than silently
  changing unrelated overrides.
- [ ] Persist validated project overrides atomically through the existing edit
  ownership contract. Display restart/build requirements without mutating the
  current frozen selection.
- [ ] Reuse normal unsaved-work handling before restart; preserve explicit
  project arguments and never discard edits through automatic relaunch.

Acceptance gate:

- [ ] Enabling/disabling via UI survives restart and produces the same resolved
  set as descriptor-driven startup. Cancelled edits leave project files intact.
- [ ] Pending state is distinct from active state. Failed writes, missing build
  artifacts, dependency conflicts, and cancelled restart retain a usable editor.
- [ ] Read-only/project-ownership failures cannot partially replace descriptors;
  no UI action performs live module or mount changes.

### Stage 6: Cook and deploy the plugin closure

Dependencies: Stage 4; Stage 5 is not required for command-line cooking.

Modules/tools: Engine cook/package services; DurinDevTool cook/staging;
ShaderBuild, RenderCore cooked libraries; DHT/CMake output inventories; Projects.

Outcome: A staged Game runs independently of the source plugin directories.

- [ ] Freeze and record the Game plugin closure for cook; stage required module
  binaries, declared native runtime dependencies, reachable content, effective
  runtime configuration, and cooked shader programs.
- [ ] Reuse existing cook roots/reference rules. Include plugin-declared runtime
  roots where explicit loading requires them; do not copy every asset blindly.
- [ ] Reject references to disabled/Editor-only plugin classes and assets. Never
  stage Editor binaries or rely on authored shader sources in Game.
- [ ] Validate staged inventories and compatibility; reject descriptor/selection
  drift between build, cook, and launch. Do not build implicitly during staging.

Acceptance gate:

- [ ] Relocate the staged fixture Game and run with source plugin roots and
  development shader caches unavailable; component, asset, configuration, and
  cooked shader behavior remain correct.
- [ ] Disabled and Editor-only plugin binaries/data are absent from inventories;
  dependency plugin runtime files are present and loaded in order.
- [ ] Missing native dependency, missing explicit-load asset, missing cooked
  shader, or mismatched selection produces a bounded diagnostic failure.

### Stage 7: Qualify integration and publish implemented contracts

Dependencies: Stages 1-6, including the management UI.

Modules/tools: All owners above; Engine, Sandbox, and RoadWeaver consumers;
documentation and applicable CI jobs.

Outcome: Restart-based plugins are a supported source-engine workflow.

- [ ] Run the final build/launch/cook matrix for no-plugin, content-only,
  Runtime+Editor, and dependent-plugin fixtures. Include normal exit and
  representative partial-startup failures with outstanding owned work.
- [ ] Run an `all` build for each supported runtime variant at final acceptance:
  plugin discovery changes generated target membership and reflection inputs
  across every project in Durin.dworkspace, so bounded module builds alone
  cannot qualify this build-graph change. Reuse earlier passing results when
  relevant inputs/environment are unchanged; do not repeat per documentation edit.
- [ ] Qualify Win64 native startup and relocated cooked Game execution. Run
  available macOS build/native acceptance through its owner or CI; record any
  unavailable platform gate explicitly rather than claiming full coverage.
- [ ] Add focused regression coverage to existing metadata, module/startup,
  package, shader, and cook suites; avoid redundant implementation-mirroring tests.
- [ ] Publish lasting descriptor/build contracts under Workspace/Development,
  startup contracts under Runtime/Application, asset/shader rules in their
  owning Runtime domains, and the restart workflow in Editor/Guides.
- [ ] Record tested revision, selections, platform/configuration, fixture inputs,
  results, and outstanding gates here. Store raw logs/reports under ignored
  Build outputs or CI artifacts. Complete/archive only through plan lifecycle.

Acceptance gate:

- [ ] All earlier gates have evidence covering final relevant inputs; no
  unsupported platform or SDK-distribution claim is implied by Win64 success.
- [ ] Existing projects build and start without requiring plugin configuration.
- [ ] A developer can create a source plugin with a reflected component, shader,
  content, and editor companion, enable it, build, restart, and deploy a Game
  without editing Engine's per-plugin module table.
- [ ] Documentation validation and all-plan lifecycle validation pass.

## Execution Order And Handoff

Implement 1 -> 1.1 -> 2 -> 3 -> 4, then UI (5) and cook (6), then qualification (7).
Stage 1.1 completed on 2026-11-12 with the final acceptance above. Earlier
checkpoint receipts remain historical; use final-input evidence for continuation.
Stages 5 and 6 are logically independent after Stage 4; this is not authorization
for concurrent writers in one checkout. Follow repository worktree ownership.

For each implementation commit, update this plan's status and only the checks
supported by evidence. Use exact Plan/Stage trailers from this file and its
executed stage title. Do not mark a stage complete based on a merged API alone
when its process, artifact, or negative-case acceptance remains unverified.
