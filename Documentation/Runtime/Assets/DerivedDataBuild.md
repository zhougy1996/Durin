# Derived Data Build Protocol

Summary: Define canonical build identity, captured input bindings, shared-output sessions and optional cache persistence for authored derived data.

Modules: DerivedDataCache, Engine, ShaderBuild

Last reviewed: 2026-09-27

## Ownership

`DerivedDataCache` contains two independent responsibilities in one module:
immutable build definitions with shared execution, and backend-neutral opaque
byte storage. Both depend only on Core. Cache backends do not include build
APIs. There is no separate DerivedDataBuild module, build scheduler, remote
worker, generic dependency graph or generic single-flight. Explicit function
registration and frozen registry snapshots bind all six producer families.

Engine owns the registered Texture2D, Cube, Volume, StaticMesh render and physics
collision functions. ShaderBuild registers the shader compiler function. Each
family owns its recipe, source representation, output validation, typed consumer
assembly and error translation. TextureBuild and MeshBuilder
remain pure typed transformations. ShaderBuild retains its compiler, dependency
manifests, LRU and single-flight. Existing compilation managers own admission,
reservations, cancellation, latest-wins checks, DLL lifetime and publication.

## Canonical identity

Definition and action factories return owned immutable values or typed validation
errors. Action accessors expose const fields, inputs and canonical bytes.
The descriptor contains function name/version, constants schema, output type/
schema and cache bucket. Constants are named typed bool, uint64, float, string
or XXH3-128 values. Named input references contain content identity, identity
scheme/version and representation/version; they contain no source pointers.

Action encoding uses a domain tag, schema 2, explicit little-endian integer widths,
length-prefixed strings, constant type tags and sorted names. Float negative
zero becomes positive zero; non-finite floats are rejected. Names are bounded
ASCII identifiers, versions and input hashes must be nonzero, duplicate names
within a field class are rejected, and the bucket must be valid. Each field
class permits at most 4,096 entries; string constants total at most 1 MiB.
The action's canonical bytes determine its XXH3-128 cache key.

Function/translator, input identity, representation and output schema versions
have distinct meanings. A recipe or output-affecting normalization change must
invalidate identity. Source compression, physical import hints, cache location,
persistence policy, cancellation, scheduling and object addresses do not enter
production identity. Family-specific composed identities may remain versioned
hashes, such as StaticMesh reconciliation and Shader's portable variant.

The migrated keys deliberately invalidate prior cache entries, without a legacy
fallback lookup. Authored package and cooked/platform payload schemas are
unchanged. Private `Build*DerivedDataKey`/`Build*DerivedDataKeyBytes` functions
project the same family actions for fixture inspection; they are not independent
encoders or compatibility cache paths.

## Unresolved definitions and registered actions

`FBuildDefinition` owns a function name, normalized constants and named opaque
captured-source references. It has no descriptor version or cache key.
`FBuildAction::TryCreate` combines a matching descriptor with resolved semantic
identities, requires exactly the definition's source names, and freezes those
values into action canonical schema 2. The action domain tag differs from the
legacy definition tag. Source locators are resolver bindings, not key fields;
equivalent captured content has the same action identity. These types do not
perform lookup or execute a recipe by themselves.

`FBuildRegistry` accepts explicitly owned functions and rejects invalid or
duplicate descriptors. `Freeze` prevents further registration and produces an
immutable snapshot. Each lookup retains the descriptor captured at registration
and a shared function owner. Destroying the registry or snapshot cannot unload a
function while another retained entry still owns it. Functions must retain the
execution services they need; hot replacement is unsupported. Registration
calls producer descriptor code without holding the registry mutex.

`IBuildInputResolver` separates metadata-only `Describe` from miss-only byte
`Resolve`. Materialized inputs contain semantic identity, representation
metadata and named immutable blocks. Resolver implementations verify content
against the promised identity. `FBuildContext` borrows the admitted action and
retained inputs for a synchronous function call, and provides cooperative
cancellation. `IBuildFunction` returns shared build output and validates its
semantic descriptors without resolving input or creating a disposable product.
These interfaces expose no typed product side channel.

