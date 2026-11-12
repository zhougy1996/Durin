# Game Export Pipeline Plan

Summary: Establish explicit staged-game paths, a stable delivery layout and declared deployment inputs, then complete project Cook policy and one export workflow.

Last reviewed: 2026-11-13

Status: Active
Completed:

## Current Status

Planning/specification only; runtime and tooling implementation has not started.
Stage 0 records the selected direction and must finish schema/fixture preparation
before Stage 1. Execute paths -> layout -> deployment declarations first, then
Cook policy/target separation -> unified export -> qualification.
Planning validation on 2026-11-13: changed-document and all-plan lifecycle checks
pass. No runtime/build/test behavior changes are included in this planning task.

Baseline revision: `871de3b06486954716dba968b25dad4163b904ad`.
Sandbox Win64 Debug and Release exports already Cook, Stage and run 120 ticks
both in their original folders and in copies outside the checkout. Release
verification also inspects PE imports and loaded modules for Debug CRT/development
binary dependencies. These are migration-smoke receipts, not clean-machine or
new-layout acceptance. Raw reports/reproduction scripts remain under ignored
`Build/Exports/SandboxVerification/` and `Build/Exports/SandboxReleaseVerification/`.

Observed gaps:

| Gap | Current implementation/evidence | Required outcome |
| --- | --- | --- |
| Engine-root discovery needs an empty Content directory | [Paths.cpp](../../Engine/Source/Runtime/Core/Private/Misc/Paths.cpp), [stage.py](../../Tools/DurinDevTool/durin_dev_tool/stage.py) | Explicit deployment roots; no source-directory probing in a staged Game |
| Cook root equals executable directory | [EngineLoop.cpp](../../Engine/Source/Runtime/Launch/Private/EngineLoop.cpp) | Content and binaries can move independently within one declared layout |
| Build output shape leaks into delivery | Stage emits `Engine/Binaries/Win64/<Configuration>/Runtime/DurinGame/...` | A stable project-facing delivery tree |
| Deployment selection is partly inferred from existing DLLs | Stage globs engine DLLs and supplements plugin inventories with PE imports | Complete target receipts, including dynamically loaded modules and runtime files |
| Native explicit loads require operator knowledge | Sandbox PlayerPawn loads `GrayboxPawn` by string; exports needed an extra `--root` | Persisted project Cook rules or tracked asset references |
| Target configuration follows the Cook host | [PluginCook.cpp](../../Engine/Source/Runtime/Projects/Private/PluginCook.cpp) captures host configuration | Separate tool-host identity, Game build identity and Cook target policy |
| User output is beside the executable | [RuntimeStorage.cpp](../../Engine/Source/Runtime/Launch/Private/RuntimeStorage.cpp) | Writable per-user data independent of installation location |

## Goal And Scope

A developer selects a project, Win64 target, Debug/Release configuration and
destination. The existing services produce one complete, relocatable Game with
explicit paths, declared native/file dependencies and reproducible asset
retention. Export reports the selected inputs and results without exposing
internal target selection as a routine user responsibility.

Win64 Debug and Release source-engine builds are the first acceptance scope.
Source Editor, Browser, source Game and headless tools remain working. Generic
metadata/path contracts must preserve supported platform semantics; native macOS
Game staging remains an explicitly separate outstanding gate.

Retain immutable plugin admission, logical asset identities, existing Cook/DDC
and Shader products, and transactional publication. This plan does not introduce
a second asset registry, cooker, module loader or application startup state machine.
Loose cooked files remain supported. Pak/IoStore equivalents, physical chunks,
DLC, installers, code signing, Shipping qualification, SDK distribution and a new
Editor packaging UI are deferred. A later UI should call the same export service.

## UE Reference And Selected Adaptation

