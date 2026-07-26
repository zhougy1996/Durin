# Runtime Variant And Tracy Profiling Plan

Summary: Rename the workspace-wide runtime selection concept and add opt-in Tracy instrumentation through isolated Release profiling presets.

Last reviewed: 2026-07-27

## Current Status

Implementation in progress. Stages 0-4 are implemented. Focused Python/native
tests, ordinary Editor/Game configuration, ordinary Release/Shipping isolation,
both Profiling builds, and an Editor capture have passed.

The former generic build-identifier mechanism was removed after worktree-owned
output trees became the repository's concurrency boundary. Profiling final
outputs remain isolated by the existing `Profiling` preset role, while
configuration-independent DHT metadata and configuration-matched third-party
runtime DLLs are shared by presets in one worktree.

Standard Release and Editor Release Profiling full builds now deploy identical
Assimp and Slang DLLs to `Engine/Binaries/Win64/ThirdParty/Release/`. The
profiling Editor launches successfully from its isolated runtime directory while
resolving those shared DLLs; Tracy remains profiling-local.

The Editor capture recorded 233 frames and 2,307 zones over 5.11 seconds. The
Game profiling executable completes initialization and remains running without a
profiler, but attaching the matching Tracy v0.13.1 capture client currently
causes an access violation. Windows Error Reporting identifies
`DurinGame-VulkanRHI.dll` at offset `0x1485a` as the repeatable faulting module.
The plan remains active until that enabled Game runtime failure is resolved and
the final validation matrix passes.

## Goal

Replace the overloaded build term `profile` with `runtime variant` for
workspace-wide `DurinEditor` and `DurinGame` selection, then introduce Tracy as
an optional development dependency used by dedicated Release profiling presets.

The normal Debug, Release, Shipping, setup, and worktree workflows must remain
independent of Tracy. A developer who does not request profiling must not need
the Tracy source tree, compile its client, deploy its runtime, or carry enabled
instrumentation.

## Scope

- Rename the repository-owned `profile` concept that selects `DurinEditor` or
  `DurinGame` to `runtime variant`.
- Migrate CMake, DurinHeaderTool, BuildTool, generated metadata, cache paths,
  tests, diagnostics, and owning documentation to the new terminology.
- Extend third-party bootstrap metadata so development-only dependencies are
  excluded from default setup.
- Pin and bootstrap the Tracy client source as a development-only direct-source
  dependency.
- Add opt-in Release profiling presets for the editor and game runtime variants.
- Provide engine-owned CPU profiling macros and instrument selected frame,
  thread, and task boundaries.
- Verify both disabled and enabled build graphs and perform a real Tracy capture.

## Non-Goals

- Creating a new `CMAKE_BUILD_TYPE` such as `Profile` or `Profiling`.
- Treating profiling as a third runtime variant alongside `DurinEditor` and
  `DurinGame`.
- Enabling Tracy in ordinary Release builds or permitting it in Shipping.
- Exposing Tracy types as part of a Durin public API contract.
- Instrumenting every function or adding broad automatic compiler
  instrumentation.
- Adding GPU, allocation, lock, frame-image, or call-stack instrumentation in
  the initial integration.
- Vendoring or distributing the Tracy profiler GUI. Developer documentation
  will point to the matching upstream release.
- Changing the meaning or layout of `.dproject` `BaseModules` and
  `ExtraModules` beyond replacing the owning terminology.

## Design Decisions and Invariants

### Runtime Variant Terminology

- The canonical concept name is **runtime variant**.
- The canonical CMake cache variable is `DURIN_RUNTIME_VARIANT`.
- Initial runtime variant values remain `DurinEditor` and `DurinGame`.
- `DURIN_WITH_EDITOR` remains the code-level semantic switch derived from the
  selected runtime variant.
- `ExtraModules.<RuntimeVariant>.Modules` remains the project-descriptor shape;
  `ExtraModules` is not renamed because it describes module enablement rather
  than the old profile concept.
- Generated metadata paths use
  `Intermediate/Build/<Platform>/<RuntimeVariant>/`.