## Captured input bindings

Before lookup, metadata-only resolution must match the definition's complete
named source set. Action construction canonicalizes bindings and rejects duplicate
names or invalid identities. A source binding retains the captured generation.
Miss-only resolution verifies its content rather than silently substituting current
asset state or reimporting a physical filename under an old identity.

Registered functions read constants and retained inputs from `FBuildContext`.
They produce immutable shared blocks, validate their family schema and never
retain per-request state. Typed product assembly remains at the owning consumer;
there is no universal product variant or opaque product cast. Cold outputs retain
recipe allocations without a mandatory serialization/decode round trip.

| Producer | Binding and preparation | Identity-specific fields |
| --- | --- | --- |
| Texture2D | Validated torn-off `FTextureSource`; resolve owned shared mip views on miss | Usage, resolved sRGB, compression quality, alpha mode/threshold, maximum resolution, target/profile |
| TextureCube | Import normalizes to canonical faces or HDR panorama; loaded canonical source uses the same executor | Canonical layout, sRGB, projection version, HDR dimension/exposure, target/profile |
| VolumeTexture | Owned torn-off source; resolve voxel view after lookup | Shape, format/filter, source schema, target/profile |
| StaticMesh render | Captured authored bulk plus source-count metadata; resolve retained bytes on miss and decode once inside the function | Separate reconciliation input contains normalization and material slots; action also records slot count, producer/output schema and target |
| Physics collision | Immutable captured positions/indices; identity is computed during capture, and Describe uses metadata only | Collision mode, query policy, weld settings, producer/output schema, target |
| Shader | Captured artifacts or metadata-bound closure resolution | Portable variant, compiler environment, source/include contents, normalized macros, exact ordered entry points/stages |

Texture request capture and metadata lookup do no source payload I/O. Valid
Texture2D, canonical Cube and Volume cache hits do not resolve source bytes.
Noncanonical panorama recovery may normalize before it has a canonical key.
Cube import reuses already normalized immutable images on cold execution.
Cube definitions use one `Source` reference for the complete ordered canonical
source, with layout/representation distinguishing faces from panorama. Cube
constants schema 2 replaces six repeated whole-source identities and invalidates
the prior Cube cache key. Source identity still covers face ordering and pixels.
All three texture families execute through owned sessions. Cube outputs retain
face-major `Face/<face>/Mip/<mip>` blocks; Volume outputs retain `VoxelMip/<mip>`
blocks. Version-1 metadata records the target, stable format and bounded mip
layout. Functions validate complete chains, exact pitches/byte counts and
semantic value IDs before product assembly. Volume base voxels retain the input
owner; generated mips freeze their own allocations. Prepared Cube images retain
canonical blocks and verify their ordered payload checksum only on a miss.
Package and Cook archives remain separate from the shared cache-record codec.
StaticMesh hits use source metadata even when canonical bulk is unavailable.
Shader hits may validate dependency file facts; they skip source capture and
compilation. A shader miss verifies expected file size/content and supplies a
bounded immutable filesystem to Slang, with no live-file fallback. See
[Shader Cache](../Rendering/ShaderCache.md) for dependency and sharing contracts.

## Native allocation ownership

Core `FSharedByteBuffer::TakeNative` adopts a moved trivially copyable
`std::vector<T>` allocation through an aliasing immutable byte owner. It retains
the vector's element lifetime and alignment without copying payload bytes.
`GetNativeView<T>` returns a borrowed read-only span only when the original
native element type matches and the byte view covers whole elements. A caller
must retain the buffer while using that span. `GetRetainedCapacityBytes` reports
the full backing vector capacity, including bytes outside a subview; logical
wire budgets still use the visible byte size. Equal size/alignment alone does
not establish a type match. Invalid/moved-from buffers reject native access.

Named build values and in-memory cache records preserve this local provenance.
Raw and compressed serialization store only bytes; decoded record blocks and
ordinary copied byte buffers cannot be treated as live native arrays. Consumers
must validate/decode serialized elements into native storage before adopting
them. Native provenance never contributes to cache identity or persisted schema.

