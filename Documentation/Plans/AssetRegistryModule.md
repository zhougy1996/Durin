# Asset Registry Module Plan

Summary: Extract Engine asset discovery, immutable metadata queries, and reference projections into a dedicated runtime AssetRegistry module.

Last reviewed: 2026-08-29

Status: Completed
Completed: 2026-08-29

## Current Status

All five stages are complete. AssetRegistry is the sole owner of mounted
package discovery, bounded construct-free DAST inspection, immutable catalog
and reference state, revisions, atomic expected-revision publication, and the
AREG/ARIX caches. Engine owns live object construction and residency, package
writing, Cook, and mutation/publication preparation. Runtime, editor, program,
and test consumers use the owning capability headers, with no Provider or
AssetDefinition layer.

Final qualification on 2026-08-29 passed:

- `AssetMetadataQueryTests` 6/6, `AssetPackageTests` 125/125,
  `AssetCookTests` 13/13, `AssetReferenceStoreTests` 7/7,
  `ContentBrowserWorkflowTests` 61 passed with one registered skip, and
  `EditorAssetWorkflowTests` 30/30;
- the complete `Win64-Debug-DurinEditor` `all` target, including the runtime,
  editor, programs, and native-test closure;
- a read-only `DurinAssetTool check` of Sandbox: 25 packages, all compatible,
  with zero incompatible, unsupported, failed, stale, or resave-required
  packages;
- an eight-second DurinEditor startup/lifecycle smoke;
- changed-document validation for four lasting contracts and validation of the
  complete plan set.

The 125 package tests compare incremental and full-validation discovery,
unchanged/changed revision sequences, cache reuse and invalidation, corruption
and schema recovery, redirects, hard/soft reference projections, mutation
publication, shutdown flush, and restart reuse against the Stage 0 behavior.
Cook and editor workflow suites preserve reachability and Content Browser
refresh/query behavior. Repository searches confirm the one-way
`Core -> CoreDObject -> AssetRegistry -> Engine` dependency, no Engine-owned
catalog store/facade or obsolete refresh API, no reverse AssetRegistry include,
and no package-reader Provider or AssetDefinition implementation.

## Implementation History

Stage 0 is complete. The extraction map and pre-migration behavior baseline are
recorded below. The selected direction was to extract the
existing Engine Asset Catalog rather than create a second registry:

- `AssetRegistry` will directly understand the repository's single canonical
  DAST package format sufficiently to inspect persistent metadata without
  constructing asset objects.
- No package-reader provider, format-provider registry, or per-asset-type scan
  callback will be introduced.
- No editor `AssetDefinition` abstraction will be introduced. Existing factory,
  workspace, thumbnail, import, and Content Browser extension mechanisms remain
  unchanged.
- Engine will depend on AssetRegistry. AssetRegistry must not depend on Engine.
- Engine retains package residency, object construction, authored package
  writing, Cook production, and transactional asset mutation orchestration.
- The existing immutable snapshot, revision, complete-refresh, redirect, and
  reference-projection semantics are preserved during extraction.

Stage 1 is complete. It established an independently linkable AssetRegistry DLL
with only Core and CoreDObject dependencies. Catalog, result, reference, scan,
snapshot, path-resolution, and publication values now come from
`AssetRegistry/...`. The module-owned `FAssetRegistryState` provides owned exact
lookup, immutable catalog/reference snapshots, revisions, and expected-revision
whole-state publication without Engine. Engine retains temporary forwarding
headers and its active catalog store until Stage 2 scan/cache movement and the
Stage 3 atomic authority handoff occur together. Four
`AssetMetadataQueryTests` prove value ownership, canonical DAST rejection,
lookup, snapshots, and stale publication without linking Engine; Engine builds
through the selected dependency direction.
Stage 2 now has one canonical bounded DAST header reader below Engine.
AssetRegistry owns the envelope validation, Public Summary and Import decoding,
file-front-matter IO, format identity, supported reader set, cache policy
fingerprint, and package fingerprint value. Engine's full v6 codec calls the
same exported summary decoder and header byte reader; its former duplicate
header parser is removed. The independent module test rejects malformed summary
bytes without linking Engine, while all 125 AssetPackageTests preserve legacy
unsupported-version classification and existing package behavior. Mounted scan,
reference inspection, and persistent projection ownership remain the next
Stage 2 slice. The deterministic AREG schema-2 and ARIX schema-1 cache codecs,
mount manifest/identity helpers, bounds, policy fingerprint checks, and atomic
cache publication have since moved into AssetRegistry as well; the 125 package
tests pass with identical reuse, invalidation, corruption recovery, and warning
behavior. The module now also owns auto-scan mount enumeration,
physical-to-virtual path mapping, file fingerprints, incremental header reuse,
bounded DAST reparsing, duplicate rejection, redirector metadata classification,
removal statistics, and complete catalog candidates. Engine consumes that
candidate only for the remaining full object-field reference projection and
publication handoff. All 125 package tests pass after deleting the former
Engine enumeration path.

