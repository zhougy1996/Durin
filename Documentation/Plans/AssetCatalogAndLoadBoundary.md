# Asset Catalog and Load Boundary Plan

Summary: Establish one authoritative asset catalog and one redirector-aware load path before broader asset architecture consolidation.

Last reviewed: 2026-08-15

Status: Completed
Completed: 2026-08-15

## Current Status

This completed M0 child plan of the
[Asset Architecture Simplification Roadmap](../Roadmaps/AssetArchitectureSimplification.md).
All six stages and the M0 exit gate are complete. Stage 0 froze the production caller inventory and the current
exact, redirected, draft, refresh, and load behavior that must survive the
boundary change. The physical-I/O seam now distinguishes logical metadata
consumption from file bytes read, bounds discovery reads to 65,593 bytes, and
records the current registry-miss fallback as two physical reads before Stage 3
removes it. Stage 1 introduces owned exact results, revision-bound snapshots,
revisioned resolution results, and an atomic refresh result. Production catalog
consumers now use namespace-level facades and cannot retain pointers into the
registry map. Stage 2 separates persistent residency from drafts and is
complete. New assets remain in an explicit draft store until successful
single-package or atomic bundle publication promotes them into the package
store. Stage 3 is complete: ordinary load is catalog-authoritative, registry
misses perform no package I/O, load no longer updates catalog state, and an
explicit validate-and-admit operation owns intentional unindexed publication.
Stage 4 retired the manager/registry surface and split package, load, mutation,
and test support headers. Stage 5 completed focused and repository-wide native
validation, a full build, hidden-window editor smoke, performance-counter
checks, and lasting documentation publication.

The selected direction retains the redirector model completed by the
[Asset Redirectors Refactor Plan](Archive/2026-08/AssetRedirectors.md). A
redirector remains a persistent exact catalog entry and an old authored path
continues resolving after its real asset moves. This plan changes where that
behavior lives: resolution becomes a pure catalog operation and ordinary load
materializes only the final real package.

The current source exposes `FAssetRegistry` and `FAssetManager` from one large
public header, retains member and free-function versions of load and authoring
operations, stores new unsaved packages beside loaded persistent packages, and
allows a registry miss to probe the derived physical package path. The header
reader used by that path loads the complete file, after which normal package
load reads it again. `ScanMountedContent` records failures but returns a plain
success result, leaving callers to discover incomplete state through separate
diagnostic accessors.

## Goal

Make persistent asset discovery and ordinary loading follow one explicit
pipeline:

```text
published catalog revision
    -> exact lookup or pure redirect resolution
    -> final real package metadata
    -> one physical read and validated decode
    -> package-store publication
```

At completion, persistent catalog state, unsaved draft state, redirect
resolution, and loaded package residency are distinct. Callers use one public
facade style, cannot reach the stateful manager, and cannot accidentally turn a
missing catalog entry into a direct filesystem load.

## Scope

- Inventory every production and focused-test include and call site for
  `AssetSystem.h`, `FAssetManager`, registry queries, refresh, create, load,
  find-loaded, unload, and direct/exact load behavior.
- Split catalog query/value types, runtime load operations, and authoring
  mutation declarations into focused public headers.
- Replace public raw pointers into mutable registry storage with value results
  or revision-bound snapshots.
- Preserve exact lookup and redirect resolution as separate named operations
  with structured terminal states.
- Make catalog refresh return completeness, diagnostics, counters, and the
  published revision in one result.
- Introduce an internal package store for loaded persistent packages and a
  separate draft store for new unsaved packages.
- Route ordinary typed loads and dependency loads through one resolve-once,
  final-real-package path.
- Remove registry-miss physical-path probing, direct load-time discovery, and
  registry mutation as a side effect of ordinary load.
- Ensure one ordinary load attempt reads package bytes once; preserve current
  v4 decode and compatibility behavior until the dedicated compatibility child
  plan.
- Move the manager implementation, exact redirector construction seam, and
  failure injection out of production public headers.
- Migrate production callers and update lasting catalog/load documentation
  after behavior is qualified.

## Non-Goals

- Removing, virtualizing, or changing the v4 serialized representation of
  `DAssetRedirector`.
- Changing relocation, chain compression, Fix Up, deletion, Undo/Redo, external
  reference stores, or Cook semantics.
- Removing the migration framework, structure upgrader registry, compatibility
  risk packages, or canonical re-encoding; the roadmap's M2 plan owns those
  changes after this plan stabilizes load results.
- Redesigning create/save/move/delete transaction phases; M1 owns the authoring
  service and mutation boundary.
- Consolidating import providers, handlers, records, or asynchronous import
  requests.
- Changing package identity from `FAssetPath`, adding stable GUIDs, or adding
  streaming/remote load APIs.
