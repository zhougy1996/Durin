# Shader Cache

Summary: Define authored ShaderBuild caching and compiler-free cooked Shader delivery.

Modules: RenderCore, ShaderBuild, DerivedDataCache, RHI

Last reviewed: 2026-10-03

ShaderBuild owns Slang dependency resolution, compilation, request coalescing,
dependency manifests, DDC orchestration, and cooked-library production.
RenderCore owns source-independent request/value types, `DSHD` encoding, the
`DSLB` cooked-library schema/reader, Shader maps, and RHI publication.
DerivedDataCache owns immutable build definitions, persistent sessions, and the
shared lookup/resolve/build/validate/cache protocol. Its public cache boundary
uses structured `FCacheRecord` values; encoding, compression, validation, and byte
storage remain private. Sessions accept inline or Core task scheduler adapters.
ShaderBuild registers its compiler function under the
[shared protocol](../Assets/DerivedDataBuild.md) and retains Shader policy,
compiler scheduling, and single-flight ownership.

## Results and diagnostics

`FShaderOperationResult` is `std::expected<void, FShaderError>`; nested operations
preserve typed causes and context. `FShaderCompilerOutput` derives success from
its error code, defaults to `CompilationNotStarted`, and never accepts partial
stages on failure. `FormatShaderError` formats only at presentation or required
string-adapter boundaries; bounded compiler text never determines classification.
Filesystem failures retain their owned path and native cause.
Generic build failures retain a bounded diagnostic description; they do not
reconstruct discarded structured causes or classify diagnostic text.

Binding may retain a valid prefix on failure; layouts and MaterialShaderMaps
publish only complete candidates, and failed ShaderMap initialization resets
the map. Payload codecs and cooked registration clear failed output; retirement
after inventory freeze retains the registration handle on failure. Semantic
tests assert codes, context, and publication behavior rather than English text.

## Storage layers and ownership

The runtime uses four bounded or reclaimable layers:

- ShaderBuild-private, machine-local dependency manifests;
- one portable DDC value for each exact compiled-output request;
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

`FCompiledShader::Code` retains a `shared_ptr<const FSharedByteBuffer>`. Compiler
output, LRU entries, single-flight results, cooked inputs and RHI readers share
immutable bytecode. Slang adopts its copied blob buffer; its reflection word
conversion remains a separate recipe cost. Package decoding adopts decoded
bytes without changing the package format.

ShaderBuild also provides the private `Shader.Output` schema-1 representation.
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

The variant identity additionally includes explicit key version, Slang backend,
SPIR-V target/profile, compiler-environment identity, root virtual path, portable
source-tree signature, and normalized macros. A schema-2 action binds that variant as `Shader.SourceClosure` version 2,
registered function version 2 and `Shader.Output` version 1. The explicit `Options` byte constant has a 1 MiB bound and schema 1: version,
root virtual path, compiler environment, generated-source flag, ordered
entry-point/frequency pairs, normalized macro name/value-presence/value triples,
and ordered virtual search roots. Counts and integers are fixed-width little
endian; strings are length-prefixed. This is a canonical field codec, never a
native struct image. Force policy remains outside its identity. `bForceRecompile` is execution policy and never enters production
identity.

Generated Shader roots add the generated source-content hash to the portable
source-tree signature. Imported files still use sorted registered virtual paths
and content hashes. The local import manifest may use physical facts and a source
path hint, but those values do not enter the compiled-output DDC identity.
Recovered imports are rechecked against the caller's virtual-prefix allowlist.

## Package compiled-output payload

The separate `DSHD` schema-1, builder-1 little-endian package codec contains the complete
ordered `FShaderCompilerOutput`. The header contains magic, schema, builder,
endianness marker, a zero reserved field, and entry count. Each entry contains:

1. length-prefixed UTF-8 source and binary entry points;
2. `uint32` frequency and zero reserved field;
3. length-prefixed UTF-8 debug name;
4. the two `uint64` halves of the XXH3-128 SPIR-V hash;
5. `uint64` SPIR-V byte count followed by bytes;
6. `uint32` resource count and each name, stage flags, set, binding, type, and array size;
7. `uint32` push-constant count and each stage flags, offset, size, and zero reserved field.

