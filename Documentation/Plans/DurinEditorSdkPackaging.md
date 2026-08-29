# DurinEditor SDK Packaging Plan

Summary: Produce relocatable Source and Binary SDK archives that let external C++ projects build reflected modules and load them in DurinEditor.

Last reviewed: 2026-08-29

Status: Active
Completed:

## Current Status

The repository builds DurinEditor and repository-owned projects from one source
workspace, and the runtime can open an absolute `.dproject` outside that
workspace. External project creation, an installed-engine CMake package, and a
release staging command do not yet exist. The current public headers are a
repository build contract rather than an installed SDK contract.

The selected path is to deliver two compatible package kinds in order:

1. a Source SDK that preserves the complete buildable engine source layout and
   establishes the external-project workflow; then
2. a Binary SDK that preserves the same installed layout while replacing engine
   source targets with imported binaries and installed metadata.

Stage 0 is the first open stage. No package format or compatibility contract is
implemented yet.

## Goal

Provide one DurinDevTool packaging workflow that can emit versioned Win64
DurinEditor SDK archives with these outcomes:

- the Source SDK can be extracted, prepared, configured, and built without a
  Git checkout;
- the Binary SDK can be extracted and used without rebuilding engine modules;
- a C++ project located outside the SDK root can keep its own `.dproject`,
  source, generated files, binaries, content, and shaders;
- the external project can use `add_durin_project(...)` and
  `add_durin_module(...)` for its source modules in both package kinds;
- a reflected external module can link against Engine modules and be loaded by
  the packaged Release DurinEditor; and
- every archive carries a machine-readable identity, file inventory, hashes,
  compatibility data, and third-party notices.

## Scope

- Win64 is the first packaging and clean-machine qualification platform.
- `DurinEditor` is the only packaged runtime variant in this plan.
- The Binary SDK consumes the `Win64-Release-DurinEditor` build contract.
- The Source SDK retains the registered Debug and Release DurinEditor source
  build paths, while its end-to-end release gate uses Release.
- Package staging, archive generation, manifest generation, inspection, and
  clean-machine verification are owned by DurinDevTool.
- The SDK layout preserves the `Engine/`, project, module, `Content/`,
  `Shaders/`, `Binaries/`, and generated-metadata ownership boundaries.
- External project configuration, build, launch, and project-module discovery
  are included.
- Engine public headers, generated headers, reflection export metadata, import
  libraries, runtime binaries, CMake metadata, DHT, third-party development
  artifacts, configuration templates, and licenses required by the Binary SDK
  are included.

## Non-Goals

- DurinGame, Shipping game packaging, cooking, staged game content, or a game
  installer.
- Editor auto-update, an installer UI, a launcher service, or marketplace
  distribution.
- C++ hot reload or rebuilding a loaded project DLL in place.
- ABI compatibility across different compiler toolsets, architectures, build
  configurations, runtime variants, or incompatible SDK manifest versions.
- macOS application bundles, signing, notarization, or distribution
  qualification.
- A fully offline Source SDK bootstrap in the first delivery. The Source SDK
  may acquire repository-managed dependencies through the existing bootstrap
  workflow, but must declare that requirement in its manifest and documentation.
- Publishing private Engine implementation headers as supported SDK APIs.
- Reorganizing runtime files into a flat `bin/` directory.

## Design Decisions and Invariants

### One logical installed layout

Both package kinds preserve this logical root instead of flattening files:

```text
Durin-<Version>-Win64-<Kind>/
├─ Engine/
│  ├─ Binaries/Win64/<Config>/
│  │  ├─ Runtime/DurinEditor/
│  │  ├─ ThirdParty/
│  │  ├─ Lib/
│  │  └─ Symbols/                  # optional separate artifact
│  ├─ Build/InstalledSdk.json
│  ├─ CMake/
│  ├─ Configs/
│  ├─ Content/
│  ├─ Shaders/Slang/
│  ├─ Source/                      # complete in Source; Public SDK surface in Binary
│  └─ Tools/DurinHeaderTool/
├─ Templates/
├─ Licenses/
└─ sdk-manifest.json
```

