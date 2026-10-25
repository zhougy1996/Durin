# Shader Cache

Summary: Define authored ShaderBuild caching and compiler-free cooked Shader delivery.

Modules: Engine, RenderCore, ShaderBuild, DerivedDataCache, RHI

Last reviewed: 2026-10-20

ShaderBuild owns Slang dependency resolution, compilation, request coalescing,
dependency manifests, DDC orchestration, and cooked-library production.
RenderCore owns source-independent request/value types, `DSHD` encoding, the
`DSLB` cooked-library schema/reader, shader-map resource code and RHI publication.
Engine owns typed Global/Material maps, their preparation, and the shared
`FShaderCompilingManager` registered directly with the asset compilation framework.
DerivedDataCache owns immutable build definitions, persistent sessions, and the
shared lookup/resolve/build/validate/cache protocol. Its public cache boundary
uses structured `FCacheRecord` values; encoding, compression, validation, and byte
storage remain private. Sessions accept inline or Core task scheduler adapters.
ShaderBuild registers its compiler function under the
[shared protocol](../../Assets/DerivedDataBuild.md) and retains Shader policy,
compiler scheduling, and single-flight ownership.

## Results and diagnostics

`FShaderCompilerOutput` is shared by direct compilation, synchronous ShaderBuild,
asynchronous completion, shared-output assembly, and manager job groups. It owns
compiled stages, an independent `EShaderBuildStatus`, and an ordered
`std::vector<FShaderCompilerError> Errors`. Success can include warnings; failure
and cancellation do not publish partial stages. Cancellation carries no diagnostics.
Each compiler diagnostic owns its message and optional source path, line and
highlighting fields. Compiler messages retain their wording rather than acquiring
a second Shader-wide error-code taxonomy. Source extraction is for navigation,
not control-flow classification. Diagnostic count and external text are bounded.

`FShaderOperationResult` is `std::expected<void, std::string>` for non-compilation
validation. Filesystem adapters format the existing `FFileError` at that boundary.
`FShaderCompletion` and `TShaderValueResult` remain for non-compilation operations
with cancellation and optional Cook/build products; they do not wrap shader output.
Runtime resource creation uses its existing category, reason, identity and retry
dependencies for control flow and failure fingerprints. Diagnostic wording does
not determine retries or resource failure identity.

Compile execution failures transport the same diagnostic array through transient
DDC observations; partial products and failures are never persisted. Successful
output diagnostics use build metadata and survive cache-record round trips.
Material assembly copies each compiler message into its existing qualified
material diagnostics without a nested Shader cause or a generic failure prefix.
Material Cook preparation preserves cancellation independently of its payload.

Cook stops discovery when Shader input capture fails and preserves its diagnostic;
it propagates Shader cancellation directly rather than polling again to classify
it. Material translation and compilation expose `EMaterialProgramStatus`, and
cancellation produces no error diagnostic or cacheable product. Render-resource
slots also retain cancellation separately, report no failure and permit retry.

Binding may retain a valid prefix on failure; layouts and MaterialShaderMaps
publish only complete candidates, and failed ShaderMap initialization resets
the map. Payload codecs and cooked registration clear failed output; retirement
after inventory freeze retains the registration handle on failure. Behavior tests assert completion state, publication, cancellation and structured
source locations. Text assertions are limited to diagnostic formatting and transport
contracts; diagnostic wording never substitutes for a control-flow reason.

## Storage layers and ownership

The runtime uses bounded or reclaimable layers:

- ShaderBuild-private, machine-local dependency manifests;
- one portable DDC value for each exact compiled-output request;
- an independent native-library DDC product for each source/profile/producer recipe;
- a 128-entry in-process compiled-output LRU;
- weak Shader-map resource entries retained only by live Shader maps.

Dependency manifests remain beneath the registered mount's private cache root:

```text
<MountCache>/<VirtualShader>.slang/Manifests/<DependencyKey>.json
```

They store normalized physical paths, sizes, modification times, content hashes,
and the paired virtual dependency identities. They are a local fingerprint-reuse
optimization, not portable DDC data. Missing, old-schema, malformed, or stale
manifests are reparsed and replaced atomically. A warm manifest validates
unchanged size/time facts without reading source contents.

