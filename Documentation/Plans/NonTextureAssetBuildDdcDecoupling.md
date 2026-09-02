# Non-Texture Asset Build DDC Decoupling Plan

Summary: Make non-Texture build modules pure typed recipe providers, move DDC orchestration into Engine, simplify asset state, and remove the unused Build Framework.

Last reviewed: 2026-09-03

Status: Active
Completed:

## Current Status

Implementation is complete through the family cutovers and Build Framework
removal. Stage 0, Stage 2, Stage 4, and Stage 5 passed their local gates.
Stage 3 code and non-GPU gates passed; its real Vulkan gate remains open.
Stage 1 remains active for Windows Game binary qualification. Stage 6 also
retains real skeletal Vulkan qualification; macOS project-loading smoke passes.

- Engine owns all non-Texture asset keys, metadata-first lookup, runtime codecs,
  cache validation/fallback, bounded operation diagnostics, and typed application.
  StaticMeshBuild, SkeletalBuild, and TerrainBuild are pure recipe providers.
  StaticMesh recipes receive material-slot metadata without object references;
  Engine restores live material bindings after reconciliation.
- All ten asset Build Functions, their registries/local envelopes, and the
  generic Build Framework's four headers, implementation, and tests are removed.
  Direct DDC Get/Put and Shader clients remain.
- Asset objects retain authored inputs, installed/cooked values, and genuine
  resource/readiness/revision state, not DDC key/origin or narrative history.
  Heightmap loading is synchronous; stale documentation describing an async
  group was corrected rather than introducing one.
- Skeletal key schema 4 removes object paths. Reverse-wire tests reproduce
  both schema-3 goldens; payload bytes are unchanged. StaticMesh, Heightmap, and
  all five World key/payload goldens remain unchanged. World's raw schema-1
  cache bodies retain exact recipe comparison on warm requests; each product
  is still independently classified, recovered, and persisted.
- Focused evidence: StaticMesh 75 cases, skeletal 35, Heightmap 13 (including
  replacement rollback), Terrain World 16, and lower-level DDC 8 pass.
  Stage 4 affected coverage passed 39 targets; Stage 5 passed 35. Final affected
  coverage passed all 37 targets, including Shader cache/service and Cook/
  source-free runtime contracts
  (`Build/.agent-state/logs/20260903-033436-015018-55758-ctest.log`).
  Final Editor `all` build passed
  (`Build/.agent-state/logs/20260903-033625-570226-56555-cmake.log`).
- Current source/test search has no Build Framework symbols or includes.
  Active StaticMeshBuild, SkeletalBuild, and TerrainBuild binaries have no DDC
  dependency or Build Framework symbols; DerivedDataCache has no framework
  symbols. Eighteen related Engine translation units pass non-Editor syntax
  and object compilation without the DDC include path or a compiled Editor
  PCH; their undefined symbols contain no Cache API or recipe-provider imports.
- All 135 current documents, all plans (5 active, 324 archived), and all
  roadmaps (2 active, 25 archived) validate. Historical archived documents are
  not rewritten as current architecture claims.

macOS smoke follow-up (explicitly requested by the user):

- Project Browser passed hidden-window startup, first presentation, 60 ticks,
  and normal rendering/GPU shutdown with exit code 0 using the existing Editor
  executable (`Build/.agent-state/logs/20260903-034719-104679-56912-DurinEditor.log`).
- Sandbox initialized macOS services and Vulkan on Apple M4 and loaded all
  three recipe modules, but failed before entering the tick loop: the default
  Level contained serialized `DSkyBoxComponent::SkyBoxSceneId`, incompatible
  with the live schema. It exited with code 1
  (`Build/.agent-state/logs/20260903-034658-009767-56894-DurinEditor.log`).
  Existing assets were not modified. The field was removed by `703928944`,
  but `Sandbox/Content/Levels/GrayboxStage15.dasset` still contains it; the
  loader at that checkpoint rejected fields without a current compatible
  property or deprecated route. This was not a passed project-loading smoke.

User-selected smoke repair (2026-09-03): automatically discard removed authored
fields on known declaring types, including nested values, without loading
dependencies referenced only by those fields. Preserve explicit deprecated
routes and strict current-field type, unknown-class, wire-corruption, and
cooked-native checks. Reading does not rewrite assets; explicit save removes
discarded fields. The repair is implemented: six added package regressions and
the updated legacy-material contract pass in all 34 affected native targets
(`Build/.agent-state/logs/20260903-040121-953201-58616-ctest.log`). The complete
Editor build passed
(`Build/.agent-state/logs/20260903-040134-977036-59243-cmake.log`).

