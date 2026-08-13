# macOS Host-Independent Preparation Plan

Summary: Prepare and verify platform boundaries, portable Vulkan admission, target-aware tooling, and the first-Mac bring-up contract using the current Windows host.

Last reviewed: 2026-08-13

Status: Active
Completed:

## Current Status

Implementation has not started. Static inspection confirms useful macOS
scaffolding, but common build and runtime paths still contain Windows-specific
source ownership, process/crash assumptions, Vulkan presentation semantics,
generator macros, dependency manifests, and cooked-target checks.

This plan is M0 of the
[macOS Platform Enablement roadmap](../Roadmaps/MacOSPlatformEnablement.md).
Every implementation stage is intentionally executable and qualifiable on the
current Windows host. The plan ends with a native-host handoff, not with a
macOS support claim.

## Goal

Make the repository structurally ready for a first Apple Silicon bring-up
without changing supported Windows behavior. Common code and deterministic
tests express platform-neutral contracts; build metadata selects explicit
platform source sets; DHT can model a MacOS target without defining Win32/MSVC;
presets describe the intended arm64 Editor bootstrap; and the next plan receives
an exact, evidence-backed native qualification checklist.

## Scope

- Inventory and classify current Windows/macOS coupling in CMake, Core, Launch,
  ApplicationCore, VulkanRHI, DHT, dependency preparation, and cooked payloads.
- Make module source discovery select common sources plus the active platform
  directory, with deterministic configuration tests.
- Introduce platform-neutral common process/crash-service boundaries where
  needed to remove direct Windows includes from common launch code while
  retaining the existing Windows implementation.
- Rename and test Vulkan physical-device presentation admission and diagnostics
  as platform-neutral concepts; keep the Windows native query as the current
  adapter.
- Extract deterministic Vulkan instance-extension and portability policy that
  can be tested with synthetic Windows and Apple availability inputs.
- Make DHT preprocessing derive target macros from its declared architecture,
  with Win64 and MacOS unit coverage.
- Replace stale macOS preset intent with an Apple Silicon Editor-first bootstrap
  shape and validate preset/schema consistency without attempting a Mac build.
- Record dependency and asset-cook readiness matrices and a precise M1 handoff.

## Non-Goals

- Compiling, linking, launching, or qualifying any macOS binary on Windows.
- Adding Objective-C++ Cocoa dialogs or other native implementations that
  cannot be compiled by the current qualification environment.
- Downloading or pinning macOS Slang, MoltenVK, Vulkan SDK, or other binary
  artifacts before their URLs, hashes, architectures, install names, and
  runtime behavior are verified on a Mac.
- Completing macOS process services, crash diagnostics, window/input behavior,
  rendering, asset cooking, `.app` packaging, signing, or notarization.
- Relaxing a Win64 cooked-payload guard without per-family compatibility
  evidence.
- Supporting Intel Mac, universal binaries, or a native Metal RHI.

## Design Decisions and Invariants

### Qualification boundary

- Windows build, native tests, and runtime smoke remain the only executable
  qualification in this plan. Synthetic target-policy tests are design
  evidence, not native macOS evidence.
- No Mac-specific implementation file is added merely to satisfy an include or
  linker shape. M1 adds and compiles native implementations on the target host.
- Unverified external binary metadata remains an explicit handoff item rather
  than a plausible-looking manifest entry.

### Source and API ownership

- Module discovery recognizes common sources and explicit platform-owned
  subtrees. The active target receives exactly its platform subtree; unknown
  platform directories or a missing required selection fail configuration with
  an actionable diagnostic.
- Common code does not include a `Windows/` implementation header or publish a
  field whose semantics require Win32. Windows adapters retain existing native
  API usage behind neutral contracts.
- The source-selection change must preserve the exact intended Win64 target
  source set except for files proven to have been incorrectly platform-owned.

### Vulkan admission

- Queue-family candidates expose `bSupportsPresentation`, and rejection
  diagnostics describe presentation rather than Win32 presentation. On the
  current platform the value still comes from the existing Win32 Vulkan query.
