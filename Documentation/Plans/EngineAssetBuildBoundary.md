# Engine Asset Build Boundary Plan

Summary: Extract source-driven Engine asset production from runtime Engine into an authoring-only EngineAssetBuild module while preserving cooked formats and import behavior.

Last reviewed: 2026-08-12

Status: Active
Completed:

## Current Status

Planning is complete and Stage 0 is ready to execute. Runtime `Engine` currently
owns both consumption and production paths for several asset families: runtime
asset objects and render resources sit beside source provenance inspection,
normalized import intermediates, asset-specific DDC builders, texture mip and
BC compression, panorama projection, rebuild diagnostics, and the Texture2D
background build coordinator. In editor configurations the `Engine` CMake
target directly links `bc7enc_rdo`.

The repository already enforces the intended product boundary in part:
`DurinGame` excludes the retired `EngineAssetBuild` identity and offline codec
libraries from its target dependency closure, runtime documentation requires
cooked-only loading without source or DDC fallback, and
`StandardAssetImport` already owns provider discovery and concrete source-format
policy. The missing work is a real module boundary that makes those constraints
structural rather than conditional code inside `Engine`.

This plan selects one authoring-only `EngineAssetBuild` module. It is available
to editor, cooker, asset-tool, and focused test targets, but absent from the
`DurinGame` module graph and deployment. Existing DMSH, TXPL, skeletal,
animation, terrain, package, DDC-object, and cooked-payload formats remain
unchanged unless Stage 0 finds a concrete correctness defect that requires an
explicitly versioned schema change.

## Goal

Make runtime Engine asset loading a pure consumer of validated authored or
cooked state while placing all source-driven, platform-producing work behind
one explicit `EngineAssetBuild` capability. Editor import, reimport, rebuild,
cook preparation, diagnostics, and command-line asset workflows must retain
their current transactional behavior and outputs, while a game build cannot
link, load, or invoke source decoders, offline encoders, DDC builders, or build
coordinators.

The final dependency direction is:

```text
Core / CoreDObject
        |
    AssetCore
        |
      Engine
        |
EngineAssetBuild (authoring profiles only)
        |                         \
StandardAssetImport          asset editors / cooker / DurinAssetTool
        |
AssetImportCore-driven editor hosts
```

`EngineAssetBuild` depends on `Engine`; `Engine` never depends on or dynamically
discovers `EngineAssetBuild`. Runtime asset objects expose only the minimal
value-state and publication seams required for an external builder to produce
and atomically exchange detached candidates.

## Scope

- Add the authoring-only `EngineAssetBuild` module and select it only for
  editor/cooker/tool/test roots that produce asset payloads.
- Move texture mip generation, panorama projection, platform-format selection,
  BC compression, build metrics, and Texture2D build coordination out of
  runtime `Engine`.
- Move asset-specific DDC key construction, DDC read/write/build policy, source
  rebuild decisions, and live rebuild diagnostics for StaticMesh, Texture2D,
  TextureCube, SkeletalMesh, AnimationClip, and TerrainHeightmap where those
  responsibilities currently reside in `Engine`.
- Split shared payload codecs where necessary: validation and decode required by
  cooked runtime remain in `Engine`; builder-only encoding, key construction,
  and DDC adapters move to `EngineAssetBuild`.
- Preserve lightweight reflected source provenance and build settings on the
  owning runtime asset schema when authored package serialization requires
  them; strip them during Cook according to existing contracts.
- Preserve `AssetCore` ownership of generic DDC object storage, package
  publication, cooked descriptors, DBLK, manifests, atomic byte publication,
  and mutation transactions.
- Make `StandardAssetImport`, relevant asset editors, Cook paths, and
  `DurinAssetTool` explicitly consume `EngineAssetBuild` where they require
  production capabilities.
- Use the existing StaticMesh decoder registration and imported-state exchange
  mechanisms as bounded migration seams; retire Runtime Engine registration
  APIs only after all explicit consumers have moved.
- Prove that editor-authored outputs and cooked runtime payloads remain
  compatible and that `DurinGame` has no transitive authoring dependency.

## Non-Goals

- Redesigning `.dasset`, DMSH, TXPL, DBLK, Cook manifest, DDC object, skeletal,
  animation, or terrain payload formats without a separately justified and
  versioned correctness change.
- Replacing `AssetImportCore` plans, snapshots, candidates, publication
  transactions, request scopes, or asynchronous import coordination.
