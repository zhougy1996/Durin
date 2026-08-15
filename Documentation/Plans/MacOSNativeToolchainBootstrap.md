# macOS Native Toolchain Bootstrap Plan

Summary: Establish a repeatable Apple Silicon macOS DevTool, dependency, configure, and bounded native compile workflow without claiming runtime support.

Last reviewed: 2026-08-16

Status: Active
Completed:

## Current Status

M0 is complete and M1 is active on the first Apple Silicon development host.
Stage 1 is complete: the POSIX launcher, standard-library bootstrap validation,
macOS preflight/toolchain strategy, native virtual environment, inherited or
scripted environment persistence, build profile, and LLDB launch generation
are implemented. A complete non-interactive setup and an immediate idempotent
rerun both pass with the selected Vulkan environment script.

Stage 2 is substantially complete. Slang 2026.5.2 is pinned from its official arm64
archive; GLM, bc7enc_rdo, GoogleTest, spdlog, GLFW, rapidyaml, Assimp, and Tracy
prepare successfully. macOS shared-install validation now uses verified native
artifact names, and Assimp uses the Xcode SDK zlib instead of its Darwin-
incompatible bundled zlib. Win64-only Tracy tools are skipped explicitly. The
configured GLFW import supplies its required Cocoa, IOKit, and CoreFoundation
framework closure; RenderCore links and deploys the pinned Slang dylib.

Stage 3 is complete and Stage 4 is active. `MacOS-arm64-Debug-DurinEditor`
configures successfully. Its
907 generated compile commands all target arm64, contain 75 MacOS or MacOS DHT
sources, and contain no Windows implementation source or Windows target macro.
CoreDObject DHT generation passes, and Core, ApplicationCore, and CoreDObject
compile and link as arm64 dylibs with `@rpath` identities. ApplicationCore links
the selected Vulkan loader plus the GLFW framework closure and records build-
tree runtime and Vulkan SDK rpaths. Focused Core utility and filesystem suites
pass 76 and 35 tests respectively.

The bounded gate required a real basic `FMacOSPlatformProcess` implementation,
portable UTF conversion, Apple pthread identity handling, and several strict-
compiler fixes. A wider `Launch` probe now compiles and links the Engine and
DurinEd dylibs after replacing unsupported libc++ `atomic<shared_ptr>` members
with the repository's portable shared-pointer atomic operations and making
vertex-layout offset narrowing explicit. The probe stops only when linking
Launch against its absent macOS process-crash service, the already assigned M2
platform boundary. The affected SkeletalAsset and Spline qualification test
bodies pass 34/34 and 2/2 assertions respectively, but both executables then
receive `SIGSEGV` after global test teardown; that exit-lifecycle failure remains
runtime work and is not reported as a passing test run. A documented
`DevTool rebuild --target Engine` repetition cleaned 545 CMake-owned files,
fresh-configured the declared preset, and rebuilt the 294-step Engine closure
successfully. The resulting dylib is Mach-O arm64, keeps the
`@rpath/libDurinEditor-Engine.dylib` identity, resolves repository modules
through `@rpath`, and records the Debug runtime directory as an `LC_RPATH`.
No Editor was launched and runtime support is not claimed. Final qualification
and documentation remain open.

The Stage 4 control-plane regression pass is green: all 369 applicable
DurinDevTool tests pass with two host-inapplicable skips, and all 159 DHT tests
pass. The DevTool suite includes the synthetic Windows setup, profile,
dependency, build-graph, launcher, and recovery contracts affected by M1. The
fresh-worktree repetition also exposed and fixed POSIX directory-symlink cleanup
in `DevTool worktree remove`; focused tests prove that detach and replacement
preserve the shared targets, and the temporary worktree was removed safely.