An external project remains outside that root and publishes its module to its
own configuration-specific runtime directory:

```text
MyGame/
├─ MyGame.dproject
├─ Configs/
├─ Content/
├─ Shaders/Slang/
├─ Source/
├─ Intermediate/
└─ Binaries/Win64/Release/Runtime/DurinEditor/
   └─ DurinEditor-MyGame.dll
```

### Shared consumer-facing project API

External project module declarations continue to use
`add_durin_project(...)` and `add_durin_module(...)`. Package-kind differences
remain inside the SDK bootstrap and target registration:

- Source SDK configuration creates Engine targets from the packaged source
  tree before registering the external project.
- Binary SDK configuration creates Engine imported targets from installed
  metadata before registering the external project.
- `add_durin_module(...)` always builds the consumer-owned module from source.

The consumer must select the SDK explicitly through a path or CMake package
location. Project setup must not infer the SDK root from the external project's
parent directory.

### Package commands do not build implicitly

The selected command surface is:

```text
DevTool package sdk --kind source --output <directory>
DevTool package sdk --kind binary --preset Win64-Release-DurinEditor --output <directory>
DevTool package sdk inspect <archive-or-staging-root>
```

The Binary SDK command consumes an already successful matching build and fails
with an actionable diagnostic when outputs are absent, stale, incompatible, or
owned by an active build. It does not configure or build implicitly. The Source
SDK command consumes repository sources and validates required materialized
inputs before staging.

### Staging is manifest-driven and non-destructive

- Packaging writes to a fresh staging directory outside all source, build,
  intermediate, binary, DDC, and saved-data roots.
- Inclusion uses explicit package components and generated inventories; it does
  not recursively copy the checkout and then delete a denylist.
- Archive entries use normalized forward-slash relative paths, stable ordering,
  and deterministic metadata where the archive format permits it.
- Existing output artifacts are never overwritten unless the caller selects an
  explicit, recoverable replacement operation.
- Package creation fails on missing large-file payloads, missing licenses,
  duplicate destination paths, case-insensitive collisions, absolute paths in
  installed CMake metadata, or manifest/file inventory disagreement.

### Identity and compatibility are explicit

`Engine/Build/InstalledSdk.json` is the engine-root marker and SDK compatibility
contract. The root `sdk-manifest.json` owns archive identity and inventory. At
minimum they record:

- engine semantic version and source revision;
- package kind and manifest schema version;
- platform, architecture, compiler/toolset, C++ runtime, configuration, and
  runtime variant;
- DHT/tool fingerprint and reflection metadata version;
- supported project descriptor and module descriptor schema versions;
- dependency/bootstrap policy;
- clean or explicitly allowed dirty source state;
- component list and SHA-256 file inventory; and
- package creation tool version.

Binary SDK configuration rejects mismatched platform, architecture, compiler
toolset, configuration, runtime variant, or SDK ABI. A project module compiled
for Debug, Shipping, or `DurinGame` is not load-compatible with the packaged
Release DurinEditor.

### Runtime and authored data remain writable only in owned roots

- Packaged Engine binaries, public headers, CMake metadata, Engine content, and
  Engine shaders are treated as read-only installed inputs.
- Editor session state and logs remain beneath the launch `Saved/` contract and
  are never included from the producer machine.
- External project generated metadata, binaries, DDC, source data, and content
  remain beneath the external project or an explicit user cache.
- The Binary SDK must configure and build an external module when the SDK root
  itself is read-only.

### Third-party redistribution is a release gate

The package component model owns the required runtime binaries, development
headers and libraries, license files, notices, versions, and provenance for
every redistributed dependency. Test-only inputs and non-redistributable test
assets are excluded. Source SDK bootstrap requirements and Binary SDK bundled
requirements are reported separately.

## Current Foundations and Gaps

### Foundations

- Runtime project selection accepts absolute external `.dproject` paths.
- Project roots already own their own `Content`, `Intermediate`, `Binaries`,
  and logical `/Game/` mount.
- The module loader already searches the active project's configuration- and
  runtime-variant-specific binary directory after engine-adjacent locations.