- Moving generic DDC storage or package/cook infrastructure from `AssetCore`.
- Combining Assimp, PNG/HDR/image parsing, glTF/FBX policy, provider discovery,
  or Scene reconciliation into `EngineAssetBuild`.
- Creating one physical module per asset family. Texture, mesh, skeletal,
  animation, and terrain builders remain internal domains of one module until
  an optional dependency, deployment, unload lifecycle, or release cadence
  requires another boundary.
- Enabling runtime source import, runtime DDC fallback, runtime asset mutation,
  hot build deployment, or shipping build coordinators.
- Changing editor UX, asset defaults, compression results, import naming,
  source ingestion, or reimport reconciliation as part of the extraction.
- Rewriting existing source decoders or normalized intermediate formats merely
  to move ownership.

## Design Decisions and Invariants

### Module identity and selection

- The module is named `EngineAssetBuild` to distinguish Engine-asset-specific
  production from `AssetCore`'s generic storage and transaction facilities.
- Its source lives under the authoring/editor module set and it is selected by
  capability roots, not by runtime asset-class reachability. It may be linked
  by editor, cooker, `DurinAssetTool`, and native-test targets.
- `DurinGame` neither selects nor transitively reaches `EngineAssetBuild`.
  Game configuration must not compile an inert copy, deploy its DLL, or rely on
  `DURIN_WITH_EDITOR` branches inside runtime `Engine` to hide it.
- `bc7enc_rdo`, `rgbcx`, and future offline encoders link only to
  `EngineAssetBuild` or a later explicitly split builder module. Assimp and
  concrete source decoder libraries remain provider dependencies of
  `StandardAssetImport`.

### Runtime Engine ownership

- `Engine` owns reflected runtime asset classes, schema needed to deserialize
  authored and cooked packages, cooked payload descriptors, strict runtime
  payload validation/decode, runtime CPU data, render resources, readiness,
  and transactional publication of accepted detached state.
- Runtime package load never opens source files, invokes an import provider,
  generates mips, compresses blocks, writes DDC, or queues a build. Missing or
  incompatible required cooked data is an explicit load failure.
- Lightweight source provenance, hashes, importer/decoder identities, and
  authoring build settings may remain reflected Engine value types because the
  owning asset must deserialize authored packages without loading a build
  module. Behavior that interprets those fields belongs to
  `EngineAssetBuild` or a provider.
- Runtime-visible status is limited to consumption facts such as payload
  availability, validation failure, and render-resource readiness. Queue,
  phase, timing, compression, source-inspection, DDC, and rebuild diagnostics
  are authoring facts.

### Build module API and publication

- Public build APIs consume immutable or owned value snapshots and return
  detached value results. Worker tasks never access `DObject`, packages,
  registry models, render resources, or RHI state.
- Main-thread consumers publish accepted results through narrow Engine-owned
  exchange/commit seams. The build module may depend on Engine public value
  types; Engine headers and implementations never include build-module APIs.
- Build requests capture every source byte identity, normalized setting,
  target/profile, builder/schema version, and dependency required for a stable
  DDC key. Physical paths and timestamps remain optimization or diagnostic
  facts, never key identity.
- A DDC miss or corrupt object is rebuildable only in an explicit authoring
  operation with complete inputs. Ordinary package load does not silently
  upgrade into an import or rebuild.
- Cancellation, bounded admission, latest-generation checks, completion
  mailboxes, and shutdown draining remain explicit. Moving the Texture2D
  coordinator cannot weaken its current task-system or main-thread publication
  contract.

### Import and source decoding boundary

- `AssetImportCore` remains format-neutral and owns capture, plans, provider
  leases, candidates, generic diagnostics, publication, and cancellation.
- `StandardAssetImport` owns file-format recognition, Assimp/image/glTF/FBX
  decoding, normalized source values, provider settings, output policy, and
  reconciliation. It calls `EngineAssetBuild` with normalized mesh, pixel,
  skeletal, animation, or height values.
- `EngineAssetBuild` owns conversion from normalized authoring values to
  platform/runtime payloads. It must not decide which provider recognizes a
  file, reopen arbitrary source dependencies, or retain provider-specific Scene
  policy.