- Retaining `AssetSystem.h` as a permanent compatibility umbrella after every
  caller has a focused replacement include.

## Design Decisions and Invariants

### One public access style

- Production callers use namespace-level AssetCore facades declared in focused
  headers. No public abstract service hierarchy is introduced while AssetCore
  has one implementation.
- `FAssetManager` becomes a private implementation detail owned by the AssetCore
  module and may be renamed or decomposed without affecting callers. Production
  code cannot call `FAssetManager::Get()` or hold manager, registry-container,
  package-store, or draft-store pointers.
- Public manager methods and free functions are not retained side by side.
  Direct manager callers migrate first; duplicate methods and the monolithic
  umbrella header are deleted in the same plan.
- Authoring operations whose transaction redesign belongs to M1 move to a
  focused mutation header without semantic change. This plan does not preserve
  a second copy in `AssetSystem.h`.

The intended header ownership is conceptual rather than a mandate for exact
filenames, but the final split must provide these non-overlapping surfaces:

| Surface | Owns | Does not expose |
| --- | --- | --- |
| Catalog query | Exact metadata values, resolved result values, snapshots, revision and explicit refresh result | Loaded objects, filesystem readers, mutable registry containers |
| Runtime load | Typed/untyped load, find-loaded, unload, load report | Redirector objects, registry mutation, direct physical path load |
| Authoring mutation | Existing create/save/move/delete/Fix Up declarations pending M1 | Manager state, runtime load internals, caller-driven rollback internals |
| Test support | Scoped catalog/package fixtures and named failure points | Process-global production toggles in ordinary public headers |

### Catalog values are stable across revisions

- Exact lookup returns an owned metadata value or explicit not-found result,
  not a `const FAssetData*` into a mutable map.
- Resolution returns an owned value containing requested path, final path,
  redirect chain, final real metadata, catalog revision, and one terminal
  status. It never returns a redirector object or requires a later lookup to
  recover the final metadata.
- Enumeration returns an immutable snapshot tied to one catalog revision.
  Content Browser and mutation analysis either finish against that snapshot or
  detect staleness before publication.
- Exact lookup remains the only occupancy truth. A redirector at `A` means `A`
  is occupied even when resolved lookup returns real asset `B`.

### Resolution is pure and bounded

- Resolution uses only one published catalog snapshot. It performs no package
  read, object construction, load, refresh, save, path rewrite, chain
  compression, or registry mutation.
- Existing direct-to-final relocation semantics remain unchanged. The resolver
  still accepts bounded chains to diagnose externally modified, merged, or
  corrupt content and reports missing target, cycle, depth overflow, and final
  type mismatch deterministically.
- Expected-class validation applies to final real metadata. Ordinary typed load
  never exposes `DAssetRedirector` when the final asset satisfies the expected
  class.
- Chain compression remains an authoring mutation performed by relocation, not
  a read-side optimization hidden inside resolution.

### Persistent, draft, and loaded state are distinct

- The catalog contains persistent filesystem-backed real and redirector
  entries only. A catalog lookup cannot return a draft.
- A new unsaved package is published to a dedicated draft store with explicit
  authoring-only lookup. Its path still reserves an authoring destination, but
  this reservation is reported separately from exact persistent catalog
  occupancy.
- Save publishes a successful draft as a persistent catalog entry and removes
  the draft only after bytes and catalog state commit. Failure leaves one valid
  draft and no partial persistent entry.
- The package store caches materialized final real packages by exact final path.
  Redirector packages are absent from ordinary residency. Internal inspection
  may construct one exactly only through a private tool/test seam.

### Ordinary load is catalog-authoritative

- Ordinary load accepts an authored asset path, resolves it against one catalog
  revision, validates final class metadata, and loads the final real package.
- `NotFound` remains `NotFound`. The load path does not derive a physical path,
  inspect an unindexed file, search drafts, refresh the catalog, or add a
  registry entry.
- A caller that intentionally operates on a draft uses the authoring draft API.
  A recovery/import workflow that intentionally admits an unindexed file must
  validate and publish it through an explicit editor operation before calling
  normal load.
- Dependency loads use the same resolver and package store. They do not call an
  exact physical fallback or create a redirector object.
- One load attempt obtains package bytes once and passes the same owned buffer
  through validation and decode. Header metadata used for discovery remains a
  separate bounded scan concern; load does not reread the file after its own
  header inspection.
- Loading does not mutate catalog truth. Registry/cache mutation belongs to
  refresh and authored publication only.

### Refresh completeness is part of the result

- Refresh returns one value containing requested mode, whether a complete
  catalog and reference projection were published, prior and resulting
  revisions, counters, warnings, and structured errors.
- A failed or incomplete refresh does not publish a partially built catalog as
  success. The prior complete revision may remain available, but the result
  states that it is retained and potentially stale.