## Inline shared-output execution

`ExecuteBuildRequest` uses a frozen registry snapshot and captured resolver.
It constructs a schema-2 action from metadata, then looks up and validates a
keyed record. A hit returns retained output blocks without resolving source
bytes. A rejected cached result falls through once to resolution and recipe
execution. Invalid fresh output fails before persistence. Resolved input
identities and aggregate limits must match the frozen action before Build.
Resolved value tables are sorted by identifier and checked for invalid or
duplicate identifiers before the function runs. The default input admission is
4,096 values; explicit request policy may admit up to 131,072 across all inputs.
Aggregate metadata remains bounded to 4 MiB, and all metadata and value bytes
count against the request byte budget. Output/cache-record tables retain their
separate 4,096-value bound.
`FBuildRequestPolicy::MaximumWorkingSetBytes` reaches the function through
`FBuildContext`; this execution reservation never changes action identity.
Owning recipes enforce the reservation before scratch/product expansion.

Persistence constructs a record, encodes it, optionally compresses it and writes
it atomically through the existing backend. Each operation can fail without
replacing the original valid output. Disabled writes skip all four operations.
Cancellation is checked between phases and after persistence; cancellation in
cached validation does not trigger rebuilding. `FBuildResult` is an
`std::expected` of shared output or `FBuildError`; cancellation uses the
`Cancelled` error category. Execution-local observers receive cache diagnostics,
not products. This synchronous entry owns no queued work, asynchronous callback
or shutdown lifecycle.

## Owned sessions and dispatch

`FBuildSession` owns a frozen registry snapshot, captured input resolver and an
optional injected dispatcher. `Submit` returns a cancellation handle; immediate
admission or dispatch rejection returns an error without a callback. Accepted
work invokes its completion exactly once with success, failure or cancellation.
Completion can run inline or on a worker, with no implicit game-thread routing.
Callbacks run without session locks. A throwing callback does not prevent
terminal accounting or shutdown.

A dispatcher must reject without invoking or retaining its work, or accept and
execute it at most once. Dropping accepted work cancels it. An empty dispatcher
executes inline. `ExecuteInline` returns one `FBuildResult`, including admission
errors, and always bypasses dispatch to drive the same execution core. Already
admitted owner workers use it to avoid queueing and waiting on a saturated pool.
Sessions create no worker pool or publication loop.

`Close` stops admission and cancels pending/running work. `Drain` also closes,
then waits for terminal callbacks, dispatch return and release of callable owners.
Calling it from the same session's execution, callback or cleanup stack returns
`WouldBlock`; external shutdown must drain before unloading producer services.
Destruction from a callback defers release through the retained execution state.
Completed handles and stale dispatch closures retain no resolver/function owners.
Owners still enforce generation and cancellation checks before publication.

## Engine texture execution

`InitializeAssetBuildService` explicitly freezes Engine registration after
TextureBuild and MeshBuilder load. Launch initializes it before authored
compilation admission and DObject initialization; headless tools and native
fixture roots own equivalent initialization. Shutdown closes asset compilation,
then closes/drains build sessions before producer unload and task shutdown.
The service retains sessions until explicit release or shutdown, including
abandoned caller handles. Game does not initialize authoring providers.

Texture2D import, detached builds, compilation, PostLoad and Cook converge on
`BuildTexture2DPlatformData`. It creates a captured-source resolver and executes
the registered function inline on its existing owner worker. Constants retain
usage, sRGB, quality, alpha policy, maximum resolution and target/profile;
schema-2 action identity deliberately invalidates the former schema-1 cache.
`Describe` reads only snapshot metadata; `Resolve` verifies the captured payload
hash and returns retained RGBA8 mip blocks with bounded width/height/gamma
metadata. It never substitutes live source or import files.

