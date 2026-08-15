# macOS Platform Enablement Roadmap

Summary: Bring Durin to supported Apple Silicon macOS editor and game execution through bounded build, platform, rendering, asset, and distribution milestones.

Last reviewed: 2026-08-16

Status: Active
Completed:

## Current Status

Durin is a Windows-qualified engine with partial macOS scaffolding. CMake
recognizes `APPLE`, Core contains initial macOS type, dynamic-library, aligned
allocation, and thread-ID support, GLFW exposes Cocoa windows and creates
Vulkan surfaces, and the Slang import target has a dylib branch. These pieces
have no macOS build or runtime qualification and do not constitute platform
support.

Known blockers include an absent macOS `PlatformProcess` implementation,
unfiltered compilation of platform-specific source directories, Windows crash
handling referenced from common launch code, Windows-only dependency manifests,
Win32-specific Vulkan presentation admission, unsupported native file dialogs,
and cooked payload contracts that name only Win64.

[macOS Host-Independent Preparation](../Plans/Archive/2026-08/MacOSHostIndependentPreparation.md)
completed M0 on the Windows qualification host. Platform source ownership,
neutral Launch/Vulkan boundaries, target-aware DHT preprocessing, arm64 preset
intent, dependency diagnostics, cook inventories, and the first-host checklist
are qualified. An Apple Silicon development host is now available, so
[macOS Native Toolchain Bootstrap](../Plans/MacOSNativeToolchainBootstrap.md)
is active. M1 begins by freezing the native baseline, adding the repository-root
POSIX DevTool entrypoint, and making setup diagnostics host-aware before
dependency and bounded compile qualification.

## Outcome

Durin Editor and Durin Game configure, build, launch, render, load compatible
cooked content, and shut down correctly on a declared Apple Silicon macOS
baseline. The supported path uses the existing Vulkan RHI through MoltenVK,
keeps platform-specific operating-system code behind explicit boundaries, and
has native automated and runtime evidence rather than cross-platform compile
assumptions.

## Scope

- Apple Silicon host and target discovery, dependency preparation, CMake
  presets, compiler settings, runtime layout, and developer workflow.
- macOS process, module, window, input, file-dialog, project-ownership, path,
  crash-diagnostic, and relaunch behavior required by Editor and Game.
- Vulkan instance, physical-device, surface, presentation, swapchain, resource,
  shader, synchronization, and diagnostics behavior through MoltenVK.
- Asset compatibility and cooking decisions for textures, geometry, animation,
  environment lighting, and shaders.
- Native tests, Editor and Game runtime smoke, `.app` layout, dylib resolution,
  signing, and supported-machine qualification.

## Non-Goals

- Intel Mac, universal binaries, iOS, iPadOS, or visionOS in the first support
  target.
- A native Metal RHI or replacement of Vulkan/Slang as a prerequisite.
- Cross-compiling or fully qualifying macOS artifacts from Windows.
- Preserving Windows-specific implementation names in platform-neutral APIs.
- Duplicating every cooked payload solely because the operating-system name is
  different when its actual binary and capability contract is portable.

## Program Decisions and Invariants

### Support and evidence boundary

- The first target is Apple Silicon arm64. The exact minimum macOS and Xcode
  versions are frozen in M1 from an available maintained toolchain before
  dependency artifacts are pinned.
- Editor Debug is the first vertical slice. Release Editor and Game follow only
  after the same platform layer and rendering path are stable; Shipping and
  distribution are final qualification work.
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
| Build metadata | CMake detects `APPLE`; macOS presets and output platform names exist. | Presets are stale x64 entries, host tooling is Windows-oriented, and no native compiler/build is qualified. |
| Platform core | macOS types, dylib loading, allocation, case-insensitive compare, and thread IDs exist. | Process services, source-set isolation, crash abstraction, native dialogs, ownership, and shell behavior are incomplete. |
| Window/input | GLFW creates Cocoa no-API windows, exposes the native handle, handles Retina framebuffer sizing, and creates Vulkan surfaces. | Keyboard conventions, IME, DPI, monitors, focus, cursor, and multi-viewport behavior need native evidence. |
| Vulkan RHI | Vulkan 1.1+, swapchain, synchronization, diagnostics, Slang SPIR-V, and portability instance flags exist. | Instance/surface requirements and queue admission are Win32-coupled; MoltenVK capabilities and lifecycle are unqualified. |
| Dependencies | Most libraries build from source and Slang CMake accepts a dylib. | Preparation manifests, Vulkan SDK/MoltenVK layout, arm64 artifacts, dylib deployment, and rpaths are missing. |
| Assets | Versioned cooked payloads and compatibility diagnostics exist. | Cook targets and many geometry/texture/animation/environment checks accept only Win64. |
| Delivery | Runtime variants and isolated binary layouts exist. | `.app` bundle structure, resource discovery, signing, notarization, packaging, and macOS CI are absent. |

## Milestone Map

```mermaid
flowchart LR
    M0["M0: Host-independent preparation"] --> M1["M1: Native toolchain bootstrap"]
    M1 --> M2["M2: Platform runtime and Editor shell"]
    M2 --> M3["M3: MoltenVK rendering vertical slice"]
    M3 --> M4["M4: Asset and cook compatibility"]
    M4 --> M5["M5: Product qualification and distribution"]
```