- Existing `RegisterStaticMeshSourceDecoder` behavior remains available during
  migration so current providers and assets keep working. New code must not
  add another Runtime Engine registry. Once all call paths explicitly connect
  providers to builders, the registration API and complete imported
  intermediate model leave the Runtime Engine public surface or become a
  minimal value bridge with no decoder ownership.

### DDC, Cook, and format compatibility

- `AssetCore` continues to own generic disposable object stores and atomic
  publication. `EngineAssetBuild` owns asset-specific namespaces, keys,
  encoders, builder versions, cache decisions, and diagnostics.
- Runtime decode/validation code remains paired with stable payload schemas in
  Engine. Builder writers must be qualified against the runtime reader before
  their previous Engine implementation is removed.
- Extraction alone does not bump schemas, builder identities, DDC keys, or
  output bytes. Golden keys and encoded payload hashes characterize the
  baseline before movement.
- Cook may invoke `EngineAssetBuild` explicitly while authoring inputs exist,
  but the published cooked package and bulk payload contain no module identity,
  source dependency, DDC path, live diagnostic, or coordinator state.

### Failure and lifecycle behavior

- Every build constructs and validates a complete candidate before mutating a
  loaded asset. Cancellation, decoder failure, compression failure, DDC write
  failure, stale generation, and publication failure preserve the previous
  complete authored/runtime state according to existing contracts.
- Best-effort DDC write failure may leave a valid in-memory candidate usable;
  it cannot create a partial package publication. Disposable cache objects do
  not participate in authored rollback.
- Editor startup initializes build services after the task system and required
  provider modules. Project switch and shutdown stop admission, cancel or drain
  scoped work, pump/discard terminal completions, unregister providers, and
  unload the build module in a documented order.
- `DurinGame` lifecycle contains no build-service initialization, pump, wait,
  or shutdown branch.

## Current Foundations and Gaps

| Area | Existing foundation | Gap to close |
| --- | --- | --- |
| Module graph | Runtime variants select roots through `.dproject`; top-level CMake already rejects `EngineAssetBuild`, Assimp, and BC encoders from `DurinGame` closure | No production `EngineAssetBuild` module exists, so the assertion does not prove actual ownership |
| Engine CMake | BC7 is conditionally linked only when `DURIN_WITH_EDITOR` | Runtime Engine remains the link owner and authoring implementation host |
| Texture build | Value-oriented source/platform data, mip builder, panorama projection, DDC codec, metrics, cancellation, and bounded coordinator already exist | APIs and implementations are exported by Engine; asset objects own queue/request/diagnostic state |
| StaticMesh build | Imported value model, decoder registration, detached render-data candidate, imported-state exchange, DDC key and payload support exist | Complete import intermediates, decoder registry, source inspection, build, DDC, and diagnostics remain exposed by Engine |
| Skeletal/animation/terrain | Stable derived-data keys and validated payload paths already exist | Builder/key/write and runtime reader responsibilities share Runtime Engine files and public headers |
| Import framework | `AssetImportCore` is provider-neutral; `StandardAssetImport` owns built-in providers and Assimp and already registers the mesh decoder | Providers call build behavior through broad concrete Engine asset methods instead of an explicit build capability |
| Tooling | `DurinAssetTool` is a capability-separated process host | It links Engine/AssetCore/AssetImportCore directly and has no explicit builder dependency |
| Tests | Texture, import, cook, DDC, terrain, skeletal, Vulkan, and runtime-process tests cover much of the behavior | Tests link BC7 directly or reach authoring APIs through Engine, so they do not enforce the final boundary |

## Implementation Stages

### Stage 0: Freeze inventory, byte compatibility, and dependency gates

- [ ] Inventory every Runtime Engine public/private symbol that reads source,
  decodes authoring bytes, generates normalized/platform data, constructs an
  asset-specific DDC key, reads/writes DDC, coordinates background builds,
  performs reimport/rebuild, or reports authoring diagnostics. Classify each as
  Runtime Engine, AssetCore, EngineAssetBuild, AssetImportCore, or provider
  ownership.
- [ ] Record current module and third-party dependency closures for
  `DurinEditor`, `DurinGame`, `TextureTests`, asset import/cook targets, and
  `DurinAssetTool`, including deployed DLLs.
- [ ] Add characterization fixtures for StaticMesh, Texture2D, TextureCube,
  SkeletalMesh, AnimationClip, and TerrainHeightmap DDC keys, payload bytes,
  validation results, source-free cooked loads, and failure preservation.