Compiled output uses the generic DDC bucket `Shader`. Its key is
a canonical lowercase XXH3-128 identity and its filesystem backend currently
maps that opaque key to the ordinary two-character-sharded `.bin` object layout.
RenderCore never constructs or observes that physical path; ShaderBuild uses
the build protocol for compiled outputs.

`FShaderCode::Code` is an `FSharedByteBuffer` value, which already shares immutable
storage; it has no outer shared pointer or allocation. Compiler
output, LRU entries, single-flight results, cooked inputs and RHI readers share
immutable bytecode. Slang adopts its copied blob buffer; its reflection word
conversion remains a separate recipe cost. Package decoding adopts decoded
bytes without changing the package format.

ShaderBuild also provides the private `Shader.Output` schema-7 representation.
Bounded metadata describes ordered source/binary entries, frequencies and hashes;
`Entry/<n>/Code` retains code storage and `Entry/<n>/Reflection` holds canonical
binding and push-constant descriptors. Validation checks the representation
without creating a compiler product or copying bytecode. Assembly reconstructs
debug names and reflection while retaining code blocks, including raw/compressed
cache-record subviews. Production sessions return this representation directly. The compiled-output
archive remains a separate package/Cook codec.

## Portable identity

The source-tree signature is built from the sorted registered virtual dependency
paths and their content hashes. It excludes physical checkout paths, mount cache
roots, timestamps, cache locations, dependency discovery order, and worker order.
Equivalent source closures mounted from different physical roots therefore have
the same portable identity.

ShaderBuild's `IShaderBuildModule::GetBuildVersion()` returns a nonzero opaque
64-bit producer identity, stable until shutdown and compared for equality only.
It combines private compile/reflection revisions with the Slang build tag and,
on macOS, the SPIRV-Cross dependency and private Metal translation revision.
Compiler environment text identifies the toolchain; algorithm revisions remain
private to ShaderBuild. Variant (version 8), dependency (version 3), request and
memory-cache identities include the composed build identity.

The session carries a named 64-bit `BuilderVersion` constant and verifies it
against the compiler service before decoding recipe options. Adapter function
version 5, constants schema 3 and `Shader.Output` schema 7 have independent
ownership. `GetCompileAdapterVersion()` exposes those contracts to authored
Material fingerprints separately from the build identity. One descriptor supplies
registration and test action construction.
The explicit `Options` byte constant has a 1 MiB bound and schema 3: version,
root virtual path, compiler environment, target/backend/code formats and opaque
settings-profile digest, generated-source flag, ordered entry-point/frequency
pairs, normalized macro name/value-presence/value triples, and ordered virtual
search roots. Counts and integers are fixed-width little endian; strings are
length-prefixed. Force policy remains outside production identity.

Generated Shader roots add the generated source-content hash to the portable
source-tree signature. Imported files still use sorted registered virtual paths
and content hashes. The local import manifest may use physical facts and a source
path hint, but those values do not enter the compiled-output DDC identity.
Recovered imports are rechecked against the caller's virtual-prefix allowlist.

## Package compiled-output payload