- Callers declare whether they require catalog completeness, reference-index
  completeness, or only the retained prior snapshot. Relocation may remain
  independent of reference-index completeness; strict Fix Up and proofs of
  zero incoming references may not.
- Diagnostics required for a decision travel with the operation result rather
  than through mutable manager-owned `GetLast...Errors()` state.

### Failure behavior is strict without changing redirect compatibility

- Missing, duplicate, corrupt, unsupported-format, incompatible-schema,
  redirect-cycle, depth-overflow, wrong-class, read, and decode failures remain
  distinguishable in load reports.
- This plan does not weaken current compatibility acceptance inside the v4
  decoder. It only prevents a failed catalog lookup from selecting a weaker
  physical path.
- Existing authored/cooked payload policies remain unchanged until M4. The
  boundary introduced here must carry enough context for a later immutable
  domain without adding another global mode switch.

## Current Foundations and Gaps

| Area | Existing foundation | Gap addressed by this plan |
| --- | --- | --- |
| Exact metadata | `FindAssetExact`, registry entry kind/destination, revisioned live map | Raw pointer results expose mutable-container lifetime and share a broad public type |
| Resolution | Bounded metadata-only redirect following with structured states | Load surrounds it with registry-miss disk probing and later exact lookup |
| Registry refresh | Incremental/full scans, persistent snapshot reconciliation, counters and diagnostics | Plain success result can coexist with side-channel failures and retained ambiguity |
| Header discovery | Bounded v4 header reader exists for registry scanning | The registry-miss load helper reads the entire package and normal load reads it again |
| Loaded packages | One cached package per path and dependency-aware unload | Persistent loads, drafts, compatibility risk, and exact redirect inspection share manager maps/seams |
| Public callers | Most production users already call free facades | `FAssetManager`, free duplicates, templates, mutation types, and testing controls remain in one 1,200-line header |
| Redirectors | Completed exact/resolved split, direct aliases, editor integration and cooked canonicalization | Ordinary load and package-store ownership are not isolated from the surrounding manager architecture |

## Stage 0 Inventory and Baseline

The inventory was frozen with targeted repository searches for the umbrella
include and every manager, registry, load, create, save, unload, relocation,
deletion, Fix Up, and scan spelling. At this baseline, 113 files include
`AssetSystem.h` directly: 81 production files and 32 native-test files. Direct
`FAssetManager::Get()` access occurs in eight production files and ten test
files. The production exceptions are the AssetCore implementation, codec,
migration and canonical-resave internals; two LevelEditor Content Browser
files; and the EnvironmentLightingBake program. All other production callers
already use namespace-level facades even though they still receive the
monolithic header.

Every inventoried caller has the following destination. The Stage 4 retirement
search repeats the same symbol and include queries and must reach zero for the
manager and umbrella surfaces.

| Current caller family | Current production footprint | Destination |
| --- | --- | --- |
| Exact lookup, resolution, enumeration, scan and reference-index inspection | AssetCore, DurinEd, LevelEditor, MainFrame, StandardAssetImport, Engine and focused tests | Public catalog values, snapshots and refresh result |
| Typed/untyped load, soft load, loaded lookup, unload and load snapshots | AssetCore codecs, Runtime Engine/Renderer, editor asset consumers, import code and focused tests | Public runtime-load facade backed by the private package store |
| Create, save, redirector, relocation, deletion and Fix Up | AssetCore, editor authoring/import modules, EnvironmentLightingBake and focused tests | Focused authoring-mutation facade; transaction semantics remain unchanged until M1 |
| Manager lifecycle, storage access and exact redirector construction | AssetCore internals plus the three direct external production callers named above | Private AssetCore implementation; editor callers use load/catalog facades and the program uses lifecycle/facade entry points |
| Failure injection and fixture lifecycle | Focused native tests, with two LevelEditor source files currently including test enums | Dedicated test-support surface; no production public toggle |
| Package format, compatibility and mutation value types included only transitively | AssetCore format headers and Engine asset types | Smallest owning format, catalog, load or mutation header |

Two behaviors depend on state overlap or fallback today:

- `FPackageAssetTests.ManualScanMountsRequireExplicitAdmissionBeforeLoading`
  deliberately creates a valid package under a mount with `bAutoScan == false`,
  removes it from residency, observes an exact-catalog miss, and succeeds only
  because ordinary load derives a physical path and reads it. No production
  caller was found that requires this behavior. Stage 3 replaces the test with
  fail-closed ordinary load followed by explicit editor admission or refresh;
  non-auto-scan mounts remain intentionally absent from automatic refresh.
- `CreateAsset` inserts a new unsaved package into `LoadedPackages` without a
  catalog entry. A subsequent ordinary load therefore succeeds through the
  registry-miss cache branch. Import candidates, new editor assets, temporary
  authoring outputs and tests require access to these objects, but none require
  them to masquerade as persistent catalog loads. Stage 2 moves them to an
  explicit draft store and migrates those authoring workflows to draft lookup.

