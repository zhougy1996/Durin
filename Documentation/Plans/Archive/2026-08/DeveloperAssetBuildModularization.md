# Developer Asset Build Modularization Plan

Summary: Split Engine asset production into target-selected Developer infrastructure and dependency-focused recipe modules while preserving current data contracts and preparing a provider-neutral Build framework.

Last reviewed: 2026-08-13

Status: Archived
Completed: 2026-08-13

## Current Status

All five implementation stages are complete. `AssetBuildCore` provides family-neutral identity,
definition, value, cache, request, registry and host contracts; `TextureBuild`
owns texture recipes, private BC compression and asynchronous coordination;
`GeometryBuild` owns StaticMesh/collision, skeletal/animation and terrain
recipes. StaticMesh workers consume immutable reconciliation snapshots, both
recipe modules coexist through generic host registration, and focused golden,
Cook, import, physics, viewport, thumbnail and Vulkan tests match the frozen
baseline.

The aggregate `EngineAssetBuild` module has been removed. Editor and migration
roots now select and drain the three Developer modules explicitly; game and
package-only tool closures contain none. Full native, qualification, clean
editor/game builds, deployment inspection, Cooked runtime and tool process
validation pass on the final graph.

The selected outcome is not one module per asset family. The first durable
Developer layout is:

```text
Source/Developer/AssetBuildCore
Source/Developer/TextureBuild
Source/Developer/GeometryBuild
```

`AssetBuildCore` will establish the provider-neutral local contracts needed by
the current builders and later Build-framework evolution. `TextureBuild` will
own Texture2D and TextureCube recipes plus their compression and asynchronous
coordination. `GeometryBuild` will own StaticMesh, SkeletalMesh, AnimationClip,
and TerrainHeightmap recipes. The physical directory is organizational;
`.dproject` roots, dependencies, host selection, runtime loading, closure
assertions, and deployment inspection provide the Developer-only semantics.

## Goal

- Replace the aggregate `EngineAssetBuild` DLL with dependency-focused
  `TextureBuild` and `GeometryBuild` recipe modules supported by a genuinely
  asset-family-neutral `AssetBuildCore` module.
- Make Developer modules explicit non-game build/runtime units that are
  selectable by editor, Cook, migration, repair, headless tools, and tests
  without depending on editor UI modules or concrete source translators.
- Give Build functions stable owner-qualified identities, immutable owned
  inputs, detached values, explicit cache/build policy, scoped cancellation and
  waits, and registration-based lifecycle so a later local/remote Build graph
  does not require another asset-boundary rewrite.
- Preserve all authored package, build-key, DDC payload, Cooked payload,
  publication, diagnostics, import/reimport, and runtime behavior unless an
  independently approved and versioned correctness change is recorded.
- Keep Runtime `Engine`, `AssetCore`, and `DurinGame` free of authoring module
  discovery, source translation, offline compressors, Build coordinators, and
  Build-function registration.

## Scope

- Add a first-class `developer` module-scaffolding convention whose default
  path is `Source/Developer/<ModuleName>` and whose default root enablement is
  `DurinEditor`, without adding a new runtime variant or treating paths as
  target classification.
- Add `AssetBuildCore` with provider-neutral Build identity/value/policy,
  request ownership, local function registration, cache access, aggregate
  diagnostics, and authoring-host lifecycle contracts.
- Move Texture2D and TextureCube recipe/key/DDC/coordinator/diagnostic code into
  `TextureBuild`; make the BC compressor a private dependency of that module.
- Move StaticMesh, SkeletalMesh, AnimationClip, and TerrainHeightmap
  recipe/key/DDC/diagnostic code into `GeometryBuild`.
- Finish the remaining pure-request/detached-product seams where the current
  outer TextureCube and StaticMesh operations still combine an Engine object
  snapshot, Build, DDC, or publication.
- Rewire `StandardAssetImport`, `MainFrame`, asset editors, Cook and migration
  paths, `DurinAssetTool`, native tests, module descriptors, target selection,
  runtime module loading, and dependency-closure assertions.
- Remove the `EngineAssetBuild` target, binary, API macro, dynamic load name,
  compatibility facade, and source directory after every named consumer has
  migrated.
- Update lasting module, asset lifecycle, import, runtime lifecycle,
  build-system, runtime-variant, workspace, and module-creation documentation.

## Non-Goals

- One physical Build module per asset class. New modules require a meaningful
  dependency, deployment, lifecycle, ownership, or release-cadence boundary.
- A distributed Build farm, remote workers, network protocol, worker process,
  cross-machine scheduler, shared remote DDC, or sandboxed execution in this
  plan.
- Runtime source import, mod ingestion, runtime texture compression, or any
  Build/DDC fallback in cooked games.
- Moving Runtime asset values, value validation, render-resource creation, or
  DDC/Cooked payload `Serialize` implementations out of their owning Runtime
  modules.
- Changing current payload schemas, build keys, DDC namespaces, compression
  formats, target platforms/profiles, Cook layouts, or authored provenance.
- Making every existing synchronous recipe asynchronous merely to exercise the
  framework.
- A common polymorphic base class for asset-family request/product structures,
  a central asset-family enum, or a mutable global `DObject` Build API.
- Replacing `AssetImportCore` orchestration or merging import-provider
  registration with Build-function registration.
- Keeping a permanent `EngineAssetBuild` facade for source or binary
  compatibility; migration wrappers are stage-local and must be removed.

## Design Decisions and Invariants

### Developer Is a Selection Contract, Not a Directory Property

`Source/Developer` communicates ownership, but it does not classify a module.
`ModuleDirs` maps the physical roots; `BaseModules` and
`ExtraModules.<RuntimeVariant>.Modules` select roots; dependencies resolve the
closure; programs explicitly load optional capabilities. No code may infer
availability from a path, and no new `DurinDeveloper` runtime variant is added.

The `developer` creation kind is a safe default for this contract:

