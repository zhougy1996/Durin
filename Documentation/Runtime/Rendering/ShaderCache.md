# Shader Cache

Summary: Define shader compilation, cache identity, invalidation, persistence, and failure behavior.

Modules: RenderCore, RHI

This document defines the current Slang shader loading and cache contract owned by `RenderCore`.

## Ownership and Layers

`GetOrCompileShader()` owns virtual-path resolution, dependency validation, compilation coalescing, and cache storage. `FSlangShaderCompiler` consumes physical source files and produces SPIR-V plus reflection; it does not choose cache paths.

The runtime uses four bounded or reclaimable layers:

- macro-specific dependency manifests on disk;
- content-addressed SPIR-V and reflection artifacts on disk;
- a 128-entry least-recently-used compiled-output cache in the compile service;
- weak shader-map resource entries that share code and RHI shaders only while a shader map still owns them.

## Paths and Identities

The registered shader mount supplies its source root and may explicitly override its cache root. Default mounts store rebuildable data beneath the active project's `DerivedDataCache/Shaders/SPIR-V/`; without a project, `FPaths::DerivedDataCacheDir()` falls back beneath the engine directory. Each virtual mount root receives a readable, hashed namespace so engine, project, plugin, and nested mounts cannot collide. Old standalone `ShaderCache` directories are not migrated or read.

A virtual shader such as `/Engine/ImGui/Button` maps to this layout:

```text
<Project>/DerivedDataCache/Shaders/SPIR-V/Engine.<MountHash>/ImGui/Button.slang/
  Manifests/
    <DependencyKey>.json
  <VariantKey>/
    <EntryPointIdentity>.spv
    <EntryPointIdentity>.reflect.json
```

All dependency and variant keys are exactly 32 hexadecimal characters. Storage rejects malformed keys before constructing a path.

The dependency key includes the virtual shader path, normalized macro definitions, and compiler environment. A manifest stores the source-tree signature and each dependency's normalized path, size, modification time, and content hash.

Higher-level deterministic compilers may request the current Slang environment
identity and a value-owned dependency fingerprint manifest. Those requests
reuse the compile service's long-lived compiler and dependency resolver; they
do not construct per-request Slang global sessions. The public dependency view
is sorted by virtual source path and contains only virtual paths plus content
hashes; physical source and cache paths remain private to RenderCore.
Callers that need only cache invalidation may instead request one aggregate
source-tree fingerprint. It uses the same macro-specific manifest as ordinary
shader compilation: a warm lookup validates persisted size and modification
metadata, while a missing or stale manifest reparses imports and refreshes
content hashes. This avoids maintaining a separate higher-level dependency
cache. The compile service memoizes aggregate fingerprints for the current
Shader reload generation. Repeated callers perform no filesystem validation;
an applied `renderer.reload-shaders changed|all` request advances the shared
generation, clears those memoized views lazily, and makes the next request
validate the persisted manifest again.

Generated shader roots use a separate owned-memory request. Their virtual path,
source-content hash, normalized macros, imported file fingerprints, compiler
environment, entry points, and frequencies form the variant identity. A
source-hash-specific dependency manifest persists imported-file fingerprints,
including the valid empty-import case. A warm process validates that manifest
before invoking Slang dependency resolution and then rechecks every recovered
import against the caller-selected registered virtual prefixes. Changed source
selects a different manifest; changed imported files invalidate the existing
manifest and resolve the dependency graph again. The source is never
materialized as authored content; SPIR-V/reflection artifacts still use the
ordinary RenderCore cache validation, retention, corruption repair, force-
recompile, and in-process output cache.

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

## Development Reload and Recovery

Failed compiler output is not stored in the in-process output cache or
published to disk. Correcting authored source therefore produces a new
dependency fingerprint and variant key without restarting the compile service.

Renderer exposes two demand-driven reload modes. The
`renderer.reload-shaders changed` command advances the Renderer shader-resource
generation; the next lookup for each demanded shader-backed resource validates
its dependencies and reuses or compiles the resulting variant normally.
`renderer.reload-shaders all` advances the same generation but also sets
`bForceRecompile` on shader candidates first demanded in that generation,
bypassing both successful memory and disk output reuse.

Neither command eagerly recompiles every registered shader type or material
identity. The console request is ordered through the render-command queue, and
resource slots decide when a stale identity is next demanded. Shader refresh
is transactional: a compile, binding, RHI, or pipeline failure leaves a valid
last-known-good payload drawable when one exists, while a successful candidate
replaces it atomically. See [Viewport Rendering](ViewportRendering.md) for the
generation, diagnostic, device-invalidation, and shutdown contracts.

Graphics-pipeline names are stable diagnostic labels, not cache identities.
Every RHI graphics-PSO request creates a fresh complete candidate, even when a
live pipeline has the same name. The demanding Renderer slot owns the logical
PSO together with the typed shader payload used for binding and reload; a
successful refresh therefore replaces both atomically without encoding a
shader generation into the debug name.

## Validation and Publication

A disk hit is published only when every requested artifact passes all checks:

- SPIR-V has the expected magic, word alignment, minimum header size, and configured maximum size;
- the recomputed bytecode hash matches the reflection sidecar;
- source entry point and frequency match the request;
- reflection counts, enum values, descriptor indices, push-constant ranges, and file sizes are within runtime bounds.

Any missing, corrupt, incompatible, or malformed record is an ordinary cache miss. Authored Slang source remains authoritative and recompilation repairs the cache.

Writers use Core's shared atomic byte-publication API, which creates a
fixed-length same-directory temporary name and atomically replaces the target.
The temporary name does not grow with the destination path. Readers therefore
observe either the prior complete file or the new complete file. Identical
requests compile once per process; independent processes use atomic
last-writer-wins publication. Binary, reflection, and dependency-manifest
round trips are supported beyond the traditional Windows `MAX_PATH` boundary
under the physical-path contract in [File I/O](../Core/FileIO.md).

## Lifetime and Retention

The compiled-output LRU retains at most 128 request results. Shader-map cache entries hold weak references to `FShaderMapResourceCode` and `FShaderMapResource`; destroying the final shader map releases compiled code and any lazily created RHI shaders. `GetShaderMapResourceCacheStats()` prunes expired entries while reporting the live cache size.

Each `FShaderCacheStore` defaults to at most 64 variant directories and 256 MiB per virtual shader. After a successful publication, maintenance orders variants by last-write time and then name, removes the oldest candidates, and always protects the variant just published. A single protected variant may exceed the byte budget because deleting the only valid result would make a successful publication immediately useless.

Cleanup only considers non-symlink, immediate child directories whose names are valid 32-character hexadecimal keys. It does not remove manifest directories, unknown siblings, or any path outside the resolved shader directory. Removal failure is logged and does not turn a successful compile into a failure.

The complete `DerivedDataCache` remains disposable while the editor is stopped. Authored Slang sources repopulate missing shader manifests and artifacts.

## Compatibility

Cache schemas are intentionally strict and do not migrate old layouts. Schema, compiler-environment, or identity changes produce misses and new records. Stale records remain safe because validation rejects them and retention eventually removes old variant directories.

## Related Documentation

- [Shader Cache Hardening Plan](../../Plans/Archive/2026-07/ShaderCacheHardening.md)
- [Versioning](../Assets/Versioning.md)
- [Native C++ Tests](../../Development/Build/NativeTests.md)
- [File I/O](../Core/FileIO.md)
