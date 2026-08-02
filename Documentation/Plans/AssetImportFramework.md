# Asset Import Framework Plan

Summary: Replace asset-specific import orchestration with a synchronous-first provider framework for immutable source snapshots, single-asset reimport, multi-output records, detached candidates, and failure-atomic publication.

Last reviewed: 2026-08-02

Status: Active
Completed:

## Current Status

This plan supersedes the archived
[Ready-to-Use Static Model Import](Archive/2026-08/ReadyToUseStaticModelImport.md)
direction. Its completed stages remain evidence for existing glTF/FBX parsing,
generated material and texture behavior, failure injection, and package
publication, but none of its `StaticModel` names, root-StaticMesh ownership,
runtime manifest placement, or model-specific transaction boundaries are
prescriptive here.

The repository already has the essential durable storage primitives:
versioned `.dasset` packages, mounted `FSourcePath` identities, disposable DDC
objects, unpublished packages, failure-atomic package-bundle save, registry
publication, and asset-specific candidate exchange. The missing architecture
is a small editor framework that composes those primitives without treating
one output asset as the owner of its peers or making simple reimport depend on
a multi-output graph.

Stages 0 through 5 are complete in the squashed framework commit
`f24bdca143986e151c6d57ac00aef7d5d8c5e86e`, based directly on Multithreading
V1 commit `0d2851c95c311c3465e4199d11958b603541b0be`. Stage 6 is the current stage;
its scheduler dependency is satisfied. The
legacy glTF/FBX workflow and the transitional `EngineAssetBuild` and
Assimp-backed `AssetImport` modules have been replaced by
`StandardAssetImport`, the default aggregate for built-in
provider implementations.
The first implementation is deliberately synchronous on the editor thread.
It validates deterministic planning, persistence, reconciliation, and failure
semantics without depending on the unfinished task-system lifecycle. A later
stage may schedule immutable preparation work asynchronously without changing
the provider, plan, candidate, or publication contracts.

The Stage 6 baseline qualification passed all 47 focused concurrency tests,
all 16 `AssetImportCoreTests`, all 59 `TextureTests`, and the full DurinEditor
`all` build. Task-scheduler lifecycle smoke passed with both the project browser
and Sandbox loaded. The first Sandbox run nevertheless exited once with a
non-reproduced access violation after the scheduler audit had succeeded;
immediate equivalent reruns completed cleanly, so repeated loaded-project
shutdown remains an explicit Stage 6 gate rather than assumed evidence.

## Goal

- Give every import provider one common plan, preview, execute, diagnostics,
  cancellation, and publication lifecycle.
- Make a source document capable of producing any number of peer assets without
  selecting a StaticMesh, Material, Texture, or another output as their owner.
- Keep ordinary one-asset import and reimport small: valid source provenance and
  a provider are sufficient; no multi-output record is required.
- Persist a generic editor-only `DImportRecord` only when an import needs stable
  multi-source or multi-output reconciliation across editor sessions.
- Resolve and read declared sources once into an immutable snapshot; parsing,
  candidate building, hashing, and publication consume the same bytes.
- Publish authored package changes failure-atomically within the documented
  live-process boundary while treating DDC objects as disposable cache effects
  and source ingestion as a separate authoring action.
- Allow a concrete provider module to be removed without making its previously
  produced runtime assets unloadable or introducing provider code into cooked
  targets.
- Replace the existing static-model workflow incrementally, retaining only
  behavior whose acceptance evidence remains valuable.

## Scope

- Generic editor import requests, immutable source snapshots, plans, output
  previews, diagnostics, candidates, and execution results.
- Provider discovery by stable provider identifier and source format.
- Single-asset import and reimport for assets with embedded lightweight source
  provenance.
- Multi-output import and reimport through a versioned editor-only
  `DImportRecord` companion asset.
- Live-process failure-atomic authored-package publication through `AssetCore`
  primitives.
- Typed, no-fail imported-state exchange for already loaded output identities.
- Derived editor indexing from import records to their managed outputs.
- Migration of the existing StaticMesh, Texture2D, TextureCube, and glTF/FBX
  workflows onto the framework.
- Cooking, runtime-dependency, compatibility, failure, cancellation, and
  end-to-end validation.

## Non-Goals

- A universal runtime `DModel`, prefab, scene, or bundle asset.
- Persisting an import-time normalized scene graph as runtime content.
- Making DDC entries transactional or authoritative.
- Monitoring complete source directories or using timestamps as semantic
  identity.
- Automatically deleting outputs removed from a source document.
- Silently adopting or overwriting an output managed by another import record.
- A generic field-level merge language for arbitrary user edits. Version one
  uses provider-declared whole-state or typed-subobject replacement boundaries.
- Soft references, redirects, async runtime asset loading, or a repository-wide
  asset metadata query language.
- One framework-level material, texture, mesh, animation, or scene schema.
  Providers own their normalized representations and mapping policies.
- Recreating Unreal Engine Interchange APIs or terminology where Durin has no
  corresponding requirement.
- Depending on the general task system before the synchronous import contracts
  and their failure behavior pass. Responsiveness and cooperative cancellation
  are added only by the later asynchronous execution stage.

## Terminology

- **Source**: an authoritative file identified by `FSourcePath` in a mounted
  `SourceAssets` domain.
- **Source snapshot**: immutable bytes plus canonical identity and hash captured
  for one plan. Later phases never reopen a snapshot source for content.
- **Provider**: an editor implementation registered with the framework that
  recognizes sources, parses them, plans outputs, builds candidates, and
  supplies provider-specific reconciliation. A module may register multiple
  providers; provider and physical module boundaries are deliberately distinct.
- **Output**: an independently addressable `.dasset` produced or updated by an
  import. Outputs are peers even when ordinary runtime references connect them.
- **Import record**: editor-only provenance and reconciliation data for one
  multi-output import relationship. It records management, not structural
  containment or runtime ownership.
- **Candidate**: detached authored state that is fully validated before an
  existing asset identity changes.
- **Publication**: the bounded editor-thread operation that revalidates a plan,
  exchanges prepared state, and failure-atomically saves all changed packages.
- **Orphan**: a previously managed output absent from the new plan. It is
  reported and detached only by explicit policy; it is not automatically
  deleted.

Repository APIs and filenames must use `Asset`, `Import`, `Scene`, or the
concrete output type according to these meanings. New framework APIs must not
use `StaticModel`. A concrete single-output geometry workflow uses
`StaticMesh`; a heterogeneous source document may use `Scene`; generic
persistence uses `ImportRecord`.

## Design Decisions and Invariants

### Layering and module ownership

The target dependency direction is:

```text
Core / CoreDObject
  atomic bytes, reflection, object/package identity
          |
AssetCore
  package I/O, unpublished packages, bundle save, registry, DDC
          |
AssetImportCore (Editor)
  providers, snapshots, plans, records, candidates, publication coordinator
          |
Provider implementations (Editor)
  StandardAssetImport default aggregate, optional independent provider modules
          |
Editor hosts
  Content Browser, asset editors, dialogs, progress and diagnostics
```

- `AssetCore` remains unaware of import providers, materials, textures, meshes,
  source formats, or import records. Only independently useful package
  publication primitives may move into it.
- `AssetImportCore` depends on generic asset and source identities but not on
  concrete Engine asset classes.
- `StandardAssetImport` aggregates the built-in StaticMesh, texture, material,
  and Scene provider implementations. It organizes their typed adapters and
  mapping policy by domain internally rather than creating one physical module
  per output asset type.
- `StandardAssetImport` owns the Assimp link and runtime deployment required by
  its built-in geometry and Scene providers. Assimp-backed decoding is concrete
  provider implementation, not an `AssetImportCore` or Runtime `Engine`
  dependency.
- A provider becomes an independent module only when its optional dependencies,
  deployment, unload lifecycle, ownership, or release cadence require a
  separate boundary. New providers otherwise join `StandardAssetImport` without
  changing the generic framework.
