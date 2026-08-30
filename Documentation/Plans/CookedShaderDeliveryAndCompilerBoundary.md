# Cooked Shader Delivery and Compiler Boundary Plan

Summary: Cook every DurinGame-reachable non-Material Shader into a validated library and move live compilation, Slang, manifests, and DDC access behind a tool-selected ShaderBuild module.

Last reviewed: 2026-08-30

Status: Active
Completed:

## Current Status

Planning is complete enough to begin Stage 0; no implementation stage has
started. The completed Shader DDC migration gives every exact live compile
request one portable, validated `DSHD` value and keeps machine-local dependency
manifests separate. RenderCore nevertheless still initializes the compile
service in both runtime variants, privately depends on `DerivedDataCache`,
links and deploys Slang, resolves source dependencies, and permits on-demand
compilation from Shader source in DurinGame.

Material has already crossed the cooked boundary: each cooked Material owns a
complete target-qualified `ProgramData` value and Runtime needs no authored
program, generated source, DDC, or compiler. Fixed Global Shaders and finite
feature-owned Shader programs, including any DurinGame-reachable UI backend
programs, do not yet have an equivalent cooked delivery authority.

This bounded plan is sufficient and does not require a roadmap. It targets the
current Win64 Game profile, the finite Shader consumer closure selected by
DurinGame, and one complete library. Cross-platform distribution, streaming,
incremental patch libraries, remote DDC, pipeline archives, and PSO caches are
separate outcomes.

## Goal

Make the cooked Shader library the sole non-Material Shader-code authority for
Cooked DurinGame while retaining live source/DDC compilation for authoring and
Cook tools behind one Developer module.

After completion:

- Cook freezes the exact target-eligible non-Material Shader request inventory,
  produces every complete output through one ShaderBuild owner, and atomically
  publishes one target/profile-qualified library;
- Cooked RenderCore validates and serves immutable SPIR-V plus reflection from
  that library without computing source identities or consulting Shader source,
  local manifests, DDC, Slang, or compiler workers;
- authored/editor execution retains dependency validation, in-flight
  coalescing, memory reuse, DDC query/store/repair, force recompile, reload
  generations, and last-known-good publication;
- `ShaderBuild` owns Slang, dependency manifests, compile workers, portable DDC
  production, and cooked-library production while RenderCore owns Shader value
  types, runtime request identity, library schema/reader, Shader maps, and RHI
  publication;
- DurinGame module and deployment closure contains neither ShaderBuild,
  DerivedDataCache, Slang, Shader source nor dependency manifests; and
- missing, incomplete, incompatible, corrupt, wrong-target, or wrong-profile
  cooked Shader data produces an explicit bounded failure and never falls back
  to live compilation.

## Scope

- Inventory and stable identity for every non-Material Shader request reachable
  from the current DurinGame closure, including Global Shader exact sets and
  finite feature/backend-owned programs.
- Target/profile eligibility that excludes editor-only Shader consumers from a
  Game library without relying on physical source location or incidental module
  load order.
- A RenderCore-owned cooked Shader library schema, atomic publication contract,
  strict preflight, bounded lazy payload reads, and immutable decoded outputs.
- Reuse of the existing validated `DSHD` compiled-output encoding, with any
  required ownership/API adjustment but no duplicate SPIR-V/reflection schema.
- A Developer `ShaderBuild` module and module-owned provider registration for
  source dependency inspection, live/generated compilation, DDC interaction,
  and Cook production.
- One explicit RenderCore Shader-data domain selected before Shader demand:
  authored/live-build or cooked-library.
- Cook orchestration, output placement, manifest integration, diagnostics,
  runtime loading, device recovery, module/deployment closure, tests, and
  lasting documentation.

## Non-Goals

- Changing Material `ProgramData`, Material graph compilation, Material
  inheritance, Material/mesh Shader maps, or embedding Material programs again
  in the global cooked Shader library.
- Shared/cloud/remote DDC, backend injection, cache promotion, authentication,
  transport, garbage collection, or remote Build execution.
- Vulkan pipeline-cache or PSO capture, driver-specific pipeline binaries,
  pipeline-library distribution, or changing RHI pipeline creation policy.
- Runtime Shader compilation, mod Shader source, downloadable Shader source,
  or an emergency source/DDC fallback in Cooked mode.
- Multi-platform or multi-profile data in one library. The first format is
  exact for one explicit Cook target/profile; another target produces another
  library.