- DHT serialized fields, command-line options, diagnostics, Python identifiers,
  and tests use `runtime_variant` or `RuntimeVariant` consistently.
- The migration is repository-atomic. There is no permanent
  `DURIN_PROFILE_NAME` or `--profile` compatibility alias. Configuration may
  detect the legacy CMake variable and fail with a focused migration message
  rather than silently ignoring it.
- CMake targets continue to mean build graph nodes such as `Core`,
  `RenderCore`, and `DurinLauncher`; the new terminology must not introduce a
  second unqualified `target` concept.

### Profiling Presets

- Profiling presets use the normal `Release` CMake configuration so captures
  reflect optimized code.
- The initial preset names are:
  - `Win64-Release-DurinEditor-Profiling`
  - `Win64-Release-DurinGame-Profiling`
- Each profiling preset has its own CMake binary directory and sets
  `DURIN_PRESET_ROLE=Profiling` so its final binaries do not collide with
  ordinary Release workflows. Configuration-independent DHT metadata is shared
  within the worktree.
- The `Profiling` role derives the `Release-Profiling` final-output
  configuration directly. The removed generic identifier is not retained as a
  second output-naming mechanism.
- Role-independent third-party runtime DLLs use
  `Binaries/<Platform>/ThirdParty/<CMakeConfig>/`; Release and Release
  Profiling share those files while Tracy remains in the profiling runtime
  directory.
- Each profiling preset sets `DURIN_ENABLE_TRACY=ON`.
- Ordinary Debug and Release presets leave `DURIN_ENABLE_TRACY=OFF`.
- Shipping configuration rejects `DURIN_ENABLE_TRACY=ON` during configure with
  a clear error.
- Profiling presets are registered in the repository Agent Build Profile so
  BuildTool can configure, build, run, inspect, and purge them through the
  supported entrypoint.

### Tracy Dependency and Linkage

- Tracy is pinned to the selected stable upstream release rather than a moving
  branch. The initial implementation target is `v0.13.1`; implementation must
  revalidate the tag, license, and CMake target names before landing.
- The bootstrap manifest marks Tracy as development-only.
- Default `Setup.bat` and bootstrap `--all` do not fetch development-only
  dependencies.
- Explicit `--libs tracy` preparation remains supported. A
  `--with-development` selector includes development-only manifests with
  `--all`, and a focused `Setup_tracy.bat` wrapper provides the normal Windows
  entrypoint.
- Disabled configuration does not inspect or require
  `Engine/External/Source/tracy`.
- Enabled configuration fails early with an actionable bootstrap command when
  the pinned source is absent or incomplete.
- The engine uses one shared Tracy client runtime per process. Instrumented
  Durin DLLs must not each embed an independent static Tracy client.
- The Tracy runtime is deployed only into the profiling build's isolated
  runtime output. Ordinary binaries have no Tracy runtime dependency.
- Tracy is configured for on-demand, localhost-only capture initially.
  Call-stack capture and other higher-overhead features remain disabled.
- Third-party license obligations are recorded using the repository's existing
  dependency documentation or distribution-notice mechanism.

### Engine Profiling Surface

- Repository-owned code includes an engine profiling header rather than
  including Tracy headers directly at call sites.
- The engine surface provides narrowly named operations for:
  - lexical CPU zones;
  - named CPU zones;
  - frame marks;
  - thread names.
- `DURIN_WITH_TRACY=0/1` is the repository-facing compile definition.
- When `DURIN_WITH_TRACY=0`, engine profiling macros expand without evaluating
  profiling-only arguments and do not require Tracy headers or symbols.
- When `DURIN_WITH_TRACY=1`, the adapter maps the engine operations to Tracy
  while keeping Tracy types out of Durin function signatures and reflected
  declarations.
- Instrumentation names are stable, bounded strings suitable for comparing
  captures. Dynamic per-item text is added only when it materially improves
  diagnosis and does not create unbounded capture noise.

## Current Foundations and Gaps

Existing foundations:

- `CMakePresets.json` already separates configuration, runtime selection, and
  isolated CMake build directories.
