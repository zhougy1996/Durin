# macOS Platform Enablement Roadmap

Summary: Establish a qualified Apple Silicon macOS development path for the Durin Editor, with product cooking and distribution explicitly deferred until selected.

Last reviewed: 2026-08-18

Status: Archived
Completed: 2026-08-18

## Current Status

M2 now provides a real macOS process-crash adapter, synchronous process and
open-path services, cross-process project ownership, actionable dylib loading,
native-dialog failure policy, balanced ApplicationCore/GLFW teardown, and a
clean ordinary native aggregate on Apple Silicon. Launch and the complete Debug
Editor closure link as Mach-O arm64 with audited `@rpath` dependencies. The
ordinary Sandbox and Project Browser paths now initialize, render bounded
hidden frames, and shut down cleanly with exit code 0.

The window/surface ordering shared with M3 is now resolved: Launch creates the
final hidden primary Cocoa window before RHI initialization, ApplicationCore
installs its `CAMetalLayer` on the AppKit main thread, and MoltenVK creates the
surface on the RHI thread without mutating the Cocoa view hierarchy. MoltenVK
then admits a surface-qualified device with the portability subset enabled, and
the first viewport reuses that surface. A smoke on the Apple M4 host creates an
sRGB BGRA swapchain, and MonaImGui derives its pipeline and render-pass format
from that actual output. Windows retains its existing Win32
presentation-support query.
Editor-authored asset classes are published before the initial registry scan,
so the Sandbox default level opens normally. A monitor-less session degrades to
single-viewport ImGui without inventing monitor geometry. Operator-driven
input, monitor/Retina, window-management, integrated-title-bar, and
visible-close qualification now completes that native lifecycle evidence.

[macOS Host-Independent Preparation](../../../Plans/Archive/2026-08/MacOSHostIndependentPreparation.md)
completed M0 on the Windows qualification host. Platform source ownership,
neutral Launch/Vulkan boundaries, target-aware DHT preprocessing, arm64 preset
intent, dependency diagnostics, cook inventories, and the first-host checklist
are qualified. An Apple Silicon development host is now available, so
[macOS Native Toolchain Bootstrap](../../../Plans/Archive/2026-08/MacOSNativeToolchainBootstrap.md)
completed M1. Its native setup, dependency, configuration, graph, fresh-
worktree reproduction, bounded Engine compile, durable documentation, and M2
entry diagnostics are qualified without claiming runtime support.
[macOS Platform Runtime](../../../Plans/Archive/2026-08/MacOSPlatformRuntime.md) completed M2. The
landed MoltenVK path, application-hosted Vulkan/window qualification, and
visible 900-tick presenting Editor run complete the M3 development rendering
vertical slice without requiring a separate child plan. On 2026-08-18 the
Windows profile configured cleanly, all 74 ordinary native targets passed, the
platform-source selection audit passed, and the full `all` build remained
clean.

This roadmap closes at the qualified Editor development boundary. M4's broad
cross-platform cook expansion is deferred until a concrete incompatible
payload or Mac cook product requirement is selected; representative Sandbox
content already publishes and loads all 28 packages on the qualified host. M5
Shipping `.app`, signing, notarization, installation, CI, supported-machine,
and full Game/product qualification are deliberately deferred to a future
productization roadmap. This completion does not claim those capabilities.

## Outcome

Durin Editor configures, builds, launches, renders, loads representative
compatible content, and shuts down correctly on the declared Apple Silicon
macOS development baseline. The qualified path uses the existing Vulkan RHI
through MoltenVK, keeps platform-specific operating-system code behind explicit
boundaries, and has native automated, operator, and runtime evidence rather
than cross-platform compile assumptions. Game product support and distribution
remain outside this completed development-enablement outcome.

## Scope

- Apple Silicon host and target discovery, dependency preparation, CMake
  presets, compiler settings, runtime layout, and developer workflow.
- macOS process, module, window, input, file-dialog, project-ownership, path,
  crash-diagnostic, and relaunch behavior required by the Editor development
  path and reusable by a future Game qualification.
- Vulkan instance, physical-device, surface, presentation, swapchain, resource,
  shader, synchronization, and diagnostics behavior needed by the
  representative Editor vertical slice through MoltenVK.
- Representative current Sandbox asset compatibility and explicit disposition
  of broader Mac cook work.
- Native tests, Editor runtime smoke, development-binary layout, dylib
  resolution, and cross-platform regression.

## Non-Goals

