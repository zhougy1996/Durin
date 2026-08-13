# macOS Host-Independent Preparation Plan

Summary: Prepare and verify platform boundaries, portable Vulkan admission, target-aware tooling, and the first-Mac bring-up contract using the current Windows host.

Last reviewed: 2026-08-13

Status: Completed
Completed: 2026-08-13

## Current Status

Stages 0-4 are complete on the Windows qualification host. Module source
selection is platform-aware, common Launch uses a required platform-neutral
crash service, Vulkan admission and extension policy are portable models, DHT
target macros and triples are truthful, and the only macOS preset is the Apple
Silicon Debug Editor bootstrap intent. Focused Python/CMake/native tests, the
native aggregate, complete Debug Editor build, Windows crash characterization,
and hidden-window runtime smoke passed. macOS remains pending native
qualification; M1 is not active because no Apple Silicon worker was available.

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

## M0 Evidence Inventories

### Source, API, Vulkan, and generator inventory

- Platform-owned source roots at the Stage 0 baseline were Core
  `Private/Windows`, Core `Public/Windows`, Core `Public/MacOS`, and Launch
  `Private/Windows`. There were no MacOS implementation translation units.
- Common Windows implementation includes were the three Launch owners
  `ApplicationRunner.cpp`, `EngineLoop.cpp`, and
  `Diagnostics/ApplicationDiagnostics.cpp`; Core's `FileHelper.cpp` and
  `Project.cpp` also contain explicitly conditional Windows implementation
  includes. Launch now uses `ProcessCrashServices.h`; the concrete forwarding
  implementation remains under `Private/Windows`.
- Required process calls are `WaitForProcessExit`, current-process identity,
  runtime binary search, relaunch, and the crash install/root/fixture lifecycle.
  The crash service intentionally has no generic fallback, so a future target
  without an implementation fails to link instead of silently disabling it.
- Vulkan Win32 coupling was limited to `bSupportsWin32Presentation`, its
  rejection diagnostic, the Win32 queue-family query, and hard-coded Win32
  instance extensions. The field and diagnostic are now presentation-neutral;
  the native query is isolated in the Windows subtree; extension construction
  accepts surface-provider and portability-policy inputs.
- DHT formerly defined `_MSC_VER=1930` and `_WIN32=1` for every target. Win64
  now owns `_MSC_VER`, `_WIN32`, and `_WIN64`; MacOS owns `__APPLE__`,
  `__MACH__`, `__arm64__`, and `__aarch64__`; Linux owns its declared synthetic
  context. Explicit Clang target triples prevent host built-ins from leaking
  between them. Parser context `target-predefines-v3` invalidates earlier phase data.
- The Stage 0 Win64 module compilation manifest contained 427 sorted
  repository-relative translation-unit paths with LF-delimited SHA-256
  `4b7c2552c2fe1a5440539b5c3466e7be719073e4448182aa9cc455d8fc4d3fa2`.
  The final manifest contains 429 paths with SHA-256
  `ea04c67d103a50d6a66b64fee27f655b182899811708281e5e23ba8f66c39897`;
  the only additions are the Windows crash-service and Vulkan presentation
  adapters. The CMake contract test checks both manifests.

### Dependency readiness

| Dependency | Current form | M0 disposition | M1 native evidence |
| --- | --- | --- | --- |
| Vulkan SDK / MoltenVK | Windows SDK outside bootstrap; no Mac manifest | Native artifact blocker | Version, arm64 slices, headers/loader, MoltenVK dylib/install names, validation-layer behavior. |
| Vulkan Memory Allocator | Supplied by the Vulkan SDK | Source-compatible candidate, coupled to SDK | Header version and native compile against selected SDK. |
| Slang | Win64 prebuilt archive | Native artifact blocker; diagnostic names both missing MacOS fields | URL/hash, arm64 dylibs, install names, compiler execution, SPIR-V output. |
| GLFW | Git source, shared install | Source-build candidate | Cocoa/Vulkan configure, arm64 link, surface creation. |
| spdlog | Git source, shared install | Source-build candidate | Apple Clang configure/link and runtime logging. |
| rapidyaml | Git source, shared install | Source-build candidate | Apple Clang configure/link. |
| Assimp | Git source, shared install | Source-build candidate | Apple Clang configure/link and representative import. |
| GLM | Git direct source | Header-only candidate | Apple Clang compile. |
| GoogleTest | Git direct source, test-only | Source-build candidate | Native test compile/run. |
| Tracy | Git direct source; Win64 tools archive | Client source candidate; host tools optional and explicitly skippable | Client compile; decide whether verified macOS profiler tools are needed. |
| Build tools | CMake/Ninja/Python/Git plus Apple Clang/Xcode | Preset/schema intent only | Exact maintained versions, command paths, arm64 host identity, SDK selection. |