- Required instance extensions are constructed from the active surface
  provider and backend portability policy, deduplicated deterministically, and
  passed through existing requirement negotiation.
- Apple portability requirements are modeled as policy inputs in M0; native
  loader and MoltenVK availability are verified only in M3.

### Tool and target metadata

- DHT target architecture owns predefined platform/compiler macros. A MacOS
  parse cannot define `_WIN32` or `_MSC_VER`; Win64 output remains stable unless
  a recorded correctness defect requires a versioned parser-context change.
- The first intended preset is Apple Silicon Debug Editor. Release, Game, and
  Shipping presets are introduced after the native bootstrap proves their
  shared assumptions.
- Cook platform work remains an audited follow-up. M0 records which payload
  families are guarded and what evidence each needs; it does not equate
  `MacOS` with Win64 or create duplicate data prematurely.

## Current Foundations and Gaps

| Boundary | Foundation usable on Windows | Preparation gap |
| --- | --- | --- |
| CMake platform | `DURIN_TARGET_PLATFORM` already maps `APPLE` to `MacOS`. | Recursive module globs do not exclude the other platform's private sources; presets still describe x64. |
| Core platform | macOS type/misc/LTS headers exist and Windows process services are isolated in concrete files. | `MacOSPlatformProcess` is absent and common Launch directly names the Windows crash handler. |
| Window/surface | GLFW owns the Cocoa window path and `glfwCreateWindowSurface`. | Native behavior is unqualified; required-extension and presentation-admission policy remain coupled to Win32 elsewhere. |
| Vulkan selection | Physical-device evaluation is already mostly a pure, testable model. | Candidate fields, queries, and diagnostics require Win32 presentation; Apple portability extension policy is incomplete. |
| DHT | CLI accepts `Win64`, `Linux`, and `MacOS`; phase identity includes platform. | Hermetic Clang arguments always define `_MSC_VER` and `_WIN32`. |
| Dependencies | Most packages are source based; Slang CMake has dylib lookup. | Slang preparation and development-tool manifests are Win64-only; no verified arm64/MoltenVK closure exists. |
| Cooked assets | Payloads carry explicit target identity and reject mismatches. | Texture, geometry, animation, terrain, and environment paths are Win64-only and lack a compatibility audit. |

## Implementation Stages

### Stage 0: Freeze the preparation contract and inventories

- [ ] Produce a checked inventory of platform-owned source directories,
  common files that include Windows implementation headers, platform process
  call sites, crash-handler call sites, Vulkan Win32 semantic fields, and DHT
  predefined macros.
- [ ] Record the current Win64 module source-set baseline so later source
  filtering can prove that supported Windows behavior was not accidentally
  dropped.
- [ ] Produce dependency readiness entries for Vulkan SDK/MoltenVK, VMA, Slang,
  GLFW, spdlog, rapidyaml, Assimp, GLM, GoogleTest, Tracy, and build tools,
  distinguishing source-build candidates from native artifacts requiring M1
  verification.
- [ ] Produce a payload-family audit for texture 2D/cube, static/skeletal mesh,
  animation, terrain, environment lighting, collision, and shaders, naming the
  current platform marker, format assumptions, likely shareability, and M4
  evidence requirement.
- [ ] Confirm the M0/M1 boundary against the roadmap and record any newly found
  blocker that cannot be made testable on Windows.

#### Acceptance Gate

- Source, API, dependency, Vulkan, generator, and cook inventories account for
  every known Win32/Win64 coupling in scope; the Win64 source baseline is
  machine-comparable; and no unresolved item is silently assigned to both M0
  and a native-host plan.

### Stage 1: Enforce platform source and common-service boundaries

- [ ] Add one CMake-owned source classification/selection helper used by Durin
  modules: common sources plus the selected platform subtree, with explicit
  treatment of headers required for IDE visibility.
- [ ] Add configuration-level tests for Win64 selection, synthetic MacOS
  selection, foreign-platform exclusion, unknown platform diagnostics, and the
  recorded Win64 source-set baseline.