- Intel Mac, universal binaries, iOS, iPadOS, or visionOS in the first support
  target.
- A native Metal RHI or replacement of Vulkan/Slang as a prerequisite.
- Cross-compiling or fully qualifying macOS artifacts from Windows.
- Full Game, Release/Shipping, dedicated Mac cook, distributable `.app`,
  signing, notarization, installation, CI, or supported-machine qualification.
- Preserving Windows-specific implementation names in platform-neutral APIs.
- Duplicating every cooked payload solely because the operating-system name is
  different when its actual binary and capability contract is portable.

## Program Decisions and Invariants

### Support and evidence boundary

- The first target is Apple Silicon arm64. The exact minimum macOS and Xcode
  versions are frozen in M1 from an available maintained toolchain before
  dependency artifacts are pinned.
- Editor Debug is the completed vertical slice. Release Editor, Game, Shipping,
  and distribution require a separately selected productization roadmap.
- Windows-only preparation may make code platform-neutral and add deterministic
  tests, but macOS support is not claimed until native configure, compile,
  tests, rendering smoke, and shutdown evidence pass on the declared baseline.

### Platform ownership

- Common modules compile common sources plus exactly one selected platform
  source set. A source file beneath `Private/Windows` or `Private/MacOS` cannot
  enter the other platform's target.
- Common runtime code calls platform-neutral process, crash-diagnostic,
  presentation, and shell-service contracts. Platform names remain in concrete
  implementations and diagnostics that truly describe the native mechanism.
- Unsupported optional editor integration must fail explicitly without
  preventing the minimal Editor render loop from launching. Required process,
  filesystem, module, and presentation services have no silent no-op fallback.

### Rendering strategy

- The existing Vulkan RHI remains authoritative. MoltenVK is the Apple platform
  implementation layer; a Metal RHI is not introduced by this roadmap.
- Vulkan instance extensions come from the active window/surface provider plus
  backend policy. Physical-device admission uses actual surface presentation
  support and portable feature names rather than Win32 presentation fields.
- Portability enumeration/subset requirements and MoltenVK feature limits are
  explicit negotiated requirements. Missing capabilities produce actionable
  rejection diagnostics or a predeclared fallback; tests do not weaken device
  admission merely to reach a frame.

### Asset compatibility

- Cook identity describes interpretation requirements. Platform-independent
  payloads may be shared only when their schema, byte order, alignment, format,
  and runtime capability contract are identical and tested.
- GPU formats, shader binaries, architecture-specific acceleration data, or
  other backend-dependent payloads receive explicit format/capability identity
  and separate derived-data keys where required.
- A new `MacOS` enum value alone is not evidence of compatibility. Each payload
  family records whether it is shared, transformed, or recooked before its
  current Win64 guard is relaxed.

## Current Foundations and Gaps

| Area | Existing foundation | Roadmap gap |
| --- | --- | --- |
| Build metadata | Native arm64 presets, toolchain selection, dependency preparation, and output layout configure and build repeatably. | Release/product matrices remain conditional productization work. |
| Platform core | Process, crash, module, filesystem, dialogs, ownership, shell, and source-set isolation are implemented and qualified. | No development-enablement gap remains. |
| Window/input | Cocoa/GLFW lifecycle, input, Retina/monitor behavior, integrated title bar, visible close, and Vulkan surface handoff are qualified. | Broader machine/display matrices remain product-support work. |
| Vulkan RHI | MoltenVK admits the surface/device, creates and recreates the swapchain, renders the Editor viewport, and shuts down cleanly under automated and visible qualification. | Exhaustive format/performance matrices remain conditional optimization or product work. |
| Dependencies | Native arm64 dependencies, Vulkan SDK/MoltenVK, Slang dylibs, deployment, and development rpaths are qualified. | Distribution-grade closure belongs to future productization. |
| Assets | The Sandbox registry publishes and loads all 28 representative packages on macOS. | A dedicated Mac cook and incompatible-payload expansion is deferred until required. |
| Delivery | Development binaries and their dylib closure launch repeatably on the declared host. | Shipping `.app`, signing, notarization, installation, CI, and support matrices are explicitly deferred. |

## Milestone Map

```mermaid
flowchart LR
    M0["M0: Host-independent preparation"] --> M1["M1: Native toolchain bootstrap"]
    M1 --> M2["M2: Platform runtime and Editor shell"]
    M2 --> M3["M3: MoltenVK rendering vertical slice"]
    M3 -. conditional .-> M4["M4: Asset and cook compatibility"]
    M4 -. conditional .-> M5["M5: Product qualification and distribution"]
```