The first Stage 3 authority slice is active: process-wide exact lookup, path
resolution, catalog/reference snapshot capture, redirect lookup, and revision
queries are now exported and served by the AssetRegistry singleton rather than
the Engine facade. Successful scan, save/admission, relocation, deletion, and
redirector-fix-up commit points publish complete catalog plus reference batches;
failed and incomplete scans retain the prior module revision. Engine's legacy
store remains as a transitional transaction-preparation mirror until its direct
collection users are converted to snapshots/publication batches. All 125
package tests pass across save, move, delete, fix-up, Undo/Redo, cache, redirect,
reference, and revision scenarios.

`FAssetRuntimeState` no longer embeds or controls catalog storage. Its loader and
mutation services obtain the transitional publication coordinator explicitly,
and shutdown flushes that coordinator only after Engine request drain. This
separates Engine residency/runtime lifetime from the process metadata singleton;
removing the remaining coordinator collections is still required before the
first Stage 3 checklist item closes.

`FAssetLoadService` now performs creation, duplication, adoption, dependency,
and unload-guard metadata checks through owned AssetRegistry entries instead of
the store's private pointer lookup. Its store friendship is removed; 125 package
tests pass with the loader reading the same committed revisions as public
callers.

Relocation, relocation restore/Undo, and redirector Fix Up now commit through a
single expected-revision prepared-state publication method rather than assigning
catalog/reference collections at their commit sites. The method updates assets,
reference occurrences, fingerprints, errors, completeness, dirty projections,
redirect indexing, and the visible module revision as one batch. Stale plans
fail before publication, compensation remains Engine-owned, and all 125 package
tests pass.

Stage 2 is complete. AssetRegistry now owns the canonical logical object-stream
writer, bounded structural decoder, byte-for-byte canonical re-emission check,
DAST v6 envelope/section/hash validation, logical stream reconstruction, and
reflection-guided construct-free reference occurrence extraction. Mounted
refresh performs enumeration, catalog and reference cache reuse, full payload
validation, all-or-nothing completeness checks, expected-revision publication,
and both cache writes entirely below Engine. Engine live loading consumes the
moved structural decoder, while its refresh wrapper only retains lifecycle
logging and cache-flush compatibility. Six Engine-free metadata tests cover the
module boundary and all 125 package, scan, cache, redirect, reference, mutation,
and lifecycle tests pass.

Stage 4 is complete. ContentBrowser, AssetTools, DurinEd, MainFrame, the asset
tool program, and direct-query tests now include AssetRegistry capability
headers and declare explicit dependencies. The pure refresh entry point is
`RefreshAssetRegistry`; the former Engine refresh export and forwarding
`Asset/Catalog.h`, `Asset/Result.h`, and `Asset/PackageTypes.h` headers are gone.
The former Engine catalog store/facade sources are absent; transactional code
uses a publication-only coordinator that owns no registry state, revision,
scan, cache, or query data. Cache dirtiness and shutdown flushing are owned by
AssetRegistry. Engine retains only loading/mutation publication preparation and
Cook reachability, which combines Engine runtime roots with immutable registry
queries. Dependencies/referencers coverage now verifies hard, soft, and redirect
edges through the unified module snapshot. AssetMetadataQueryTests pass 6/6,
AssetPackageTests pass 125/125, and ContentBrowser, MainFrame, and DurinAssetTool
build successfully.

Stage 3 is complete. Deletion, restore, save/admission, relocation, Fix Up,
Undo, and Redo all prepare owned value projections and replace the whole
AssetRegistry state against an expected revision. `FAssetCatalogStore` no longer
owns or mirrors assets, references, redirect indices, or a revision; it retains
only transitional scan/cache operational state until Stage 2 finishes moving
refresh orchestration. AssetRegistry exposes no store/coordinator friendship,
and Engine reads committed metadata through exact owned entries or immutable
snapshots. A simultaneous-publisher boundary test proves only one writer can
commit a shared expected revision. The five Engine-free metadata tests and all
125 asset package, transaction, rollback, recovery, cache, revision, and
shutdown-lifecycle tests pass after the authority handoff.

## Goal

Make persistent asset discovery and metadata queries a cohesive runtime module
that can be consumed without taking a dependency on the full Engine module.

At completion, `AssetRegistry` owns mounted-content scanning, bounded read-only
DAST inspection, deterministic persistent caches, immutable catalog and
reference snapshots, path resolution, dependency and referencer queries, and
atomic metadata publication. Engine consumes those capabilities for loading,
Cook, save, relocation, deletion, and fix-up without reaching into registry
private storage.