- A provider owns parsing, normalized data, settings, output roles, imported
  state, and reconciliation rules for its domain.
- Editor hosts request capabilities from the framework. They do not show a
  reimport action merely because an object has a particular concrete class.
- Runtime asset types retain only runtime state and the minimum lightweight
  provenance required for their own single-asset rebuild. A runtime type never
  contains a Scene-provider-specific manifest.

### Synchronous semantic core, asynchronous preparation later

- Stages 0 through 5 execute capture, discovery, parse, plan, candidate build,
  validation, and publication synchronously on the editor thread. They do not
  submit worker tasks, own result mailboxes, or depend on the Multithreading V1
  lifecycle.
- The synchronous executor is the reference implementation for ordering,
  diagnostics, publication boundaries, persistence, and failure behavior. It
  remains directly callable by focused tests after asynchronous execution is
  introduced.
- Stage 6 may schedule only immutable source capture, hashing, dependency
  discovery, parsing, normalization, and candidate CPU preparation. Object and
  package operations retain their editor-thread ownership.
- One `AssetImportCore` coordinator owns asynchronous request admission,
  request serials, cancellation sources, task handles, immutable input leases,
  and a synchronized completed-result mailbox. Providers and editor hosts do
  not launch phase-specific tasks or own private worker pools.
- Core V1 deliberately has no typed task result or universal game-thread
  continuation pump. Workers publish move-owned import results into the
  coordinator mailbox; an existing editor-thread tick calls a narrow drain API.
  A mailbox entry is consumable only after its matching task handle is terminal.
- A task handle reaching terminal state does not by itself prove that the
  worker's callable wrapper has been destroyed. Before returning, the
  framework-owned callable must move its provider lease into the mailbox entry
  or release it explicitly, leaving no provider-owned capture for worker-side
  cleanup. Provider unload additionally drains or discards matching mailbox
  entries before checking that outstanding leases reached zero.
- Asynchronous execution is an adapter over the same semantic phases, not a
  second provider API. For the same snapshot, settings, prior state, and target
  revisions, synchronous and asynchronous preparation produce equivalent plans,
  candidates, diagnostics, and authored package bytes.
- Progress is phase-based in the synchronous implementation. Interactive
  responsiveness, cooperative cancellation during long preparation, task
  failure propagation, and shutdown draining are deferred until Stage 6.

### Two deliberately different reimport modes

**Single-asset reimport** requires only:

- a current asset identity;
- complete lightweight source provenance;
- valid provider identity, version, and settings;
- available declared source inputs.

It produces one candidate and failure-atomically saves one authored package.
StaticMesh geometry reimport follows this path and must work for compatible
legacy assets without a `DImportRecord`.

**Import-record reimport** starts from a `DImportRecord`, not from a selected
root output. It reparses the source snapshot, reconciles every managed output,
prepares all candidates, and publishes the changed packages plus the updated
record as one bundle. Selecting any managed output may navigate to this action
through the derived editor index.

The UI names the capabilities separately. A provider may offer one or both;
the presence of one never implies the other.

### Source acquisition is not an asset transaction

- Selecting an existing mounted source records a no-copy `FSourcePath`.
- Selecting an external file first runs the existing explicit ingestion
  workflow into a chosen writable SourceAssets destination. Successful
  ingestion may leave an unused authored source if subsequent import fails;
  this is safe and visible, unlike a partially published asset graph.
- Replacing shared source bytes and relocating source files remain separate,
  impact-aware operations. Import execution never silently mutates a shared
  mounted source.
- A provider resolves only dependencies declared by the root document or
  selected explicitly by the user. It does not watch, hash, or snapshot the
  containing directory.
- Relative dependencies resolve against their declaring source after mount
  containment, traversal, link-target, scheme, size, and count validation.
- The framework captures each physical root exactly once, then lets the selected
  provider inspect only captured bytes and request declared logical
  dependencies. It resolves and captures each requested dependency at most once
  and repeats this bounded discovery until the source closure is complete.
- Dependency discovery has framework-enforced depth, source-count, per-source,
  and aggregate-byte limits. Canonical identities detect duplicate requests and
  cycles; optional and missing dependencies remain explicit diagnostics.
- Planning starts only after the source closure is frozen into one immutable
  snapshot. The same captured bytes feed parsing, hashes, candidate builds, DDC
  keys, and persisted source records; later phases never reopen physical input.
- Embedded bytes belong to the snapshot. A provider may request an explicit
  later extraction action, but a temporary extraction path is never semantic
  identity.

This removes time-of-check/time-of-use verification from ordinary execution.
If a mounted file changes after snapshot capture, the completed asset records
the captured hash and correctly reports changed source on its next status
check; publication does not mix bytes from two revisions.

### Lightweight per-asset provenance

Single-asset workflows persist an asset-specific reflected field built from a
shared vocabulary:

```text
ProviderId
ProviderContractVersion
SettingsSchemaId, SettingsSchemaVersion, and normalized settings
ordered source identities, FSourcePath values, roles, hashes, and sizes
last successful authored-output fingerprint
```

Concrete assets may store typed settings, but source identity and provider
identity follow one common contract. Provenance contains no DDC path, physical
workstation path, timestamp identity, output graph, or UI session state.

Provider contract compatibility, settings serialization, provider-state
serialization, and derived-data invalidation use separate versions. A builder
or algorithm revision may invalidate DDC without making persisted settings or
provider state unreadable. Reimport selects the persisted `ProviderId`, applies
only registered schema readers or migrations, preserves unknown opaque payloads,
and refuses unknown newer schemas before mutation.

The framework exposes a type-erased view and provider capability query; it does
not require every runtime asset to inherit from a new import-owned base class.

### Multi-output persistence

`DImportRecord` is an editor-only main asset in its own `.dasset` package. Its
destination is explicit and previewed; the default is a sibling package named
`<SourceName>_Import`. Content Browser class filters may hide records from the
ordinary asset view without making their paths implicit.

The generic persisted shape is conceptually:

```text
RecordVersion
RecordId
ProviderId
ProviderContractVersion
Settings
  SchemaId, SchemaVersion, bounded normalized bytes, payload hash
Sources[]
  StableIdentity, Role, FSourcePath, ContentHash, ByteCount
Outputs[]
  StableIdentity, Role, FAssetPath, ManagementPolicy, LastPublishedFingerprint
AcceptedDiagnostics[]
ProviderState
  SchemaId, SchemaVersion, bounded opaque bytes, payload hash
```

- Sources and outputs are ordered canonically by provider-stable identity, not
  discovery order, filesystem enumeration, or allocation order.
- `FAssetPath` is used for output association without creating eager runtime
  strong dependencies. Asset move and delete contributors update or reject
  affected records explicitly.
- One output has at most one active managing record. Other imports may reference
  it but cannot claim or overwrite it.
- Settings and provider reconciliation state are distinct bounded payloads. The
  settings payload is sufficient to recreate a reimport request after restart;
  its fingerprint is verification and cache input, not a substitute for bytes.
- Provider state is an opaque, bounded, versioned payload so the generic record
  remains loadable and preservable while a provider module is unavailable.
  Only the matching provider decodes it.
- The generic record never contains `DStaticMesh`, `DMaterialInstance`,
  `DTexture2D`, slot, sampler, skeleton, or scene-node fields.
- A record may designate a primary output for navigation, but primary does not
  imply ownership and is not required.
- Records are authored editor packages, never DDC entries or JSON sidecars.
  Default cooks exclude their packages and all provider state.
- An ordinary package duplicate retains serialized evidence but is a management
  conflict until an explicit clone or repair action assigns a new `RecordId`.
  A clone begins without `Managed` claims; duplicate records never race to pick
  an authoritative manager.

### Derived import-record index

- `AssetImportCore` builds a rebuildable editor index from registry-visible
  `DImportRecord` packages and their bounded generic summaries.
- The index maps each managed output path to its record and output identity,
  detects duplicate managers, and provides Content Browser relationships.
