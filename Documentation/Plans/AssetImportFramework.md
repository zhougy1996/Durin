# Asset Import Framework Plan

Summary: Replace asset-specific import orchestration with a provider-based editor framework for immutable source snapshots, single-asset reimport, multi-output import records, detached candidates, and atomic package publication.

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
objects, unpublished packages, atomic package-bundle save, registry
publication, and asset-specific candidate exchange. The missing architecture
is a small editor framework that composes those primitives without treating
one output asset as the owner of its peers or making simple reimport depend on
a multi-output graph.

No implementation stage has started. Stage 0 must validate the selected
package-candidate and loaded-object commit boundary against the existing
`AssetCore` APIs before repository types or persisted data are renamed.

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
- Publish authored package changes atomically while treating DDC objects as
  disposable cache effects and source ingestion as a separate authoring action.
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
- Atomic authored-package publication through `AssetCore` primitives.
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

## Terminology

- **Source**: an authoritative file identified by `FSourcePath` in a mounted
  `SourceAssets` domain.
- **Source snapshot**: immutable bytes plus canonical identity and hash captured
  for one plan. Later phases never reopen a snapshot source for content.
- **Provider**: an editor module implementation that recognizes sources,
  parses them, plans outputs, builds candidates, and supplies provider-specific
  reconciliation.
- **Output**: an independently addressable `.dasset` produced or updated by an
  import. Outputs are peers even when ordinary runtime references connect them.
- **Import record**: editor-only provenance and reconciliation data for one
  multi-output import relationship. It records management, not structural
  containment or runtime ownership.
- **Candidate**: detached authored state that is fully validated before an
  existing asset identity changes.
- **Publication**: the bounded editor-thread operation that exchanges prepared
  state and atomically saves all changed packages.
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
Concrete provider modules (Editor)
  TextureImport, StaticMeshImport, SceneImport, future domain importers
          |
Editor hosts
  Content Browser, asset editors, dialogs, progress and diagnostics
```

- `AssetCore` remains unaware of import providers, materials, textures, meshes,
  source formats, or import records. Only independently useful package
  publication primitives may move into it.
- `AssetImportCore` depends on generic asset and source identities but not on
  concrete Engine asset classes.
- A provider owns parsing, normalized data, settings, output roles, imported
  state, and reconciliation rules for its domain.
- Editor hosts request capabilities from the framework. They do not show a
  reimport action merely because an object has a particular concrete class.
- Runtime asset types retain only runtime state and the minimum lightweight
  provenance required for their own single-asset rebuild. A runtime type never
  contains a SceneImport-specific manifest.

### Two deliberately different reimport modes

**Single-asset reimport** requires only:

- a current asset identity;
- complete lightweight source provenance;
- valid provider identity, version, and settings;
- available declared source inputs.

It produces one candidate and atomically saves one authored package. StaticMesh
geometry reimport follows this path and must work for compatible legacy assets
without a `DImportRecord`.

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
- Planning reads every required file exactly once into a bounded immutable
  source snapshot. The same bytes feed parsing, hashes, candidate builds, DDC
  keys, and persisted source records.
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
ProviderVersion
SettingsVersion and normalized settings
ordered source identities, FSourcePath values, roles, hashes, and sizes
last successful authored-output fingerprint
```

Concrete assets may store typed settings, but source identity and provider
identity follow one common contract. Provenance contains no DDC path, physical
workstation path, timestamp identity, output graph, or UI session state.

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
ProviderVersion
SettingsVersion
SettingsFingerprint
Sources[]
  StableIdentity, Role, FSourcePath, ContentHash, ByteCount
Outputs[]
  StableIdentity, Role, FAssetPath, ManagementPolicy
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
- Provider state is an opaque, bounded, versioned payload so the generic record
  remains loadable and preservable while a provider module is unavailable.
  Only the matching provider decodes it.
- The generic record never contains `DStaticMesh`, `DMaterialInstance`,
  `DTexture2D`, slot, sampler, skeleton, or scene-node fields.
- A record may designate a primary output for navigation, but primary does not
  imply ownership and is not required.
- Records are authored editor packages, never DDC entries or JSON sidecars.
  Default cooks exclude their packages and all provider state.

### Derived import-record index

- `AssetImportCore` builds a rebuildable editor index from registry-visible
  `DImportRecord` packages and their bounded generic summaries.
- The index maps each managed output path to its record and output identity,
  detects duplicate managers, and provides Content Browser relationships.