- Sandbox now loads its default Level and completes Editor initialization;
  the removed-field failure is resolved without rewriting the tracked asset.
  The subsequent viewport texture creation triggers the RHI open-buffer-lock
  assertion, exiting with signal status -5 before 60 ticks
  (`Build/.agent-state/logs/20260903-040207-855102-59316-DurinEditor.log`).
  The crash stack is `FSceneViewport::UpdateRHIViewport` -> `RHICreateTexture`
  -> `ExecuteFallibleSynchronousOperation`. This was a distinct rendering
  failure at that checkpoint, repaired below.
- Project Browser was rerun with the updated executable and again completed
  60 ticks and normal shutdown, exit code 0
  (`Build/.agent-state/logs/20260903-040248-024689-59364-DurinEditor.log`).

User-selected viewport repair (2026-09-03): the UI thread's direct offscreen
texture creation raced the previous frame's render-thread ImGui buffer locks
on the immediate command list. Creation and the initial transition now execute
together on the render thread; allocation/resize waits for a render-thread
fence before publishing the same-frame texture, without a GPU-idle flush.
The 106 viewport tests pass, including creation/resize ordering, unchanged-size
reuse, failed-creation retry, and render-thread callers. All 37 affected native
targets pass (`Build/.agent-state/logs/20260903-041222-676682-60006-ctest.log`).
Full Editor `all` build
passes (`Build/.agent-state/logs/20260903-041118-327270-59882-cmake.log`).
Sandbox passes two consecutive hidden-window 60-tick runs on Apple M4, including
default Level load, first presentation, rendering, and normal render/RHI/GPU
shutdown (exit code 0), without modifying the tracked asset:

- `Build/.agent-state/logs/20260903-041128-984397-59962-DurinEditor.log`
- `Build/.agent-state/logs/20260903-041147-919180-59973-DurinEditor.log`

Pending acceptance:

- Build and inspect the existing `Win64-Debug-DurinGame` closure/deployment,
  proving no asset DDC or provider dependency in Game.
- Run `SkeletalMeshRenderResourcesVulkanTests` qualification.
  Local GPU qualification compiled but failed at Vulkan initialization because
  Metal was unavailable and instance extension dependencies were reported
  (`Build/.agent-state/logs/20260903-031815-880363-52007-ctest.log`).
- The session exposes only the local macOS host and this checkout has no CI
  workflow entry point. Windows access details were requested and are still
  needed. Do not mark the plan complete or substitute local syntax checks for
  those acceptance gates.

## Goal

Apply the proven Texture ownership split to every remaining asset build family:

```text
Engine-owned identity -> DDC Get -> typed payload decode/validate
    -> pure family provider call on miss -> typed payload encode -> DDC Put
    -> Engine-owned result application or Terrain generation commit
```

At completion:

- StaticMeshBuild, SkeletalBuild, and TerrainBuild have no DerivedDataCache or
  Build Framework dependency, cache policy, key persistence, cache-origin, or
  object result-application responsibility;
- Engine owns editor-only DDC orchestration for StaticMesh render/collision,
  SkeletalMesh, AnimationClip, TerrainHeightmap, and Terrain World products;
- each Developer build module exposes only copied/owned normalized requests,
  detached typed outputs, immutable recipe descriptors, and bounded provider
  registration;
- StaticMesh, SkeletalMesh, AnimationClip, and TerrainHeightmap objects no
  longer retain recomputable DDC keys, cache-origin flags, persistence
  messages, or narrative build diagnostics; synchronous operations return
  their result/error directly and any retained asynchronous history belongs to
  a bounded Engine manager;
- authored editor PostLoad, import/reimport, property-driven rebuild, Scene
  import, collision rebuild, Terrain generation, and Cook preparation use the
  corresponding Engine-owned orchestration path;
- Game runtime consumes required cooked payloads without DerivedDataCache or
  Developer build modules in its compile, link, load, or deployment closure;
- DerivedDataCache retains only its backend-neutral synchronous Get/Put facade
  after the now-unused family-neutral Build Framework is removed.

## Scope

- Freeze existing keys, bucket names, maximum-value policies, payload bytes,
  hit/miss/failure behavior, and Cook/runtime closure for all ten functions.
- Introduce or refine Engine-owned typed provider contracts for StaticMesh,
  SkeletalMesh, AnimationClip, TerrainHeightmap, and Terrain World recipes.
- Classify non-Texture object fields as authored input, installed/cooked
  payload, genuine render/physics/generation lifetime state, or operation
  metadata; remove or relocate the last category unless Stage 0 proves a
  correctness dependency.
- Move family key construction, DDC Get/Put, payload serialization and
  validation, cache fallback, origin classification, diagnostics, and result
  application into Engine editor-only code.
- Reduce the three Developer build modules to deterministic object-free recipe
  implementations and module-owned provider registrations.
- Preserve StaticMesh material-slot reconciliation and collision policy;
  skeletal compatibility and imported-payload behavior; TerrainHeightmap
  canonical samples and lazy authored bulk; and Terrain World independent
  product caching plus atomic complete-generation visibility.
- Remove all asset Build Functions, local input envelopes, private cache
  codecs, registration transactions, and the DerivedDataCache Build Framework
  once no client remains.