- default path: `Source/Developer/<ModuleName>`;
- default enablement: `ExtraModules.DurinEditor.Modules`;
- no addition to `BaseModules` or `DurinGame` roots;
- custom `--path` and explicit `--enable` retain their existing authority;
- schemas and generated metadata continue to classify modules by descriptors
  and roots, not directory spelling.

### Target Module Graph

Arrows point from consumers to dependencies:

```text
AssetCore <---------------------------+
  ^                                   |
  |                                   |
Engine <---- TextureBuild --------> AssetBuildCore
  ^             ^                     ^
  |             |                     |
  +------ GeometryBuild ---------------+
                 ^
                 |
          StandardAssetImport
                 ^
                 |
        editor hosts / Cook / tools

DurinGame -----> Engine and never enters the Developer branch
```

The intended direct boundaries are:

| Module | Owns | Must not own or depend on |
| --- | --- | --- |
| `AssetBuildCore` | Build function identity/registration, immutable opaque values, cache/build policy, request ownership, cancellation/wait scopes, service contributions, aggregate snapshots, host admission/pump/drain | `Engine` asset types, `DObject`, source providers, editor UI, RHI, concrete codecs, asset-family namespaces or recipes |
| `TextureBuild` | Texture2D/TextureCube requests/products, key inputs, recipes, projection, mip generation, format selection, BC compression, Texture DDC policy, coordinator and diagnostics | encoded image decoders, import-provider policy, UI, Runtime payload field order |
| `GeometryBuild` | StaticMesh/collision, SkeletalMesh, AnimationClip and TerrainHeightmap requests/products, keys, conversion/hierarchy recipes, DDC policy and diagnostics | Assimp/glTF/FBX/PNG translation, provider policy, UI, Runtime payload field order |
| `StandardAssetImport` | standard concrete translators, provider/import policy, reconciliation planning and authoring workflow adapters | Build execution framework, Runtime payload codecs, offline compression algorithms |
| asset editor modules | UI, preview, user commands and diagnostic presentation | Build recipes, DDC storage, source decoders not required by their UI boundary |

`AssetBuildCore` may depend on `Core` and the asset-family-neutral storage
surface of `AssetCore`. It must not depend on `Engine`. Recipe modules depend on
their Runtime value owner and `AssetBuildCore`. Concrete import modules depend
on the recipe modules, never the reverse.

### Minimal General Build Contracts

The V1 framework exposes only concepts that are already required by current
local production and remain useful for later remote/distributed evolution:

- an owner-qualified Build function identity and independently versioned
  implementation identity;
- an immutable Build definition containing canonical recipe identity, target
  facts, and named immutable input/value references;
- immutable identified output values represented by owned/shared buffers with
  content identity and size, without asset-family interpretation;
- explicit cache-query, local-build, cache-store, data-return, priority, and
  cancellation policy;
- a request owner/scope that owns cancellation, request barriers, waits,
  callbacks, and diagnostic aggregation;
- a local Build-function registry with duplicate-identity rejection and a
  lifetime token/handle;
- a service-contribution registry for family coordinators that supports
  admission, completion pumping, bounded waits, aggregate snapshots, and
  ordered drain without adding fields to a central service struct;
- a cache client that maps opaque namespaces, keys, and values to the existing
  asset-family-neutral object store while keeping family serialization and
  compatibility interpretation in the recipe module.

Build-key field order remains owned by typed recipe key-input `Serialize`
functions. Runtime payload field order remains owned by Runtime value
`Serialize`. The generic framework never hashes reflected `DObject` memory,
constructs a key from timestamps/paths, or interprets an Engine payload.

The first implementation may execute registered functions only in-process and
locally. Its definitions and values must nevertheless avoid raw pointers,
process-local callback identity, mutable objects, absolute source paths, and
other facts that would make later export impossible. Any input that cannot yet
be represented portably stays outside the generic definition behind a named
local-only capability, rather than silently weakening the contract.

### Request, Worker, and Publication Boundaries

- Capture of object state, source provenance, settings, reconciliation facts,
  package identity, and relationship facts occurs on the GameThread before a
  Build definition/request becomes immutable.
- Build workers consume only immutable or uniquely owned values and return
  detached products/values. They do not touch a `DObject`, package, registry,
  editor model, render resource, RHI object, or provider instance.
- DDC value serialization calls the Runtime value's owning `Serialize`
  operation, but cache lookup/store and Build policy remain Developer-owned.
- Publication is a separate GameThread adapter that revalidates generation and
  dependencies, exchanges complete Engine state, and commits through existing
  `AssetCore` transaction semantics.
- TextureCube receives explicit normalized panorama/faces request and detached
  product types before moving to `TextureBuild`.
- StaticMesh capture converts material slots, normalization settings, object
  identity used for deterministic diagnostics/reconciliation, and source facts
  into an owned snapshot; its recipe no longer accepts `DStaticMesh&`.

### Lifecycle and Threading

- One host owns process admission. Editor and headless authoring hosts start it
  after the task system and before Build functions accept requests.
- Recipe modules register functions and optional service contributions; the
  host never names Texture2D, StaticMesh, or another family.
- Registration/unregistration and completion pumping occur on the GameThread.
  Duplicate function/service identities fail explicitly.
- Each registration owns or references a request scope. Unregistration first
  stops that scope's admission, then cancels, waits, pumps accepted
  completions, drains callbacks, and finally removes the function/service.
- Provider unload drains provider/import request scopes before Build module
  registrations disappear. Build modules drain before Runtime objects, the
  process task system, or Core module infrastructure shut down.
- `MainFrame`, `StandardAssetImport`, recipe modules, and lazy submissions may
  request an already-running host but do not independently claim shutdown
  ownership. Shutdown is idempotent for failure recovery, not a substitute for
  one explicit owner.
- Synchronous recipes use the same immutable definition/value and cache policy
  contracts without being forced through a worker thread.

### Failure and Compatibility Policy

- Missing cache data may build only when the caller's policy permits it.
  Storage corruption is reported separately from recipe/schema incompatibility;
  the recipe module decides whether its authoring workflow may rebuild.
- A cache write preserves each current family's qualified failure or
  best-effort semantics. This plan does not silently normalize differing
  publication behavior.