- The preset role already distinguishes profiling behavior from standard
  builds and can derive its isolated final-output configuration.
- The third-party bootstrap already supports pinned git direct-source
  dependencies and explicit `--libs` selection.
- Engine-specific third-party targets are registered from
  `Engine/CMake/ThirdParty`.
- Core owns runnable-thread naming, the queued thread pool, and common
  cross-module facilities.
- The launch loop provides a central frame lifecycle boundary.

Current gaps:

- `profile` is used in CMake variables, Python models, CLI options, serialized
  metadata, cache paths, diagnostics, and documentation.
- Bootstrap distinguishes only normal and test-only dependencies; `--all`
  currently selects every non-test manifest.
- There is no workspace profiling feature option or profiling preset role.
- There is no process-wide profiling client ownership rule or deployment path.
- Engine code has no profiler-neutral instrumentation surface.
- No automated check proves that disabled builds remain independent of Tracy.

Stage 0 inventory classified the migration surfaces as follows:

- Runtime-variant terminology: root presets; workspace/project CMake setup,
  target naming, shared PCH definitions, and launcher naming; DHT CLI,
  configuration models, worker context, generated CMake, reflection/export
  manifests, cache keys, paths, locks, and tests; BuildTool descriptor,
  scaffolding, runtime/test path, purge, status, and tests; owning Workspace,
  Build, Reflection, and Runtime Lifecycle documentation.
- Performance profiling: this plan, the new Tracy bootstrap/CMake integration,
  profiling presets, instrumentation macros, and capture workflow.
- Unrelated and intentionally unchanged: BuildTool host build profiles, Slang
  shader profiles, IDE/user profiles, asset platform feature profiles, upstream
  third-party content, and archived plans.
- Stale metadata behavior: reflection schema v5 and export-manifest schema v6
  require `RuntimeVariant`; old `Profile` fields load as absent and fail the
  schema/contract comparison, causing deterministic regeneration beneath the
  new runtime-variant path.

## Implementation Stages

### Stage 0: Lock The Migration And Validation Contract

- [x] Inventory every repository-owned use of `profile`, classifying it as
  runtime-variant terminology, performance profiling, shader profile, user
  profile, or unrelated third-party content.
- [x] Record the exact CMake, DHT, BuildTool, generated-schema, path, preset,
  test, and documentation surfaces that require migration.
- [x] Add or update focused tests that capture current editor/game module
  selection and output derivation before renaming identifiers.
- [x] Define the stale-cache behavior for old generated metadata and confirm
  that the terminology migration invalidates or regenerates it deterministically.
- [x] Confirm the selected Tracy tag, BSD-3-Clause license, upstream
  `Tracy::TracyClient` target, and shared-library behavior on supported hosts.

#### Acceptance Gate

- The inventory separates genuine runtime-variant uses from unrelated meanings
  of `profile`, and regression tests cover both current runtime variants before
  production identifiers change.

### Stage 1: Rename Profile To Runtime Variant

- [x] Replace `DURIN_PROFILE_NAME` with `DURIN_RUNTIME_VARIANT` in root presets,
  workspace setup, project setup, common definitions, output derivation, and
  generated CMake.
- [x] Rename DHT configuration models, CLI arguments, worker context, cache
  keys, generated manifest fields, path helpers, and diagnostics.
- [x] Rename corresponding BuildTool descriptors, display text, scaffolded
  metadata, and tests.
- [x] Preserve `DurinEditor`, `DurinGame`, `DURIN_WITH_EDITOR`, module
  selection, module filenames, launcher filenames, and runtime behavior.
- [x] Add a focused configure diagnostic for a supplied legacy
  `DURIN_PROFILE_NAME`.
- [x] Update current owning documentation and direct links from `profile` to
  `runtime variant`; leave unrelated shader, performance, and user-profile
  terminology unchanged.
- [x] Update generated-metadata and cache invalidation tests for the renamed
  serialized field and path component.

#### Acceptance Gate

- DHT and BuildTool tests pass using only runtime-variant terminology.
- Editor and game presets configure with the same enabled module graphs and
  output names as before.