- Incremental patch libraries, chunk streaming, optional install bundles,
  deduplication across Cook roots, compression, encryption, or signing.
- Moving Renderer-owned Global Shader declarations into RenderCore or merging
  feature-owned Shader programs into one monolithic Global Shader set.
- Changing render-resource generations, exact-set publication, parameter
  binding, merged layouts, device-loss ordering, RHI object ownership, or
  last-known-good behavior except to select compiled bytes from the cooked
  authority.
- Treating editor-only Texture preview programs as Game requirements merely
  because their APIs share RenderCore compile vocabulary.

## Design Decisions and Invariants

### Runtime and production identities are distinct

Cooked Runtime cannot reconstruct a DDC key because that key intentionally
includes source-tree content, compiler environment, builder versions, macros,
and production facts unavailable without authoring inputs. The cooked library
therefore records two identities:

- a stable runtime request identity derived only from category, owner, logical
  request/set name, stable Shader type identities or feature entry names,
  ordered entry-point/frequency membership, target, profile, and runtime schema;
- an opaque production identity containing the exact compiled-output/DDC key
  and output digest used by Cook for diagnostics, determinism, and provenance.

Runtime lookup uses only the first identity. It never hashes source files,
queries manifests, asks for a compiler environment, or follows a DDC reference.
Duplicate runtime identities, one identity with inconsistent membership, and
one production identity claimed by incompatible runtime records are Cook
failures.

Global Shader exact-set identity remains caller-named and type-complete. A
small owner-specific contribution seam covers finite non-Global programs that
remain reachable in DurinGame; it does not turn them into Global Shader types
or expose their RHI/backend objects to Cook.

### One selected inventory is complete for the target

Each contributing module owns stable descriptors for its finite Shader
requests and an explicit target/profile eligibility predicate. Global Shader
registration adapts registered exact sets into the same Cook inventory;
feature/backend modules register their own descriptors through a module-owned
resource/callback lifetime.

Cook freezes inventory only after every module in the selected Runtime closure
has completed Shader registration. Ordering before the freeze is irrelevant;
the frozen table is sorted canonically. Registration after freeze, duplicate
identity, missing owner, empty request, unstable type identity, unsupported
target/profile, or a required request that cannot be produced fails Cook.

Editor-only eligibility is a declared fact, not inferred from a name such as
`EditorGrid`, a source directory, or whether an Editor happened to load the
type. Stage 0 records the exact current DurinGame and DurinEditor inventories
and resolves every ambiguous consumer before format work begins.

### RenderCore owns the readable format

RenderCore owns the cooked library header, directory entries, runtime request
identity encoding, bounds, target/profile vocabulary, digests, decoder,
immutable output handles, and compatibility policy. ShaderBuild may call a
narrow producer-facing encoder owned by RenderCore but cannot define a second
runtime SPIR-V/reflection representation.

The library is one immutable file beneath the target/profile Cook root. Stage 0
freezes its exact relative path, magic, schema, builder identity, byte order,
alignment, header, directory, string table if selected, record ordering,
offset/size arithmetic, per-record digest, whole-file extent/digest, and maximum
counts and bytes. Records contain or reference complete existing `DSHD` values;
SPIR-V and reflection are never independently readable.

Publication uses the repository Cook transaction and shared atomic byte
publication boundary. Runtime preflight validates the complete header,
target/profile, directory ordering and uniqueness, ranges, alignment, extent,
whole-file digest, and required-inventory closure before any record is admitted.
Individual payload bytes may be read lazily, but their digest and `DSHD`
request identity are validated before publication.

### ShaderBuild is the only live-build owner

`ShaderBuild` is a Developer module selected by authoring and Cook roots and
excluded from DurinGame. It owns:

- Slang compiler and dependency resolver lifetime;
- local dependency-manifest storage and file-fingerprint reuse;
- compile workers, request single-flight, memory output LRU, force-recompile
  handling, and authoring diagnostics;
- compiled-output DDC key/query/validation/store/maintenance orchestration;
- generated-source/import allowlist handling; and
- Cook inventory production and library encoding/publication orchestration.

RenderCore retains public value/request types used by Shader maps and higher
level deterministic compilers. Existing live-build entry points become a
facade over one module-owned ShaderBuild feature. When the Authored domain is
selected and the provider is absent or retiring, the request fails explicitly;
RenderCore never silently creates a compiler. Provider retirement closes
admission, completes or cancels accepted work according to the frozen policy,
drains callbacks/resources, and leaves no callable function pointer in
RenderCore.

