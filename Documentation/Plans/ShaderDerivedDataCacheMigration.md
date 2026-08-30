# Shader Derived Data Cache Migration Plan

Summary: Prepare the generic DDC for concurrent shader workloads and migrate portable compiled Shader outputs out of RenderCore's private filesystem store.

Last reviewed: 2026-08-30

Status: Completed
Completed: 2026-08-30

## Current Status

All stages and acceptance gates are complete. Generic DDC Get/Put operations
share a bounded weak bucket-lock registry while Trim owns only its selected
bucket. RenderCore retains local schema-6 dependency manifests in
`FShaderDependencyManifestStore`, builds portable source identity from sorted
virtual paths and content hashes, and stores one complete schema-1 `DSHD` value
per exact request in the `Shaders/CompiledOutput` bucket. The legacy
`FShaderCacheStore`, variant directories, `.spv` files, and reflection JSON
sidecars are absent.

Focused qualification passes: `DerivedDataCacheTests` (13 passed, one symlink
case skipped because the Windows token lacks symlink privilege),
`RenderShaderCacheTests` (6), `RenderShaderServiceTests` (14), the complete
`@shader` domain including renderer reload/recovery, `MaterialTests` (108),
`MaterialVulkanTests` (1), `AssetPackageTests` (131), and
`TextureCookIntegrationTests` (2). `DevTool.bat test all` passes all 84
registered native targets. Full Debug DurinEditor and DurinGame builds pass.
The generated DurinGame closure contains `DerivedDataCache` and contains none
of TextureBuild, StaticMeshBuild, SkeletalBuild, TerrainBuild,
AssetForgeBuiltins, or editor modules. Changed-document validation and the
repository-wide plan lifecycle validation pass.

## Goal

Make one complete compiled Shader request a portable, validated, disposable DDC
object while preserving source invalidation, compilation coalescing, force
recompile, corruption recovery, best-effort persistence, reload generations,
and typed Shader publication.

After completion:

- dependency manifests remain a machine-local RenderCore optimization and do
  not enter portable DDC identity or shared payloads;
- the full SPIR-V plus reflection result for one exact entry-point/frequency
  request is encoded as one atomically published Shader-owned binary value;
- RenderCore uses the generic DDC Cache API for compiled-output query, store,
  and bounded maintenance, without exposing filesystem paths;
- independent DDC Get/Put traffic can execute concurrently while Trim remains
  safe and bounded within its selected bucket;
- the legacy per-entry-point `.spv`, `.reflect.json`, variant-directory, and
  retention paths are no longer produced or read; and
- missing, incompatible, malformed, or corrupt DDC values remain ordinary
  compile misses and never publish partial Shader output.

## Scope

- Generic DDC synchronization and immutable-buffer behavior required by
  parallel Shader cache traffic.
- Separation of machine-local dependency-manifest identity from portable
  Shader source identity.
- A Shader-owned canonical binary payload for a complete
  `FShaderCompilerOutput` request.
- Stable compiled-output DDC key, bucket, size, cleanup, diagnostics, and
  best-effort store policy.
- Direct RenderCore use of the low-level DDC Cache API; the existing Shader
  compile service remains the build orchestrator for this migration.
- Existing mounted-file and generated-source Shader requests, including their
  imported dependency validation and allowlists.
- Module graph, runtime-variant closure, focused tests, aggregate validation,
  and lasting documentation affected by the migration.

## Non-Goals

- Remote, cloud, shared, memory-tier, Zen-like, or authenticated cache
  backends.
- An asynchronous DDC scheduler, priorities, request merging, dependency
  graphs, remote execution, or moving Shader workers into DDC.
- Multi-value Cache records or partial retrieval of individual Shader stages;
  the current consumer requires one complete request result.
- Migrating machine-local dependency manifests, source timestamps, physical
  source paths, or file-fingerprint reuse state into generic DDC objects.
- Migrating AssetRegistry, Thumbnail, PSO, cooked-output, or unrelated cache
  formats.
- Changing Slang compilation semantics, public Shader types, Global/Material
  Shader map ownership, reload generation fan-out, RHI resource creation, or
  last-known-good publication.
- Creating a cooked Shader library or removing startup/on-demand Shader
  compilation from DurinGame. Those changes are required before a later plan
  can remove DDC and Shader compiler closure from a cooked game target.