- Cancellation and stale completion never publish partial state. A failure at
  capture, key construction, cache read, Build, serialization, cache store, or
  publication preserves the previous complete asset state.
- Module/function absence is an explicit authoring capability error. Runtime
  load never discovers or requests a builder.
- Existing key bytes, namespaces, payload bytes, status classification, Cooked
  data, and source/provider fingerprints remain unchanged during physical
  moves. Any unavoidable byte change is removed from this plan or approved as
  a separately versioned migration before implementation.
- Payload schema versions remain Runtime-owned; Build function/builder/
  compressor versions remain recipe-owned; provider versions remain import-
  owned; generic framework/cache protocol versions remain `AssetBuildCore`-
  owned.

### Module Granularity and Future Evolution

The initial Texture/Geometry split is selected from measured differences:

- texture production owns the current offline BC compressor, most asynchronous
  coordination, panorama projection, and mip/format policy;
- geometry production shares conversion, topology/relationship validation,
  collision/hierarchy preparation, and currently has no separate deployable
  third-party compressor boundary;
- TerrainHeightmap is too small to justify its own module and remains in
  `GeometryBuild` until a measured terrain dependency or release boundary
  exists.

Future non-Engine recipe modules such as `AudioBuild` or `NavigationBuild`
depend on `AssetBuildCore` and their Runtime value owners. Adding one must not
require editing a central asset-family enum, config struct, pump switch, or
shutdown switch. A later split of `GeometryBuild`, remote executor, Build worker
process, or shared DDC backend extends the generic registration/definition/value
contracts instead of moving Runtime serialization or source translation again.

## Current Foundations and Gaps

### Foundations to Preserve

- `EngineAssetBuildBoundary` completed canonical archives, Runtime-owned value
  serialization, Build-owned key serialization, normalized translators,
  detached products, narrow publication, Cook separation, and product closure.
- `Asset::FDerivedDataObjectStore` already stores opaque byte values under
  family namespaces and owns atomic publication/budget mechanics without
  interpreting Engine types.
- Texture2D has owned worker requests/results, bounded admission, priority,
  cancellation, latest-generation publication, diagnostics, waits, and a
  GameThread mailbox.
- TextureCube has normalized panorama/faces builders even though its outer
  operation still combines Build and publication.
- StaticMesh, skeletal, animation, and terrain values own their Runtime
  serialization and expose Build-owned key inputs and detached product/value
  structures.
- Runtime Engine exposes narrow authoring/post-load/collision/uncooked-loader
  registration seams without loading an authoring module.
- Root CMake dependency-closure assertions and editor/game deployment checks
  already distinguish Runtime, Build, source translation, Assimp, and offline
  compression.
- Focused and full native tests cover golden values/keys, hostile input, cache,
  import/reimport, Cook, publication rollback, lifecycle, editor consumers,
  Vulkan resource creation, tools, and source/DDC-free game loading.

### Gaps This Plan Closes

- `FAuthoringBuildServiceConfig` and service state contain Texture2D directly,
  so adding a coordinator currently requires modifying a central service.
- Host ownership is distributed across `EngineAssetBuildModule`, `MainFrame`,
  `StandardAssetImport`, and lazy Texture2D submission.
- TextureCube directly accepts/mutates `DTextureCube` in its outer Build
  operations, while StaticMesh recipe construction reads reconciliation state
  from `DStaticMesh&`.
- Family implementations repeat local cache serialization/status plumbing
  without a provider-neutral Build definition/value/policy boundary.
- One API macro and DLL export 77 current public Build declarations; target and
  test consumers link the aggregate module even when they use one family.
- The BC compressor is linked at the aggregate module boundary instead of being
  private to texture production.
- `StandardAssetImport` publicly exposes some `EngineAssetBuild` request and
  coordinator types, broadening transitive authoring dependencies.
- DurinDevTool has runtime/editor creation defaults but no developer
  scaffolding default, even though custom module paths and explicit enablement
  already support the architecture.

## Implementation Stages

### Stage 0: Freeze the target graph and compatibility baseline

Dependencies: completed `EngineAssetBuildBoundary` plan and a clean qualified
editor/game baseline.

- [x] Inventory every current `EngineAssetBuild` public declaration, include,
  dmodule/CMake dependency, dynamic load/shutdown string, module-root entry,
  program dependency, test target, API macro, third-party link, and deployed
  binary.
- [x] Classify every public operation as generic Build mechanism, typed recipe,
  object-state capture, source translation, DDC serialization/policy,
  publication, editor presentation, or temporary compatibility surface.
- [x] Record the exact Texture2D, TextureCube, StaticMesh/collision,
  SkeletalMesh, AnimationClip, and TerrainHeightmap key bytes, DDC namespaces,
  payload golden bytes, status/failure behavior, builder/compressor/provider
  identities, and Cook/runtime expectations that a module move must preserve.
- [x] Finalize the V1 names and ownership of Build function identity,
  definition, named input/value, policy, request owner, function registration,
  service contribution, host, cache client, snapshot, and registration-token
  contracts. Resolve any contract that cannot remain free of Engine types
  before implementation.
- [x] Define the exact direct/public/private dependency graph for
  `AssetBuildCore`, `TextureBuild`, `GeometryBuild`, `StandardAssetImport`,
  `MainFrame`, asset editors, tests, Cook and `DurinAssetTool`.
- [x] Add or update executable dependency-closure tests that will fail when
  `AssetBuildCore` reaches Engine/import/UI/RHI, a recipe reaches import/UI,
  Runtime reaches Developer modules, or Game reaches Build/import/compressors.
- [x] Decide the migration order for API headers and export macros. Any
  forwarding header is named, delegates only, has a removal stage, and cannot
  create a permanent aggregate facade.
- [x] Rebaseline editor and game selected module/deployment inventories before
  changing descriptors or binaries.