The provider may depend privately on RenderCore and DerivedDataCache.
RenderCore drops its DerivedDataCache and Slang dependencies and never includes
provider-private headers. The dependency direction is `Core/RHI -> RenderCore
<- ShaderBuild -> DerivedDataCache` at the relevant public/private seams, with
Renderer and Engine consuming RenderCore contracts only.

### Shader-data domain is immutable per runtime lifetime

RenderCore receives one immutable Shader-data configuration before the first
Shader map, renderer resource, UI backend resource, or pipeline demand:

- `Authored` requires an active ShaderBuild provider and permits live source,
  local manifests, memory/DDC reuse, compilation, reload, and force recompile;
- `Cooked` requires an absolute normalized Cook root, explicit target/profile,
  successful library preflight, and forbids provider, source, manifest, DDC,
  compile, reload-all force compilation, or repair fallback.

Initialization may reopen only after complete shutdown. Replacing a live
configuration, demanding Shader code before initialization, selecting Cooked
without a library, or loading a library for another target/profile is an
explicit failure. The configuration contains no Engine object, package,
mutable command-line view, or physical Shader source path. Launch adapts the
selected Runtime/Cook domain into RenderCore before Renderer demand without
creating a RenderCore dependency on Engine.

Authored reload behavior remains unchanged. Cooked `changed`/`all` source
reload commands are unavailable; device invalidation and manual resource retry
may reread already-qualified library values but cannot revise production
identity or compile.

### Cook is strict; Runtime has no authoring fallback

Cook may reuse a validated in-process result or DDC hit, but it copies the
complete value into Cook ownership. A DDC store failure does not invalidate a
complete candidate; a missing/corrupt DDC value rebuilds through ShaderBuild.
A compiler failure, incomplete inventory, duplicate record, encode error,
target/profile mismatch, or publication failure fails the Cook transaction and
preserves the prior complete output.

Cooked Runtime treats a missing/corrupt/incompatible required record as a
content/deployment failure. No synthetic Shader, source lookup, DDC query,
Slang load, or best-effort omission is allowed. Existing same-device
last-known-good payloads may survive a later lazy-read or device-resource
attempt only when they were already produced from the same qualified library
generation; no payload crosses a target, profile, library, or device identity.

Material programs remain package-owned `ProgramData`. Their SPIR-V may share
compiler mechanics during Cook but is not copied into this library, and cooked
Material loading continues to require neither ShaderBuild nor the library for
its package-local program bytes.

## Implementation Stages

### Stage 0: Freeze closure, ownership, and formats

- [ ] Inventory every DurinGame- and DurinEditor-reachable call to mounted-file
  or generated-source Shader compilation, every registered Global Shader type
  and exact set, every finite feature/backend program, and all startup,
  shutdown, reload, device-recovery, and Cook call paths.
- [ ] Classify each current consumer as Game-required, Editor-only,
  package-owned Material, or unsupported, with one selected owner and explicit
  target/profile eligibility; resolve MonaImGui and any other non-Global
  DurinGame program explicitly.
- [ ] Freeze the runtime request identity stream and contribution descriptor,
  including owner/category/name, type or entry membership, ordering,
  target/profile, schema, duplicate rules, and registration/freeze lifetime.
- [ ] Freeze the exact library relative path and binary contract: magic,
  schema/builder versions, byte order, header, sorted directory, optional string
  table, record envelope, DSHD reuse, alignment, ranges, digests, counts, total
  size, preflight, lazy-read, and compatibility behavior.
- [ ] Specify the immutable Authored/Cooked RenderCore configuration, Launch
  adaptation, initialization/shutdown order, wrong-domain calls, cooked reload
  behavior, library generation, and device-recovery policy.
- [ ] Specify the ShaderBuild feature API, provider registration/retirement,
  public RenderCore value boundary, module dependencies, target selections,
  Slang deployment ownership, and test substitution seam.
- [ ] Freeze Cook inventory timing, target/profile selection, DDC-hit/build
  provenance, deterministic ordering, atomic publication, failure rollback,
  output manifest/receipt, and repeat-Cook byte identity.
- [ ] Record baseline focused tests, both runtime-variant closures, deployed
  files, cold/warm compile counts, registered inventories, Cooked Material
  behavior, source/DDC-free fixtures, and expected removal targets.

#### Acceptance Gate