## Scope

- Add a shared runtime module named `AssetRegistry` beneath Engine in the module
  dependency graph.
- Move `FAssetData`, catalog entry/snapshot/refresh values, scan statistics,
  registry errors, and read-only query entry points to AssetRegistry public
  headers.
- Move registry and reference cache persistence, mounted-content scanning,
  immutable publication, redirect indexing, and dependency/reference queries
  out of Engine.
- Move or extract the bounded read-only DAST inspection required by registry
  scanning and reference extraction so AssetRegistry does not depend on Engine.
- Make Engine's full DAST reader reuse the AssetRegistry-owned wire definitions
  and inspection primitives instead of maintaining a second Public Summary
  interpretation.
- Add a narrow atomic publication capability used by successful Engine package
  save and mutation transactions.
- Rehome process lifecycle ownership so AssetRegistry starts before Engine
  asset services and stops accepting publication only after Engine has drained
  asset operations.
- Update Engine, AssetTools, ContentBrowser, DurinEd, LevelEditor, Cook tooling,
  and tests to include and depend on the owning module directly where needed.
- Preserve cache invalidation, corruption recovery, full-validation behavior,
  catalog completeness, reference completeness, and revision semantics.
- Publish the resulting lasting ownership in the asset and code-module
  documentation after validation.

## Non-Goals

- Introducing an editor `AssetDefinition`, UE `UAssetDefinition` equivalent, or
  a common replacement for factories, workspaces, thumbnails, importers, or
  Content Browser extensions.
- Introducing `IAssetMetadataReader`, package-format providers, per-class
  registry scanners, or dynamically selected package readers.
- Supporting another authored package format or making DAST format-pluggable.
- Redesigning DAST v6, changing `.dasset` bytes, or requiring asset resave.
- Changing asset identity, mount syntax, load visibility, redirect behavior,
  hard/soft reference semantics, residency, or garbage-collection behavior.
- Moving package creation, authored serialization, loading, object
  construction, residency, Cooked payload handling, or mutation transaction
  policy out of Engine.
- Adding arbitrary registry tags, search indexes, asynchronous background scan,
  filesystem watching, remote registries, or editor search UX in this plan.
- Renaming user-facing Asset Catalog concepts solely to imitate Unreal Engine
  terminology where the existing name is clearer.
- Preserving obsolete repository-internal include paths through indefinite
  forwarding headers after all callers have migrated.

## Design Decisions and Invariants

### One registry replaces the Engine-owned catalog

The current `Asset::Catalog` implementation is the baseline and moves into the
new module. There must never be an Engine catalog mirrored into a second
AssetRegistry store. A successful refresh or mutation publishes one immutable
state and one monotonically advancing process-local revision.

Public callers continue to receive owned entry values and immutable snapshots;
they never receive pointers, iterators, locks, or mutable access to module
storage. Exact lookup, snapshot capture, path resolution, dependencies, and
referencers must all identify the revision against which their result was
produced.

### AssetRegistry directly owns bounded DAST inspection

Durin has one canonical authored package format. AssetRegistry therefore reads
DAST metadata directly rather than routing every file through a provider. Its
read-only package capability is limited to the wire facts needed for discovery,
validation, redirect classification, dependency indexing, and reflected
reference extraction without constructing `DObject` instances or invoking
`PostLoad`.

The format identity, Public Summary representation, bounded field-reading
limits, and validation shared by registry inspection and Engine loading have
one implementation below Engine. Engine's object construction and package
writer remain Engine-owned but consume the shared definitions. A registry scan
must not load the main asset, create a resident package, mutate authored bytes,
or require an asset-class-specific module callback.

### AssetDefinition is an editor concern and is not required

Asset type display names, colors, editors, menus, import policy, factories, and
thumbnail behavior are presentation or authoring concerns. The runtime registry
operates on persisted class identities and reference metadata even when an
editor integration for that class is absent.

Unknown or currently unavailable reflected classes remain discoverable as
metadata. Operations requiring an actual `DClass`, such as expected-class
resolution, report the existing typed failure rather than hiding the entry.

### Dependency direction is acyclic

The selected dependency direction is:

```text
Core -> CoreDObject -> AssetRegistry -> Engine -> editor and feature modules
```

AssetRegistry may use Core filesystem, serialization, mount, and module
primitives plus CoreDObject asset paths and reflection metadata. It must not
include Engine headers, name Engine runtime objects, or call Engine loading,
residency, mutation, Cook, rendering, or editor services.

Engine declares AssetRegistry as a public dependency while Engine public asset
entry points expose registry values. Modules that only query persistent asset
metadata may depend directly on AssetRegistry; modules that also load or mutate
assets retain Engine as well.

### Mutation publication is narrow and atomic