- Update lasting asset lifecycle, Terrain World, module ownership, build, and
  family-specific documentation after implementation qualifies.

## Non-Goals

- Changing mesh optimization, normal/tangent generation, collision cooking,
  skeletal validation, animation sampling, height filtering, Terrain
  composition, or Terrain World product algorithms.
- Changing source import formats, authoring schemas, visible editor workflows,
  target platforms/profiles, package formats, Cook reachability, or render and
  physics readiness semantics.
- Moving ShaderBuild DDC orchestration into Engine or removing its direct Cache
  API use.
- Adding remote DDC, async cache backends, request coalescing, dependency
  graphs, generic worker scheduling, process-wide eviction, or cache quotas.
- Creating a public typeless asset-build coordinator or preserving the Build
  Framework under a new name.
- Persisting derived keys or DDC payload bytes in authored packages.
- Removing render revisions, render/physics readiness, lazy cooked-payload
  state, or Terrain complete-generation identity when those values enforce a
  real runtime lifecycle invariant.
- Renaming genuine render-resource state publication or Terrain's atomic
  complete-generation visibility merely to eliminate the word `Publish`.

## Selected Responsibility Split

| Layer | Owns | Must not own |
| --- | --- | --- |
| Engine | Runtime payload schemas, family keys and cache policy, editor-only Get/Put, typed payload validation, fallback, operation/manager diagnostics, object scheduling, Cook serialization, result application, Terrain complete-generation visibility | Asset recipe algorithms, source decoding, DDC backend mechanics |
| Runtime asset objects | Authored inputs and relationships, installed/cooked payloads, and genuine render, physics, or generation lifetime state | Recomputable DDC keys, cache origin, persistence messages, narrative operation history |
| StaticMeshBuild | Pure render-data and collision recipes, reconciliation algorithm and versions, typed provider registration | DDC, Build Functions, cache keys/bytes/origin, live object mutation |
| SkeletalBuild | Pure SkeletalMesh and AnimationClip recipes, compatibility validation and versions, typed provider registration | DDC, Build Functions, cache metadata, live object mutation |
| TerrainBuild | Pure Heightmap and Terrain World normalization/product recipes, product codecs and versions where the runtime value contract requires them, typed provider registration | DDC policy, Build Functions, cache origin, object application, generation admission |
| DerivedDataCache | Opaque synchronous bucket/key Get/Put, immutable returned bytes, backend concurrency and atomic replacement | Build recipes, typed payloads, family validation, Build Function registry/session |
| AssetForgeBuiltins | Physical source capture/translation, canonical imported values, editor transaction and save sequencing | DDC access, recipe implementation, final Engine result application |
| Cooked runtime | Required typed payload and Terrain region consumption from cooked packages | DDC, Developer build modules, authored rebuild inputs |

## Design Decisions and Invariants

### Engine owns persistence because Engine owns runtime consumption

The module that interprets and consumes a derived runtime value owns its DDC
identity, validation, fallback, and Cook transition. Developer build modules
receive normalized values and return detached values; they do not decide
whether a request queries or persists DDC and do not report a cache hit.

StaticMesh, SkeletalMesh, AnimationClip, and TerrainHeightmap use their Engine
payload `Serialize` protocols for both DDC and Cook with the appropriate
archive purpose and explicit target context. A cache hit requires complete
archive consumption and typed validity. Corrupt, truncated, trailing,
oversized, incompatible, or failed reads fall back to a local recipe when
canonical authored input is available. Put failure remains best effort and
does not invalidate an already built in-memory result.

### Provider requests and products are cache-free

Each provider descriptor supplies stable producer identity and every algorithm
version required by Engine's key. Requests own or immutably borrow normalized
source values, settings, target, relationship facts, and cancellation
observation. Products contain only detached typed outputs and recipe metrics.

`bQueryDerivedData`, `bPersistDerivedData`, `DerivedDataKey`, cache origin,
store diagnostics, and Build Framework values are absent from provider APIs.
Engine combines provider output with cache metadata in a family-specific
internal result and applies it directly. Standalone `Publish*Product` APIs that
only wrap object assignment are removed; genuine atomic Terrain generation
commit and render-resource state publication remain distinct lifecycle
concepts.

### Diagnostics describe operations, not assets

The Texture state simplification is the object-shape precedent as well as the
DDC-ownership precedent. A successful build may return its key, cache origin,
timings, persistence warning, and bounded diagnostics in an Engine-owned
operation result, but applying the detached payload does not copy that history
onto the asset. Synchronous callers consume the result or `OutError` directly;
an asynchronous domain may retain bounded request history in its owning Engine
manager.