### Cooked payload readiness

| Family | Current marker and assumptions | Likely disposition | M4 evidence requirement |
| --- | --- | --- | --- |
| Texture 2D / cube | `ECookTargetPlatform::Win64`; versioned mips using stable BC1/3/5/7 and uncompressed format IDs | Capability-dependent, not OS-name-dependent | MoltenVK format/sample support, byte fixtures, cross-host cook/load equality. |
| Static mesh | `EStaticMeshTargetPlatform::Win64`; versioned vertex/index/submesh and bounds payload | Potentially shared | Endianness/width/alignment fixtures and native upload/render. |
| Skeletal mesh | `ESkeletalPayloadTargetPlatform::Win64`; joints, weights, inverse bind and render buffers | Potentially shared | Numeric layout/alignment fixtures, skinning upload and representative animation. |
| Animation / skeleton | Win64 cooked-bulk and skeletal marker | Potentially shared | Deterministic key/pose fixtures and native playback. |
| Terrain | Win64 cooked-bulk marker; height samples plus render payload | Potentially shared with GPU constraints | Height serialization fixture, selected GPU format, native render. |
| Environment lighting | Win64 cooked-bulk marker; texture-backed lighting data | Capability-dependent | Texture-format support, orientation/value fixtures, native lighting render. |
| Collision | Embedded in Win64 static-mesh descriptors | Architecture/layout-sensitive until proven | Canonical wire bytes, alignment/endianness, native queries against representative meshes. |
| Shaders | Slang-generated Vulkan SPIR-V and reflection cache, no Mac cook identity | Backend-shareable candidate | Same compiler/version/options key, MoltenVK module creation, reflection and representative graphics/compute. |

No payload guard is relaxed by M0. The M0/M1 boundary remains unchanged: all
native artifacts and implementations await an available Apple Silicon worker;
no newly discovered item requires untestable Mac code in this plan.

## Implementation Stages

### Stage 0: Freeze the preparation contract and inventories

- [x] Produce a checked inventory of platform-owned source directories,
  common files that include Windows implementation headers, platform process
  call sites, crash-handler call sites, Vulkan Win32 semantic fields, and DHT
  predefined macros.
- [x] Record the current Win64 module source-set baseline so later source
  filtering can prove that supported Windows behavior was not accidentally
  dropped.
- [x] Produce dependency readiness entries for Vulkan SDK/MoltenVK, VMA, Slang,
  GLFW, spdlog, rapidyaml, Assimp, GLM, GoogleTest, Tracy, and build tools,
  distinguishing source-build candidates from native artifacts requiring M1
  verification.
- [x] Produce a payload-family audit for texture 2D/cube, static/skeletal mesh,
  animation, terrain, environment lighting, collision, and shaders, naming the
  current platform marker, format assumptions, likely shareability, and M4
  evidence requirement.
- [x] Confirm the M0/M1 boundary against the roadmap and record any newly found
  blocker that cannot be made testable on Windows.

#### Acceptance Gate

- Source, API, dependency, Vulkan, generator, and cook inventories account for
  every known Win32/Win64 coupling in scope; the Win64 source baseline is
  machine-comparable; and no unresolved item is silently assigned to both M0
  and a native-host plan.

### Stage 1: Enforce platform source and common-service boundaries

- [x] Add one CMake-owned source classification/selection helper used by Durin
  modules: common sources plus the selected platform subtree, with explicit
  treatment of headers required for IDE visibility.
