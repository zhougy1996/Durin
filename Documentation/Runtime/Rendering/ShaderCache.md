# Shader Cache

Summary: Define authored ShaderBuild caching and compiler-free cooked Shader delivery.

Modules: RenderCore, ShaderBuild, DerivedDataCache, RHI

Last reviewed: 2026-08-30

ShaderBuild owns Slang dependency resolution, compilation, request coalescing,
dependency manifests, DDC orchestration, and cooked-library production.
RenderCore owns source-independent request/value types, `DSHD` encoding, the
`DSLB` cooked-library schema/reader, Shader maps, and RHI publication.
DerivedDataCache owns only synchronous
opaque `bucket + key -> immutable bytes` persistence and bounded bucket
maintenance. It does not schedule or execute Shader builds.

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

Compiled output uses the generic DDC bucket `Shaders/CompiledOutput`. Its key is
a canonical lowercase XXH3-128 identity and its filesystem backend currently
maps that opaque key to the ordinary two-character-sharded `.bin` object layout.
RenderCore never constructs or observes that physical path; ShaderBuild uses
only the bucket/key API.

## Portable identity

The source-tree signature is built from the sorted registered virtual dependency
paths and their content hashes. It excludes physical checkout paths, mount cache
roots, timestamps, cache locations, dependency discovery order, and worker order.
Equivalent source closures mounted from different physical roots therefore have
the same portable identity.

The variant identity additionally includes explicit key version, Slang backend,
SPIR-V target/profile, compiler-environment identity, root virtual path, portable
source-tree signature, and normalized macros. The compiled-output key adds the
payload schema, builder version, and the exact ordered entry-point/frequency
request. `bForceRecompile` is execution policy and never enters production
identity.

Generated Shader roots add the generated source-content hash to the portable
source-tree signature. Imported files still use sorted registered virtual paths
and content hashes. The local import manifest may use physical facts and a source
path hint, but those values do not enter the compiled-output DDC identity.
Recovered imports are rechecked against the caller's virtual-prefix allowlist.

## Compiled-output payload

One `DSHD` schema-1, builder-1 little-endian binary value contains the complete
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
DDC value permits at most 256 MiB. Decode validates header versions, request
identity and ordering, enum masks, all counts and length arithmetic, reserved
fields, SPIR-V minimum size/alignment/magic, recomputed code hashes, reflection
bounds, and complete byte consumption before publishing a candidate.

Missing, excessive, incompatible, malformed, or corrupt values are ordinary
compile misses. Decode failure leaves the caller output empty; SPIR-V and
reflection are never independently published or accepted.

## Request flow and failure policy

The ShaderBuild compile service performs:

1. macro validation and identical-request single-flight admission;
2. local dependency-manifest validation or dependency resolution;
3. in-process output-LRU query;
4. DDC Get and complete Shader-owned decode;
5. local Slang compilation on a miss;
6. one encode and best-effort DDC Put;
7. one bounded bucket Trim attempt;
8. complete typed output publication and LRU admission.

Force recompile bypasses steps 3 and 4, retains ordinary dependency validation,
and best-effort replaces the same DDC key after success. Compiler or validation
failure is never stored or admitted to the LRU. Put or Trim failure is diagnosed
and counted independently but does not invalidate a successful compiler result.
Statistics distinguish memory hits, validated DDC hits, compilations, corrupt
DDC misses, store failures, and maintenance failures without exposing a path.

The DDC bucket budget is 2 GiB. Each post-compile maintenance pass deletes at
most 16 oldest canonical entries. Trim ignores symlinks and unrecognized shapes,
never leaves its selected bucket, and may report a bounded partial result.

## Concurrency and lifecycle

DDC Get and Put operations hold a shared lock for their logical bucket; Trim
holds that bucket's exclusive lock. The lock registry holds only weak entries,
so metadata cannot grow with every historical bucket, and its short mutex never
covers filesystem I/O. Different buckets and ordinary same-bucket operations
may progress concurrently. Atomic file replacement gives identical writers
safe last-writer-wins publication and readers a prior or new complete object.

ShaderBuild owns workers, single-flight records, compiler lifetime,
memory caching, and shutdown. Reload generation invalidates memoized source
fingerprints. Global and Material Shader owners continue to publish complete
last-known-good typed sets atomically; this storage migration does not change
their generation or RHI-resource contract.

DurinEditor and Cook-capable tools select ShaderBuild. Its module-owned
`IShaderBuildProvider` is the only live-build path; provider absence or
retirement is an explicit authored failure. RenderCore has no Slang or
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
uses the provider; Cooked Game mode opens only the qualified library below the
Cook root. Missing, corrupt, incomplete, wrong-target, or wrong-profile data is
a bounded content failure and never falls back to source, manifests, DDC, Slang,
or a compiler provider. Material `ProgramData` remains package-owned.

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