Stage 0 classifies the existing `DerivedDataKey`,
`bLoadedFromDerivedDataCache`, diagnostic/status, revision, and publication-
shaped fields on StaticMesh, SkeletalMesh, AnimationClip, and
TerrainHeightmap. Recomputable provenance and narrative history are removed.
Fields that gate stale-result rejection, lazy cooked materialization, render or
physics readiness, or Terrain's atomic complete-generation visibility remain
with the lifecycle owner that enforces the invariant. The migration must not
replace family-specific status fields with a generic asset-state container.

### Compatibility precedes ownership movement

Existing cache bucket strings, canonical key bytes/strings, function recipe
versions, payload bytes/hashes, maximum value sizes, and Cook bytes remain
compatible unless Stage 0 identifies a concrete correctness defect. Any
intentional break is versioned and recorded before code moves.

Key inputs exclude object paths except where the current output contract proves
they affect the value. In particular, Stage 0 must decide whether skeletal
`StableOutputIdentity` is accidental object identity: neither skeletal payload
contains it and relocation currently changes otherwise reusable cache keys.
Stage 3 removes it from both key inputs and increments the skeletal key schema
version. Existing disposable entries become misses by an explicit versioned
compatibility decision rather than a silent key change.

### Terrain World keeps independent cache values and atomic generations

The five Terrain World product classes remain independently keyed, queried,
validated, classified, and stored. Generation IDs and package placement remain
outside product keys.

Implementation inspection in Stage 1 found that the existing DDC format is a
raw product body, not the checksummed TWPD runtime envelope. Its validation
compares the cached body byte-for-byte with the locally computed recipe body;
the old warm path already computes those bodies before querying the cache.
This migration preserves that exact-input corruption check and existing cache
bytes. Terrain World therefore retains one pure recipe pass for warm
validation; it must not claim recipe-free warm reuse. A cache envelope that
permits recipe-free validation requires an explicit versioned format change
and is deferred. StaticMesh, skeletal, and Heightmap warm hits continue to skip
their recipe calls.

Engine owns cache orchestration and accepts a complete typed candidate only
after all required products, dependencies, neighbor evidence, and generation
identity validate. The prior complete generation remains visible on partial
miss, build failure, cancellation, supersession, corrupt cache data, or failed
application. Stage 0 freezes whether the existing public Terrain World value
contracts move to Engine or are split into Engine-owned runtime contracts and
TerrainBuild-owned recipe contracts; Engine must not gain a static dependency
on a Developer module in Game variants.

### The Build Framework is removed only at zero clients

Family migrations are incremental. Existing Build Functions remain operational
for families not yet cut over, and each family removes its functions and
registration only after its Engine path is qualified. After Terrain World
cutover, repository search must prove that `FBuildSession`, definitions,
values, function registration, and associated tests have no production client;
then the entire Build Framework API and implementation are deleted from
DerivedDataCache without a compatibility alias.

### Generalization waits for repeated evidence

Engine may share private byte-oriented helpers for bounded Get/Put timing and
diagnostics, but family key construction, typed decode, validation, fallback,
and application remain explicit. A broader editor derived-data orchestrator is
considered only after StaticMesh and Skeletal migrations demonstrate identical
end-to-end lifecycle needs; this plan does not require one.

## Implementation Stages

### Stage 0: Freeze all compatibility and ownership baselines

Dependencies: none.

- [x] Inventory the ten remaining asset Build Functions, registrations,
  buckets, keys, local input envelopes, codecs, policies, production callers,
  direct tests, and module closure edges.
- [x] Record golden key bytes/strings and representative payload bytes, hashes,
  and sizes for StaticMesh render/collision, SkeletalMesh, AnimationClip,
  TerrainHeightmap, and all five Terrain World product classes.
- [x] Characterize cold build, warm hit, corrupt/truncated/trailing hit, read
  failure, Put failure, query/store disabled, cancellation, supersession,
  provider unavailable/ambiguous, unload/reload, and Cook behavior wherever
  each family supports them.
- [x] Map every authored PostLoad, import/reimport, Scene import, collision
  rebuild, Terrain generation, Cook, result-application, render/physics
  readiness, and runtime-load entry point to one selected Engine owner.
- [x] Classify every non-Texture asset field and public status/publication API;
  record which values are authoritative asset/resource state and which are
  removable DDC provenance, request bookkeeping, or narrative diagnostics.
- [x] Freeze typed provider descriptor fields and decide the Terrain World
  runtime/recipe contract split without introducing an Engine-to-Developer
  static dependency.
- [x] Decide and document skeletal `StableOutputIdentity` compatibility and any
  other current key field whose semantic ownership is unclear.

#### Acceptance Gate

- Every current byte, key, status, failure, lifecycle, and module-closure
  behavior has executable evidence; each derived output dimension has one
  selected owner; and no Terrain World or skeletal identity decision remains
  unresolved.

Stage 0 evidence (2026-09-03):