- A repository-owned search finds no stale runtime-selection use of
  `DURIN_PROFILE_NAME`, `--profile`, `ProfileName`, or `profile_name`.
- Old metadata cannot be mistaken for current runtime-variant metadata.

### Stage 2: Add Development-Only Dependency Bootstrap

- [x] Extend manifest validation with a boolean `development_only` field whose
  default is false.
- [x] Exclude development-only manifests from `--all` unless
  `--with-development` is supplied.
- [x] Preserve explicit `--libs <name>` behavior for development-only
  manifests.
- [x] Add positive, negative, malformed-manifest, and selection-order tests for
  the new bootstrap behavior.
- [x] Add a pinned Tracy direct-source manifest and `Setup_tracy.bat`.
- [x] Add conditional Tracy CMake registration that is not evaluated when
  `DURIN_ENABLE_TRACY=OFF`.
- [x] Update third-party bootstrap documentation with default, explicit, and
  development-inclusive preparation flows.

#### Acceptance Gate

- Manifest validation passes.
- Default `--all` selection excludes Tracy, while `--libs tracy` and
  `--all --with-development` select it.
- A clean disabled configure succeeds when the Tracy source directory is
  absent.
- An enabled configure without Tracy fails with one actionable preparation
  instruction.

### Stage 3: Add Isolated Release Profiling Presets

- [x] Add `DURIN_ENABLE_TRACY`, defaulting to `OFF`, to workspace build options.
- [x] Reject Tracy in Shipping and expose
  `DURIN_WITH_TRACY=0/1` consistently to repository targets and shared PCH
  contexts.
- [x] Configure one shared Tracy client runtime with on-demand and
  localhost-only capture.
- [x] Ensure every instrumented module links to the same shared client and that
  the runtime is deployed through existing third-party runtime mechanisms.
- [x] Add editor and game Release Profiling presets with isolated CMake and
  final-output directories.
- [x] Share role-independent third-party runtime DLLs by platform and CMake
  configuration while keeping Tracy in the profiling runtime directory.
- [x] Register both presets in the repository Agent Build Profile and extend
  BuildTool configuration tests.
- [x] Ensure configure summaries and status output identify the runtime variant,
  Release configuration, Profiling preset role, and Tracy state separately.

#### Acceptance Gate

- Ordinary Debug and Release link graphs contain no Tracy client or runtime
  dependency.
- Both Profiling presets configure and build through BuildTool.
- Editor and game Profiling outputs are isolated from their ordinary Release
  outputs.
- Shipping plus `DURIN_ENABLE_TRACY=ON` is rejected at configure time.
- A profiling executable and all instrumented module DLLs resolve one shared
  Tracy client runtime.

### Stage 4: Add Initial CPU Instrumentation

- [x] Add the engine-owned profiling header and disabled-build macro tests.
- [x] Add one frame mark at the authoritative engine frame boundary.
- [x] Forward game, rendering, worker, and owned service-thread names at their
  thread entry boundaries.
- [x] Add zones around queued-task execution and a small set of stable top-level
  frame phases.
- [x] Add targeted zones to representative renderer and asset workloads only
  where the boundary has actionable ownership.
- [x] Verify profiling-only arguments are not evaluated in disabled builds.
- [x] Document naming rules and the supported instrumentation surface for
  repository C++ contributors.

#### Acceptance Gate

- A Tracy capture from the editor Profiling preset shows frame marks, named
  game/render/worker threads, queued tasks, and selected top-level phases.
- A Tracy capture from the game Profiling preset connects and records the same
  common runtime boundaries without editor-only assumptions.
- Capturing is optional at runtime and normal shutdown does not wait
  indefinitely for a profiler.
- Disabled builds compile the same call sites without Tracy headers, symbols,
  runtime files, or evaluation of profiling-only expressions.

### Stage 5: Complete Validation And Move Lasting Contracts

- [x] Run the plan validator and all focused Python/native tests introduced or
  affected by the work.