The current result mapping to preserve or intentionally tighten is:

| Scenario | Current result | Selected boundary result |
| --- | --- | --- |
| Exact real asset | `FAssetData*` into the live map | Owned exact metadata value plus catalog revision |
| Exact redirector | Redirector metadata pointer; path is occupied | Owned redirector metadata value; exact identity remains occupied |
| Resolved real or alias | Structured final path/chain/metadata, but no revision | Owned result with requested/final path, chain, final metadata and revision |
| Missing path | Resolver says `NotFound`; load may still probe disk or find an unsaved package | `NotFound`; draft lookup and external admission are separate explicit operations |
| Missing target | `MissingRedirectTarget` mapped to `NotFound` | Same terminal resolution state and load error |
| Cycle or depth overflow | Both map to `CircularDependency` | Preserve distinct resolution states and the load error mapping |
| Unknown or wrong final class | `UnknownClass` or `TypeMismatch` | Preserve status after final-metadata validation |
| Corrupt redirect metadata/package | `CorruptFile`; scan omits the entry but returns success | Preserve load error; refresh directly reports incomplete publication/retention |
| Loaded persistent package | Zero package-file reads | Same final-path cache hit |
| Unloaded persistent package | One counted full-package read per package in the dependency closure | One physical read of each final real package |
| Unsaved draft | Ordinary load succeeds from `LoadedPackages` | Ordinary load is `NotFound`; explicit authoring draft lookup succeeds |
| Scan parse/reference failure | Plain success with `GetScanErrors()` or reference-index side channels | One refresh result reports catalog/reference completeness, diagnostics and retained/published revision |

Existing focused coverage freezes exact occupancy, metadata-only redirect
resolution, direct-to-final aliases, restart, cache recovery, missing target,
cycle/depth/type failures, redirected typed load, no redirector residency,
dependency loading, partial-load rollback, and Cook canonicalization. The
large-header fixture currently reports less than 1 KiB of logical metadata for
an 8 KiB-plus package, while `ReadAssetPackageHeader` physically calls
`LoadFileToArray`; the Stage 0 I/O seam must expose both physical and logical
bytes. `FAssetLoadReport::PackageFileReadCount` currently counts one full read
per unloaded package after resolution (two for an owner plus one dependency),
but does not count the registry-miss header probe because the counter is reset
after that probe. The new baseline test must make that hidden second read
observable before Stage 3 removes it.

Refresh consumers require the following completeness levels:

| Consumer | Required result |
| --- | --- |
| Runtime/editor startup and normal load | Complete catalog revision; startup must not silently continue as though a failed new scan were complete |
| Content Browser | Complete catalog for authoritative projection; diagnostics remain displayable with an explicitly retained prior snapshot |
| Relocation | Complete exact catalog; reference-index incompleteness does not block the move |
| Deletion | Complete exact catalog and fail-closed persistent-reference evidence for destructive publication |
| Strict Fix Up and zero-incoming proof | Complete catalog, complete reference index and all required external stores |
| Cook | Complete catalog and successful resolution of every root and dependency before manifest publication |
| Import/reimport publication | Existing complete or explicitly retained catalog revision plus atomic authored publication; no load-time discovery |
| Tests and recovery tools | Declare whether they require complete catalog, complete references, or retained-prior inspection; intentional unindexed admission is explicit |

Stage 0 validation used the `windows-msvc-x64` profile and
`Win64-Debug-DurinEditor` preset. `AssetPackageTests` built successfully, the
three focused baseline cases passed, and the full target passed all 97 tests.
The instrumented fixture records 65,593 physical header bytes for a package
larger than that bound; registered cold load, loaded cache hit, and current
unindexed fallback record one, zero, and two package-file reads respectively.

## Implementation Stages

### Stage 0: Freeze catalog and load behavior

- [x] Inventory every production include and symbol use of `AssetSystem.h`,
  `FAssetManager`, `FAssetRegistry`, exact/resolved lookup, refresh, create,
  load, find-loaded, unload, and internal exact load; assign every caller to the
  catalog, runtime-load, authoring-mutation, internal, or test surface.
- [x] Identify every production workflow that currently succeeds only because
  registry-miss load probes disk or because a draft is found in the loaded
  package map. Name its explicit replacement or prove it is unneeded before
  removing the fallback.
- [x] Record the existing result/status mapping for real, redirected, missing,
  corrupt, wrong-class, loaded, unloaded, dependency, draft, and registry-scan
  cases.
- [x] Add or identify focused characterization coverage for exact occupancy,
  redirect resolution, direct-to-final aliases, chain/cycle/missing-target
  diagnostics, typed load through an alias, restart, and no redirector leakage.