AssetRegistry owns mutable catalog storage but exposes no general public mutable
manager. Engine uses a capability-named publication API accepting a complete
validated batch with an expected base revision. A batch can add, update, move,
or remove entries and their reference occurrences, but it either publishes all
visible changes and one resulting revision or publishes none.

Engine remains responsible for physical staging, package serialization,
journaling, rollback, compensation, residency validation, and editor policy.
Registry publication cannot independently move, delete, or rewrite a package.
Commit, Undo, and Redo each publish only after their Engine-owned authoritative
transition succeeds.

### Startup, refresh, and shutdown preserve current authority

Mounted Content remains authoritative. `AssetRegistry/Registry.bin` and
`AssetRegistry/References.bin` remain deterministic rebuildable projections,
not content mounts or sources of package bytes. Exact fingerprints may be
reused only under the current incremental rules; full validation bypasses
fingerprint reuse.

AssetRegistry initializes after Core mount publication and before Engine asset
loading accepts requests. During shutdown, Engine first stops and drains load,
save, compile, and mutation operations; AssetRegistry then stops publication,
flushes eligible caches, and releases state. No module callback may enter a
retiring owner.

### Source compatibility is bounded and wire compatibility is unchanged

Repository callers migrate atomically from Engine-owned catalog headers to
AssetRegistry-owned headers. Temporary forwarding headers are allowed only
inside an intermediate buildable stage and are removed before completion.

Existing `.dasset`, `.dabulk`, registry cache, and reference cache compatibility
must remain unchanged unless characterization proves that a cache schema change
is unavoidable. Any cache schema increment must be deterministic, rebuildable,
and require no authored-content migration.

## Current Foundations and Gaps

Foundations already present:

- `FAssetCatalogStore` owns mounted scanning, exact lookup, immutable snapshot
  capture, redirect indexing, reference projection, and Cook reachability.
- Startup incremental refresh and explicit full validation already report
  completeness, counters, warnings, errors, and publication revisions.
- `Registry.bin` and `References.bin` are bounded, deterministic, rebuildable
  caches with corruption and compatibility fallback.
- DAST Public Summary already contains class, entry kind, redirect destination,
  dependencies, format version, and object count without requiring object
  construction.
- ContentBrowser, AssetTools, DurinEd, Engine loading, Cook, relocation,
  deletion, fix-up, and compatibility tooling consume catalog facts through
  known call sites.
- Engine mutation transactions already publish catalog changes only after
  validation and retain rollback or recovery state.

Gaps to close:

- Catalog state is embedded in `FAssetRuntimeState` beside residency, loading,
  and mutation services.
- Scanning and persistence include Engine-private DAST codec and package-value
  helpers.
- Reference extraction and full package inspection are implemented inside
  Engine even though they do not construct resident objects.
- Engine services and mutations use `FAssetCatalogStore` private pointers,
  friendship, and direct map access rather than a module boundary.
- Registry public values use `ENGINE_API` and live beneath Engine include paths.
- Engine startup and shutdown currently own catalog lifetime implicitly.
- Native tests do not enforce an AssetRegistry-to-Engine dependency boundary or
  independently qualify the extracted module.

## Stage 0 Extraction Map and Baseline

### Movement map

| Current Engine-owned area | Current files | Target owner | Movement rule |
| --- | --- | --- | --- |
| Public catalog values and query facade | `Public/Asset/Catalog.h`, `Private/Asset/AssetCatalogFacade.cpp` | AssetRegistry | Move values and queries; keep the `Durin::Asset` namespace so callers do not need a semantic rename |
| Public reference projection | `Public/Asset/References.h`, query portions of `Private/Asset/AssetCatalog.cpp` | AssetRegistry | Move immutable edges, snapshots, referencer/target queries, and redirect lookup; keep external reference-store authoring policy in Engine |
| Catalog store and scan | `Private/Asset/AssetCatalogStoreInternal.h`, `Private/Asset/AssetCatalog.cpp` | AssetRegistry | Move mounted enumeration, immutable state, refresh, redirect index, revisions, and cache dirtiness |
| Persistent projections | `Private/Asset/AssetCatalogPersistenceInternal.h`, `Private/Asset/AssetCatalogPersistence.cpp` | AssetRegistry | Move `Registry.bin` and `References.bin` codecs without changing bytes or paths |
| Read-only DAST surface | `Public/Asset/PackageInspection.h`, read-only portions of `Private/Asset/AssetPackageCodec.*`, `AssetPackageV6Codec.*`, `AssetPackageOperations.cpp`, and `AssetPackageValueCodec.h` | AssetRegistry | Extract the format envelope, Public Summary, bounded inspection, and non-constructing value reads; Engine writer, loader, relocation, rewrite, and object construction stay in Engine |
| Reference extraction | Read-only extraction portions of `Private/Asset/AssetReference.cpp` | AssetRegistry | Move inspection-to-edge extraction and ordering; reference rewriting remains Engine-owned |
| Runtime lifetime | catalog member in `AssetRuntimeStateInternal.h`, initialization and shutdown in `AssetRuntime.cpp` | AssetRegistry module lifecycle | Registry is initialized before Engine accepts asset requests and retires after Engine drains and flushes publication |
| Mutation publication | direct store mutations in `AssetRelocation.cpp`, `AssetDeletion.cpp`, `AssetRedirectorFixup.cpp`, and save/admission paths in `AssetPackageOperations.cpp` | Engine prepares; AssetRegistry atomically publishes | Replace collection replacement and friendship with an expected-revision complete batch |
| Read-only Engine consumers | `AssetCanonicalResave.cpp`, `CookedAsset.cpp`, deletion analysis/extensions, package operations, editor consumers, and programs | Owning consumer through AssetRegistry public API | Use exact entries, immutable snapshots, revision-bound resolution, or reference snapshots; no private store pointers |