- Every current Shader consumer and dependency is classified; runtime and
  production identities, library bytes, domain configuration, provider
  lifetime, Cook transaction, module graph, compatibility, and validation
  baselines are reviewable without an unresolved implementation choice.

### Stage 1: Introduce runtime identities and cooked library reading

- [ ] Add closed target/profile-qualified runtime request descriptors and
  canonical identity construction in RenderCore without source, DDC,
  compiler-environment, asset, Renderer-private, or RHI object fields.
- [ ] Add module-owned registration and canonical inventory freeze for Global
  exact sets and finite feature/backend contributions, including deterministic
  duplicate/late-registration/retirement rejection and a bounded test seam.
- [ ] Implement the RenderCore-owned library header/directory and DSHD record
  encode/decode helpers with checked arithmetic, exact bounds, canonical
  ordering, digest validation, request matching, and complete consumption.
- [ ] Implement immutable library preflight plus bounded lazy record access and
  decoded-output lifetime that never retains an unqualified raw file span or
  mutable producer storage.
- [ ] Add golden empty-invalid, minimal, multi-record, multi-stage, and full
  current-inventory fixtures plus wrong-target/profile/version, duplicate,
  unsorted, overlap, gap if prohibited, alignment, extent, digest, truncation,
  overflow, malformed DSHD, request-mismatch, and trailing-byte cases.

#### Acceptance Gate

- RenderCore can freeze a deterministic target inventory and strictly read a
  complete synthetic library into exact immutable compiler outputs without
  Shader source, DDC, Slang, Engine, Cook, or Renderer implementation access.

### Stage 2: Extract live compilation into ShaderBuild

- [ ] Create the `ShaderBuild` Developer module with private RenderCore,
  DerivedDataCache, Core, and Slang dependencies and select it for DurinEditor
  and Cook-capable roots but not DurinGame.
- [ ] Move Slang compiler/resolver, dependency manifests, file-fingerprint
  state, compile service/workers, in-flight records, output LRU, Shader DDC
  orchestration, generated-source handling, and related private tests from
  RenderCore into ShaderBuild without changing keys, DSHD bytes, cache buckets,
  limits, counters, or diagnostics.
- [ ] Register one module-owned live-build provider implementing the frozen
  RenderCore feature contract, including bounded admission, invocation/resource
  leases, retirement, shutdown, and deterministic fake-provider testing.
- [ ] Rewire existing RenderCore compile/dependency facades and all current
  authored consumers to the provider while keeping public request/result value
  types and Global/Material/mesh map contracts stable.
- [ ] Move Slang link, delay-load, runtime-file deployment, and compiler test
  ownership to ShaderBuild; remove private compiler/DDC includes and build
  metadata from RenderCore.
- [ ] Prove Authored cold/warm DDC, corruption repair, force recompile,
  generated imports, dependency fingerprints, reload changed/all, concurrent
  requests, provider absence/retirement, and last-known-good publication retain
  their exact behavior.

#### Acceptance Gate

- Authored execution is behaviorally unchanged through one unload-safe
  ShaderBuild provider, while RenderCore builds and links without Slang or
  DerivedDataCache and owns no source/compiler/DDC implementation.

### Stage 3: Produce the library through Cook

- [ ] Add a ShaderBuild-owned Cook producer that freezes the selected target
  inventory only after required module registration, resolves every descriptor
  through the ordinary live-build/DDC path, and retains complete validated
  immutable candidates.
- [ ] Adapt Global exact sets and each Game-required finite feature/backend
  program to stable contributions without changing their runtime map/resource
  ownership or forcing unrelated Editor-only entries into the Game inventory.
- [ ] Encode the canonical library from sorted runtime requests, DSHD values,
  production identities, and digests; reject missing, duplicate, incompatible,
  failed, canceled, or target-ineligible required outputs before publication.
- [ ] Integrate the library with the existing Cook root, target/profile,
  transaction, output receipt/manifest, atomic replacement, rollback, and
  stale-output repair boundaries rather than publishing beside Cook ad hoc.
- [ ] Add cold-DDC, warm-DDC, force rebuild if selected, failed compiler,
  corrupt DDC repair, duplicate/late contribution, missing module, failed
  publication, prior-output preservation, deterministic ordering, and repeated
  byte-identical Cook coverage.
- [ ] Prove Material `ProgramData` remains package-owned and byte/behavior
  compatible and no Material payload is duplicated in the global library.

#### Acceptance Gate