- The record packages remain authoritative. The index may live under `Saved/`
  or the DDC and may be deleted without data loss.
- Version one may load the small population of import records during index
  construction. A future searchable-property/header-summary contract may
  remove that cost; this plan does not add a general asset query system solely
  for import records.

### Provider contract

A provider has stable identity and implements bounded phases equivalent to:

```text
CanImport(source descriptors)
CaptureSettings()
BuildSourceSnapshot(request)
Plan(snapshot, previous record or target asset)
BuildCandidates(plan)
ValidateCandidates(candidates)
DescribeImportedStateExchange(target, candidate)
EncodeProviderState(result)
```

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
  candidate classes. It must be validated before publication, no-fail once
  entered, and involutive so reverse-order exchange restores the old state.
- Arbitrary `std::function` apply/rollback callbacks are not part of the public
  publication API.

### Publication and failure model

Publication deliberately protects authored packages, not every cache side
effect:

1. capture and validate immutable sources;
2. build detached candidates and any required in-memory runtime data;
3. populate disposable DDC objects as needed for immediate editor use;
4. resolve every target package and collision before mutation;
5. on the editor thread, exchange prepared imported state into existing
   identities through typed no-fail operations;
6. call `Asset::SavePackagesAtomically` for all changed output packages and the
   updated `DImportRecord`, designating the record as the root published last;
7. on save failure, reverse the state exchanges and report both the primary
   failure and any invariant violation;
8. on success, discard candidates and publish the new derived index revision.

- New assets remain unpublished packages until the bundle commit succeeds.
- DDC objects created by a failed attempt may remain; they are content-addressed
  disposable cache entries and do not require rollback.
- Source ingestion is already complete before this lifecycle and is not rolled
  back with asset packages.
- Orphans are reports, not deletions. Explicit detach or delete is a separate
  impact-checked operation.
- Package save remains the durable atomic boundary. The framework does not
  claim ACID semantics across arbitrary filesystems and loaded object code.
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

### Concurrency and cancellation

- Source reads, hashing, parsing, decode, normalization, and candidate CPU
  builds may run on worker tasks using immutable snapshots.
- Package lookup that loads assets, object creation requiring reflection,
  imported-state exchange, registry mutation, and package publication run on
  the editor thread unless their owning contracts explicitly permit otherwise.
- Cancellation is accepted through candidate validation. Once publication
  starts it runs to completion or rolls back; it is not interrupted between
  state exchange and package save.
- Closing a dialog, project, or editor safely abandons unpublished candidates
  and joins or rejects outstanding results according to the Multithreading V1
  lifecycle contract.

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

- `FStaticModelImportManifest` makes a StaticMesh the owner of peer material and
  texture outputs and places editor SceneImport policy in Runtime Engine state.
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

- [ ] Inventory current import entry points, persisted source/import fields,
  package publication calls, state-exchange methods, generated ownership, and
  cook filters for StaticMesh, Texture2D, TextureCube, MaterialInstance, and
  the existing Scene workflow.
- [ ] Characterize `SavePackagesAtomically`, unpublished package creation,
  registry publication, loaded package cache behavior, and move/delete repair
  with focused tests and targeted code inspection.
- [ ] Prove or revise the selected typed no-fail candidate exchange contract;
  record the smallest required `AssetCore` addition if existing primitives are
  insufficient.
- [ ] Freeze compatibility fixtures for legacy assets with no import record,
  current `FStaticModelImportManifest` packages, generated owner fields, moved
  outputs, and unknown newer import data.
- [ ] Preserve existing failure-injection and rendered-import fixtures that
  describe user-visible guarantees rather than obsolete class boundaries.
- [ ] Record baseline commit, initial working set, provider/module names,
  persisted schema identifiers, and explicit migration/removal decisions.

#### Acceptance Gate

- A reviewed handoff demonstrates how one loaded asset, one new asset, and one
  mixed multi-package update commit or restore without arbitrary mutation
  callbacks, and every legacy fixture has an explicit load/migrate/retain/
  reject outcome.

### Stage 1: Introduce AssetImportCore and immutable source snapshots

Outcome: providers can recognize and parse bounded source graphs without asset
or package mutation.

- [ ] Add the editor-only `AssetImportCore` module with provider registration,
  capability discovery, stable provider identities, settings capture, source
  descriptors, and structured diagnostics.
- [ ] Implement bounded immutable source snapshots for mounted roots, declared
  relative dependencies, and embedded bytes.