- [x] Add an instrumented large-package fixture proving the registry header
  reader is bounded and ordinary load performs one physical file read.
- [x] Record refresh completeness requirements for startup, Content Browser,
  relocation, deletion, strict Fix Up, Cook, import/reimport, and tests.

#### Acceptance Gate

- Every production call site and fallback-dependent workflow has one named
  destination; no unresolved consumer requires implicit physical loading.
- Characterization tests distinguish exact and resolved identity and preserve
  every completed redirector invariant.
- The selected value/result types and failure mapping are recorded before a
  public signature changes.
- The baseline reports header-scan bytes and ordinary-load file-read count so
  later stages can prove the duplicate path is gone.

### Stage 1: Introduce the catalog value boundary

Dependencies: Stage 0 caller and behavior inventory.

- [x] Extract catalog metadata, exact lookup, resolution, immutable snapshot,
  revision, refresh-mode, and refresh-result declarations into a focused public
  catalog surface.
- [x] Replace raw registry-entry pointers at the public boundary with owned
  values or revision-bound snapshot views whose lifetime is explicit.
- [x] Make resolution return requested path, final path, complete bounded chain,
  final metadata, catalog revision, and terminal state in one value.
- [x] Replace scan side-channel status with one refresh result that states
  publication/completeness, retained prior revision, counters, warnings, and
  structured errors.
- [x] Keep exact lookup and resolution free of package I/O, object construction,
  catalog mutation, and implicit refresh; add assertions or focused tests at
  the implementation seam.
- [x] Migrate registry inspection, Content Browser projection, collision
  checks, and load preflight to the new values without changing behavior.

#### Acceptance Gate

- Catalog consumers cannot retain a dangling pointer across refresh.
- Exact lookup still exposes redirector occupancy while resolution returns the
  same final real path, chain, and diagnostics as the baseline.
- Failed/incomplete refresh is visible in its direct return value and never
  masquerades as a newly complete published catalog.
- Focused catalog, redirector, snapshot, registry-cache, and Content Browser
  tests pass with no package construction during query or resolution.

Stage 1 validation used the `windows-msvc-x64` profile and
`Win64-Debug-DurinEditor` preset. `AssetPackageTests` passed all 97 cases,
`AssetImportCoreTests` passed all 27 cases, and `EditorAssetWorkflowTests`
passed 80 cases with its directory-symlink case skipped because the Windows
test process lacks symlink privilege. The catalog tests preserve an owned
pre-refresh snapshot across a rejected duplicate-path refresh, prove the prior
revision and entries remain unchanged, and receive completeness, counters, and
structured errors from the refresh result itself. Unsupported package refresh
likewise retains the prior owned exact entry and revision. Resolution still
leaves both alias and final packages unloaded, while returning the requested
path, final path, complete chain, owned final metadata, and catalog revision.
Repository search finds no production `GetAssetRegistry()` caller outside the
AssetCore implementation; Content Browser, source indexing, collision checks,
startup, and load preflight use the catalog facade values or snapshots.

### Stage 2: Separate package residency and drafts

Dependencies: Stage 1 catalog values and refresh results.

- [x] Introduce a private package-store owner for loaded final real packages and
  move package cache, dependency retention, unload, and GC interaction behind
  it.
- [x] Introduce a separate private draft-store owner for new unsaved packages,
  path reservations, draft lookup, successful save publication, and failed-save
  retention.
- [x] Remove draft lookup from ordinary persistent load while preserving
  explicit authoring access to newly created assets.
- [x] Route package-store load from final catalog metadata through one owned
  byte buffer so one attempt performs one physical package read before decode.
- [x] Prevent ordinary package-store insertion of redirector objects; retain a
  narrowly scoped private exact inspection seam only for validated tooling and
  tests that require body/header agreement.
- [x] Preserve loaded-package identity, hard-dependency retention, unload/GC,
  save, and relocation behavior while changing internal ownership.

#### Acceptance Gate

- Persistent catalog entries, drafts, and loaded final packages have separate
  stores and observable state transitions.
- An unsaved draft cannot make ordinary persistent load succeed, but create,
  edit, save, failure, and retry workflows remain functional.
- Redirected load caches only the final real package and never exposes or
  retains a redirector through the ordinary package store.
- Instrumented load tests prove one physical read per unloaded package attempt;
  dependency and repeated-loaded cases retain their expected behavior.

Stage 2 validation used the `windows-msvc-x64` profile and
`Win64-Debug-DurinEditor` preset. `AssetPackageTests` passed all 97 cases,
`AssetImportCoreTests` passed all 27 cases, `AssetImportTests` passed all 17
cases, and `SceneImportTests` passed all 15 cases. The affected editor target
compiled and its focused destination-occupancy case passed. Package coverage
proves draft creation leaves the catalog revision and persistent-cache dirty
state unchanged, persistent and draft lookups are disjoint, ordinary and soft
loads do not resolve a draft, successful save promotes exactly once, failed
save retains the same draft for retry, and failed atomic bundle publication
keeps its unpublished package out of persistent residency. Existing cold load
still records one physical file read and loaded reuse records zero. Production
import, collision, cleanup, and candidate-selection paths now query drafts
explicitly where authoring occupancy is required.

