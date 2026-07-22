# Shader Cache

This document defines the current Slang shader loading and cache contract owned by `RenderCore`.

## Ownership and Layers

`GetOrCompileShader()` owns virtual-path resolution, dependency validation, compilation coalescing, and cache storage. `FSlangShaderCompiler` consumes physical source files and produces SPIR-V plus reflection; it does not choose cache paths.

The runtime uses four bounded or reclaimable layers:

- macro-specific dependency manifests on disk;
- content-addressed SPIR-V and reflection artifacts on disk;
- a 128-entry least-recently-used compiled-output cache in the compile service;
- weak shader-map resource entries that share code and RHI shaders only while a shader map still owns them.

## Paths and Identities

The registered shader mount supplies source and cache roots. For the default `/Engine/` mount, sources are under `Engine/Shaders/Slang/` and cache data is under `Engine/ShaderCache/SPIR-V/`.

A virtual shader such as `/Engine/ImGui/Button` maps to this layout:

```text
Engine/ShaderCache/SPIR-V/ImGui/Button.slang/
  Manifests/
    <DependencyKey>.json
  <VariantKey>/
    <EntryPointIdentity>.spv
    <EntryPointIdentity>.reflect.json
```

All dependency and variant keys are exactly 32 hexadecimal characters. Storage rejects malformed keys before constructing a path.

The dependency key includes the virtual shader path, normalized macro definitions, and compiler environment. A manifest stores the source-tree signature and each dependency's normalized path, size, modification time, and content hash.

The variant key includes the virtual shader path, source-tree signature, normalized macros, target settings, and Slang build identity. Artifact filenames use a stable hash of the unsanitized source entry point and requested shader frequency, so separate requests cannot collide through filename sanitization or stage differences.

## Warm and Miss Paths

On lookup, the compile service:

1. normalizes and validates macros;
2. coalesces an identical in-process request through a single-flight record;
3. loads the macro-specific dependency manifest;
4. reuses persisted hashes when dependency size and modification time are unchanged;
5. resolves the Slang dependency graph and hashes content only when the manifest is absent or stale;
6. checks the in-process output LRU, then the disk artifacts;
7. compiles on a miss, loading the Slang module once and deriving each entry-point program from it;
8. atomically publishes each artifact and applies retention maintenance.

`bForceRecompile` skips compiled-output and disk-artifact hits, but still validates dependencies and republishes the successful result.

## Validation and Publication

A disk hit is published only when every requested artifact passes all checks:

- SPIR-V has the expected magic, word alignment, minimum header size, and configured maximum size;
- the recomputed bytecode hash matches the reflection sidecar;
- source entry point and frequency match the request;
- reflection counts, enum values, descriptor indices, push-constant ranges, and file sizes are within runtime bounds.

Any missing, corrupt, incompatible, or malformed record is an ordinary cache miss. Authored Slang source remains authoritative and recompilation repairs the cache.

Writers create same-directory temporary files and atomically replace the target. Readers therefore observe either the prior complete file or the new complete file. Identical requests compile once per process; independent processes use atomic last-writer-wins publication.

## Lifetime and Retention

The compiled-output LRU retains at most 128 request results. Shader-map cache entries hold weak references to `FShaderMapResourceCode` and `FShaderMapResource`; destroying the final shader map releases compiled code and any lazily created RHI shaders. `GetShaderMapResourceCacheStats()` prunes expired entries while reporting the live cache size.

Each `FShaderCacheStore` defaults to at most 64 variant directories and 256 MiB per virtual shader. After a successful publication, maintenance orders variants by last-write time and then name, removes the oldest candidates, and always protects the variant just published. A single protected variant may exceed the byte budget because deleting the only valid result would make a successful publication immediately useless.

Cleanup only considers non-symlink, immediate child directories whose names are valid 32-character hexadecimal keys. It does not remove manifest directories, unknown siblings, or any path outside the resolved shader directory. Removal failure is logged and does not turn a successful compile into a failure.

## Compatibility

Cache schemas are intentionally strict and do not migrate old layouts. Schema, compiler-environment, or identity changes produce misses and new records. Stale records remain safe because validation rejects them and retention eventually removes old variant directories.

## Related Documentation

- [Shader Cache Hardening Plan](../Plans/ShaderCacheHardening.md)
- [Versioning](Versioning.md)
- [Native C++ Tests](../Setup/NativeTests.md)