- Reading or migrating the legacy Shader artifact layout. DDC contents are
  disposable, so the new key/schema intentionally produces cold misses.

## Design Decisions and Invariants

### Migrate compiled output, not dependency manifests

Dependency manifests retain normalized physical paths, sizes, modification
times, and content hashes so a warm local process can validate unchanged files
without reading their contents. They remain under a private RenderCore-owned
local store. Their path and JSON schema are not DDC contracts, are never copied
to a shared backend, and do not become a Build input value.

Portable source identity is separately defined from sorted registered virtual
dependency paths, content hashes, and any size field retained by the selected
canonical encoding. It excludes physical paths, mount roots, timestamps,
cache paths, request order, and worker order. Equivalent registered source
closures under different physical checkout roots must produce the same
compiled-output DDC key.

### One request is one atomic Shader value

The Shader owner encodes all requested compiled stages and reflection into one
versioned binary value. The envelope has a distinct magic, schema version,
builder version, bounded entry count, and complete-consumption rule. Each entry
contains its source and binary entry-point identities, frequency, debug value,
SPIR-V hash and bytes, resource bindings, and push-constant ranges.

Decode validates request identity, entry ordering, counts, enum domains,
descriptor indices, push-constant ranges, length arithmetic, total allocation,
SPIR-V minimum size, word alignment and magic, recomputed code hashes, reserved
fields, and trailing bytes before returning a complete candidate. The exact
field order and bounds are frozen in Stage 0 and covered by golden bytes.

SPIR-V and reflection are not independent cache values because current
publication accepts only the complete exact request. One DDC object removes the
legacy window where a writer can leave a valid binary beside a missing or stale
reflection sidecar.

### Shader owns Build; DDC owns opaque storage

The first migration uses `FDerivedDataCache::Get`, `Put`, and `Trim` directly.
It does not register an `IBuildFunction` or route compilation through
`FBuildSession`. RenderCore already owns dependency resolution, compiler
lifetime, single-flight requests, memory LRU, force-recompile behavior,
statistics, and typed output publication. Reproducing those responsibilities
inside a DDC function would create two Shader build orchestrators.

The lookup order remains dependency validation, in-process output cache, DDC,
local compilation, best-effort DDC store, then in-process publication. A cache
write or maintenance failure does not invalidate a complete compiler result.
Failed compilation and failed validation are never stored or admitted to the
memory cache.

### Identity and policy are explicit

The DDC key is the canonical lowercase 128-bit hash of the portable Shader
variant identity plus the exact requested entry points and frequencies. The
canonical stream includes distinct key, payload schema, and builder version
tokens; Slang backend, target format/profile, compiler environment identity,
virtual root/path, sorted virtual dependency identities and content hashes,
normalized macros, and request outputs. Any output-semantic change bumps the
builder/key version even when the payload remains readable; any payload field
or validation change bumps the payload schema as required.

`bForceRecompile` is execution policy, not production identity. It bypasses
memory and DDC output queries, retains ordinary dependency validation, compiles
the same stable identity, and best-effort replaces the same DDC object after a
successful build.

Compiled outputs use one selected logical bucket beneath the generic object
layout. Stage 0 freezes its exact name, maximum value size, bucket-wide byte
budget, and bounded deletes per maintenance pass from measured current
requests. RenderCore never derives an entry path or inspects DDC files.

### Concurrency remains synchronous and composable

DDC calls stay synchronous and execute on the Shader caller's existing worker.
The cache facade must not hold one process-wide lock across filesystem I/O.
Ordinary Get/Put operations for unrelated entries may proceed concurrently;
Trim takes exclusive ownership only for the selected bucket while it enumerates
and deletes candidates. The lock registry itself may use a short metadata lock,
but that lock cannot cover reads, atomic publication, enumeration, or deletion.

Readers observe either the prior complete object or the new complete object.
Concurrent identical writers remain safe last-writer-wins publications of the
same immutable identity. Trim cannot escape its bucket, follow symlinks, delete
unrecognized entry shapes, or turn a successful Shader compile into failure.

### Runtime-variant closure is honest