The module identity is `AssetRegistry`, its export macro is `ASSETREGISTRY_API`,
and its public include layout begins at `AssetRegistry/...`. The authoritative
project map gains `Source/Runtime/AssetRegistry`; its `.dmodule` depends only on
Core and CoreDObject. Engine gains AssetRegistry as a public dependency. No
existing module or target uses the `AssetRegistry` name.

### Direct-store replacement map

| Current access | Classification | Replacement |
| --- | --- | --- |
| `FAssetLoadService` friendship and pointer lookup | loading/read query | exact owned entry and revision-bound path resolution |
| `FAssetMutationCoordinator` friendship | mutation coordination | query snapshot plus expected-revision publication capability |
| deletion loops over `GetAssets()` | read query and mutation preparation | immutable catalog snapshot |
| relocation/fix-up copies of `Registry.Assets` | prepared publication and rollback state | owned pre-snapshot plus complete post-state publication batch |
| relocation/fix-up access to `ReferenceIndex.Edges`, fingerprints, errors, and completeness | prepared publication and reference coordination | owned reference snapshot plus complete source projections in the same publication batch |
| deletion backup/restore assignment of `Registry.Assets` | rollback/undo | inverse expected-revision publication batch after authoritative disk transition |
| canonical resave revision reads | staleness check | `GetAssetCatalogRevision()` |
| Cook reachability private store access | Cook query | AssetRegistry reachability query over one immutable revision; Engine contributes registered external Cook roots explicitly |
| testing cache dirtiness/warning/flush calls | test seam | AssetRegistry-owned testing capability kept out of general mutable API |

There are two friends to retire: `FAssetLoadService` and
`FAssetMutationCoordinator`. Direct mutable collection access is confined to
relocation, deletion, and redirector fix-up; read-only store access is confined
to package loading/operations, CookedAsset validation, canonical resave, and
deletion analysis/extensions.

### DAST read-only trace

The canonical read path is file envelope load -> codec resolution -> DAST v6
section-table validation -> Public Summary parse -> bounded object/field
inspection -> reflection-guided reference occurrence extraction. Stage 2 moves
the envelope/section/Public Summary definitions and the inspection-only value
decoder as one unit. Engine's codec table remains responsible only for
full-object load, authored write, relocation, reference rewrite, redirector
write, and compatibility operations, and calls the moved parser. This avoids a
second parser and does not introduce a provider callback.

### Wire, cache, refresh, and lifecycle baseline

- Authored DAST stays at v6; Stage 0 records no format or resave requirement.
- `Registry.bin` uses magic `AREG` (`0x47455241`), schema 2, the package-reader
  policy fingerprint, deterministic mount/entry ordering, and a one-million
  entry bound.
- `References.bin` uses magic `ARIX` (`0x58495241`), schema 1, extractor schema
  1, the same package-reader policy fingerprint, deterministic source/edge
  ordering, a 1 GiB file bound, 100,000 references per package, and one million
  references per snapshot.
- Incremental refresh may reuse an entry only when stable file-size and
  last-write ticks match; reference reuse additionally matches reader version.
  Full validation bypasses both reuse paths and inspects complete package bytes.
- Corrupt, incompatible, duplicate, unmappable, or unreadable packages add a
  typed error and make the candidate catalog incomplete. An incomplete refresh
  publishes nothing and retains the prior revision. A complete changed refresh
  publishes catalog and reference state together and advances the process-local
  revision once; an unchanged complete refresh keeps its revision.
- Empty and mixed mounts follow the same all-or-nothing rule. Redirectors must
  have the redirector class, one valid non-self destination, one matching
  dependency, and one object. Manual-scan-disabled mounts are not enumerated.