A detached fresh worktree at the Stage 4 commit passed macOS preflight while
being prepared, validated all 10 third-party manifests, idempotently prepared
the complete test/development selection, fresh-configured the declared preset,
parsed all 56 Engine DHT headers on the cold path, and linked the Engine target.
Its output was Mach-O arm64 with the expected `@rpath` identity and worktree-
local Debug runtime `LC_RPATH`. The worktree intentionally shared the already
qualified `.venv`, local configuration, and external dependency tree, so this
is fresh-worktree evidence rather than a second fresh-host acquisition claim.

### Validated candidate host

- Apple Silicon arm64, macOS 26.6.1 (25G76), Xcode 26.6 (17F113), macOS SDK
  26.5, and Apple Clang 21.0.0 targeting `arm64-apple-darwin25.6.0`. The
  selected developer directory is `/Applications/Xcode.app/Contents/Developer`;
  `xcrun` and `clang` resolve from `/usr/bin`.
- CMake 4.4.2 at `/opt/homebrew/Cellar/cmake/4.4.2/bin/cmake`, plus Homebrew
  Ninja 1.13.2, Python 3.12.14, and Git 2.55.0 under `/opt/homebrew/bin`.
- LunarG Vulkan SDK 1.4.357.0 with Vulkan header revision 357. Its Vulkan
  loader and MoltenVK dylibs contain arm64 slices and retain `@rpath` install
  names. `VULKAN_SDK` resolves to
  `/Users/zhougy/Programs/VulkanSDK/1.4.357.0/macOS`; the machine-local
  environment entrypoint is the parent SDK `setup-env.sh`.
- Official Slang 2026.5.2 macOS arm64 package. `slangc` and the compiler dylib
  are arm64 under `Engine/External/Packages/slang`, and setup validates the
  pinned archive digest and required files.
- Repository-built spdlog, GLFW, rapidyaml, and Assimp Debug artifacts are
  arm64. Assimp 6.0.4 installs as `@rpath/libassimp.6.dylib` and links the
  platform zlib.