- A Win64 Game Cook transaction deterministically publishes one complete
  library covering every selected non-Material Runtime Shader request, reuses
  or rebuilds DDC values safely, preserves Material ownership, and leaves no
  partial output after any injected failure.

### Stage 4: Switch Cooked Runtime to the library

- [ ] Add explicit Shader-data configuration and initialize Authored Editor or
  Cooked Game mode before the first Shader demand; reject replacement,
  premature demand, missing provider/library, and target/profile disagreement.
- [ ] Make Global Shader exact-set resolution consume library outputs in Cooked
  mode while preserving typed map construction, parameter binding, merged
  layouts, lazy RHI creation, exact generations, pipeline coupling, and
  last-known-good rules.
- [ ] Switch every Game-required finite feature/backend program to its stable
  library request in Cooked mode while keeping its module-local resource and
  shutdown ownership.
- [ ] Disable live compile, generated-source compile, dependency inspection,
  DDC repair, force recompile, and source reload in Cooked mode with explicit
  domain diagnostics rather than unavailable symbol calls or silent fallbacks.
- [ ] Preserve metadata-only startup where selected, bounded first-demand
  payload reads, shared immutable code/reflection lifetimes, complete
  missing/corrupt-record rejection, and device-loss reconstruction from the
  same qualified library generation.
- [ ] Add cooked startup, first demand, all Global sets, feature/backend
  consumers, Material coexistence, wrong/missing/corrupt library, missing
  record, device loss, retry, shutdown/reopen, and no-authoring-fallback tests.

#### Acceptance Gate

- Cooked DurinGame renders through complete library-backed Global and
  feature-owned Shader outputs, coexists with package-owned Material programs,
  survives qualified device recovery, and cannot query source, manifests, DDC,
  Slang, or a live-build provider.

### Stage 5: Remove authoring dependencies from DurinGame

- [ ] Remove DerivedDataCache from RenderCore dependencies and DurinGame
  closure, remove Slang link/deployment from RenderCore and Game, and ensure
  ShaderBuild remains selected only by authoring/Cook roots.
- [ ] Remove Runtime initialization and shutdown of compiler workers,
  dependency manifests, file-fingerprint caches, DDC statistics/maintenance,
  and generated-source state; retain only domain-neutral values and the cooked
  reader in RenderCore.
- [ ] Audit the generated DurinGame module graph, binary imports, deployed
  runtime files, source/package closure, logs, and filesystem accesses for
  ShaderBuild, DerivedDataCache, Slang, Shader source extensions, manifest
  directories, and DDC paths.
- [ ] Run a Cooked Game fixture after removing/renaming source trees, local
  manifests and the complete DDC, and prove the same representative frame,
  Global/feature Shader identities, Material programs, reflection layouts, and
  RHI resources are produced.
- [ ] Inject missing/corrupt/incompatible library and record failures with all
  authoring inputs restored but unavailable to the Cooked domain, proving no
  source/compiler/DDC fallback is reachable.

#### Acceptance Gate

- DurinGame and its deployment are source-, manifest-, DDC-, ShaderBuild-, and
  Slang-free, while a valid Cooked library supplies every required non-Material
  Shader and invalid content fails explicitly without an authoring escape hatch.

### Stage 6: Complete qualification and lasting contracts

- [ ] Update Shader cache/build, Global Shader, Material boundary, runtime
  lifecycle, runtime variants, Cook/data lifecycle, code-module, build/deploy,
  and file-I/O documentation with only their lasting owned contracts.
- [ ] Update module closure assertions, native-test ownership/selection,
  deployment audits, Cook receipts, compatibility/versioning coverage, and
  documentation routing affected by ShaderBuild and the library.
- [ ] Run the repository-prescribed focused Core archive/file, RenderCore
  Shader/library/map, ShaderBuild compile/DDC, Material Cook/runtime,
  Renderer/Vulkan, Cook/package, module-closure, deployment, DurinEditor,
  DurinGame, source-free process, full build, complete native aggregate, and
  documentation validation under the testing and build/run guides.
- [ ] Record exact validation evidence, close every acceptance gate, mark the
  plan completed, and stage and commit the isolated implementation with exact
  Plan and final Stage provenance.

#### Acceptance Gate

- Lasting contracts describe one tool-owned live compiler and one Runtime-owned
  cooked delivery boundary; all required focused, closure, source-free,
  deployment, aggregate, and documentation validation passes; and the completed
  plan is ready for archival.

## Validation Matrix