The separate `DSHD` schema-6 little-endian package codec contains the complete
ordered `FShaderCompilerOutput`. The header contains magic, schema,
endianness marker, a zero reserved field, target/profile, and entry count.
Each `FShaderCode` entry stores source/binary entry points, stage/format/group
metadata, debug name, code-only hash and bytes, canonical named reflection and
push ranges, versioned opaque platform bytes, and complete shader identity.
There is no separately serialized logical interface or name digest; see the
[compiled-stage contract](GlobalShaders.md#atomic-shader-sets).

The request supports 1–32 stages. A stage permits at most 64 MiB code, 65,536
resource bindings, 65,536 push-constant ranges, 32 KiB per string, descriptor
indices through 65,535, and push-constant extents within 65,536 bytes. The whole
compiled output permits at most 256 MiB. Decode validates header versions, request
identity and ordering, enum masks, all counts and length arithmetic, reserved
fields, target code shape, recomputed code hashes, reflection bounds, opaque
metadata compatibility, complete shader identity, and complete byte consumption
before publishing a candidate. ShaderFormat owns code and metadata admission.

Archive decode failure leaves the caller output empty; code and reflection
are never independently published or accepted. Session cache records instead
use the shared-output validator; rejection triggers one uncached compile.

## Request flow and failure policy

The ShaderBuild compile service performs:

1. macro validation and existing identical-request single-flight admission;
2. local dependency-manifest validation or captured-artifact dependency resolution;
3. explicit definition/action construction and in-process output-LRU query;
4. session lookup, cache-record integrity and shader metadata/block validation;
5. on a miss, bounded capture and verification of the identified closure;
6. registered compilation using only explicit constants and captured byte inputs;
7. immutable shared output and optional cache-record persistence;
8. compiler-output assembly retaining bytecode, then LRU admission.

Mounted-source resolution checks expected size and content hash before binding
immutable files to the compiler. A changed file fails with
`DependencyContentConflict`; newer bytes never compile under the earlier key.
The compiler uses the captured filesystem without a live-file fallback. Already
captured source requests and generated roots use the same definition/executor;
physical source paths are mapped to registered virtual identities when available.
Module identities omit `.slang`; captured file names retain it. The resolver
stores ordered virtual paths in the bounded `FileTable` input value, with empty
generic metadata, and aliases immutable capture buffers into `File/<ordinal>`
values. Optional generated source occupies its own value. Explicit admission
supports the existing 65,536-file limit and 4,096-byte path bound; descriptor
bytes count against the aggregate input byte budget. The
function restores the compiler filesystem map by retaining the same immutable
blocks, without copying file contents. It recomputes the
portable closure/variant identity before compiling, including generated root
bytes, and rejects mismatches. No typed options or product sidecar crosses DDC.
Warm hits skip capture/compilation; manifest validation still inspects file facts.

Force recompile bypasses memory and persistent output reuse, retains dependency
validation, and best-effort replaces the same key. Compiler or validation failure
never enters cache or LRU. Encode/Put failure is diagnosed and counted without
invalidating a usable compiler result. Statistics distinguish memory hits,
validated cache hits, compilations, corrupt misses, and store failures. The
`ContentReads` statistic counts fingerprint-cache hashing reads, not all compiler
or capture filesystem reads. Requests perform no cache scan, eviction or Trim.

## Concurrency and lifecycle

DDC Get and Put hold a shared lock for their logical bucket. The weak-entry lock
registry does not grow with every historical bucket, and its short mutex never
covers filesystem I/O. Different buckets and same-bucket operations may progress
concurrently. Atomic replacement gives identical writers last-writer-wins
publication and readers a prior or new complete object.

ShaderBuild owns single-flight records, compiler contexts, memory caching, and
shutdown. Mounted and generated requests share subscriber admission, compiler
flights, completion/repair and the output LRU. Public live jobs enter through
Engine's `FShaderCompilingManager::SubmitJobs`; generated batches and mounted
global Shader requests share bounded dispatch, indexed result assembly, priority,
cancellation and end-boundary waits. A group permits two active jobs and checks
the caller's aggregate retained-output limit before dispatching further work.
Synchronous compilation submits through the same manager and waits at the caller
boundary. Backend submission methods on `IShaderBuildModule` are private to the
manager; the old public synchronous module entry points are removed.
Callers own their material/asset operations. Filesystem-backed
mounted source discovery uses a per-input identity gate; it covers neither
compilation nor DDC I/O. Other source identities prepare independently.
Each Slang compiler/resolver call exclusively leases a global session; all derived
objects are destroyed before returning that lease. Idle contexts are reused only
within their originating thread lifetime; asynchronous continuations never migrate
a pooled Slang context between threads. Pool locks cover only the idle
inventory, with at most four idle sessions retained per pool. Active contexts follow
the caller's worker/admission budget; acquisition creates a context when none is
idle instead of blocking a worker behind another compilation.
Thread identity uses a retained per-thread token rather than `std::thread::id`,
which may be recycled after a thread exits. A new thread therefore cannot inherit
a departed thread's Slang context even if the OS reuses its identifier.
Shutdown requires all calls and their leases to drain before destroying the builder. Module startup
constructs the compiler service and freezes registration before constructing the
builder. Shutdown closes builder admission, cancels and joins accepted subscribers, then
closes and drains both service sessions before releasing compiler contexts.
Requests retain captured resolvers; their input description runs in the build
session. Synchronous callers use a DDC request owner and wait before reading
completion, independently of whether the session executes inline.

Filesystem-backed generated requests validate dependencies and imports before
single-flight admission using the complete output identity. Captured-source requests
include source contents, artifact contents, ordered search roots, normalized macros,
entry points/stages, compiler identity, and import policy in their flight identity.
Generated roots use the `/Generated/` namespace. Optional source-tree descriptions
carry the caller's fingerprint, entry points/stages, macros and complete-closure
declaration; ShaderBuild verifies those bytes without material-specific defaults.
Compiler flights use a request-owner submission barrier acquired before publishing
the joinable flight, preserving request end across inline completion and hit repair.
Forced and ordinary requests have separate flights. Each subscriber owns
one move-only handle with nonblocking cancellation, mutable priority and explicit
wait. Rejection has no callback; acceptance completes exactly once, including
inline completion before submission returns. Canceling one subscriber preserves
a live follower; the last cancellation withdraws the flight from joinable lookup
and requests cooperative producer cancellation. Source validation returns a
validated recipe before output lookup; only a newly admitted flight leader
encodes its DDC definition and creates the captured resolver. Macro options are
normalized once for that recipe. Flight priority is the highest
live subscriber priority. Task-backed DDC sessions release dispatch workers while
cache I/O progresses. Synchronous APIs submit through the same pipeline
and wait at their caller boundary. Callback-stack waits return `WouldBlock`.
Admission bounds are 512 subscribers, 128 compiler flights and 1 GiB captured
source bytes; the 128-entry output LRU is additionally capped at 256 MiB.
`MaximumOutputBytes` applies to the complete build product on both DDC and LRU
paths through the same output-limit validation, including metadata/reflection.
The LRU retains the immutable build output alongside the typed result and charges
serialized data, typed reflection, push constants and Metal binding allocations;
shader code buffers remain shared.
Material scheduling coalesces consumers before launching work. Success and failure completion wake waiters and remove the record, allowing
retries. Unexpected recipe exceptions become generic failure completions; source-preparation
exceptions also deliver terminal failures through the subscriber.
Reload generation invalidates memoized source fingerprints and refreshes their
content hashes. Material generated requests verify their expected environment
fingerprint against immutable captured bytes before output lookup, including
caller-supplied artifacts; changed dependencies cannot compile under an old
material identity. Mounted and generated invalid typed cache hits retry once
with query disabled. Source preparation, retained output caching, Cook input
capture and shared-build adaptation are separate private components within
ShaderBuild; Slang remains the compiler backend. Test cache reset reuses service
initialization while retaining the resident, thread-affine compiler pool.

Global and Material Shader owners publish complete typed sets atomically.
Renderer resource slots expose only the requested Ready generation; pending
or failed replacements do not return a previous set. Accepted Material program
installation remains a separate owner transaction.

DurinEditor and Cook-capable tools select ShaderBuild. Its resident
`IShaderBuildModule` is the independent live-build backend; module absence is an explicit
authored failure. Like MeshBuilder and TextureCompressor, it does not support runtime
unloading. Consumers drain their work before normal module shutdown releases
compiler and Shader-data state. The module owns a private `FShaderBuilder`;
its instance contains compiler/cache state and is also passed to cooked-library
production. Tests construct independent builders without replacing module state.
Engine compilation entry points invoke the backend through the shared manager.
Engine selects ShaderBuild as an optional private dependency, so Engine shutdown
closes admission and drains manager work before ShaderBuild destroys compiler/cache
state. The backend closes and joins its own requests independently. Test service
reset advances the existing Shader reload generation, invalidating prepared CPU
outputs without depending on Engine. Neither RenderCore nor ShaderBuild depends
on Engine.
RenderCore has no Slang or
DerivedDataCache dependency, and DurinGame selects neither ShaderBuild nor DDC.

## Cooked delivery

MacOS Game selects a distinct `MetalLibraryShaderTarget`; authoring and
EditorValidation preserve the original `Msl20Source` profile. ShaderBuild first
reuses the ordinary MSL stage product, then submits an independent
`Durin.Metal.Library` build action through a continuation. The native action
keys explicit MSL bytes, immutable binary profile and compiler/SDK version and
build provenance. Host installation/scratch paths are excluded. It stores one
opaque library per stage in the Shader DDC bucket; logical reflection, entry
and compact binding metadata remain with the final Shader artifact. Force
bypasses both source and library reuse. Strict binary requests never substitute
source output when native production fails.

The native producer explicitly selects Xcode's `Metal` toolchain, bounds active
processes and producers to two, captures at most 16 KiB of diagnostics, and
cancels/reaps its process group on cancellation or a 120-second deadline. Source
constants are limited to 1 MiB, intermediate and library output to 64 MiB each;
the caller's aggregate output budget still applies. Native action cache hits
need no producer execution once its explicit provenance has been captured.
The default high-level binary request discovers that provenance locally.
An owner can capture `GetNativeLibraryProducerIdentity` earlier and supply it as
`FShaderBuildRequestOptions::NativeProducerIdentity`. That path performs no SDK
discovery. `bNativeCacheOnly` requires explicit provenance and forbids native
local execution; a miss or corrupt value fails rather than invoking tools.
Source-only requests and forced cache-only requests reject those options.

Cook freezes the target-eligible non-Material request inventory and asks
ShaderBuild to resolve each exact request through the ordinary memory/DDC/
compiler path. It publishes `Shaders/ShaderLibrary.dslb` in the same Cook
transaction as packages and records it in `CookManifest.bin`.

`DSLB` schema 6 is exact for one target/profile. Its sorted directory maps a
source-independent runtime request identity to one complete embedded `DSHD`
value and records production identity and payload digests. RenderCore preflights
the full header, directory, bounds, alignment, target/profile, inventory closure
and whole-file digest before serving a record. A lazy record load revalidates
its digest and exact request membership.

Launch selects one immutable Shader-data domain before demand. Authored mode
uses the module; Cooked Game mode opens only the qualified library below the
Cook root. Missing, corrupt, incomplete, wrong-target, or wrong-profile data is
a bounded content failure and never falls back to source, manifests, DDC, Slang,
or a compiler module. Material `ProgramData` remains package-owned. MacOS Game
libraries and material programs from the former MSL profile require recooking.
The native loader reads binary bytes and resolves the recorded function;
`metallib` loading does not imply that device-specific PSOs are already created.

## Compatibility

Runtime request identity has its own hash version, independent of the DSLB file
schema. Changing the container layout does not change request membership keys.

DSHD and DSLB omit the producer version word. DSLB uses a 108-byte
header; older payloads and libraries require recook. Algorithm or compiler upgrades invalidate
build/Cook identity without rejecting compatible existing payloads or libraries.
Magic, schema, byte-order marker, target/code format, request membership, reflection
and binding-remap semantics, bounds and content digests remain validation gates.
Cook input and library production identities include the compile build identity,
private library production revision, adapter version/constants/output schemas,
DSHD and DSLB schemas, plus the appropriate sources or runtime requests.

The former variant directories, per-entry-point `.spv` files, and
`.reflect.json` sidecars are neither read nor migrated. Disposable old data is a
cold miss. Payload readability changes bump schema; output-semantic changes bump
builder/key identity even when the payload remains readable.

Loading owners can retain the same ShaderMap and pipeline resources through
[load preparation](../Pipeline/LoadPreparation.md) before covered scene drawing.

## Related Documentation

- [Global Shaders](GlobalShaders.md)
- [Asset Data Lifecycle](../../Assets/AssetDataLifecycle.md)
- [File I/O](../../Core/FileIO.md)
- [Runtime Variants](../../../Development/Build/RuntimeVariants.md)