- [x] Add configuration-level tests for Win64 selection, synthetic MacOS
  selection, foreign-platform exclusion, unknown platform diagnostics, and the
  recorded Win64 source-set baseline.
- [x] Introduce a platform-neutral process/crash-service surface sufficient for
  common Launch code to stop including `Windows/WindowsProcessCrashHandler.h`;
  preserve the current Windows crash implementation and behavior behind it.
- [x] Make required versus optional platform services explicit so M1 cannot
  satisfy process, module, or filesystem requirements with silent generic
  no-ops.
- [x] Update module/source ownership documentation only where the implemented
  selection contract becomes lasting behavior.

#### Acceptance Gate

- The Windows build graph contains its complete recorded source set and no
  MacOS implementation sources; synthetic MacOS selection contains common and
  MacOS sources but no Windows implementation sources; common Launch code has
  no Windows implementation include; existing Windows crash qualification
  remains green.

### Stage 2: Remove Win32 semantics from portable Vulkan admission

- [x] Rename queue candidate presentation state and evaluation diagnostics from
  Win32-specific to platform-neutral terms without changing Windows admission
  results or device preference ordering.
- [x] Keep platform-native presentation probing in a focused Windows adapter or
  conditional boundary, and make the future surface-based MacOS probe location
  explicit without adding an uncompiled implementation.
- [x] Extract deterministic instance-extension request construction from
  runtime enumeration, accepting surface-provider requirements and portability
  policy as inputs before existing negotiation.
- [x] Add focused unit cases for required-extension deduplication, missing
  surface requirements, portability enumeration requirements, optional
  diagnostics, presentation rejection, and unchanged Win64 candidate ranking.
- [x] Remove Win32 presentation terminology from platform-neutral test names,
  data structures, and diagnostics.

#### Acceptance Gate

- Pure Vulkan admission tests express presentation and portability without a
  Win32 semantic dependency; the Windows adapter still supplies the same native
  support facts; all existing suitable/unsuitable GPU decisions and diagnostics
  remain deterministic.

### Stage 3: Make generation and preset intent target-aware

- [x] Refactor DHT hermetic Clang arguments so `Win64` and `MacOS` select
  explicit, non-overlapping predefined macro sets and compiler assumptions.
- [x] Version parser context when required and add tests proving Win64
  generation stability, MacOS platform branches, phase-cache identity, and
  rejection of undeclared preprocessor dependencies.
- [x] Replace the stale x64 macOS preset pair with one clearly named Apple
  Silicon Debug Editor bootstrap preset carrying the runtime variant and
  repository-standard cache settings.
- [x] Add preset/tooling validation that can run on Windows without invoking a
  macOS compiler and distinguishes schema validity from native availability.
- [x] Make dependency-preparation diagnostics enumerate the missing MacOS
  manifest/platform entries needed by M1; do not add unverified archive URLs or
  hashes.

#### Acceptance Gate

- DHT tests demonstrate truthful and isolated Win64/MacOS preprocessing;
  existing Win64 generated behavior remains qualified; CMake preset parsing and
  repository metadata validation accept the arm64 Editor bootstrap intent; no
  documentation or command reports that a native Mac build has passed.

### Stage 4: Qualify Windows and publish the first-Mac handoff

- [x] Run the focused CMake/tooling, DHT, Core/Launch, RHI, and Vulkan test
  owners selected through the repository testing workflow.
- [x] Run the native aggregate, full Debug Editor build, and hidden-window
  runtime smoke required by the repository build workflow because M0 changes
  build graph and startup/RHI boundaries.
- [x] Compare the final Win64 source graph and Vulkan admission outcomes to the
  Stage 0 baseline and account for every intended difference.
- [x] Record M1's exact first-host sequence: supported-baseline decision,
  dependency artifact verification, bounded configure/compile targets, expected
  missing native implementations, log capture, and stop/rollback conditions.
- [x] Update this plan and the roadmap with validation evidence and activate M1
  only when an Apple Silicon worker is actually available.