- Existing startup initializes Engine Asset after mount configuration but does
  not scan implicitly; callers request refresh. Existing shutdown first stops
  new Engine asset requests, flushes both dirty projections, clears residency,
  resets the loader, and then releases object roots. The extracted lifecycle
  preserves that order while moving projection flush after Engine drain and
  before AssetRegistry teardown.
- Cache absence is a normal rebuild. Corruption, schema/policy mismatch, bounds
  failure, or mount-manifest mismatch yields a warning and deterministic
  rebuild; authored package bytes are never changed by recovery.

### Validation baseline

On 2026-08-29, `DevTool.bat test AssetPackageTests` passed 125 tests across nine
suites in the `Win64-Debug-DurinEditor` preset. The focused migration set is:

- `AssetPackageTests` for DAST, scan, cache, redirect, references, mutation, and
  revision behavior;
- `AssetCookTests` for dependency closure and reachability;
- `AssetReferenceStoreTests` for Engine-owned external reference stores;
- `ContentBrowserWorkflowTests` for registry-driven editor discovery;
- new AssetRegistry module-boundary tests, added with Stage 1, that link Core,
  CoreDObject, and AssetRegistry without Engine.

Stage 5 expands this set only where the moved dependency closure or observed
integration behavior requires it.

## Implementation Stages

### Stage 0: Freeze the extraction map and behavior baseline

- [x] Inventory the exact public headers, private sources, helpers, build
  metadata, tests, tools, and consumers participating in catalog scan, cache,
  reference extraction, resolution, and mutation publication.
- [x] Classify each current `FAssetCatalogStore` friend and direct-storage use as
  read query, prepared publication, mutation coordination, loading, Cook, or
  test seam, and record its target owner.
- [x] Trace DAST read-only inspection from envelope through Public Summary and
  reference occurrence extraction; select the smallest code movement that
  leaves object construction and writing in Engine with no duplicate parser.
- [x] Record current incremental and full-validation results for empty,
  ordinary, redirector, corrupt, incompatible, manual-scan, and mixed mounts.
- [x] Record cache schema/version behavior, deterministic byte expectations,
  revision advancement rules, startup ordering, shutdown ordering, and exact
  failure semantics.
- [x] Select focused native test targets and add characterization cases for any
  invariant not already protected before moving production ownership.
- [x] Confirm the proposed module name, API macro, public include layout, and
  `.dmodule` dependency closure do not collide with repository targets.

#### Acceptance Gate

- The complete movement map, dependency graph, lifecycle sequence, direct-store
  replacement map, wire/cache compatibility baseline, and focused validation
  set are recorded with no unresolved ownership or parser decision.

### Stage 1: Establish the AssetRegistry module and public query contract

- [x] Add the AssetRegistry shared runtime module, API export header, CMake
  target metadata, `.dmodule` descriptor, and module lifecycle shell with only
  Core and CoreDObject dependencies.
- [x] Move catalog data, scan/result, snapshot, path-resolution, and reference
  query value types under AssetRegistry ownership while preserving namespace
  and value semantics where practical.
- [x] Move the immutable query facade and private state shell without changing
  the active Engine-backed scan or mutation path yet.
- [x] Declare AssetRegistry as an Engine public dependency and migrate a small
  vertical query slice through the new exported API.
- [x] Add module-boundary tests proving AssetRegistry links without Engine and
  Engine consumes AssetRegistry only in the selected direction.
- [x] Keep every intermediate state buildable; use temporary forwarding only
  when needed for staged caller migration.

#### Acceptance Gate

- AssetRegistry builds and links independently of Engine, owns the exported
  query value types and facade, and the selected vertical lookup/snapshot slice
  passes existing semantics and new module-boundary coverage.

### Stage 2: Move read-only DAST inspection, scanning, and persistent caches

- [x] Move the canonical DAST format identity, Public Summary wire definitions,
  bounded read primitives, and read-only validation needed by AssetRegistry
  below Engine.
- [x] Make Engine's full package reader consume the moved definitions and
  primitives; remove duplicate Public Summary parsing and validation authority.
- [x] Move mounted-content enumeration, fingerprint reuse, redirect
  classification, deterministic registry cache load/write, and refresh
  publication into AssetRegistry.
- [x] Move non-constructing reference occurrence extraction and deterministic
  reference cache load/write into AssetRegistry while retaining current bounds
  and all-or-nothing source publication.
- [x] Preserve manual-scan admission, incremental reuse, full validation,
  corruption rebuild, unsupported-format rejection, statistics, warnings, and
  completeness reporting.
- [x] Add or migrate focused tests for bounded inspection, scanning, cache
  determinism, invalidation, corruption recovery, redirect metadata, and
  reference extraction.

#### Acceptance Gate

- AssetRegistry can rebuild and reuse both persistent projections from mounted
  `.dasset` files without Engine, without constructing objects, and without a
  provider callback; Engine loading and package validation use the same
  canonical DAST summary interpretation.