The function invokes TextureBuild and emits `Texture2D.Output` version 1,
containing target/profile, stable format and ordered mip descriptors plus
`Mip/<n>` blocks. Validation reads descriptors only. Engine assembly retains
these blocks, and DDC persistence uses the keyed record codec. Package/Cook
serialization remains unchanged. There is no legacy lookup or execution fallback.
Recipe timings are scalar execution observations through `FBuildContext`, never
persisted metadata; generic errors retain a producer code and one formatted
family description. Engine translates completion once to its existing typed
operation error. Object generation checks and publication remain with managers.


## StaticMesh render sessions

Engine registers `Durin.StaticMesh.Render` at explicit asset-build bootstrap.
The resolver captures canonical authored bulk and scalar source metadata, plus
material names/source indices and normalization without material object bindings.
Metadata-only Describe freezes separate source/reconciliation identities. Resolve
verifies bulk identity and returns existing bytes without reserializing geometry;
the registered function decodes that authored representation once and calls the
pure MeshBuilder recipe. It does not use an opaque decoded-geometry side channel.

`StaticMesh.RenderOutput` version 1 contains bounded layout metadata, explicit
section tables and native-adopted active streams. Cache validation borrows bytes
without constructing render resources. Cold assembly retains original arrays;
warm assembly performs one typed conversion per active stream, then retains it.
Engine restores material/display names and performs bounds, resource and ray
preparation after successful session completion. Publication rejection is a
contract failure, not a second cache fallback. Package/Cook schemas are unchanged.

## Physics collision sessions

Engine explicitly registers `Durin.Physics.Collision` with the asset-build service.
`FPhysicsCookHelper::Capture` takes ownership of prepared positions/indices and
computes their canonical content identity. `CookCaptured` retains this immutable
snapshot in the resolver; Describe reads only the identity, and a warm hit never
resolves or hashes source bytes. Resolve and the function verify named input
bytes against that identity on a miss. Detached compilation and Cook transfer
prepared vectors into capture. The convenience `Cook` entry captures an owned
copy of its borrowed input before invoking the same session.

`Physics.CollisionOutput` version 1 contains target/profile, mode/policy,
Simple/Complex presence, geometry kind, double bounds and seven bounded array
counts. Only active arrays appear under the corresponding geometry prefix.
Vertices are f64x3; triangles retain source ordinals; triangle meshes carry BVH
nodes and leaf ordinals, while convex hulls carry planes, half-edges and faces.
PhysicsCore completes these native arrays before returning identity-free cooked
data. Cold output and geometry assembly retain those allocations. Serialized
record validation reads elements without constructing geometry; warm assembly
converts each required native array once, then publishes a new geometry identity.
The output never persists runtime geometry identities or embeds product objects.

The action uses canonical schema 2 and the shared-output version independently
of package collision schema. Engine translates generic resolution/input errors
to the Input stage, recipe/output errors to Cook, and cancellation separately.
Optional persistence failures do not discard usable output. Non-editor builds
keep direct PhysicsCore cooking without loading build services. Package/Cook
collision bytes and owner-side generation/publication checks are unchanged.

## Publication and runtime boundaries

Execution-local cache observers receive bounded causes, action identity and phase.
Without an observer, the executor logs each failed cache operation once with the
function, key and stage. Normal misses are not errors. Optional persistence
failure never invalidates a complete output; public products carry no cache
observation wrapper. Shader counters remain ShaderBuild-owned. Cancellation is
latched between phases, including after persistence: a late cancellation may leave
a reusable atomic cache entry, but cannot return output for publication. Family
managers also check generation and cancellation at their publication boundary.

Render and collision definitions remain independent. Cache success does not
mean an asset is current, applied, GPU-ready or physics-ready. Imported source,
transactions, rollback, Cook publication and runtime resources remain governed
by [Asset Data Lifecycle](AssetDataLifecycle.md),
[Asset Compilation](AssetCompilation.md) and
[Static Mesh Building](StaticMeshBuilding.md).

Game uses cooked values and runtime codecs without the build protocol, source
payloads or authoring modules. Engine's build integration is editor-gated;
RenderCore has no DerivedDataCache dependency. Editor/module shutdown drains
existing family work before releasing build implementations.