- Duplicate `RecordId` values or multiple managers put every affected record in
  a conflict state. No affected record may publish until the conflict is
  explicitly repaired.
- The record packages remain authoritative. The index may live under `Saved/`
  or the DDC and may be deleted without data loss.
- Version one may load the small population of import records during index
  construction. A future searchable-property/header-summary contract may
  remove that cost; this plan does not add a general asset query system solely
  for import records.

### Provider contract

A provider has stable identity and implements bounded phases equivalent to:

```text
CanImport(root descriptors and bounded captured prefixes)
CaptureSettings()
DiscoverDependencies(captured sources, dependency request sink)
Plan(snapshot, previous record or target asset)
BuildCandidates(plan)
ValidateCandidates(candidates)
PrepareImportedStateExchange(target, candidate)
CommitImportedStateExchange(prepared exchange)
ReverseImportedStateExchange(prepared exchange)
EncodeProviderState(result)
```

- Initial import treats multiple matching providers as an explicit ambiguity
  unless the user selects a provider; reimport resolves the persisted
  `ProviderId` and never silently falls back to another provider.
- `Plan` and `BuildCandidates` do not mutate packages, the registry, loaded
  assets, mounted source bytes, or user settings.
- Plans contain all output paths, collision decisions, resource estimates,
  warnings, and management actions needed for preview.
- A plan owns or references immutable snapshot bytes; execute never reparses a
  changed physical file.
- Provider diagnostics use framework categories and stable source/output
  identities while retaining provider-specific details.
- Providers declare allocation budgets before decoding or candidate creation.
- Provider-specific state exchange is typed and registered for exact target and
  candidate classes. `PrepareImportedStateExchange` performs every operation
  that may fail, including runtime-resource preparation. Commit and reverse are
  no-fail and involutive for authored and immediately observable runtime state;
  asynchronous resource completions use revisions so stale work cannot publish.
- Plans, candidates, and execution results retain a provider lease. Provider
  unregistration closes admission, and module unload is rejected until every
  lease and any registered exchange operation has been released.
- Arbitrary `std::function` apply/rollback callbacks are not part of the public
  publication API.

### Optimistic plans and stale-plan rejection

- Preview does not lock assets. Every plan records the base import-record
  fingerprint, derived-index and registry revisions, and the path, class,
  package edit revision or authored fingerprint, and management owner observed
  for every existing target.
- Publication acquires one editor-thread import-publication guard and rechecks
  those preconditions immediately before any state exchange. A changed target,
  record, path occupant, management owner, or relevant index revision returns a
  structured `StalePlan` result and mutates nothing.
- Stage 0 identifies the smallest generic edit-revision or package-fingerprint
  primitive needed when current package dirty state cannot distinguish edits
  made after preview. Providers do not implement their own locking scheme.
- The guard serializes only final preflight and publication. It does not turn
  source capture, preview, parsing, or candidate construction into a global
  critical section.

### Publication and failure model

Publication deliberately protects authored packages, not every cache side
effect:

1. capture and validate immutable sources;
2. build detached candidates and any required in-memory runtime data;
3. populate disposable DDC objects as needed for immediate editor use;
4. prepare typed imported-state exchanges and all other potentially failing
   runtime work without mutating existing identities;
5. on the editor thread, acquire the publication guard and revalidate every
   recorded target, record, registry, index, and collision precondition;
6. commit prepared imported state into existing identities through typed
   no-fail operations;
7. call `Asset::SavePackagesAtomically` for all changed output packages and the
   updated `DImportRecord`, designating the record as the root published last;
8. on save failure, reverse the state exchanges and report both the primary
   failure and any invariant violation;
9. on success, finalize notifications, discard candidates, release the guard,
   and publish the new derived index revision.

- New assets remain unpublished packages until the bundle commit succeeds.
- DDC objects created by a failed attempt may remain; they are content-addressed
  disposable cache entries and do not require rollback.
- Source ingestion is already complete before this lifecycle and is not rolled
  back with asset packages.
- Orphans are reports, not deletions. Explicit detach or delete is a separate
  impact-checked operation.
- Package-bundle save is failure-atomic for errors returned within the live
  process. Version one does not claim crash-atomic multi-file publication or
  ACID semantics across arbitrary filesystems and loaded object code. Root-last
  publication plus recorded output fingerprints makes an interrupted mismatch
  detectable after restart; repair is explicit and never overwrites silently.
- If existing `AssetCore` primitives cannot stage a required package set or
  preserve loaded identities without provider callbacks, Stage 0 must record
  the smallest generic package API addition before implementation continues.

### User edits and management policy

Version one supports three generic output policies:

- `Managed`: the provider may replace its declared imported state on record
  reimport.
- `Referenced`: the record uses an existing asset but never mutates it.
- `Detached`: the former output remains an ordinary asset and is no longer
  reconciled by the record.

Management policy is persisted separately from transient reconciliation state.
`Present`, `Missing`, `Orphaned`, and `Collision` describe what a plan observes;
`Keep`, `Recreate`, `Detach`, `Adopt`, and `Reject` describe proposed actions.
They are not additional management policies. Detached entries remain bounded
tombstones until an explicit forget action, so a later source element cannot
silently reclaim their prior path.

There is no generic field-level merge. A provider documents its imported-state
boundary. For example, a Scene provider may replace an importer-managed
material instance while preserving component material overrides and stable mesh
slot identities. Durable user customization uses an independent asset,
inheritance, an override, or explicit detach.

An unrelated asset at a proposed path, an output managed by another record, an
unknown newer provider-state schema, or an ambiguous stable identity fails
preflight before candidate publication.

### DDC and cooking

- Each output computes its own DDC keys from only the source subset, settings,
  provider/builder versions, platform, and schema that can affect that output.
  A Scene record fingerprint never forces unrelated mesh geometry and texture
  payloads to share one invalidation key.
- DDC hits and misses do not alter import-record identity or authored package
  bytes except for separately persisted lightweight diagnostics when required.
- `DImportRecord`, provider state, source paths, import diagnostics, and editor
  import modules are excluded from default cooked dependency closure.
- Cooked outputs retain only normal runtime references among independently
  addressable assets.
- A runtime target can load every imported output when its provider module and
  source files are absent.

### Thread ownership and later cancellation

- The initial framework executes every phase synchronously on the editor thread.
  Correctness stages do not pump nested UI events or introduce partial local
  schedulers to simulate responsiveness.
- After Stage 6, source reads, hashing, dependency discovery, parsing, decode,
  normalization, and candidate CPU preparation may run on worker tasks using
  immutable snapshots and provider leases.
- Package lookup that loads assets, object creation requiring reflection,
  imported-state exchange, registry mutation, and package publication always
  run on the editor thread unless their owning contracts explicitly permit
  otherwise.
- Cooperative cancellation is introduced with asynchronous preparation and is
  accepted through candidate validation. Once publication starts it runs to
  completion or rolls back; it is never interrupted between exchange and save.
- Closing a dialog cancels its request and relinquishes UI interest without
  blocking; the coordinator retains and later discards its terminal result.
  Project switch, provider unload, asset-system shutdown, and editor teardown
  close the matching import admission scope, request cancellation, wait without
  holding registry or ownership locks, drain or discard mailbox entries,
  abandon unpublished payloads, and only then release providers or mutable
  editor state.
- Process exit may rely on the process scheduler's close-and-drain ordering only
  after the import coordinator has detached its producers. In-process project
  selection and dynamic provider unload require explicit coordinator barriers;
  global scheduler shutdown is not their substitute.

## Current Foundations and Gaps

### Foundations to retain

- `FSourcePath` and mounted SourceAssets containment provide portable source
  identity and explicit ingestion, replacement, repair, and relocation flows.
- `AssetCore` owns versioned packages, unpublished package discard,
  `SavePackagesAtomically`, registry publication, compatibility reports, and
  DDC storage.