- [x] Validate normal Editor and Game presets through the repository BuildTool.
- [ ] Validate both Release Profiling presets and record a smoke-capture result.
- [x] Complete a successful full `all` build for the registered Agent editor
  preset required by repository handoff rules.
- [x] Move lasting runtime-variant ownership rules into Workspace documentation.
- [x] Move lasting preset, bootstrap, build-option, deployment, and profiling
  workflow rules into Development documentation.
- [ ] Record completion evidence, close the plan checklists, and archive the
  plan according to the plan lifecycle rules.

#### Acceptance Gate

- All validation-matrix rows pass, current documentation owns the implemented
  contracts, and the active plan contains no unresolved required work.

## Validation Matrix

| Area | Disabled/normal case | Enabled/profiling case |
| --- | --- | --- |
| Plan metadata | All-plan validation succeeds | Same |
| Runtime terminology | Editor and Game select unchanged module graphs | Profiling presets report the same runtime variants |
| Bootstrap | `--all` excludes Tracy | Explicit and development-inclusive selection prepare the pinned source |
| Configure without Tracy source | Debug and Release succeed | Fails with an actionable bootstrap message |
| CMake option | `DURIN_WITH_TRACY=0` | `DURIN_WITH_TRACY=1` |
| Link/runtime graph | No Tracy library or deployed runtime | One shared Tracy client is linked and deployed |
| Output isolation | Ordinary Release runtime paths remain unchanged | The `Profiling` role isolates Durin/Tracy outputs and shares configuration-matched third-party DLLs |
| Shipping | Configures with Tracy disabled | Enabling Tracy is rejected |
| CPU instrumentation | Macros are no-op and skip argument evaluation | Zones, frame marks, and thread names appear |
| Runtime smoke | Editor and Game launch normally | Editor and Game connect, capture, and shut down normally |

## Definition of Done

- Repository-owned runtime-selection terminology consistently uses
  `runtime variant`.
- No supported preset, BuildTool command, DHT command, generated schema, or
  current owning document requires the old profile terminology.
- Tracy is pinned, license-accounted, and excluded from default setup.
- Ordinary builds neither require nor link Tracy.
- Dedicated Release Profiling presets build and run with isolated outputs.
- Shipping cannot enable Tracy.
- A real capture demonstrates frame, thread, task, and selected CPU-phase data.
- Required focused tests and full-build validation pass through documented
  repository entrypoints.
- Long-lived behavior is documented outside the plan and the completed plan is
  archived.

## Deferred Follow-ups

- Vulkan GPU context and command-buffer zones.
- Allocation tracking at engine allocator boundaries.
- Lock contention instrumentation after the multithreading contracts stabilize.
- Optional call-stack and sampling presets with measured overhead.
- Remote capture policy and non-localhost workflows.
- Automated capture regression tooling and performance budgets.
- Managed installation or launch of the Tracy profiler GUI.

## Related Documentation

- [Workspace And Projects](../Workspace/WorkspaceProjects.md)
- [Runtime Variants](../Development/Build/RuntimeVariants.md)
- [CPU Profiling](../Development/Build/Profiling.md)
- [Build System](../Development/Build/BuildSystem.md)
- [Build And Run](../Development/Build/BuildAndRun.md)
- [Third-Party Bootstrap](../Development/Build/ThirdPartyBootstrap.md)
- [C++ Coding Standards](../Development/Standards/CodingStandards.md)

## Related Code

- `CMakePresets.json`
- `CMake/Config/BuildOptions.cmake`
- `CMake/Project/ProjectSetup.cmake`
- `CMake/Project/ProjectTargets.cmake`
- `Engine/CMake/ThirdParty/`
- `Engine/Scripts/Bootstrap/setup_third_party.py`
- `Engine/Scripts/Bootstrap/thirdparty/`
- `Engine/Scripts/Build/AgentBuildProfiles.json`
- `Engine/Scripts/Build/durin_build_tool/`
- `Engine/Source/Programs/DurinHeaderTool/durin_header_tool/`
- `Engine/Source/Runtime/Core/`
- `Engine/Source/Runtime/Launch/`