Stage 0 completed (2026-08-13): the aggregate module contained 77 exported
declarations/macro sites across 15 public headers: 43 Texture, 26 Geometry
(including 12 skeletal/animation), 6 family-neutral host lifecycle, and the two
API-macro definitions. The final ownership map assigns generic identity, definition,
value, policy, registry, request scope, cache client, service contribution,
snapshot, registration token, and host contracts to `AssetBuildCore`; every
Texture2D/TextureCube declaration to `TextureBuild`; every StaticMesh,
collision, SkeletalMesh, AnimationClip, and TerrainHeightmap declaration to
`GeometryBuild`; and no declaration to a compatibility facade. Capture and
publication remain typed family adapters, source translation remains in
`StandardAssetImport`, and presentation remains in editor modules.

The frozen direct graph is `AssetBuildCore -> Core + AssetCore`,
`TextureBuild -> AssetBuildCore + Engine` with private `bc7enc_rdo`,
`GeometryBuild -> AssetBuildCore + Engine`, and
`StandardAssetImport -> AssetImportCore + TextureBuild + GeometryBuild` plus
its existing Runtime/editor workflow dependencies. `MainFrame` depends on the
generic host and selected recipe modules; Cook, migration, repair, tests and
tools select only the recipe modules they exercise. Existing root closure
assertions are the executable pre-move baseline and will be renamed/tightened
as each final target appears. Migration is vertical: Core first, then all
Texture headers/sources and consumers, then all Geometry headers/sources and
consumers, followed by direct removal of the old target/API/load name; no
forwarding headers are selected.

Compatibility is frozen by the existing canonical tests: Texture2D key
`ceabc87aee9e8db676c2f6c13020593f`, TextureCube six-face key
`61dec1a0575878952e205558f058bd2d`, StaticMesh key
`423fd576f6529b0df5c564c4f093ae11`, SkeletalMesh key
`d4a4365a271f98c048d49b3491170eb3`, AnimationClip key
`a8eb4f43273627152dd71bea93f6d2e9`, and TerrainHeightmap key
`7b5c3faf0186011b52e3ff3368519321`. Payload hashes remain the checked-in
TXPL format fixtures, TextureCube `d476639b4d52cee3e9b5db3b09e6c874`,
StaticMesh `f8a1b99877a1dd9fd070e498ba1ca9b2` and
`fc478ee22fb777e44d793448c41be804`, SkeletalMesh
`fc7f61d6067225ce84e3c50ccce55c51`, AnimationClip
`7da58a36cd32f38a3dcb1daa910994f7`, and TerrainHeightmap
`b82a8b45c019f5a7a7d9c748c9d25d17`. The recipe-owned store namespaces,
status enums, builder/provider/compressor versions, Cook expectations and
runtime payload magic (`TXPL`, `DMSH`, `DCOL`, `DSKM`, `DANM`, `THPL`) remain
byte-for-byte governed by those tests.

The pre-move `Win64-Debug-DurinEditor` inventory contains
`DurinEditor-EngineAssetBuild.dll`, `DurinEditor-StandardAssetImport.dll` and
the existing editor modules; the `DurinGame` inventory contains none of them
and no offline compressor DLL. `TextureTests` (66 run, 64 passed and two
existing skips), `StaticMeshTests` (68), `SkeletalAssetTests` (34),
`TerrainHeightmapTests` (6), `AssetCookTests` (12), and
`LaunchProcessBoundaryTests` (2) passed before any source or descriptor move.
Exportable V1 definitions contain only canonical owner-qualified identities,
target facts and owned named byte values; function implementations,
GameThread capture/publication adapters and any process callback stay
explicitly local-only.

#### Acceptance Gate

- Every symbol and consumer has exactly one selected final module or deletion
  stage; no ownership or public/private dependency decision remains ambiguous.
- Baseline tests prove current keys, payloads, cache behavior, Cook/runtime
  behavior, lifecycle, and deployed game closure before files move.
- The selected generic contracts contain no Engine type, `DObject`, source
  provider, editor UI, RHI object, or family switch and have an explicit local-
  only versus future-exportable boundary.

### Stage 1: Establish AssetBuildCore and Developer module selection

Dependencies: Stage 0 contract and graph freeze.

- [x] Extend DurinDevTool module creation with `--kind developer`, the
  `Source/Developer` default path, `DurinEditor` root default, dry-run output,
  descriptor mutation, collision validation, help text, and focused tool tests.
- [x] Create `AssetBuildCore` under `Engine/Source/Developer` with explicit
  dependencies on only the generic Core/AssetCore surfaces required by its
  public and private contracts.
- [x] Implement immutable Build identity/definition/input/value and policy
  primitives without moving any asset-family key field order or Runtime payload
  serializer into the new module.
- [x] Implement local function registration with stable owner-qualified
  identity, lifetime handles, duplicate rejection, lookup diagnostics, and
  module-unload-safe unregister rules.
- [x] Implement request owners/scopes with cancellation, priorities, request
  barriers, bounded waits, terminal callbacks, and diagnostics that work for
  synchronous and asynchronous local functions.
- [x] Implement family-neutral service contributions and a single host that
  owns admission, completion pumping, aggregate snapshots, wait/drain, and
  shutdown ordering without naming Texture2D or another family.
- [x] Implement opaque cache query/store plumbing over the existing generic
  object store. Preserve recipe-owned serialization, namespaces,
  compatibility classification, budget decisions, and family-specific write
  failure semantics.
- [x] Add Core-level tests for canonical identities/definitions, duplicate and
  missing functions, policy combinations, cache hit/miss/storage error,
  cancellation, callback ordering, registration lifetime, concurrent request
  owners, pump/wait/drain, repeated initialization, and shutdown.
- [x] Add compile/dependency tests proving that public `AssetBuildCore` headers
  and its complete target closure contain no Engine, DObject, import, editor UI,
  RHI, Assimp, image decoder, or offline compressor dependency.
- [x] Update build-system, runtime-variant, workspace, module-creation, and code-
  module documentation with the Developer selection contract.

Stage 1 completed (2026-08-13): DurinDevTool now accepts the typed
`developer` kind, defaults it to `Source/Developer/<ModuleName>` and the
`DurinEditor` root, and preserves explicit path/enablement authority. The
focused scaffolding/registry suite passed 43 tests and a repository dry run
showed only the expected Developer tree plus `Engine.dproject` mutation.