- Module DLL naming already includes the runtime variant.
- DHT accepts multiple complete `.dproject` paths and preserves cross-project
  dependency context.
- Engine, third-party, project runtime, import-library, and symbol outputs
  already have separate directory owners.
- DurinDevTool already owns setup, dependency preparation, configure, build,
  run, locking, project scaffolding, and documentation command architecture.

### Gaps

- The root CMake graph registers repository projects explicitly and provides no
  external-project entrypoint.
- Generated setup files and project setup assume a complete source workspace
  and source-tree DHT path.
- `Toolchains.cmake` discovers the workspace and Python environment from the
  repository layout rather than an installed SDK identity.
- Engine targets have no install/export rules or imported-target package.
- Public include directories and transitive dependencies do not distinguish
  build and install interfaces.
- Engine generated reflection headers and export metadata have no installed
  publication contract.
- There is no standalone, relocatable DHT distribution.
- There is no package component registry, installed manifest schema, license
  inventory, package inspection command, or clean-machine SDK fixture.
- Runtime engine-root detection recognizes directory shape but not an installed
  SDK marker and compatibility record.
- The current project scaffolder deliberately excludes installed-engine and
  external-project creation.

## Implementation Stages

### Stage 0: Freeze package, compatibility, and fixture contracts

- [ ] Define and version the `InstalledSdk.json` and `sdk-manifest.json`
  schemas, including compatibility fields and normalized file inventory.
- [ ] Define Source and Binary package components, destinations, ownership,
  required/optional status, and license provenance.
- [ ] Define clean-source policy: release packaging rejects a dirty worktree by
  default; an explicit preview option records dirty state and is not a
  releasable artifact.
- [ ] Define LFS/materialized-input validation without packaging `.git` or
  relying on producer-machine object storage.
- [ ] Define the exact external-project CMake entrypoint and keep project/module
  declarations common between Source and Binary SDKs.
- [ ] Define the supported Win64 compiler/toolset and C++ runtime compatibility
  check from existing host build-profile data.
- [ ] Add a minimal external fixture project with one shared runtime module, one
  reflected class, Engine/Core dependencies, content, config, and shader roots.
- [ ] Record lasting installed-layout and external-project contracts in the
  authoritative build/workspace documentation before closing the stage.

#### Acceptance Gate

- Both manifests validate against checked-in schemas.
- Every included file category has one owner and every excluded mutable/cache
  category has an explicit rule.
- The fixture expresses the same project/module descriptors intended for both
  SDK kinds and does not depend on a workspace-relative project path.
- No open decision can change archive layout, consumer CMake entrypoints,
  configuration compatibility, or third-party redistribution scope.

### Stage 1: Establish the external Source SDK build boundary

- [ ] Add an explicit SDK/workspace root input and stop external project setup
  from inferring Durin through the project's parent directory.
- [ ] Add a supported CMake entrypoint that registers packaged Engine source and
  one absolute external project source directory with a dedicated binary
  directory.
- [ ] Keep `add_durin_project(...)` responsible for DHT project metadata and
  module enumeration, but remove assumptions that the project is a direct
  child of the workspace.
- [ ] Keep `add_durin_module(...)` responsible for consumer module generation,
  compilation, dependency wiring, output naming, and project-owned outputs.
- [ ] Ensure DHT receives canonical Engine and external project descriptors and
  does not require a global project registry.
- [ ] Extend DurinDevTool configure, build, path, and run requests with an
  explicit external project descriptor/root while preserving checkout locking.
- [ ] Extend project scaffolding so an explicit external path can be created
  without editing the SDK root CMake file.
- [ ] Add CMake/DHT/DevTool tests for another drive, spaces, non-ASCII path
  components, and project roots outside the SDK tree.

#### Acceptance Gate

- A source checkout can configure and build the external fixture without
  editing root `CMakeLists.txt` or placing the fixture below the Durin root.
- The reflected fixture module is emitted only beneath the fixture's own
  `Intermediate` and `Binaries` roots.
- Release DurinEditor loads the fixture module from the active project binary
  directory and resolves its reflected class.
- Existing Engine and Sandbox configure/build paths retain their current target
  names, output layout, and runtime behavior.