| Milestone | Requirement | Child plan | Entry gate | Exit gate |
| --- | --- | --- | --- | --- |
| M0: Host-independent preparation | Required; completed | [macOS Host-Independent Preparation](../../../Plans/Archive/2026-08/MacOSHostIndependentPreparation.md) | Met: Windows build and test environment is available and the principal platform couplings are identifiable statically. | Met: Windows behavior remains qualified; build/source ownership, platform-neutral Vulkan admission models, target-aware generation metadata, and a reproducible first-Mac handoff are complete. |
| M1: Native toolchain bootstrap | Required; completed | [macOS Native Toolchain Bootstrap](../../../Plans/Archive/2026-08/MacOSNativeToolchainBootstrap.md) | Met: M0 is complete and an Apple Silicon Mac is available for repeatable local execution. | Met: the declared Xcode/macOS baseline configures and compiles a bounded Core/ApplicationCore target set with pinned arm64 dependencies, repeatable setup diagnostics, and fresh-worktree evidence. |
| M2: Platform runtime and Editor shell | Required; completed | [macOS Platform Runtime](../../../Plans/Archive/2026-08/MacOSPlatformRuntime.md) | Met: M1 toolchain and dependency preparation are stable. | Met: core services and the Editor shell launch, create and service Cocoa windows, process input, relaunch/open paths, enforce ownership, and shut down cleanly. |
| M3: MoltenVK rendering vertical slice | Required; completed in landed runtime/title-bar work | [macOS Platform Runtime](../../../Plans/Archive/2026-08/MacOSPlatformRuntime.md), [macOS Custom Title Bar Bridge](../../../Plans/Archive/2026-08/MacOSCustomTitleBarBridge.md) | Met: M2 provides a stable window/surface lifecycle and MoltenVK is pinned. | Met: application-hosted Vulkan qualification and a visible presenting Editor run cover surface/device/swapchain admission, resize/recreate, representative rendering, and clean shutdown. |
| M4: Asset and cook compatibility | Conditional; deferred | Future `MacOSAssetCookCompatibility` only when an incompatible payload or Mac cook product requirement is selected. | Not selected: representative current content already loads on the qualified host. | Dispositioned: the Sandbox publishes and loads all 28 packages; broader shared-or-recooked decisions remain future product work. |
| M5: Product qualification and distribution | Conditional; deferred | Future `MacOSProductQualification` productization roadmap. | Not selected: no Shipping distribution or supported-machine commitment is in scope. | Dispositioned: `.app` assembly, signing/notarization, installation, CI, Game, and support matrices are explicitly not claimed by this roadmap. |

## Child Plan Boundaries

### [macOS Host-Independent Preparation](../../../Plans/Archive/2026-08/MacOSHostIndependentPreparation.md)

Owns platform source selection, neutral common APIs, deterministic Vulkan
admission/extension policy models, target-aware DHT configuration, coherent
arm64 preset metadata, and the first-native-host readiness handoff. It does not
add uncompiled Objective-C++ implementations, pin unverified macOS binary URLs,
or claim native support.

### [macOS Native Toolchain Bootstrap](../../../Plans/Archive/2026-08/MacOSNativeToolchainBootstrap.md)

Owns the supported macOS/Xcode baseline, command entrypoints, arm64 dependency
acquisition/build, Vulkan SDK and MoltenVK layout, Slang dylib selection, CMake
configure, compiler repairs, rpaths for development binaries, and a small native
compile qualification set. It does not implement full Editor services or
rendering correctness.

### [macOS Platform Runtime](../../../Plans/Archive/2026-08/MacOSPlatformRuntime.md)

Owns macOS process and shell services, platform source implementations, crash
diagnostic policy, project ownership, native dialogs, Cocoa/GLFW behavior,
Editor shell lifecycle, and platform-focused tests. It owns the bounded
surface-first startup handoff and real-surface device admission needed by the
ordinary shell, but does not complete MoltenVK rendering qualification.

### Landed M3 MoltenVK rendering vertical slice

The required development vertical slice landed across the completed platform
runtime, application-host, and custom-title-bar work: MoltenVK
instance/device/surface/swapchain admission, representative public RHI and
Editor rendering, resize/recreate, diagnostics, and clean shutdown are
qualified. Exhaustive feature/format or performance expansion remains separate
future work and does not reopen this roadmap.

### Deferred `MacOSAssetCookCompatibility`