The request supports 1–32 stages. A stage permits at most 64 MiB SPIR-V, 65,536
resource bindings, 65,536 push-constant ranges, 32 KiB per string, descriptor
indices through 65,535, and push-constant extents within 65,536 bytes. The whole
compiled output permits at most 256 MiB. Decode validates header versions, request
identity and ordering, enum masks, all counts and length arithmetic, reserved
fields, SPIR-V minimum size/alignment/magic, recomputed code hashes, reflection
bounds, and complete byte consumption before publishing a candidate.

Archive decode failure leaves the caller output empty; SPIR-V and reflection
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
shutdown. Callers own task scheduling and admission. Generated requests do not
hold a builder-wide lock across dependency validation, cache access, or compilation.
Each Slang compiler/resolver call exclusively leases a global session; all derived
objects are destroyed before returning that lease. Pool locks cover only the idle
inventory, with at most four idle sessions retained per pool. Active contexts follow
the caller's worker/admission budget; acquisition creates a context when none is
idle instead of blocking a worker behind another compilation. Shutdown requires
all calls and their leases to drain before destroying the builder. Module startup
constructs the compiler service and freezes registration before constructing the
builder. Shutdown closes service admission, cancels and drains sessions, and
waits for accepted execution to retire before releasing the builder/service.
Requests retain captured resolvers; their input description runs in the build
session. Synchronous callers use a DDC request owner and wait before reading
completion, independently of whether the session executes inline.

Filesystem-backed generated requests validate dependencies and imports before
single-flight admission using the complete output identity. Captured-source requests
include source contents, artifact contents, ordered search roots, normalized macros,
entry points/stages, compiler identity, and import policy in their flight identity.
Forced and ordinary requests have separate flights. Identical synchronous callers
wait only for their matching flight; unrelated cache hits and compiles can proceed.
Material scheduling coalesces consumers before launching work. Success and failure completion wake waiters and remove the record, allowing
retries. Unexpected recipe exceptions become generic failure completions; direct
pre-session exceptions still use the existing flight exception path.
Reload generation invalidates memoized source fingerprints.

Global and Material Shader owners publish complete typed sets atomically.
Renderer resource slots expose only the requested Ready generation; pending
or failed replacements do not return a previous set. Accepted Material program
installation remains a separate owner transaction.

DurinEditor and Cook-capable tools select ShaderBuild. Its resident
`IShaderBuildModule` is the only live-build path; module absence is an explicit
authored failure. Like MeshBuilder and TextureBuild, it does not support runtime
unloading. Consumers drain their work before normal module shutdown releases
compiler and Shader-data state. The module owns a private `FShaderBuilder`;
its instance contains compiler/cache state and is also passed to cooked-library
production. Tests construct independent builders without replacing module state.
RenderCore build functions adapt directly to the module interface.
RenderCore has no Slang or
DerivedDataCache dependency, and DurinGame selects neither ShaderBuild nor DDC.

## Cooked delivery

Cook freezes the target-eligible non-Material request inventory and asks
ShaderBuild to resolve each exact request through the ordinary memory/DDC/
compiler path. It publishes `Shaders/ShaderLibrary.dslb` in the same Cook
transaction as packages and records it in `CookManifest.bin`.

`DSLB` schema 1 is exact for one target/profile. Its sorted directory maps a
source-independent runtime request identity to one complete embedded `DSHD`
value and records production identity and payload digests. RenderCore preflights
the full header, directory, bounds, alignment, target/profile, inventory closure
and whole-file digest before serving a record. A lazy record load revalidates
its digest and exact request membership.

Launch selects one immutable Shader-data domain before demand. Authored mode
uses the module; Cooked Game mode opens only the qualified library below the
Cook root. Missing, corrupt, incomplete, wrong-target, or wrong-profile data is
a bounded content failure and never falls back to source, manifests, DDC, Slang,
or a compiler module. Material `ProgramData` remains package-owned.

## Compatibility

The former variant directories, per-entry-point `.spv` files, and
`.reflect.json` sidecars are neither read nor migrated. Disposable old data is a
cold miss. Payload readability changes bump schema; output-semantic changes bump
builder/key identity even when the payload remains readable.

## Related Documentation

- [Global Shaders](GlobalShaders.md)
- [Asset Data Lifecycle](../Assets/AssetDataLifecycle.md)
- [File I/O](../Core/FileIO.md)
- [Runtime Variants](../../Development/Build/RuntimeVariants.md)