RenderCore currently initializes the Shader compile service in both runtime
variants. This plan therefore makes `DerivedDataCache` a private RenderCore
dependency, which brings the Core-only cache module into DurinEditor and
DurinGame closure without pulling TextureBuild, StaticMeshBuild, SkeletalBuild,
TerrainBuild, AssetForgeBuiltins, or editor modules into the game target.
Physical placement under `Source/Developer` does not override the selected
dependency graph.

This is an explicit transitional runtime contract: DurinGame supports local
on-demand Shader compilation and disposable compiled-output DDC. A later
cooked-Shader-library plan must move compiler ownership behind a tool-selected
boundary, prove source/compiler/DDC-free game startup, and then remove this
dependency from the game closure. This migration does not pretend that cooked
runtime boundary already exists.

## Implementation Stages

### Stage 0: Freeze boundaries, identities, and compatibility baselines

- [x] Inventory mounted-file and generated-source request flows, dependency
  manifest forms, current key streams, cache counters, force-recompile paths,
  maximum observed entry-point counts and payload sizes, and module startup and
  shutdown ordering.
- [x] Freeze the machine-local manifest boundary and select its post-migration
  owner, path, filename grammar, corruption behavior, and retention policy.
- [x] Specify the portable source-identity stream using virtual dependency
  paths and content hashes; prove that no physical path, mount root, timestamp,
  cache location, or request order enters it.
- [x] Specify the compiled-output key stream, exact DDC bucket, schema and
  builder versions, maximum value bytes, bucket byte budget, cleanup delete
  limit, and force-recompile policy.
- [x] Specify the binary payload field order, integer widths, string encoding,
  stage ordering, reserved fields, count/size limits, checksum rules, and
  complete-consumption validation.
- [x] Freeze existing warm-manifest no-content-read, cold/warm compile,
  generated-source import allowlist, malformed artifact, multi-entry-point,
  long-path, retention, reload, and in-flight coalescing behavior in the
  focused test inventory.
- [x] Confirm the selected RenderCore private dependency and DurinGame DDC
  closure, including the explicit absence of asset Build and editor modules.

#### Acceptance Gate

- The manifest/DDC boundary, canonical identities, binary format, bounds,
  cleanup policy, target closure, compatibility strategy, and preserved tests
  are reviewable without an unresolved implementation choice.

### Stage 1: Qualify concurrent generic DDC access

- [x] Replace the process-wide I/O mutex with bounded bucket synchronization:
  parallel ordinary operations and bucket-exclusive Trim, with lock metadata
  lifetime that cannot grow without bound.
- [x] Preserve contained-path checks, two-character sharding, `.bin` objects,
  maximum-value enforcement, immutable results, atomic last-writer-wins Put,
  deterministic bounded Trim, and existing status/diagnostic meanings.
- [x] Avoid copying an immutable cache hit before a Shader-owned decoder can
  consume its `FSharedByteBuffer`; extend `FBuildValue` shared-buffer adoption
  only if the implementation can do so without changing Build semantics.
- [x] Add focused concurrency coverage for unrelated buckets/keys, concurrent
  identical Get/Put, Put versus same-bucket Trim, different-bucket Trim, blocked
  storage, symlinks, malformed requests, and retirement/shutdown.
- [x] Prove existing asset Build session policy, cache corruption recovery,
  cleanup, and best-effort store behavior remain unchanged.

#### Acceptance Gate

- DDC cache tests demonstrate safe concurrent Shader-scale access without a
  filesystem-wide critical section, and existing Build consumers retain their
  exact observable behavior.

### Stage 2: Separate local manifests from portable Shader identity

- [x] Split `FShaderCacheStore` so dependency-manifest load/save and local
  fingerprint reuse have one explicitly machine-local owner independent of
  compiled-output persistence.
- [x] Derive sorted virtual dependency identities from registered mounts while
  retaining physical fingerprints only inside the local manifest and resolver.
- [x] Build source-tree and variant identities from virtual paths and content
  hashes with explicit version bumps; preserve normalized macro, backend,
  profile, compiler-environment, mounted-source, and generated-source inputs.
- [x] Preserve warm manifest validation without source-content reads when
  size/time facts are unchanged, stale-manifest reparsing, generated import
  allowlists, compile-service memoization, and reload-generation invalidation.