### Stage 3: Replace direct Engine store access with queries and publication

- [x] Remove catalog ownership from `FAssetRuntimeState` and make Engine asset
  services acquire the AssetRegistry query/publication capabilities through
  explicit construction or module lifecycle access.
- [x] Replace loader, Cook, compatibility, canonical-resave, relocation,
  deletion, redirector-fix-up, and save-time direct map/pointer access with
  owned snapshots, exact entries, or revision-bound query results.
- [x] Add an expected-revision atomic publication batch for save, admission,
  relocation, deletion, fix-up, Undo, and Redo outcomes.
- [x] Ensure reference projections are updated in the same visible publication
  as their source catalog entries and that incomplete extraction cannot publish
  partial state.
- [x] Preserve transaction staleness checks, rollback, compensation, recovery
  journals, residency collision checks, and committed-only observers.
- [x] Remove `FAssetCatalogStore` friendship and Engine access to AssetRegistry
  private collections.
- [x] Add concurrency, stale-publication, rollback, revision, and shutdown-drain
  tests around the new boundary.

#### Acceptance Gate

- Engine performs loading, Cook, save, move, delete, fix-up, Undo, and Redo
  through immutable queries plus atomic publication; it has no friendship,
  pointer, iterator, or direct collection access to AssetRegistry internals.

### Stage 4: Migrate consumers and retire Engine catalog ownership

- [x] Migrate ContentBrowser, AssetTools, DurinEd, LevelEditor, programs, and
  tests to AssetRegistry-owned capability headers and explicit module
  dependencies where they query registry data.
- [x] Preserve Engine aggregate asset entry points only where they continue to
  combine loading or other Engine-owned behavior; remove catalog ownership from
  Engine API metadata.
- [x] Remove obsolete Engine catalog facade, store, persistence, forwarding
  headers, sources, build entries, and API exports after the last caller moves.
- [x] Add Dependencies and Referencers query coverage over the existing unified
  hard/soft/redirect projection without adding new search or tag features.
- [x] Search production, programs, tests, and build metadata to prove no
  Engine-owned catalog implementation, retired include path, or reverse module
  dependency remains.

#### Acceptance Gate

- All persistent metadata consumers use AssetRegistry ownership, the retired
  Engine catalog implementation is absent, and module/build metadata proves an
  acyclic AssetRegistry-to-Engine boundary.

### Stage 5: Qualify lifecycle, compatibility, and publish lasting contracts

- [x] Run focused AssetRegistry, Engine asset, editor asset workflow, Cook, and
  module-boundary native tests selected through the repository testing
  workflow.
- [x] Build the affected runtime, editor, program, and test target closure using
  the repository build workflow.
- [x] Compare incremental and full-validation scan results, cache bytes or
  schema behavior, revision sequences, redirect resolution, dependency and
  referencer projections, and Cook reachability against the Stage 0 baseline.
- [x] Run applicable authored and Cooked runtime smoke tests, Content Browser
  refresh/query smoke, and shutdown/restart cache-reuse smoke.
- [x] Update Asset Catalog and Mutation, Asset Packages, Code Modules, build
  ownership, and any directly affected editor contracts to describe the
  implemented module boundary without duplicating implementation status.
- [x] Validate changed documentation and the complete active-plan set, record
  evidence in Current Status, and close only gates supported by results.

#### Acceptance Gate

- Focused and integration tests, affected builds, runtime/editor/Cook smokes,
  compatibility comparisons, lifecycle checks, repository searches, and
  documentation validation pass with evidence recorded in this plan.

## Validation Matrix

| Concern | Validation | Required evidence |
| --- | --- | --- |
| Module dependency | Descriptor/CMake boundary tests and affected target build | AssetRegistry depends only on selected lower modules; Engine depends on AssetRegistry; no reverse edge exists |
| Query semantics | Focused AssetRegistry native tests | Exact lookup, snapshots, revisions, path resolution, dependencies, and referencers retain owned immutable results |
| DAST inspection | Package inspection and corruption fixtures | Registry reads canonical summary/reference metadata without object construction and rejects malformed or unsupported packages within bounds |
| Cache compatibility | Registry/reference cache tests and baseline comparison | Deterministic reuse, invalidation, corruption rebuild, schema handling, and warnings match the recorded contract |
| Refresh completeness | Incremental/full-validation mount fixtures | Auto/manual mounts, failures, retained prior revision, statistics, and completeness remain exact |
| Mutation publication | Engine asset mutation tests | Save, move, delete, fix-up, Undo, and Redo publish one atomic revision or none; stale and compensated attempts do not leak partial state |
| Runtime behavior | Engine asset load/unload and authored/Cooked smoke | Catalog miss, redirect resolution, class checks, residency, hard dependency guards, and Cook reachability remain unchanged |
| Editor behavior | Content Browser and editor asset workflow tests/smoke | Refresh, browse, pick, reveal, duplicate, move, delete, and fix-up consume the extracted registry without presentation changes |
| Lifecycle | Startup/shutdown tests and restart smoke | Mounts precede scan, Engine drains before registry retirement, caches flush safely, and restart reuses valid state |
| Ownership cleanup | Targeted repository search | No Engine catalog store, reverse include/dependency, provider abstraction, AssetDefinition, or obsolete forwarding surface remains |
| Documentation | Changed-doc and all-plan validators | Active plan and lasting asset/module contracts are valid |