| Milestone | Requirement | Child plan | Entry gate | Exit gate |
| --- | --- | --- | --- | --- |
| M0: Host-independent preparation | Required; completed | [macOS Host-Independent Preparation](../Plans/Archive/2026-08/MacOSHostIndependentPreparation.md) | Met: Windows build and test environment is available and the principal platform couplings are identifiable statically. | Met: Windows behavior remains qualified; build/source ownership, platform-neutral Vulkan admission models, target-aware generation metadata, and a reproducible first-Mac handoff are complete. |
| M1: Native toolchain bootstrap | Required; active | [macOS Native Toolchain Bootstrap](../Plans/MacOSNativeToolchainBootstrap.md) | Met: M0 is complete and an Apple Silicon Mac is available for repeatable local execution. | Declared Xcode/macOS baseline configures and compiles a bounded Core/ApplicationCore target set with pinned arm64 dependencies and repeatable setup diagnostics. |
| M2: Platform runtime and Editor shell | Required; proposed | `MacOSPlatformRuntime` | M1 toolchain and dependency preparation are stable. | Core process/module/filesystem services and the Editor shell launch, create a Cocoa window, process input, relaunch/open paths, enforce project ownership, and shut down without rendering-backend requirements being bypassed. |
| M3: MoltenVK rendering vertical slice | Required; proposed | `MacOSMoltenVKRendering` | M2 provides a stable window/surface lifecycle and MoltenVK is pinned. | Editor renders and presents representative graphics and compute work with validation diagnostics, resize/minimize/recreate, shader compilation, resource lifetime, and clean shutdown passing on the target Mac. |
| M4: Asset and cook compatibility | Required; proposed | `MacOSAssetCookCompatibility` | M3 publishes exact supported GPU formats and shader/runtime capabilities. | Every currently Win64-guarded runtime payload family has a recorded shared-or-recooked decision, versioned keys, focused tests, and representative Editor/Game load evidence. |
| M5: Product qualification and distribution | Required; proposed | `MacOSProductQualification` | M2-M4 are complete and the supported runtime feature set is frozen. | Debug/Release Editor and Game pass native suites and runtime smoke; Shipping `.app`, dylib closure, resources, signing/notarization policy, installation, upgrade, and supported-machine matrix are documented and verified. |

## Child Plan Boundaries

### [macOS Host-Independent Preparation](../Plans/Archive/2026-08/MacOSHostIndependentPreparation.md)

Owns platform source selection, neutral common APIs, deterministic Vulkan
admission/extension policy models, target-aware DHT configuration, coherent
arm64 preset metadata, and the first-native-host readiness handoff. It does not
add uncompiled Objective-C++ implementations, pin unverified macOS binary URLs,
or claim native support.

### [macOS Native Toolchain Bootstrap](../Plans/MacOSNativeToolchainBootstrap.md)

Owns the supported macOS/Xcode baseline, command entrypoints, arm64 dependency
acquisition/build, Vulkan SDK and MoltenVK layout, Slang dylib selection, CMake
configure, compiler repairs, rpaths for development binaries, and a small native
compile qualification set. It does not implement full Editor services or
rendering correctness.

### `MacOSPlatformRuntime`

Owns macOS process and shell services, platform source implementations, crash
diagnostic policy, project ownership, native dialogs, Cocoa/GLFW behavior,
Editor shell lifecycle, and platform-focused tests. It may launch a backend
diagnostic window but does not weaken or complete MoltenVK device admission.

### `MacOSMoltenVKRendering`

Owns MoltenVK instance/device/surface/swapchain negotiation, portability subset,
feature and format admission, Slang output compatibility, synchronization,
resize and presentation lifecycle, GPU diagnostics, and representative public
RHI plus Editor rendering evidence. It does not decide every asset payload.

### `MacOSAssetCookCompatibility`

Owns the per-payload audit, capability/format identities, MacOS cook target,
derived-data keys, migrations, cross-platform reuse proofs, recook behavior,
and representative project content. It consumes M3's supported rendering
capabilities instead of guessing them in advance.

### `MacOSProductQualification`

Owns the full configuration/runtime matrix, regression and performance budget,
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
| Cook/build -> runtime interpretation | M4 | Per-family compatibility table, stable keys, cross-platform fixtures where shared, recook fixtures where distinct, and representative project load. |
| Build output -> distributable application | M5 | Clean-machine `.app` launch, dependency closure audit, signing/notarization evidence, configuration matrix, and runtime smoke. |

All build and native-test execution follows the repository [build and run](../Development/Build/BuildAndRun.md)
and [native testing](../Development/Build/NativeTests.md) contracts rather than
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

- M0-M5 meet their exit gates and every proposed child plan is completed or
  replaced with linked provenance.
- The supported Apple Silicon macOS/Xcode matrix and known limitations are
  explicit and exercised by CI or a repeatable qualification worker.
- Editor and Game build, launch, render representative workloads, consume
  compatible cooked content, and shut down in all declared configurations.
- Durable platform, build, rendering, asset, and distribution contracts live in
  their authoritative documentation domains.
- Windows qualification remains green and platform-neutral APIs contain no
  Win32-only semantic requirements.

## Related Documentation

- [Build System](../Development/Build/BuildSystem.md)
- [Third-Party Dependency Preparation](../Development/Build/ThirdPartyBootstrap.md)
- [Viewport Rendering](../Runtime/Rendering/ViewportRendering.md)
- [Shader Parameters](../Runtime/Rendering/ShaderParameters.md)
- [Asset Versioning](../Runtime/Assets/Versioning.md)

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