- [ ] Freeze the exact Engine value types that must remain for authored package
  deserialization and the narrower detached build/publication seams required by
  each asset family. Record any type that cannot move and why.
- [ ] Freeze build-service initialization, editor tick/pump, project-switch,
  save/cook wait, provider unload, and process-shutdown ordering from current
  behavior.
- [ ] Record the Stage 0 handoff in this plan before moving source files or
  changing module descriptors.

#### Acceptance Gate

- Every candidate source and symbol has exactly one selected final owner; no
  implementation is classified simultaneously as Engine and EngineAssetBuild.
- Golden DDC keys, payload hashes, cooked load results, build status
  transitions, coordinator behavior, and failure transactions cover all moved
  asset families.
- The expected editor/tool/test and forbidden game dependency closures are
  explicit and mechanically inspectable.

### Stage 1: Introduce the module and move offline third-party ownership

Dependencies: Stage 0 ownership and dependency baselines.

- [ ] Add `EngineAssetBuild.dmodule`, its CMake target, API export, module
  lifecycle, and `Engine.dproject` selection for authoring roots. Give it only
  the minimal public/private dependencies required by the frozen inventory.
- [ ] Move the `bc7enc_rdo` link from Engine to `EngineAssetBuild`; keep Assimp
  and concrete source decoder deployment owned by `StandardAssetImport`.
- [ ] Update focused native-test targets so builder tests link
  `EngineAssetBuild` rather than BC7 or builder-only Engine APIs directly.
- [ ] Strengthen target dependency/deployment assertions so `DurinGame` rejects
  `EngineAssetBuild`, source decoder/provider modules, Assimp, image decoder
  libraries, BC encoders, and any authoring-only runtime file.
- [ ] Prove that an editor build loads the new empty/thin module in the selected
  lifecycle order while a game build neither builds nor deploys it as part of
  the runtime product.
- [ ] Record the Stage 1 handoff with target closures, deployed files, module
  load order, and unchanged characterization results.

#### Acceptance Gate

- `EngineAssetBuild` is a real independently linked module selected only by
  authoring capabilities.
- Runtime Engine no longer links an offline encoder, and the complete
  `DurinGame` closure/deployment is free of build and provider dependencies.
- No asset behavior, DDC key, payload byte, or cooked load result changes.

### Stage 2: Move pure builders, keys, writers, and DDC adapters

Dependencies: Stage 1 module and closure gates.

- [ ] Move Texture2D/cube mip generation, platform-format selection, BC
  compression, panorama projection, build metrics, and cancellation-aware pure
  algorithms into the texture domain of `EngineAssetBuild`.
- [ ] Split texture and mesh payload code so strict reader/validator and runtime
  data construction remain in Engine while writer, builder, key, and DDC-store
  adapters move to `EngineAssetBuild`.
- [ ] Apply the same split to StaticMesh collision/build data,
  SkeletalMesh, AnimationClip, and TerrainHeightmap where the Stage 0 inventory
  identifies source-driven or builder-only responsibilities.
- [ ] Replace broad builder access to mutable assets with immutable settings,
  provenance, source hashes, normalized input values, target/profile, and
  version snapshots. Return detached payloads, keys, metrics, and bounded
  diagnostics.
- [ ] Keep generic cache object storage and atomic I/O in AssetCore; remove
  duplicated cache/path/publication primitives discovered during movement.
- [ ] Qualify every moved writer against the unchanged Engine runtime reader
  and record the Stage 2 handoff with golden keys/hashes and failure matrices.

#### Acceptance Gate

- Engine contains no mip generation, projection, BC encoding, asset-specific
  DDC key construction, or DDC writer implementation selected for movement.
- Builder outputs are byte-identical to the Stage 0 baseline and remain
  strictly accepted by runtime Engine readers.
- Pure worker paths access no `DObject`, package, registry, render resource, or
  RHI state and preserve cancellation/resource bounds.

### Stage 3: Move build coordination, rebuild policy, and diagnostics

Dependencies: Stage 2 detached builder results and stable runtime readers.

- [ ] Move `FTexture2DBuildCoordinator`, its request table, admission budgets,
  worker phases, metrics, cancellation, completion mailbox, waits, and global
  lifecycle out of Engine and into `EngineAssetBuild`.
- [ ] Introduce asset-family build operations that inspect source provenance,
  resolve explicit authoring inputs, make DDC decisions, call pure builders,
  and return detached publication candidates without making Engine depend on
  the module.