- [x] Add fixtures that mount byte-identical Shader closures beneath different
  physical roots and prove equal portable identity while physical or virtual
  content changes miss deterministically.

#### Acceptance Gate

- Local manifests retain their current acceleration behavior, while equivalent
  source closures in different checkout roots produce the same portable Shader
  output key and expose no physical provenance to DDC.

### Stage 3: Introduce the atomic compiled-output payload

- [x] Add Shader-private canonical encode/decode and validation for the complete
  requested `FShaderCompilerOutput` using Core archive and immutable-byte
  primitives without exposing RenderCore types through DDC headers.
- [x] Validate every Stage 0 format and request invariant before allocating or
  publishing a candidate; failure leaves the caller output empty and bounded.
- [x] Add golden single-stage and multi-stage payloads plus truncated,
  oversized, overflow, version, reserved-field, identity, enum, SPIR-V,
  reflection, hash, duplicate/order, and trailing-byte rejection cases.
- [x] Prove a complete compiler output round-trips without JSON, physical
  artifact paths, or RHI resources and remains readable beyond traditional
  Windows path limits through the DDC object boundary.

#### Acceptance Gate

- One versioned Shader value atomically represents and strictly validates every
  artifact required by the exact compile request.

### Stage 4: Route compiled Shader output through DDC

- [x] Replace disk-artifact lookup with bounded DDC Get followed by
  Shader-owned decode/validation; map missing, excessive, incompatible,
  malformed, corrupt, and storage-failure results to the selected compile-miss
  diagnostics without partial publication.
- [x] Preserve the in-process output LRU before DDC, single-flight compile on a
  miss, force-recompile query bypass, compiler failure behavior, and
  last-known-good higher-level Shader-map publication.
- [x] After a successful compile, encode once, best-effort Put the stable DDC
  object, perform one bounded bucket maintenance attempt, and publish the
  complete typed output even when Put or Trim fails.
- [x] Adapt cache metrics and diagnostics to distinguish memory hit, validated
  DDC hit, rebuilt result, corrupt/incompatible miss, store failure, and
  maintenance failure without exposing a physical DDC path.
- [x] Cover mounted and generated sources, exact multi-entry-point requests,
  cold/warm DDC, corruption repair, source/macro/environment/request
  invalidation, force recompile, failed compile, failed store, concurrent
  identical requests, reload changed/all, shutdown, and module retirement.
- [x] Add the private RenderCore dependency, qualify DurinEditor and DurinGame
  module closures, and verify no recipe or editor module becomes reachable from
  the game root.

#### Acceptance Gate

- Every supported Shader compile path uses the memory -> DDC -> compile ->
  best-effort store flow with deterministic portable identity, complete value
  validation, preserved coalescing/publication, and qualified runtime closure.

### Stage 5: Retire legacy artifacts and complete repository qualification

- [x] Remove compiled-output methods and retention state from the old
  `FShaderCacheStore`, then rename the surviving machine-local manifest owner
  and its tests to match its remaining responsibility.
- [x] Remove variant-directory, `.spv`, `.reflect.json`, binary/reflection path,
  per-virtual-shader retention, partial-publication, and direct compiled-output
  filesystem code; keep only manifest paths that remain selected in Stage 0.
- [x] Prove no production compiled Shader path calls `FShaderPaths` for a DDC
  object, parses reflection JSON, enumerates variant directories, or bypasses
  the generic DDC Cache API.
- [x] Update the lasting Shader cache, asset-data lifecycle, code-module,
  workspace/runtime-variant, and file-I/O contracts with only their owned
  behavior; do not leave the implementation plan as a competing specification.
- [x] Run the repository-prescribed focused DDC, Shader cache/contract,
  Global/Material Shader, renderer reload/recovery, module-closure, DurinEditor,
  DurinGame, and complete native validation selected under the testing and
  build/run guides.
- [x] Record exact validation evidence, close every acceptance gate, mark the
  plan completed, and stage and commit the isolated implementation with exact
  Plan and final Stage provenance.

#### Acceptance Gate

- The legacy compiled-output store is absent, lasting contracts describe the
  implemented ownership, both runtime variants have the selected closure, all
  required focused and aggregate validation passes, and the completed plan is
  ready for archival.

## Validation Matrix