`AssetBuildCore` is selected independently beside the still-unchanged
`EngineAssetBuild`. It exposes owner-qualified identities, immutable named
content-identified values, portable definitions, explicit cache/build/store/
return policy, local function registrations, request owners, opaque
`FDerivedDataObjectStore` routing, generic service contributions and one
restartable host. Registered implementation callbacks are explicitly
local-only and never enter an exportable definition. Duplicate identities,
missing lookup, two independent functions, cancellation/bounded wait,
terminal callbacks, cache hit/miss/skip/store behavior, two independent
services, aggregate snapshots, deterministic drain and repeated init/shutdown
passed in `AssetBuildCoreTests` (5 tests). Configure-time closure requires only
`Core`/`AssetCore` and rejects Engine, DObject/import/UI/RHI, Assimp, image/
offline compression targets; the `DurinGame` closure also rejects
`AssetBuildCore`.

#### Acceptance Gate

- An asset-family-free native test module can register two local Build
  functions and two service contributions, execute/cache/cancel/wait them, and
  unload them without changing `AssetBuildCore` source.
- `AssetBuildCore` has one explicit host owner, deterministic drain order, no
  family config fields/switches, and no Engine/import/UI/RHI dependency.
- `--kind developer` produces the documented path and `DurinEditor` enablement
  while leaving `BaseModules` and `DurinGame` unchanged.
- Existing `EngineAssetBuild` behavior remains unchanged while the new
  substrate is qualified independently.

### Stage 2: Extract TextureBuild and prove the extensible host

Dependencies: qualified `AssetBuildCore` registry, host, request ownership, and
cache contracts.

- [x] Create `TextureBuild` under `Source/Developer` with its own API macro,
  dmodule, module entry point, CMake target, PCH, and direct dependency metadata.
- [x] Move Texture2D request/product, builder, key, DDC, publication adapter,
  authoring service, coordinator, diagnostics, and tests from
  `EngineAssetBuild` without changing public semantics or byte identities.
- [x] Replace the Texture2D field embedded in `FAuthoringBuildServiceConfig` and
  service state with a `TextureBuild` registration/service contribution owned
  by the texture module.
- [x] Make `MainFrame` pump the generic host and make Texture2D-specific waits,
  cancellation and diagnostics resolve through the texture registration/request
  owner rather than a global family pointer.
- [x] Define normalized TextureCube request/product/publication-context types;
  move projection, platform build, key, DDC and publication into separate
  worker-safe recipe and GameThread adapter operations.
- [x] Migrate `StandardAssetImport`, Texture editor/UI, post-load, source
  relocation, environment lighting, Cook, Vulkan consumers, tools and tests to
  the `TextureBuild` APIs.
- [x] Make `bc7enc_rdo` and any texture-only implementation library private to
  `TextureBuild`; prove that `AssetBuildCore`, `GeometryBuild` and non-texture
  consumers do not acquire it transitively.
- [x] Preserve Texture2D latest-wins, admission budget, interactive priority,
  cancellation, source relocation, diagnostics, save/Cook waits, provider
  unload, and shutdown behavior through the generic host.
- [x] Run the focused Texture build/key/DDC, coordinator/failure, import,
  property editing, Cook, editor, material/environment-lighting, renderer and
  Vulkan test targets.

Stage 2 completed (2026-08-13): all Texture2D and TextureCube public/private
recipe sources moved directly to `Source/Developer/TextureBuild` with the
`TEXTUREBUILD_API` boundary; no forwarding header or duplicate implementation
was retained. `bc7enc_rdo` is now a private `TextureBuild` implementation
dependency and the aggregate `EngineAssetBuild` closure explicitly excludes
it. TextureCube now builds an owned detached `FTextureCubeBuildProduct` from
normalized panorama/faces inputs; `PublishTextureCubeProduct` is the sole
mutable-asset adapter and `StandardAssetImport` supplies the captured
provenance context.

Texture coordination registers as `Durin.TextureBuild.Coordinator` through the
family-neutral host. `MainFrame` pumps/shuts down only that host, while
Texture-specific submission, waits and diagnostics resolve through the
texture-owned registration. The former family-shaped authoring service config
and implementation were deleted. Configure-time closure proves TextureBuild
depends on AssetBuildCore/Engine/the compressor but excludes import providers,
Assimp and the old aggregate module. `TextureTests` (66 run, 64 passed with two
existing skips), `SceneImportTests` (15), `TextureCookIntegrationTests` (1),
`SkyBoxTests` (10), `VulkanRHIIntegrationTests` (55), and
`AssetBuildCoreTests` (5) passed; `MainFrame` also built successfully. Existing
texture key/payload goldens, latest-wins/cancellation/admission behavior, Cook,
import and rendering therefore remain unchanged from Stage 0.

#### Acceptance Gate

- Texture2D and TextureCube have exactly one recipe/key/DDC path in
  `TextureBuild`; their workers accept only owned values and their publication
  adapters are the only Build-side operations accepting mutable texture assets.
- Adding `TextureBuild` to the generic host required registration only and no
  `AssetBuildCore` family field, enum, branch, or include.
- Golden texture keys/TXPL values, DDC behavior, import/reimport, cancellation,
  Cook/runtime load and rendered results match the Stage 0 baseline.
- No non-texture Developer consumer links the BC compressor.

### Stage 3: Extract GeometryBuild and remove object-bound recipes

Dependencies: Stage 2 proves a recipe module can register and operate through
the generic host without a central family switch.

- [x] Create `GeometryBuild` under `Source/Developer` with its own API macro,
  dmodule, module entry point, CMake target, PCH, and dependency metadata.
- [x] Move StaticMesh and collision imported values, requests/products, key
  inputs, conversion algorithms, DDC policy, diagnostics and publication
  adapters out of `EngineAssetBuild`.
- [x] Replace `DStaticMesh&` recipe input with a GameThread-captured immutable
  reconciliation snapshot containing only the material, normalization,
  provenance, stable identity and settings facts required by the recipe.