- [ ] Move source availability, cache hit/miss/rebuild, queue, phase, timing,
  encoder, and last-build diagnostics into build-owned snapshots or an
  authoring service indexed by stable asset/request identity. Retain only
  serialization-required or runtime-consumption status on Engine assets.
- [ ] Replace Texture2D, TextureCube, StaticMesh, SkeletalMesh,
  AnimationClip, and TerrainHeightmap broad self-building methods with narrow
  Engine publication/exchange primitives and explicit external operations.
  Keep compatibility forwarding only while a named consumer remains.
- [ ] Rebind editor tick, save/cook waits, project switch, provider unload, and
  shutdown to the new coordinator lifecycle and prove no callback crosses
  module or object teardown.
- [ ] Record the Stage 3 handoff with request/status transitions, thread facts,
  stale-generation behavior, shutdown counts, and remaining forwarding APIs.

#### Acceptance Gate

- Runtime Engine has no build queue, worker, compression timing, DDC rebuild,
  or source-inspection lifecycle.
- Background and synchronous builds preserve latest-wins, cancellation,
  bounded admission, main-thread publication, wait, and shutdown semantics.
- Every failure and stale result preserves the prior complete asset and render
  state; valid DDC-write failure behavior remains unchanged.

### Stage 4: Migrate providers, editors, Cook, and DurinAssetTool

Dependencies: Stage 3 explicit build operations and coordinator lifecycle.

- [ ] Make `StandardAssetImport` depend explicitly on `EngineAssetBuild` and
  pass captured/decoded normalized values into builders for single-asset and
  Scene workflows. Preserve provider identities, output fingerprints,
  reconciliation, and candidate publication order.
- [ ] Retain the existing StaticMesh decoder registration seam while legacy
  self-building call paths remain; migrate each caller to explicit
  provider-to-builder calls without changing source formats or normalized
  results.
- [ ] Make TextureEditor, StaticMeshEditor, SkeletalMeshEditor, Terrain/editor
  hosts, and diagnostics panels call/query build capabilities only where they
  directly expose rebuild or live build status. Import-only hosts continue
  through `AssetImportCore` and providers.
- [ ] Make Cook preparation and `DurinAssetTool` explicitly link/invoke
  `EngineAssetBuild` for rebuild, audit, or migration operations. Keep audit
  commands that only inspect packages free to use smaller capability closures
  where supported.
- [ ] Qualify import, reimport, source repair, build-setting edits, save, Cook,
  batch tool operation, cancellation, project switch, and shutdown across all
  asset families.
- [ ] Record the Stage 4 handoff with direct consumers, remaining compatibility
  APIs, editor/tool deployment, import fingerprints, and cooked hashes.

#### Acceptance Gate

- Every authoring consumer reaches builders through an explicit module
  dependency or provider capability; no consumer relies on Runtime Engine to
  discover or manufacture build support.
- Import/reimport output graphs, DDC keys, package bytes, cooked payloads,
  diagnostics, and failure rollback match the frozen baseline.
- `DurinAssetTool` and Cook operate with intentional capability closures, and
  runtime-only inspection paths do not pull providers unnecessarily.

### Stage 5: Remove migration seams and qualify the runtime boundary

Dependencies: Stage 4 complete consumer migration.

- [ ] Remove obsolete Runtime Engine source decoder registration, complete
  imported intermediate exposure, self-building methods, forwarding wrappers,
  editor-only private fields, `DURIN_WITH_EDITOR` builder branches, and offline
  third-party includes after repository-wide consumer proof.
- [ ] Reduce Engine headers to runtime schema, lightweight authored provenance,
  cooked payload access, detached publication seams, runtime status, and render
  resources. Verify exported ABI/API and reflection inputs deliberately.
- [ ] Run focused builder, Engine asset, import, DDC, Cook, editor lifecycle,
  tool, runtime-process, renderer/Vulkan, and game-closure validation. Because
  this refactor crosses module and native-test target boundaries, run the final
  full native-test gate at default target granularity after focused diagnosis.
- [ ] Complete clean full `all` builds for both the selected editor and game
  Agent Build Profiles; inspect runtime deployments and execute source/DDC-free
  cooked game smoke validation.