- StaticMesh, Texture2D, and TextureCube already persist source provenance,
  build immediate runtime data, and populate content-addressed DDC objects.
- Existing glTF/GLB and Assimp adapters prove normalized immutable parsing for
  geometry, materials, images, and declared dependencies.
- Existing static-model tests prove valuable collision, candidate, failure,
  reimport, move, orphan, and reload scenarios.
- Runtime assets already expose domain-specific state exchange operations that
  can be narrowed into typed import publication contracts.

### Gaps and implementation debt

- The Assimp-backed `AssetImport` module contains concrete format and Scene
  parsing behind a generic name and remains a dependency of Runtime `Engine`.
  Its provider implementation belongs in the default editor aggregate.
- `FStaticModelImportManifest` makes a StaticMesh the owner of peer material and
  texture outputs and places editor Scene-provider policy in Runtime Engine
  state.
- `FMultiAssetImportTransaction` combines source mutation, texture building,
  DDC cleanup, package save, registry publication, arbitrary mutation
  callbacks, and rollback in one public class.
- Ordinary Content Browser StaticMesh reimport is incorrectly gated by a valid
  multi-output manifest.
- `StaticModel` terminology conflates a source document, one StaticMesh, and a
  heterogeneous output set.
- Generated outputs persist root-mesh ownership instead of generic provenance.
- External-input verification compensates for reopening physical inputs after
  planning instead of executing from one immutable byte snapshot.
- There is no provider-neutral capability query, import-record asset, derived
  output-to-record index, or generic diagnostics/progress model.

## Implementation Stages

### Stage 0: Validate boundaries and freeze replacement evidence

Outcome: the new framework has an implementable package, loaded-identity, and
compatibility boundary before persisted schemas change.

Dependencies: none.

- [x] Inventory current import entry points, persisted source/import fields,
  package publication calls, state-exchange methods, generated ownership, and
  cook filters for StaticMesh, Texture2D, TextureCube, MaterialInstance, and
  the existing Scene workflow.
- [x] Characterize `SavePackagesAtomically`, unpublished package creation,
  registry publication, loaded package cache behavior, and move/delete repair
  with focused tests and targeted code inspection.
- [x] Characterize live-process rollback, interrupted root-last publication,
  stale stage/backup residue, and restart mismatch detection without claiming
  crash-atomic multi-file save.
- [x] Prove or revise prepare/commit/reverse/finalize imported-state exchange,
  including render-resource and notification side effects; record the smallest
  required generic API addition if existing primitives are insufficient.
- [x] Select the package edit revision or authored fingerprint used to reject a
  plan after its target or manager changes between preview and publication.
- [x] Freeze compatibility fixtures for legacy assets with no import record,
  current `FStaticModelImportManifest` packages, generated owner fields, moved
  outputs, and unknown newer import data.
- [x] Preserve existing failure-injection and rendered-import fixtures that
  describe user-visible guarantees rather than obsolete class boundaries.
- [x] Record baseline commit, initial working set, provider/module names,
  persisted schema identifiers, and explicit migration/removal decisions.

#### Acceptance Gate

- A reviewed handoff demonstrates how one loaded asset, one new asset, and one
  mixed multi-package update commit or restore without arbitrary mutation
  callbacks, and every legacy fixture has an explicit load/migrate/retain/
  reject outcome. It also records the selected stale-plan and interrupted-save
  detection contracts.

#### Stage 0 Handoff

- Squashed baseline/result: Multithreading V1 baseline
  `0d2851c95c311c3465e4199d11958b603541b0be`; framework result
  `f24bdca143986e151c6d57ac00aef7d5d8c5e86e`.
- Initial working set: `AssetSystem.h/.cpp`,
  `StaticModelImportBuild.h/.cpp`, and `StaticMesh.h`. Direct validation
  expanded only to `Package.h/.cpp`, the StaticMesh, Texture2D, TextureCube,
  and MaterialInstance exchange/cook implementations, `PackageTests.cpp`,
  `StaticModelImportBuildTests.cpp`, Content Browser reimport call sites, and
  this plan.
- Module and provider names: the framework module is `AssetImportCore`.
  Stage 2 adapters preserve the persisted provider identities `Assimp` for
  ordinary StaticMesh geometry and `DurinImage` for Texture2D/TextureCube.
  The current `AssetImport.StaticModel` identity remains a legacy Scene
  relationship until Stage 4 migrates it; Stage 1 does not rename the existing
  format parser module or persisted identities. Framework test providers use
  `Tests.*` identifiers and never become persisted production data.
- Persisted schema identifiers: existing `FStaticMeshSourceImportData`,
  `FTexture2DSourceImportData`, and `FTextureCubeSourceImportData` are retained
  as the Stage 2 compatibility sources. Static-model manifest schema 1,
  material-mapper schema 1, StaticMesh material-slot schema 1, the current
  importer/decoder versions, and generated `ImportOwner` fields remain readable
  until their owning migration stages. New settings and provider-state payloads
  use independent schema identifiers and versions; a provider contract or DDC
  builder version is not reused as either payload schema.

The frozen inventory and replacement boundary are:

| Surface | Existing contract | Replacement decision |
| --- | --- | --- |
| StaticMesh | `ImportAsset`, source inspection/change/repair, typed source settings, `FStaticModelImportManifest`, `ExchangeImportedState`, and cook filtering | Keep lightweight geometry provenance; Stage 2 exposes it through capabilities. Move the manifest to `DImportRecord` only in Stage 4. |
| Texture2D | `ImportAsset`, `ReimportSource`, source replacement/relocation, `DurinImage` provenance, DDC state, `ImportOwner`, and symmetric no-fail exchange | Stage 2 wraps the existing detached build/exchange and revisioned render-resource rebuild. Stage 4 removes Scene ownership after record migration. |
| TextureCube | Panorama/six-face import and reimport with `DurinImage` provenance and cook filtering, but no detached exchange | Stage 2 adds typed candidate prepare/commit/reverse/finalize before routing reimport through the framework. |
| MaterialInstance | Generated-owner field plus symmetric parameter/parent exchange and render invalidation | Retain as a typed Stage 3 exchange target; remove root-mesh ownership in Stage 4. |
| Scene workflow | `PlanStaticModelImport` is mutation-free, while `FMultiAssetImportTransaction` combines source ingestion, candidates, callbacks, DDC, and package publication | Preserve parser, fixture, naming, reconciliation, failure, and rendered evidence. Do not expose its arbitrary mutation callbacks from `AssetImportCore`. |
| Editor hosts | Content Browser offers StaticMesh-class menu items and calls manifest-gated `PlanStaticModelReimport`; texture dialogs call concrete runtime entry points | Stage 2 queries provider-neutral capabilities. Record navigation is added in Stage 3; Scene workflow migration remains Stage 4. |
| Cooking | StaticMesh strips legacy source fields and manifest; Texture2D/TextureCube strip source provenance, while generated owner fields remain current compatibility data | `DImportRecord` and provider state are editor-only and never enter a cook. Existing owner stripping/removal is completed with Scene migration. |

Publication and loaded-identity decisions:

- `SavePackagesAtomically` serializes and validates every package, rejects
  compatibility-risk packages, stages hidden bytes, backs up prior files,
  publishes the designated root last, and updates registry rows and dirty state
  only after every file is present. Injected staging, dependency publication,
  root publication, and registry failures restore prior files during the live
  process. DDC effects remain disposable and outside the bundle.
- `CreateAsset` supplies a loaded but registry-invisible package;
  `DiscardUnpublishedPackage` is its bounded rollback. A new output therefore
  commits by joining the authored bundle and restores by discard. An existing
  loaded output commits only through a typed prepared exchange and restores by
  the same retained exchange token. A mixed update performs every failable
  preparation first, exchanges all loaded targets, saves existing and new
  packages together, then finalizes or reverses in reverse order.
