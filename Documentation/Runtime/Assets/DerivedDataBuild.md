# Derived Data Build Protocol

Summary: Define canonical build identity, captured input bindings, and typed synchronous execution for authored derived data.

Modules: DerivedDataCache, Engine, ShaderBuild

Last reviewed: 2026-09-27

## Ownership

`DerivedDataCache` contains two independent responsibilities in one module:
immutable build definitions with shared execution, and backend-neutral opaque
byte storage. Both depend only on Core. Cache backends do not include build
APIs. There is no separate DerivedDataBuild module, function registry, build
scheduler, remote worker, generic dependency graph or generic single-flight.

Engine owns texture, StaticMesh render and collision adapters. ShaderBuild owns
its shader adapter. Family adapters know their recipes, source representations,
targets, payload codecs, error types and cache policy. TextureBuild and MeshBuilder
remain pure typed transformations. ShaderBuild retains its compiler, dependency
manifests, LRU and single-flight. Existing compilation managers own admission,
reservations, cancellation, latest-wins checks, DLL lifetime and publication.

## Definition and identity

`FBuildDefinition::TryCreate` returns an owned immutable value or a typed
validation error. Accessors expose const fields, inputs and canonical bytes.
The descriptor contains function name/version, constants schema, output type/
schema and cache bucket. Constants are named typed bool, uint64, float, string
or XXH3-128 values. Named input references contain content identity, identity
scheme/version and representation/version; they contain no source pointers.

Encoding uses a domain tag, schema 1, explicit little-endian integer widths,
length-prefixed strings, constant type tags and sorted names. Float negative
zero becomes positive zero; non-finite floats are rejected. Names are bounded
ASCII identifiers, versions and input hashes must be nonzero, duplicate names
within a field class are rejected, and the bucket must be valid. Each field
class permits at most 4,096 entries; string constants total at most 1 MiB.
The definition's canonical bytes determine its XXH3-128 cache key.

Function/translator, input identity, representation and output schema versions
have distinct meanings. A recipe or output-affecting normalization change must
invalidate identity. Source compression, physical import hints, cache location,
persistence policy, cancellation, scheduling and object addresses do not enter
production identity. Family-specific composed identities may remain versioned
hashes, such as StaticMesh reconciliation and Shader's portable variant.

The migrated keys deliberately invalidate prior cache entries, without a legacy
fallback lookup. Authored package and cooked/platform payload schemas are
unchanged. Private `Build*DerivedDataKey`/`Build*DerivedDataKeyBytes` functions
project the same definitions for fixture inspection; they are not independent
encoders or compatibility cache paths.

## Snapshot bindings and typed adapters

Before lookup, execution verifies the resident function descriptor and exact
input reference set, then calls family binding validation. Sorted bindings use
an allocation-free linear comparison; unordered bindings sort references in
O(n log n) without copying owned strings. Duplicates and any changed identity or
representation field fail the match. A source binding
must retain the generation named by the definition. Resolution either produces
that generation or fails; it never silently substitutes current asset state or
reimports a physical filename under an old identity.

Adapters retain family-specific product/error types and own execution-local
prepared values. `Resolve` prepares input only after a miss; `Build` invokes the
recipe; `Validate` checks a complete product; `Decode` and `Encode` use existing
family codecs. `MakeError` and `IsCancelled` preserve family error classification.
There is no universal product variant or opaque payload cast. Cold products move
directly to their consumer, without a serialization/decode round trip.

| Producer | Binding and preparation | Identity-specific fields |
| --- | --- | --- |
| Texture2D | Validated torn-off `FTextureSource`; resolve owned shared mip views on miss | Usage, resolved sRGB, compression quality, alpha mode/threshold, maximum resolution, target/profile |
| TextureCube | Import normalizes to canonical faces or HDR panorama; loaded canonical source uses the same executor | Canonical layout, sRGB, projection version, HDR dimension/exposure, target/profile |
| VolumeTexture | Owned torn-off source; resolve voxel view after lookup | Shape, format/filter, source schema, target/profile |
| StaticMesh render | Retained `FStaticMeshSource`; acquire immutable geometry on miss | Normalization and material-slot reconciliation, producer/schema, target |
| Physics collision | Operation-owned positions/indices; independent geometry identity | Collision mode, query policy, weld settings, producer/schema, target |
| Shader | Captured artifacts or metadata-bound closure resolution | Portable variant, compiler environment, source/include contents, normalized macros, exact ordered entry points/stages |

Texture request capture and metadata lookup do no source payload I/O. Valid
Texture2D, canonical Cube and Volume cache hits do not resolve source bytes.
Noncanonical panorama recovery may normalize before it has a canonical key.
Cube import reuses already normalized immutable images on cold execution.
Cube definitions use one `Source` reference for the complete ordered canonical
source, with layout/representation distinguishing faces from panorama. Cube
constants schema 2 replaces six repeated whole-source identities and invalidates
the prior Cube cache key. Source identity still covers face ordering and pixels.
Texture adapters share internal archive encode/decode helpers while retaining
their own product validation, errors and archive diagnostics.
StaticMesh hits use source metadata even when canonical bulk is unavailable.
Shader hits may validate dependency file facts; they skip source capture and
compilation. A shader miss verifies expected file size/content and supplies a
bounded immutable filesystem to Slang, with no live-file fallback. See
[Shader Cache](../Rendering/ShaderCache.md) for dependency and sharing contracts.

## Execution and failure policy

`ExecuteBuild` is synchronous. The caller keeps adapter, module and context
alive until it returns. Its order is binding validation, lookup, decode and
validation; otherwise resolve, build, validation, encode and store. Corrupt or
incompatible cached data is discarded and rebuilt once within that execution.
A valid hit never invokes the resolver or recipe. Policy independently controls
reads, writes and forced rebuilding, and bounds cache bytes.

Cancellation is latched and checked between phases, including before publishing
success after persistence. Expensive family operations also receive their
existing cancellation control. A late cancellation may leave a complete reusable
cache entry but must not return a product for application. Family managers still
check request generation and cancellation at the publication boundary.

Read/decode failures become observations followed by a miss. Resolve/build/
validation failures return the family's error. Encode and Put are best effort
by default; explicit strict encoding policy is available. Observations retain
typed cache/codec causes, origin, byte counts and phase durations. Normal misses
are not retained as errors. `VisitBuildIssues` visits each failed cache operation
with its original typed cause. Engine logs these once at the execution boundary,
including function, key, stage and bounded detail; public products and completion
records carry no cache diagnostic wrappers. Shader counters remain ShaderBuild
owned. Cache availability never changes source identity or authoring authority.

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