- [ ] Introduce a platform-neutral process/crash-service surface sufficient for
  common Launch code to stop including `Windows/WindowsProcessCrashHandler.h`;
  preserve the current Windows crash implementation and behavior behind it.
- [ ] Make required versus optional platform services explicit so M1 cannot
  satisfy process, module, or filesystem requirements with silent generic
  no-ops.
- [ ] Update module/source ownership documentation only where the implemented
  selection contract becomes lasting behavior.

#### Acceptance Gate

- The Windows build graph contains its complete recorded source set and no
  MacOS implementation sources; synthetic MacOS selection contains common and
  MacOS sources but no Windows implementation sources; common Launch code has
  no Windows implementation include; existing Windows crash qualification
  remains green.

### Stage 2: Remove Win32 semantics from portable Vulkan admission

- [ ] Rename queue candidate presentation state and evaluation diagnostics from
  Win32-specific to platform-neutral terms without changing Windows admission
  results or device preference ordering.
- [ ] Keep platform-native presentation probing in a focused Windows adapter or
  conditional boundary, and make the future surface-based MacOS probe location
  explicit without adding an uncompiled implementation.
- [ ] Extract deterministic instance-extension request construction from
  runtime enumeration, accepting surface-provider requirements and portability
  policy as inputs before existing negotiation.
- [ ] Add focused unit cases for required-extension deduplication, missing
  surface requirements, portability enumeration requirements, optional
  diagnostics, presentation rejection, and unchanged Win64 candidate ranking.
- [ ] Remove Win32 presentation terminology from platform-neutral test names,
  data structures, and diagnostics.

#### Acceptance Gate

- Pure Vulkan admission tests express presentation and portability without a
  Win32 semantic dependency; the Windows adapter still supplies the same native
  support facts; all existing suitable/unsuitable GPU decisions and diagnostics
  remain deterministic.

### Stage 3: Make generation and preset intent target-aware

- [ ] Refactor DHT hermetic Clang arguments so `Win64` and `MacOS` select
  explicit, non-overlapping predefined macro sets and compiler assumptions.
- [ ] Version parser context when required and add tests proving Win64
  generation stability, MacOS platform branches, phase-cache identity, and
  rejection of undeclared preprocessor dependencies.
- [ ] Replace the stale x64 macOS preset pair with one clearly named Apple
  Silicon Debug Editor bootstrap preset carrying the runtime variant and
  repository-standard cache settings.
- [ ] Add preset/tooling validation that can run on Windows without invoking a
  macOS compiler and distinguishes schema validity from native availability.
- [ ] Make dependency-preparation diagnostics enumerate the missing MacOS
  manifest/platform entries needed by M1; do not add unverified archive URLs or
  hashes.

#### Acceptance Gate

- DHT tests demonstrate truthful and isolated Win64/MacOS preprocessing;
  existing Win64 generated behavior remains qualified; CMake preset parsing and
  repository metadata validation accept the arm64 Editor bootstrap intent; no
  documentation or command reports that a native Mac build has passed.

### Stage 4: Qualify Windows and publish the first-Mac handoff

- [ ] Run the focused CMake/tooling, DHT, Core/Launch, RHI, and Vulkan test
  owners selected through the repository testing workflow.
- [ ] Run the native aggregate, full Debug Editor build, and hidden-window
  runtime smoke required by the repository build workflow because M0 changes
  build graph and startup/RHI boundaries.
- [ ] Compare the final Win64 source graph and Vulkan admission outcomes to the
  Stage 0 baseline and account for every intended difference.
- [ ] Record M1's exact first-host sequence: supported-baseline decision,
  dependency artifact verification, bounded configure/compile targets, expected
  missing native implementations, log capture, and stop/rollback conditions.
- [ ] Update this plan and the roadmap with validation evidence and activate M1
  only when an Apple Silicon worker is actually available.

#### Acceptance Gate

- All required Windows qualification passes, the baseline comparison contains
  no unexplained source or admission changes, the native-host checklist is
  executable without rediscovering M0 decisions, and the repository still
  describes macOS as pending native qualification.

