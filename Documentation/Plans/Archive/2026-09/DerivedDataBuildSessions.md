# Derived Data Build Sessions Plan

Summary: Replace template-adapter execution with UE-inspired build sessions, registered functions, immutable actions, shared build outputs and cache records inside DerivedDataCache.

Last reviewed: 2026-09-27

Status: Archived
Completed: 2026-09-27

## Current Status

Stages 0 through 5 are complete. Texture2D, Cube, Volume, StaticMesh render and
physics collision execute through explicitly registered shared-output sessions.
Cold native arrays are retained through outputs and consumers; serialized mesh
and collision arrays undergo one warm native conversion. Collision cooking
finishes immutable arrays before runtime geometry publication, and input capture
computes identity before metadata-only lookup. Package/Cook schemas and bytes
remain unchanged. Paired CPU time, peak-memory and allocation-transfer gates
pass on the validated macOS profile. ShaderBuild now executes through its
registered shared-output session, with immutable source and bytecode blocks.
Stage 5 final CPU performance and consumer validation pass. Stage 6 legacy API
removal, test migration, documentation and macOS validation are complete. On
2026-09-27, the owner deferred Windows Game build/startup validation because no
Windows acceptance environment is available in the near term. That validation
was not performed and is no longer a completion gate for this plan; it remains
future platform coverage when an environment becomes available.

Decision revised on 2026-09-27: establish shared metadata/data-block products
before migrating execution. Add a UE-inspired `FCacheRecord` as the keyed
persistence representation, with explicit conversion to/from `FBuildOutput`.
Remove the mandatory single-`Primary` archive and cold serialize/decode round
trip. Cache record encoding is optional persistence work, not a prerequisite for
returning a valid in-memory output. This supersedes the earlier plan's blanket
encoding-failure behavior change.

Baseline commit
`079166382` removes public cache diagnostic wrappers, while the six producer
families still execute through `ExecuteBuild<TAdapter>`.

This plan succeeds the execution design in
[Unified Derived Data Build Architecture](UnifiedDerivedDataBuildArchitecture.md).
Its definition capture, source ownership, recipe boundaries and publication
rules remain useful. The earlier plan also records the deferred Windows Game
build/startup validation; neither plan claims completion evidence for it. Its
restrictions against a function registry and separate definition/action
identities have been superseded by the implemented stages.
[Derived Data Build Protocol](../../../Runtime/Assets/DerivedDataBuild.md) records the
implemented contract.

## Goal

Make the shared executor depend on one concrete input/output protocol rather
than instantiate the entire pipeline for every family's product and error type.
Build functions receive immutable constants and named byte inputs, then emit
small immutable metadata and named shared data blocks. Engine and ShaderBuild
assemble their strongly typed products from those blocks outside the executor.
The in-memory product representation is independent of its cache/archive format. Existing managers still decide
when work may run and whether its result may be published.

This is an architectural change, not a virtual interface with the current
adapter's `Resolve/Build/Validate/Decode/Encode/MakeError/IsCancelled` hooks.
Keep all shared build APIs in `Developer/DerivedDataCache`; add no
`DerivedDataBuild` module or replacement asset compilation framework.

## UE Reference and Selected Adaptation

The following Epic public APIs were checked on 2026-09-27. They establish public
responsibilities, not proof of UE's private scheduling, cache recovery or codec
implementation. The proposed Durin signatures below are design sketches.