### Stage 3: Make ordinary load catalog-authoritative

Dependencies: Stage 2 package and draft stores.

- [x] Route every typed, untyped, hard-reference, soft-reference, and dependency
  load through one `Resolve -> validate final metadata -> package store` path.
- [x] Remove registry-miss physical-path derivation, complete-file header probe,
  internal exact fallback, and registry insertion performed by successful
  ordinary load.
- [x] Route intentional external-file admission through an explicit editor
  validate-and-publish operation or existing import/catalog-refresh workflow;
  keep it out of runtime load headers.
- [x] Ensure load reports retain requested and final paths, redirect chain, one
  catalog revision, final class, and exact failure status without consulting
  mutable last-error state.
- [x] Verify catalog refresh, authored save/import publication, relocation,
  Fix Up, deletion, and Undo/Redo remain the only paths that change persistent
  catalog truth.
- [x] Add regression coverage proving a valid unindexed `.dasset` does not load
  until an explicit publication/refresh operation admits it.

#### Acceptance Gate

- Repository production code contains one ordinary package load pipeline and no
  registry-miss disk probe or load-time catalog insertion.
- Real and redirected loads preserve typed results, authored soft-reference
  identity, dependency retention, reload, and final-package caching.
- Missing, unindexed, corrupt, cycle, depth, and wrong-class inputs fail through
  their structured statuses without partial package or catalog publication.
- Explicit editor recovery/import remains possible without adding a second
  runtime load path.

Stage 3 validation used the `windows-msvc-x64` profile and
`Win64-Debug-DurinEditor` preset. `AssetPackageTests` passed all 97 cases,
`AssetImportCoreTests` passed all 27 cases, and `EditorAssetWorkflowTests`
passed 80 cases with the existing directory-symlink privilege skip. The manual
scan fixture now proves a valid unindexed package returns `NotFound`, records
the requested catalog revision, performs zero package-file reads, and remains
unloaded until `AdmitAssetPackageToCatalog` validates and publishes it. Its
following ordinary load uses the admitted final metadata and reads the package
once. Redirected load reports the authored request, final real path, complete
chain, catalog revision, final class, success status, and one cold read; a
wrong typed request reports `TypeMismatch` without another read. Repository
search finds no load transaction registry projection or registry-miss header
probe; remaining `GetPhysicalPath` use in the load area belongs only to the
named migration-exact seam.

### Stage 4: Hide the manager and remove duplicate public surfaces

Dependencies: Stage 3 authoritative load path.

- [x] Split runtime load and existing authoring mutation declarations into
  focused public headers and migrate all production includes to the smallest
  owning surface.
- [x] Move `FAssetManager`, registry/package/draft storage, exact redirector
  construction, internal load phases, and failure injection into private or
  test-support code.
- [x] Remove public member/free duplicates, direct `FAssetManager::Get()` calls,
  load templates coupled to manager internals, and the `AssetSystem.h` umbrella
  after caller migration.
- [x] Keep M1-owned mutation semantics unchanged while ensuring their public
  declarations no longer expose manager state or internal load helpers.
- [x] Remove helpers proven unused by the Stage 0 inventory and record any
  intentionally retained temporary mutation surface as an M1 entry-gate item.
- [x] Run repository searches for retired includes, manager access, raw catalog
  pointers, direct-load fallbacks, and duplicate entry points.

#### Acceptance Gate

- No production header exposes `FAssetManager`, mutable registry/package-store
  ownership, exact load internals, or failure-injection controls.
- Every production caller includes a focused catalog, load, or mutation header;
  `AssetSystem.h` and duplicate operation spellings have no references and are
  removed.
- One public operation has one implementation entry; authoring operations retain
  existing behavior and are ready for M1 transaction simplification.
- A clean affected build proves the header split introduces no hidden include
  dependency.

Stage 4 retired the 1,200-line `AssetSystem.h` umbrella and moved its stateful
implementation into private `FAssetRuntimeState` and `FAssetCatalogStore`
types. Package-format values now live in `AssetPackage.h`, runtime loading and
immutable reference snapshots in `AssetLoad.h`, M1-owned authoring operations
in `AssetMutation.h`, and deterministic failure controls in
`AssetTestSupport.h`. Production and native-test callers were migrated to an
owning surface; production source has no test-support include. The retained
relocation, deletion, Fix Up, observer, contributor, and reference-store APIs
are the explicit M1 entry gate: their behavior is unchanged here so M1 can
replace public phase orchestration without overlapping the catalog/load cut.