- The inventory contains ten functions, not the nine stated by the initial
  plan: StaticMesh render/collision, SkeletalMesh, AnimationClip,
  TerrainHeightmap, and Terrain World Metadata/Height/Coverage/Collision/Query.
  Their frozen buckets are respectively `StaticMesh/Objects`,
  `StaticMeshCollision/Objects`, `SkeletalMesh/Objects`,
  `AnimationClip/Objects`, `TerrainHeightmap/Objects`, and
  `TerrainWorld/<value>/Objects`; registration remains one two-function Static
  Mesh transaction, one two-function skeletal transaction, and one six-
  function Terrain transaction until the owning family stage cuts over.
- Golden key bytes and exact key strings are covered by
  `StaticMeshDerivedDataContractTests`, `SkeletalAssetTests`, and
  `TerrainHeightmapTests`. `TerrainWorldBuildTests` freezes all five exact keys,
  payload hashes, and payload sizes from one asymmetric fixture. Existing
  payload tests freeze StaticMesh, skeletal, heightmap, and Terrain World
  serialized values and their corruption/trailing/limit rejection.
- Existing cold/warm/corrupt/store-failure/source-unavailable/PostLoad/Cook
  coverage maps the current behavior. Provider zero/multiple-registration
  rejection is frozen by the common single-feature invocation contract and is
  re-qualified with each new provider interface in Stage 1. Terrain World alone
  supports cancellation/supersession and atomic prior-generation retention.
- Engine owns the selected entry points: family-specific editor orchestration
  serves authored PostLoad, import/reimport, Scene import, property/collision
  rebuild, and Cook preparation; existing cooked mesh and Terrain consumers
  remain the runtime path and never call an editor provider or DDC.
- StaticMesh retains authored imported geometry/material/collision policy,
  installed render/collision values, cooked bulk, and real render/cooked-load
  lifetime state. SkeletalMesh and AnimationClip retain authored source,
  skeleton compatibility and summaries plus installed/cooked payload state.
  TerrainHeightmap retains canonical samples, installed/cooked payload, and
  only actual component readiness and payload revisions; the current synchronous
  load has no asynchronous generation owner or request-group state to migrate. DDC key/origin, persistence messages, source-
  invocation facts, and narrative diagnostics are operation state and leave
  all four asset types. Terrain World accepted products likewise lose key and
  origin; those remain on the Engine operation result.
- Terrain World runtime enums, product/manifest/region schemas, decoding, and
  complete-generation publication move to Engine. TerrainBuild owns normalized
  recipe requests and pure composition/product algorithms and may depend on
  Engine's value contract; Engine and Game never depend on TerrainBuild.
- `StableOutputIdentity` is removed with a skeletal key-schema increment in
  Stage 3 because the payload is path-independent. This is the sole selected
  intentional key break; all other frozen keys, payload bytes, buckets, value
  names, size ceilings, and failure policies remain compatible.

### Stage 1: Establish Engine provider and cache primitives

Dependencies: Stage 0 complete.

- [x] Add family-specific Engine provider interfaces and immutable descriptor
  snapshots for the remaining recipe families, using module callback gates to
  bound calls during provider retirement.
- [x] Add Engine-private typed key and DDC helpers guarded by
  `DURIN_WITH_EDITOR`; reuse the optional DerivedDataCache dependency already
  established by Texture without exposing DDC types publicly.
- [x] Route DDC and Cook payload encoding through the owning runtime value
  serialization protocol; require archive end, target compatibility, size
  bounds, checksums, dependency validation, and typed validity on decode.
- [x] Define family-specific Engine results carrying key, `CacheHit` or
  `Rebuilt` origin, bounded diagnostics, phase timings, descriptor, metrics,
  and detached payloads; keep these results internal to orchestration and
  application.
- [x] Establish validated typed payload setters and explicit resource/collision
  update transitions where current `Publish*Product` helpers conflate value
  installation, operation history, dirtying, notification, or resource work.
- [x] Add direct golden compatibility and corrupt-value fallback tests before
  switching any production caller.
- [ ] Prove the Game Engine compilation closure preprocesses all new code with
  `DURIN_WITH_EDITOR=0` and imports no DDC or provider symbol.

#### Acceptance Gate

- Engine can key, round-trip, reject, and classify representative values for
  every family without calling a Build Function, while Game builds have no new
  Developer or DDC closure edge.

Stage 1 partial evidence (2026-09-03):

- `./DevTool test affected` passed all 36 selected native-test targets;
  the receipt is `Build/.agent-state/logs/20260903-023520-659552-44460-ctest.log`.
- Direct `EngineProviderPath*` tests pass for StaticMesh, SkeletalAsset,
  TerrainHeightmap, and TerrainWorldBuild targets. Existing StaticMesh key
  goldens, skeletal payload/key goldens, Heightmap goldens, and the complete
  15-case Terrain World baseline passed during extraction.
- StaticMesh tests compare both render and collision keys with the legacy
  path and verify that initial material-slot reconciliation persists under the
  reconciled key rather than the pre-build empty-slot key.