- [x] Move SkeletalMesh and AnimationClip normalized requests/products, key
  inputs, preparation, DDC policy, diagnostics and uncooked-loader registration
  into `GeometryBuild` without moving Runtime payload serialization.
- [x] Move TerrainHeightmap normalized samples, hierarchy recipe, key, DDC
  policy, diagnostics and publication adapter into `GeometryBuild`.
- [x] Register StaticMesh collision and skeletal uncooked-load capabilities from
  the geometry module through existing narrow Runtime Engine seams; preserve
  reverse-order unregister and no-provider runtime behavior.
- [x] Route geometry cache operations through the generic cache/value policy
  without changing family namespaces, value serialization, corruption/
  incompatibility classification, cleanup budgets, or publication behavior.
- [x] Migrate `StandardAssetImport`, Level/StaticMesh/Skeletal editor consumers,
  Scene import, terrain workflows, Cook, physics/viewport/rendering consumers,
  tools and tests to `GeometryBuild`.
- [x] Run focused StaticMesh, collision, SkeletalMesh, AnimationClip, Scene
  import, TerrainHeightmap, Cook, editor, physics, renderer, thumbnail, viewport
  and Vulkan test targets.

#### Acceptance Gate

- Geometry recipes and cache policy live only in `GeometryBuild`; workers and
  pure recipe entry points accept no mutable asset, package, registry, provider,
  render resource or RHI object.
- Golden DMSH/DCOL/DSKM/DANM/THPL keys and values, material reconciliation,
  skeleton compatibility, collision, terrain hierarchy, import/reimport,
  rollback, Cook/runtime and rendering results match the Stage 0 baseline.
- Geometry module registration changes no `AssetBuildCore` family source and
  proves at least two independent recipe modules can coexist, drain and unload.

Stage 3 completed (2026-08-13): `GeometryBuild` now owns StaticMesh/collision,
skeletal/animation and terrain recipe code, keys, diagnostics and publication
adapters. StaticMesh pure build/cache entry points consume an immutable
GameThread capture instead of a mutable asset. All geometry object-store I/O
uses `FBuildCacheClient` while retaining the existing family namespaces,
canonical payload serialization and cleanup budgets. Geometry registers its
host contribution plus Runtime collision/uncooked-loader seams without adding
family logic to `AssetBuildCore`; an executable host test loads Geometry and
Texture contributions together, drains them, and restarts the host.

Validation evidence: `GeometryBuild` configured and built; `StaticMeshTests`
68/68, `SkeletalAssetTests` 34/34, `TerrainHeightmapTests` 6/6,
`TerrainHeightmapCookTests` 1/1, `SceneImportTests` 15/15,
`PhysicsSceneTests` 38/38, `StaticMeshThumbnailTests` 8/8,
`ViewportQualificationTests`, `SceneImportVulkanTests` 1/1 and the expanded
`TextureTests` 65 passed with the two established skips.

### Stage 4: Rewire hosts, import surfaces, tools, and remove EngineAssetBuild

Dependencies: every recipe and named consumer is available in its final module.

- [x] Make the editor's authoring root start one `AssetBuildCore` host after the
  task system, explicitly load selected recipe/import modules, pump generic
  completions, and drain the host before providers, objects, modules and tasks.
- [x] Remove independent shutdown ownership from `StandardAssetImport`, recipe
  modules, and lazy submission while retaining bounded failure-recovery cleanup.
- [x] Make headless Cook/migration/repair tools select only the generic host,
  recipe modules, and source providers their operation requires. Preserve the
  package-only `DurinAssetTool` audit with no Build/import DLL loaded.
- [x] Rework `StandardAssetImport` public APIs so implementation-only recipe
  types do not force aggregate/transitive Build dependencies. Keep a public
  recipe dependency only where the public contract intentionally exposes that
  typed recipe value and record the reason.
- [x] Update `Engine.dproject`, dmodule public/private dependencies, CMake
  targets, program dependencies, runtime loading, native-test metadata, output
  deployment and root closure assertions for the three Developer modules.
- [x] Remove every migration forwarding header, `ENGINEASSETBUILD_API` use,
  `EngineAssetBuild` include/link/load/shutdown string, module descriptor,
  CMake target, DLL expectation, project entry, source directory, and test
  dependency.
- [x] Update `CodeModules`, `AssetImportFramework`, `AssetDataLifecycle`,
  `RuntimeLifecycle`, build-system, runtime-variant, workspace and Build/run
  documentation to name the final owners and selection/lifecycle contract.
- [x] Add target/deployment tests proving the editor and required headless tools
  receive the selected Developer modules while `DurinGame` and package-only
  tools do not.

#### Acceptance Gate

- `rg`, generated metadata, configured targets and deployed binaries contain no
  obsolete `EngineAssetBuild` source/API/target/load dependency.
- Editor startup, frame pumping, project/source operations, provider unload,
  save/Cook waits and shutdown have one explicit host lifecycle and drain with
  no accepted callback targeting an unloaded module or destroyed object.
- `StandardAssetImport` remains the only standard concrete source translator;
  recipe modules contain no encoded-source decoder or provider dependency.
- Runtime and game closures contain no Developer module, Build function/key,
  DDC builder/coordinator, Assimp, editor image decoder or offline compressor.

Stage 4 completed (2026-08-13): MainFrame explicitly loads AssetBuildCore and
the selected recipe modules, starts the generic host after task startup, pumps
it per frame and drains it before editor teardown. StandardAssetImport and
Texture submission no longer acquire independent host ownership. The migration
tool dynamically loads AssetBuildCore/GeometryBuild/TextureBuild and drains in
reverse order, while PE import inspection proves its package-only executable
has no Build DLL import. GeometryBuild is private to StandardAssetImport;
TextureBuild remains public solely because the public Texture2D translation
contract intentionally exposes its settings, priority and completion values.