## Definition of Done

- `AssetRegistry` is a dedicated shared runtime module below Engine.
- The module owns mounted discovery, bounded read-only DAST inspection,
  immutable asset metadata, redirect/dependency/reference indexes, persistent
  caches, query APIs, revisions, and atomic publication.
- Engine owns loading, residency, object construction, package writing, Cook,
  and mutation orchestration and has no access to registry private storage.
- Registry scanning constructs no asset objects and requires neither a package
  provider nor an AssetDefinition.
- Registry and Engine share one DAST Public Summary interpretation with no wire
  format or authored-content migration.
- Save, admission, relocation, deletion, fix-up, Undo, and Redo preserve atomic
  catalog/reference publication and existing recovery behavior.
- Runtime, editor, tool, and test consumers include and depend on the owning
  module directly where appropriate.
- Temporary forwarding headers, direct store friendships, duplicate parsers,
  and reverse dependencies are removed.
- Required tests, builds, smokes, compatibility checks, searches, and
  documentation validators pass with recorded evidence.
- Lasting asset and module documentation describes the implemented ownership,
  and the plan is completed only after every acceptance gate passes.

## Deferred Follow-ups

- UE-style arbitrary registry tags and tag-value filtering.
- Asynchronous background discovery, filesystem watching, scan progress UI,
  and incremental change events for editor presentation.
- Remote, cooked-manifest, or platform-specific registry backends.
- Memory-layout and string-table optimization for very large registries.
- Consolidating editor factories, workspaces, thumbnails, menus, and import
  policy behind a future AssetDefinition layer if independent duplication and
  lifecycle analysis justifies it.
- Supporting additional authored package formats or a provider architecture if
  Durin adopts more than one canonical format.

## Related Documentation

- [Asset Catalog and Mutation](../Runtime/Assets/AssetCatalogAndMutation.md)
- [Asset Packages](../Runtime/Assets/AssetPackages.md)
- [Asset Data Lifecycle and Storage](../Runtime/Assets/AssetDataLifecycle.md)
- [Code Modules](../Workspace/CodeModules.md)
- [Content Browser](../Editor/Architecture/ContentBrowser.md)
- [Agent Build and Run Workflow](../Agents/BuildAndRun.md)
- [Agent Testing Workflow](../Agents/Testing.md)

## Related Code

- `Engine/Source/Runtime/Engine/Public/Asset/Catalog.h`
- `Engine/Source/Runtime/Engine/Public/Asset/References.h`
- `Engine/Source/Runtime/Engine/Public/Asset/PackageInspection.h`
- `Engine/Source/Runtime/Engine/Private/Asset/AssetCatalog.cpp`
- `Engine/Source/Runtime/Engine/Private/Asset/AssetCatalogFacade.cpp`
- `Engine/Source/Runtime/Engine/Private/Asset/AssetCatalogPersistence.cpp`
- `Engine/Source/Runtime/Engine/Private/Asset/AssetCatalogStoreInternal.h`
- `Engine/Source/Runtime/Engine/Private/Asset/AssetReference.cpp`
- `Engine/Source/Runtime/Engine/Private/Asset/AssetPackageV6Codec.cpp`
- `Engine/Source/Runtime/Engine/Private/Asset/AssetRuntimeStateInternal.h`
- `Engine/Source/Runtime/Engine/Private/Asset/AssetPackageOperations.cpp`
- `Engine/Source/Runtime/Engine/Private/Asset/AssetRelocation.cpp`
- `Engine/Source/Runtime/Engine/Private/Asset/AssetDeletion.cpp`
- `Engine/Source/Runtime/Engine/Private/Asset/AssetRedirectorFixup.cpp`
- `Engine/Source/Runtime/Engine/Engine.dmodule`
- `Engine/Source/Editor/ContentBrowser/ContentBrowser.dmodule`
- `Engine/Source/Editor/ContentBrowser/Private/Panels/ContentBrowserModel.cpp`
- `Engine/Source/Editor/AssetTools/AssetTools.dmodule`
- `Engine/Source/Editor/DurinEd/DurinEd.dmodule`
- `Engine/Tests/Native/EngineTests`