Would own the per-payload audit, capability/format identities, MacOS cook target,
derived-data keys, migrations, cross-platform reuse proofs, recook behavior,
and representative project content. It consumes M3's supported rendering
capabilities instead of guessing them in advance.

### Deferred `MacOSProductQualification`

Would own the full configuration/runtime matrix, regression and performance budget,
`.app` assembly, dylib closure, assets/resources, code signing, notarization,
installation, CI, support diagnostics, and durable user/developer contracts.

## Program Validation Matrix

| Boundary | Required milestone | Evidence |
| --- | --- | --- |
| Common/platform source ownership | M0, M1 | Windows graph excludes Mac sources; macOS graph excludes Windows sources; native compile proves the selected set. |
| Generator target context | M0, M1 | DHT tests cover Win64 and MacOS macro contexts; native generated modules compile without pretending to be MSVC/Win32. |
| Process/module/filesystem lifecycle | M2 | Focused native tests plus Editor launch, relaunch, module load/unload, project ownership, logging, and shutdown smoke. |
| Cocoa window/input -> Vulkan surface | M2, M3 | Retina resize, focus, keyboard/mouse, monitor and multi-viewport fixtures with actual surface recreation. |
| Vulkan/Slang -> MoltenVK/Metal | M3 | Public RHI graphics/compute, resource format, synchronization, shader, presentation, diagnostics, and repeated lifecycle tests. |
| Cook/build -> runtime interpretation | Conditional M4 | Representative current Sandbox packages load; a per-family shared/recooked matrix is deferred until a Mac cook or incompatible payload is selected. |
| Build output -> distributable application | Conditional M5 | Development dylib closure is audited; distributable `.app`, signing/notarization, CI, and supported-machine evidence are deferred and not claimed. |

All build and native-test execution follows the repository [build and run](../../../Development/Build/BuildAndRun.md)
and [native testing](../../../Development/Build/NativeTests.md) contracts rather than
embedding command recipes in child plans.

## Risks and Control Gates

- **No native host:** M0 may finish, but M1 cannot activate and no macOS support
  claim is permitted until a maintained Apple Silicon worker exists.
- **MoltenVK capability mismatch:** M3 records exact feature and format gaps.
  A missing required rendering contract blocks M3 or triggers a separately
  selected fallback plan; admission checks are not silently removed.
- **Unverified dependency artifacts:** binary URLs, hashes, install names, and
  dylib closures are pinned only after retrieval and execution on the native
  host.
- **Cooked-data ambiguity:** M4 blocks relaxation of a Win64 guard until the
  owning payload has byte/schema/format evidence and a versioning decision.
- **Platform leakage:** source graph and architecture tests must make accidental
  compilation or inclusion of the other platform's implementation a failure.
- **Scope expansion:** Intel/universal, native Metal, and other Apple operating
  systems require separate evidence and roadmap decisions.

## Completion Criteria

- Required M0-M3 exit gates pass with linked native and Windows provenance;
  conditional M4-M5 are explicitly dispositioned without unsupported claims.
- The declared Apple Silicon macOS/Xcode development baseline and known
  limitations are explicit and exercised on a repeatable qualification host.
- Editor builds, launches, renders representative workloads, consumes the
  qualified Sandbox content, and shuts down on the declared development
  configuration.
- Durable platform, build, rendering, asset, and distribution contracts live in
  their authoritative documentation domains.
- Windows qualification remains green and platform-neutral APIs contain no
  Win32-only semantic requirements.

## Related Documentation

- [Build System](../../../Development/Build/BuildSystem.md)
- [Third-Party Dependency Preparation](../../../Development/Build/ThirdPartyBootstrap.md)
- [Viewport Rendering](../../../Runtime/Rendering/ViewportRendering.md)
- [Shader Parameters](../../../Runtime/Rendering/ShaderParameters.md)
- [Asset Versioning](../../../Runtime/Assets/Versioning.md)

## Related Code

- `CMakePresets.json`
- `CMake/Config/Toolchains.cmake`
- `CMake/Project/ProjectTargets.cmake`
- `Engine/Source/Runtime/Core/Public/HAL/`
- `Engine/Source/Runtime/Core/Public/MacOS/`
- `Engine/Source/Runtime/ApplicationCore/Private/Window/GlfwWindow.cpp`
- `Engine/Source/Runtime/VulkanRHI/`
- `Engine/Source/Programs/DurinHeaderTool/`
- `Tools/DurinDevTool/durin_dev_tool/bootstrap/thirdparty/`