The EngineAssetBuild target, descriptor, API macro/header, source directory,
load strings, dependencies and stale deployed artifacts are removed. Configure,
MainFrame and DurinAssetTool builds passed; package audit and migration-plan
processes both exited 0; AssetBuildCoreTests 5/5, TextureTests 65 passed with
two established skips, StaticMeshTests 68/68, SceneImportTests 15/15 and
LaunchProcessBoundaryTests 2/2 passed.

### Stage 5: Qualify the modular product boundary and future-framework seam

Dependencies: final modules and host/tool selection are complete; no
compatibility facade remains.

- [x] Add API compile tests for immutable definitions/values, request ownership,
  recipe registration, object-free worker operations, private publication
  adapters, and absence of family types from `AssetBuildCore` public headers.
- [x] Add lifecycle tests for arbitrary module registration order, duplicate
  identities, partial startup rollback, provider unload, recipe unload with
  active requests, cancellation races, callback reentrancy, wait timeout,
  repeated shutdown, and final task-system drain.
- [x] Add cache-policy tests covering query/build/store combinations, missing,
  storage corruption, recipe incompatibility, cancellation, best-effort versus
  required writes, immutable value identity, and exact family namespace/key
  preservation.
- [x] Exercise an asset-family-free sample Build function and one local-only
  function through the same registry, proving future functions can be added
  without Engine types or edits to the host.
- [x] Run all affected focused targets, then the full native aggregate because
  the change crosses shared authoring lifecycle, cache policy, module loading,
  tools, Cook and multiple test targets.
- [x] Complete clean full `all` builds for the selected editor and game Agent
  Build Profiles, inspect module/third-party deployment, run package-only and
  migration `DurinAssetTool` process tests, and run source/DDC-free cooked game
  loading/rendering smoke.
- [x] Move every lasting rule into its owning documentation, update this plan's
  evidence and checklists, set its completion metadata, and commit the final
  handoff with exact plan/stage provenance.

#### Acceptance Gate

- All focused and full native tests, clean editor/game full builds, deployment
  inspection, Cook, tools and runtime smoke pass from the final module graph.
- Current asset keys, payloads, namespaces, authored packages, import records,
  Cooked products and runtime results remain compatible with the Stage 0
  baseline.
- A new local recipe module can define typed requests/products, register Build
  functions/services, use opaque cache policy and participate in host drain
  without editing `AssetBuildCore`, Runtime `Engine`, `MainFrame`, or an asset-
  family switch.
- The final design documents which V1 inputs/functions are exportable versus
  explicitly local-only and identifies remote execution as an additive future
  transport/executor, not another ownership migration.

Stage 5 completed (2026-08-13): AssetBuildCore contract coverage now has 10
tests for portable immutable values, identity validation, independent and
asset-family-free functions, duplicate registration, cancellation, active
request unload/drain, callback reentrancy, wait timeout, partial-start rollback,
ordered/repeated shutdown and required versus best-effort cache writes. CMake
closure and direct public-header compilation enforce that the core has no
Engine family dependency; family golden/corruption/incompatibility suites retain
exact namespaces, keys and values.

Final validation: the 70-target default native aggregate passed at four-way
parallelism; all six qualification targets passed (one Spline timing sample
was rerun after a 0.095 ms transient overage); clean `all` rebuilds passed for
`Win64-Debug-DurinEditor` and `Win64-Debug-DurinGame`. Source/DDC-free cooked
runtime tests passed for texture, StaticMesh, skeletal assets and terrain.
Editor deployment contains exactly AssetBuildCore, GeometryBuild and
TextureBuild; game contains no Build DLL; DurinAssetTool imports no Build DLL
and both package audit and migration-plan processes exited 0. Lasting ownership,
selection, lifecycle and exportability rules are recorded in the owning docs.

## Stage Execution Rules

- Preserve one source/build writer per checkout and follow the repository root
  build/test/long-running-operation rules.
- Do not combine a physical module move with an unapproved key, payload,
  namespace, schema, compression, target, or Cook-format change.
- Complete one vertical recipe move—API, algorithm, DDC, registration,
  publication, consumers, tests and closure—before deleting its old source.
- Temporary forwarding headers delegate only and are deleted in their named
  stage; no duplicate recipe, key serializer, payload serializer or lifecycle
  owner may survive an acceptance gate.
- `AssetBuildCore` may gain a generic concept only when at least two current
  consumers use it or Stage 0 records why it is required for the stable future
  contract. Family data remains typed in recipe modules.
- Never make a generic interface by accepting `void*`, `DObject*`, unowned
  spans, process-local pointers, provider instances, or arbitrary mutable
  callbacks in an otherwise immutable/exportable definition.
- When implementation conflicts with a selected invariant, update Stage 0 and
  the design rationale before changing code; do not hide divergence in a
  migration wrapper.
- Update `Last reviewed`, `Current Status`, stage checklists, handoff evidence,
  lasting documentation and plan provenance in each substantive stage commit.

## Validation Matrix