- Texture2D and MaterialInstance already have no-fail symmetric exchanges.
  Texture2D queues revisioned render work, so stale completions cannot publish.
  StaticMesh currently performs candidate initialization inside
  `ExchangeImportedState` and may fail again while reversing retired render
  data; TextureCube has no exchange. The minimum required Engine addition is a
  typed prepared exchange object per affected class: prepare performs resource
  initialization and retains displaced resources, commit/reverse are no-fail
  and involutive, and finalize releases displaced resources and emits any
  deferred notifications. `AssetImportCore` stores only a registered
  type-erased owner of those typed tokens, never arbitrary apply/rollback
  functions.
- `DPackage::GetEditRevision()` is the selected in-process optimistic token.
  Every `MarkDirty()` advances it even when the package was already dirty;
  `ClearDirty()` does not. Plans also retain path/class/registry revision,
  persisted provenance or record fingerprint, management owner, and disk
  package fingerprint where relevant. This distinguishes an edit after preview
  without serializing every dirty package merely to detect it.
- Root-last publication is live-process failure-atomic, not crash-atomic.
  Existing `.bundle-stage` or `.bundle-backup` residue blocks a later save
  rather than being guessed away. After Stage 3, the root record's persisted
  output fingerprints are authoritative: restart reports an interrupted or
  externally changed output when a published peer does not match the old root.
  No automatic roll-forward or rollback is claimed. An in-process restore
  failure is a fatal invariant diagnostic and closes further import
  publication for the session.
- Asset move/delete already has registry-aware contributors and loaded-package
  cache repair. Stage 3 registers record/output contributors and rebuilds its
  derived manager index after project changes instead of adding generic import
  knowledge to `AssetCore`.

Compatibility outcomes are frozen as follows:

| Fixture | Load outcome | Import-framework outcome |
| --- | --- | --- |
| Legacy StaticMesh/Texture2D/TextureCube with complete source provenance and no record | Retain current typed fields | Offer single-asset reimport through the preserved provider identity; never synthesize a record. |
| Schema-1 `FStaticModelImportManifest` package | Retain and continue to render | Stage 4 migrates an unambiguous relationship to a record; ambiguous ownership requires explicit repair. |
| Generated material/texture owner fields | Retain current owner path | Treat as migration evidence, never as permission for a different record to adopt the output. |
| User-moved output | Keep object/package identity and repaired references | Stage 3 record contributors update its `FAssetPath`; legacy object references remain Stage 4 migration evidence. |
| Unknown newer manifest or generic unknown import field | Preserve serialized evidence; generic compatibility-risk packages remain unsavable without explicit data-loss authority | Reimport/publish rejects before mutation. Manifest validity now requires the supported schema exactly. |
| Missing generated output | Keep the remaining graph loadable | Report missing and require explicit recreate; do not silently adopt a path occupant. |

Validation from the Stage 0 working tree passed the complete
`AssetPackageTests` and `TextureTests` targets, including the new edit-revision
and unknown-newer-manifest cases plus the existing bundle, planning,
failure-injection, loaded-state restore, and compatibility evidence. Existing
rendered static-model fixtures and their Stage 3/4 predecessor handoffs remain
the visible-output baseline; Stage 0 did not rename or regenerate them. Open
questions: none for Stage 1.

### Stage 1: Introduce AssetImportCore and immutable source snapshots

Outcome: providers can recognize and parse bounded source graphs without asset
or package mutation.

Dependencies: Stage 0.

- [x] Add the editor-only `AssetImportCore` module with provider registration,
  capability discovery, stable provider identities, settings capture, source
  descriptors, and structured diagnostics.
- [x] Implement bounded immutable source snapshots for mounted roots, declared
  relative dependencies, and embedded bytes.
- [x] Implement capture-root, bounded provider dependency discovery, canonical
  duplicate/cycle handling, dependency capture, and freeze as distinct phases.
- [x] Ensure paths are resolved once, containment and link targets are checked,
  and exact bytes and hashes are reused through the complete plan.
- [x] Separate external-source ingestion from import execution and route it
  through the existing mounted-source workflow.
- [x] Add generic plan and preview records for output paths, roles, policies,
  estimates, collisions, warnings, target preconditions, provider leases, and
  provider-private immutable data.
- [x] Add tests for changed physical inputs after capture, traversal, missing
  dependencies, duplicate identities, resource budgets, deterministic ordering,
  and provider/module absence.

#### Acceptance Gate

- Repeating a plan from identical captured bytes produces byte-identical
  generic plan data, changing a physical file after capture cannot alter that
  plan or its candidates, and no planning path creates a package, DDC object,
  source file, loaded Engine asset, or worker task.

#### Stage 1 Handoff

- Squashed baseline/result: Multithreading V1 baseline
  `0d2851c95c311c3465e4199d11958b603541b0be`; framework result
  `f24bdca143986e151c6d57ac00aef7d5d8c5e86e`.
- Working set: the new editor-only `AssetImportCore` module and Engine module
  registration, `AssetImportCoreTests.cpp`, its focused test-target wiring, and
  this plan. Runtime Engine and existing concrete import modules were not
  changed.
- Key symbols: `FProviderRegistry`, `FProviderLease`, `IImportProvider`,
  `FSourceSnapshotBuilder`, `FSourceSnapshot`, `FDependencyRequestSink`,
  `FImportPayload`, `FImportPlanBuilder`, `FImportPlan`, `BuildImportPlan`, and
  `CreateImportPlan`.
- Provider decision: registration validates a stable identifier and non-zero
  contract version. Discovery is deterministic by provider ID; zero matches is
  unavailable and multiple matches require explicit selection. Unregistration
  closes registry admission while extant plans retain shared provider leases.
- Snapshot decision: the public request accepts only mounted `FSourcePath`
  identities. External physical selection remains an explicit host ingestion
  action before planning. Root capture, iterative dependency discovery,
  dependency capture, and freeze are distinct operations. Relative paths reject
  traversal, schemes, roots, missing required files, mount-policy violations,
  and link escapes through the existing path resolver. Physical captures are
  shared by canonical path, while logical stable identities and embedded bytes
  remain separate immutable snapshot entries.
- Budget and ordering decision: source count, depth, per-source, aggregate,
  embedded, settings, recognition-prefix, and per-round provider request limits
  are checked before framework copying. Frozen sources and output previews sort
  by provider-stable identity; generic fingerprints include provider/settings,
  source graph hashes, policies, estimates, target preconditions, registry
  revision, and diagnostics but never physical paths or allocation order.
- Mutation boundary: Stage 1 contains no object/package creation, source
  ingestion, DDC call, worker task, or publication API. Provider-private plan
  data is retained only as immutable shared state behind the provider lease.
- Validation: the complete `AssetImportCoreTests` target passed immutable
  post-capture planning, optional/required dependency, traversal,
  duplicate/cycle, embedded and mounted budgets, deterministic ordering,
  mutation-free planning, provider absence/ambiguity, unregister, and lease
  cases. The `AssetImportCore` editor target built successfully, and the
  DurinGame `Engine` target built without enabling or linking the editor module.
  Open questions: none for Stage 2.

### Stage 2: Add single-asset import and reimport

Outcome: ordinary assets import and reimport without a multi-output record.

Dependencies: Stage 1.

- [x] Define the provider-neutral view over per-asset source provenance and
  typed asset-specific settings.
- [x] Add capability queries for import, reimport from current source, reimport
  from a new source, repair source, and unsupported-provider diagnostics.
- [x] Implement one-output detached candidate build, validation, typed state
  prepare/commit/reverse/finalize exchange, package save, and
  restore-on-save-failure.
- [x] Reject a stale single-asset plan when the selected asset, package edit
  revision, source provenance, path occupant, or provider changes after preview.
- [x] Migrate StaticMesh geometry reimport so legacy Teapot-class assets with
  valid source provenance do not require a manifest.
- [x] Migrate Texture2D and TextureCube source reimport without regressing their
  source replacement, relocation, DDC, or render-resource contracts.
- [x] Update asset-editor and Content Browser actions to use capabilities and
  explain exactly which authored state each reimport replaces.

