# Restart-Based Plugin System Plan

Summary: Add project-selected source and content plugins with deterministic build, startup, cooking, and restart-required enablement.

Last reviewed: 2026-11-11

Status: Active
Completed:

## Current Status

Stage 0 is complete as a planning decision only. Stages 1-7 are unimplemented;
their checks remain open. This change authorizes and records the implementation
path, not execution of the later stages. No plugin behavior has been validated.

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
  plugin/module identity collisions and nested plugin ownership. Do not provide
  implicit project-over-engine replacement or arbitrary search paths initially.
- A plugin is an optional distribution/enablement container. A module remains
  the compilation, reflection, and native loading unit. Existing project modules
  continue to work without becoming plugins.
- Introduce a small runtime `Projects` module depending on `Core`, with no
  Engine, Renderer, or editor dependencies. It owns plugin descriptors,
  selection, and manifest admission. Keep generic loading in Core's
  `FModuleManager`; Core must not depend back on Projects. Existing Core project
  selection/path primitives remain in Core for this iteration.
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

- [ ] Register Projects in Engine's project/module graph without introducing
  Core-to-Projects or Projects-to-Engine dependencies.
- [ ] Add `.dplugin` schema and project overrides; preserve existing descriptors
  with no Plugins field. Define absent versus explicitly empty values.
- [ ] Implement discovery, containment checks, duplicate detection, host/platform
  filtering, required dependency closure, cycle checks, and stable ordering.
- [ ] Validate cross-plugin `.dmodule` references against declared plugin
  dependencies. Reject dependencies on disabled/ineligible plugin modules and
  Runtime-to-Editor links, including transitive violations.
- [ ] Freeze phase ordering and its mapping to startup hooks, including a rule
  rejecting an earlier-phase consumer of a later-phase provider.
- [ ] Define canonical selection/build manifests with schema version, logical
  owners, reasons for activation, inputs/fingerprints, and binary identity.
  Separate machine-local resolved paths from portable identity.

Acceptance gate:

- [ ] Shared fixtures produce equivalent canonical selections in Python and C++:
  empty project, content-only plugin, required closure, Editor/Game selection,
  explicit-disable conflict, cycle, duplicate ID/module, missing descriptor,
  path escape, invalid phase edge, and unknown schema version.
- [ ] Existing Engine, Sandbox, and RoadWeaver project descriptors retain their
  existing effective module roots when no plugins are enabled.
- [ ] Errors identify the descriptor and complete failing dependency chain.

### Stage 2: Integrate plugin modules into build and reflection

Dependencies: Stage 1.

Modules/tools: DurinHeaderTool; CMake workspace/project/target generation;
DurinDevTool configure/build services; CoreDObject integration fixtures.

Outcome: Enabled plugin modules compile and expose reflected types through the
existing toolchain, without hand-editing Engine's ModuleDirs for each plugin.

- [ ] Feed resolved plugin modules to CMake and DHT using the selected workspace
  membership; preserve module ownership independently from project ownership.
- [ ] Generate export headers, reflection inputs, targets, include paths, and
  dependencies through existing module mechanisms.
- [ ] Namespace generated state and outputs by plugin ownership and relevant
  target/configuration inputs. Reconfigure when discovery or enablement changes.
- [ ] Produce a build manifest containing exact binary paths, build identity,
  selection fingerprint, and required native runtime dependencies.
- [ ] Create a small repository-owned PluginFixture and a consumer fixture:
  Runtime module with a reflected component/asset, Editor companion, dependent
  plugin, and content-only plugin. Keep tests/fixtures under their owning test
  roots or ignored Build outputs; do not depend on Water development.

Acceptance gate:

- [ ] Enabled fixture builds in DurinEditor and DurinGame; Editor companion is
  absent from the Game graph. Its reflected headers and API exports work across
  the fixture/consumer DLL boundary.
- [ ] Disable the fixture, reconfigure, and prove its targets are absent and
  stale DLLs do not re-enter the selection. Re-enable and verify regeneration.
- [ ] Configuration/variant outputs do not overwrite one another; no-plugin
  project builds remain unchanged in effective membership.

### Stage 3: Admit and start the frozen plugin set

Dependencies: Stage 2.

Modules: Projects, Launch, Engine, DurinEd; narrowly scoped Core ModuleManager
and CoreDObject integration changes only where existing hooks are insufficient.

Outcome: Editor and Game load exactly the admitted plugin module set before
their consumers run, and report actionable startup failures.

- [ ] Resolve project plugin inputs early enough to prepare immutable roots,
  validate build compatibility, and freeze selection before consumption.
- [ ] Check manifests against current descriptors and binary identities before
  native loading. Missing or stale source-plugin artifacts report configure/
  build requirements; never build implicitly inside startup.
- [ ] Register exact module paths and execute dependency-aware phase order.
  Ensure plugin reflection/providers are ready before catalog/package loading
  and before World subsystem membership is captured.
- [ ] Enforce admitted module membership at loading boundaries so a disabled
  plugin cannot be loaded by guessing its name or by stale output discovery.
- [ ] Keep Project Browser admission independent of project plugins. Define the
  restart path for opening a project with a different set, including plugins
  whose shaders require roots unavailable after renderer initialization.
- [ ] Integrate partial-failure and normal-exit cleanup with existing startup/
  exit owners; stop callbacks and drain work before their providers disappear.
  Retain native code through the lifetime of plugin objects/reflection metadata.

Acceptance gate:

- [ ] Process fixtures verify phase/dependency ordering, reflected-type
  availability, and frozen selection in both Editor and Game.
- [ ] Missing DLL, incompatible identity, stale selection, provider-start failure,
  and attempted disabled-module loading fail at the correct boundary with an
  error result and clean teardown. No fallback silently omits required plugins.
- [ ] A no-project Browser starts without project plugin initialization; opening
  a different-set project follows the documented restart path.
- [ ] Editing overrides during a process does not change its modules or mounts;
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

Implement 1 -> 2 -> 3 -> 4, then UI (5) and cook (6), then qualification (7).
Stages 5 and 6 are logically independent after Stage 4; this is not authorization
for concurrent writers in one checkout. Follow repository worktree ownership.

For each implementation commit, update this plan's status and only the checks
supported by evidence. Use exact Plan/Stage trailers from this file and its
executed stage title. Do not mark a stage complete based on a merged API alone
when its process, artifact, or negative-case acceptance remains unverified.