Retirement searches find no `AssetSystem.h`, public `FAssetManager`, public
`FAssetRegistry`, external `FAssetRuntimeState::Get()`, or external mutable
catalog-store access. `AssetPackageTests` builds and passes all 97 cases, and
the affected `LevelEditor` target builds with the focused headers under the
`windows-msvc-x64` / `Win64-Debug-DurinEditor` profile.

### Stage 5: Validate and publish the boundary

Dependencies: Stages 0-4 complete.

- [x] Run focused AssetCore package, registry, redirector, reference, Cook, and
  mounted-source tests using the repository test workflow.
- [x] Run affected AssetImportCore and editor Content Browser/move/deletion
  suites to prove the public split and catalog values preserve integrations.
- [x] Run the required affected targets, complete native-test qualification,
  and hidden-window editor smoke through the repository build/run workflow.
- [x] Measure cold/warm refresh header reads and bytes, direct/redirected load
  physical reads, and loaded-package cache hits against the Stage 0 baseline.
- [x] Update Asset Packages, Asset Data Lifecycle, Content Browser, and any
  affected import documentation with the implemented catalog/load boundary;
  remove descriptions of registry-miss direct load.
- [x] Run changed-document, all-plan, all-roadmap, and repository documentation
  validation and record the final implementation handoff.

#### Acceptance Gate

- Focused, affected, complete-native, build, and editor-smoke evidence passes
  under one recorded profile with no unexpected skip or fallback.
- Ordinary unloaded load performs one physical package read, warm loaded access
  performs none, and catalog resolution performs no file I/O.
- Redirector relocation, restart, Fix Up, deletion, Undo/Redo, and Cook behavior
  remains equal to the completed redirector baseline.
- Lasting documentation describes the new catalog-authoritative load contract,
  and this plan contains evidence for every completed stage and roadmap M0 exit
  gate.

Stage 5 used the `windows-msvc-x64` profile and
`Win64-Debug-DurinEditor` preset. `AssetPackageTests` passed 97/97,
`AssetImportCoreTests` 27/27, `AssetImportTests` 17/17, `SceneImportTests`
15/15, and `EditorAssetWorkflowTests` passed 80 cases with its existing Windows
directory-symlink privilege skip. The complete ordinary native aggregate built
and passed all 72 registered targets. A full `all` build passed, followed by a
hidden-window `DurinEditor` run against `Sandbox.dproject` for eight ticks.

The retained counter assertions match the Stage 0 contract: bounded discovery
reads at most 65,593 physical header bytes; a cold direct or redirected final
package load records one physical read; a warm residency hit records zero; an
unindexed catalog miss records zero; and explicit admission followed by load
records one. Incremental refresh reparses changed entries only, a stable warm
refresh performs zero header/payload reads, and full validation reads every
enumerated source. The complete native pass also qualified redirector move,
restart, Fix Up, deletion, Undo/Redo, Cook, authored rollback, and explicit
Cooked catalog refresh across their owning targets.

`AssetPackages.md`, `AssetDataLifecycle.md`, `ContentBrowser.md`, and
`AssetImportFramework.md` now own the lasting catalog, draft, residency,
refresh-result, and public-header contracts. Changed-document, all active and
completed plan, all roadmap, and repository-wide documentation validation pass.

## Validation Matrix

| Area | Scenarios | Required evidence |
| --- | --- | --- |
| Exact catalog | Real asset, redirector, missing path, duplicate path, refresh revision | Exact occupancy and value lifetime remain correct across refresh |
| Resolution | Direct alias, chain, compressed chain, missing target, cycle, depth limit, type mismatch | Pure no-I/O result contains requested/final paths, chain, revision, metadata and stable status |
| Refresh | Cold, warm, full, delta, corrupt cache, parse failure, retained prior revision | One result reports completeness and no partial map is published as success |
| Real load | Loaded/unloaded, typed/untyped, dependency, unload/reload, read/decode failure | One final-package path, one read when unloaded, no catalog mutation |
| Redirected load | Hard and soft reference, expected class, repeated move, restart | Final real object returned, authored soft path unchanged, redirector not resident or exposed |
| Missing/unindexed | Missing file, valid unindexed file, stale snapshot | Ordinary load fails; explicit refresh/admission is required before success |
| Draft | Create, lookup, edit, save, save failure, retry, destination collision | Draft state is separate and publication is atomic |
| Mutation regression | Move, folder move, move-back, Fix Up, delete, Undo/Redo, stale token | Existing redirector and catalog revision behavior remains unchanged |
| Cook regression | Redirected root, redirected hard/soft dependency, unresolved alias | Produced bytes are canonical and redirectors remain absent from runtime output |
| API surface | Include graph, symbol search, exported headers, failure injection | No public manager, raw mutable registry pointer, umbrella header, or duplicate entry remains |
| Performance/I/O | Cold/warm scan, large header fixture, direct and redirected unloaded load, cache hit | Header reads remain bounded; one unloaded load reads once; resolution and cache hits read zero bytes |
| Qualification | Focused suites, affected targets, full native cases, editor smoke, docs | All required evidence passes under the documented agent workflows |