### Stage 2: Produce the Source SDK archive

- [ ] Add the `package sdk` command family with specification, handler,
  application service, manifest model, staging infrastructure, and tests that
  follow existing DurinDevTool dependency boundaries.
- [ ] Stage the Source SDK from explicit components: root build entrypoints,
  Engine source/build metadata, CMake modules, DHT, DevTool, schemas, templates,
  configs, content, shaders, bootstrap manifests, and required licenses.
- [ ] Exclude Git metadata, `.venv`, build trees, intermediate outputs,
  binaries, Saved state, DDC, downloaded source caches, native tests and test
  assets unless a component contract requires a specific fixture.
- [ ] Verify that tracked LFS-backed package inputs contain materialized content
  rather than pointer files.
- [ ] Generate manifests and a deterministic archive from the staging root.
- [ ] Add `package sdk inspect` to validate identities, hashes, collisions,
  required components, unexpected files, and forbidden producer paths without
  extracting into the checkout.
- [ ] Document prerequisites and the prepare/configure/build/run sequence for a
  Source SDK consumer.

#### Acceptance Gate

- Two Source SDK archives produced from the same clean revision and inputs have
  identical manifests and archive hashes.
- Inspection succeeds for the produced archive and fails after removal,
  addition, corruption, or case-collision of an inventoried file.
- The archive contains no Git metadata, build/cache/session output, LFS pointer,
  absolute producer path, native test asset, or unowned third-party payload.
- The archive can be extracted to a path containing spaces and prepared without
  a Git checkout.

### Stage 3: Qualify the Source SDK end to end

- [ ] Add an isolated test driver that extracts the Source SDK into a fresh root
  outside the producing checkout and creates/copies the external fixture into a
  second root.
- [ ] Run setup/dependency preparation through the packaged DevTool using only
  declared host prerequisites.
- [ ] Configure and complete the full Release DurinEditor build from the
  extracted SDK.
- [ ] Configure and build the external fixture through the supported SDK
  entrypoint.
- [ ] Launch the built Editor with the fixture `.dproject`, verify bounded
  startup/module/reflection/content/shader behavior, and shut down cleanly.
- [ ] Confirm a second external project can use the same extracted Source SDK
  without generated project state crossing project roots.
- [ ] Capture package/build/launch receipts and failure diagnostics in CI
  artifacts.

#### Acceptance Gate

- The end-to-end driver succeeds on a clean qualified Win64 machine with no
  checkout-relative paths or undeclared producer-machine dependencies.
- Deleting the original producing checkout after archive creation does not
  affect extraction, preparation, build, or launch.
- Two external projects produce isolated generated and binary outputs and load
  the correct module from the selected project.
- The Source SDK is declared the first releasable SDK milestone before Binary
  SDK implementation becomes a release blocker.

### Stage 4: Install Engine development interfaces and imported targets

- [ ] Classify Engine headers as public SDK, generated-public SDK, private, or
  unsupported transitive leakage; remove public-header dependencies on private
  files.
- [ ] Give every exported target correct `BUILD_INTERFACE` and
  `INSTALL_INTERFACE` include paths, compile definitions, link interfaces, and
  configuration mapping.
- [ ] Install Engine DLLs, import libraries, public/generated headers, reflection
  exports, and required development resources into the preserved layout.
- [ ] Export a versioned Durin CMake package that creates Engine imported
  targets and compatibility aliases expected by `.dmodule` dependencies.
- [ ] Publish third-party imported targets with complete development and runtime
  metadata without leaking producer install paths.
- [ ] Package DHT as a standalone relocatable tool with schemas and pinned
  runtime dependencies; remove the installed path's dependency on repository
  `.venv`, `requirements.txt`, or Engine private source.
- [ ] Make Binary SDK external-module generation consume installed Engine export
  metadata instead of regenerating Engine metadata from source.
- [ ] Make installed SDK root discovery and CMake configuration validate
  `InstalledSdk.json` rather than accepting directory shape alone.

#### Acceptance Gate

- A read-only staged SDK can configure and compile the external fixture module
  without adding Engine source directories to the consumer build graph.