This plan is M1 of the
[macOS Platform Enablement roadmap](../Roadmaps/MacOSPlatformEnablement.md) and
consumes the archived M0
[first-host handoff](Archive/2026-08/MacOSHostIndependentPreparation.md#m1-first-host-handoff).
It ends at repeatable native configuration and a bounded compile gate; M2-M5
retain runtime, rendering, asset, and product qualification ownership.

## Goal

Provide one repeatable Apple Silicon macOS bootstrap path that starts from a
manually provisioned host, runs through a repository-owned DevTool entrypoint,
validates the selected Xcode/Apple Clang and Vulkan/MoltenVK environment,
prepares pinned arm64-compatible dependencies, configures the existing Debug
Editor preset, and compiles the smallest Core/ApplicationCore-oriented closure
that does not require M2 runtime implementations.

## Scope

- Freeze and record the candidate macOS, Apple Silicon, Xcode, Apple Clang,
  macOS SDK, CMake, Ninja, Python, Git, Vulkan SDK/MoltenVK, and Slang baseline.
- Add an extensionless repository-root POSIX `DevTool` launcher while retaining
  `DevTool.bat` as the Windows launcher.
- Make setup preflight, toolchain selection, Python virtual-environment layout,
  libclang validation, local toolchain persistence, and generated VS Code
  launch configuration host-aware.
- Register one `macos-xcode-arm64` Agent Build Profile that selects only
  `MacOS-arm64-Debug-DurinEditor` and inherits the validated host environment.
- Verify macOS artifacts outside the repository before pinning exact archive
  URLs, hashes, required files, architectures, and dylib identities.
- Extend dependency preparation for the `MacOS` platform, including source
  dependencies, the verified Slang arm64 package, Vulkan SDK/VMA discovery,
  and MoltenVK layout diagnostics.
- Configure and inspect the arm64 graph, repair Apple Clang and linker issues
  within M1 ownership, and establish development rpaths for the bounded native
  target closure.
- Preserve Windows setup, dependency preparation, configuration, and focused
  regression coverage while introducing the macOS path.
- Update lasting setup and dependency documentation with the qualified
  baseline, commands, environment contract, and known limitations.

## Non-Goals

- Installing or upgrading Homebrew packages, Xcode, the Vulkan SDK, or changing
  shell startup files and system-global configuration from DevTool.
- Implementing the complete macOS `FPlatformProcess`, Launch crash-service,
  native-dialog, project-ownership, Cocoa input, or Editor lifecycle behavior.
- Weakening required process, crash, window, or rendering contracts with
  generic no-op implementations merely to make a target link.
- Qualifying MoltenVK device admission, presentation, rendering correctness,
  resize behavior, or shader workload parity.
- Launching the Editor as evidence of support, enabling Game/Release/Shipping,
  adding Intel or universal builds, or producing an `.app` bundle.
- Deciding cooked asset compatibility, signing, notarization, packaging, CI,
  or the supported-machine product matrix.

## Design Decisions and Invariants

### Command entrypoints

- The macOS entrypoint is an executable, extensionless `DevTool` script at the
  repository root. It resolves the checkout from its own location, prefers
  `.venv/bin/python`, falls back to a discoverable Python 3.10 or newer only
  for bootstrap-capable commands, and forwards arguments without rewriting
  them.
- `DevTool` and `DevTool.bat` are thin host launchers for the same Python entry
  module. Tool behavior remains in `durin_dev_tool`; shell scripts do not
  duplicate setup, dependency, build, or environment policy.
- Dependency-backed commands continue to require the prepared `.venv` and
  report the host-correct repair command when it is absent or incomplete.

### Host and toolchain ownership

- Setup dispatches through an explicit host strategy. Windows keeps Visual
  Studio environment capture and long-path validation; macOS inherits the
  caller environment and validates Xcode selection, Apple Clang, macOS SDK,
  CMake, Ninja, Python, Git, and arm64 host/target identity.
- DevTool never runs Homebrew, installs Xcode components, edits shell profiles,
  selects a system-global Vulkan installation, or accepts Rosetta translation
  as native arm64 qualification. Failures identify the missing command, path,
  architecture, or environment variable and leave machine repair to the user
  or CI image owner.
- A macOS inherited environment does not require a synthetic setup script in
  `.agents/DevTool.user.json`. Optional environment scripts remain available
  only as an explicit local override, and persisted configuration stays
  portable across repeated invocations on the same worker.

### Dependency and binary provenance

- Source dependencies remain repository-bootstrap owned and pinned by their
  existing manifests. Homebrew copies of engine dependencies are not consumed
  opportunistically.
- The Vulkan SDK and MoltenVK are externally installed host prerequisites.
  Preflight validates their selected layout and architecture without copying
  them into an invented repository package. VMA continues to come from the
  selected SDK unless a separately recorded dependency decision changes that
  contract.
- Slang is repository-bootstrap owned. A MacOS manifest entry is added only
  after its publisher URL, SHA-256, arm64 slice, headers, executables, dylib
  install names, dependent libraries, and runtime load behavior are captured
  on the native host.
- Bootstrap publishes a prepared dependency only after acquisition, integrity,
  required-file, architecture, and install validation succeeds. A failed
  preparation leaves no apparently valid partial package or install tree.

### Build and qualification boundary

- `MacOS-arm64-Debug-DurinEditor` remains the only M1 configure preset. Every
  configured CMake, DHT, compiler, dependency, and imported binary identity
  must resolve to arm64 and must not define Win32/MSVC target semantics.
- The generated graph contains common plus MacOS sources and excludes Windows
  implementation translation units. Graph inspection happens before native
  compilation.
- Development dylibs use explicit build-tree/install-tree locations and rpaths
  that are inspectable with native tooling. Environment-only loader fixes are
  diagnostics, not the final repository contract.
- M1 compiles in bounded dependency order and stops before inventing missing M2
  platform services. Configure success or a partial native compile is not an
  Editor runtime or macOS support claim.

## Current Foundations and Gaps

| Area | Existing foundation | M1 gap |
| --- | --- | --- |
| Entry point | `DevTool.bat` selects the prepared or system Python and invokes the shared Python module. | There is no POSIX root launcher, and several repair diagnostics name only the batch file. |
| Setup orchestration | Setup is idempotent, preflights before mutation, creates local config and `.venv`, and prepares dependencies. | Host admission, Visual Studio selection, `.venv/Scripts`, `libclang.dll`, and `cppvsdbg` are Windows-specific. |
| Build profiles | The model already supports `inherit`, and CMake contains one arm64 Debug Editor preset. | The registered profile manifest contains only `windows-msvc-x64`; macOS cannot be selected by DevTool. |
| Dependency service | Platform mapping already recognizes `darwin` as `MacOS`, source and archive manifests are validated before preparation, and partial publication is guarded. | Required MacOS archive entries and shared-install compiler/linker behavior have not been natively verified. |
| Vulkan and Slang | M0 added actionable missing-platform diagnostics; Slang CMake has a dylib branch. | Vulkan/MoltenVK layout, VMA discovery, Slang arm64 archive metadata, install names, deployment, and rpaths are unqualified. |
| Native graph | M0 established platform source filtering, target-aware DHT metadata, and `MacOS-arm64-Debug-DurinEditor`. | No Apple toolchain configure log, source-graph evidence, or bounded native compile has passed. |

## Implementation Stages

### Stage 0: Freeze the native baseline and artifact evidence

- [x] Record the host model and architecture, macOS version, selected Xcode and
  macOS SDK, Apple Clang, CMake, Ninja, Python, Git, Vulkan SDK/MoltenVK, and
  candidate Slang versions with their resolved executable and SDK paths.
- [ ] Define the supported minimum/candidate baseline and explicitly record
  whether qualification depends on a newer locally installed version.
- [x] Verify the candidate Vulkan SDK/MoltenVK and Slang artifacts outside the
  repository, including provenance, hashes, arm64 slices, required files,
  executable behavior, dylib identities, dependencies, and current rpaths.
- [ ] Capture the current macOS setup failure, dependency manifest diagnostics,
  preset parse result, and configure result as the native starting baseline.
- [ ] Stop without pinning metadata if an artifact cannot be traced to its
  publisher, fails integrity or arm64 inspection, or requires an unsupported
  host-global mutation.

#### Acceptance Gate

- The exact host and artifact candidates are reproducible from recorded
  versions and paths; every binary proposed for a manifest is verified as
  arm64 with known integrity and dylib identities; and no unverified MacOS URL,
  hash, or required-file list has entered tracked metadata.

### Stage 1: Add the POSIX launcher and host-aware setup foundation

- [x] Add the root `DevTool` launcher with prepared-environment selection,
  bootstrap fallback, argument/exit-code preservation, repository-root
  independence, and actionable Python diagnostics.
- [x] Keep bootstrap-capable commands standard-library-only by strictly
  validating `DevTool.json` and `.agents/DevTool.user.json` without
  `jsonschema`; retain lazy JSON Schema validation for complex prepared-
  environment contracts and test the hand-written/schema field parity.
- [x] Introduce explicit Windows and macOS setup/toolchain strategies while
  preserving preflight-before-mutation ordering and shared error aggregation.
- [x] Implement host-correct virtual-environment interpreter discovery,
  creation, package installation, and libclang native-library validation.
- [x] Register `macos-xcode-arm64` with the inherited environment provider,
  `MacOS-arm64-Debug-DurinEditor`, `MacOS` output identity, and suffix-free
  native executable names.
- [x] Make saved toolchain settings support a valid inherited macOS environment
  without requiring an environment script, while retaining explicit local
  script overrides.
- [x] Generate LLDB-compatible macOS VS Code launch entries and retain existing
  Windows `cppvsdbg` entries.
- [x] Make all setup and prepared-environment repair diagnostics render the
  correct root launcher for the active host.
- [x] Add focused launcher, preflight, selection, local-config, Python-layout,
  profile, VS Code, and Windows-regression tests.

#### Acceptance Gate

- From outside the repository directory, the root POSIX launcher reaches
  bootstrap-capable commands; macOS setup reaches dependency preparation using
  a validated inherited Apple toolchain and `.venv/bin/python`; invalid hosts
  and tools fail before repository mutation; and Windows setup contract tests
  remain unchanged or have an explicitly justified host-neutral update.

### Stage 2: Prepare pinned MacOS dependencies

- [x] Add only the Stage 0-verified MacOS Slang archive metadata, hash, required
  files, versioned package layout, and repair diagnostics.
- [x] Make source and shared-install dependency preparation consistently pass
  the selected Apple Clang environment, arm64 architecture, deployment target,
  CMake configuration, and `MacOS` install layout.
- [x] Validate and prepare GLM, spdlog, rapidyaml, GLFW, Assimp, GoogleTest,
  Tracy client, and other selected source dependencies one at a time before
  exercising the complete setup selection.
- [x] Treat unsupported host-only development tools explicitly; do not block
  ordinary engine preparation on a Win64-only profiler executable package.
- [x] Validate the selected Vulkan SDK headers, loader, MoltenVK libraries, VMA
  header, architecture, and environment contract with actionable diagnostics.
- [x] Verify Slang imported targets, headers, compiler executable, dylibs,
  dependent libraries, deployment locations, and development rpaths without
  relying on a global Homebrew Slang installation.
- [x] Add focused manifest, acquisition, publication, CMake argument, required
  file, architecture, and unsupported-development-tool tests.

#### Acceptance Gate

- A clean repository-managed external tree can be prepared repeatedly on the
  declared host; every installed or packaged dependency resolves to the pinned
  source/artifact and arm64 identity; failed integrity or validation publishes
  no usable-looking result; and Windows dependency preparation remains
  qualified.

### Stage 3: Configure and compile the bounded native closure

- [x] Configure only `MacOS-arm64-Debug-DurinEditor` and preserve the complete
  toolchain, dependency discovery, configure, and generation diagnostics.
- [x] Inspect the generated graph for common-plus-MacOS source ownership,
  Windows-source exclusion, arm64 target consistency, truthful DHT macros and
  triples, selected SDK/deployment target, and imported dependency locations.
- [x] Compile DHT generation, Core headers and common sources, then
  ApplicationCore/GLFW and the smallest reachable Editor bootstrap closure in
  dependency order.
- [x] Repair Apple Clang language, warning, visibility, framework, linker,
  install-name, and development-rpath issues that belong to M1 without adding
  runtime no-ops or weakening common contracts.
- [x] Record the first remaining missing macOS platform implementations and
  assign them to M1 only when required for the bounded compile gate; otherwise
  preserve them as explicit M2 entry diagnostics.
- [x] Repeat the bounded configure and compile from a clean worktree/external
  state using only the documented host environment and DevTool workflow.

#### Acceptance Gate

- The declared preset configures reproducibly; graph evidence proves correct
  platform and arm64 selection; the agreed Core/ApplicationCore-oriented
  target closure compiles and links with inspectable dylib resolution; and any
  remaining Editor/runtime failure is an explicit M2-owned implementation gap
  rather than a toolchain, dependency, or hidden loader problem.

### Stage 4: Qualify the workflow and publish the M2 handoff

- [x] Run the focused DurinDevTool, dependency, CMake metadata, DHT, and native
  compile validations selected through the repository testing workflow.
- [x] Re-run the required Windows DevTool and build-graph regression coverage
  for every shared setup, profile, dependency, and CMake behavior changed by
  M1.
- [x] Update the authoritative build/run and third-party preparation contracts
  with macOS prerequisites, root command syntax, environment ownership,
  dependency layout, baseline versions, and bounded qualification status.
- [x] Record exact fresh-host/fresh-worktree reproduction evidence, resolved
  binary locations, dylib closure, known limitations, and rollback boundaries.
- [x] Publish M2's entry diagnostics for process, crash, filesystem/module,
  native dialog, window/input, project ownership, and Editor shell work without
  claiming runtime or rendering support.
- [ ] Update this plan and the roadmap only after every M1 acceptance gate has
  evidence; leave the plan active if the native compile gate is incomplete.

#### Acceptance Gate

- A second clean execution on the declared Apple Silicon baseline reproduces
  setup, dependency preparation, configure, graph inspection, and the bounded
  compile result; shared Windows behavior remains qualified; authoritative
  documentation matches the implemented commands; and M2 receives a bounded,
  evidence-backed platform-runtime handoff.

### M2 entry diagnostics and rollback boundary

- **Process and crash:** Launch compiles but does not link because MacOS lacks
  `InstallProcessCrashHandler`, `UninstallProcessCrashHandler`,
  `ConfigureProcessCrashTestOptions`, `RunProcessCrashFixture`, and
  `PublishProcessCrashRoot`. M2 must implement the real contract rather than a
  link-only no-op.
- **Module/filesystem and shutdown:** the M1 `FMacOSPlatformProcess` covers the
  basic bounded compile needs, but full process/module lifetime is unqualified.
  SkeletalAsset and Spline qualification test bodies pass 34/34 and 2/2 before
  both processes receive `SIGSEGV` after global teardown; M2 owns diagnosis and
  clean-exit qualification.
- **Dialogs, window/input, and ownership:** GLFW compiles with its Cocoa, IOKit,
  and CoreFoundation closure, but native dialogs, project ownership, Cocoa
  input, window lifecycle, relaunch/open-path behavior, and the Editor shell
  have not been run or qualified.
- **Rendering boundary:** Vulkan SDK and MoltenVK discovery is qualified only as
  a dependency contract. Device admission, surface/presentation, rendering,
  resize, and shader workload behavior remain M3 work.
- **Rollback:** M1 changes are confined to repository launchers, DevTool host
  policy/tests, dependency metadata/build glue, MacOS platform sources, portable
  compiler fixes, and documentation. DevTool did not install host packages,
  modify shell profiles, or change system-global Xcode/Vulkan selection. A code
  rollback can therefore revert the M1 commits; generated preset outputs can be
  removed with the documented `DevTool purge` workflow, while external SDK and
  Homebrew installations remain user-owned.

## Validation Matrix

| Boundary | Required evidence |
| --- | --- |
| Root launcher | POSIX path/quoting/exit-code tests, invocation from outside the checkout, prepared and bootstrap Python selection, missing/old Python diagnostics, and unchanged batch-launcher coverage. |
| Setup preflight | Synthetic host tests plus native macOS checks for arm64, Xcode/Apple Clang, SDK, CMake, Ninja, Git, Vulkan/MoltenVK, and aggregated pre-mutation failures; existing Windows MSVC and long-path cases remain covered. |
| Python environment | Host-correct `.venv/bin/python` creation, pinned requirements import, native libclang discovery, idempotent rerun, and incomplete-environment repair behavior. |
| Build profile and local config | Automatic macOS profile selection, single preset membership, inherited environment behavior, optional script override, suffix-free executable path, LLDB launch generation, and Windows profile parity. |
| Dependency manifests | Complete MacOS platform validation, pinned URL/hash/required files, checksum rejection, arm64 and dylib inspection, atomic publication, unsupported development-tool handling, and Windows manifest regression. |
| Third-party builds | Per-library then complete preparation on Apple Clang arm64, deterministic `MacOS` source/install/package layout, config separation, and clean-state reproduction. |
| CMake and DHT graph | Preset/schema parse, arm64 architecture, selected SDK/deployment target, MacOS source inclusion, Windows source exclusion, truthful target macros/triples, and imported target locations. |
| Native compile and link | Ordered DHT/Core/ApplicationCore-oriented compile targets, Apple Clang diagnostics, framework/link settings, Slang/MoltenVK dylib closure and rpaths, clean rebuild, and recorded M2 stop boundary. |
| Documentation lifecycle | Changed-document validation, all-plan lifecycle validation, live roadmap links, and authoritative build/dependency guidance updated with evidence rather than anticipated behavior. |

Build, native-test selection, and documentation validation follow the
repository [agent build workflow](../Agents/BuildAndRun.md),
[agent testing workflow](../Agents/Testing.md), and
[documentation workflow](../Agents/Documentation.md). The plan records evidence
and target ownership rather than duplicating mutable command recipes.

## Definition of Done

- All Stage 0-4 acceptance gates pass on the declared Apple Silicon baseline,
  and the same setup-to-compile path is repeated from clean repository-managed
  state.
- The root POSIX launcher and `DevTool setup` provide a supported, diagnostic
  macOS bootstrap path while Windows launcher/setup behavior remains qualified.
- Every MacOS dependency artifact and source revision is pinned, validated,
  arm64-compatible, and resolved from the repository-managed layout or the
  explicitly external Vulkan SDK contract.
- `MacOS-arm64-Debug-DurinEditor` configures with a correct platform graph and
  the bounded Core/ApplicationCore-oriented closure compiles and links with a
  known dylib closure and development rpaths.
- Lasting macOS setup and dependency behavior is documented in the owning
  development contracts, the roadmap records M1 completion evidence, and M2's
  runtime boundary is explicit.
- No completion statement implies Editor launch, rendering, asset, packaging,
  signing, notarization, Intel, universal, or product support qualification.

## Deferred Follow-ups

- M2 implements and qualifies macOS process, crash, module/filesystem, shell,
  dialog, Cocoa/GLFW input, project-ownership, and Editor lifecycle behavior.
- M3 qualifies MoltenVK device admission, shaders, graphics/compute,
  synchronization, presentation, resize, and rendering diagnostics.
- M4 decides and validates per-payload asset sharing or recooking.
- M5 expands the preset/runtime matrix, adds `.app` assembly, CI, signing,
  notarization, packaging, installation, and supported-machine qualification.
- Intel Mac, universal binaries, other Apple operating systems, and a native
  Metal RHI require separately selected work.

## Related Documentation

- [macOS Platform Enablement roadmap](../Roadmaps/MacOSPlatformEnablement.md)
- [macOS Host-Independent Preparation](Archive/2026-08/MacOSHostIndependentPreparation.md)
- [Build And Run](../Development/Build/BuildAndRun.md)
- [Third-Party Dependency Preparation](../Development/Build/ThirdPartyBootstrap.md)
- [Agent Build And Run Workflow](../Agents/BuildAndRun.md)
- [Agent Testing Workflow](../Agents/Testing.md)
- [Agent Documentation Workflow](../Agents/Documentation.md)

## Related Code

- `DevTool.bat`
- `CMakePresets.json`
- `Templates/DurinDevTool/DevTool.user.json`
- `Templates/VSCode/`
- `Tools/DurinDevTool/DevTool.user.schema.json`
- `Tools/DurinDevTool/durin_dev_tool/bootstrap/`
- `Tools/DurinDevTool/durin_dev_tool/bootstrap/thirdparty/`
- `Tools/DurinDevTool/durin_dev_tool/build/AgentBuildProfiles.json`
- `Tools/DurinDevTool/durin_dev_tool/build/toolchain_context.py`
- `Tools/DurinDevTool/durin_dev_tool/toolchain.py`
- `Tools/DurinDevTool/tests/`
- `Engine/CMake/ThirdParty/`
- `Engine/Source/Runtime/Core/Private/MacOS/`
- `Engine/Source/Runtime/Core/Public/MacOS/`
- `Engine/Source/Runtime/ApplicationCore/Public/ThirdParty/Glfw/`
- `Engine/Tests/Native/CoreTests/`
- `Engine/Source/Programs/DurinHeaderTool/`