- Terrain World runtime validation, TWPD encoding/decoding, and generation
  publication moved without changing the five golden bodies. A structurally
  valid changed height sample is still rejected by exact-input cache
  validation; the other four products retain warm-origin classification.
- At that extraction checkpoint, transitional interfaces and legacy
  publication contracts still remained; those receipts did not qualify the
  final interfaces or Game binary closure. Stage 2 through Stage 6 subsequently
  removed the legacy callers and publication adapters, privatized keys, and
  separated Engine operation results from pure provider and runtime value
  contracts. Operation results are not retained on assets.

Sequencing clarification: family cutovers proceeded after their direct provider
paths and compatibility tests passed, allowing interface cleanup alongside
removal of the legacy callers. That cleanup is now complete. Stage 1 remains
open only for the user-selected Windows Game binary closure gate; this host
has no registered Game preset.

### Stage 2: Migrate StaticMesh render and collision

Dependencies: Stage 1 direct provider paths and compatibility tests qualified;
its final interface cleanup and Game closure remain explicit joint gates.

- [x] Split material-slot reconciliation, render-data construction, and
  collision construction into pure typed provider calls with no live
  `DStaticMesh` or `DBodySetup` access.
- [x] Move render and collision keys, Get/decode/build/encode/Put fallback,
  diagnostics, and result application to Engine.
- [x] Route uncooked PostLoad, standalone import/reimport, Scene import,
  collision policy changes, duplication, and Cook through the selected Engine
  paths while preserving rollback and resource invalidation ordering.
- [x] Replace `BuildAndPublishImported`, `TryLoadImportedProduct`,
  `PublishImportedProduct`, and cache-bearing products with explicit Engine
  orchestration and direct result application.
- [x] Remove StaticMesh object copies of DDC key/origin and narrative build
  diagnostics; derive CPU render/collision readiness and resource readiness
  from their actual owners while preserving revision-based stale-work guards.
- [x] Remove the two StaticMesh Build Functions, local envelopes/codecs,
  registry state, and DerivedDataCache dependency after cutover.

#### Acceptance Gate

- StaticMesh render/collision keys and payloads remain compatible; warm hits
  avoid recipe work; all import/PostLoad/Cook/render/physics cases pass; and
  StaticMeshBuild contains no DDC, Build Framework, cache, or live-object
  vocabulary.

### Stage 3: Migrate SkeletalMesh and AnimationClip

Dependencies: Stage 2 complete.

- [x] Reduce SkeletalBuild to pure typed SkeletalMesh and AnimationClip
  providers with copied relationship facts, detached payloads, cancellation,
  recipe identity, and no object or cache fields.
- [x] Move both key schemas and Get/decode/build/encode/Put fallback into Engine
  while preserving skeleton compatibility, bind transforms, clip identity,
  material-slot validation, and imported-data fingerprint behavior.
- [x] Route uncooked PostLoad, direct import/reimport, Scene import, duplicate,
  skeleton relationship changes, Cook preparation, and result application
  through one Engine path per family.
- [x] Remove `Rebuild*FromImportedData`, cache-bearing build products, and
  `PublishBuiltProduct` application wrappers in favor of Engine-owned results
  and direct application.
- [x] Remove SkeletalMesh and AnimationClip object copies of DDC key/origin and
  payload-storage diagnostics; keep authored compatibility identity and actual
  payload/render readiness in their existing semantic owners.
- [x] Remove the two skeletal Build Functions, local envelopes/codecs, registry
  state, and DerivedDataCache dependency after cutover.

#### Acceptance Gate

- SkeletalMesh and AnimationClip cold/warm/corrupt/failure, relationship,
  Scene import, PostLoad, Cook, animation, and render-resource tests pass with
  selected key compatibility; SkeletalBuild is a pure provider module with no
  DDC or live-object boundary.

### Stage 4: Migrate TerrainHeightmap

Dependencies: Stage 3 code cutover and local non-GPU tests complete; its
Windows Vulkan qualification remains an explicit final gate.

- [x] Reduce TerrainHeightmapBuild to a pure canonical-sample-to-payload
  provider and remove query/store policy, key, persistence diagnostic, source
  provenance, and live `DTerrainHeightmap` application from its contract.
- [x] Move key construction, metadata-only lookup, lazy authored-source
  fallback, typed payload serialization, DDC Get/Put, diagnostics, and result
  application into Engine.
- [x] Route PostLoad, PNG/raw import and reimport, property mutation, Cook, and
  source-unavailable warm-hit behavior through the Engine path without forcing
  authored bulk reads on a valid hit.
- [x] Remove `BuildTerrainHeightmapInto`, `PublishTerrainHeightmapProduct`,
  `LoadTerrainHeightmapDerivedData`, its Build Function, local envelope/codec,
  and obsolete cache-bearing public values.