- [ ] Move lasting ownership, lifecycle, Cook, and runtime-independence rules
  into Workspace, Runtime Assets, Asset Import Framework, Runtime Variants,
  Build System, and Runtime Lifecycle documentation. Remove stale references to
  retired module identities or conditional Engine ownership.
- [ ] Record final module closures, deployed libraries, test counts, golden
  keys/hashes, cooked smoke evidence, executable paths, and any measured
  regressions before completing this plan.

#### Acceptance Gate

- Runtime Engine contains and deploys no source decoder, offline encoder,
  asset-specific builder/DDC writer, build coordinator, or editor rebuild
  diagnostic implementation.
- Editor, Cook, tests, and tools retain complete qualified authoring behavior;
  existing authored packages and cooked payloads require no migration solely
  because of this refactor.
- `DurinGame` loads cooked assets and creates render resources without source,
  DDC, `EngineAssetBuild`, `AssetImportCore`, `StandardAssetImport`, Assimp,
  image decoders, or BC encoders.
- Focused validation, final full native tests, editor/game full builds,
  deployment inspection, cooked runtime smoke, documentation validation, and
  committed handoff all pass.

## Validation Matrix

| Scenario | Required behavior | Evidence owner |
| --- | --- | --- |
| Module selection | Editor/cooker/tool/test roots select `EngineAssetBuild`; game roots do not | DHT module metadata and dependency-closure checks |
| Third-party closure | BC encoders belong only to build; Assimp/image decoders belong only to providers; none enter game | CMake closure and deployment checks |
| Payload compatibility | DMSH, TXPL, skeletal, animation, terrain, and DBLK bytes/read results remain stable | Builder/runtime codec tests and golden hashes |
| DDC compatibility | Keys, namespaces, hit/miss/corrupt handling, atomic writes, and best-effort write failures remain stable | Asset-specific builder and AssetCore tests |
| Texture build | 2D/cube decode handoff, mip generation, projection, formats, compression, alpha policy, cancellation, and metrics are unchanged | `TextureTests` and builder-focused tests |
| Mesh build | StaticMesh normalized input, slots, render data, collision, candidate exchange, source repair, and DDC behavior are unchanged | StaticMesh/import/collision tests |
| Skeletal/animation | Skeleton compatibility, skeletal/clip keys, candidate payloads, relationship validation, and Cook remain stable | Scene import, skeletal, animation, and Cook tests |
| Terrain | Heightmap source handoff, derived payload, revision, Cook/load, and failure preservation remain stable | Terrain heightmap and Cook tests |
| Coordinator | Admission, priorities, latest generation, cancellation, mailbox, waits, metrics, and shutdown are bounded and race-safe | Coordinator and runtime lifecycle tests |
| Import transactions | Snapshot bytes, plans, fingerprints, candidate order, publication rollback, records, and reconciliation are unchanged | AssetImportCore and StandardAssetImport tests |
| Editor integration | Rebuild actions and live diagnostics work without Runtime Engine owning build services | Asset editor and editor workflow tests |
| Tool/Cook | Explicit build operations work headlessly; inspection-only paths keep minimal closures | `DurinAssetTool` process and Cook tests |
| Cooked game | Source/DDC-free packages load, validate, and create render resources with no authoring modules deployed | Game runtime process and renderer/Vulkan smoke |
| Failure transaction | Decode/build/compress/DDC/publication/cancellation/stale failures preserve previous complete state | Failure and lifecycle matrices |

## Definition of Done

- `EngineAssetBuild` is the single selected owner of Engine asset platform
  production, asset-specific DDC builders, rebuild policy, build coordination,
  and live build diagnostics.
- Runtime `Engine` owns only serialized asset schema, cooked/runtime payload
  validation and consumption, detached state publication, runtime CPU data,
  render resources, and consumption status.
- `AssetCore`, `AssetImportCore`, and `StandardAssetImport` retain their generic
  storage, framework, and provider responsibilities without dependency cycles
  or duplicated facilities.
- Offline third parties and authoring modules are mechanically excluded from
  `DurinGame`; cooked runtime never consults source or DDC and fails explicitly
  on missing/incompatible required payloads.
- Existing source formats, normalized values, authored packages, DDC keys,
  cooked payload formats, import outputs, editor workflows, and rollback
  behavior remain compatible.
- Focused tests, required full native validation, editor/game full builds,
  deployment inspection, cooked runtime smoke, plan validation, lasting
  documentation, and committed handoff are complete.

## Deferred Follow-ups

