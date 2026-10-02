# Unified Derived Data Build Architecture Plan

Summary: Introduce immutable build definitions, snapshot-backed input resolution, and a shared derived-data execution protocol across texture, static-mesh, physics, and shader producers while preserving typed compilation and publication.

Last reviewed: 2026-09-27

Status: Archived
Completed: 2026-09-27

## Current Status

A subsequent design is recorded in
[Derived Data Build Sessions](DerivedDataBuildSessions.md). It selects UE-inspired
registered functions, non-template sessions and shared metadata/data-block output
that replaced the typed executor. Both plans record the unperformed Windows Game
validation as deferred, without claiming a passing result.

Stages 0-5 established immutable definitions and synchronous typed execution
inside the existing `DerivedDataCache` module; the successor plan later replaced
the typed executor with sessions. Texture2D, TextureCube, VolumeTexture,
StaticMesh render, physics collision and ShaderBuild retain captured source
generations, and valid asset cache hits skip input resolution. No standalone
DerivedDataBuild module was added.

Stage 6 implementation and documentation cleanup are complete. Local acceptance
results are recorded below. On 2026-09-27, the owner deferred Windows Game
build/startup and cooked-only loading validation because no Windows acceptance
environment is available in the near term. That validation was not performed
and is no longer a completion gate; it remains future platform coverage.
Editor-native cooked loading tests and static dependency inspection do not
substitute for that runtime validation.

Decision revised on 2026-09-26: follow UE's module organization rather than add
a standalone `DerivedDataBuild` module. Build execution and cache storage need
distinct responsibilities, not separate binaries. Expand the existing module's
documented responsibility during implementation instead of preserving its
current storage-only scope through another module and dependency boundary.

The initial repository inspection confirmed:

- `FTexture2DBuildRequest` combines decoded `SourceMips`, a separate identity,
  and optional `DeferredSource`. Exclusivity and matching identity are checked
  in both build and compilation admission. PostLoad uses a torn-off source;
  the synchronous factory eagerly acquires mips.
- `FTexture2DBuildInput` already provides a separate, borrowed mip view for the
  synchronous TextureBuild recipe. Preserve this useful boundary.
- Cube requests carry normalization input; Volume requests borrow source data;
  StaticMesh requests own source and reconciliation snapshots. Their lifetimes
  and normalization rules cannot be unified by renaming fields.
- `AssetDerivedDataCache` shares bounded Get/Put and diagnostics, but family
  code still owns key construction, decode/rebuild decisions, and persistence.
- Physics uses `PhysicsCookHelper` and its own key/codec. ShaderBuild uses
  `ShaderDerivedData` and direct Get/Put in `ShaderBuilder`, with independent
  source-dependency discovery and worker ownership.
- `Durin.dworkspace` declares Engine, Sandbox, and RoadWeaver. Shared API
  migrations must account for all three projects.

## Goal

Represent a build as a deterministic description of a transformation and its
inputs, independently of where those inputs reside or which object will consume
the output. Execute cache lookup, deferred resolution, typed construction,
validation, and best-effort persistence through one protocol.

Remove the decoded-versus-deferred choice from Engine build requests. A request
binds a definition to immutable input providers; decoded images/geometry are
execution-local recipe inputs. Keep per-object freshness, transactions, CPU
publication, GPU readiness, and physics readiness with their existing owners.

## Scope and Boundaries

Required migrations cover Texture2D, TextureCube, VolumeTexture, StaticMesh
render data, physics collision cooking, and ShaderBuild compiled output. Import,
PostLoad/rebuild, explicit synchronous operations, Scene import, and Cook must
reach the same family definition and execution path where they build the same
product. Shader source discovery and material application remain specialized.

This plan does not introduce remote workers, a distributed cache, arbitrary
build graphs, a persistent build-definition database, a new thread pool, or a
universal asset status enum. It does not change authored package or cooked
payload formats merely to accommodate the framework. Generic single-flight is
deferred; existing Shader/Material sharing remains owned by those systems.

### UE reference and deliberate adaptation

Epic's public APIs establish the following reference boundaries:

- [FBuildDefinition](https://dev.epicgames.com/documentation/unreal-engine/API/Developer/DerivedDataCache/FBuildDefinition)
  is an immutable reference to a function and inputs, with constants and input
  references distinguished from materialized data.
- [IBuild](https://dev.epicgames.com/documentation/unreal-engine/API/Developer/DerivedDataCache/IBuild)
  separates definitions, input resolution, sessions, and build functions.
  UE places `DerivedDataBuild.h` and `DerivedDataBuildDefinition.h` inside
  `Developer/DerivedDataCache/Public`; these are build APIs within the
  `DerivedDataCache` module, not evidence of a separate build module.
- [FTextureSource](https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/Engine/FTextureSource)
  provides torn-off copies preserving DDC identity and separate compressed
  payload/decompressed mip access.
- [ITextureCompressorModule::BuildTexture](https://dev.epicgames.com/documentation/unreal-engine/API/Developer/TextureCompressor/ITextureCompressorModule/BuildTexture)
  receives prepared image mips and settings.

These are references for responsibilities, not a claim that this plan reproduces
UE's private texture task implementation. UE permits definitions whose variable
inputs are resolved into actions. Durin's first implementation intentionally
requires frozen function versions and stable input content identities before
lookup, so it needs one resolved definition identity rather than separate
definition and action key systems. Do not copy UE's complete graph/worker stack.

## Selected Architecture

### Ownership and dependency direction

| Owner | Responsibility after migration |
| --- | --- |
| `Core` | Existing hashes, immutable buffers, canonical serialization primitives, tasks, cancellation and module lifetime primitives. |
| `DerivedDataCache` build subsystem | Validated definition values and canonical identity, provider contracts, shared execution protocol, common cache observations and execution controls. Uses the module's cache API and Core; never depends on Engine or RenderCore. |
| `DerivedDataCache` cache subsystem | Existing bucket/key-to-opaque-bytes storage, bounds and atomic publication. Remains independent of build definitions, function bindings and asset recipe policy. |
| Engine family adapters | Canonical settings and target projection, detached source binding, family codecs, typed errors/products, recipe invocation and family validation. |
| `TextureBuild`, `MeshBuilder`, physics producer | Existing typed transformations and producer versions; no DDC, live-object access, or scheduling authority. |
| `ShaderBuild` adapter | Shader definitions, dependency snapshots, compiler invocation and output codec; uses the common protocol without depending on Engine. |
| Typed compiling managers | Admission, task scopes, memory budgets, consumer cancellation, completion mailboxes, object freshness and result application. |
| Editor operations and Cook | Existing transaction, rollback, persistence and cooked-publication policy. |

Keep the existing `DerivedDataCache` module and export boundary; do not add
another module, export macro, or library dependency for the build subsystem.
Internally, Build uses Cache, and Cache does not depend on Build. Existing
storage-only Get/Put clients do not need to construct definitions or sessions.
Engine's dependency remains editor-only and private/optional. Public
runtime asset headers must not acquire a mandatory Developer dependency;
framework bindings can remain in private adapters behind typed capture/build
entrypoints. ShaderBuild uses the build APIs through its existing
`DerivedDataCache` dependency. Stage 0 fixes header placement within that module
and verifies authoring/Game target selection before changing includes.

The shared executor is called synchronously inside the owner's existing task or
blocking boundary. It does not recursively enqueue work or create another
compilation scheduler. A small typed adapter/template layer can share the
protocol while retaining family product/error types; do not introduce
`std::any`, `void*` payload bags, or a central variant of every asset family.

### Definition, bindings, and execution are separate

Proposed names describe contracts, not final header declarations:

| Value | Required contract |
| --- | --- |
| `FBuildFunctionDescriptor` | Stable function ID, producer version, constants schema, output contract/schema, and logical cache namespace. Obtained from the actual resident producer, not duplicated version literals. |
| `FBuildInputReference` | Unique logical input name, stable content identity and identity scheme/version, representation/schema, and bounded size metadata where available. Contains no asset pointer, filesystem path, callback or decoded image. |
| `FBuildDefinition` | Frozen descriptor, canonical normalized constants including relevant targets, and ordered input references. Validated construction returns `std::expected`; the value cannot mutate after submission. |
| `FBuildInputBindings` | Request-owned providers mapping input names/identities to detached immutable snapshots or retained payload leases. Location and lifetime are outside the definition/key. |
| `FBuildExecutionPolicy` | Cache read/write policy and explicit force-rebuild behavior. Does not affect deterministic identity. |
| `FBuildExecutionContext` | Borrowed cancellation, work reservation, timing and checkpoints, valid only during execution. |
| Typed family product | Validated CPU result plus build identity/cache observations. Object serials and application context remain outside the deterministic result identity. |

Each adapter exposes capture, definition construction, input preparation, recipe
execution, product validation, cache decode, and cache encode hooks with precise
ownership. The executor owns the ordering. The first implementation does not
need a process-global dynamic function registry: bind the typed adapter and
resident producer descriptor at admission and verify their match. A registry is
future work only when independently discovered functions have a real consumer.

The function descriptor is data; the execution callback is held by a scoped
adapter binding. The owning manager retains a module/provider lease, stops
admission, and drains all callbacks and results before unloading code. A frozen
definition must never be used as evidence that a DLL is still resident.

### Identity and canonical encoding

The cache identity is a versioned, domain-separated canonical encoding of the
function ID/version, output contract/schema, normalized constants, and all
named input identities/representations. Use the existing 128-bit hash/storage
contract initially; no new hash algorithm is justified by this refactor.

- Encode field tags/types, widths, byte order, lengths and ordering explicitly;
  never hash C++ object memory, padding, pointers, unordered iteration or debug
  text. Reject duplicate input names and unknown required fields.
- Normalize defaults before identity construction, including implicit sRGB;
  define finite-float handling and reject unsupported targets before lookup.
- Distinguish semantic authored-source identity from compressed payload hash.
  A storage recompression that preserves canonical source content must not
  invalidate a build unless that representation affects the algorithm.
- Source paths, timestamps, package/object IDs, request serials, priorities,
  cancellation, persistence policy and diagnostic names are excluded. Logical
  shader include names belong in identity only where they affect compilation;
  machine-specific absolute paths do not.
- Freeze transitive output-affecting dependencies before computing the key.
  Shader dependency discovery may need source reads; do not promise texture's
  metadata-only warm-hit behavior for a cold shader dependency scan.
- Required input identities must be available without loading texture/mesh
  source payloads during ordinary PostLoad lookup. Import computes them when
  accepting canonical authored data. Missing legacy identities need an explicit
  bounded recovery decision in Stage 0, never a fabricated zero key.

The descriptor/key and actual invocation must use the same normalized settings
and target. Avoid a second freely mutable settings copy that can diverge after
the definition has been frozen.

### Deferred input resolution and lifetime

Capture bindings before asynchronous submission, but acquire source bytes only
when needed. Use source-owner APIs such as torn-off texture snapshots or
StaticMesh immutable source snapshots. Capturing is not synonymous with eager
decoding. Providers must retain the exact underlying data generation; they may
not resolve a mutable path or ask a live object for its latest source later.

Validate name/identity binding consistency without payload reads at admission.
On resolution, verify the captured representation and integrity using the
source's established validation; prove that the resolved bytes correspond to
the advertised semantic identity. Content edits after capture must either keep
the captured generation readable or cause an explicit input-resolution error,
never silently build new content under the old key.

Provider results own buffers/leases for the full synchronous normalization and
recipe call. Borrowed mip spans stay inside that lifetime. Resident imported
source and package-backed source implement the same binding contract; neither
requires a second public `SourceMips` request alternative. Providers must honor
cancellation and source/decode limits and must not hide unbounded decoded
residency beyond the manager's working-set accounting.

### Shared execution protocol

```text
typed capture -> frozen definition + retained bindings
    -> validate definition/bindings; establish producer lifetime
    -> derive key; query bounded DDC value if policy permits
       -> decode and validate complete family product
          -> accepted hit: return typed product without resolving source
          -> corrupt/incompatible value: record rejection and continue
    -> cancellation check; reserve/confirm required work budget
    -> resolve exact input snapshots; normalize typed recipe input
    -> invoke resident typed producer; validate complete output
    -> encode/store if policy permits; retain internal typed observations
    -> report recoverable cache issues once at the Engine execution boundary
    -> return typed product to the existing owner/application boundary
```

Cache reads, source resolution, normalization, recipe execution and persistence
have separate timing/byte observations. Invalid definitions and unavailable
producers are failures, not cache misses. Missing/corrupt cache data may rebuild;
unavailable or corrupt authoritative input must fail rather than recapture the
original import file. Cancellation during cache decode must remain cancellation,
not fall through to a rebuild. Recheck cancellation before returning a product;
manager freshness still decides whether a completed result may apply.

Read failures and best-effort write failures preserve existing family policy and
diagnostic causes. A valid product remains usable after a cache write failure.
No partial product is published or persisted. A completed atomic write may
remain after late cancellation; it is deterministic cache data, not permission
to apply a canceled result. Every accepted consumer gets one terminal outcome.

Retain typed CPU products on cold builds; do not force encode-then-decode merely
to pass through a generic byte result. Cache bytes use existing family-owned
serializers. Decode destinations remain unpublished until validation succeeds.
Zero-copy buffer ownership and output moves must survive the adapter boundary.

### Compatibility and existing contracts

Use a new explicit build-key schema/domain per migrated family. Accept the
one-time disposable DDC miss rather than maintaining permanent dual lookup.
Remove each legacy key path when its callers migrate. Preserve authored identity
and cooked payload formats unless a separately demonstrated representation
change requires a version bump. Cook must continue copying validated products
into package ownership; Game never needs definitions, source resolvers or DDC.

This deliberately expands `DerivedDataCache` from a storage-only module to a
module containing both build and cache subsystems, and changes the current rule
that each Engine family implements its own complete cache execution flow.
Engine still selects asset policy and owns codecs/application, while the
module's build subsystem performs the shared protocol. The cache subsystem
continues to expose opaque storage without asset or recipe knowledge.
It preserves the prohibition on recipe-owned DDC and on a typeless compiling
manager. Update these authorities only with the corresponding implementation:

- [Asset data lifecycle](../../../Runtime/Assets/AssetDataLifecycle.md#serialization-and-production-ownership)
  and its derived-data identity/storage rules.
- [Asset compilation](../../../Runtime/Assets/AssetCompilation.md#proven-reuse-boundary)
  for typed manager, freshness, scheduling and module lifetime boundaries.
- [Static mesh building](../../../Runtime/Assets/StaticMeshBuilding.md),
  [Volume textures](../../../Runtime/Assets/VolumeTextures.md), and
  [Shader cache](../../../Runtime/Rendering/ShaderCache.md) for family behavior.
- [Async asset operations](../../../Editor/Architecture/AsyncAssetOperations.md) and
  [Cooking](../../../Runtime/Assets/Cooking.md) for transactions and publication.
- [Code modules](../../../Workspace/CodeModules.md) for the expanded `DerivedDataCache`
  responsibility and preserved dependency direction.

## Implementation Stages

### Stage 0: Freeze contracts and establish baselines

Dependencies: none. Outcome: an auditable API/identity matrix and reproducible
before-state measurements, with all implementation-blocking choices resolved.

- [x] Record the selected architecture, UE reference boundaries, initial family
  differences, and existing contracts that will change.
- [x] Select UE-style placement within `DerivedDataCache`, with separate Build
  and Cache responsibilities and no standalone build module.
- [x] Audit all producers/callers across Engine, Sandbox and RoadWeaver; record
  exact source/test owners, cache buckets, key fields, versions, source identity
  schemes, target support, error dispositions and output serializers.
- [x] Fix build API/header placement within `DerivedDataCache` and verify
  authoring/Game target selection without Engine/RenderCore dependency cycles.
  Define the minimal typed adapter API.
- [x] Verify snapshot stability across source edits, unload/reload, recompression
  and package replacement; settle integrity checks and missing-identity recovery.
- [x] Resolve Cube normalization ownership from current code and fixtures before
  changing its identity; do not assume panorama and six-face sources are interchangeable.
- [x] Capture cold/warm build outputs, source-range read counts, decode/recipe
  counts, peak/resident bytes, latency and cancellation/shutdown behavior for
  representative texture, mesh, collision and shader fixtures. Define acceptable
  regression bounds and repeat counts before collecting after-state results.

Gate: record the matrix, decisions and baseline receipts in this plan. Do not
close this stage based on document review alone.

#### Stage 0 audit and frozen decisions (2026-09-26)

The initial audit covers the source/test roots of all three workspace projects.
The six request/key/helper families have 105 declaration/use matches under
Engine; Sandbox and RoadWeaver have no direct matches for these request or key
APIs. They remain integration build consumers of Engine. Additional loaded
texture paths live in `TextureCube.cpp` and `VolumeTexture.cpp` and must
be migrated alongside detached build entrypoints, not left as duplicate caches.

| Producer and owning implementation | Current identity and cache | Version/target/output boundary | Principal regression owner |
| --- | --- | --- | --- |
| Texture2D: `Engine/Private/Texture/Texture2DBuild.cpp`; module `TextureBuild` | `Textures/Objects`; canonical `FTextureSource` identity, usage, resolved sRGB, quality, alpha mode/threshold, max resolution | Builder 4, texture payload schema 2; Win64 Game/EditorValidation; `FTexturePlatformData` serializer | `TextureTests`, `TextureImportWorkflowTests`, `SceneImportTests`, `CookFunctionalTests` |
| Cube: `TextureCubeBuild.cpp` plus `TextureCube.cpp` loaded-cache fast path | `TextureCube/Objects`; canonical faces identity for LDR; full-precision panorama identity, face dimension and exposure for HDR; sRGB, projection version | Builder 4, projection 3, texture payload schema 2; Win64 Game/EditorValidation; `FTextureCubePlatformData` serializer | `TextureTests`, panorama and six-face fixtures, import and Cook targets |
| Volume: `VolumeTextureBuild.cpp` and source-to-platform adapters | `VolumeTexture/Objects`; canonical source identity, dimensions, output format/mip filter and source schema | Builder 3; Win64 Game/EditorValidation; `FVolumeTexturePlatformData` serializer | `TextureTests` volume identity, mip, transactional replacement and package/Cook fixtures |
| StaticMesh: `StaticMeshDerivedDataBuild.cpp`; module `MeshBuilder` | `StaticMesh/Objects`; source geometry identity plus reconciliation hash (normalized size and ordered slot name/source name/source index) | Builder 4, payload schema 5, legacy key schema 4; Win64; `FStaticMeshPayloadData` serializer | `StaticMeshTests`, `StaticMeshBuildQualificationTests`, import/Cook targets |
| Collision: `PhysicsCookHelper.cpp` | `StaticMeshCollision/Objects`; geometry hash, source mode, query policy, weld-tolerance bits in key contract | Cook builder 2, payload schema 2, legacy key schema 3; Win64; `FPhysicsCollisionPayloadData` serializer | Physics cook and separated lifecycle fixtures in `StaticMeshTests` |
| Shader: `ShaderBuild/Private/ShaderBuilder.cpp` and `ShaderDerivedData.cpp` | `Shaders/CompiledOutput`; variant v6 (compiler environment, target, dependencies/options) plus ordered entry points/frequencies | Compiled-output builder 1/schema 1; existing Slang target/profile; `ShaderCompiledOutput` codec | `RenderShaderBuilderTests`, `RenderShaderCacheTests`, shader Cook/Material lifecycle targets |

Unsupported targets remain explicit failures. Platform identity in these
contracts is the produced payload target, not the macOS host running tests.
Keep family input/recipe/codec causes typed; cache miss/read rejection may rebuild,
Put failure remains a warning, and cancellation never becomes a corrupt-cache miss.
Existing Cube/Volume string diagnostics and Shader counters need boundary adapters,
not a universal string-only error replacement.

Snapshot decisions:

- `FEditorBulkData` copies retain an immutable state holding a buffer or package
  resource range. Package reads check expected size and content hash. A replacement
  can provide the retained generation or fail integrity checks; never reopen an
  import filename or silently substitute current object content.
- `FTextureSource::CopyTornOff` clears the owner and creates independent decoded
  residency. Source identity includes kind, schema, shape/layers, gamma, channel
  metadata, canonical size and decoded hash, excluding lossless storage compression.
  `GetMipData` checks decoded content hash before publishing residency.
- StaticMesh copies retain source BulkData, and geometry acquisition verifies
  payload, metadata, full decode and cancellation before publishing residency.
  Source identity includes geometry schema, counts and payload content identity.
- Reject invalid/zero identities at capture with a family input error. There is
  no implicit legacy reimport or invented identity fallback in this migration.
  Any legacy compatibility conversion belongs to explicit asset maintenance.
- Shader uses captured source/include artifacts and logical dependency identities;
  preserve the existing captured-include tests and fingerprint invalidation.

Cube decision: LDR panorama import normalizes to canonical RGBA8 faces before
derived construction; HDR retains RGBA32F panorama and performs projection as
derived work. Loaded canonical faces/HDR panorama already have metadata-only
lookup paths. A noncanonical authored panorama may require normalization first;
do not broaden the zero-source-read guarantee to that recovery path. Definition
construction must converge for import and reload of the same canonical source.
Volume currently acquires source before its synchronous PostLoad build; Stage 3
must move that acquisition behind lookup, not treat the existing path as lazy.

API placement: use `DerivedDataCache/Public/DerivedDataBuildDefinition.h` for
immutable definitions/canonical constants and `DerivedDataBuild.h` for execution
policy, observations and a typed synchronous executor. Private implementation
stays within the same module. Core-only module dependencies and the existing
Engine optional-private / ShaderBuild private dependencies remain sufficient.
`Engine.dproject` selects DDC for DurinEditor and excludes it from DurinGame.

The minimal adapter freezes descriptor/constants and typed source snapshots,
then offers binding validation, resolve/prepare, build, validate, encode and
decode operations. Family product and error types remain template parameters;
shared code controls stage ordering and cache I/O. Validate binding identities
even on a warm hit; do not invoke the payload resolver on that hit. Definitions
own their data and expose const access only. Canonical constants use tagged,
fixed-width little-endian fields and normalized floats rather than raw structs.

Baseline comparison policy, fixed before after-state measurements: preserve
deterministic product bytes and source identity, exact read/recipe counts, all
terminal and rollback assertions, and current reservation bounds. Fixed-fixture
retained product bytes must remain equal; no extra full source/product copy is
allowed. Timing and sampled process allocation are diagnostic on this shared
host, not proof of a performance gate. Investigate a repeated median increase
over 10% (or 1 ms for short operations) or peak increase over 10% before closing
the affected stage. Reproduce with one warm-up and three measured runs where
existing qualification supports it; performance acceptance requires a quiet
lane as specified by the qualification guidance.

Initial receipts on `MacOS-arm64-Debug-DurinEditor` at baseline `b12acfd93`:

- `TextureTests --report`: 124/124 passed, 8.945 s test-body total.
- `StaticMeshTests --report`: 141/141 passed, 1.117 s test-body total; includes
  independent collision cooking, cancellation, worker drain and source residency.
- `RenderShaderBuilderTests --report`: 20/20 passed, 1.720 s; includes corrupt
  recovery, failed Put, captured inputs and concurrent single-flight.
- `StaticMeshBuildQualificationTests --mode qualification --report`: 4/4 passed.
  The 100k-triangle source is 4,800,147 bytes; sampled allocated peak 174,310,752
  bytes. The budget fixture reports 2,759,296 ray bytes and 10,248,808 collision
  bytes, with decode/render 1.785 s and collision 1.568 s. Concurrent render
  reservation is 616,583,552 bytes and sampled peak is 345,593,984 bytes.
- `TextureCompressionQualificationTests --mode qualification --report`: passed;
  serial/parallel median ms: Color 835.110/325.657, Normal 3450.470/1335.930,
  DataMask 2104.820/818.113. One warm-up and three samples per mode; diagnostic only.
- XML receipts were copied to ignored `Build/UnifiedDerivedDataBuildBaseline/`
  before subsequent runs overwrite the normal report paths. Correctness fixtures
  retain byte-equivalence assertions; these timings are not end-to-end import times.
- `AssetPackageTests` initially failed to compile because `FCookShaderStub`
  omitted the existing pure virtual `GetCookInputIdentity`; baseline preparation
  supplies a stable fixture identity and cancellation result. The rerun selected
  prepared package snapshots, EditorBulkData, and package Cook cases: 21/21 passed.
- `RenderShaderCacheTests --report`: 6/6 passed, including portable identity,
  exact entry-point identity, malformed payload rejection and golden output bytes.
- Shader cold/warm baseline: 101,559/303 microseconds; 8 dependencies, 3,498
  source bytes, 3,752 SPIR-V bytes and 4,384 DDC bytes. Full-process shader peak
  allocation is not measured; retained artifact size is the comparable baseline.
- Texture qualification now also records deterministic mip hashes and memory:
  peak intermediate mip bytes 1,398,100 for all usages; retained mip bytes
  Color 699,064, Normal/DataMask 1,398,128. Hashes respectively
  `f83f899dddee83f4f99ec62ce8512bf5`, `a7c883d3207e5cd1edcde57f297f927f`,
  `a014b1b2457ab9d6d9030180b61af423`. Repeated serial/parallel rounds agree.
  This is algorithm-reported intermediate memory, not whole-process RSS.

Stage 0 completion is based on these bounded CPU fixtures, existing lifecycle
assertions, source inspection, and preserved receipts. GPU/application smoke is
not required for this architecture baseline. No cross-host timing claim is made.

### Stage 1: Add definition and execution primitives

Dependencies: Stage 0. Outcome: a bounded, object-independent framework exercised
by a deterministic fixture adapter, without changing production callers.

- [x] Add build APIs and implementation within the existing `DerivedDataCache`
  module/export boundary; preserve its Core-only module dependency and existing
  authoring selection. Keep cache storage usable independently of build execution.
- [x] Implement validated immutable definitions, canonical key encoding,
  snapshot-binding checks, policy/context and typed adapter execution.
- [x] Test equivalent canonical definitions, each output-affecting field,
  excluded policy fields, duplicate/missing inputs and descriptor mismatches.
- [x] Exercise hit, miss, corrupt decode, resolver failure, cancellation at each
  boundary and failed Put, including complete-product-only publication.
- [x] Prove no new module or scheduler, retained live object, mandatory Game
  dependency, type-erased family payload or cold-build serialization round trip
  is required; cache backends must not depend on build APIs.

Gate: core protocol and identity fixtures pass; source resolution is never called
on a valid fixture cache hit, and typed failures retain their causes.

Stage 1 evidence (2026-09-26): `DerivedDataBuildTests --report` passes 12/12
cases on the default macOS Debug editor profile. The target links only Core and
DerivedDataCache. Definitions own canonical ordered fields, normalize negative
zero, reject non-finite floats and ambiguous/oversized input, and explicitly
encode little-endian values. Typed execution preserves move-only products and
family errors, checks bindings before cache reads, and latches cancellation.
The framework remains unused by production asset callers until their stages.
`DerivedDataCacheTests --report` also passes 12/12 existing storage cases.
Configuration required authorized access to the main checkout shared dependency
lock; no dependency or target-profile changes were needed.

### Stage 2: Migrate Texture2D end to end

Dependencies: Stage 1. Outcome: one Texture2D build definition and execution path
serves import, PostLoad, explicit rebuild, Scene import and Cook.

- [x] Replace `SourceMips + SourceIdentity + optional<DeferredSource>` request
  state with a typed captured source binding and frozen definition preparation.
  Make invalid capture fail explicitly instead of returning an empty request.
- [x] Keep `FTexture2DBuildInput` as the synchronous borrowed recipe view; prepare
  its mips only after a miss, with execution-owned lifetime and budget accounting.
- [x] Migrate manager admission, dimension/working-set inspection, synchronous
  entrypoints, tests and all consumers; remove duplicate branch validation and
  the legacy key path. Preserve latest-wins and selected finish semantics.
- [x] Verify warm hits cause zero source-range reads/decode/recipe calls; miss and
  corrupt-cache paths build the captured generation exactly once per execution.
- [x] Test source mutation, destruction, late cancellation, unavailable module,
  failed persistence, shutdown and Cook reuse without changing publication rules.

Gate: typed product equivalence, measured baseline comparison, all affected
consumer targets and a complete `all` build pass before handoff.

Stage 2 evidence (2026-09-26): `TextureTests --report` passes 124/124
(9.125 s versus the 8.945 s baseline, a diagnostic 2.0% increase). Existing
recipe byte-equivalence, mip layout and source-generation assertions pass.
Capture now performs zero source reads, including the synchronous entrypoint;
external-bulk warm hits remain zero-read. Corrupt recovery, shutdown, selected
finish, unavailable modules, failed persistence and transactional rollback pass.
`TextureImportWorkflowTests` passes 23/23, `SceneImportTests` 20/20,
`CookFunctionalTests` 5/5 and `AsyncTaskPilotLifecycleTests` 1/1.
`./DevTool build --target all` passes on `MacOS-arm64-Debug-DurinEditor`,
including workspace consumers. Source/test searches across Engine, Sandbox and
RoadWeaver found no remaining legacy request fields. The key golden changes
intentionally to the definition schema; platform payload encoding is unchanged.

### Stage 3: Migrate TextureCube and VolumeTexture

Dependencies: Stage 2. Outcome: all texture families share definition/execution
mechanisms while preserving their distinct source and normalization contracts.

- [x] Replace borrowed Volume request lifetime with owned retained input bindings
  at submission; retain efficient synchronous recipe views internally.
- [x] Separate Cube import translation/canonical source acceptance from derived
  platform construction; encode projection/normalization versions wherever
  they affect derived output. Never reimport physical files on a DDC miss.
- [x] Migrate family targets, codecs, diagnostics and cache policies; remove
  superseded key/execution paths and temporary Texture2D-only scaffolding.
- [x] Validate six-face and panorama fixtures, volume mip/shape rules, warm-hit
  source reads, cold/corrupt recovery, cancellation and cooked-only loading.

Gate: all texture entrypoints use the common protocol; recipe signatures remain
typed and behavior/working-set baselines meet Stage 0 bounds.

Stage 3 evidence (2026-09-26): `TextureTests --report` passes 124/124
(9.077 s, 1.5% above Stage 0, diagnostic only). The 128-cubed Volume fixture now
asserts zero external source-range reads on warm PostLoad, then one on explicit
source access. Volume requests own `FTextureSource`; a retained-generation
assertion passes after the original voxel input changes. Cube import normalization
and loaded canonical source construction converge on one definition/executor;
normalized import images are reused by shared view on cold execution. Existing
six-face, LDR/HDR panorama, corrupt recovery, shape/mip, import rollback,
queued cancellation and cooked-only fixtures pass. Texture platform payload
codecs remain unchanged; key goldens intentionally adopt the definition schema.
`TextureImportWorkflowTests` passes 23/23 and `CookFunctionalTests` 5/5;
`./DevTool build --target all` passes. The obsolete texture Load/Store helper
and Cube's second lookup path are removed. No shader/mesh migration is implied.

### Stage 4: Migrate StaticMesh and physics collision builds

Dependencies: Stage 3. Outcome: independent render and collision definitions use
the framework without merging their readiness or publication transactions.

- [x] Adapt StaticMesh source snapshots, reconciliation and render codecs; decide
  from the audited matrix which material metadata affects cached output and
  which is restored only during application.
- [x] Adapt PhysicsCookHelper and collision identity/validation. Keep geometry,
  collision settings, producer and target dependencies explicit; do not assume
  a render key is sufficient to identify collision output.
- [x] Preserve mutation acceptance, render publication, collision admission,
  canceled-decode handling and source working-set reservations.
- [x] Test independent render/collision hits and misses, missing/corrupt input,
  supersession, rollback, unchanged source identity and Cook product assembly.
- [x] Remove old family key/execution duplication after all callers migrate.

Gate: render and collision equivalence and resource limits pass; no generic DAG
is needed merely to coordinate the existing family-owned sequence.

Stage 4 evidence (2026-09-26): `StaticMeshTests --report` passes 141/141
(0.716 s versus the diagnostic 1.117 s baseline). Existing fixtures verify
independent render/collision lookup, corrupt recovery, source residency, selected
completion, canceled decode, supersession, replacement rollback and Cook assembly.
Material slot names/source names/source indices and normalization remain in the
canonical reconciliation identity; runtime-only restoration remains Engine-owned.
Render finalization now runs in adapter validation before publishing cache bytes.
Cache decode reads shared immutable bytes directly, eliminating the old byte copy.
`StaticMeshBuildQualificationTests --mode qualification --report` passes 4/4:
100k source bytes remain 4,800,147; ray/collision retained bytes remain
2,759,296/10,248,808; concurrent reservation remains 616,583,552. Sampled source
peak is 138,704,080 and concurrent peak 271,958,816 bytes, below the diagnostic
baselines. Decode/render and collision are 0.907/1.004 s; no timing regression
is inferred from this shared-host run. `SceneImportTests` passes 20/20,
`CookFunctionalTests` 5/5, and `./DevTool build --target all` passes. Only key
goldens change; the frozen collision payload golden and render payload tests pass.

### Stage 5: Migrate ShaderBuild cache execution

Dependencies: Stage 4. Outcome: ShaderBuild proves the framework is independent
of Engine while preserving shader-specific dependency and sharing semantics.

- [x] Capture complete source/include/generated-input identities and normalized
  compiler options, compiler version, entry points, target and reflection contract.
- [x] Bind resolution to the captured shader dependency closure; reject changed
  content rather than compiling new bytes under an old variant identity.
- [x] Replace direct duplicated cache execution with the shared adapter; retain
  ShaderBuild's dependency manifests, worker limits, LRU and single-flight.
- [x] Preserve per-consumer cancellation, Material last-known-good results,
  reload checks and RenderCore-owned runtime output serialization.
- [x] Test include edits, compiler/options changes, identical inputs at different
  physical roots, corrupt entries, multi-consumer cancellation and module drain.

Gate: compiled-output/reflection equivalence and dependency invalidation pass;
no second generic sharing layer or Engine dependency has been introduced.

Stage 5 evidence (2026-09-26): `RenderShaderBuilderTests --report` passes
22/22, including two new boundaries: changed bytes fail captured-identity
resolution, and a mounted generated include changed after resolution cannot
alter the compiled generation. Mounted and already captured requests now share
the definition/executor; the original LRU, manifests, compiler sessions and
single-flight ownership remain in ShaderBuild. `RenderShaderCacheTests` passes
6/6, `RenderShaderContractTests` 52/52, `RenderShaderCookIntegrationTests` 1/1,
and `MaterialCompileLifecycleTests` 4/4 (including cancellation and module drain).
The fixed shader fixture retains 8 dependencies, 3,498 source bytes, 3,752 SPIR-V
bytes and 4,384 DDC bytes. One diagnostic run reports cold/warm 105,669/427 us
versus 101,559/303 us; the short warm delta is below 1 ms. `all` passes after the
shared RenderCore snapshot ownership overload. A macOS dyld crash occurred
before main during discovery/startup (Loader::loadAddress / generateAtlas);
command-local `DYLD_USE_CLOSURES=0` allows the final checks to pass. No project
build configuration or machine-wide setting was changed.

### Stage 6: Retire legacy paths and publish contracts

Dependencies: Stages 2-5. Outcome: the architecture is authoritative across the
required producers, with no permanent compatibility branch or partial migration.

- [x] Search all workspace source/test roots for legacy key builders, dual-source
  requests and duplicated lookup/rebuild/store flows; remove or explicitly
  justify unrelated storage-only DDC clients.
- [x] Publish implemented build-definition, identity, resolver and execution
  contracts in the owning documentation domain; update all linked ownership
  documents and module routing. Resolve any stale Shader ownership wording
  against the migrated code rather than copying it into the new contract.
- [x] Complete the host-supported acceptance matrix, compare Stage 0 outputs/
  measurements and perform an `all` build covering Engine/Sandbox/RoadWeaver.
- [x] Verify native cooked-loading paths and authoring shutdown with queued/
  running/canceled work.
- [x] Record the owner's 2026-09-27 decision to defer the unperformed Windows
  Game build/startup and cooked-loading validation until an environment is
  available, without treating it as a passed check.
- [x] Record exact local validation evidence and close only passed checks.
- [x] Complete this plan according to its lifecycle rules after the
  host-supported acceptance gates pass and the Windows gap is recorded.

Gate: every required family is migrated, the host-supported acceptance gates
pass, lasting contracts are published, and migration-only APIs have been removed.
Windows Game validation is deferred outside this plan's completion gate.

Stage 6 evidence (2026-09-26): workspace source/test searches cover Engine,
Sandbox and RoadWeaver. Removed `AssetDerivedDataCache` lookup/store wrappers,
unused key-schema constants and redundant Cube recipe identities. Remaining
private key inspection helpers project the canonical definition; direct Get/Put
in migrated-family tests only seeds corrupt entries or exercises diagnostics.
Volume import releases translation pixels after canonical source ownership is
established. The authoritative contract is now
[Derived Data Build Protocol](../../../Runtime/Assets/DerivedDataBuild.md), linked from
asset lifecycle, mesh, volume, shader, module ownership and routing documents.

Final cleanup checks use `MacOS-arm64-Debug-DurinEditor` and command-local
`DYLD_USE_CLOSURES=0`. `TextureTests` passes 124/124 (9.162 s), `StaticMeshTests`
141/141 (0.768 s), `TextureImportWorkflowTests` 23/23, `CookFunctionalTests` 5/5,
`DerivedDataBuildTests` 12/12, `DerivedDataCacheTests` 12/12,
`CookedMeshLoadingTests` 4/4, `RenderShaderCookedLibraryTests` 4/4 and
`AsyncTaskPilotLifecycleTests` 1/1. The final `all` build passes and covers
Engine, Sandbox and RoadWeaver (`20260926-230102-125543-19448-cmake.log`). Stage 5 shader/reflection/Cook and material
shutdown receipts remain applicable; shader production code did not change in
cleanup. The cooked-library executable initially crashed before main despite
the environment setting; its macOS diagnostic report again identifies
`dyld4::Loader::loadAddress` / `generateAtlas`. The identical command passed on
retry. The environment setting is not a confirmed fix for this intermittent
host-loader failure. Native XML receipts are under
`Build/NativeTestResults/MacOS-arm64-Debug-DurinEditor/`.

Stage 0 comparisons are recorded in the corresponding migration stages:
texture output hashes and retained-byte bounds, mesh/collision payload output
and qualification bounds, and shader dependency/source/SPIR-V/DDC byte counts
are preserved. Definition keys intentionally change. Diagnostic wall times on
this shared host are not a performance qualification claim.

Deferred platform validation: `CMakePresets.json` registers Game only for Win64;
`Engine.dproject` includes authoring/DDC modules only in Editor extras and
RenderCore's module manifest has no DDC dependency. These inspections plus the
Editor-native cooked tests establish local coverage, not a successful Windows
Game build or startup. Run the Windows Game build, startup and cooked-loading
checks when an acceptance environment becomes available; their outcome is
currently unknown.

#### Stage 6 follow-up consolidation (2026-09-27)

The user approved a further cleanup of reusable mechanics after implementation.
Core now owns fixed-width byte encoding and canonical hash field updates;
BinaryFormat, DDC, StaticMesh and shader identities share those primitives.
Non-Cube key bytes remain compatible on the supported little-endian targets.
Cube uses a single complete-source reference and constants schema 2; this
explicitly supersedes the Stage 3 Cube key goldens while leaving payloads intact.
Engine retains texture codecs and error translation. DDC exact input matching
uses a linear sorted fast path and O(n log n) unordered fallback. Core adds
bounded file reads; ShaderBuild retains content verification, cancellation and
shared capture budgets. No additional module or generic adapter hierarchy is
introduced.

Follow-up validation passes on `MacOS-arm64-Debug-DurinEditor`: `all --jobs 1`
completes (`20260927-004351-352425-21941-cmake.log`), and `test affected
--test-jobs 1 --report` passes all 52 selected native-test targets with no
failures or skips (`20260927-004656-856329-22476-ctest.log`, `affected.xml`).
The final affected run used command-local `DURIN_AGENT_JOBS=1` outside the
sandbox after repeated pre-main dyld `loadAddress` / `generateAtlas` failures
inside it. Serial execution and `DYLD_USE_CLOSURES=0` alone did not eliminate
those failures; no project or machine-wide configuration changed. Earlier
focused passes include Core utilities 115/115, file-system 57/57, build protocol
13/13, textures 125/125 and shader builder 22/22. Added coverage checks explicit
canonical bytes and field boundaries, rejects implicit float/pointer hashing,
exercises bounded reads, validates 4,096 unordered/duplicate bindings, and proves
Cube source identity retains face order. Cube key goldens are now
`3a8c771aab669bbdac6874ba34c3c5ab` (faces) and
`8fb682cb2f8e7294c71edd27f49bd59b` (panorama). Changed-document and all-plan
validation pass. Windows Game validation remains unperformed and deferred.

#### Stage 6 diagnostic simplification (2026-09-27)

Remove `FAssetCacheDiagnostic`, `FAssetCacheDiagnostics` and
`FAssetBuildCacheWarning`, their formatters, and cache diagnostic fields passed
through public products, worker records and completion callbacks. Engine reports
recoverable cache failures through its existing logger once at the execution
boundary, even when a later build or publication fails. Callers retain actual
build/application errors and completion states. DDC keeps typed internal causes
and metrics; `VisitBuildIssues` exposes the original failed operation without
creating another wrapper. Normal misses are not error observations. Texture
codec errors preserve their Archive cause until reporting. Payloads, keys and
build algorithms are unchanged by this cleanup.

Validation on `MacOS-arm64-Debug-DurinEditor`: `build --target all --jobs 8`
passes (`20260927-011004-323766-26657-cmake.log`). The affected run passes
87 of 88 native-test targets (`20260927-010848-153898-25006-ctest.log`);
`RenderShaderContractTests` crashes before main in dyld `loadAddress` /
`generateAtlas`, including one isolated target retry outside the sandbox.
A command-local `DYLD_USE_CLOSURES=0` retry passes all 52 shader contract cases
(`20260927-011137-657630-27083-RenderShaderContractTests.log`), completing
coverage of the selected targets without changing project or system settings.
After final formatting and test-capture cleanup, StaticMesh passes 141/141,
build protocol passes 14/14, and the texture cache-failure async completion case
passes in isolation. The earlier complete Texture run passes 125/125.
Changed-document and all-plan validation pass. Windows Game validation remains
unperformed and deferred.

## Validation and Handoff

Follow [agent build guidance](../../../Agents/BuildAndRun.md) and
[agent test guidance](../../../Agents/Testing.md) before selecting/running builds and
native tests. Use [documentation validation](../../../Agents/Documentation.md) for
plan/contract changes. Each implementation handoff records exact fixtures,
target/configuration, checks run, measured deltas and any unsupported host path.
Deferred platform gaps remain recorded rather than implied cross-platform success.

The minimum behavioral matrix includes identity determinism/invalidation;
metadata-only texture/mesh warm lookup; cold and corrupt-cache recovery; bounded
source decode; source-generation stability; typed error preservation; persistence
failure with usable output; cancellation before/during/after work; supersession;
selected completion; shutdown/module retirement; import/reimport rollback; and
Cook reuse. Cooked-only Game loading is deferred Windows coverage. Shader
warm-hit measurements distinguish dependency-discovery I/O from recipe
execution and artifact lookup.

Every shared Engine API migration searches all project roots declared in
`Durin.dworkspace`, migrates consumers together, and completes an `all` build.
Avoid concurrent source/build writers in one checkout. Commit validated stage
work with this plan's exact `Plan` and `Stage` trailers and update its status in
the same commit. Do not start subsequent stages to conceal a failed earlier gate.