- [x] Move heightmap DDC key/origin and narrative diagnostics out of the asset;
  retain status/generation fields only where Stage 0 proves they are required
  for asynchronous stale-result rejection or component readiness, and place
  any bounded operation history with the Engine orchestration owner.

#### Acceptance Gate

- TerrainHeightmap key/payload compatibility, cold/warm/corrupt/failure,
  lazy-source, import/PostLoad/Cook, revision, and runtime-load tests pass; its
  recipe path has no cache or object mutation responsibility.

### Stage 5: Migrate Terrain World products and generation orchestration

Dependencies: Stage 4 complete.

- [x] Apply the Stage 0 Terrain runtime/recipe contract split so Game-visible
  types and codecs are Engine-owned while TerrainBuild implements only pure
  normalization, composition, and five-class product recipes.
- [x] Move each product key and independent Get/decode/build/encode/Put path to
  Engine, preserving domain-specific key inputs, dependency hashes, bucket
  layout, schema ceilings, and partial warm-hit reuse.
- [x] Keep generation ID outside product identity and perform complete-set
  dependency, neighbor, border, generation, cancellation, and supersession
  checks before atomic Engine generation commit.
- [x] Route AssetForgeBuiltins Terrain World adaptation and Cook production
  through Engine orchestration; prove cooked region/runtime loading needs no
  DDC or TerrainBuild module.
- [x] Remove the five Terrain World Build Functions, cache policy from
  normalized input, cache metadata from products, registration state, and the
  final TerrainBuild DerivedDataCache dependency.

#### Acceptance Gate

- All five product classes preserve golden keys/payloads and independent cache
  classification with the schema-1 exact-input validation exception; partial
  failure retains the previous complete generation; Cooked
  Terrain World loads without source/DDC/Developer modules; and TerrainBuild is
  a pure typed recipe provider.

### Stage 6: Remove the Build Framework and qualify all closures

Dependencies: Stage 5 complete.

- [x] Prove no production or test client remains for Build definitions,
  values, policies, sessions, cancellation adapters, function names,
  registration, or Build Function callback gates.
- [x] Delete the DerivedDataCache Build Framework headers, implementation,
  tests, and current documentation claims; retain the low-level Get/Put facade.
- [x] Audit provider modules, public contracts, runtime object fields, and
  current Editor binaries for DDC/framework coupling, cache history, and live
  object application. Validate non-Editor compilation without DDC headers.
- [x] Run focused family/DDC tests, final affected tests, default Editor build,
  and native Cook/source-free runtime contracts.
- [x] Pass macOS Project Browser startup/shutdown smoke (60 ticks, exit code 0).
- [x] Implement the user-selected removed-authored-field discard policy and
  qualify package/type/deprecation/cooked boundaries plus affected tests.
- [x] Repair viewport creation ordering and pass two consecutive macOS Sandbox
  project-loading startup/shutdown smokes (60 ticks each, exit code 0).
- [ ] Complete the user-selected Windows Game build/deployment closure,
  and real skeletal Vulkan qualification.
- [x] Update lasting architecture/module/family documentation and validate
  changed/all documentation, all plans, and all roadmaps.

#### Acceptance Gate

- All three Developer build modules are pure providers; Engine is the only
  asset DDC orchestrator in Editor and none exists in Game; DerivedDataCache has
  no Build Framework; focused/affected tests, Editor/Game builds, smokes, Cook,
  closure audits, and documentation validators pass.

## Validation Matrix

| Concern | Required evidence |
| --- | --- |
| Key compatibility | Golden key bytes and strings for all ten former Build Functions are unchanged except for the versioned removal of skeletal `StableOutputIdentity` |
| Payload compatibility | DDC and Cook bytes/hashes round-trip through the owning typed serialization; corrupt, trailing, oversized, and incompatible values fail at the Engine boundary |
| Pure providers | Requests contain no query/store policy; products contain no key/origin/cache diagnostic; providers touch no live object or DDC API |
| Cache behavior | Warm hits skip recipe work except Terrain World's preserved schema-1 exact-input validation pass; misses/read failures/corruption rebuild; Put failure is best effort; disabled persistence performs no Put |
| Object shape | Runtime assets retain authored inputs, installed/cooked payloads, and genuine resource/generation state; no recomputable DDC key, cache-origin flag, persistence message, or narrative operation history remains |
| Diagnostics | Synchronous results/errors remain caller-owned; any asynchronous history is bounded and manager-owned; readiness is queried from payload, resource, physics, or generation owners rather than inferred from DDC provenance |
| StaticMesh | Reconciliation, render/collision policy, import/Scene/PostLoad/Cook, rollback, render readiness, and physics readiness remain qualified |
| Skeletal | Skeleton compatibility, mesh/clip identity, Scene/PostLoad/Cook, animation playback, and render readiness remain qualified |
| TerrainHeightmap | Canonical PNG/raw equivalence, lazy authored bulk, warm hit, revision, PostLoad/import/Cook, and runtime load remain qualified |
| Terrain World | Five independent products, domain-specific keys, borders/dependencies, cancellation/supersession, atomic generation commit, manifests, Cook, and source/DDC-free runtime load remain qualified |
| Lifecycle | Provider registration rollback and retirement drain admitted calls; Engine result application preserves existing thread and rollback ordering |
| Module closure | Editor Engine optionally links DDC and dynamically invokes providers; Game and cooked deployment exclude DDC and all three Developer build modules |
| DDC surface | Direct Get/Put tests remain; no Build Framework type, registration, source, binary symbol, or documentation claim remains |
| Documentation | Changed/all document, all-plan, and all-roadmap validators pass after lasting ownership is updated |