- [ ] Ensure paths are resolved once, containment and link targets are checked,
  and exact bytes and hashes are reused through the complete plan.
- [ ] Separate external-source ingestion from import execution and route it
  through the existing mounted-source workflow.
- [ ] Add generic plan and preview records for output paths, roles, policies,
  estimates, collisions, warnings, and provider-private immutable data.
- [ ] Add tests for changed physical inputs after capture, traversal, missing
  dependencies, duplicate identities, resource budgets, deterministic ordering,
  and provider/module absence.

#### Acceptance Gate

- Repeating a plan from identical captured bytes produces byte-identical
  generic plan data, changing a physical file after capture cannot alter that
  plan or its candidates, and no planning path creates a package, DDC object,
  source file, or loaded Engine asset.

### Stage 2: Add single-asset import and reimport

Outcome: ordinary assets import and reimport without a multi-output record.

- [ ] Define the provider-neutral view over per-asset source provenance and
  typed asset-specific settings.
- [ ] Add capability queries for import, reimport from current source, reimport
  from a new source, repair source, and unsupported-provider diagnostics.
- [ ] Implement one-output detached candidate build, validation, typed state
  exchange, atomic package save, and restore-on-save-failure.
- [ ] Migrate StaticMesh geometry reimport so legacy Teapot-class assets with
  valid source provenance do not require a manifest.
- [ ] Migrate Texture2D and TextureCube source reimport without regressing their
  source replacement, relocation, DDC, or render-resource contracts.
- [ ] Update asset-editor and Content Browser actions to use capabilities and
  explain exactly which authored state each reimport replaces.

#### Acceptance Gate

- A legacy geometry-only StaticMesh, Texture2D, and TextureCube can each
  reimport from their persisted source with stable asset identity; failure
  preserves prior authored and runtime state; none creates a `DImportRecord`.

### Stage 3: Persist generic multi-output import records

Outcome: one source relationship can manage peer outputs without rooting them
under a concrete asset type.

- [ ] Add editor-only `DImportRecord` with the generic schema, bounded provider
  state, explicit cook exclusion, compatibility handling, and deterministic
  sibling package naming.
- [ ] Implement the derived output-to-record index and duplicate-manager
  diagnostics without adding runtime strong dependencies.
- [ ] Add move, rename, delete, duplicate, unload, and project-switch
  contributors for record and output paths.
- [ ] Implement initial multi-output planning against an absent record and
  reimport reconciliation against a prior record.
- [ ] Publish output packages and the record through one atomic package bundle,
  with the record designated as the final root package.
- [ ] Implement managed, referenced, detached, missing, collision, and orphan
  policies with explicit user-visible actions.
- [ ] Add round-trip, unknown-provider, unknown-provider-state, moved-output,
  duplicate-manager, failed-save, and index-rebuild tests.

#### Acceptance Gate

- A record with heterogeneous peer outputs survives package reload and asset
  moves, can be inspected without loading its output dependency closure, and a
  failed reimport leaves both the prior output packages and prior record
  authoritative.

### Stage 4: Replace the existing glTF/FBX Scene workflow

Outcome: existing useful model import behavior becomes a Scene provider rather
than a StaticMesh-owned special case.

- [ ] Rename normalized and workflow APIs away from `StaticModel`; use
  `StaticMesh` for concrete geometry outputs, `Scene` for heterogeneous source
  documents, and generic framework names elsewhere.
- [ ] Implement the glTF/GLB and supported FBX source document provider on
  `AssetImportCore` snapshots and plans.
- [ ] Represent StaticMesh, MaterialInstance, and Texture2D as peer outputs in
  one `DImportRecord`; retain a primary output only for navigation.
- [ ] Preserve stable mesh slot identities, component material overrides,
  imported material and image reconciliation, deterministic naming, explicit
  collision handling, and non-destructive orphan reporting.
- [ ] Migrate current manifest data and generated owner fields into a generic
  record when the relationship is unambiguous; retain a compatibility report
  and require explicit repair when it is not.
- [ ] Remove `FStaticModelImportManifest`, root-StaticMesh importer ownership,
  and `FMultiAssetImportTransaction` after every consumer migrates.
- [ ] Rehome reusable package/source primitives and keep material, texture,
  mesh, and Scene mapping policy in their concrete provider modules.
- [ ] Preserve current opaque BaseColor rendering behavior before expanding
  the material subset in a separate material/Scene-provider stage or plan.

#### Acceptance Gate

