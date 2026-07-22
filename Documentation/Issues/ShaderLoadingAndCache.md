# Shader Loading and Cache Issues

Last reviewed: 2026-07-23

## Current Status

Open. The current cache avoids repeated SPIR-V code generation, but its warm path still performs substantial source processing and several cache failure modes do not reliably recover as ordinary misses.

## Scope Reviewed

The review followed the runtime path from virtual shader resolution through Slang dependency discovery, source fingerprinting, disk artifact loading, shader-map reuse, and lazy RHI shader creation. It also inspected the current on-disk cache sample and the RenderCore shader test coverage.

The current cache consists of:

- one source metadata file per virtual shader path;
- one variant directory per source-tree signature and normalized macro set;
- one SPIR-V binary and reflection JSON sidecar per requested entry point;
- one process-wide shader-map resource cache that shares compiled code and RHI shaders.

At review time, the engine cache contained 59 files across 11 variant directories and used 55,161 bytes. The size is currently small, but multiple retained Gizmo and StaticMesh variants demonstrate that stale variant directories accumulate.

## Verified Findings

### P1: Cached SPIR-V integrity and request identity are not validated

`FShaderCacheStore::TryLoad` reads the SPIR-V bytes and then trusts the hash, source entry point, frequency, and reflection data stored in the JSON sidecar. It does not recompute the bytecode hash or validate the SPIR-V magic and alignment. It also does not compare the sidecar entry point and frequency with the current request before reporting a successful cache hit.

Consequences:

- A truncated or mismatched binary can reach RHI shader creation instead of triggering recompilation.
- A valid JSON sidecar paired with the wrong binary can supply a false shader-map identity because the cached hash is not derived from the bytes that were loaded.
- A sidecar with the wrong source entry point or frequency is accepted by the cache service and rejected later by `FShaderMapBase::Initialize`, where there is no retry through the compiler.
- Corrupt reflection enum values and ranges can progress beyond the cache boundary without targeted validation.

Candidate direction:

- Recompute the bytecode hash on load and compare it with the sidecar.
- Validate SPIR-V size, alignment, and magic before publishing a hit.
- Validate source entry point, frequency, reflection enum domains, counts, and ranges against the request and runtime limits.
- Treat every validation failure as a recoverable cache miss and compile again.

### P1: One metadata signature causes macro-dependent dependency variants to displace one another

Dependency discovery uses the active macro environment, but each virtual shader path has only one metadata file and therefore only one stored `SourceTreeSignature`. The variant directory key already includes both the current source-tree signature and normalized macros, yet lookup is additionally gated on the single metadata signature matching.

If different macros select different imported files, alternating between those macro variants overwrites the shared metadata signature. Both variant directories can exist and be valid, but only the most recently written dependency signature is allowed to reach artifact lookup; the other variant recompiles unnecessarily.

Candidate direction:

- Remove the single-signature metadata gate and use the current dependency signature plus macros to address the variant directory directly; or
- persist a dependency manifest per preprocessing environment or variant.

### P1: The process-wide shader-map cache retains RHI resources without a bound

`FShaderMapResourceCache` stores strong references to every cached `FShaderMapResource`. Each resource can retain its lazily created RHI shaders even after all owning shader maps have been destroyed. Entries are cleared only during RenderCore shutdown.

The current static shader set keeps this small, but dynamic material permutations, shader hot reload, or repeated source changes can retain obsolete GPU resources for the remainder of the process.

Candidate direction:

- Store weak references and recreate an entry after its last external owner disappears; or
- introduce an explicit memory/GPU budget and LRU eviction policy.
- Provide source-identity invalidation for hot reload and expose cache statistics for diagnosis.

### P2: A disk-cache hit still performs Slang dependency parsing and first-use content hashing

Before checking compiled artifacts, `FShaderCompileService` creates a Slang dependency session, loads the module, enumerates dependencies, and builds the source-tree signature. On the first request in a process, `FFileFingerprintCache` reads and hashes every dependency. A warm disk hit therefore avoids code generation and reflection, but it does not avoid source parsing or initial source-file I/O.

On a cache miss, the compiler then uses a separate Slang session and calls the module-loading path once for every entry point. Slang may internally reuse work within a session, but the engine still repeats the API path and maintains a separate dependency-resolution load.

Candidate direction:

- Persist dependency paths, file size, modification time, and content hash in a versioned manifest.
- On the warm path, use inexpensive file metadata to reuse stored hashes; resolve the dependency graph and hash file contents only after a relevant change.
- Consolidate dependency discovery and compilation around one Slang session/module on misses.
- Add an in-process compiled-output cache and per-key single-flight so concurrent or repeated requests do not repeat dependency and disk work.