## Definition of Done

- StaticMeshBuild, SkeletalBuild, and TerrainBuild accept normalized value
  requests and return detached typed outputs through unload-safe providers.
- Engine editor code is the sole DDC client for all package-backed asset and
  Terrain World derived values and owns key, payload validation, fallback,
  operation diagnostics, and application or generation commit.
- StaticMesh, SkeletalMesh, AnimationClip, and TerrainHeightmap retain no
  recomputable DDC provenance or narrative operation history; their public
  mutation surface installs validated typed values and triggers only explicit
  resource, collision, or generation transitions.
- Game runtime consumes only cooked payloads and has no DDC or Developer build
  dependency.
- All ten legacy Build Functions, their registries, envelopes, codecs, cache
  fields, and object application helpers are removed.
- DerivedDataCache exposes only the low-level synchronous cache facade; the
  Build Framework and its tests are deleted after zero-client proof.
- Compatibility, focused/affected tests, Editor/Game builds, smoke, Cook,
  module closure, and documentation gates pass with evidence recorded here.

## Deferred Follow-ups

- A versioned checksummed Terrain World DDC envelope that permits recipe-free
  warm validation while retaining exact corruption detection; the current raw
  body bytes remain unchanged in this ownership migration.
- A family-neutral Engine editor derived-data orchestrator, only if the
  completed StaticMesh, skeletal, and Terrain paths demonstrate repeated code
  that cannot remain small private helpers.
- Async cache backends, remote DDC, request coalescing, global quotas,
  background garbage collection, cache statistics, and a user-facing Clear DDC
  command based on measured workload requirements.
- Additional asynchronous compilation domains for meshes, skeletal assets, or
  Terrain when editor latency measurements justify worker scheduling; this
  ownership migration preserves current scheduling semantics.
- ShaderBuild architecture changes, including any future shared cache helper,
  as a separate RenderCore/ShaderBuild plan.

## Related Documentation

- [Texture Build DDC Decoupling](Archive/2026-09/TextureBuildDdcDecoupling.md)
- [Texture Asset State Simplification](Archive/2026-09/TextureAssetStateSimplification.md)
- [Asset Data Lifecycle and Storage](../Runtime/Assets/AssetDataLifecycle.md)
- [Asset Compilation](../Runtime/Assets/AssetCompilation.md)
- [Static Mesh Rendering](../Runtime/Rendering/StaticMeshRendering.md)
- [Skeletal Mesh Rendering](../Runtime/Rendering/SkeletalMeshRendering.md)
- [Skeletal Animation Playback](../Runtime/Animation/SkeletalAnimationPlayback.md)
- [Terrain Heightmap Asset](../Runtime/Terrain/TerrainHeightmapAsset.md)
- [Terrain World Data](../Runtime/Terrain/TerrainWorldData.md)
- [Code Modules](../Workspace/CodeModules.md)
- [Agent Build and Run Workflow](../Agents/BuildAndRun.md)
- [Agent Testing Workflow](../Agents/Testing.md)

## Related Code

- `Engine/Source/Developer/DerivedDataCache/Public/DerivedDataCache/DerivedDataCache.h`
- `Engine/Source/Runtime/Engine/Private/Asset/AssetDerivedDataCache.h`
- `Engine/Source/Developer/StaticMeshBuild`
- `Engine/Source/Developer/SkeletalBuild`
- `Engine/Source/Developer/TerrainBuild`
- `Engine/Source/Runtime/Engine/Private/StaticMesh`
- `Engine/Source/Runtime/Engine/Private/SkeletalMesh`
- `Engine/Source/Runtime/Engine/Private/Animation`
- `Engine/Source/Runtime/Engine/Private/Terrain`
- `Engine/Source/Editor/AssetForgeBuiltins/Private/SceneDirectImport.cpp`
- `Engine/Source/Editor/AssetForgeBuiltins/Private/TerrainWorldBuildAdapter.h`
- `Engine/Tests/Native/EngineTests/Private/DerivedDataCacheTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/StaticMeshDerivedDataContractTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/StaticMeshDerivedDataCacheTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/StaticMeshPayloadCodecTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/SkeletalAssetTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/Terrain/TerrainHeightmapTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/Terrain/TerrainWorldBuildTests.cpp`