## Validation Matrix

| Change boundary | Windows-host evidence | Deferred native evidence |
| --- | --- | --- |
| CMake source selection | Pure classification/configuration tests, generated graph comparison, full Editor build. | Apple toolchain compiles the selected MacOS source set in M1. |
| Process/crash facade | Existing Windows crash policy/fixture tests and startup smoke. | macOS process and crash implementation tests in M2. |
| Vulkan presentation model | Pure candidate/negotiation tests and Windows Vulkan integration tests. | Surface-support query and MoltenVK device admission in M3. |
| DHT target context | Python unit tests for Win64/MacOS macros, generation, and phase identity. | Native Clang compile of generated modules in M1. |
| Preset/tool metadata | JSON/CMake schema and repository tooling tests. | Configure and build with Xcode/Apple Clang/Ninja in M1. |
| Dependency readiness | Manifest validation and explicit missing-platform diagnostics. | Download/hash/architecture/install-name/runtime verification in M1. |
| Cook audit | Complete inventory and compatibility evidence requirements. | Per-family cook/load decisions and fixtures in M4. |
| Regression closure | Focused suites, native aggregate, Debug Editor build, hidden-window smoke. | Cross-platform CI matrix beginning in M1 and expanding through M5. |

Validation commands and target selection follow [Agent Build and Run](../Agents/BuildAndRun.md)
and [Agent Testing](../Agents/Testing.md); this plan records results rather than
duplicating command recipes.

## Definition of Done

- Stages 0-4 pass their acceptance gates and checklists reflect actual evidence.
- Common source and API boundaries no longer require Windows implementation
  semantics where M0 has selected a neutral contract.
- Portable Vulkan admission and DHT target behavior have focused deterministic
  coverage for synthetic Apple inputs while Windows behavior stays qualified.
- The intended Apple Silicon Debug Editor preset and dependency diagnostics are
  internally coherent without unverified binary pins.
- The M1 handoff names every known missing native implementation and the exact
  evidence needed to select toolchain/dependency versions.
- No macOS build, runtime, Cook, or product-support claim is made by completion
  of this plan.

## Deferred Follow-ups

- Native compiler repairs, dependency retrieval, MoltenVK installation, dylib
  rpaths, and macOS `PlatformProcess` implementation belong to M1/M2.
- Cocoa dialogs, project ownership, Finder integration, input/IME, crash
  diagnostics, and Editor shell behavior belong to M2.
- Surface-based queue probing, MoltenVK feature/format qualification, rendering,
  synchronization, and resize/present lifecycle belong to M3.
- Cook platform enums, capability identities, payload migrations, derived-data
  keys, and representative content belong to M4.
- `.app`, signing, notarization, release configurations, performance, and CI
  support matrix belong to M5.

## Related Documentation

- [macOS Platform Enablement Roadmap](../Roadmaps/MacOSPlatformEnablement.md)
- [Build System](../Development/Build/BuildSystem.md)
- [Third-Party Dependency Preparation](../Development/Build/ThirdPartyBootstrap.md)
- [Viewport Rendering](../Runtime/Rendering/ViewportRendering.md)
- [Shader Parameters](../Runtime/Rendering/ShaderParameters.md)
- [Asset Versioning](../Runtime/Assets/Versioning.md)

## Related Code

- `CMakePresets.json`
- `CMake/Config/Toolchains.cmake`
- `CMake/Project/ProjectTargets.cmake`
- `Engine/Source/Runtime/Core/Public/HAL/PlatformProcess.h`
- `Engine/Source/Runtime/Core/Public/MacOS/`
- `Engine/Source/Runtime/Launch/Private/EngineLoop.cpp`
- `Engine/Source/Runtime/ApplicationCore/Private/Window/GlfwWindow.cpp`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanDevice.cpp`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanDynamicRHI.cpp`
- `Engine/Source/Programs/DurinHeaderTool/durin_header_tool/parser/clang_context.py`
- `Tools/DurinDevTool/durin_dev_tool/bootstrap/thirdparty/`