| Concern | Required evidence |
| --- | --- |
| Generic cache behavior | Existing Get/Put/Trim status, path, long-path, atomicity, bounds, symlink, cleanup, and Build-session tests remain green |
| DDC concurrency | Parallel unrelated operations, identical-key publication, bucket-exclusive Trim, different-bucket progress, shutdown and failure injection |
| Portable identity | Equal keys across different physical roots; deterministic misses for virtual path, content, macro, compiler, backend/profile, schema, builder, entry-point, or frequency changes |
| Local manifests | Warm metadata-only validation, stale dependency recovery, generated-source import allowlist, corruption, memoization and reload-generation invalidation |
| Shader payload | Golden single/multi-stage bytes, exact round trip, every structural/bounds/hash/request corruption class, no partial candidate |
| Compile flow | Memory hit, DDC hit, cold compile/store, corrupt repair, force recompile, best-effort persistence, compiler failure and single-flight concurrency |
| Rendering consumers | Global, Material and mesh-Material exact-set publication, reload changed/all, last-known-good recovery, RHI resource and pipeline coupling |
| Module graph | RenderCore privately reaches only Core, RHI and DDC additions; DurinGame gains no asset Build or editor modules |
| Runtime variants | DurinEditor and current source-compiling DurinGame start, compile/reuse Shaders and shut down cleanly under their selected closure |
| Repository | Focused targets, full configured builds, complete native aggregate, documentation validation and plan validation pass |

## Deferred Follow-ups

- Extract a tool-selected `ShaderBuild`/Shader compiler owner and introduce a
  cooked Shader library so a cooked DurinGame can start without source, Slang,
  compiler workers, dependency manifests, or DDC.
- Add backend injection and a configured local/shared/cloud hierarchy only
  after a second real consumer or deployment requirement defines promotion,
  authentication, transport, availability, and garbage-collection contracts.
- Add multi-value records, attachment-style large blobs, partial fetch, or
  remote Build execution only when measured consumers require them; this
  Shader payload deliberately does not pre-design those abstractions.
- Evaluate Thumbnail generated previews as a DDC consumer separately; keep
  AssetRegistry snapshots and cooked registry state under AssetRegistry
  ownership.

## Related Documentation

- [Shader Cache](../Runtime/Rendering/ShaderCache.md)
- [Asset Data Lifecycle and Storage](../Runtime/Assets/AssetDataLifecycle.md)
- [Code Modules](../Workspace/CodeModules.md)
- [Workspace And Projects](../Workspace/WorkspaceProjects.md)
- [File I/O](../Runtime/Core/FileIO.md)
- [Global Shaders](../Runtime/Rendering/GlobalShaders.md)
- [Native C++ Tests](../Development/Build/NativeTests.md)

## Related Code

- [`DerivedDataCache`](../../Engine/Source/Developer/DerivedDataCache)
- [`RenderCore`](../../Engine/Source/Runtime/RenderCore)
- [`ShaderDependencyManifestStore.h`](../../Engine/Source/Runtime/RenderCore/Private/Shader/ShaderDependencyManifestStore.h)
- [`ShaderDependencyManifestStore.cpp`](../../Engine/Source/Runtime/RenderCore/Private/Shader/ShaderDependencyManifestStore.cpp)
- [`ShaderDerivedData.h`](../../Engine/Source/Runtime/RenderCore/Private/Shader/ShaderDerivedData.h)
- [`ShaderDerivedData.cpp`](../../Engine/Source/Runtime/RenderCore/Private/Shader/ShaderDerivedData.cpp)
- [`ShaderCompileService.cpp`](../../Engine/Source/Runtime/RenderCore/Private/Shader/ShaderCompileService.cpp)
- [`ShaderCompileUtilities.cpp`](../../Engine/Source/Runtime/RenderCore/Private/Shader/ShaderCompileUtilities.cpp)
- [`ShaderDerivedDataTests.cpp`](../../Engine/Tests/Native/RenderCoreTests/Private/ShaderDerivedDataTests.cpp)
- [`DerivedDataCacheTests.cpp`](../../Engine/Tests/Native/EngineTests/Private/DerivedDataCacheTests.cpp)
- [`DerivedDataBuildTests.cpp`](../../Engine/Tests/Native/EngineTests/Private/DerivedDataBuildTests.cpp)
