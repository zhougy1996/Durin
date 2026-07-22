# Shader Cache Hardening Plan

Last reviewed: 2026-07-23

## Current Status

Implementation in progress. Baseline `RenderCoreTests` passed 26 tests on 2026-07-23. Phases 1 and 2 are complete and Phase 3 is active. Phase 1 passed 32 tests; Phase 2 passed all 38 `RenderCoreTests`, including warm restart, conditional macro dependency, and eight-way concurrent request coverage.

## Goal

Make the runtime shader cache self-validating, cheap on unchanged warm starts, safe under repeated or concurrent requests, and bounded in both process and disk lifetime while preserving automatic recovery through recompilation.

## Scope

- Validate cached SPIR-V, reflection, request identity, and compiler identity before publishing a hit.
- Publish cache records atomically and give every artifact an unambiguous identity.
- Persist macro-specific dependency manifests so unchanged warm starts avoid Slang dependency parsing and file-content hashing.
- Coalesce identical in-process requests and avoid redundant module loads on compilation misses.
- Release unused in-process RHI shader resources and prune stale disk variants within explicit bounds.
- Add native coverage for corruption, invalidation, macro variants, identity, concurrency, and retention.

## Non-Goals

- Remote or shared network shader caches.
- Cross-platform reuse of one cache directory.
- Shader hot-reload UI or editor controls.
- Pre-cooking all shader permutations for packaged builds.

## Design Decisions and Invariants

- Authored Slang source remains authoritative; every cache failure is recoverable through recompilation.
- Cache compatibility is independent of the engine release version and includes explicit schema and compiler-environment identities.
- A dependency manifest is keyed by virtual shader path plus normalized macros, not by the resulting source-tree signature.
- Artifact identity includes the unsanitized source entry point and requested frequency through a stable hash.
- Readers observe only atomically published files and validate bytecode against sidecar metadata.
- In-process request coalescing never holds the cache mutex while compiling or performing disk I/O.
- Cleanup only removes validated immediate children of the resolved shader-cache directory.

## Current Foundations and Gaps

The existing compile service already separates path resolution, dependency discovery, fingerprinting, compilation, reflection, and storage. Variant keys include source content and normalized macros, and `FFileFingerprintCache` avoids repeated hashes within one process. The unresolved gaps are recorded in [Shader Loading and Cache Issues](../Issues/ShaderLoadingAndCache.md).

## Implementation Stages

### Phase 1: Cache Integrity, Identity, and Atomic Publication

- [x] Include the Slang build identity in variant keys.
- [x] Derive artifact names from source entry point and frequency without cross-request collisions.
- [x] Validate SPIR-V structure, bytecode hash, request identity, and reflection bounds on load.
- [x] Publish binary and JSON records through same-directory temporary files and atomic replacement.
- [x] Add disk-cache tests for valid hits, corruption recovery, identity mismatches, and artifact separation.

#### Acceptance Gate

- `RenderCoreTests` passes with new corruption and identity cases.
- No invalid cache record can return `bSucceeded=true`.
- Phase 1 changes and this status update are committed independently.

### Phase 2: Warm-Path Manifests and Request Coalescing

- [x] Replace the single source signature gate with macro-specific dependency manifests.
- [x] Reuse persisted fingerprints on unchanged size and modification time without reading file contents.
- [x] Resolve and persist a fresh dependency graph whenever a manifest is absent or stale.
- [x] Coalesce identical in-process compile-service requests.
- [x] Load one Slang module per compile request and derive all entry-point programs from it.
- [x] Add warm-hit, transitive invalidation, alternating macro graph, and concurrent request tests.

#### Acceptance Gate

- Unchanged warm lookup does not invoke Slang dependency resolution or content hashing.
- Alternating macro-specific dependency graphs remain cache hits after their first compilation.
- Relevant native tests pass and Phase 2 is committed independently.

### Phase 3: Bounded Resource and Disk Lifetime

- [ ] Replace permanent strong shader-map resource ownership with reclaimable entries.
- [ ] Add bounded per-shader disk retention using validated variant directory names and deterministic cleanup.
- [ ] Add resource reclamation and cache-root confinement tests.
- [ ] Update architecture documentation, resolve the issue record, and record final validation.

#### Acceptance Gate

- Destroying the last shader-map owner permits its cached resource and RHI shaders to be released.
- Disk maintenance respects configured bounds and cannot traverse outside the shader-cache root.
- Relevant native tests pass and Phase 3 is committed independently.

## Validation Matrix

| Area | Validation |
| --- | --- |
| Cache load | Valid hit, truncated SPIR-V, invalid magic, hash mismatch, malformed sidecar, entry-point mismatch, and frequency mismatch |
| Identity | Compiler build change, presence-only versus valued macros, sanitized-name collision, and frequency separation |
| Dependencies | Root edit, transitive edit, unchanged warm hit, and alternating macro-selected imports |
| Concurrency | Simultaneous identical requests produce one compile/publication operation |
| Memory | Last external owner release expires the process cache entry |
| Disk | Variant count/size pruning and cleanup-root confinement |
| Integration | Full native test preset, full editor build, and hidden-window DurinEditor shader smoke test |

## Definition of Done

- Every open finding in `Documentation/Issues/ShaderLoadingAndCache.md` is implemented and covered by an automated test or an explicit integration validation.
- All phase commits exist independently and contain their corresponding plan status update.
- Architecture documentation describes the final cache contract rather than the superseded implementation.
- The issue record is marked resolved and points to this plan and the resolving commits.
- Full native tests, a full `all` build, and the hidden-window shader smoke test pass.

## Deferred Follow-ups

- Cross-process locking beyond atomic last-writer-wins publication, unless real shared-cache use demonstrates a need.
- Remote cache transport and packaged shader libraries.

## Related Documentation

- [Shader Loading and Cache Issues](../Issues/ShaderLoadingAndCache.md)
- [Shader Cache](../Architecture/ShaderCache.md)
- [Versioning](../Architecture/Versioning.md)
- [Native C++ Tests](../Setup/NativeTests.md)

## Related Code

- `Engine/Source/Runtime/RenderCore/Private/Shader/ShaderCompileService.cpp`
- `Engine/Source/Runtime/RenderCore/Private/Shader/ShaderCacheStore.cpp`
- `Engine/Source/Runtime/RenderCore/Private/Shader/ShaderCompileUtilities.cpp`
- `Engine/Source/Runtime/RenderCore/Private/Shader/SlangShaderCompiler.cpp`
- `Engine/Source/Runtime/RenderCore/Private/Shader/SlangShaderDependencyResolver.cpp`
- `Engine/Source/Runtime/RenderCore/Private/Shader/Shader.cpp`
- `Engine/Source/Runtime/Core/Private/Misc/FileFingerprintCache.cpp`
- `Engine/Source/Programs/Tests/RenderCoreTests/`