#### Acceptance Gate

- All required Windows qualification passes, the baseline comparison contains
  no unexplained source or admission changes, the native-host checklist is
  executable without rediscovering M0 decisions, and the repository still
  describes macOS as pending native qualification.

## M1 First-Host Handoff

Run this sequence only on a repeatable Apple Silicon worker. Each step records
commands, tool versions, stdout/stderr, and resolved paths before continuing.

1. Freeze the candidate baseline: record macOS version, Apple Silicon model,
   Xcode/Apple Clang, SDK, CMake, Ninja, Python, Git, Vulkan loader/MoltenVK,
   and Slang versions. Stop if the host is Intel, translated, unmanaged, or
   cannot be reset to a known dependency state.
2. Verify artifacts outside the repository first: retrieve candidate Vulkan
   SDK/MoltenVK and Slang archives, verify publisher, hash, arm64 slices,
   headers, dylib install names/rpaths, loader discovery, and executable
   behavior. Only then add exact MacOS manifest entries in a bounded M1 change.
3. Run dependency manifest validation, then prepare source candidates one at a
   time: GLM, spdlog, rapidyaml, GLFW, Assimp, GoogleTest, Tracy client, VMA via
   the selected SDK, followed by the verified Slang and Vulkan artifacts. Stop
   at the first architecture, compiler, deployment, or checksum mismatch.
4. Parse `CMakePresets.json`, select
   `MacOS-arm64-Debug-DurinEditor`, and configure without adding Release, Game,
   Shipping, Intel, or universal presets. Preserve the complete configure log.
5. Inspect the generated graph before compilation. It must contain common and
   MacOS sources, exclude Windows implementation translation units, define no
   `_WIN32`/`_MSC_VER` in MacOS DHT commands, and target arm64 consistently.
6. Compile in bounded order: DHT generation, Core headers/common sources,
   ApplicationCore/GLFW, then the smallest Editor bootstrap closure. Expected
   first missing implementations are MacOS `FPlatformProcess`, the required
   Launch crash-service adapter, and the Vulkan native presentation adapter.
   Implement them only in their MacOS subtrees under M1/M2 ownership; do not add
   generic no-ops or weaken source filtering.
7. After each repair, rerun only the failed configure/build target and retain
   the first and final diagnostics. Stop and return to plan selection if a
   required engine contract would need weakening, MoltenVK lacks a required
   capability, or an artifact's provenance/architecture cannot be verified.
8. M1 may advance only after clean configure plus its bounded compile gate is
   reproducible from a fresh worktree. Runtime windowing, crash qualification,
   rendering, Cook compatibility, packaging, signing, and support claims remain
   owned by M2-M5.

Rollback means removing only M1-added unverified manifest/preset changes and
the worktree-local build/install outputs, preserving these M0 boundaries and
captured logs for the next baseline decision.

## Completion Evidence

- CMake platform-source contract passed Win64/MacOS selection, foreign
  implementation exclusion, unknown-target diagnostics, and both recorded
  source-manifest hashes.
- DHT Python suite: 159 passed. DurinDevTool Python suite: 353 passed. The
  focused combined target/preset/dependency selection was 63 passed.
- Vulkan focused coverage: six instance-negotiation and four device-candidate
  cases passed, including portability request construction, deduplication,
  presentation rejection, and deterministic ranking.
- Launch crash policy focus and the complete
  `NativeCrashCharacterizationTests` characterization target passed.
- `DurinNativeTests` aggregate built and all ordinary target-level native tests
  passed. Qualification/characterization-only coverage remained separately
  selected according to repository policy.
- The complete `Win64-Debug-DurinEditor` `all` target built successfully.
  `DurinEditor` then launched the Sandbox project with `--hidden-window
  --exit-after-ticks=5` and exited successfully.
- Final source graph difference from Stage 0 is exactly two platform-owned
  Win64 translation units: the crash-service adapter and Vulkan presentation
  adapter. No MacOS or Linux translation unit appears in the configured Win64
  graph, and Vulkan admission decisions remain unchanged apart from neutral
  terminology and the new pure extension-policy input.

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