#### Acceptance Gate

- A legacy geometry-only StaticMesh, Texture2D, and TextureCube can each
  reimport from their persisted source with stable asset identity; failure
  preserves prior authored and runtime state; none creates a `DImportRecord` or
  depends on the task system.

#### Stage 2 Handoff

- Squashed baseline/result: Multithreading V1 baseline
  `0d2851c95c311c3465e4199d11958b603541b0be`; framework result
  `f24bdca143986e151c6d57ac00aef7d5d8c5e86e`.
- Initial working set: the Stage 1 `AssetImportCore` API/implementation and
  focused tests, `EngineAssetBuild` module wiring, and the Stage 1 handoff.
  The recorded typed-exchange gap required expanding to the exact StaticMesh,
  Texture2D, TextureCube, AssetImport byte-decoder, Content Browser, and
  TextureEditor call sites listed in the Stage 0 compatibility matrix.
- Key symbols: `FSingleAssetProvenance`, `FSingleAssetCapabilitySet`,
  `ISingleAssetImportHandler`, `ISingleAssetCandidate`,
  `IPreparedImportedStateExchange`, `FSingleAssetImportPlan`,
  `CreateSingleAssetReimportPlan`, `ExecuteSingleAssetImport`,
  `FStaticMeshImportedStateExchange`,
  `DTextureCube::BuildPanoramaFromEncodedBytes`,
  `DTextureCube::BuildFacesFromEncodedBytes`, and
  `RegisterEngineSingleAssetImportHandlers`.
- Capability decision: a selected runtime asset is projected through an
  exact-class handler into common provider, versioned settings, ordered source,
  hash, and output-fingerprint vocabulary. Initial import recognition remains
  the Stage 1 provider query; selected-asset capabilities distinguish current
  mounted-source reimport, mounted source replacement, and repair. Hosts show
  provider diagnostics and the exact provider-declared authored/runtime state
  replacement description rather than class-presence actions.
- Candidate and exchange decision: candidate construction consumes only frozen
  snapshot bytes. Texture2D retains its symmetric exchange; TextureCube now has
  captured-byte builders and symmetric exchange. StaticMesh prepares candidate
  render resources before returning a retained token whose commit and reverse
  only swap authored/runtime state and cannot fail. Geometry-only legacy meshes
  preserve invalid/newer legacy manifest evidence and do not require a valid
  multi-output manifest.
- Publication decision: the process-local editor publication guard covers final
  target path/class/occupant, package edit revision, full provenance, provider
  registry revision, and handler registry revision revalidation. All failable
  candidate and resource preparation occurs before the guard. A package-bundle
  save failure reverses the same retained exchange token, restores the prior
  clean/dirty state, and abandons the unpublished candidate; DDC effects remain
  disposable.
- Validation: the four focused single-asset cases passed Texture2D save-failure
  restore, stale-plan rejection, manifest-free legacy StaticMesh reimport, and
  captured-byte TextureCube panorama reimport. The complete `TextureTests`
  target passed 74 tests, `AssetImportCoreTests` passed all 6 tests,
  `LevelEditor` and `TextureEditor` built, and the full DurinEditor `all` target
  built successfully. Open questions: none for Stage 3.

### Stage 3: Persist generic multi-output import records

Outcome: one source relationship can manage peer outputs without rooting them
under a concrete asset type.

Dependencies: Stage 2.

- [x] Add editor-only `DImportRecord` with independently bounded normalized
  settings and provider-state schemas, output fingerprints, explicit cook
  exclusion, compatibility handling, and deterministic sibling package naming.
- [x] Implement the derived output-to-record index and duplicate-manager
  diagnostics without adding runtime strong dependencies.
- [x] Add move, rename, delete, duplicate, unload, and project-switch
  contributors for record and output paths.
- [x] Implement initial multi-output planning against an absent record and
  reimport reconciliation against a prior record.
- [x] Publish output packages and the record through one live-process
  failure-atomic package bundle, with the record designated as the final root
  package.
- [x] Implement managed, referenced, detached, missing, collision, and orphan
  reconciliation with separate persisted policies, observed states, proposed
  actions, and bounded detached tombstones.
- [x] Revalidate record, target, registry, index, and management preconditions
  under the publication guard; stale plans and duplicate managers publish
  nothing.
- [x] Add round-trip, unknown-provider, unknown-provider-state, moved-output,
  duplicate-record-id, duplicate-manager, stale-plan, interrupted-root-last,
  failed-save, and index-rebuild tests.

#### Acceptance Gate

- A record with heterogeneous peer outputs survives package reload and asset
  moves, can be inspected without loading its output dependency closure, and a
  failed reimport leaves both the prior output packages and prior record
  authoritative. Restart detects rather than silently accepting an output whose
  fingerprint does not match the root record.

#### Stage 3 Handoff

- Squashed baseline/result: Multithreading V1 baseline
  `0d2851c95c311c3465e4199d11958b603541b0be`; framework result
  `f24bdca143986e151c6d57ac00aef7d5d8c5e86e`.
- Working set: the Stage 2 `AssetImportCore` planning/publication API,
  `AssetCore` move contribution boundary, `EngineAssetBuild` candidate
  fingerprints, the focused `AssetImportCoreTests` target, and this plan.
- Key symbols: `DImportRecord`, `FImportRecordState`,
  `FImportRecordIndex`, `FMultiOutputImportPlan`,
  `CreateMultiOutputImportPlan`, `ExecuteMultiOutputImport`,
  `FPreparedMultiOutputImport`, `ComputeImportPackageFingerprint`, and
  `FAssetMoveContribution::AdditionalPackages`.
- Persistence decision: record packages persist independently bounded settings
  and opaque provider state, canonical source/output identities, explicit
  management policies, bounded detached tombstones, and the exact last
  published output-package fingerprints. Paths persist as normalized strings
  and materialize as value-only `FAssetPath` views, so records do not create
  eager output dependencies and remain loadable without provider modules.
- Index and lifecycle decision: record packages remain authoritative while the
  rebuildable value-only index detects duplicate record IDs, duplicate managed
  outputs, missing outputs, and disk fingerprint drift. The output move
  contributor updates and saves the managing record in the same reversible
  move boundary; delete, duplicate, unload, and project-switch invalidation
  entrypoints rebuild or preserve value summaries as appropriate.
- Reconciliation and publication decision: initial and prior-record plans keep
  persisted policy separate from observed state and proposed action across
  managed, referenced, detached, missing, collision, and orphan cases. Final
  preflight shares the Stage 2 publication guard and revalidates the record,
  target revisions, registry, index, provider, and managers. Prepared exchanges
  commit before actual output package fingerprints are calculated; one atomic
  bundle publishes all outputs and the record root last, and any save failure
  reverses outputs and the record to their prior authoritative states.
- Validation: six focused Stage 3 cases passed heterogeneous round-trip,
  provider-independent reload, output move, duplicate ID/manager detection,
  restart fingerprint drift, policy reconciliation, collision, stale-plan,
  root-last interruption, failed reimport restore, and index rebuild coverage.
  The full `AssetImportCoreTests` target passed all 12 tests, the four focused
  Stage 2 single-asset cases passed, and the full DurinEditor `all` target built
  successfully. Open questions: none for Stage 4.

### Stage 4: Replace the existing glTF/FBX Scene workflow

Outcome: existing useful model import behavior becomes a Scene provider in the
default `StandardAssetImport` aggregate rather than a StaticMesh-owned special
case.

Dependencies: Stage 3.

- [x] Rename normalized and workflow APIs away from `StaticModel`; use
  `StaticMesh` for concrete geometry outputs, `Scene` for heterogeneous source
  documents, and generic framework names elsewhere.
- [x] Replace the transitional `EngineAssetBuild` and Assimp-backed
  `AssetImport` module identities with `StandardAssetImport`; migrate consumers
  before retiring either old module, and make the aggregate the direct owner of
  the Assimp link and runtime deployment.