### P2: Artifact identity omits compiler environment and frequency

The variant key contains manually maintained key versions, a backend name, target format, target profile, virtual path, source signature, and macros. It does not automatically include the Slang compiler version or other future optimization, debug, target-environment, or ABI-affecting options.

Entry points are converted to sanitized filenames, while shader frequency is stored only inside the sidecar. Filename collisions are detected only among entry points in one request. Two separate requests whose raw entry points sanitize to the same name can address the same artifact, and requesting the same entry point with a different frequency can reuse a sidecar whose reflection stage flags were built for another request.

Candidate direction:

- Define a compiler-environment identity containing the Slang version and every code-generation or reflection-affecting option.
- Include that identity in the variant key.
- Derive each artifact name from the raw source entry point, requested frequency, and an unambiguous stable hash rather than sanitization alone.

### P2: Cache publication is not atomic and concurrent requests are not coalesced

Compiled binaries, reflection sidecars, and source metadata are written directly and sequentially. A process exit, failed write, or concurrent writer can expose a partially updated variant. Missing files generally recover as misses, but a complete-looking mismatched pair is not currently detected because binary hashes are not verified.

The compile service also has no per-key single-flight operation, so simultaneous identical misses can both compile and write the same files.

Candidate direction:

- Write artifacts into same-filesystem temporary files or a temporary variant directory.
- Flush, close, and validate the complete set before atomically renaming it into place.
- Publish metadata only after the variant is complete.
- Coalesce identical in-process requests; add a narrowly scoped cross-process publication lock if multiple engine processes are expected to share a cache root.

### P2: Disk variants have no retention policy

Every new source signature or macro variant creates another content-addressed directory, and no maintenance path removes obsolete entries. The current footprint is negligible, but material permutations and iterative shader development can make growth unbounded.

Candidate direction:

- Place cache schema/compiler generations under an explicit version namespace.
- Track size and last access, then enforce a configurable global or per-shader budget.
- Confine every cleanup target to the registered shader cache root and make deletion recoverable by recompilation.

## Recommended Resolution Order

1. Make cache loading self-validating and guarantee that invalid data falls back to compilation.
2. Publish complete artifact sets atomically and coalesce identical requests.
3. Remove or redesign the single metadata signature so macro-dependent dependency variants remain reusable.
4. Persist dependency manifests to make warm startup avoid Slang parsing and full source hashing.
5. Complete compiler and artifact identity.
6. Bound process memory, GPU retention, and disk usage.

## Validation Gaps

Existing RenderCore tests cover shader-map resource reuse, cache-key distinctions at the in-memory layer, reflection, pipeline layout construction, parameter binding, and limited shader path behavior. They do not exercise the disk-backed cache store end to end.

Required coverage for a future resolution:

- unchanged warm hit without invoking the compiler;
- root-source and transitive-dependency invalidation;
- alternating macro-dependent import graphs;
- truncated SPIR-V, invalid magic, hash mismatch, malformed sidecar, and mismatched binary/sidecar recovery;
- source entry point sanitization collisions and frequency separation;
- interrupted publication and concurrent identical requests;
- compiler-environment identity changes;
- bounded memory and disk eviction without deleting outside the cache root.

## Related Documentation

- [Shader Cache](../Architecture/ShaderCache.md)
- [Shader Parameters](../Architecture/ShaderParameters.md)
- [Runtime Architecture](../Architecture/RuntimeArchitecture.md)
- [Versioning](../Architecture/Versioning.md)
- [Native C++ Tests](../Setup/NativeTests.md)

## Related Code

- `Engine/Source/Runtime/RenderCore/Private/Shader/ShaderCompileService.cpp`
- `Engine/Source/Runtime/RenderCore/Private/Shader/ShaderCacheStore.cpp`
- `Engine/Source/Runtime/RenderCore/Private/Shader/ShaderCompileUtilities.cpp`
- `Engine/Source/Runtime/RenderCore/Private/Shader/SlangShaderDependencyResolver.cpp`
- `Engine/Source/Runtime/RenderCore/Private/Shader/SlangShaderCompiler.cpp`
- `Engine/Source/Runtime/RenderCore/Private/Shader/Shader.cpp`
- `Engine/Source/Runtime/Core/Private/Misc/FileFingerprintCache.cpp`
- `Engine/Source/Programs/Tests/RenderCoreTests/Private/ShaderFoundationTests.cpp`
- `Engine/Source/Programs/Tests/RenderCoreTests/Private/ShaderReflectionTests.cpp`