- Initial import and record reimport produce the same or better validated
  visible output as the prior Stage 4 implementation, outputs are peers under
  one editor-only record, geometry-only reimport remains independent, and no
  runtime asset type contains Scene-provider manifest state.

### Stage 5: Complete editor lifecycle and diagnostics

Outcome: the framework is usable for large imports and failures without
provider-specific host logic.

- [ ] Add framework progress for snapshot, parse, plan, candidate build,
  validation, publication, and restore phases.
- [ ] Add cancellation and safe result abandonment before publication and
  non-interruptible completion once publication begins.
- [ ] Add a shared preview of sources, outputs, management policies, warnings,
  resource estimates, collisions, replacements, missing outputs, and orphans.
- [ ] Add import-record inspection and navigation from every managed output,
  plus explicit detach, recreate, repair, and reveal actions.
- [ ] Persist accepted diagnostics in records and compare warning identity on
  reimport without storing transient UI state.
- [ ] Add project close, editor shutdown, cancellation, provider unload,
  candidate abandonment, and failed-restore invariant tests.

#### Acceptance Gate

- A large import remains responsive through planning and candidate building,
  cancellation publishes nothing, publication cannot be interrupted into a
  partial authored state, and every error identifies its provider phase,
  source identity, and output when known.

### Stage 6: Close compatibility, cooking, and architecture handoff

Outcome: the new framework is the only supported import orchestration path and
does not burden runtime targets.

- [ ] Remove obsolete workflow files, public APIs, serialized fields, menu
  actions, compatibility accessors, and model-specific transaction tests only
  after their replacements pass.
- [ ] Prove cooked outputs contain no import records, source paths, provider
  state, editor diagnostics, or provider module dependencies.
- [ ] Inspect runtime target dependency graphs and deployment output for
  `AssetImportCore`, Scene providers, Assimp, and editor image decoders.
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
| Provider discovery | unique IDs, unsupported formats, unavailable providers, version mismatch, deterministic selection |
| Source snapshots | root and sidecars, embedded bytes, containment, traversal, link escape, size/count budgets, post-capture changes |
| Planning | mutation-free output preview, canonical ordering, stable identities, collision and estimate completeness |
| Single-asset reimport | legacy StaticMesh, Texture2D, TextureCube, changed/missing source, package-save failure, runtime-state restore |
| Import records | round trip, provider payload bounds/version, unknown provider, peer outputs, move/rename/delete, duplicate manager |
| Reconciliation | unchanged, reorder, rename, add/remove, moved/missing output, detach, recreate, orphan, unrelated collision |
| Publication | new/existing mixed packages, root-last save, registry failure, loaded-state reverse exchange, process restart after success |
| DDC | cold/warm/corrupt/missing cache, per-output invalidation, harmless failed-attempt residue, no persisted cache paths |
| Editor lifecycle | preview, progress, cancel, close, provider unload, diagnostics, navigation, repair |
| Cooking/runtime | record and provenance stripping, no provider dependencies, dependency closure, cooked load and rendered smoke |
| Compatibility | no record, current static-model manifest, generated owner fields, unknown newer data, explicit data-loss refusal |

## Definition of Done

- A provider can import one asset or a peer set of heterogeneous assets through
  the same source snapshot, plan, candidate, diagnostics, and publication model.
- Ordinary asset reimport requires no multi-output record.
- Multi-output relationships persist in editor-only `DImportRecord` packages
  and never use one output as the owner of its peers.
- Every plan and candidate consumes one immutable bounded source snapshot.
- Authored package publication is atomic; source ingestion and DDC effects have
  explicit independent lifetime rules.
- Existing loaded identities update only through typed validated no-fail state
  exchange, not arbitrary public mutation callbacks.
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
- `Engine/Source/Editor/AssetImport/`
- `Engine/Source/Editor/EngineAssetBuild/`
- `Engine/Source/Editor/LevelEditor/Private/Assets/`
- `Engine/Source/Editor/LevelEditor/Private/Panels/ContentBrowserPanel.cpp`
- `Engine/Source/Runtime/Engine/Public/StaticMesh/StaticMesh.h`
- `Engine/Source/Runtime/Engine/Public/Texture/Texture2D.h`
- `Engine/Source/Runtime/Engine/Public/Texture/TextureCube.h`
- `Engine/Source/Runtime/Engine/Public/Materials/MaterialInstance.h`
- `Engine/Tests/Native/AssetCoreTests/`
- `Engine/Tests/Native/EngineTests/Private/Texture/StaticModelImportBuildTests.cpp`