- Split texture, geometry, skeletal, animation, or terrain builders into
  independent modules only after a measured optional-dependency, deployment,
  unload-lifecycle, or release-cadence requirement emerges.
- Remote/shared DDC services, distributed builds, build farms, shader-style
  worker processes, and cross-machine coordinator protocols.
- Runtime asset generation, mod source ingestion, runtime texture compression,
  or development-only hot-build deployment.
- New payload schemas, new compression formats, platform variants, virtual
  textures, mesh optimization pipelines, Nanite-like geometry, or incremental
  terrain-region building.
- Generalizing EngineAssetBuild into a provider-agnostic workspace-wide build
  graph before a second non-Engine consumer proves the abstraction.

## Related Documentation

- [Asset Data Lifecycle and Storage](../Runtime/Assets/AssetDataLifecycle.md)
- [Asset Import Framework](../Editor/Architecture/AssetImportFramework.md)
- [Asset Packages](../Runtime/Assets/AssetPackages.md)
- [Runtime Lifecycle](../Runtime/Core/RuntimeLifecycle.md)
- [Runtime Variants](../Development/Build/RuntimeVariants.md)
- [Build System](../Development/Build/BuildSystem.md)
- [Native Tests](../Development/Build/NativeTests.md)
- [Code Modules](../Workspace/CodeModules.md)

## Related Code

- `CMakeLists.txt`
- `Engine/Engine.dproject`
- `Engine/Source/Runtime/AssetCore/AssetCore.dmodule`
- `Engine/Source/Runtime/Engine/Engine.dmodule`
- `Engine/Source/Runtime/Engine/CMakeLists.txt`
- `Engine/Source/Runtime/Engine/Public/StaticMesh/StaticMesh.h`
- `Engine/Source/Runtime/Engine/Private/StaticMesh/StaticMesh.cpp`
- `Engine/Source/Runtime/Engine/Public/StaticMesh/StaticMeshDerivedData.h`
- `Engine/Source/Runtime/Engine/Private/StaticMesh/StaticMeshDerivedData.cpp`
- `Engine/Source/Runtime/Engine/Public/Texture/Texture2D.h`
- `Engine/Source/Runtime/Engine/Private/Texture/Texture2D.cpp`
- `Engine/Source/Runtime/Engine/Public/Texture/TextureCube.h`
- `Engine/Source/Runtime/Engine/Private/Texture/TextureCube.cpp`
- `Engine/Source/Runtime/Engine/Public/Texture/TextureBuild.h`
- `Engine/Source/Runtime/Engine/Private/Texture/TextureBuild.cpp`
- `Engine/Source/Runtime/Engine/Public/Texture/Texture2DBuildCoordinator.h`
- `Engine/Source/Runtime/Engine/Private/Texture/Texture2DBuildCoordinator.cpp`
- `Engine/Source/Runtime/Engine/Public/Texture/TextureDerivedData.h`
- `Engine/Source/Runtime/Engine/Private/Texture/TextureDerivedData.cpp`
- `Engine/Source/Runtime/Engine/Public/Texture/EquirectangularTextureCube.h`
- `Engine/Source/Runtime/Engine/Private/Texture/EquirectangularTextureCube.cpp`
- `Engine/Source/Runtime/Engine/Public/SkeletalMesh/SkeletalDerivedData.h`
- `Engine/Source/Runtime/Engine/Private/SkeletalMesh/SkeletalDerivedData.cpp`
- `Engine/Source/Runtime/Engine/Public/Terrain/TerrainHeightmapDerivedData.h`
- `Engine/Source/Runtime/Engine/Private/Terrain/TerrainHeightmapDerivedData.cpp`
- `Engine/Source/Editor/AssetImportCore/AssetImportCore.dmodule`
- `Engine/Source/Editor/StandardAssetImport/StandardAssetImport.dmodule`
- `Engine/Source/Editor/StandardAssetImport/Private/StandardAssetImportProviders.cpp`
- `Engine/Source/Editor/StandardAssetImport/Private/SceneImport.cpp`
- `Engine/Source/Programs/DurinAssetTool/CMakeLists.txt`
- `Engine/Source/Programs/DurinAssetTool/Private/AssetToolMain.cpp`
- `Engine/Tests/Native/EngineTests/CMakeLists.txt`
- `Engine/Tests/Native/AssetCoreTests/CMakeLists.txt`