- Installed CMake metadata contains no path into the producing checkout, build
  tree, user profile, Python environment, or dependency cache.
- Every public Engine header can be compiled through its owning installed target
  under the supported toolset.
- Removing a required imported library, generated header, export metadata file,
  DHT component, or third-party development artifact fails with a targeted SDK
  diagnostic.

### Stage 5: Produce the Binary SDK archive

- [ ] Add Binary package component collection from a successful matching
  Release DurinEditor build and installed-development staging pass.
- [ ] Stage Editor runtime modules, launcher, tools required by the supported
  workflow, Engine content, shaders, config templates, import libraries,
  public/generated headers, CMake package files, DHT, third-party runtime and
  development artifacts, manifests, and licenses.
- [ ] Keep project modules out of the Engine package and keep optional symbols
  in a separately identified artifact whose manifest points to the same build
  identity.
- [ ] Reject stale/mixed Debug, Release, Shipping, profiling, DurinGame,
  architecture, compiler, DHT, and source-revision inputs during collection.
- [ ] Reuse Source SDK archive normalization, hashing, collision detection,
  inspection, and license validation.
- [ ] Document Binary SDK configure/build/run workflow and exact compatibility
  diagnostics.

#### Acceptance Gate

- Binary SDK inspection proves one coherent build identity across runtime,
  libraries, headers, DHT metadata, manifests, and optional symbols.
- The archive contains no Engine private source, build objects, CMake cache,
  DHT state cache, editor session data, DDC, logs, or producer absolute paths.
- Runtime lookup succeeds without flattening Engine, third-party, or project
  module directories.
- A mismatched consumer configuration or toolset is rejected before native
  compilation or module loading.

### Stage 6: Qualify Binary SDK development end to end

- [ ] Extract the Binary SDK into a fresh read-only root containing spaces.
- [ ] Configure and build the external fixture in a separate writable root
  using only the installed CMake package and supported host prerequisites.
- [ ] Exercise DHT export/reflection generation for the fixture and incremental
  rebuild after a reflected header change.
- [ ] Launch packaged Release DurinEditor with the fixture `.dproject`, load the
  project DLL from project-owned binaries, resolve reflected types, mount Engine
  and project content/shaders, and shut down cleanly.
- [ ] Run negative qualification for missing/corrupt files, wrong configuration,
  wrong toolset, wrong runtime variant, incompatible manifest/schema, and a
  writable-output attempt beneath the SDK root.
- [ ] Add CI retention for Binary SDK, optional symbols, manifests, inspection
  receipts, external-fixture build receipts, and smoke-test diagnostics.
- [ ] Move lasting package, installed-build, external-project, and compatibility
  contracts into their authoritative Development and Workspace documents.

#### Acceptance Gate

- The Binary SDK end-to-end matrix passes on a clean qualified Win64 host after
  the producing checkout and build tree are made unavailable.
- The SDK root remains unchanged during consumer configure, DHT generation,
  build, launch, and shutdown.
- Source and Binary SDK consumers use the same `.dproject`, `.dmodule`,
  `add_durin_project(...)`, and `add_durin_module(...)` declarations.
- Failure cases identify the missing or incompatible package component and do
  not fall back to undeclared machine-local paths.

## Validation Matrix

| Area | Required validation |
| --- | --- |
| Manifest schemas | Unit tests for valid identities, required fields, schema upgrades, malformed values, duplicate paths, and normalized hashes |
| Package components | Unit tests for inclusion ownership, exclusions, licenses, LFS pointers, case collisions, forbidden paths, and deterministic ordering |
| Command architecture | DurinDevTool specification/handler/service/infrastructure tests and architecture-boundary tests |
| External project CMake | Configure fixtures for absolute roots, another drive, spaces, non-ASCII paths, missing SDK root, and repeated project registration |
| DHT | Engine plus external descriptor context, installed export consumption, reflected generation, incremental rebuild, fingerprint mismatch, and missing metadata |
| Installed targets | Public-header compile checks, imported target closure, configuration/toolset mapping, and absence of producer paths |
| Runtime layout | Engine-root marker, Engine/third-party/project module lookup, content mounts, shader mounts, config templates, and writable Saved state |
| Source SDK E2E | Clean extraction, setup, dependency preparation, full Release Editor build, external fixture build, launch, and second-project isolation |
| Binary SDK E2E | Read-only SDK extraction, external reflected-module build, packaged Editor launch, and negative compatibility/corruption cases |
| Regression | Existing Engine/Sandbox Debug workflow plus risk-triggered Release Editor full build following the repository build and test guides |
| Documentation | Changed-document validation during implementation and all-plan validation for lifecycle/status changes |