| Concern | Required evidence |
| --- | --- |
| Inventory | Every current Global, feature/backend, generated and Material request is classified; Game inventory is deterministic and complete |
| Runtime identity | Stable canonical identities are independent of source, DDC, compiler environment, discovery order and physical checkout paths |
| Library format | Golden header/directory/record bytes, exact target/profile, checked ranges/alignment/digests, DSHD request validation and complete consumption |
| Provider lifetime | Authored provider availability, concurrent invocation, retirement, shutdown, absence and deterministic fake substitution |
| Authored compatibility | Existing dependency manifest, memory/DDC hit, corruption repair, force compile, generated source, reload and last-known-good behavior |
| Cook | Cold/warm production, deterministic inventory/order/bytes, strict failure, atomic replacement, rollback, receipt and prior-output preservation |
| Cooked Runtime | Preflight, lazy values, all Global exact sets, finite feature/backend programs, Material coexistence, device loss and bounded failure |
| No fallback | Missing/corrupt/wrong-target data never reads source, manifest or DDC and never loads Slang or invokes ShaderBuild |
| Module graph | RenderCore excludes DDC/Slang; ShaderBuild owns them; DurinGame excludes ShaderBuild, DDC, Slang and Editor modules |
| Deployment | Cooked Game contains the selected library and no Shader source, dependency manifests, DDC objects, compiler binary or authoring-only module |
| Source-free process | Representative Cooked Game frame and recovery pass after source, manifests and DDC are unavailable |
| Repository | Focused targets, both configured variants, Cook/process tests, full builds, complete native aggregate and documentation validators pass |

## Deferred Follow-ups

- Add additional target platforms and profiles only after their compiler,
  feature capability, packaging, golden-format, and source-free Runtime matrix
  is explicit.
- Introduce chunked, streamed, optional, patch, compressed, encrypted, or signed
  Shader libraries only when deployment requirements select exact install and
  trust semantics.
- Design Local/Shared/Cloud DDC backend injection separately around authoring
  and Cook availability, promotion, authentication, transport, failure, and GC;
  Cooked Runtime remains independent of that service.
- Add PSO capture and driver pipeline caches separately; portable
  SPIR-V/reflection delivery is not a substitute for backend-specific pipeline
  state.
- Evaluate removal or relocation of Editor-only local manifests only if
  measured warm dependency validation no longer justifies them.

## Related Documentation

- [Shader Cache](../Runtime/Rendering/ShaderCache.md)
- [Global Shaders](../Runtime/Rendering/GlobalShaders.md)
- [Material System](../Runtime/Rendering/MaterialSystem.md)
- [Asset Data Lifecycle and Storage](../Runtime/Assets/AssetDataLifecycle.md)
- [Runtime Lifecycle](../Runtime/Core/RuntimeLifecycle.md)
- [Runtime Variants](../Development/Build/RuntimeVariants.md)
- [Code Modules](../Workspace/CodeModules.md)
- [Workspace And Projects](../Workspace/WorkspaceProjects.md)
- [File I/O](../Runtime/Core/FileIO.md)
- [Native C++ Tests](../Development/Build/NativeTests.md)
- [Shader Derived Data Cache Migration Plan](Archive/2026-08/ShaderDerivedDataCacheMigration.md)

## Related Code

- [`RenderCore`](../../Engine/Source/Runtime/RenderCore)
- [`Renderer`](../../Engine/Source/Runtime/Renderer)
- [`MonaImGui`](../../Engine/Source/Runtime/MonaImGui)
- [`DerivedDataCache`](../../Engine/Source/Developer/DerivedDataCache)
- [`ShaderCompilerCore.h`](../../Engine/Source/Runtime/RenderCore/Public/Shader/ShaderCompilerCore.h)
- [`ShaderCompileService.cpp`](../../Engine/Source/Runtime/RenderCore/Private/Shader/ShaderCompileService.cpp)
- [`ShaderDerivedData.cpp`](../../Engine/Source/Runtime/RenderCore/Private/Shader/ShaderDerivedData.cpp)
- [`ShaderDependencyManifestStore.cpp`](../../Engine/Source/Runtime/RenderCore/Private/Shader/ShaderDependencyManifestStore.cpp)
- [`GlobalShader.cpp`](../../Engine/Source/Runtime/RenderCore/Private/Shader/GlobalShader.cpp)
- [`MaterialCook.cpp`](../../Engine/Source/Runtime/Engine/Private/Materials/MaterialCook.cpp)
- [`Engine.dproject`](../../Engine/Engine.dproject)