| UE reference | Adopted responsibility | Deliberate Durin scope |
| --- | --- | --- |
| [IBuild](https://dev.epicgames.com/documentation/unreal-engine/API/Developer/DerivedDataCache/IBuild) | Build service creates sessions and exposes registered functions | A concrete module service is sufficient; no worker registry or duplicate service abstraction |
| [FBuildDefinition](https://dev.epicgames.com/documentation/unreal-engine/API/Developer/DerivedDataCache/FBuildDefinition) | Description names a function, constants and input references | Current families capture stable source identities; do not add arbitrary dependency graphs |
| [FBuildAction](https://dev.epicgames.com/documentation/unreal-engine/API/Developer/DerivedDataCache/FBuildAction) | Action fixes the function version and resolved input identity for execution | Explicit identity schemes preserve existing semantic source hashes; do not silently treat them as raw byte hashes |
| [FBuildSession](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Developer/DerivedDataCache/FBuildSession) | Session groups builds with a resolver and exposes asynchronous completion | Dispatch through existing owner workers; no new thread pool |
| [IBuildFunction](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Developer/DerivedDataCache/IBuildFunction) | Registered, versioned, stateless function transforms context inputs into outputs | Engine owns asset function implementations; pure recipe modules remain unchanged |
| [FBuildContext](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Developer/DerivedDataCache/FBuildContext) | Context provides constants/input bytes and accepts output values and deterministic messages | A bounded local byte protocol; no new compact-binary library or remote execution requirement |
| [IBuildInputResolver](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Developer/DerivedDataCache/IBuildInputResolver) | Resolve build input references | Separate metadata identity resolution from materializing bytes, preserving zero-source-read warm hits |
| [FCacheRecord](https://dev.epicgames.com/documentation/unreal-engine/API/Developer/DerivedDataCache/FCacheRecord) | Keyed record holds metadata and named values for cache storage | Convert complete build outputs to/from one record while retaining the current atomic Get/Put backend |

Additional public references supporting the data ownership design:

- [FBuildOutput](https://dev.epicgames.com/documentation/unreal-engine/API/Developer/DerivedDataCache/FBuildOutput)
  contains metadata and ID-addressed values, and saves values as attachments.
- [FSharedBuffer](https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/Core/FSharedBuffer)
  supports taking ownership and views retaining an outer buffer's lifetime.
- [ITextureCompressorModule::BuildTexture](https://dev.epicgames.com/documentation/unreal-engine/API/Developer/TextureCompressor/ITextureCompressorModule/BuildTexture)
  produces compressed mip data and metadata rather than a complete platform object.

These interfaces support the selected design; they do not prove zero-copy
behavior in every UE texture path. Cache decompression, layout conversion and
GPU upload can still allocate or copy. Durin must measure its own paths.

UE also uses templates such as `TBuildFunctionFactory` for registration. The
selected change removes product/error templates from the execution contract;
it does not ban small implementation helpers or typed recipe APIs.

## Current Code and Replacement Boundaries

| Current owner/location | Migration |
| --- | --- |
| `DerivedDataCache/Public/DerivedDataBuild.h` | Replace `ExecuteBuild<TAdapter>` and `TBuildObservations<TError>` with non-template session execution and internal counters |
| `DerivedDataCache/Public/DerivedDataBuildDefinition.h` | Split unresolved description from the frozen action; retain canonical field helpers |
| `Engine/Private/Texture/Texture2DBuild.cpp` | Replace local adapter with definition construction, snapshot resolver, registered function and shared-block product assembly |
| `Engine/Private/Texture/TexturePlatformBuild.h`, `TextureCubeBuild.cpp`, `VolumeTextureBuild.cpp` | Remove adapter template; retain shared texture archive helpers and family recipes |
| `Engine/Private/StaticMesh/StaticMeshDerivedDataBuild.cpp` | Separate source resolution, CPU build function and render-data construction |
| `Engine/Private/Physics/PhysicsCookHelper.cpp` | Independent collision function and geometry assembly; retain the non-editor direct cook path |
| `ShaderBuild/Private/ShaderBuilder.cpp` | Replace adapter while preserving source closure verification, compiler ownership, LRU and single-flight |
| `Engine/Private/Asset/AssetDerivedDataBuild.h` | Replace typed observation visitor with execution-local reporting; retain one reporting boundary |

Paths in this table are relative to `Engine/Source/Developer` or
`Engine/Source/Runtime` according to their module. All consumers in Engine,
Sandbox and RoadWeaver declared by `Durin.dworkspace` must be inventoried before
shared API changes. The source table is a starting point, not a consumer list.

## Selected Architecture

### Data and module ownership

`DerivedDataCache` continues to depend only on Core. It owns definitions,
actions, immutable byte inputs/outputs, keyed cache records, function
registration, local execution, cache policy and cancellation/completion
mechanics. Backend Get/Put storage continues to work independently of sessions.

Engine registers the texture, StaticMesh and collision functions, because their
codecs and platform-data contracts already belong to Engine. They call
TextureBuild, MeshBuilder or PhysicsCore through existing interfaces.
ShaderBuild registers its own compiler function. Registration must occur at an
explicit editor/tool initialization point after required recipe modules are
available, including headless Cook and native tests; no static initialization
or Engine-to-Developer link cycle is introduced.

A registry rejects duplicate names and invalid descriptors. Every submitted
request retains an immutable registry entry and the existing owner/module
lifetime guarantee. Shutdown stops admission, cancels/drains requests, releases
callbacks and functions, then unloads producers. Registration is fixed while
requests are active; runtime hot replacement is out of scope.

### Definitions, actions and input resolution

`FBuildDefinition` contains the function name, normalized constants and named
source references. It does not embed a live object, provider callback, recipe
instance or final cache key. Its optional definition hash identifies a request
description, not an interchangeable cache action.

`FBuildAction` contains a frozen registered descriptor/version, constants,
resolved named input identities and representation versions, plus its cache
key. It is immutable after admission. Moving the version from definition to
action provides an actual responsibility difference between these types.

`IBuildInputResolver` has two responsibilities: describe the captured references
for action construction, and resolve their immutable bytes on a miss. For the
existing families the first operation uses already captured metadata, without
payload I/O. A hit must not acquire texture mips, mesh geometry or shader source
contents merely to compute a raw hash. Resolver bindings retain the source
generation until terminal completion, and verify the bytes against the promised
semantic identity when materialized. Wrong identity, missing captured source or
invalid representation fails the request; there is no fallback to current live
asset state or the original import filename.

Resolver output is named `FSharedByteBuffer` data plus representation metadata.
Functions understand their own canonical representations; DDC never interprets
texture mips or vertices. Prepared inputs and their views must own or retain all
borrowed memory. Do not use `std::any`, `void*`, a universal product variant, or
an opaque typed-product side channel to bypass the protocol.

Shader include ordering, virtual paths, macros and entry points remain explicit
versioned inputs/constants. Compiler service instances may be execution services,
but request-specific options may not be hidden in a registered function closure.

### Functions, outputs and cache recovery

A function exposes name/version/configuration and `Build(FBuildContext&)`.
Configuration may estimate scratch memory from metadata. Build consumes only
context constants and resolved data, invokes the existing typed recipe, and
emits complete immutable values. It neither submits owner tasks nor applies
objects. Cancellation is available from the context and remains cooperative.

`FBuildOutput` owns small schema-tagged metadata, named immutable data blocks and
bounded deterministic build messages. There is no mandatory `Primary` archive
of an entire runtime object. Value IDs and required/optional fields are defined
by each family schema; reject duplicates, missing required values, inconsistent
sizes, overflow and target/profile mismatches. Stable IDs encode semantic order,
not allocator or container iteration order.

Keep `FBuildOutput` as the in-memory result consumed by product assembly.
`FCacheRecord` is the keyed persistence representation: record metadata carries
the output schema and deterministic descriptors/messages, and named values carry
the required data blocks. Define bounded conversion between a complete
`FBuildOutput` and one `FCacheRecord`; validate the key, schema, value table and
block integrity when loading. Keep cache timing, backend failures and other
execution-local observations out of both persisted output and record metadata.
Record construction and encoding must not become a prerequisite for returning
an in-memory output.

| Family | Initial in-memory output shape | Consumer responsibility |
| --- | --- | --- |
| Texture2D/Cube/Volume | Dimensions, format and mip/layout descriptors plus owned mip/face/voxel blocks; grouped storage may expose subviews | Assemble platform descriptors retaining blocks; do not decompress GPU texture formats merely to wrap them |
| StaticMesh render | Bounds/section/layout metadata plus vertex/index and other required CPU streams | Assemble render data and perform Engine-owned resource/acceleration preparation |
| Physics | Geometry description plus immutable cooker data blocks in a documented representation | Construct/retain PhysicsCore geometry with the required ownership and alignment |
| Shader | Ordered entry-point/stage/reflection metadata plus bytecode blocks | Assemble compiler output and existing LRU/cooked-library inputs |

The table specifies ownership boundaries, not unverified layouts. Stage 0 must
map each existing representation and freeze its schema. A native cooker blob
may remain a single block when it is the producer's natural output; wrapping
that block must not require serializing an already usable object first.

The local executor owns Get/Put and cache-record integrity validation. A small
family `ValidateOutput` hook checks the metadata/block representation without
source resolution or constructing a disposable runtime object. This hook is a
Durin extension, not a claim about UE's API. Rejected cached output produces one
internal diagnostic and exactly one uncached build attempt. Invalid fresh
output fails. Cancellation during validation never becomes a cache miss.
Consumers run their necessary publication checks; rejection after session
validation is a contract failure, not a second independent cache fallback loop.

### Shared buffer ownership before execution migration

Reuse Core's existing `FSharedByteBuffer::Take`, `Share`, `MakeView` and
`SharesStorageWith`. It currently owns a shared immutable `FByteBuffer` with
bounded subviews. First move compatible producer allocations into this type and
retain them from both output and product. Extend Core only when a measured
producer requires another allocator/alignment/deleter; do not assume it already
supports arbitrary native allocations or add another buffer framework.

Product containers must support retaining immutable storage before sessions
replace their current builder path. Mutable access requires explicit owned
storage or copy-on-write at the mutation boundary; no writable alias may remain
after a block is published. Views retain their backing allocation, never just
a pointer into a worker's stack or scratch storage. Verify alignment, element
lifetime and byte order before exposing typed views; raw casting serialized
bytes is not an ownership or portability solution.

Separate representation data from object application state. DDC must not carry
`std::any`, object pointers or typed product sidecars. Existing recipe APIs may
still use typed values internally, but final large buffers are moved/shared into
the common representation. Release recipe scratch before consumer assembly.
A required layout conversion or native-geometry reconstruction is permitted
only as an explicit, measured family cost, not an assumed zero-copy operation.

### Cold/warm paths, persistence and compatibility

The selected flow is:

```text
cold: recipe -> metadata + owned data blocks -> BuildOutput -> product assembly
                                      \-> optional CacheRecord encoding/compression/Put
warm: cache -> CacheRecord validation/decompression -> same metadata + blocks
                                                -> product assembly
```

Cold execution does not construct a whole product, archive it, then decode it
into another product. Cache encoding may copy/compress bytes for storage, but
its result is not the local consumer's input. With writes disabled there is no
persistence encoding at all. The synchronous persistence path must still return
the original shared blocks; adding background writes is outside this plan.

Failure to generate required mip/geometry/bytecode data is a build failure, as
before. Once complete metadata and blocks exist, cache-record construction,
encoding, compression or Put failure is recoverable and cannot discard the
usable output.
Assembly/allocation failure remains an actual operation failure. This preserves
best-effort cache semantics without a typed-product escape hatch. Cook/package
serialization remains a separate operation with its existing error contract.

Serialize one `FCacheRecord` as an atomic envelope with a value table and blocks,
matching today's Get/Put storage API. The record abstraction does not require a
new multi-key cache, partial fetch or streaming implementation. Bound aggregate
size, validate offsets and integrity before exposing views, and publish all
required values atomically. Group contiguous blocks when appropriate; do not
impose one allocation per mip or arbitrarily split native payloads.

Measure cold/warm time, peak retained bytes, source reads and full-payload copy
counts. Stage 0 sets fixture-specific thresholds before migration. For compatible
texture storage, require shared backing between cold output and assembled
product, including after the session is destroyed. Any unavoidable family copy
must have a named reason and budget. Do not claim warm zero-copy when cache
compression requires decompression, or charge shared backing repeatedly when
estimating unique retained bytes. Cache scratch overlap still counts toward
admission budgets. Regression beyond the frozen limits blocks rollout.

Action canonical schema 2 and cache-record envelope schema 1 intentionally invalidate
current DDC entries once. Keep existing buckets. Per-family build-output schemas
are new internal contracts; existing authored `.dasset` and cooked payload
schemas/bytes must remain compatible through their own serializers. No legacy
lookup, dual write, silent reinterpretation or machine-local key salt is allowed.
Retain source-compression independence and semantic ordering; update key goldens
explicitly and prove package/Cook compatibility separately from DDC invalidation.

### Sessions, dispatch and completion

`FBuildSession` binds the registry, input resolver and local dispatch service.
The asynchronous entry submits a definition and returns an owned request handle;
the handle controls cancellation and retains request state. Completion carries
one terminal status (`Succeeded`, `Failed`, `Cancelled`) and optional output.
It occurs exactly once for admitted work, including shutdown cancellation;
immediate admission rejection returns directly and schedules no callback.
Document that completion can occur inline or on a worker, never implicitly on
the game thread. Session state/resolvers outlive all admitted work.

Use the existing Core task facilities and owner scheduling bridges. Asset
compilation managers retain admission, priorities, reservations and mailbox
publication. Their already admitted worker can execute the session inline;
it must not submit another queued task and block on the same exhausted pool.
The synchronous convenience path drives that same local execution directly.
Neither path pumps object publication or creates a second worker pool.

Cancellation after a cache write may leave a reusable complete entry but cannot
publish a product. Cancellation racing completion is serialized in request
state, with one terminal callback. A callback is invoked without registry or
session locks held. Managers still apply generation/owner checks and report
superseded separately from generic build completion. Shader's existing
single-flight owns waiter cancellation and is not duplicated in DDC.

### Errors and observations

Keep the simplification from `079166382`. Products and asset completion records
must not regain cache warning lists or nested backend/archive error trees.
The session needs only terminal status and a bounded failure description with
an actionable phase/category; cancellation is a status, not a message match.
Deterministic compiler/recipe messages may belong to output. Storage paths,
cache backend failures, timings and trace data stay execution-local and never
enter persisted deterministic output.

Format a family cause once when it crosses the generic execution boundary.
Translate terminal failure into the existing family operation error once on
return. Validate actionable typed input/admission errors before entering the
session where possible. Stage 0 must inventory downstream switches on error
codes or structured fields and choose explicit mappings for every required
behavior; do not replace cancellation/input distinctions with string parsing.
No additional error enum or result wrapper is allowed for each pipeline phase.

Normal misses are silent. Actual cache failures log once with function, action
key and operation; existing counters consume private execution observations.
Cache writes cannot change a successfully constructed output into an operation
failure. Pure recipe code does not log a second copy of session diagnostics.

## Implementation Stages

### Stage 0: Freeze contracts and migration baselines

Dependency: none. Outcome: measurable behavior and error compatibility contracts.

- [x] Record the selected UE-inspired responsibility split and deliberate differences.
- [x] Inventory six producer call chains and all Engine/Sandbox/RoadWeaver consumers.
- [x] Record source representations, required output values, schemas, key goldens,
  typed error consumers, module registration points and shutdown ownership.
- [x] Map existing producer allocations and mutable consumers; freeze each
  metadata/block schema, `FBuildOutput`/`FCacheRecord` mapping, ownership transfer
  and any unavoidable conversion.
- [x] Add fault scenarios distinguishing required recipe output from optional
  cache-record encoding/compression/storage; preserve usable products on cache
  failures.
- [x] Record cold/warm time, peak memory and source reads for representative
  textures, a large mesh, collision and shader batches; include copy counts and
  set rollout thresholds.

Exit: no unresolved ownership/error mapping; baseline receipts and thresholds
are recorded here before implementation of the new shared protocol.

#### Stage 0 inventory (2026-09-27)

The inventory started at `2b610b863`, including its explicit cache-record
boundary, and reflects the subsequent Cube private-source extraction. Searches covered `Engine/Source`, `Engine/Tests`, `Sandbox` and
`RoadWeaver`, the three projects in `Durin.dworkspace`. The six production
`ExecuteBuild` call sites are confined to the files below; the only other
executor consumer is `DerivedDataBuildTests`. `AssetDerivedDataBuild.h` owns
Engine's observation visitor. ShaderBuild consumes observations directly.

| Family and execution boundary | Production callers and downstream ownership |
| --- | --- |
| `Texture2DBuild.cpp`: `BuildTexture2DPlatformData` | `Texture2DCompilation.cpp` owns detached/synchronous builds and application; `TextureCompilingManager.cpp` owns worker admission and cancellation. Texture import, property rebuild, PostLoad and Cook converge through these APIs. Render resources retain platform data. |
| `TextureCubeSourceBuild.cpp`: `BuildTextureCubeSource` | `TextureCube.cpp` uses the captured source for PostLoad/Cook; `BuildTextureCubeDetached` and `BuildTextureCubeSynchronously` serve `AssetForgeBuiltins/TextureCubeImport.cpp` and rebuild. Import normalization precedes execution and is reused on a miss. |
| `VolumeTextureBuild.cpp`: `BuildVolumeTextureWithDiagnostic` | Detached and synchronous APIs serve `VolumeTexture.cpp` and `AssetForgeBuiltins/VolumeTextureImport.cpp`; source preparation is separate from building and object application. |
| `StaticMeshDerivedDataBuild.cpp`: `BuildStaticMeshRenderData` | `StaticMeshBuild.cpp`, `StaticMeshCompilingManager.cpp`, `StaticMeshCook.cpp` and `AssetForgeBuiltins/SceneDirectImport.cpp`. Engine owns material reconciliation, resource assembly, ray acceleration and publication. |
| `PhysicsCookHelper.cpp`: `FPhysicsCookHelper::Cook` | `StaticMeshCompilingManager.cpp` and `StaticMeshCook.cpp`; collision source capture is independent of the render build result in production. Keep the non-editor direct cook branch. |
| `ShaderBuilder.cpp`: `ExecuteDerivedBuild` | Mounted/generated compiler entry points and `ShaderLibraryProducer` own compilation and cooked-library production. `IShaderBuildModule` serves RenderCore `ShaderMap.cpp` and `ShaderBuildRequests.cpp`; Renderer/material consumers use those boundaries. LRU and single-flight remain in ShaderBuild. |

Sandbox and RoadWeaver have no direct executor, definition, product-buffer or
family build API consumers. Their downstream use is authored/cooked object
loading and components: Sandbox `PlayerPawn` loads `DStaticMesh` and owns a
`DStaticMeshComponent`; RoadWeaver `RoadNetActor` installs a preview mesh.
Their integration build gates remain required after a shared Engine API change.
Test consumers are owned by DerivedDataBuildTests, TextureTests,
TextureImportWorkflowTests, SceneImportTests, StaticMeshTests, PhysicsSceneTests,
CookFunctionalTests, CookedMeshLoadingTests and the RenderShader test targets;
the qualification targets below do not replace that behavior coverage.

#### Representation and ownership findings

| Family | Captured input and existing allocation | Required output and migration constraint |
| --- | --- | --- |
| Texture2D | Torn-off `FTextureSource`, semantic `TextureSource` identity/schema 3; `Texture2D.RGBA8` representation 1. Miss resolution uses `FImage::TryCreate` from captured mip bytes. | Pixel format and ordered width/height/row-pitch descriptors, plus each mip's `FByteBuffer Pixels`. Transfer these byte vectors through `FSharedByteBuffer::Take`; product and output must retain the same allocation. |
| Cube | Schema-3 source, six RGBA8 faces or retained RGBA32F long-lat panorama. Import can reuse `FTextureCubeCanonicalBuildInput`; HDR projection/version/exposure remain explicit. | Six ordered face chains, format and mip descriptors. Existing faces contain the same mutable `FTexture2DMipData` as Texture2D. Face order must match the current archive/RHI order. |
| Volume | Schema-3 captured volume source; dimensions, portable format and Box filter. Resolution currently calls `Voxels.UpdatePayload` and can copy source bytes. | Format, width/height/depth, row/depth pitches and ordered `FByteBuffer Voxels`. Output ownership can move; source normalization/copying is a separate measured cost. |
| StaticMesh | Semantic source geometry identity plus reconciliation identity; `AcquireGeometry` retains decoded geometry. Recipe arrays are typed vectors. | Bounds, material-slot count, LOD/section layout, positions, normals, tangents, UVs, optional colors and indices. Current assembly moves vectors into render buffers; Core's byte-vector owner cannot take arbitrary typed vectors. A byte-oriented recipe allocation or one explicit conversion is needed before claiming shared output storage. |
| Collision | `FCookBodySetupInfo` owns float positions and indices. Current code hashes their canonical contents before cache lookup. | Kind, mode/policy, bounds, vertices, triangles/source ordinals and triangle BVH/leaf data; convex representation must also preserve hull topology. PhysicsCore currently returns an opaque immutable geometry reference, not a native cooked byte blob. Extracting an archive from that object and reconstructing it on the cold path is prohibited. |
| Shader | Variant identity v6, captured source closure, ordered entry points/frequencies, normalized macros and compiler environment. | Ordered entry-point, stage, hash and reflection descriptors plus bytecode. `FCompiledShader::Code` is currently `shared_ptr<FByteBuffer>`; Slang copies its blob into this buffer and then copies into `vector<uint32>` for reflection. Freeze mutable ownership before sharing bytecode; count those two existing conversions separately. |

Writable consumers requiring migration include texture/volume recipes and
archive loading, StaticMesh render resource initialization and
`GetMutablePositions`/`GetMutableColors`/`GetMutableIndices` test and codec
callers, and Shader compiler/decoder bytecode construction. `FSharedByteBuffer`
only retains a byte-vector allocation or its bounded subviews. It does not
prove typed alignment, element lifetime, or ownership of a PhysicsCore native
allocation. The later typed-storage experiment below justifies vector-allocation adoption;
it does not justify arbitrary runtime-object backing or borrowed typed views.

The existing authored/cooked schemas remain TXPL 2, DMSH 5, collision 2 and
shader compiled output 1. The new family output schemas must be separate from
these serializers. Existing key goldens to preserve until each action migration:

| Fixture | Existing key |
| --- | --- |
| Texture2D, `TextureDerivedDataTests.cpp` | `ea4147f928e24dd218363febc4385e12` |
| Cube six-face / panorama, `TextureDerivedDataTests.cpp` | `3a8c771aab669bbdac6874ba34c3c5ab` / `8fb682cb2f8e7294c71edd27f49bd59b` |
| Volume, `TextureBuildTests.cpp` | `ccb233b1772bcb6c91f88f55c4e82873` |
| Render, `StaticMeshDerivedDataContractTests.cpp` | `cebb7c8969004fb632d19bcd999d02d8` |
| Collision, `StaticMeshDerivedDataContractTests.cpp` | `71216e62310fc31b71a7695ad7d7d4a9` |

Shader action-key golden: `34e7cc7d296f3588e078eec6cd12b469` for the
`ShaderDerivedDataTests` two-entry fixture and fixed variant
`00112233445566778899aabbccddeeff`. The separate compiled-output byte golden
remains payload-format coverage.

#### Cache-record mapping and unresolved contract work

The mapping is one admitted action key to one complete record. Copy schema-tagged
metadata, ordered deterministic messages and value IDs from the output; retain
their shared blocks when constructing the record. Loading must validate the
requested key, schema, bounded metadata/value table, offsets, sizes and hashes
before producing output views. Neither direction interprets family objects.
Record construction is skipped with disabled writes; construction, encoding,
compression and Put must each have an independent recoverable-failure scenario.
The current executor has no record-construction or compression injection seam;
its encoding-failure tests cannot establish those future cases.

The texture representation is selected for Stage 1: separate schema names
`Texture2D.Output`, `TextureCube.Output` and `VolumeTexture.Output`, each version
1. Metadata uses canonical fixed-width little-endian integers, never a native
struct image. The header contains target platform/profile, pixel format and mip
count; descriptors contain `uint32` width/height/row pitch, with additional
`uint32` depth/depth pitch for volumes. Cube descriptors are face-major in the
existing six-face order, with identical mip counts and formats across faces.
Each descriptor names exactly one required value: `Mip/<decimal-index>`,
`Face/<decimal-face>/Mip/<decimal-index>` or `VoxelMip/<decimal-index>`.
No leading zeros or alternative spellings are admitted. IDs contain semantic
indices, not hash-map iteration positions. Metadata also declares each block's
`uint64` byte count; the validator recomputes the expected layout with checked
arithmetic and rejects missing, extra, duplicate or incorrectly sized blocks.
Use the current family limits (2D 16,384, Cube 4,096, Volume 2,048 per axis,
at most 32 mips and 2 GiB total payload), the existing portable format mapping,
and the existing mip-chain validity rules. Unknown schema versions and invalid
target/profile combinations are semantic cache rejection. This is independent
of TXPL 2; package serialization remains at that existing boundary.

Texture recipes initially retain their mutable byte-vector construction. On
successful recipe completion, move every vector into `FSharedByteBuffer::Take`
and retain the same block in output and product descriptors. Package loading
uses a temporary mutable vector before freezing it. Any remaining mutation
consumer must explicitly construct owned writable bytes; no mutable shared
alias is exposed. Product assembly performs descriptor validation and retains
blocks, with no payload archive or GPU-format conversion. Tests must retain
products after the output and recipe temporaries are destroyed, including
subviews into a grouped backing allocation. The qualification observer now
establishes the pre-migration zero-copy transfer baseline for all three families.

Common output bounds are selected as 4 MiB of immutable metadata, 4,096
uniquely named values, 96-byte schema/value IDs, and at most 128 deterministic
messages of 4,096 bytes each. Family limits and the admitted aggregate-byte
budget apply in addition; these are ceilings, not allocation requests. Large
section/reflection tables use dedicated values as specified below. Every table
count, multiplication, offset and total must be checked before allocating or
creating a view. Record conversion retains metadata/value owners; only the
optional envelope encoder flattens them for the existing atomic Put API.

The typed-storage experiment now selects a narrow extension to Core's existing
`FSharedByteBuffer`, rather than introducing another buffer class. Add adoption
of moved, trivially copyable `std::vector<T>` allocations and a checked native
array view. Retain the vector's control block through an aliasing immutable byte
owner; preserve element-type provenance, alignment, element count and lifetime.
A typed view succeeds only for the original element type and a whole-element
subview. Equal size/alignment alone is insufficient. Serialized byte owners have
no native-type provenance and must never pass this check. Do not accept arbitrary
runtime objects or use a product object as the backing owner. The common DDC
representation still exposes named bytes; native provenance is local storage
information, never serialized metadata or a cache-key input.

On a cold mesh/collision result, adopt native arrays and retain the same blocks
in consumers. On a cache hit, retain record byte subviews until assembly, then
construct each required native vector exactly once and copy/decode its elements
before freezing it. That one warm conversion is explicit; no temporary mesh or
collision object is built for validation. Persist only documented fixed-width
little-endian layouts. A native layout can serve as its canonical bytes only
with compile-time size/offset/IEEE-format/endianness checks; otherwise use an
explicit codec for that platform. No reinterpretation of arbitrary serialized
storage is admitted. Mutable mesh accessors must detach into owned vectors
before writing and leave prior shared readers valid.

Selected non-texture schemas, each version 1:

| Schema | Metadata and required blocks |
| --- | --- |
| `StaticMesh.RenderOutput` | Target/profile, double-precision bounds, material-slot count, LOD count; each LOD carries screen size, bounds, vertex/index counts, UV-channel count, color-presence flag and section count. A required `LOD/<n>/Sections` block contains fixed-width first/count/min/max/material indices plus double-precision bounds, avoiding large section tables in common metadata. IDs `LOD/<n>/Positions`, `Normals`, `Tangents`, `Indices`, `UV/<channel>` and optional `Colors` have the same LOD prefix. Layouts are f32x3, f32x3, f32x4, u32, f32x2 and f32x4 respectively. Require one block for every active stream, no block for absent colors/inactive UV channels, matching vertex counts and triangle-list indices. Preserve `MaximumStaticMeshLODs`, `MaximumStaticMeshVerticesPerLOD`, `MaximumStaticMeshIndicesPerLOD`, `MaximumStaticMeshSectionsPerLOD`, `MaximumMeshMaterialSlots` and working-set admission. Material object bindings and regenerated display names remain outside output. |
| `Physics.CollisionOutput` | Target/profile, source mode/query policy, presence of Simple/Complex geometry, kind, double-precision bounds and array counts. Prefix IDs with `Simple/` or `Complex/`: `Vertices` is f64x3; `Triangles` is four u32 fields including source ordinal; triangle meshes require `Nodes` (f32x3 minimum/u32 first/f32x3 maximum/u32 count-or-second) and `LeafTriangles` (u32). Convex hulls require `Planes` (f32x3 normal/f32 distance), `HalfEdges` (origin/twin/next/face u32), and `Faces` (first/count/source/reserved u32). Keep 256-vertex convex and 2,000,000-triangle limits, existing cleanup, BVH-depth, index/finite/bounds and hull-topology checks. No blocks belong to absent geometry. Runtime geometry identity is newly allocated, never serialized. |
| `Shader.Output` | Ordered entry points, frequencies, binary entry names, bytecode hashes and reflection descriptors. IDs `Entry/<n>/Code` contain immutable SPIR-V bytes; required `Entry/<n>/Reflection` blocks encode the existing binding name/stage/set/binding/type/array size and push-constant ranges canonically. This separate descriptor block prevents large reflection tables from becoming unbounded common metadata; it never contains bytecode or a serialized compiler output. Debug names remain reconstructible caller diagnostics. Preserve 32 entry points, 64 MiB per code block, 256 MiB total, 65,536 reflection entries, 32,768-byte strings, descriptor-index and push-constant limits. Validation uses ordered request metadata and SPIR-V alignment/header checks, without compiling. |

PhysicsCore must split cooking from geometry publication: finish owned arrays,
including convex topology, before emitting blocks; construct the final immutable
geometry from those blocks afterwards. Do not extract arrays from an already
published geometry on the production cold path. The qualification snapshot does
so only to measure the current native element layouts, and reports its setup
copy separately. Keep current float-input to double-vertex conversion inside
the cooker and count it separately from representation transfer. ShaderBuild
moves its completed bytecode vector into the shared block, and all `Code`
consumers retain immutable storage. The existing Slang-blob and reflection-word
copies are recipe costs; for the measured 4,192-byte batch they account for
8,384 explicit copy bytes by inspection of the two `memcpy` boundaries, not by
allocation sampling. They are not justification for another output assembly copy.

Selected error compatibility boundary: the generic terminal failure carries
phase/category and one bounded, already formatted description (4,096 bytes).
For compatibility it also carries an optional producer code and an optional
in-process diagnostic identity. These are scalar values opaque to DDC, not a
family error variant, serialized error tree, object pointer or callback sidecar.
Cancellation is a distinct terminal status and is mapped before any code field.
A missing/invalid producer code maps to that family's generic operation failure.
Do not reconstruct typed causes by parsing the description.

| Family | Admission and terminal mapping |
| --- | --- |
| Texture2D | Validate metadata/settings/availability before admission and return existing typed input errors there. A byte-resolution failure remains InvalidInput with a preformatted diagnostic; recipe codes map back to the existing `ETexture2DBuildError`, while cancellation always becomes `Cancelled`. Extend the family formatter to accept the already formatted boundary description without rebuilding an archive/backend cause tree. Detached completion retains Failed/InvalidInput/Canceled. |
| Cube/Volume | Keep typed normalization before admission where possible. Generic input/category failure maps to `ETextureBuildFailure::InvalidInput`; recipe/output failures map to the existing builder-product failure; preserve the stage and bounded diagnostic. Cache issues never enter the product or detached completion. |
| StaticMesh/Physics | Preserve `IsCancelled()` and phase-to-stage mapping: resolution to Source/Input, recipe to Render/Cook, fresh validation to Validation/Cook. Scheduling/application/resource failures remain owner-side. Transfer the bounded message once, without reformatting. |
| Shader | Preserve typed capture/closure/admission errors before the session. At the generic boundary preserve the original `EShaderError` scalar, `FormatShaderError` text and `GetSemanticFingerprint()` identity. On return use an explicit preformatted-diagnostic path whose fingerprint returns that identity; do not recreate discarded structured causes. Existing direct compiler/capture APIs retain their structured fields. SlangFailure, cancellation and input codes remain typed. RenderResourceCreation's diagnostic grouping continues to distinguish compiler phase/native status through the preserved identity without depending on external wording. |

The Shader mapping follows the source/test consumer audit: production consumers
format the error and call `GetSemanticFingerprint`; direct `CompilerPhase` and
`NativeStatus` interpretation lives in those helpers. Builder regression tests
also require `SlangFailure` and pre-admission `ImportNotAllowed` codes. Add
boundary tests for identical semantics with changed external wording and for
changed native status/phase; existing direct-field diagnostic tests must remain
unchanged. This explicit mapping extends the generic failure with two optional
scalars to preserve observed behavior without reintroducing nested errors.

Remaining contract and baseline work:


- Validate complete native layout/provenance and per-family metadata budgets
  against all existing producers, including convex topology and large reflection
  tables. Exercise the selected terminal mappings at their migration boundaries.
- Add and validate the selected bootstrap ordering below when sessions land;
  metadata registration alone does not establish provider availability or drain.

Selected bootstrap: one explicit Engine service initialization registers its
stateless texture/render/collision functions after TextureBuild and MeshBuilder
are loaded. Launch's editor PreInit must load those providers and initialize
this service before authored compilation admission/DObject initialization.
DurinAssetTool initializes it after its existing provider loads; its scoped
editor-service destructor first closes asset compilation, then drains build
sessions, then shuts down the task system and object services. EngineLoop Exit
likewise closes asset compilation, closes session admission and drains sessions
before optional-provider/module teardown, including failed-startup exits.
ShaderBuild Startup registers its compiler function before constructing the
builder; Shutdown closes/drains shader sessions before releasing the builder
and registration. Native fixture roots explicitly initialize/drain the same
services around provider lifetime. Registration must reject conflicting
name/version descriptors, and a registration handle cannot unload a callable
function while sessions retain it. No Game entry point loads authoring providers
or requires these services. This order is selected from the current Launch,
AssetTool and ShaderBuild roots; failure/partial-startup tests are still required.

#### Required fault matrix

The migration tests must distinguish these boundaries. Only the old-executor
rows identified in the receipts below are implemented so far; this table does
not claim that the new record/session machinery has been tested.

| Injected event | Required observable result |
| --- | --- |
| Input resolution or recipe failure | No product, no record or Put attempt; preserve the mapped phase/category and cancellation distinction. |
| Fresh output validation failure | No product and no persistence; exactly one recipe invocation. |
| Cached record integrity or family semantic rejection | One diagnostic, then exactly one uncached recipe attempt; a second rejection fails without a loop. |
| Cancellation during cached validation | Cancelled terminal status; zero source resolution, recipe or fallback calls. |
| Record construction rejected by persistence budget/policy | Return the already valid output with the same backing; zero encoding/compression/Put calls. |
| Envelope encoding failure | Return the same output; zero compression/Put calls. |
| Optional compression failure | Return the same output; zero Put calls and no duplicate fallback/recipe work. |
| Atomic Put failure, including a real filesystem failure | Return the same output; one write observation and no second recipe invocation. |
| Writes disabled | Zero record construction, encoding, compression and Put calls; retain output after session/context destruction. |
| Completion callback races cancellation/shutdown | Exactly one terminal callback; no published partial output and no retained function after registry/provider teardown. |

#### Fault and measurement receipts

`DerivedDataBuildTests` passed 18 cases on macOS arm64 Debug, including four
new cases: cached semantic rejection rebuilds exactly once; cancellation during
cached validation never rebuilds; a regular file at the cache root causes a real
storage failure without losing the original product allocation; optional encoding
failure and disabled writes preserve that allocation without cold decode.
Existing source/recipe/fresh-validation failure and checkpoint cancellation
coverage remains in the same suite. This characterizes the old execution
boundary; it does not claim that `FCacheRecord` exists yet.
All 18 cases also passed with `--isolate --test-jobs 1`. Each of the three
texture qualification cases passed independently as well as in its target suite;
the shader batch and new mesh/collision case passed independently. Changed-doc
validation and all-plan lifecycle validation passed. `test affected --explain`
expands test-CMake edits to all Engine native targets; bounded owning-target
coverage was used for this test-only change. Production behavior and shared
Engine APIs are unchanged, so this receipt is not the later `all` build gate.

Qualification receipts use `MacOS-arm64-Debug-DurinEditor`, Tracy enabled,
bounded two-worker test scheduler and process-local cache roots. Timing is
diagnostic absolute measurements, not standalone pass/fail targets: trace/debug sink I/O is suppressed
for the end-to-end fixtures, but exclusive host load has not been established.
Background CPU contention was observed during this batch. No performance gate
is closed. The common macOS sampler measures the process default malloc zone
every 1 ms, excluding its own thread startup from the baseline; it can miss
short-lived allocations and non-zone memory. Peak increases below are maxima
across three measured rounds, not exact request peaks or admission estimates.

| Fixture and command | Recorded result | Scope limit |
| --- | --- | --- |
| `test StaticMeshBuildQualificationTests --mode qualification --report` | Four cases passed. 100,000 triangles: canonical source 4,800,153 B; decode/render 903.340 ms; ray build 559.282 ms / 2,759,296 retained B; collision 1,011.332 ms / 10,248,808 retained B. | Existing no-write fixture; single timing sample, not a cold/warm cache comparison. |
| Same target, source-residency fixture | 100,000 triangles: sampled default-zone peak 138,704,128 B; canonical source 4,800,147 B. | Process-wide 1 ms sampler, not exact request peak or unique block accounting. |
| `test TextureCompressionQualificationTests --mode qualification --report` | One warm-up and three samples: 1024-square BC1/BC5/BC7 parallel compression medians 326.997 / 1,373.140 / 813.961 ms; intermediate bytes 1,398,100; output bytes 699,064 / 1,398,128 / 1,398,128. | Recipe-only compression, not full build/cache latency. |
| `test DerivedDataTextureQualificationTests --mode qualification --report` | Three cases passed, each with one warm-up and three measured cold/warm/no-write rounds. BC7: 891.449 / 20.647 / 865.749 ms, 1,398,128 output B; Cube: 378.694 / 24.690 / 373.582 ms, 262,224 B; Volume: 12.621 / 4.529 / 6.664 ms, 299,593 B. | Includes output hashing/destruction and Cube normalization. The observing module forwards real recipes and compares returned block addresses/sizes against final products: zero post-recipe payload-copy bytes for all three families. This does not count recipe scratch or persistence copies. |
| Same texture target, source and allocation probes | Texture2D and Volume resource requests: cold 1, warm 0. Sampled zone increases cold/warm/no-write: BC7 4,231,472 / 2,809,008 / 2,811,872 B; Cube 4,473,120 / 4,473,120 / 4,473,120 B; Volume 928,048 / 612,912 / 306,912 B. | Source probes use package-backed retained bytes with source residency released before each interval. Cube uses the normalized import path and explicitly reports no source probe; zero source counts there are not evidence of no reads. |
| `test ShaderBuildQualificationTests --mode qualification --report` | Eight vertex shaders, one warm-up and three measured batches: cold 46.522 ms, fresh-builder DDC 2.529 ms, LRU 0.851 ms; 4,192 bytecode B. Fingerprint content reads: cold 8, DDC/LRU 0. Sampled zone increases: 76,429,040 / 16,208 / 2,352 B. | Builder construction/destruction is outside timing; counters distinguish eight compiles, eight DDC hits and eight LRU hits. Bytecode copy instrumentation remains open. |
| `test StaticMeshBuildQualificationTests FStaticMeshBuildQualificationTests.ColdAndWarmRenderAndCollision --mode qualification --report` | One warm-up and three measured pairs, 100,000 triangles: render cold/warm 1,940.150 / 857.931 ms; collision 1,637.970 / 601.540 ms. Source 4,800,147 B; collision retained 10,248,808 B. Render requests: cold 1, warm 0. Sampled zone increases: render 104,606,560 / 74,533,136 B; collision 51,939,888 / 44,617,632 B. | Source probe serves retained bytes through the package-resource API; it is not disk latency. Collision starts with independently captured arrays and includes existing pre-lookup content hashing. |

The captured six-face Cube path also passed in the four-case texture target:
cold/warm/no-write medians 340.919 / 4.187 / 334.071 ms, 262,224 output B,
source requests 1 / 0, and zero recipe-to-product copy bytes. Sampled zone
increases were 2,402,384 / 546,512 / 1,935,008 B. This calls the internal
captured-source boundary used by PostLoad/Cook, with no prepared import input.
The qualification target compiles the Engine-owned captured coordinator with
explicit `PRIVATE_SOURCE_OWNER Engine` metadata. This avoids relying on macOS
default symbol visibility or exporting a private function solely for tests. The
coordinator was moved verbatim from `TextureCubeBuild.cpp`; production callers
and behavior are unchanged.
The HDR panorama case also passed independently: 512 by 256 linear RGBA32F
projected to 128-square faces, cold/warm/no-write 459.914 / 30.593 / 421.374 ms,
2,097,120 output B, source requests 1 / 0 and zero post-recipe copy bytes.
Sampled zone increases were 8,435,120 / 4,214,352 / 5,777,184 B.
The separate import fixture remains useful because its normalization costs are
real caller costs. These inputs use different gamma settings and are not an
output-hash equivalence comparison.

The final five-case qualification target and the existing 125-case TextureTests
suite passed after extraction. Both captured Cube cases also passed when
selected independently. The extracted coordinator body was compared against
its prior committed body and is unchanged. Windows execution is not verified
on this host.

The native-array ownership experiment uses the actual 100,000-triangle render
recipe product. Its streams total 27,600,000 B; explicit byte-vector then native
reconstruction copies 55,200,000 B, with a maximum sampled per-stream increase
of 9,601,072 B in isolation (13,615,152 B in the full target). Collision arrays total 10,248,544 B; the same conversion copies
20,497,088 B, with a maximum sampled increase of 15,220,784 B in isolation
(17,629,232 B in the full target). The four-point convex fixture contributes
480 B across vertices/triangles/planes/half-edges/faces and 960 B of explicit
round-trip copies; all native element types pass the same ownership checks. Aliasing byte
ownership preserves the original aligned element addresses and element
lifetimes after the vector owner is released, with zero transfer copy bytes.
The test also verifies final consumer release destroys the native owner. These
are representation experiments, not a claim that production Core already adopts
typed vectors or that PhysicsCore already exposes transferable arrays. The
collision fixture's separately reported snapshot copy is outside the experiment.
All six StaticMeshBuildQualificationTests cases passed with the experiment and
convex extension. The final experiment including convex coverage also passed independently. All
six RenderShaderCacheTests passed with the action-key golden. Per-stream sampled
peaks vary with process activity, as shown above.

#### Frozen copy accounting and rollout gates

Decision clarified for the Stage 0 exit: the fault matrix freezes scenarios
before the new machinery exists. Its executable record-construction/compression
and session lifecycle cases belong to Stages 1/2; the existing executor's
required-failure, encode/storage-failure and no-write cases provide the current
behavior baseline. This does not waive any later fault test.

Logical copy accounting counts complete payload transfers between named owners,
not vector-capacity growth, allocator internals or filesystem/kernel copies.
Process-zone samples above capture observed overlap including those other
allocations. Let P be the admitted active output bytes and E the encoded cache
value size. The following boundaries were checked in the current serializers:

| Family | Existing persistence/assembly transfers and overlap |
| --- | --- |
| Texture2D/Cube/Volume | Cold recipe-to-product 0 (measured). TXPL saving copies P into its Body, Body into the outer archive, then E into the backend envelope: three payload-bearing writes. Warm parsing borrows record regions and copies P once into final mip vectors. Cold persistence therefore overlaps the usable product with encoding buffers; warm assembly overlaps the loaded cache allocation with final mip storage. No-write removes all three persistence writes. |
| Mesh | Cold recipe-to-render transfer 0 by vector moves. `MakeStaticMeshPayloadData` copies active streams once, the chunk serializer writes those streams and its compressed/uncompressed chunks, and Put copies E. Warm decoding creates payload vectors, then `MakeStaticMeshRenderData` copies them into final buffers. The 27.6 MB native fixture includes initialized inactive UV/color arrays; only active streams enter the persistence copy count. These are distinct from source decoding and ray-acceleration allocation. |
| Collision | Cooking already converts float input to double vertices. `MakePhysicsCollisionPayloadData` converts vertices back to float and copies index/ordinal/node/leaf arrays. Saving constructs a temporary native geometry for validation before writing/reordering streams; loading also constructs a validation geometry, then the caller constructs the returned geometry. Each native reconstruction includes double vertices and copied topology. Native data in the triangle fixture is 10,248,544 B; temporary vertices alone are 7,200,000 B, plus ordinal-remapping/tree scratch. The new representation must remove these disposable validation geometries, not merely hide them in record conversion. |
| Shader | The measured 4,192-byte batch has two recipe bytecode copies (Slang blob and reflection words: 8,384 B). Cold output/LRU transfer retains bytecode owners. Persistence writes bytecode into the encoded output once and Put copies E. Warm DDC decoding copies bytecode once; LRU retains the same owners. Source closure capture remains separately accounted. |

These explicit boundaries, native-array experiment and sampled peak receipts
are the baseline, with no claim of an exact whole-process copy counter. New
representation transfers must be zero for cold compatible output blocks; a
warm native-array conversion may copy each required element once, with no
intermediate whole-product encode/decode. Texture and shader warm assembly must
retain compatible shared bytes. Disabled writes must execute zero persistence
operations. Warm package-backed texture/mesh requests remain zero, shader warm
compile/content-read counters remain zero, and collision content hashing moves
to capture rather than being repeated merely to form an action.

Freeze these numerical comparison gates for the existing fixtures and profile:
median cold/warm/no-write time at most 1.15 times the recorded baseline for that
path; sampled peak increase at most baseline plus the larger of 10% or 2 MiB.
The allowances exceed the observed repeated timing spread and small-allocation
noise, but do not permit an added whole payload copy. For mesh/collision the
native-copy ceilings above apply even when a sampled peak misses a short-lived
allocation. Any structural regression requires investigation and a documented
decision, not silently widening thresholds.

CPU qualification does not require the exclusive GPU lane specified for GPU
timing. Prior receipts remain diagnostic absolute measurements on this host;
acceptance uses a paired run of frozen pre-protocol commit `8989d1310` and the candidate on
the same host/profile, with identical warm-up/sample counts and no concurrent
build/test. If background load makes the comparison unstable, repeat that pair
before accepting the migration. No Windows performance claim is inferred from
macOS. This freezes a reproducible comparison method and thresholds now rather
than treating ordinary host activity as an indefinite implementation blocker.

### Stage 1: Establish metadata and shared-block products

Dependency: Stage 0. Outcome: proven ownership and assembly without a new executor.

- [x] Define bounded immutable metadata/value containers, the keyed
  `FCacheRecord` contract and per-family output-to-record mappings; test IDs,
  layouts, required values, integrity and aggregate limits.
- [x] Adapt Texture2D output/product storage to retain common blocks using Core's
  existing shared buffer. Validate producer-to-output-to-product ownership first.
- [x] Test destruction of producer/session-independent fixture state, subview
  lifetime, cancellation cleanup, alignment and explicit mutable access.
- [x] Prove cold assembly does not require archive encode/decode and does not
  duplicate compatible mip buffers. Test cache encoding failure independently.
- [x] Preserve existing synchronous/async builder behavior and package/Cook
  bytes while making the representation usable by the future session.

Foundation receipt: `DerivedDataBuildOutput.h/.cpp` now defines bounded immutable
`FBuildOutput` and explicit keyed `FCacheRecord` conversion. Value IDs are sorted
and checked for duplicates; metadata, table, message and aggregate limits are
validated before publication. Record creation copies only descriptors and retains
metadata/value blocks; return conversion verifies the requested key and block
hashes, then retains immutable record state. A stricter persistence budget can
reject a record without changing the original usable output. No family product,
Engine dependency, archive or session is needed by this boundary.

The seven new representation cases and 18 existing executor cases passed as one
25-case target and in serial per-case isolation. Coverage includes retained
subviews after producer/output/record destruction, invalid identifiers/messages,
independent limits, optional construction rejection, wrong action keys and
metadata/value integrity after a deliberately violated caller immutability
contract. The module manifest still declares only Core. This initial receipt did not
cover byte encoding/compression; that coverage is recorded below. Session fault
injection remains a Stage 2 requirement.

Texture2D storage receipt: completed mip bytes now use `FSharedByteBuffer`.
Compression and Cube panorama projection freeze owned writable allocations only
after work drains. Cube shares this mip type, but retains its existing execution
path. Texture2D publishes bounded little-endian layout metadata plus `Mip/<n>`
blocks, drops the recipe's platform container, and assembles by retaining the
same blocks. Semantic validation reads descriptors without constructing a
disposable platform product. Legacy DDC persistence still uses TXPL until Stage
2; this storage change does not claim production warm-cache zero-copy.

Three new tests cover grouped subview lifetime after producer/output/record
destruction, explicit mutable copies, malformed family layouts, optional record
budget rejection and unchanged Cook bytes. They passed individually in serial
isolation and within the 128-case TextureTests target. The shared Engine API
`all` build passed, followed by all 88 affected test targets with two scheduling
slots, including import, asynchronous lifecycle and Cook coverage. An omitted
AssetCookTests fixture was migrated after its compiler diagnostic; a sandboxed
test-discovery timeout was resolved by rerunning outside the sandbox after
confirming no remaining build processes. The modified SkyBox and Thumbnail
Vulkan targets also compiled; a fixture-local variable collision found in the
latter was corrected. GPU qualification was not executed.

Paired CPU qualification on 2026-09-27 compared `8989d1310` with this storage
migration using the same macOS arm64 Debug/Tracy profile, one warm-up and three
measured rounds, sequentially with no overlapping build/test. Texture2D
cold/warm/no-write medians were 865.556/21.6152/841.836 ms before and
888.255/21.5358/861.155 ms after (ratios 1.0262/0.9963/1.0229). Sampled zone
increases were 4,231,568/2,809,008/2,811,712 B before and
4,232,144/2,809,728/2,812,480 B after. All five texture fixtures passed the
1.15 time and peak-increase gates; the maximum ratio was 1.0267 and maximum
peak-increase delta was 3,360 B. Output hashes were identical, measured cold
recipe-to-product copies remained zero, and all package-backed warm source
request counts remained zero. The normalized Cube fixture has no package source
probe. These receipts establish the storage comparison, not the future session
or cache-record encoding/compression fault gates.

Exit: representation tests and Texture2D baseline comparisons pass. No new
session is required to demonstrate the storage improvement; no typed object
side channel or whole-product serialization round trip is introduced.

Record codec selection: envelope schema 1 uses raw format 0 with a canonical
value-offset table, per-block hashes and a complete-envelope hash. Optional
format 1 compresses that complete record with the repository's pinned Zstd
library; DerivedDataCache links it privately while its only module dependency
remains Core. Encoding and compression are separate fallible operations so the
future session can skip both when writes are disabled and observe their failures
independently. Decoding bounds both stored and inflated record sizes and accepts
only one compression layer. Uncompressed records expose retained byte views;
compressed records retain the single decompressed allocation. This is cache
record persistence and does not change package/Cook encoding.

Record codec receipt: eight additional representation cases bring
DerivedDataBuildTests to 33 cases, passing as a whole target and in serial
per-case isolation. Coverage includes deterministic raw encoding, every
truncation and single-byte corruption of a fixture, authenticated malformed
tables, logical/encoded limits, empty output, one-byte-aligned backend subviews,
shared lifetime after backend/record release, actual backend Get/Put and a
filesystem write failure. Optional compression failures leave the original
output blocks intact; successful decoding retains one inflated allocation and
rejects false sizes, invalid frames and trailing frame bytes. Session-level
fault injection and zero-operation counters remain in Stage 2.

TextureTests now passes 129 cases, including raw/compressed cache record to
Texture2D assembly with identical Cook bytes and retained mip addresses. Its
new round-trip case also passed in isolation, and the `all` build passed after
the private Zstd linkage change. Its
existing worker-cancellation test drains compression and observes an empty
platform result. Together with weak-owner destruction and explicit mutable-copy
fixtures, this closes the Stage 1 byte-buffer ownership checks; native typed
alignment/adoption remains the Stage 4 responsibility. The DDC module test set
covers both cache targets and both shader consumers. Sandboxed test discovery and shader-cache launches failed inside macOS dyld
before main (confirmed from crash reports). The unchanged shader-cache target
passed alone, and the complete four-target set passed outside the sandbox with
two scheduling slots. This is recorded as an environment limitation, not a
codec test failure.

### Stage 2: Introduce sessions and migrate Texture2D

Dependency: Stage 1. Outcome: one production family proves the full boundary.

- [x] Add definitions/actions/context and explicit registry; implement metadata
  resolution, action keying, `FBuildOutput`/`FCacheRecord` conversion, bounded
  record encoding and non-template execution on the existing Get/Put backend.
- [x] Implement semantic validation, one rebuild, cooperative cancellation and
  exactly-once completion with fixture functions. Test malformed values/schema,
  descriptor conflicts, source mismatch and no-read warm hits.
- [x] Provide injected local dispatch and inline synchronous execution; test
  one-worker saturation, reentrant completion and shutdown drain.
- [x] Register Engine's Texture2D function, resolve snapshot blocks and invoke
  the pure recipe; assemble products from the Stage 1 representation.
- [x] Route import, async compilation, PostLoad/rebuild and Cook through the
  session. Test output lifetime after completion, record round trips,
  no-write/no-encode behavior, recoverable persistence failure, stale owners
  and cancellation.
- [x] Compare time, peak memory and copy counts to Stage 0. Remove the Texture2D
  adapter. New execution must not delegate to `ExecuteBuild<TAdapter>`.

Foundation receipt: legacy schema-1 definitions are now explicitly named
`FLegacyBuildDefinition` throughout all declared projects' source/test roots.
Their canonical encoder and family key goldens are preserved. New
`FBuildDefinition` stores only function name, constants and captured-source
locators. `FBuildAction` freezes descriptor versions and resolved identities,
validates exact source-name bindings and uses canonical schema 2 with the
`Durin.DerivedData.BuildAction` domain tag. Locators do not enter the action key.
The fixture golden is `5046b215c7c0a6e98d4bd4a0c5c70264`.

Explicit registration validates and freezes descriptor copies, rejects duplicate
names/conflicting versions and prevents post-freeze registration. Snapshots and
retained entries keep functions alive independently of registry lifetime.
Core-only resolver/function/context interfaces expose immutable named byte
blocks and a generic failure type with 4,096-byte description normalization,
optional producer code and semantic diagnostic identity. They do not introduce an executor, scheduling or
production registrations. Stage 2 checklists remain open until those consumers
and session semantics are implemented.

Validation: all 38 DerivedDataBuildTests cases passed in serial per-case
isolation and in the affected target batch. The five added cases cover the
schema-2 golden, canonical ordering, source-locator independence, descriptor and
identity changes, invalid bindings, duplicate/conflicting registration, frozen
descriptor copies, retained function/block lifetime and diagnostic scalar
preservation. The `all` build and all 88 affected targets passed; changed-document
validation passed for the plan and three corrected runtime contracts. Existing
schema-1 family goldens remain unchanged. No session lifecycle or performance
acceptance is inferred from this foundation coverage.

Inline execution receipt: `ExecuteBuildRequest` now drives the frozen registry,
metadata-only action construction, bounded record lookup/decode, family semantic
validation, miss-only byte resolution, recipe execution and optional persistence.
It does not call the legacy template executor, queue tasks or publish objects.
A malformed cached result produces one diagnostic and one uncached attempt;
invalid fresh output fails. Cancellation during cached validation never falls
back. Input identity/count/table/aggregate bounds are checked before recipe
entry. Resolver implementations still own semantic content verification.

Independent record/encode/compress/Put hooks and budgets establish recoverable
persistence faults, including allocation and actual filesystem failure. Disabled
writes enter none of those operations. Valid output retains recipe allocations
through failed persistence; a cancellation after a complete write returns no
output. Diagnostics remain execution-local, and diagnostic allocation failure
cannot invalidate usable output. The synchronous completion distinguishes failed,
cancelled and succeeded results and bounds producer descriptions while retaining
opaque producer code/identity. It establishes no asynchronous admission,
exactly-once callback, dispatch or shutdown guarantee on its own; the session
wrapper below supplies those guarantees.

Validation: 49 DerivedDataBuildTests cases cover the inline core, including warm
hits with unavailable source bytes, changed-source rejection on a forced miss,
each cold cancellation phase, invalid cache schemas/values, fresh validation,
read failure, independent persistence failure and shared allocation retention.
Serial per-case isolation and the four-target DDC module batch passed outside
the sandbox. Some launches failed inside macOS dyld before main, both inside and outside
the sandbox, confirmed by crash reports. The affected case was rerun separately;
no test failure was converted into a pass. The `all` build passed.

Owned-session receipt: `FBuildSession` retains a frozen registry, resolver and
injected dispatcher. Accepted work completes exactly once, including cancellation
and discarded dispatch closures; rejected admission invokes no callback. Inline
execution bypasses dispatch so existing owner workers never queue and wait on
their own saturated pool. Completion and callable destruction occur outside
session locks. Close stops admission and cancels work; drain waits for callbacks,
dispatch return and provider release. Reentrant drain returns `WouldBlock` rather
than waiting on its own execution stack. Completed handles and stale dispatch
closures retain no producer services.

Validation: all 60 DerivedDataBuildTests cases passed in serial isolation and
in the four-target DDC module batch. Eleven session cases cover rejected/dropped
work, callable exceptions, 100 cancellation/runner races, 50 close/admission
races, queued/running shutdown, reentrant completion, destruction from a callback,
provider retention through dispatch return and nested inline execution on one
Core worker. The `all` build passed. Engine bootstrap, Texture2D production
migration and its paired performance comparison remain open.

Texture2D integration receipt: the registered function receives bounded
RGBA8 mip metadata and retained source blocks, calls TextureBuild and returns
shared output. Source resolution verifies the captured payload hash. Recipe
scalar metrics stay in execution-local observers. The new action golden is
`e61ee4bf1de319d653ce7b3cb8b492f0`. Launch, tools and opted-in native fixture roots
explicitly initialize and drain the Engine service. The source change is covered by the final integration and performance receipts
below.

Initial integration validation passed 131 TextureTests cases and the `all`
build. The 92-target affected batch passed 90 targets. RenderShaderContractTests
crashed in dyld before main and then passed its 52 cases separately. RoadWeaver's
scene fixture omitted build-service bootstrap/drain and crashed in a late
texture completion; after fixing that fixture, its five cases passed. Three
Texture2D session cases now pass in serial isolation, including semantic-cache
rejection and service shutdown/restart. Provider dependencies were added to affected native targets so isolated builds
do not rely on another target deploying MeshBuilder.

Paired Texture2D session qualification on 2026-09-27 used frozen `8989d1310`
and this candidate on the same macOS arm64 Debug/Tracy profile, sequentially,
with one warm-up and three measured rounds. Initial cold sampled peak increase
was 7,021,184 B and exceeded the frozen gate. Record encoding now reserves its
validated complete size before appending blocks, removing payload reallocation
copies without changing record bytes or widening acceptance thresholds.

After that correction, Texture2D cold/warm/no-write medians were
893.689/21.1484/868.693 ms before and 876.423/19.039/858.443 ms after, ratios
0.98068/0.90026/0.98820. Sampled increases were
4,231,568/2,809,008/2,811,712 B before and
4,235,904/1,413,744/2,814,720 B after. All five texture fixtures passed the
1.15 time and peak-increase gates; maximum time ratio was 1.04093 and maximum
peak delta 4,336 B. Hashes were identical, recipe-to-product copies remained
zero and package-backed warm source reads remained zero. Cube's normalized
import fixture still has no source-read probe. The final candidate measurement
includes explicit service-owned session release.

Final integration receipt: the `all` build and all 92 affected native targets
passed after the final ownership/record-reservation changes, covering Engine,
Sandbox and RoadWeaver consumers. One test-discovery launch failed inside dyld
before main; the resumed build and complete batch passed without skipping tests.
The new bounded writer reservation has a dedicated encoding/budget test. Engine
retains admitted sessions until explicit release or shutdown, so discarded
caller handles cannot escape shutdown drain; normal Texture2D returns release
their resolver immediately. The three Texture2D session cases and bounded writer reservation case also passed
in serial isolation. The 60-case DDC isolation run had one dyld-before-main
launch failure; that exact case passed on its separate rerun. Four changed Vulkan
fixture targets (package reload, scene import, SkyBox and thumbnail) compiled.
No GPU/application execution is claimed.

Exit: texture/import/Cook/compilation coverage and performance gates pass;
DDC remains Core-only and does not reference Engine product/error types. The old
executor remains only for unmigrated families, with no dual per-request path.

### Stage 3: Migrate Cube and Volume textures

Dependency: Stage 2. Outcome: shared texture block ownership without a template executor.

- [x] Add separate registered functions and byte resolvers; retain canonical
  Cube faces/panorama identity, HDR projection and Volume shape/filter semantics.
- [x] Adapt platform containers to retain mip/face/voxel blocks; preserve cold
  import normalization reuse. Reuse archive helpers for persistence/Cook only
  where appropriate; never repeat physical-file import during recovery.
- [x] Migrate all build/rebuild/Cook paths and remove `TTexturePlatformBuildAdapter`.
- [x] Verify warm source-read counts, schema rejection, transactional replacement,
  key goldens, import workflows and cooked round trips for both families.

Exit: both families have one session path and pass their performance/behavior gates.

Stage 3 implementation receipt (2026-09-27): Engine explicitly registers
`Durin.TextureCube` and `Durin.VolumeTexture`. Captured byte resolvers verify
canonical identity on misses; Cube retains prepared import blocks without
reimport or repacking. Both families emit version-1 shared output layouts and
assemble platform descriptors retaining the same blocks. Volume recipes retain
base source voxels and freeze generated mip allocations. The texture adapter
header is removed; source/test searches across Engine, Sandbox and RoadWeaver
find no remaining texture adapter or legacy texture definition consumers.
Cancellation maps to the existing public canceled operation before generic
failure translation. Package/Cook TXPL schemas and bytes remain unchanged.

The schema-2 action goldens are Cube faces
`71a417531fded5faaab02e3c0265d30d`, Cube panorama
`0fe45f5840fd7969b8cc6753f0f3183e`, and Volume
`868aedbc1b663d4ffd621d5d954afbaa`. These deliberately replace the frozen
schema-1 baseline keys above, without a compatibility lookup.

The paired run used frozen `8989d1310` at
`20260927-043619-850204-54063-ctest.log` and the final candidate at
`20260927-043755-038154-54138-ctest.log`, on the same Debug arm64 host/profile,
one warm-up plus three measured rounds, with no overlapping build/test.
Every output hash matches, recipe-to-product transfer copies remain zero, and
instrumented warm source reads remain zero. Normalized-import Cube retains its
existing uninstrumented source-read probe rather than claiming a new probe.

| Fixture | Baseline cold/warm/no-write median ms | Candidate cold/warm/no-write median ms | Baseline sampled peak increase B | Candidate sampled peak increase B |
| --- | --- | --- | --- | --- |
| Texture2D BC7 | 887.493 / 21.4495 / 865.162 | 871.627 / 19.5505 / 841.995 | 4231568 / 2809008 / 2811968 | 4235840 / 1413776 / 2814864 |
| Cube normalized import | 380.483 / 25.2125 / 375.091 | 384.532 / 25.9713 / 379.787 | 4473120 / 4473120 / 4473120 | 4473120 / 4473120 / 4473120 |
| Cube captured faces | 344.272 / 4.25075 / 338.024 | 350.046 / 4.09908 / 345.112 | 2402416 / 546512 / 1935552 | 2412432 / 286256 / 1940816 |
| Cube captured HDR | 465.324 / 32.5123 / 426.798 | 471.547 / 30.0625 / 437.144 | 12592624 / 6295120 / 5777712 | 12602352 / 4202672 / 5782640 |
| Volume | 12.5519 / 4.71029 / 6.82417 | 10.5004 / 4.33312 / 5.35733 | 928080 / 612912 / 306944 | 669488 / 315584 / 47760 |

All ratios satisfy the frozen 1.15 timing and peak-memory gates. The initial
Volume candidate failed cold/no-write timing (21.9015/16.5338 ms) because its
inner voxel loops repeatedly reconstructed shared-buffer subspans in Debug.
Caching one retained read view per mip fixed that measured regression; no
threshold was widened and no voxel algorithm or output changed.

Behavior validation includes 88 affected native targets in one bounded batch
(`20260927-043516-827140-53020-ctest.log`), shared Engine `all` builds, and focused
isolated Cube/Volume session tests. After the measured hot-loop correction,
all 137 TextureTests passed (`20260927-043901-501029-54186-TextureTests.log`)
and the final `all` build passed (`20260927-043945-399017-54249-cmake.log`). New coverage checks zero persistence phases
with writes disabled, warm success with unavailable source, one fallback from
an intact but semantically invalid family record, prepared Cube checksum
mismatch, retained grouped-block lifetime and identical Cook bytes. The changed
application-hosted `VolumetricCloudSceneVulkanTests` fixture is not configured
with application tests disabled; neither its compilation nor GPU/application
execution is claimed. This optional coverage does not establish Windows Game
validation.


### Stage 4: Migrate StaticMesh and collision independently

Dependency: Stage 2; may follow Stage 3 sequentially in the same checkout.
Outcome: shared data outputs with separate render and collision identities/lifetimes.

- [x] Register Engine render/collision functions; resolve retained geometry into
  canonical input bytes without a live object or render-data dependency for collision.
- [x] Adapt stream/geometry ownership to retain compatible blocks; measure and
  document required PhysicsCore allocation/conversion rather than promise zero-copy.
- [x] Assemble render resources/collision geometry outside DDC; retain material
  reconciliation, bounds, acceleration and publication validation with Engine.
- [x] Remove render/physics adapters; keep the non-editor direct physics cook path.
- [x] Validate large-geometry memory overlap, independent render/collision hits,
  cancellation, stale installation, missing bulk, Cook and cooked-only loading.

Exit: StaticMeshTests, PhysicsSceneTests and affected mesh/Cook consumers pass;
no combined render/collision generic product or new runtime DDC dependency exists.

Stage 4 foundation in progress (2026-09-27): Core can now adopt moved native
vectors through the existing `FSharedByteBuffer`, retaining original type,
alignment, whole-element subviews and owner lifetime. Ordinary byte storage has
no native provenance. The new Core fixture covers an over-aligned element type,
an unrelated equal-layout type, partial-element rejection, retained subview
lifetime and moved-from access. The DDC fixture verifies zero-copy in-memory
output/record retention and provenance removal across both raw and Zstd codecs.
Mesh resource detachment, canonical streams, cooker/publication separation,
family functions and performance gates remain open; no Stage 4 checklist is
closed by this storage prerequisite.

Foundation validation: 15 archive tests and all 61 DDC build tests passed;
both new ownership/provenance cases also passed in separate processes. The
five-target affected selection passed, then all 83 `fast-all` targets passed
(`20260927-044818-261316-56764-ctest.log`) and the workspace `all` build passed
(`20260927-044900-535333-57474-cmake.log`). Test discovery initially hit macOS
dyld pre-main crashes for DDC, RenderShaderCache and the isolation probe; each
report identified `Loader::loadAddress` / `generateAtlas`, and subsequent
unmodified discovery and execution passed. No case was skipped. An initial
archive filter against CoreFileSystemTests selected zero cases and is not
counted; the actual owner is CoreUtilityTests.

A fresh same-host paired texture run compares frozen `8989d1310`
(`20260927-044944-620714-57577-ctest.log`) with this foundation
(`20260927-045021-436071-57609-ctest.log`), one warm-up/three samples and no
concurrent build/test. All five fixtures retain matching hashes, zero transfer
copies and zero instrumented warm reads. The maximum cold/warm/no-write ratio
is 1.02454 and the maximum sampled-peak increase over baseline is 11,072 B;
all existing texture gates remain satisfied. This does not claim mesh/collision
migration performance.


Stage 4 Mesh storage receipt (2026-09-27): position, normal, tangent, UV,
color and index resources retain native shared blocks or owned mutable vectors.
Read access returns borrowed spans. Explicit mutable access detaches while
preserving other block owners; invalid native types and serialized byte blocks
are rejected transactionally. Existing cold render assembly adopts recipe
vectors and Finalize materializes only missing defaults, retaining populated
shared streams. Archive conversion and independent collision input capture
still perform their existing explicit copies. Source/test consumers were
inventoried across Engine, Sandbox and RoadWeaver; cross-rebuild/unload fixtures
now take explicit snapshots rather than accidentally retaining borrowed spans.

Core additionally reports the complete backing vector capacity, including
capacity outside a subview. Mesh admission uses that retained capacity rather
than silently reducing its prior reservation estimate to the visible count.
The new resource fixture checks every stream's allocation identity, read-only
Finalize, invalid type/byte rejection, retained lifetime, mutation isolation and
over-reserved capacity accounting. All 142 StaticMeshTests passed, the new case
passed independently, 88 affected targets passed on final code
(`20260927-050252-094657-61601-ctest.log`), and `all` passed
(`20260927-050335-723982-62565-cmake.log`). An AssetMetadataQuery discovery failure
was confirmed in dyld before main; a RenderContract discovery SIGSEGV had no
matching crash report and remains unclassified. Both passed unmodified on the
complete rerun; neither is counted as a skipped success.

Paired 100,000-triangle validation used frozen `8989d1310`
(`20260927-050439-791702-62641-ctest.log`) and this Mesh storage candidate
(`20260927-050521-561158-63119-ctest.log`) on the same host/profile, one warm-up
and three measured pairs without concurrent build/test. Baseline versus candidate
render cold/warm medians were 1938.17/862.625 ms versus 1957.19/869.07 ms;
sampled peak increases were 104606592/74533136 B versus 104607376/74533520 B.
Collision cold/warm medians were 1662.28/620.344 ms versus 1659.71/620.129 ms,
with unchanged sampled peak increases 51939888/44617632 B and retained geometry
10248808 B. All timing and peak-memory gates pass; render source reads remain
one cold and zero warm. This qualifies the ownership prerequisite, not the still
open session migration or removal of archive-based validation/persistence copies.


Stage 4 render-output foundation (2026-09-27): `StaticMesh.RenderOutput` schema
1 implements the selected bounded metadata and named-stream layout. Cold output
adopts recipe arrays and assembly retains those allocations. Raw/Zstd records
lose native provenance; assembly creates each required typed vector once from
validated bytes. Compile-time little-endian, IEEE and element-layout checks
justify the native wire representation. Sections use their explicit fixed-width
codec. Output validation rejects missing/extra/truncated streams, bad counts,
non-triangle index/section counts and invalid geometry before constructing a
runtime product. Names/material bindings and resource/acceleration preparation
remain Engine responsibilities.

Archive payloads, runtime stream views and shared output reuse the same semantic
validator. Production finalization no longer copies a full payload solely for
validation; package conversion remains explicit. Five new fixtures cover all
cold stream allocation identities and owner lifetime, raw/Zstd restoration with
identical Cook bytes across one, two and three LODs and absent UV/color streams, malformed
output, mid-assembly cancellation and parity with archive validation. The output
codec is not yet connected to a registered Mesh function, and the legacy render
adapter remains. This foundation does not close the session/collision gates.


Foundation validation: all 147 StaticMeshTests passed; all five new cases passed
independently (`20260927-052136-888923-65275-ctest.log`). The final 40-target
affected selection, including PhysicsSceneTests, Cook/cooked loading and Sandbox,
passed (`20260927-052051-285163-64631-ctest.log`), as did the workspace `all`
build (`20260927-052121-386584-65221-cmake.log`). No startup failure or skipped
case occurred in this batch.

Sequential same-host qualification compared frozen `8989d1310`
(`20260927-052155-957145-65335-ctest.log`) with this candidate
(`20260927-052223-353183-65369-ctest.log`), one warm-up and three measured pairs.
Render cold/warm medians were 1939.18/859.693 ms versus 1950.75/862.516 ms;
sampled peak increases were 104606592/74533136 B versus 104607376/61311232 B.
Collision medians were 1661.89/613.338 ms versus 1660.79/617.25 ms, with unchanged
51939888/44617632 B sampled peak increases and 10248808 B retained geometry.
The largest time ratio is 1.0064; all timing/memory gates pass. Render source
reads remain one cold and zero warm. These measurements qualify production
view-based finalization; shared-output session performance remains unmeasured
until that production migration is connected.


Stage 4 render session migration (2026-09-27): Engine explicitly registers
`Durin.StaticMesh.Render`, captures canonical authored bulk and separate
reconciliation metadata, and invokes MeshBuilder through a byte-only resolver
and registered function. No material object binding or decoded geometry sidecar
enters DDC. Describe uses metadata only; Resolve verifies captured bytes and the
function decodes the existing authored representation once on a miss. This
reuses canonical source bytes instead of serializing acquired geometry again;
source residency remains independent. Assembly retains compatible native output
blocks, restores runtime names, and performs finalization outside DDC.

The render adapter and its required archive encode/decode are removed. Render
keys use schema-2 actions with separate source/reconciliation identities, target
and material-slot count, registered builder version and output schema 1. The
updated key golden is `198d50a43bafbc097641a3d2428962be`; package/Cook payload
schema 5 remains unchanged. Generic request policy now exposes a working-set
reservation through context without adding it to the action identity. Recipes
still reject expansion before native geometry and scratch allocation.

Session fixtures verify retained input generations, zero warm source reads,
one rebuild of a semantically invalid record, optional record failure, zero
persistence phases with writes disabled, fresh invalid output, cached-validation
cancellation and wrong source/reconciliation bytes. A generic fixture verifies
that changing only the reservation reaches the function without changing its
key. The module-replacement fixture now drains/reinitializes the explicit
registry, preserving the immutable registration contract. Physics, Spline and
ContentBrowser fixture roots explicitly bootstrap the service when exercising
Mesh production builds. Collision is independent and remains on its legacy
adapter; no combined render/collision product is introduced.


Validation: all 150 StaticMeshTests passed, then the three new session/function
cases and migrated module-replacement case passed independently
(`20260927-054120-320672-68134-ctest.log`). The reservation case also passed
independently (`20260927-054146-254277-68217-ctest.log`). The final affected batch
passed 85 of 88 targets (`20260927-053812-100955-66995-ctest.log`), including
PhysicsSceneTests, SplineTests, DDC, Mesh, Cook and Sandbox. ContentBrowser's
missing explicit bootstrap was then fixed and all 135 cases passed
(`20260927-053931-391383-67994-ContentBrowserWorkflowTests.log`). ShaderContract
passed all 52 cases independently (`20260927-054021-961562-68097-RenderShaderContractTests.log`);
ShaderCookIntegration passed in the bounded shader rerun
(`20260927-054002-709125-68064-ctest.log`). Thus every selected target has passing
execution evidence on its final code, without skipped cases. ShaderBuilder,
ShaderContract and ShaderCookIntegration also experienced intermittent startup
SIGSEGVs; matching reports consistently identified dyld `Loader::loadAddress` /
`generateAtlas` before main. An earlier EngineViewportHeader discovery timeout
had no classified cause; the process tree exited, DevTool status was clean, and
subsequent discovery/execution passed. The workspace `all` build passed
(`20260927-054137-873938-68181-cmake.log`).

The paired 100,000-triangle gate compares frozen `8989d1310`
(`20260927-054154-697075-68235-ctest.log`) with the production-session candidate
(`20260927-054229-499309-68266-ctest.log`) on the same host/profile, one warm-up
and three measured pairs without concurrent build/test. Render cold/warm medians
were 1914.94/847.724 ms versus 1202.63/805.942 ms; sampled peak increases were
104606592/74533136 B versus 52504992/65752272 B. Cold native stream transfers
remain zero and warm source reads remain zero. Collision cold/warm medians were
1637.77/600.577 ms versus 1642.45/609.679 ms; retained geometry stayed 10248808 B.
Its sampled increases were 51939888/44617632 B versus 50580016/38064032 B; this
allocation history observation does not imply a collision implementation change.
All time/peak gates pass. All six StaticMeshBuildQualificationTests additionally
passed (`20260927-054256-813035-68304-ctest.log`), covering detached budgets,
concurrent large builds, cancellation and native transfer alternatives. The
collision migration and remaining Stage 4 checklist items stay open.



Stage 4 PhysicsCore ownership/validation receipt (2026-09-27):
`FCollisionCookedData` finishes triangle cleanup, deterministic BVH construction
or convex topology before freezing its native arrays. It has no geometry
identity. `FCollisionGeometryRef::MakeCooked` retains those arrays and allocates
a fresh runtime identity only at publication. Existing direct constructors and
builders use this boundary, including non-editor consumers; PhysicsCore still
has no DDC dependency. The triangle recipe reserves the exact deterministic
median-split node count before adoption, avoiding retention of the previous
scratch over-allocation. Retained-byte accounting includes backing capacity;
package arrays may therefore have different capacity from recipe arrays. The
Cook regression now compares every vertex, triangle, node and leaf ordinal and
bounds loaded retention by authored retention instead of requiring identical
allocator capacity.

`FCollisionCookedBlocks` has fixed-width layout/offset, IEEE and little-endian
proofs for all seven arrays. Validation reads serialized elements through
`memcpy` without native casts, native-array conversion or geometry creation.
It checks finite/exact bounds, indices, convex closure/orientation, planes and
half-edge/face consistency, or complete BVH/triangle visitation, parent bounds,
leaf bounds and depth 64. `FromBlocks` validates first, retains compatible
native blocks and constructs each serialized native vector once. Cancellation
cannot publish a partial result. The Engine collision adapter, input capture
hashing and archive path are intentionally still pending, so the combined
Stage 4 checklist remains open.

Validation: final-code affected selection covered 92 targets; 91 passed in
`20260927-060206-907193-72152-ctest.log`, including all 49 PhysicsSceneTests and
150 StaticMeshTests. CoreConcurrencyTests failed before test output; its matching
`ExcUserFault_CoreConcurrencyTests-2026-09-27-060222.ips` reports dyld atlas
iterator invalidation. Its focused rerun passed all 158 cases in
`20260927-060254-936769-74718-CoreConcurrencyTests.log`. Eight geometry cases
passed serial isolation (`20260927-060323-203928-74784-ctest.log`). The required
`all` build passed (`20260927-060313-137809-74760-cmake.log`). All declared
Engine/Sandbox/RoadWeaver source/test roots were searched for constructor and
builder consumers; public compatibility entry points remain available.

The new 100,000-triangle production-array qualification passed in
`20260927-060153-138584-72043-ctest.log`: 10,248,544 logical array bytes, zero
cold representation-transfer bytes, one 10,248,544-byte warm native conversion,
and sampled assembly peak increases of 18,432/10,273,664 B. Float-input to double
recipe conversion is separately reported as 7,200,000 B. Unlike the Stage 0
layout experiment, this case starts from cooked data without extracting a
published geometry.

Sequential same-host baseline/candidate end-to-end runs (one warm-up and three
measured pairs) passed the frozen time/peak gates. Logs are baseline
`20260927-060344-879646-74849-ctest.log` at `8989d1310` and candidate
`20260927-060410-810382-74883-ctest.log`. Collision cold/warm medians changed from
1655.60/612.243 ms to 1665.59/623.289 ms; sampled peak increases changed from
51,939,888/44,617,632 B to 50,581,456/38,046,320 B. Candidate collision retained
bytes are 10,249,168 B. These qualify the Core ownership change through the
existing collision path, not the still-pending collision session migration.
Render cold/warm medians remain within gates at 1208.79/812.378 ms versus
1936.82/857.500 ms, with cold/warm source requests still 1/0.


Stage 4 collision session migration receipt (2026-09-27): Engine explicitly
registers `Durin.Physics.Collision`. `FPhysicsCookInput` owns native source blocks
and their capture-time identity. Detached compilation and Cook move prepared
vectors into capture; the borrowed convenience entry creates an owned snapshot.
The resolver describes metadata only and exposes named Positions/Indices bytes
on a miss. The registered function verifies those bytes, performs the existing
float-to-double recipe conversion, and emits `Physics.CollisionOutput` schema 1
from `FCollisionCookedData` without publishing/extracting temporary geometry.
Engine assembles geometry after session completion. `TPhysicsBuildAdapter` and
its DDC archive encode/decode path are removed. Non-editor direct cooking and
package collision serialization remain available with no new runtime DDC edge.

Output metadata is bounded to 184 bytes (104 for either currently emitted
single-geometry mode). Validation requires exact target/profile, mode/policy,
presence, kind, array counts and active block names, then uses PhysicsCore's
view-based semantic checks. Cold blocks retain recipe allocations; raw and Zstd
records convert each native array once at warm assembly. Raw/compressed round
trips preserve exact package collision bytes. The action is schema 2 with
separate output version 1; collision key goldens are now
`333a859a554afe2b71352d202fafc066` (contract fixture) and
`92525216610547fa54e74aaf0c25f019` (380-byte frozen geometry fixture).

Validation: all 155 StaticMeshTests passed, including new owned-capture,
no-resolve warm hit, malformed-record rebuild-once, optional persistence failure,
zero no-write persistence phases, warm cancellation, forged-input/budget,
raw/compressed byte compatibility and malformed output tests
(`20260927-061645-452940-75533-StaticMeshTests.log`). All 88 affected targets
passed (`20260927-061712-754336-75580-ctest.log`), including PhysicsSceneTests,
Cook/cooked-only loading, stale generation, missing bulk, source ownership and
independent render/collision coverage. Five new cases passed serial isolation
(`20260927-061803-017160-76466-ctest.log`). All seven StaticMesh qualification
cases passed (`20260927-061824-763202-76488-ctest.log`), covering large source
residency, memory admission, concurrent work, cancellation and native transfers.
The shared API all build passed (`20260927-061758-199087-76433-cmake.log`). Searches
across Engine/Sandbox/RoadWeaver found no legacy physics adapter/definition calls.

Sequential same-host/profile paired runs used frozen baseline `8989d1310`, one
warm-up and three measured pairs, with no concurrent build/test. Baseline log
`20260927-061909-465669-76521-ctest.log` and candidate
`20260927-061941-824513-76555-ctest.log` satisfy both frozen time and peak gates:
collision cold/warm medians 1650.57/609.110 ms became 1192.30/288.680 ms;
sampled peak increases 51,939,888/44,617,632 B became 46,764,736/29,100,064 B.
Render cold/warm medians were 1933.86/858.411 ms versus 1206.47/809.323 ms;
peaks were 104,606,592/74,533,136 B versus 52,505,008/65,752,288 B. The unchanged
end-to-end fixture still includes capture costs; cold/warm render source reads
remain 1/0. Session tests separately prove warm collision execution does not
resolve or rehash captured source. Existing native-array qualification and
output pointer checks establish zero cold representation transfers and one
warm native conversion. These receipts close Stage 4; Shader and final legacy
removal/platform/documentation gates remain open.


### Stage 5: Migrate ShaderBuild

Dependency: Stage 2.
Outcome: registered shader function without losing compiler-specific ownership.

- [x] Represent normalized options and complete source closure in explicit
  constants/input data; retain content verification and bounded virtual filesystem.
- [x] Keep Slang instance lifetime, single-flight, LRU, generated source handling,
  dependency manifests and cooked-library ownership within ShaderBuild.
- [x] Move/share bytecode blocks into output and consumer records; retain explicit
  ordered metadata and verify lifetime across LRU insertion and waiter completion.
- [x] Replace the shader adapter and translate generic completion once; preserve
  diagnostics, output order, cancellation and DDC/LRU counter semantics.
- [x] Validate generated/file-based sources, changed includes, warm no-compile
  behavior, concurrent waiters, module shutdown and cooked shader consumers.

Exit: shader contract/cache/builder/cooked-library and material integration
coverage passes; no per-request mutable state is stored in registered functions.

Stage 5 ownership/output foundation (2026-09-27): compiled bytecode now retains
immutable common storage across compiler output, LRU/waiter copies and cooked/RHI
consumers. Slang blob and reflection-word conversions remain explicit existing
recipe costs. The private `Shader.Output` schema-1 codec keeps ordered entry
metadata separate from shared code and bounded reflection blocks. Cold assembly
retains producer storage; raw and Zstd cache-record assembly retains decoded
record subviews. Validation checks code hashes, SPIR-V headers, exact entry/value
ordering, reflection bounds and complete consumption without constructing a
compiler product or copying code. Package/Cook archive bytes remain unchanged.
The generic diagnostic boundary preserves bounded formatted text and the original
semantic fingerprint without reconstructing structured causes. The registered
function, explicit source-closure input and production completion mapping are
still pending; this foundation does not close Stage 5.

Validation: all 46 affected targets passed on final code
(`20260927-063452-792314-78379-ctest.log`), including shader contract/cache/builder,
Cook/cooked-library, material lifecycle and Sandbox. The shared-API `all` build
passed (`20260927-063524-189920-78535-cmake.log`); the changed GPU qualification
consumer compiled (`20260927-063619-989062-78753-cmake.log`) without GPU execution.
Three shared-output cases and the diagnostic case passed independently: two in
`20260927-063541-406451-78661-ctest.log`, raw/compressed restoration in
`20260927-063609-720716-78720-ctest.log`, and diagnostic identity in
`20260927-063615-733054-78736-ctest.log`. Matching crash reports attribute earlier
ShaderCache/ShaderContract startup failures to dyld before main; the restoration
case passed unchanged outside the sandbox after repeated dyld startup failures.
An initial MaterialRuntime discovery timeout had no classified cause; the process
tree exited, DevTool status was clean, and full affected discovery/execution then
passed. No failure was converted into a skipped success.

Sequential same-host/profile CPU qualification compared frozen `8989d1310`
(`20260927-063644-468337-78785-ctest.log`) with this candidate
(`20260927-063705-546705-78842-ctest.log`), eight shaders, one warm-up and three
measured batches. Cold/DDC/LRU medians were 43.5219/1.75012/0.642667 ms versus
45.8893/1.73854/0.637167 ms. Sampled peak increases were
76,429,104/19,600/2,576 B versus 76,429,040/20,928/2,352 B. Both frozen gates pass;
bytecode remains 4,192 B, with eight cold content reads/compiles and zero warm
reads/compiles. These measurements qualify immutable storage through the existing
executor, not the still-pending production session path. Pointer/lifetime tests
separately prove no bytecode transfer in the shared-output codec. Changed-document
validation passed for the plan and ShaderCache contract.

Stage 5 session migration (2026-09-27): ShaderBuild explicitly constructs its
compiler service, registers `Durin.Shader.Compile` version 2 and freezes the
registry before constructing the builder. Constants schema 1 encodes normalized
options as one bounded canonical `Options` byte constant; source-closure input
version 2 contains ordered virtual file descriptors and named immutable bytes.
Metadata-only Describe preserves no-read warm hits. Resolve verifies captured
file hashes, and the registered function recomputes the complete portable variant
including generated-root bytes before compilation. The function retains compiler
services only; per-request options and source data come from the context.

`FShaderSourceArtifacts` now retains immutable common blocks, allowing resolver
and compiler filesystem assembly to preserve capture allocations. The existing
Slang filesystem copy and reflection-word conversion remain recipe costs.
The legacy Shader adapter is removed. Sessions return shared outputs, and the
builder translates completion once before assembly/LRU admission. Compiler phase,
native-status diagnostic grouping survives the generic boundary. Unexpected
recipe exceptions become failure results and leave flights retryable. Cache
issue counters remain execution-local, counting one corrupt miss or optional
persistence failure. Private legacy key/codec helpers remain for Stage 6 cleanup;
production persistence uses schema-2 actions and cache records.

Shutdown closes admission, cancels/drains every admitted session and waits for
inline service calls to retire before compiler teardown. Concurrent tests prove
independent compilers, coalesced identical work, shared bytecode across waiters
and LRU, and retained output after builder/service destruction. Source-identity,
malformed record/options, no-write, optional-persistence and cancellation cases
exercise the new production boundary.

The first paired performance run exposed warm DDC/LRU regressions. LRU now uses
a dedicated in-process identity over variant, ordered entries and search roots,
so hits avoid constructing a session request. DDC action construction occurs
once. A bounded explicit options byte codec avoids separately allocating named
constants for every entry/macro/root. Temporary phase instrumentation was removed;
performance gates remain unchanged and final qualification is still required.

Stage 5 final acceptance receipt (2026-09-27): the input protocol now stores
ordered virtual paths in a bounded `FileTable` value rather than generic metadata.
Generic input admission retains its 4,096-value default, with explicit policy
support up to 131,072 values; output/cache-record limits remain unchanged. Shader
requests admit 65,536 files plus descriptor/generated values. Full-count coverage
uses more than 4 MiB of descriptors and proves shared source retention, explicit
admission and successful compilation. Identifier, duplicate and byte-budget
failures remain rejected before the function runs.

The warm lookup regression was resolved by eliminating redundant bucket
construction and repeated lexical normalization of cache entry paths. Normalized
bucket directories and validated hexadecimal binary-key components preserve
containment; file-type, symlink and integrity checks remain unchanged. The new
normalized-root/key-extremes test passed with all 13 cache tests
(`20260927-071816-530879-81616-DerivedDataCacheTests.log`).

Final validation passed all eight affected targets
(`20260927-071929-003533-81887-ctest.log`), all eleven Material targets
(`20260927-072008-136181-81966-ctest.log`), and the shared Engine API `all` build
(`20260927-072034-747488-82346-cmake.log`). Eight shader session/concurrency cases
passed serial isolation (`20260927-072046-781851-82393-ctest.log`); the generic
large-table boundary passed independently (`20260927-072102-537595-82422-ctest.log`).
All declared projects' source/test roots were searched for source-artifact API
consumers. Startup crashes in the initial qualification and affected discovery
runs matched dyld `generateAtlas`/`loadAddress` before main; unchanged runs outside
the sandbox passed. GPU/application execution was not requested or performed.

Sequential same-host/profile qualification compared frozen `8989d1310`
(`20260927-072113-033566-82438-ctest.log`) with the final candidate
(`20260927-072120-168746-82467-ctest.log`), eight shaders, one warm-up and three
measured batches. Cold/DDC/LRU medians were 43.0258/1.65542/0.656250 ms versus
45.2691/1.83975/0.478916 ms. Sampled peak increases were
76,429,104/18,032/2,112 B versus 76,429,040/20,800/2,352 B. All frozen time and
memory gates pass. Bytecode remains 4,192 B, cold reads/compiles remain eight,
and warm reads/compiles remain zero. These receipts close Stage 5.

### Stage 6: Remove the legacy executor and publish contracts

Dependency: Stages 3-5. Outcome: one shared protocol across all six families.

- [x] Remove `ExecuteBuild<TAdapter>`, all family adapters, typed build observations,
  obsolete codec/reporting bridges and compatibility-only scaffolding.
- [x] Search all declared projects' source/test roots and migrate every consumer.
- [x] Update DerivedDataBuild, AssetDataLifecycle, AssetCompilation, StaticMesh,
  Physics, shader contracts and CodeModules only as implemented behavior lands.
- [x] Complete the required shared Engine API `all` build and affected project
  targets; execute the risk-based native coverage below and record receipts.
- [x] Coordinate with the predecessor's Windows Game gate and inspect Game
  dependencies. Record the owner's 2026-09-27 decision to defer the unperformed
  Windows build/startup validation until an environment is available.
- [x] Run documentation/lifecycle validation and close this plan after the
  host-supported acceptance conditions pass, with Windows validation explicitly
  deferred rather than reported as passed.

Stage 6 local cleanup receipt (2026-09-27): removed the template executor,
schema-1 keyed definition, typed observation visitor, Engine reporting bridge and
Shader legacy key/codec forwarding helpers. Identity tests now exercise schema-2
actions, including type tags, normalized floats, every descriptor/input field and
4,096 unordered bindings. Existing session fault/lifecycle tests supersede the
old adapter harness. Default diagnostic logging is covered at the shared executor
boundary, preserving stage/key/cause and suppressing ordinary misses. Shader
package golden bytes continue through RenderCore's actual package codec; its
request-key test now constructs the production session request and action.
Searches of Engine, Sandbox and RoadWeaver source/test roots found no remaining
legacy execution symbols or includes.

Game dependency inspection: `Engine.dproject` selects DerivedDataCache and
ShaderBuild only in DurinEditor. Engine's DDC dependency remains optional and
asset service registration is editor-gated; RenderCore depends on RHI/Core and
has no DDC dependency. Registered Game presets are Win64-only, so this host cannot
supply a Windows Game build or cooked-only startup receipt. Both revisions lack
Windows Game evidence; that validation is deferred until an environment is available.

Final local validation: 87 of 88 affected targets passed in
`20260927-072742-463070-83400-ctest.log`, covering texture/import, StaticMesh,
physics, asset compilation, Scene import, Cook/cooked loading, Shader, Material
and Sandbox. The remaining ShaderCache startup failure matched a dyld
`loadAddress`/`generateAtlas` crash before main; all nine cases passed unchanged
in `20260927-072842-399938-84085-RenderShaderCacheTests.log`. Earlier ShaderBuilder
discovery failures matched the same pre-main dyld stack; its 28 cases passed in
`20260927-072702-158339-83361-RenderShaderBuilderTests.log`. No test failure was
converted to a skip. The final `all` build passed
(`20260927-072850-405817-84100-cmake.log`). Five migrated identity/default-diagnostic
cases passed serial isolation (`20260927-072901-688418-84133-ctest.log`), as did the
Shader action-key case (`20260927-072917-239331-84156-ctest.log`). Changed-document
and all-plan validation passed. Stage 5 performance receipts remain applicable:
cleanup removes unreachable APIs and leaves the session execution and action bytes
unchanged, with canonical-action and package golden checks still passing.
Windows Game build/startup validation remains unperformed and deferred by the
owner's 2026-09-27 decision; it is no longer a plan completion gate.

## Validation and Handoff

Follow [Build and Run](../../../Agents/BuildAndRun.md),
[Testing](../../../Agents/Testing.md) and
[Documentation](../../../Agents/Documentation.md). Select targets from the registry
and affected analysis at implementation time. Required coverage includes build
protocol/DDC tests; TextureTests and texture import workflows; StaticMeshTests
and physics; Shader contract/cache/builder/cooked-library tests; asset compilation,
Scene import, CookFunctionalTests and CookedMeshLoadingTests. Validate affected
Sandbox and RoadWeaver targets as well as Engine. Do not infer a runnable Game
from Editor-native cooked tests.

Use fault injection for resolver/recipe failure, optional cache-record encoding,
compression and write failure, cache rejection, cancellation at every boundary
and late callback races. Assert that a complete in-memory output survives every
persistence failure and that disabled writes invoke no persistence codec. Run new
async cases independently as well as in their target suite.
Check deterministic metadata/block contents across input storage/compression
changes and warm/cold paths. Verify shared backing, ownership after producer and
session destruction, mutation isolation, no mandatory cold archive round trip,
and existing authored/cooked byte compatibility. Measure performance without logging noise or persistent-cache
contamination, recording fixture sizes and build configuration.

Each family migration is a bounded commit with this plan's exact stage trailer.
The temporary old/new coexistence is by family, never dual execution or fallback
within one request. If a stage fails acceptance, retain the last migrated state
and repair or revert that stage; do not add a permanent alternate executor.