| UE reference | Adopt in Durin | Boundary |
| --- | --- | --- |
| [BuildCookRun and build operations](https://dev.epicgames.com/documentation/en-us/unreal-engine/build-operations-cooking-packaging-deploying-and-running-projects-in-unreal-engine) | Separate Build/Cook/Stage services with one explicit orchestration request | Existing low-level commands keep their current no-implicit-build/Cook contract |
| [RuntimeDependencies and staged file kinds](https://dev.epicgames.com/documentation/unreal-engine/integrating-third-party-libraries-into-unreal-engine) | Declare runtime modules/files and emit target build receipts | PE import inspection supplements declarations; it cannot discover every dynamic load |
| [BuildGraph receipt tasks](https://dev.epicgames.com/documentation/unreal-engine/buildgraph-script-tasks-reference-for-unreal-engine) | Select actual build products and runtime dependencies from completed-build records | Reuse DHT/CMake ownership; no independent directory-scanning build catalog |
| [Primary Asset Cook rules](https://dev.epicgames.com/documentation/en-us/unreal-engine/cooking-content-and-creating-chunks-in-unreal-engine) | Use current AssetManager, AlwaysCook and tracked references | No guessing arbitrary C++ strings and no copy-all-content workaround |
| [Packaging settings](https://dev.epicgames.com/documentation/en-us/unreal-engine/project-section-of-the-unreal-engine-project-settings) | Make runtime prerequisites and delivery settings explicit | Do not copy UE container formats or promise an installer in this milestone |

These are architectural adaptations from public documentation, not claims of
matching UE's private implementation or every platform's output layout.

## Selected Contracts

### One startup path context

Core owns low-level immutable path values; Launch selects/initializes the host
mode before any EngineDir/RootDir/config/storage consumer caches a value.
Projects continues to own project/plugin identity and admission. Extend existing
FPaths/mount/configuration services instead of adding parallel deployed variants.

- Source Editor/tools obtain their source context through existing host setup.
  Source Game becomes an explicit DevTool launch mode; migrate its launcher,
  probes and documentation rather than inferring source mode from nearby files.
- A standalone Game defaults to staged mode. Its launcher passes the deployment
  manifest explicitly; direct executable launch uses one documented location
  relative to the executable. An invalid/missing staged manifest is a bounded
  failure, including when a source checkout exists higher in the directory tree.
- Relative paths resolve against the manifest's canonical deployment root,
  never the current working directory. Reject escapes, unsupported schema/target,
  conflicting roots and aliases escaping the deployment before publishing paths.
- RootDir, EngineDir, engine/project content, binary, config and third-party
  accessors receive explicit mode-specific bindings. Audit callers that append
  source-only suffixes. Cooked mounts use the declared Cook root, not LaunchDir.
- Staged project admission consumes a minimal detached project view from the
  runtime manifest; no authored `.dproject` or workspace lookup is necessary.
  Source descriptor management and Editor history retain their existing owners.

### Stable delivery layout

The selected Win64 layout is project-facing and independent of CMake's build
directory hierarchy. The output folder can be renamed without changing identity:

```text
Sandbox/
  RunGame.bat
  Runtime.json
  Binaries/Win64/
    Sandbox.exe
    DurinGame-*.dll
    <declared native dependencies>
  Config/
    Project.yaml
    DurinGame.yaml
    Defaults/DurinGame.yaml
  Content/
    CookManifest.bin
    <versioned runtime Cook snapshot>
    Engine/...
    Game/...
    Plugins/<Id>/...
    Shaders/ShaderLibrary.dslb
```

The executable and imported native DLLs initially share the binary directory,
respecting Windows loader behavior before application code can run. The root
batch launcher locates the executable/manifest; a native bootstrap is optional
future presentation work. Source build outputs retain their existing layout.
There is no empty Engine/Content, Project/Content or ThirdParty directory used
only to satisfy discovery. Real declared non-asset files may create directories.
Preserve `/Engine/`, `/Game/` and `/Plugins/<Id>/` serialized identities.

Evolve `PluginRuntime.json` into deployment-root `Runtime.json` schema v2, including
generic target/project/path/native fields and the existing plugin section. Core
reads only its generic bootstrap subset; Projects validates its project/plugin
subset. Both consume one document, not separately maintained inventories.
CookManifest remains the sole asset catalog/checksum authority. Reduce the Cook
snapshot to required runtime selection/configuration bindings; keep full authored
descriptors, workspace inputs and build provenance in export evidence. Hashes
bind the runtime manifest to the committed Cook/native inputs.

Existing exported folders retain their original binaries/behavior and are not
rewritten. New output moves to the new schema atomically. Unsupported old Cook
snapshots receive an explicit recook/restage diagnostic; there is no permanent
legacy-layout switch or silent source fallback. Stage 1 may map the new schema
onto current physical paths during migration; Stage 2 changes those path values,
using the same runtime mechanism rather than a second layout implementation.

### Declared deployment inputs

DHT/module metadata and CMake target rules declare linked inputs, dynamically
loaded Runtime modules, and required runtime files. Dynamic module dependencies
cause build/receipt membership without inventing a linker dependency. Platform,
host and configuration eligibility remain part of the existing module graph.

A completed-build receipt records target/project/engine identity, platform,
architecture, configuration, runtime variant, ABI, exact build products, required
dynamic modules and runtime files. Records include ownership, source artifact,
deployment-relative destination, file kind and content identity. Emit receipts
only after their inputs successfully build; configuration alone is not a receipt.
Ordinary Engine/project modules and selected plugins use the same deployment
closure, while the available plugin build inventory may remain a superset.
Selection-only overrides are deployment inputs, not native build-availability
inputs; they must not force configure/build or inventory republication when
matching artifacts already exist. Source builds still compile all eligible
discovered plugins under the existing build policy.

Stage consumes receipts plus frozen Cook selection. It never authorizes files by
globbing a binary directory. PE ordinary/delay import checks detect missing or
incompatible native inputs; dynamic libraries and non-DLL data still need explicit
declarations. Reject differing DLLs with the same loader identity, path escapes,
stale receipts, wrong target/ABI and dependencies on inactive/Editor modules.

Distinguish OS-provided libraries from redistributable runtime dependencies.
Presence in the build machine's System32 is not sufficient evidence of an OS
guarantee. Stage 0 selects an explicit Win64 system-library policy and VC++
runtime delivery/prerequisite policy with provenance/license requirements.

### Cook, configuration and writable data

Game configuration/target build identity are explicit Cook/export inputs, separate
from the tool-host configuration and provenance. A compatible Debug Cook host
must be able to target Release Game without importing Game DLLs into the host.
Validate host provider/schema compatibility against the selected target inputs;
retain the standalone tool's existing provider/work lifetime and offline saved-
input contract. Never switch providers or mutate active plugin admission mid-run.

Use existing target platform policy, AssetManager settings, primary IDs, bundles
and required references. Persist Sandbox's native explicit-load requirement in
project policy or a tracked asset configuration. Normal export must include
GrayboxPawn without a caller remembering an extra command-line root, and must
exclude unrelated optional content. Ad hoc `--root` remains a developer override.
Preserve NeverCook, EditorOnly/source-only and blocked-plugin semantics.

Effective Engine/plugin/project defaults remain immutable deployment inputs.
Saved user overrides, logs and caches use a stable application identity and an
explicit per-user writable policy (Win64 LocalAppData), independent of package
location/configuration. Preserve existing overrides during migration; never seed
user overrides with merged inherited project/plugin values. Portable writable
storage, if exposed, must be explicit. Installation write failure cannot cause
fallback into source or an arbitrary current directory.

### Unified export request

The intended public entrypoint, not yet implemented, is:

```powershell
.\DevTool.bat export --project Sandbox/Sandbox.dproject --platform Win64 --configuration Release --output Build/Exports/SandboxRelease --verify
```

One normalized request freezes project, target, effective plugin selection, Cook
policy, layout, prerequisites and output identity. It selects the exact tool and
Game build targets from declarations. Users need not list Renderer/VulkanRHI or
choose a Cook-host preset merely to request a Game configuration.

The top-level command explicitly orchestrates Build -> Cook -> Stage -> optional
verification/archive using existing services. Individual build/cook/stage commands
remain available. Reuse compatible build receipts before requesting incremental
construction of missing/stale declared inputs. Skip-build/skip-Cook modes require
compatible receipts and
committed outputs; they cannot weaken freshness/identity checks. A preview mode
reports selected targets/files/policies without building or publishing.

Use one invocation work root and output-writer ownership. Verify a temporary
complete candidate before replacing an existing public export; failed build,
Cook, Stage, verification or requested archive publication preserves the previous
committed generation. Reuse existing transaction/rollback mechanisms. Verification
uses isolated user data and keeps logs/reports outside the delivered tree.

## Ownership And Related Plans

| Responsibility | Owners |
| --- | --- |
| Explicit bootstrap paths, DLL and low-level filesystem mechanisms | Core, Launch |
| Detached project/plugin admission and configuration bindings | Projects, Launch |
| Module/runtime file declarations and completed-build receipts | DurinHeaderTool, CMake, DurinDevTool |
| Target policy, retained assets and runtime Shader products | Engine, TargetPlatform, ShaderBuild, RenderCore |
| Orchestration, staging, archive, inspection and verification | DurinDevTool, DurinAssetTool |
| Default-level/native explicit-load fixture | Sandbox |

[Restart-Based Plugin System](RestartBasedPluginSystem.md) remains authoritative
for selection, phase order and immutable plugin lifetime; its native macOS gate
is not closed by this plan. This plan evolves the already implemented Win64
deployment envelope. [Runtime Startup And Scene Activation](RuntimeStartupAndSceneActivation.md)
owns readiness, scene operations and shutdown; inject configuration through its
existing bootstrap boundary without creating another startup state machine.
[DurinEditor SDK Packaging](DurinEditorSdkPackaging.md) owns Editor SDK products,
installed development interfaces and their layout. Share useful receipt and
redistribution metadata, without changing the SDK's selected layout or adding
SDK delivery to Game export.

## Implementation Stages

### Stage 0: Freeze migration contracts and baseline fixtures

Status: Planning decisions recorded; schema and fixture work pending.

- [x] Record current export behavior, concrete gaps, UE references and ownership.
- [x] Select the root/layout/receipt/Cook/orchestration direction and scope above.
- [ ] Freeze versioned manifest/receipt fields, FPaths accessor bindings, source
  launch migration, application identity and Win64 prerequisite policy.
- [ ] Add portable schema/golden fixtures for no-plugin, content-only, dependent
  Runtime+Editor, dynamic native modules and native non-asset data. Preserve the
  Sandbox Debug/Release receipts as historical baseline inputs.

Acceptance: schema fixtures encode valid layouts and target identities plus
invalid/missing manifest, containment and incompatible-version cases. Every
subsequent stage has one selected contract to implement; no product behavior is
claimed from this planning document alone.

### Stage 1: Initialize explicit source and staged startup paths

Dependencies: Stage 0. Owners: Core, Launch, Projects, DevTool run/stage.

- [ ] Initialize immutable path context before cached FPaths/config/logger/storage
  reads. Publish paths only after bootstrap validation; retain early stderr errors.
- [ ] Migrate staged admission and Stage's emitter to Runtime.json v2, initially
  permitting current physical path values. Preserve working end-to-end exports
  between commits; adapt existing plugin/native admission rather than duplicating it.
- [ ] Add explicit source Game launch policy through DevTool and migrate probes/
  consumers across every Durin.dworkspace project. Preserve Editor/Browser/tools.
- [ ] Route cooked mounts, Shader library, project configuration and native lookup
  through declared roots; prevent staged-mode source/cwd fallback.

Acceptance: a deployed fixture without the sentinel Engine/Content starts from
an unrelated cwd. Missing/corrupt manifests fail both outside and beneath a
checkout without adopting its Engine. Source Editor, source Game, Browser and
headless tool startup retain their intended path context and clean shutdown.

### Stage 2: Publish the stable delivery layout and writable-data policy

Dependencies: Stage 1. Owners: DevTool Stage, Core, Launch, Projects.

- [ ] Emit the selected Binaries/Config/Content tree and named Game executable;
  remove build-configuration nesting and discovery-only directories.
- [ ] Bind project/config/native/Cook paths relative to deployment root. Omit
  authored descriptors/workspace inputs unnecessary for runtime admission.
- [ ] Resolve stable per-user Saved/config/log/cache roots before first write;
  keep frozen defaults read-only and preserve user overrides across relocation.
- [ ] Keep full provenance in external export evidence; version and bind the
  minimal runtime snapshot through the existing Cook transaction.

Acceptance: Sandbox starts with default Level, mesh and cooked Shader data after
folder rename/relocation to a path containing spaces/non-ASCII characters. A
read-only installation runs using isolated writable user data without creating
Saved beside the executable. Required-file/mount/config failures retain usable
previous outputs; no empty directory is needed for path discovery.
Layout qualification may use the existing explicit pawn Cook root until Stage 4
persists that requirement as project policy.

### Stage 3: Stage the declared native and runtime-file closure

Dependencies: Stages 0-2. Owners: DHT/CMake, DevTool, Projects.

- [ ] Extend existing module/target metadata for dynamic Runtime modules and
  runtime files; migrate Renderer, RHI selection, Sandbox and plugin fixtures.
- [ ] Generate target receipts after successful builds, binding all ownership,
  compatibility, source artifact, destination and content identities.
- [ ] Replace Stage DLL globs/manual target knowledge with receipt-based selection
  for ordinary modules and the enabled plugin closure. Preserve build-all versus
  startup/deployment-subset semantics.
- [ ] Apply declared file categories and system/prerequisite policy. Retain PE
  checks and reject undeclared imports/loads, collisions and stale/missing artifacts.

Acceptance: native dynamic-module and non-DLL fixtures work without manual copy/
build steps. Planting an unrelated or stale DLL in build output does not deploy
it. Missing required runtime files, inactive/Editor edges, same-name differing
DLLs and wrong target/ABI fail before publication. Available plugin artifacts
remain a superset and selection changes do not republish build inventories.

### Stage 4: Make Cook roots and target configuration declarative

Dependencies: Stage 3. Owners: Engine, Projects, DurinAssetTool, Sandbox.

- [ ] Pass immutable target Game identity/configuration independently of host
  identity; validate provider/schema compatibility and bind committed Cook inputs.
- [ ] Declare GrayboxPawn through existing project AssetManager/asset references;
  remove the requirement for export scripts to add a special Sandbox root.
- [ ] Preserve merged defaults, retained references, explicit-load/AlwaysCook,
  bundle, blocked-plugin, NeverCook and EditorOnly/source-only behavior.
- [ ] Keep asset checksums in CookManifest and required native Shader programs in
  the existing project library. Do not retain all enabled plugin content.

Acceptance: default Sandbox Cook includes the pawn mesh and its dependencies,
while an optional unreferenced fixture remains absent. Debug host -> Release Game
Cook succeeds with compatible providers. Incompatible hosts/targets and required
references conflicting with policy fail without changing committed Cook output.

### Stage 5: Add one export orchestration entrypoint

Dependencies: Stages 1-4. Owner: DevTool; reuse existing native services.

- [ ] Implement preview and normalized project/platform/configuration/output
  selection, declared target construction, stage summaries and typed JSON reports.
- [ ] Orchestrate Build/Cook/Stage with strict skip/reuse checks; optional verify
  and ZIP archive remain part of the same explicit request and result.
- [ ] Verify private candidates using isolated user data before final replacement;
  preserve output ownership, rollback/recovery and stable-input boundaries.
- [ ] Provide reproduction metadata and actionable phase/target/file diagnostics.
  Keep source descriptors unchanged and lower-level commands independently usable.

Acceptance: the example export command produces Sandbox without manual target/
root/preset knowledge. Preview makes no output/build mutations. Invalid reuse,
cancelled work and injected failures in every requested phase preserve the last
committed export; logs, temporary user data and developer settings are not shipped.

### Stage 6: Qualify delivery and publish implemented contracts

Dependencies: Stages 1-5. Owners: all above, platform/CI qualification owner.

- [ ] Qualify the matrix below and record exact source/fixture/toolchain inputs,
  target receipts, selections, prerequisites, manifests, commands and outcomes.
- [ ] At final acceptance, run incremental `all` builds for Win64 Debug/Release
  Editor and Game: shared Core path bindings and generated module/build-receipt
  membership affect all workspace projects. Reuse unchanged passing receipts.
- [ ] Run owner-specific path/configuration/admission, metadata, Cook and Shader
  tests plus bounded real-process cases. Follow [build](../Agents/BuildAndRun.md)
  and [test selection](../Agents/Testing.md); do not substitute every native test
  or repeated builds for the actual deployment/clean-machine gates.
- [ ] Publish lasting contracts in Workspace/Development, Runtime/Application and
  owning asset domains, plus an export guide. Update plugin-plan cross references
  only for replaced contracts; preserve prior evidence and unqualified platforms.
- [ ] Validate changed documents and all-plan lifecycle; complete/archive only
  when every mandatory gate has evidence.

Mandatory validation matrix:

| Case | Required evidence |
| --- | --- |
| Win64 Debug and Release Sandbox | Default Level, native pawn mesh and cooked shaders; normal frame progress and exit; configuration/CRT identities match |
| No-plugin, content-only, dependent Runtime+Editor | Only selected Game/native/content closure; phase order; no Editor binaries/compiler/source requirements |
| Explicit dynamic library and non-asset data | Receipt-selected files work; undeclared/missing files reject publication |
| Source hosts | Editor/Browser/source Game/tool startup and shutdown work through one existing lifecycle |
| Relocated/read-only install | Unrelated cwd; renamed Unicode/space-containing path; per-user output; no writes to immutable deployment files |
| Source/development inputs unavailable | Workspace, authored plugins/assets/shaders and development caches unavailable; restricted PATH; native modules load from the deployment/system locations allowed by policy |
| Clean Windows environment | No VS/compiler/SDK/source tree; positive run with declared prerequisites, clear prerequisite failure without them; inspect actual loaded modules and Release imports for Debug CRT |
| Failure and drift | Missing/corrupt native/config/Cook/Shader/snapshot files, wrong schema/ABI/selection, stale receipt, write/verification/archive failures; preserve previous committed generation |

Raw reports, traces and reproduction scripts belong under ignored Build outputs
or CI artifacts. A 120-tick migration smoke does not close clean-machine coverage.
If the clean-machine lane is unavailable, record it as an open gate. Native
macOS staging, Shipping, installers, signing and container-format work remain
explicitly outside this milestone; do not infer their support from Win64 success.

## Execution And Handoff

Implement Stage 0 -> 1 -> 2 -> 3 -> 4 -> 5 -> 6. Preserve source/build ownership;
no concurrent writers in one checkout. When changing shared APIs or descriptors,
search and migrate consumers across every project in Durin.dworkspace, including
tests and source launch paths. Validate owners/affected consumers before final
matrix coverage. Avoid unrelated startup/readiness or SDK-layout redesign.

Implementation commits update this plan's evidence-backed checks and use exact
`Plan: Documentation/Plans/GameExportPipeline.md` and the executed stage title as
`Stage` trailers. This initial document records decisions and baseline evidence;
it does not authorize marking runtime/schema/fixture implementation gates passed.