| Scenario | Required behavior | Evidence owner |
| --- | --- | --- |
| Developer scaffolding | `--kind developer` selects `Source/Developer` and `DurinEditor` roots while custom path/enablement remains authoritative | DurinDevTool module-creation tests |
| Core dependency closure | `AssetBuildCore` excludes Engine, DObject, import, UI, RHI, concrete codecs and compressors | DHT/CMake closure and API compile tests |
| Recipe dependency closure | Texture/Geometry depend on Runtime values/Core mechanisms and exclude StandardImport, Assimp, image decoders and UI | DHT/CMake closure and binary inspection |
| Function identity | Stable owner-qualified identities reject duplicates and separate function, recipe, payload and provider versions | AssetBuildCore contract tests |
| Immutable definitions/values | Inputs/outputs are owned, named and content-identified; exportable definitions contain no process-local facts | AssetBuildCore/API compile tests |
| Policy behavior | Query/build/store/data-return/priority/cancel combinations preserve explicit caller intent | AssetBuildCore cache/request tests |
| Cache routing | Generic storage sees opaque namespaces/keys/values; recipe modules own serialization and compatibility | AssetBuildCore, AssetCore and family DDC tests |
| Worker isolation | Build functions touch no object/package/registry/provider/editor/render/RHI state | Recipe API compile tests and controlled worker tests |
| Publication isolation | Only GameThread adapters exchange complete products and commit transactions | Family failure/rollback tests |
| Multi-module services | Texture and Geometry register, pump, wait, drain and unload without central family branches | Host lifecycle tests |
| Texture behavior | Texture keys/TXPL, compression, panorama/faces, async behavior, diagnostics and Cook/rendering remain stable | Texture, import, Cook, material/environment and Vulkan tests |
| Geometry behavior | DMSH/DCOL/DSKM/DANM/THPL, reconciliation, compatibility, collision/hierarchy and Cook/rendering remain stable | Mesh/skeletal/animation/terrain, Scene, physics, Cook and Vulkan tests |
| Host ordering | Admission starts after tasks; providers drain before recipes; recipes drain before objects/modules/tasks | Editor/headless lifecycle and shutdown tests |
| Tool selection | Package-only audit loads no Build/import module; migration/Cook select required generic/recipe/provider modules explicitly | DurinAssetTool and Cook process tests |
| Game deployment | Game closure and directory contain no Developer/import modules, Assimp, image decoder or offline compressor | CMake closure, deployment inspection and game smoke |
| Compatibility | Golden keys/payloads/namespaces, authored packages, records and Cooked results match the pre-split baseline | Existing golden/integration suites |
| Extensibility seam | Asset-family-free and local-only sample functions register without Engine types or host edits | AssetBuildCore extensibility tests |

## Definition of Done

- `AssetBuildCore`, `TextureBuild`, and `GeometryBuild` are the only selected
  Developer Build modules; no `EngineAssetBuild` source, target, binary, API,
  dependency or runtime load name remains.
- Developer module creation and selection are documented, tool-supported and
  mechanically distinct from Runtime/Game inclusion without relying on path
  spelling or a new runtime variant.
- `AssetBuildCore` owns only generic Build/cache/request/service/host mechanics,
  depends on no Engine asset type, and accepts new functions/modules without a
  family enum, field, switch or lifecycle edit.
- Texture and geometry recipe modules own their typed requests/products, key
  serializers, algorithms, versions, namespaces, DDC policy, diagnostics and
  registration; texture-only compressors are private to `TextureBuild`.
- Runtime modules continue to own runtime values, value serialization,
  validation, publication/exchange seams and resource construction without
  discovering authoring capabilities.
- Standard import providers remain the only owners of concrete source
  translation and import policy; editor modules remain presentation and user-
  workflow owners.
- Build workers consume owned values and return detached products; object
  capture and publication occur only on the GameThread through failure-atomic
  seams.
- Editor, Cook, tools and tests select and drain authoring capabilities
  explicitly; Game loads cooked outputs with no Build/import/DDC fallback or
  deployed Developer dependency.
- Current keys, DDC values, Cooked values, authored assets, import records,
  diagnostics and workflows are compatible with the pre-split baseline.
- Focused/full native tests, clean editor/game full builds, deployment
  inspection, Cook/tool/game process tests, lasting documentation, plan
  validation and committed handoff are complete.

## Deferred Follow-ups

- Remote/shared DDC backends, distributed scheduling, build farms, worker
  processes, sandboxing, cross-machine input transfer, remote capability
  negotiation and result attestation.
- Exporting currently local-only Build functions once their inputs and
  dependencies have portable immutable representations.
- Splitting `GeometryBuild` into Mesh/Skeletal/Terrain modules after a measured
  optional dependency, deployment, lifecycle, ownership or release-cadence
  requirement.
- Adding `AudioBuild`, `NavigationBuild`, shader/pipeline recipes, plugin-owned
  recipes, or non-Engine Runtime value owners; these should validate the V1
  extension seam rather than broaden this migration.
- Multi-value partial fetch, hierarchical/incremental actions, content-addressed
  input graphs, action memoization across processes, remote execution policy,
  build trace visualization and farm observability.
- Runtime source import, mod ingestion, hot-build deployment and live Shipping
  Build capabilities.

## Related Documentation

- [Engine Asset Serialization and Build Boundary Plan](EngineAssetBuildBoundary.md)
- [Code Modules](../../../Workspace/CodeModules.md)
- [Workspace and Projects](../../../Workspace/WorkspaceProjects.md)
- [Build System](../../../Development/Build/BuildSystem.md)
- [Build and Run](../../../Development/Build/BuildAndRun.md)
- [Runtime Variants](../../../Development/Build/RuntimeVariants.md)
- [Asset Import Framework](../../../Editor/Architecture/AssetImportFramework.md)
- [Asset Data Lifecycle and Storage](../../../Runtime/Assets/AssetDataLifecycle.md)
- [Runtime Lifecycle](../../../Runtime/Core/RuntimeLifecycle.md)
- [CPU Task System](../../../Runtime/Core/TaskSystem.md)

## Related Code

- `CMakeLists.txt`
- `Engine/Engine.dproject`
- `Engine/Source/Editor/EngineAssetBuild/`
- `Engine/Source/Editor/StandardAssetImport/`
- `Engine/Source/Editor/MainFrame/`
- `Engine/Source/Editor/TextureEditor/`
- `Engine/Source/Editor/StaticMeshEditor/`
- `Engine/Source/Editor/SkeletalMeshEditor/`
- `Engine/Source/Runtime/AssetCore/Public/DerivedDataObjectStore.h`
- `Engine/Source/Runtime/AssetCore/Private/DerivedDataObjectStore.cpp`
- `Engine/Source/Runtime/Engine/Public/Texture/`
- `Engine/Source/Runtime/Engine/Public/StaticMesh/`
- `Engine/Source/Runtime/Engine/Public/SkeletalMesh/`
- `Engine/Source/Runtime/Engine/Public/Animation/`
- `Engine/Source/Runtime/Engine/Public/Terrain/`
- `Engine/Source/Programs/DurinAssetTool/`
- `Engine/Source/Programs/DurinHeaderTool/`
- `Engine/Tests/Native/EngineTests/`
- `Engine/Tests/Native/VulkanRHITests/`