## Definition of Done

- All required stage acceptance gates pass and are recorded with evidence.
- Source and Binary SDK archives are produced through the documented DevTool
  commands and pass independent inspection.
- A clean Win64 host can use each package kind to build and load the same
  external reflected C++ project.
- The Binary SDK can be read-only and has no dependency on the producer checkout,
  build tree, user profile, `.venv`, or dependency cache.
- Runtime, project, and third-party binaries remain in their defined owners;
  packaging does not rely on a flat binary directory.
- SDK manifests, ABI checks, component inventories, license notices, and
  optional symbols share one coherent build identity.
- Existing repository Engine and Sandbox workflows remain supported.
- Lasting build, installed-layout, project, module, and tooling contracts are
  documented outside this plan.
- The plan is marked completed only after both package kinds pass their clean
  end-to-end gates; Source SDK completion is recorded separately as the first
  releasable milestone.

## Deferred Follow-ups

- DurinGame and Shipping game distribution, cooking, and staged content.
- macOS installed SDK layout, `.app` packaging, signing, notarization, and
  universal or architecture-specific packages.
- Offline Source SDK dependency media.
- Editor installer, updater, launcher service, delta patches, and distribution
  channels.
- Stable ABI across SDK patch/minor versions or side-by-side binary module
  compatibility beyond exact manifest identity.
- C++ live coding, hot reload, and safe replacement of loaded project modules.
- Plugin SDK packaging and separately versioned plugin binary distribution.

## Related Documentation

- [Build System](../Development/Build/BuildSystem.md)
- [Build And Run](../Development/Build/BuildAndRun.md)
- [Third-Party Dependency Preparation](../Development/Build/ThirdPartyBootstrap.md)
- [DurinDevTool Command Interface](../Development/Tooling/DurinDevTool.md)
- [Workspace And Projects](../Workspace/WorkspaceProjects.md)
- [Code Modules](../Workspace/CodeModules.md)

## Related Code

- [`CMakeLists.txt`](../../CMakeLists.txt)
- [`CMake/DurinWorkspaceSetup.cmake`](../../CMake/DurinWorkspaceSetup.cmake)
- [`CMake/DurinBuildApi.cmake`](../../CMake/DurinBuildApi.cmake)
- [`CMake/Config/Toolchains.cmake`](../../CMake/Config/Toolchains.cmake)
- [`CMake/Project/ProjectSetup.cmake`](../../CMake/Project/ProjectSetup.cmake)
- [`CMake/Project/ProjectTargets.cmake`](../../CMake/Project/ProjectTargets.cmake)
- [`CMake/Project/ProjectOutputs.cmake`](../../CMake/Project/ProjectOutputs.cmake)
- [`Engine/CMakeLists.txt`](../../Engine/CMakeLists.txt)
- [`Engine/CMake/EngineSetup.cmake`](../../Engine/CMake/EngineSetup.cmake)
- [`Engine/Source/Programs/DurinHeaderTool`](../../Engine/Source/Programs/DurinHeaderTool)
- [`Engine/Source/Runtime/Core/Private/Misc/Paths.cpp`](../../Engine/Source/Runtime/Core/Private/Misc/Paths.cpp)
- [`Engine/Source/Runtime/Core/Private/Modules/ModuleManager.cpp`](../../Engine/Source/Runtime/Core/Private/Modules/ModuleManager.cpp)
- [`Tools/DurinDevTool/durin_dev_tool`](../../Tools/DurinDevTool/durin_dev_tool)
- [`Tools/DurinDevTool/tests`](../../Tools/DurinDevTool/tests)
- [`Sandbox`](../../Sandbox)