Build and test selection, process-conflict checks, target invocation, and result
reporting follow the repository agent workflows rather than commands copied
into this plan.

## Definition of Done

- Persistent real assets and redirectors live in one authoritative exact
  catalog; drafts and loaded packages live in separate private stores.
- Exact lookup and redirect resolution return revision-safe values and remain
  distinct at every caller.
- Redirect resolution is bounded, pure, no-I/O, and behaviorally identical to
  the completed redirector contract.
- Ordinary load resolves once, validates final metadata, reads final package
  bytes once, and publishes only the final real package.
- Registry miss, valid unindexed content, and drafts cannot trigger implicit
  runtime loading or catalog mutation.
- Catalog refresh reports completeness and diagnostics directly and publishes
  one atomic revision or retains the prior revision explicitly.
- `FAssetManager`, raw mutable registry/package state, exact redirector loading,
  and failure injection are private; production calls use one facade style.
- `AssetSystem.h`, duplicate member/free operations, raw catalog entry pointers,
  and direct-load fallback code are removed after all callers migrate.
- Existing relocation, redirector, Fix Up, deletion, Undo/Redo, import, and Cook
  behavior passes its focused regression matrix.
- Required build, native-test, editor-smoke, performance/I/O, and documentation
  validation passes, lasting contracts are updated, and roadmap M0 is marked
  complete.

## Deferred Follow-ups

- Simplify create/save/move/delete/Fix Up into one authoring service and hide
  caller-driven transaction phases in roadmap M1.
- Split validated decode from offline canonical verification and remove unused
  runtime migration/structure compatibility surfaces in M2.
- Consolidate import provider, single-asset handler, record handler, and async
  ownership in M3.
- Remove unused build executor abstractions and make Authored/Cooked domain
  construction immutable in M4.
- Consider async streaming, bundles, stable non-path identities, asset
  consolidation, or cooked alias tables only under separate consumer-driven
  plans; none is implied by this boundary.

## Related Documentation

- [Asset Architecture Simplification Roadmap](../Roadmaps/AssetArchitectureSimplification.md)
- [Asset Packages](../Runtime/Assets/AssetPackages.md)
- [Asset Data Lifecycle](../Runtime/Assets/AssetDataLifecycle.md)
- [Asset Versioning](../Runtime/Assets/Versioning.md)
- [Content Browser](../Editor/Architecture/ContentBrowser.md)
- [Asset Redirectors Refactor Plan](Archive/2026-08/AssetRedirectors.md)
- [Agent Build and Run Workflow](../Agents/BuildAndRun.md)
- [Agent Testing Workflow](../Agents/Testing.md)

## Related Code

- [`AssetPackage.h`](../../Engine/Source/Runtime/AssetCore/Public/AssetPackage.h)
- [`AssetLoad.h`](../../Engine/Source/Runtime/AssetCore/Public/AssetLoad.h)
- [`AssetMutation.h`](../../Engine/Source/Runtime/AssetCore/Public/AssetMutation.h)
- [`AssetTestSupport.h`](../../Engine/Source/Runtime/AssetCore/Public/AssetTestSupport.h)
- [`AssetSystem.cpp`](../../Engine/Source/Runtime/AssetCore/Private/AssetSystem.cpp)
- [`AssetPackageV4Reader.h`](../../Engine/Source/Runtime/AssetCore/Public/AssetPackageV4Reader.h)
- [`AssetPackageV4Reader.cpp`](../../Engine/Source/Runtime/AssetCore/Private/AssetPackageV4Reader.cpp)
- [`MountedSource.h`](../../Engine/Source/Runtime/AssetCore/Public/Asset/MountedSource.h)
- [`MountedSource.cpp`](../../Engine/Source/Runtime/AssetCore/Private/Asset/MountedSource.cpp)
- [`EditorAssetMoveCoordinator.cpp`](../../Engine/Source/Editor/LevelEditor/Private/Assets/EditorAssetMoveCoordinator.cpp)
- [`PackageTests.cpp`](../../Engine/Tests/Native/AssetCoreTests/Private/PackageTests.cpp)
- [`PackageV4ReaderTests.cpp`](../../Engine/Tests/Native/AssetCoreTests/Private/PackageV4ReaderTests.cpp)
- [`MountedSourceTests.cpp`](../../Engine/Tests/Native/AssetCoreTests/Private/MountedSourceTests.cpp)