- [x] Implement the glTF/GLB and supported FBX source document provider on
  `AssetImportCore` snapshots and plans.
- [x] Represent StaticMesh, MaterialInstance, and Texture2D as peer outputs in
  one `DImportRecord`; retain a primary output only for navigation.
- [x] Preserve stable mesh slot identities, component material overrides,
  imported material and image reconciliation, deterministic naming, explicit
  collision handling, and non-destructive orphan reporting.
- [x] Migrate current manifest data and generated owner fields into a generic
  record when the relationship is unambiguous; retain a compatibility report
  and require explicit repair when it is not.
- [x] Remove `FStaticModelImportManifest`, root-StaticMesh importer ownership,
  and `FMultiAssetImportTransaction` after every consumer migrates.
- [x] Rehome independently reusable package/source primitives under their
  generic owners; organize material, texture, mesh, and Scene provider policy
  as internal domains of `StandardAssetImport`, splitting a physical provider
  module only for a distinct dependency, deployment, or unload lifecycle.
- [x] Preserve current opaque BaseColor rendering behavior before expanding
  the material subset in a separate material/Scene-provider stage or plan.

#### Acceptance Gate

- Initial import and record reimport produce the same or better validated
  visible output as the prior Stage 4 implementation, outputs are peers under
  one editor-only record, geometry-only reimport remains independent, and no
  runtime asset type contains Scene-provider manifest state. The default
  providers and Assimp dependency are owned by `StandardAssetImport`; neither
  legacy module identity remains a production dependency.

Stage 4 handoff (squashed baseline
`0d2851c95c311c3465e4199d11958b603541b0be`, result
`f24bdca143986e151c6d57ac00aef7d5d8c5e86e`):

- Working set: `StandardAssetImport`, the `AssetImportCore` snapshot and
  multi-output publication boundary, runtime Engine asset exchange/source
  primitives, LevelEditor import and reimport hosts, compatibility handlers,
  and Scene import tests.
- Key symbols and decisions: `SceneImportProviderId` owns glTF/GLB/FBX plans;
  `DImportRecord` owns the peer StaticMesh, MaterialInstance, and Texture2D
  relationship; `StandardAssetImport` directly links and deploys Assimp;
  runtime StaticMesh decoding is registered through
  `FStaticMeshSourceDecodeFunction` without a provider-module dependency.
  Embedded container images receive deterministic content-addressed mounted
  source companions so Texture2D packages remain independently reloadable.
- Compatibility: `MigrateLegacySceneImport` reads the exact retired manifest
  schema through editor-only compatibility types and object-free package
  inspection. It publishes a generic record only for an unambiguous graph;
  missing, conflicting, or newer evidence reports `RepairRequired`. Runtime
  StaticMesh, MaterialInstance, and Texture2D no longer reflect Scene ownership
  state, while their structure handlers retain explicit compatibility reports.
- Validation: all 18 `AssetImportTests`, all 12 `AssetImportCoreTests`, all 58
  `TextureTests`, and the Vulkan Scene reload/render test passed. The Vulkan
  test preserves the opaque BaseColor image and factor golden output. The full
  DurinEditor `all` target built successfully.
- Open questions: none for the Stage 4 acceptance gate. Stage 5 owns shared
  progress, preview, repair UX, diagnostics, and provider-neutral host cleanup.

### Stage 5: Complete editor lifecycle and diagnostics

Outcome: the complete synchronous framework is usable and diagnosable without
provider-specific host logic or task-system dependencies.

Dependencies: Stage 4.

- [x] Add framework progress for snapshot, parse, plan, candidate build,
  validation, publication, and restore phase boundaries.
- [x] Add a shared preview of sources, outputs, management policies, warnings,
  resource estimates, collisions, replacements, missing outputs, and orphans.
- [x] Add import-record inspection and navigation from every managed output,
  plus explicit detach, recreate, repair, and reveal actions.
- [x] Persist accepted diagnostics in records and compare warning identity on
  reimport without storing transient UI state.
- [x] Add provider unregister, plan/candidate lease release, project switch,
  candidate abandonment, stale-plan, and failed-restore invariant tests.

#### Acceptance Gate

- Every synchronous workflow uses the same framework phases, publishes no
  partial authored state after a reported failure, identifies the provider
  phase, source identity, and output for every known error, and submits no
  worker task.

#### Stage 5 Handoff

- Squashed baseline/result: Multithreading V1 baseline
  `0d2851c95c311c3465e4199d11958b603541b0be`; framework result
  `f24bdca143986e151c6d57ac00aef7d5d8c5e86e`.
- Working set: `AssetImportCore` progress, preview, diagnostic, record-index,
  provider-lease, multi-output publication, and generic record-action APIs;
  `StandardAssetImport` Scene estimates and record handler; LevelEditor preview
  and Content Browser record actions; focused framework and Scene tests.
- Key symbols and decisions: `IImportProgressReporter` reports synchronous
  snapshot, parse, plan, candidate-build, validation, publication, and restore
  boundaries without submitting worker work. `FImportPreview` is the common
  source/output/policy/warning/estimate view. Stable diagnostic identities and
  accepted warnings persist in import-record schema version 2; version 1 loads
  with an empty diagnostic history. `IImportRecordHandler` keeps reimport,
  recreate, and repair hosts provider-neutral, while record inspection owns
  reveal and detach behavior.
- Lifecycle and failure behavior: prepared candidates and execution results
  retain provider leases; unregister closes new admission while existing leases
  drain; abandoned prepared candidates are disposed through RAII. Failed
  publication reverses output and record exchanges, verifies restored authored
  fingerprints, and emits a restore diagnostic if that invariant fails.
- Validation: all 18 `AssetImportTests`, all 16 `AssetImportCoreTests`, all 59
  `TextureTests`, and the Vulkan Scene reload/render test passed. The full
  DurinEditor `all` target built successfully.
- Open questions: none for the Stage 5 acceptance gate. Multithreading V1 is now
  available; Stage 6 owns the import-specific coordinator, mailbox, and teardown
  integration described below.

### Stage 6: Add asynchronous preparation without semantic changes

Outcome: large imports become responsive by scheduling only immutable
preparation while preserving the synchronous framework as the reference path.

Dependencies: Stage 5 and Multithreading V1 commit
`0d2851c95c311c3465e4199d11958b603541b0be` (satisfied).

- [ ] Add one framework-owned asynchronous coordinator and adapter for source
  capture, hashing, dependency discovery, parsing, normalization, and candidate
  CPU preparation. The coordinator owns request serials, task handles,
  cancellation sources, immutable inputs, and local admission; do not add
  asynchronous methods to providers or launch one task per provider phase.
- [ ] Define a move-owned preparation result that contains no `DObject`, package,
  registry, editor-model, render-resource, or RHI reference. Keep reflection
  object creation, package lookup, typed exchange, registry mutation, and
  publication on the editor thread.
- [ ] Deliver results through an `AssetImportCore` synchronized mailbox drained
  by an existing editor-thread tick. Consume an entry only when its request
  serial is current and its task handle is terminal; synthesize stable failure
  or cancellation diagnostics when a terminal task produced no entry, and
  treat an invalid launch handle as work that was never accepted.
- [ ] Retain provider leases and immutable inputs until each accepted task and
  mailbox entry reaches one terminal disposition. Before a worker callable
  returns, move its provider lease into the mailbox entry or release it so
  callable-wrapper destruction cannot execute provider cleanup after a terminal
  wait. Add a latch-controlled provider-unload race test for this boundary.
- [ ] Add cooperative cancellation checks at bounded intervals during capture,
  parse, normalization, and CPU preparation, then check again between
  editor-thread candidate creation and validation steps. Publication remains
  guarded, revalidated, non-interruptible, and failure-atomic once it starts.
- [ ] Add scoped `CancelAndDrain` barriers for request/dialog, project, provider,
  and process ownership. Project selection invokes the project barrier before
  replacing mounts or editor state; provider shutdown drains its requests and
  mailbox entries before unregistering handlers/providers; process shutdown
  detaches import producers before the global scheduler drain and Asset Manager
  shutdown.
- [ ] Run synchronous-versus-asynchronous equivalence tests for plans,
  candidates, diagnostics, DDC keys, authored bytes, failures, and stale-plan
  rejection.
- [ ] Add repeated loaded-project shutdown qualification with accepted and
  canceled import work, retained terminal handles, provider unload, and render
  activity. Require at least 20 consecutive clean Sandbox shutdowns because the
  Stage 6 baseline audit observed one non-reproduced access violation after a
  successful scheduler audit; project-browser and immediate Sandbox reruns
  completed cleanly.

#### Acceptance Gate

- Large preparation remains responsive and cancelable; dialog close, project
  switch, provider unload, and process shutdown leave no live callable,
  provider lease, mailbox entry, or unpublished payload. The same inputs
  produce equivalent synchronous and asynchronous outcomes before the common
  editor-thread publication path, and repeated loaded-project shutdown passes
  without a crash or late callback.

### Stage 7: Close compatibility, cooking, and architecture handoff

Outcome: the new framework is the only supported import orchestration path and
does not burden runtime targets.

Dependencies: Stage 6.

- [ ] Remove obsolete workflow files, public APIs, serialized fields, menu
  actions, compatibility accessors, and model-specific transaction tests only
  after their replacements pass.
- [ ] Prove cooked outputs contain no import records, source paths, provider
  state, editor diagnostics, or provider module dependencies.
- [ ] Inspect runtime target dependency graphs and deployment output for
  `AssetImportCore`, `StandardAssetImport`, optional independent providers,
  Assimp, and editor image decoders.
- [ ] Run provider-focused suites, complete Engine tests, the repository full
  `all` build, editor import/reimport smoke workflows, DDC cold/warm cases, and
  cooked runtime smoke coverage.
- [ ] Publish lasting package, provenance, import-record, provider, reimport,
  DDC, cooking, and editor workflow contracts under their owning Runtime and
  Editor Architecture documentation.
- [ ] Update other active plans only for dependencies whose implementation and
  acceptance evidence actually landed.
- [ ] Complete and archive this plan after all required gates and documentation
  handoffs pass.

#### Acceptance Gate

- Existing supported authored assets migrate or remain usable, runtime-only
  builds load all imported outputs without editor/provider code, full validation
  passes, and no lasting framework contract remains owned only by this plan.

## Validation Matrix

| Area | Required validation |
| --- | --- |
| Provider discovery | unique IDs, bounded prefix recognition, ambiguity, explicit selection, unavailable persisted provider, contract and schema compatibility |
| Source snapshots | root and recursive declared dependencies, embedded bytes, containment, traversal, link escape, duplicate/cycle/depth/size/count budgets, post-capture changes |
| Planning | mutation-free output preview, canonical ordering, stable identities, target preconditions, stale revision, collision and estimate completeness |
| Single-asset reimport | legacy StaticMesh, Texture2D, TextureCube, changed/missing source, package-save failure, runtime-state restore |
| Import records | settings and provider-state round trip/bounds/version, unknown provider, peer outputs, move/rename/delete, duplicate ID and manager |
| Reconciliation | unchanged, reorder, rename, add/remove, moved/missing output, detach tombstone, recreate, orphan, unrelated collision |
| Publication | new/existing mixed packages, stale-plan rejection, root-last save, registry failure, no-fail reverse exchange, interrupted-save mismatch detection, restart after success |
| DDC | cold/warm/corrupt/missing cache, per-output invalidation, harmless failed-attempt residue, no persisted cache paths |
| Synchronous execution | no task submission, phase ordering, deterministic results, provider lease, diagnostics, navigation, repair |
| Asynchronous execution | sync equivalence, progress, rejected launch, cancel, stale serial, terminal-without-result, dialog close, project switch, provider unload, callable-cleanup race, mailbox drain, task failure, repeated loaded-project shutdown |
| Cooking/runtime | record and provenance stripping, no provider dependencies, dependency closure, cooked load and rendered smoke |
| Compatibility | no record, current static-model manifest, generated owner fields, unknown newer data, explicit data-loss refusal |

## Definition of Done

- A provider can import one asset or a peer set of heterogeneous assets through
  the same source snapshot, plan, candidate, diagnostics, and publication model.
- Ordinary asset reimport requires no multi-output record.
- Multi-output relationships persist in editor-only `DImportRecord` packages
  and never use one output as the owner of its peers.
- Every plan and candidate consumes one immutable bounded source snapshot.
- Persisted normalized settings are sufficient to recreate reimport after
  restart; settings, provider state, provider contracts, and builders have
  independent compatibility and invalidation versions.
- Stale plans are rejected under the publication guard before mutation.
- Authored package publication is live-process failure-atomic, interrupted
  root-last mismatches are detectable after restart, and source ingestion and
  DDC effects have explicit independent lifetime rules.
- Existing loaded identities update only through typed validated no-fail state
  commit/reverse exchange after all failable preparation, not arbitrary public
  mutation callbacks.
- The synchronous executor remains the semantic reference; asynchronous
  preparation has equivalent results and never moves object or publication
  ownership off the editor thread.
- The glTF/FBX workflow uses Scene terminology and generic records; obsolete
  `StaticModel` workflow types and runtime manifest fields are gone.
- Runtime asset references, DDC keys, cooked payloads, and rendering remain
  independent of import records and provider modules.
- Compatibility, focused tests, full build, editor smoke, and cooked runtime
  validation pass, and lasting contracts live outside the active plan.

## Deferred Follow-ups

- A runtime scene, prefab, model, or hierarchy asset when gameplay and editor
  workflows require one independently of import provenance.
- Soft asset references and registry-level searchable property metadata.
- Field-level user/importer merge policies beyond typed provider state
  boundaries.
- Automatic orphan deletion after reference-safety, undo, recovery, and source
  control policy exist.
- Distributed import workers, remote DDC, import farms, and source-control
  checkout orchestration.
- Export and round-trip authoring pipelines.
- Additional providers such as skeletal scenes, animation, audio sessions,
  fonts, atlases, CAD, USD, and project-specific pipelines.

## Related Documentation

- [Asset Packages](../Runtime/Assets/AssetPackages.md)
- [Asset Data Lifecycle and Storage](../Runtime/Assets/AssetDataLifecycle.md)
- [Mounted Source Workflows](../Editor/Guides/MountedSourceWorkflows.md)
- [Material System](../Runtime/Rendering/MaterialSystem.md)
- [Texture System](../Runtime/Rendering/TextureSystem.md)
- [Multithreading V1 Plan](MultithreadingV1.md)
- [Archived Ready-to-Use Static Model Import](Archive/2026-08/ReadyToUseStaticModelImport.md)
- [Archived Source Library References](Archive/2026-07/SourceLibraryReferences.md)

## Related Code

- `Engine/Source/Runtime/AssetCore/Public/AssetSystem.h`
- `Engine/Source/Runtime/AssetCore/Private/AssetSystem.cpp`
- `Engine/Source/Editor/AssetImportCore/`
- `Engine/Source/Editor/StandardAssetImport/`
- `Engine/Source/Editor/LevelEditor/Private/Assets/`
- `Engine/Source/Editor/LevelEditor/Private/Panels/ContentBrowserPanel.cpp`
- `Engine/Source/Runtime/Engine/Public/StaticMesh/StaticMesh.h`
- `Engine/Source/Runtime/Engine/Public/Texture/Texture2D.h`
- `Engine/Source/Runtime/Engine/Public/Texture/TextureCube.h`
- `Engine/Source/Runtime/Engine/Public/Materials/MaterialInstance.h`
- `Engine/Tests/Native/AssetCoreTests/`
- `Engine/Tests/Native/EngineTests/Private/Texture/SceneImportTests.cpp`
