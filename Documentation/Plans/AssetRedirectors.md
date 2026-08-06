# Asset Redirectors Refactor Plan

Summary: Replace eager move-time reference rewriting with first-class asset redirectors, transparent path resolution, atomic batch relocation, and explicit fix-up workflows.

Last reviewed: 2026-08-06

Status: Active
Completed:

## Current Status

- Stage 0 is complete: the redirect wire format, public vocabulary, resolution
  behavior, mutation protocol, compatibility policy, and fixture catalog are
  frozen below. Production implementation has not started; Stage 1 is next.
- The completed [Soft Asset References Plan](SoftAssetReferences.md) is historical
  evidence for the current implementation only. Its move-time rewrite contract,
  compatibility promises, stage structure, and validation baseline do not
  constrain this refactor.
- Current target moves require a complete soft-reference index, load and save
  hard referencers, rewrite loaded and unloaded soft referencers, update
  registered external stores, relocate companion files, and restore every
  participant from backups on failure. This plan deliberately removes those
  reference repairs from the move critical path.
- The selected model follows the Unreal Engine asset-redirector principle: a
  successful move publishes the real asset at its new path and a redirector at
  its old path; reference rewriting and redirector deletion happen later through
  an explicit Fix Up operation.
- This plan strengthens that model with direct-to-final redirectors, automatic
  chain compression during later moves, immutable batch plans, and atomic
  multi-file publication.

## Goal

Make asset relocation independent of the number, residency, writability, or
indexability of its referencers. After a move, every previously valid hard or
soft asset reference must continue resolving through an authored redirector at
the old path, while the stored reference remains unchanged until an explicit
Fix Up operation rewrites it to the final asset path.

The final system must provide:

- one first-class redirector asset and one authoritative redirect-resolution
  path from registry lookup through typed object loading;
- one exact registry view that still exposes redirectors to mutation tools and
  the Content Browser;
- one atomic batch-relocation transaction for assets and folders;
- one explicit, verifiable Fix Up transaction that rewrites package and
  external references before deleting redirectors;
- deterministic redirect behavior across loading, deletion, cooking,
  Undo/Redo, editor restart, registry-cache rebuild, and repeated moves; and
- complete removal of the current move-time hard/soft reference rewrite and
  external-store transaction architecture.

## Scope

- Add an AssetCore-owned `DAssetRedirector` main-asset type whose destination
  is one non-null hard reference to another package main asset.
- Publish redirector kind and destination as bounded, authoritative asset
  registry metadata without constructing package objects during discovery.
- Distinguish exact path lookup from redirect-following resolution throughout
  the registry, asset manager, mutation tools, Content Browser, and tests.
- Add bounded redirect-chain resolution, reverse redirector lookup, cycle
  detection, missing-target diagnostics, path compression, and final-target
  type validation.
- Extend `FSoftObjectPtr` cache state so an authored old path can cache an
  object loaded from a different resolved package path without changing logical
  identity or serialized bytes.
- Replace single-asset `MoveAsset` orchestration with immutable-plan atomic
  batch relocation that creates redirectors, relocates asset-owned companion
  files, and publishes one registry revision.
- Generalize the current soft-reference extractor and tagged package rewriter
  into a unified authored package reference index/codec for hard, soft, and
  redirect edges.
- Add strict Fix Up planning, preflight, publication, verification, and
  redirector deletion for package references and registered external stores.
- Split asset-owned payload relocation, external persistent-reference repair,
  and transient move observation into separate contracts.
- Integrate redirectors with Content Browser filtering and commands, global
  editor Undo/Redo, deletion analysis, import records, project default-level
  settings, thumbnails, document/session state, cooking, and cooked output.
- Remove obsolete implementation, cache schema, public APIs, tests, and
  documentation after all production callers use the replacement contracts.

## Non-Goals

- Adding Core Redirects for reflected class, struct, enum, function, property,
  or native package-name migration. Code/schema redirects remain a separate
  future system.
- Preserving the current requirement that a successful move immediately
  rewrites all hard references, soft references, import records, or project
  settings to the new path.
- Preserving current `MoveAsset`, `FAssetMoveExternalStore`, move-contributor
  additional-package, `.movebak`, or sequential editor rollback APIs for source
  compatibility.
- Treating every `FAssetPath` or string as an asset reference. Source paths,
  physical files, document identifiers, cache keys, and operation destinations
  retain domain-specific semantics.
- Allowing normal callers to mutate a redirector destination, move a redirector
  as an ordinary asset, or overwrite an unrelated redirector collision.
- Automatically modifying the authored path stored by `FSoftObjectPath` or
  `TSoftObjectPtr` merely because resolution followed a redirector.
- Adding asynchronous loading, streamable handles, priorities, bundles,
  cancellation, remote assets, subobject paths, or network-replicated asset
  references.
- Building a complete source-control provider. Existing authoring-write policy
  remains authoritative; Fix Up reports unavailable or read-only inputs before
  publication.
- Implementing asset consolidation or replace-references UI beyond the
  redirect and fix-up primitives required by this refactor.

## Design Decisions and Invariants

### Redirector Ownership and Persistence

- AssetCore owns `DAssetRedirector`, redirect metadata, resolution, reverse
  indexing, mutation planning, Fix Up, and cook integration. CoreDObject remains
  unaware of registry and filesystem services.
- A redirector is a normal `.dasset` package whose main asset is
  `DAssetRedirector`. It contains exactly one non-null
  `TObjectPtr<DObject> DestinationObject` referencing a package main asset.
- The hard destination makes redirector lifetime, deletion blocking, and cook
  reachability explicit. A redirector cannot target itself, a transient object,
  an inner object, a package object, or another object in its own package.
- Redirector kind and destination are published in bounded package-header
  registry metadata generated from the canonical body value during save.
  Header/body disagreement, an absent destination, multiple destinations, or
  a destination not represented by the hard dependency table is corruption.
- Stage 0 assigns an unused authored package-envelope version for this explicit
  metadata. Inferring redirect targets from dependency ordering or scanning
  complete object payloads during normal registry discovery is rejected.
- Redirectors are authored editor content, hidden by default but never virtual
  registry-only aliases. Their files participate in fingerprints, snapshots,
  source-control policy, staging, Undo/Redo, and restart discovery.

### Exact Identity and Redirected Resolution

- `FAssetPath` remains a canonical syntactic identity and never resolves itself.
- `FAssetRegistry::FindAssetExact(Path)` returns only the entry physically
  registered at `Path`, including a redirector. Mutation, collision, deletion,
  Content Browser, and registry inspection code use this API.
- `ResolveAssetPath(Path, Options)` follows registry redirect metadata and
  returns requested path, final path, complete chain, final metadata, and a
  structured terminal state. Load and reference APIs use this path.
- Public `LoadAsset` and hard/soft reference loading follow redirectors before
  final type validation. An internal exact-load seam may construct a redirector
  for inspection and tooling but cannot leak redirector objects as the result
  of an ordinary typed asset load.
- Normal resolution accepts at most 32 redirects, records visited paths, and
  reports stable `MissingRedirectTarget`, `RedirectCycle`,
  `RedirectDepthExceeded`, and `RedirectTypeMismatch` results.
- Expected-class validation is performed against the final real asset. The
  redirector class never causes an otherwise valid typed reference to fail.
- Registry exact lookup and resolution never load packages or change package
  residency.

### Soft Reference Identity and Cache

- The authored `FSoftObjectPath` remains the sole serialized identity and the
  sole input to equality and hashing.
- `FSoftObjectPtr` additionally records the resolved package path associated
  with its weak cache. A value authored as `A` may therefore cache the real
  object at `B` after AssetCore proves `A -> B`.
- Ordinary public object assignment continues capturing the object's current
  canonical package path. Only AssetCore's checked resolution seam may cache an
  object whose current package path differs from the authored path.
- `Get()` validates the weak object against the cached resolved path; it never
  loads or consults the registry. If the real asset moves again, the changed
  package path invalidates the old cache and the next explicit resolve/load
  refreshes it.
- Resolution never changes the authored path. Fix Up, direct user assignment,
  paste, Undo/Redo, or explicit reset are the only durable path mutations.
- Redirector destinations are immutable except for move-driven compression and
  same-object destination reclamation. Arbitrary retargeting to a different
  live object is rejected so existing resolved caches cannot silently become
  semantically stale.

### Reference Graph and Package Rewrite

- Replace the soft-only derived projection with one `FAssetReferenceIndex`
  containing `HardObject`, `SoftObject`, and `Redirect` edges.
- Every package edge records source package, target path, reference kind,
  expected class when present, stable field/container route when applicable,
  display route, and complete source fingerprint.
- Hard package dependencies remain available as a compact graph, but Fix Up
  correctness uses exact tagged field occurrences rather than assuming the
  package header identifies every field to rewrite.
- Package inspection and rewrite operate on bounded DAST wire descriptors,
  construct no owner objects, invoke no constructors or `PostLoad`, load no
  targets, and preserve every unrelated field byte.
- The generic codec handles direct, fixed-array, Array, Map-value, and nested
  struct hard/soft occurrences. Unsupported Map keys and malformed or
  unbounded routes fail closed.
- Registry reconciliation, save, source move, source deletion, format/schema
  change, and fingerprint change update or invalidate only affected source
  projections. Cache corruption remains a rebuildable derived-data failure.
- An incomplete reference index never blocks a move. It may block redirector
  deletion because Fix Up cannot prove that every incoming authored reference
  was rewritten.

### Atomic Batch Relocation

- Asset relocation is a batch operation even for one asset. Analysis produces
  one immutable `FAssetRelocationBatchToken` with source/destination mapping,
  registry revision, physical fingerprints, mount/write policy, redirector
  mutations, loaded-package state, and asset-owned payload moves.
- A batch validates duplicate sources/destinations, mapped destination
  occupancy, mount containment, reparse points, companion ownership, loaded
  package conflicts, and exact redirector collisions before mutation.
- Mutation stages real assets and generated redirectors first, then publishes
  filesystem state, loaded-package paths, object names, companion paths, and
  registry projection as one journaled transition.
- A successful `A -> B` move leaves the real asset at `B` and a redirector
  `A -> B`. No referencer package or external reference store is read or saved.
- Registry visibility changes once per batch revision; observers never see a
  source missing before its redirector or a destination visible without the
  corresponding source alias.
- The moving asset itself may be loaded to maintain package/object naming and
  class-owned payload invariants. No referencer is loaded merely because it
  points to a moving asset.
- Moving `B -> C` when `A -> B` already exists atomically produces direct
  aliases `A -> C` and `B -> C`. Normal authoring operations do not leave
  redirector chains.
- A destination redirector may be reclaimed only when its final destination is
  the same real asset being moved. Reclaiming `A` for `C -> A` removes the old
  `A -> C`, creates `C -> A`, and retargets same-object upstream aliases
  directly to `A` in the same batch. An unrelated redirector remains a hard
  collision.
- Redirector chain handling is bounded by the number of alias packages, not by
  the number of arbitrary asset referencers, and does not depend on the
  package-reference index.

### Mutation Transaction and Threading

- Asset mutation planning may inspect immutable registry/package snapshots on
  workers, but token creation, revalidation, loaded-object mutation,
  publication, compensation, and registry revision changes occur on the game
  thread.
- Introduce one reusable journaled asset-mutation primitive for relocation and
  Fix Up instead of embedding ad hoc backup ownership in `MoveAsset`.
- Every authored output is prepared before any authoritative input is removed.
  File publication uses contained staging roots and atomic single-file writes
  or same-volume renames as appropriate.
- Revalidation compares the exact registry revision and physical fingerprints
  captured by the token. Stale input produces no mutation and requires a new
  plan/confirmation.
- Compensation runs in reverse publication order. A compensation failure
  enters an explicit recovery-required state with retained staging metadata;
  it is never reported as an ordinary failed move with assumed rollback.
- Editor Undo restores the exact pre-operation real asset and redirector file
  set. Redo reapplies the same mapping only after current-state and staged-byte
  revalidation.

### Fix Up Redirectors

- Fix Up is the only bulk path-rewrite workflow. It is never invoked implicitly
  by move, load, save, registry scan, property display, or editor startup.
- Selecting redirectors computes their upstream redirector closure and final
  real targets. All rewritten references point directly to final paths, never
  to intermediate aliases.
- `AnalyzeRedirectorFixup` returns an immutable plan containing selected alias
  closure, package occurrences, external-store occurrences, fingerprints,
  dirty/compatibility state, proposed writes, and redirectors eligible for
  deletion.
- The default `RewriteAndDelete` mode requires a complete reference index,
  every required external provider, writable clean inputs, and successful
  preconstruction of all output bytes. It publishes all changes or none.
- `RewriteOnly` may atomically rewrite the complete known plan while retaining
  redirectors, but it never claims that incomplete indexing made a redirector
  deletable.
- Loaded clean packages may be rewritten through validated live reflected
  values and saved with exact rollback snapshots. Dirty packages block by
  default; UI may require a separate explicit save before rebuilding the plan.
- Unloaded packages use the lossless tagged package codec and are never loaded
  merely to fix references.
- After publication, Fix Up rebuilds affected index projections and proves
  there is no remaining package or external occurrence targeting a deleted
  alias before removing redirector files and registry entries.
- Failure to rewrite one participant leaves every selected redirector valid.

### Contribution Contracts

- Replace `FAssetMoveContribution` with three non-overlapping responsibilities:
  - `FAssetOwnedPayloadRelocator` describes only files and in-memory state
    physically owned by the moving asset and participates in relocation.
  - `IAssetReferenceStore` enumerates and transactionally rewrites persistent
    paths outside `.dasset` packages and participates only in Fix Up.
  - `IAssetMoveObserver` receives committed move batches to invalidate or move
    transient caches and presentation state; observers cannot fail or roll back
    authored relocation.
- Project default-level settings become an `IAssetReferenceStore`. They keep
  the authored old path after a move, resolve through the redirector, and change
  only when Fix Up commits.
- Import-record output identities remain domain values rather than mechanically
  becoming soft object properties. Lookup and reimport resolve them before use;
  their owning provider exposes fixable occurrences when canonicalization is
  requested.
- Thumbnail entries, document titles, Content Browser selection, and other
  rebuildable/session state react through committed move observation and never
  join authored transaction rollback.

### Deletion

- A redirector's hard destination edge normally blocks deleting the real target
  until the redirector is fixed up or included in the same explicit deletion
  set.
- Deleting a redirector alone requires zero incoming hard, soft, redirect, and
  registered external occurrences. Force deletion is not a normal Content
  Browser action.
- Batch deletion may explicitly include a real target plus its full redirector
  closure. Hard edges inside the deletion set are allowed; soft references
  outside the set may become dangling only after confirmation reports their
  count and paths.
- Existing hard-reference deletion safety remains otherwise unchanged. A soft
  path to an asset that never had a redirector may still dangle after explicit
  target deletion.
- Delete, Undo, and Redo publish exact registry entries and redirector metadata
  from their captured token rather than reconstructing aliases heuristically.

### Cooking and Runtime Output

- Cook root discovery and graph traversal resolve redirectors before class/type
  validation and terminate cycles through the shared resolver.
- Authoring packages remain unchanged by cooking. Cook serialization rewrites
  hard and soft paths in produced bytes to final real asset paths.
- Redirector packages are editor-only authoring artifacts and are excluded from
  normal cooked publication after all reachable references and external roots
  have been canonicalized.
- Project default level and other external runtime roots are canonicalized by
  their owning cook contribution. A missing provider or unresolved alias is a
  packaging failure.
- Missing targets, redirect cycles, depth overflow, type mismatch, and a cooked
  reference that still targets a redirector fail deterministically before
  manifest publication.
- Cooked runtime remains read-only and does not need to construct redirector
  assets or carry a mutable redirect table.

### Editor Workflow

- Content Browser hides redirectors by default while preserving their folders
  and exact registry identities.
- Add `Show Redirectors`, redirector-only filtering, `Fix Up Redirectors`, and
  `Fix Up Redirectors in Folder` actions.
- Redirector details show old path, direct target, final target, chain,
  referencer counts by kind, index completeness, and terminal diagnostics.
- Ordinary asset pickers exclude redirectors as candidates. A property holding
  an old path displays a redirected state and final target without silently
  assigning the new path.
- Creating or importing at a redirector-occupied path is rejected with an
  actionable Fix Up/delete explanation.
- Moving folders uses one relocation batch and one global editor transaction,
  not a sequence of single-asset calls with reverse moves as rollback.

## Current Foundations and Gaps

### Reusable Foundations

- `FAssetPath`, `FSoftObjectPath`, `FSoftObjectPtr`, and
  `TSoftObjectPtr<T>` already provide canonical authored identity, typed soft
  semantics, reflection, Archive/snapshot persistence, and bounded DAST fields.
- AssetCore already has synchronous registry discovery, exact package
  fingerprints, header hard dependencies, package inspection, loaded-package
  caching, atomic package save, soft occurrence extraction, soft referencer
  queries, and cook reachability.
- The current unloaded soft rewriter demonstrates bounded tagged DAST traversal
  without owner construction or `PostLoad`.
- Content Browser deletion already uses immutable analysis tokens, physical
  fingerprints, same-volume staging, revalidation, global Undo/Redo, and
  recovery-required state. Relocation should reuse those architectural
  principles without sharing deletion-specific ownership.
- Asset class contributors already identify asset-owned companion files, and
  the editor already has thumbnail/document/session refresh seams after content
  mutations.

### Gaps to Replace

- `FAssetRegistry::FindAsset` does not distinguish exact lookup from resolution
  because redirectors do not exist.
- Package headers expose no authoritative redirector kind/destination metadata
  or generic searchable asset metadata contract.
- `FSoftObjectPtr` rejects a cached object whose real package path differs from
  its authored path.
- Hard and soft reference loading do not share a redirect-aware resolver.
- The reference index is soft-only and the hard dependency graph has no tagged
  field occurrences for lossless Fix Up.
- `FAssetManager::MoveAsset` owns referencer loading, soft rewrites, external
  stores, companion files, registry backups, `.movebak` files, and rollback in
  one function.
- `FEditorAssetMoveCoordinator` performs a batch as sequential moves and uses
  reverse moves as compensation.
- Move contributors mix asset-owned payloads with unrelated reference-owner
  package edits, while move external stores mix persistence with editor
  lifetime registration.
- Content Browser has no redirector item model, visibility filter, Fix Up
  workflow, redirect-aware collision diagnostics, or move Undo/Redo token.
- Deletion and cooking do not recognize redirect edges or redirector closure.

## Stage 0 Contract Baseline

This section is the implementation contract for Stages 1-6. Later stages may
add private helpers, but changing a name, byte layout, failure state, ordering
rule, or ownership boundary below requires recording the changed decision and
rationale here before implementation continues.

### Current Seam Inventory

| Seam | Current symbol and behavior | Replacement boundary |
| --- | --- | --- |
| Package header and registry snapshot | `AssetVersion = 2`, `WritePackageFile`, `ReadPackageHeader`, `FAssetPackageHeader`, and `FAssetData` expose class, hard dependencies, and object count only. `Registry.bin` reuses that projection by physical fingerprint. | DAST v3 adds bounded entry kind and redirect destination. The registry snapshot persists the same fields and remains rebuildable. |
| Registry identity | `FAssetRegistry::FindAsset` is one exact map lookup because aliases do not exist. | Mutation and inspection use `FindAssetExact`; loading and reference consumers use `ResolveAssetPath`. |
| Soft reference graph | `FSoftAssetReference`, `ExtractSoftAssetReferences`, and `SoftReferences.bin` index only tagged soft occurrences. Header dependencies have no field routes. | `FAssetReferenceIndex` owns hard, soft, and redirect edges. The old projection is removed after all consumers migrate. |
| Single-asset move | `FAssetManager::MoveAsset` loads hard referencers, rewrites loaded and unloaded soft referencers, saves contributor packages and external stores, moves companions, and uses `.movebak` copies for rollback. | One immutable relocation batch creates aliases and never visits arbitrary referencers or external stores. |
| Editor batch and folders | `FEditorAssetMoveCoordinator::MoveAssets` calls the single move repeatedly; it and `FContentBrowserOperations` reverse completed moves when later work fails. | One AssetCore batch and one editor transaction own single, multi-asset, and folder relocation plus Undo/Redo. |
| Deletion transaction | `FAssetDeletionBatchToken`, `FContentDeletionPlan`, and `FContentDeletionTransaction` already bind registry revision, fingerprints, same-volume staging, reverse compensation, Undo/Redo, and `RecoveryRequired`. | The shared mutation journal reuses these principles, while deletion, relocation, and Fix Up retain separate domain plans. |
| Default level | `FEditorAssetMoveCoordinator` registers project `Editor.DefaultLevel` as an `FAssetMoveExternalStore`, so a move rewrites YAML or rolls the move back. | The setting resolves through aliases and becomes an `IAssetReferenceStore` participant only during Fix Up. |
| Import records | `RegisterImportRecordMoveContributor` loads the managing `DImportRecord`, edits output paths, and contributes its package to the move transaction. | Output identities resolve before lookup/reimport and import records participate in Fix Up, never relocation. |
| Asset companions | `FAssetMoveContribution` and `FAssetDeleteContribution` are class-keyed callbacks. StaticMesh and Texture sources are explicitly independent, while future truly owned payloads may contribute files. | `FAssetOwnedPayloadRelocator` covers only exclusive asset-owned files and reversible in-memory state. Shared mounted sources remain outside asset relocation. |
| Cook | `BuildCookReachability` walks exact hard dependencies plus the soft-only index; `FCookContext` publishes caller-supplied package bytes and companions. | Reachability and package emission canonicalize every redirect to the final real path and exclude redirector packages. |
| Transient editor state | Viewport/session state is updated inside move coordination and can currently force asset rollback; Content Browser panels refresh from mutation revisions. | `IAssetMoveObserver` receives a committed batch, cannot veto it, and repairs only rebuildable/transient state. |

### DAST v3 Redirect Summary

- DAST v3 is assigned to this plan. The deferred compact serialization format
  is DAST v4; its roadmap must consume v3 as an input format and must not fold
  compact tables, sparse values, LEB128, or other v4 work into this refactor.
- The v3 header retains every v2 scalar, string, dependency, object, and field
  encoding. Immediately after `AssetClassName`, it inserts exactly these two
  values before `DependencyCount`:

  ```text
  uint8  EntryKind
  string RedirectDestination
  ```

  `string` is the existing `uint64` byte count followed by that many bytes and
  remains capped by `MaximumPackageStringBytes` (1 MiB). `EntryKind` is the
  wire value of `EAssetRegistryEntryKind`: `Asset = 0`, `Redirector = 1`.
  Unknown values are corrupt input. An `Asset` encodes a zero-length
  destination; a `Redirector` encodes one non-empty canonical `FAssetPath`.
- V2 has no inserted fields and is projected as `Asset` with no destination.
  After Stage 1, readers accept v2 and v3, while every authorized save emits
  v3. Scan and load never migrate or dirty v2 packages merely because of their
  version. A package claiming `DAssetRedirector` in v2 is corrupt because v2
  cannot carry the authoritative summary.
- A v3 redirector has main class `Durin::Asset::DAssetRedirector`, one object
  record, one non-null external `DestinationObject` hard reference, and one
  unique dependency equal to `RedirectDestination`. The destination cannot be
  the package itself. A redirect kind with another class, a redirector class
  with ordinary kind, a null/internal/second destination, a dependency mismatch,
  or header/body disagreement is `CorruptRedirector`.
- Header-only discovery validates magic, supported version, kind, class/kind
  pairing, destination syntax, dependency cardinality/membership, counts, and
  bounds without reading object records. Full inspection, validation, save,
  and load additionally prove the body invariant. No registry scan constructs
  a redirector or its destination.
- `FAssetPackageHeader`, `FAssetData`, registry cache entries, and scan
  statistics carry `EntryKind` and `RedirectDestination`. The registry cache
  schema is bumped once; old snapshots rebuild non-fatally. The current soft
  cache is likewise invalidated by the package-version/schema change and is
  later deleted when `FAssetReferenceIndex` becomes authoritative.

### Frozen Public Vocabulary and Ownership

| Name | Owner and contract |
| --- | --- |
| `DAssetRedirector` | AssetCore reflected main-asset type with private `TObjectPtr<DObject> DestinationObject`; ordinary callers receive no destination mutator. |
| `EAssetRegistryEntryKind` | `Asset` or `Redirector`, stored in package and registry metadata. |
| `FAssetRegistry::FindAssetExact` | Returns only the entry physically registered at the requested path, including a redirector. The old ambiguous `FindAsset` name is removed after callers migrate. |
| `EAssetPathResolveState` | `Resolved`, `NotFound`, `MissingRedirectTarget`, `RedirectCycle`, `RedirectDepthExceeded`, `UnknownTargetClass`, `RedirectTypeMismatch`, or `CorruptRedirector`. |
| `FAssetPathResolveOptions` | Optional expected final `DClass`; redirect depth is the fixed system limit, not a caller-controlled value. |
| `FAssetPathResolveResult` | Value result containing state, requested path, final path, ordered redirect chain, and a copy of final `FAssetData` when available. It owns no registry pointer and remains diagnostic after a revision change. |
| `FAssetRegistry::ResolveAssetPath` | Non-loading resolver used by all normal hard/soft load and reference seams. |
| `FAssetRegistry::FindRedirectorsTo` | Deterministic direct reverse lookup over exact redirect metadata. |
| `EAssetReferenceKind` | `HardObject`, `SoftObject`, or `Redirect`. |
| `FAssetReferenceRouteSegment`, `FAssetReferenceEdge`, `FAssetReferenceIndex` | Generalized tagged package occurrence route, edge record, and authoritative derived graph. |
| `FAssetRelocationMapping`, `FAssetRelocationBatchToken` | Requested source/destination pair and the getter-only AssetCore batch token returned by `AnalyzeAssetRelocationBatch`. The token is the AssetCore plan; there is no second mutable AssetCore plan object. |
| `AnalyzeAssetRelocationBatch`, `RevalidateAssetRelocationBatch`, `ApplyAssetRelocationBatch`, `RestoreAssetRelocationBatch` | Only supported relocation lifecycle. LevelEditor may wrap the token in an immutable `FContentRelocationPlan` for confirmation and history. |
| `EAssetRedirectorFixupMode` | `RewriteOnly` or `RewriteAndDelete`. |
| `FAssetRedirectorFixupPlan` | Getter-only plan containing selected/upstream aliases, final mapping, package/store occurrences, proposed bytes/actions, fingerprints, compatibility/dirty state, and deletable aliases. |
| `AnalyzeRedirectorFixup`, `RevalidateRedirectorFixup`, `ApplyRedirectorFixup` | Only supported persistent path-rewrite lifecycle. |
| `FAssetOwnedPayloadRelocator` | Class-keyed relocation contract for files and in-memory state exclusively owned by the moving asset. |
| `IAssetReferenceStore` | Registered persistent non-package reference provider. It enumerates exact occurrences and prepares fingerprint-bound reversible writes for Fix Up only. |
| `IAssetMoveObserver` | Post-commit, game-thread notification for transient/rebuildable state. It returns no failure and owns no authored rollback. |
| `FAssetMutationJournal` | AssetCore-internal journal shared by relocation and Fix Up; deletion may adopt it later without sharing domain plan types. |

### Resolution, Cache, and Redirect Mutation Rules

- `FAssetPath` and `FSoftObjectPath` remain syntax and authored identity only.
  Resolution follows at most 32 redirectors. `RedirectChain` contains the exact
  alias paths in traversal order and excludes the final real path. A chain of
  32 aliases followed by a real asset succeeds; encountering a 33rd alias is
  `RedirectDepthExceeded`.
- The resolver records visited exact paths before following each edge. Revisiting
  any path is `RedirectCycle`; a syntactically valid destination with no exact
  registry entry is `MissingRedirectTarget`. `NotFound` is reserved for a
  missing requested path. Expected-class lookup and validation apply only to
  the final real entry; an unavailable reflected class is `UnknownTargetClass`
  and an incompatible final class is `RedirectTypeMismatch`.
- Normal loading resolves first, constructs only the final real package, and
  returns only its main asset. AssetCore exposes a private exact-load seam for
  redirector validation/tooling. Neither a redirector object nor an intermediate
  package is loaded by registry resolution.
- `FSoftObjectPtr` adds `ResolvedPackagePath` beside its authored path and weak
  object. Its private `TrySetResolvedObject` seam accepts only an AssetCore-proven
  successful resolution for the same authored path. `Get()` performs no I/O;
  it returns the weak object only when it remains a package main asset of the
  expected class and its current package path equals `ResolvedPackagePath`.
  `SetPath`, direct object assignment, reset, and move-from clear or replace
  both cache fields. A dead weak object or failed `Get()` validation returns
  null without mutating authored identity; a failed assignment preserves the
  prior value. Equality, ordering, hashing, Archive, snapshots, DAST bytes, and
  property editing continue to use only the authored path.
- A redirect destination is immutable to general authoring. Creation rejects
  null, self, package, inner, transient, same-package, missing, corrupt, or
  type-invalid destinations and never accepts an arbitrary redirector target.
  Only relocation may replace an alias destination, and only while proving it
  still denotes the same final real asset.
- A successful `A -> B` emits `A -> B`. Moving that real asset `B -> C`
  publishes `A -> C` and `B -> C` in the same batch. For move-back `C -> A`,
  the occupied `A -> C` may be reclaimed only after exact resolution proves it
  targets the moving `C`; upstream aliases to the same object are rewritten
  directly to `A`. Any unrelated or unprovable redirector collision is a hard
  blocker with no mutation.

### Reference Graph and Fix Up Completeness

- `FAssetReferenceEdge` records source package, complete source fingerprint,
  source object id/class, declaring type and field, `EAssetReferenceKind`,
  target path, optional expected class, stable route, and display route.
  `FAssetReferenceRouteSegment` has `FixedArray`, `ArrayElement`, `MapValue`,
  and `StructField` kinds with the required index, canonical Map-key token, or
  declaring-type/field identity.
- Extraction retains the current limits: four container levels, 100,000
  occurrences per package, 1,000,000 per snapshot, 1 MiB package paths and
  Map-key tokens, and 4 KiB display routes. Direct, fixed-array, Array,
  Map-value, and nested-struct hard/soft values are supported. Soft Map keys,
  object Map keys that cannot produce stable lossless tokens, schema mismatch,
  malformed payloads, overflows, and unconsumed bytes fail the complete source
  projection closed.
- The index publishes one explicit completeness state plus per-source errors.
  Relocation never consults it. `RewriteAndDelete` requires a complete index and
  every registered store; `RewriteOnly` may publish only a complete known plan
  while retaining aliases and must report that deletion was not proven.
- Fix Up always expands the selected aliases to their upstream closure and maps
  all matching hard, soft, redirect, and store occurrences directly to final
  real paths. It does not load unloaded packages. Dirty loaded packages,
  compatibility-risk saves, provider absence, read-only inputs, stale
  fingerprints, malformed routes, or any output that cannot be fully prepared
  are preflight blockers, not partial work.

### Shared Mutation Protocol

- `EAssetMutationState` is internal and has exactly `Planned`, `Prepared`,
  `Publishing`, `Committed`, `Compensating`, `Restored`, and
  `RecoveryRequired`. Preflight failures remain `Planned`; no authoritative
  input changes before `Prepared`. A failed publish reaches `Restored` only
  after complete reverse compensation. Any compensation failure enters
  `RecoveryRequired` and retains all staging and recovery metadata.
- Each affected writable content mount owns
  `<Content>/.durin-asset-mutation/operation-<128-bit-id>/`. Entries use
  extensionless deterministic names so registry discovery cannot mistake them
  for packages. The root contains an exact ownership marker and a versioned
  journal recording operation id/type/state, original/staged/published paths,
  pre/post fingerprints, step order, and completed/compensated bits. Provider
  staging roots must declare the same containment, non-reparse, ownership, and
  same-volume atomic-replace guarantees. A locator beneath
  `Saved/AssetMutationRecovery` names all roots; it is not authoritative data.
- Planning snapshots may be inspected on workers, but registry/package
  snapshot capture, token construction, revalidation, loaded-object changes,
  provider prepare/apply, journal state changes, publication, compensation,
  registry revision publication, and observer dispatch occur on the game
  thread. No observer or provider callback runs while a worker owns mutable
  package state.
- Revalidation compares the exact registry revision; source and participant
  size/time/content fingerprints; exact path occupancy and redirect resolution;
  mount identity, containment, write policy, and reparse status; loaded package
  pointer/path/dirty/edit revision; payload ownership and companion
  fingerprints; provider identity/version/fingerprints; and every staged output
  hash. A mismatch returns stale data and performs no mutation.
- Relocation prepares all real package, redirector, companion, and registry
  projections first. Publication then journals originals into staging,
  publishes real destinations and owned payloads, publishes source/upstream
  redirectors, updates loaded package paths/object names and payload state,
  and swaps the complete registry/reverse-index projection under one revision.
  Only after that commit do move observers run. Ordinary failure compensates
  those completed steps in exact reverse order.
- Fix Up prepares every package byte image and store action first. Publication
  applies package and store rewrites, builds a private candidate reference
  projection from the published bytes/state, verifies zero incoming occurrence
  for every alias proposed for deletion, stages those redirector files, and
  swaps registry/reference projections once. `RewriteOnly` commits before the
  deletion steps and retains every alias. Failure compensates store and package
  writes before restoring any staged alias.
- Undo retains the exact journal-owned pre-operation bytes and metadata. Redo
  revalidates the post-operation fingerprints and destination availability;
  neither operation recomputes a new mapping. Recovery cleanup validates the
  marker, operation id, roots, and non-reparse containment and never removes a
  `RecoveryRequired` root automatically.

### Compatibility and Cook Policy

- The supported authored inputs become DAST v2 and v3; v3 is the only writer
  after Stage 1. Saving is the explicit authorization to migrate an ordinary
  v2 package. Registry scans, loads, resolution, relocation of unchanged bytes,
  and Fix Up analysis never migrate merely by observation.
- Registry and reference caches are disposable schemas, not compatibility
  surfaces. Schema mismatches rebuild. `MoveAsset`,
  `FAssetMoveContribution`, `FAssetMoveExternalStore`, `.movebak`, the
  soft-only index/cache, and sequential editor rollback remain only until their
  staged replacements have all callers, then are removed without shims. This
  cleanup happens in the earliest stage that makes an item unreachable; Stage 6
  is the final audit, not a requirement to retain dead code until then. Tests
  that exist only to preserve removed behavior are deleted with it, while tests
  for independent persistence, failure, compatibility, or recovery guarantees
  are migrated to the replacement contract.
- From redirect-producing relocation onward, Cook fails closed with a stable
  redirect-canonicalization diagnostic until the Stage 6 canonical Cook path is
  active. The final Cook resolves every root and hard/soft/store edge to a real
  path, validates the final class, rewrites produced package bytes to those
  paths, and never emits `DAssetRedirector` packages or redirect metadata.
  Authored files are not modified by cooking.
- Project default level and import-record output paths deliberately retain old
  authored values after relocation. Loads/lookups resolve them; strict Fix Up
  rewrites them through `IAssetReferenceStore`. Mounted source paths, DDC keys,
  document identifiers, and other classified non-reference identities are not
  mechanically rewritten.

### Frozen Fixture Catalog

| Fixture | Required proof |
| --- | --- |
| `Redirect.Direct` | `A -> B` survives save, unload, header scan, exact lookup, resolved lookup, and restart without constructing `A`. |
| `Redirect.RepeatedMove` | `A -> B`, then `B -> C`, stores only `A -> C` and `B -> C`. |
| `Redirect.MoveBack` | `C -> A` reclaims only the same-object `A -> C`; an unrelated alias collision remains unchanged. |
| `Redirect.Depth` | 32 aliases resolve; 33 fail with `RedirectDepthExceeded`; the reported chain is deterministic. |
| `Redirect.CycleAndMissing` | Self/cycle/missing targets produce their exact terminal states with no partial final metadata. |
| `Redirect.CorruptWire` | Unknown kind, class/kind mismatch, null/invalid destination, dependency mismatch, extra object/reference, and header/body disagreement fail closed. |
| `Registry.RestartAndCache` | Incremental snapshot reuse retains kind/destination; full validation and corrupt-cache rebuild reproduce the same map and reverse index. |
| `Reference.HardSoft` | Old authored hard and soft paths load the final typed object; soft equality/hash/serialized bytes remain unchanged. |
| `Relocation.BatchFolder` | Single, multi-asset, cross-directory, and folder mappings publish one revision with no referencer load/save. |
| `Relocation.Collision` | Duplicate sources/destinations, occupied real targets, unrelated aliases, reparse points, ownership conflicts, and stale tokens make no change. |
| `Relocation.FailureJournal` | Every prepare/publish/compensate boundary restores exactly or retains a diagnosable `RecoveryRequired` root. |
| `FixUp.PackageRoutes` | Direct/fixed/Array/Map-value/nested-struct hard and soft occurrences rewrite losslessly to final paths without owner construction. |
| `FixUp.ExternalStores` | Default level and import records rewrite atomically; missing/read-only/stale providers retain all aliases. |
| `Deletion.RedirectClosure` | Redirect hard blockers, alias-only rejection, target-plus-alias deletion, and Undo/Redo restore exact files and registry entries. |
| `Editor.UndoRedo` | Single/multi/folder relocation and Fix Up occupy one history entry; stale Redo and compensation failure preserve the current head. |
| `Cook.Canonical` | Redirected roots and hard/soft dependencies emit only final real paths, exclude aliases, and fail deterministically for missing/cycle/type errors. |

## Implementation Stages

Every stage ends with a compact handoff recording its baseline commit, working
set, key symbols and decisions, open questions, and validation outcome. A later
stage treats completed handoffs as established context and expands their
working set only when targeted validation shows a direct dependency gap.

### Stage 0: Freeze Redirect and Mutation Contracts

Dependencies: none.

- [x] Inventory current move, batch move, reference-index, package-header,
  deletion-token, cook, default-level, import-record, and companion-file seams
  using the bounded working set named in this plan.
- [x] Assign the package envelope version and exact bounded encoding for
  redirector kind/destination metadata; record interaction with the compact
  serialization roadmap without adopting its unrelated encoding scope.
- [x] Freeze public names and ownership for exact lookup, resolution result,
  redirect type, reference graph, relocation plan/token, Fix Up plan/modes,
  payload relocator, reference store, and move observer.
- [x] Freeze the 32-hop limit, structured error taxonomy, direct-to-final chain
  invariant, same-object destination reclamation rules, and forbidden
  retargeting behavior.
- [x] Define transaction states, staging ownership, revalidation inputs,
  compensation ordering, recovery metadata, and game-thread boundaries shared
  by relocation and Fix Up.
- [x] Define authored and cooked compatibility policy explicitly. No current
  move API or cache schema is preserved accidentally.
- [x] Define fixtures for direct redirects, repeated moves, move-back, folder
  batches, collision, cycle/corruption, hard/soft paths, external stores,
  deletion, Undo/Redo, registry restart, and cook canonicalization.
- [x] End with the required stage handoff.

#### Acceptance Gate

- One unambiguous contract covers exact identity, resolution, persistence,
  caching, batch publication, Fix Up, deletion, cooking, and editor behavior.
- Wire format, ownership, failure, ordering, thread, and recovery decisions are
  selected before production implementation starts.
- The historical soft-reference plan is referenced only as replaced current
  behavior, not as future acceptance criteria.

#### Stage 0 Handoff

- Baseline commit: `a6f7276b14630d0f60e3ab5809dbc98c076a54b6`.
- Working set: `AssetSystem.h/.cpp`, `Package.h/.cpp`, `AssetPath.h`,
  `SoftObjectPtr.h/.cpp`, `EditorAssetMoveCoordinator.h/.cpp`,
  `ContentBrowserOperations.h/.cpp`, `ContentDeletionTransaction.cpp`,
  `ImportRecordIndex.cpp`, `ReflectedPropertyView.cpp`, the StaticMesh/Texture
  contributor registrations, `AssetPackages.md`, `ContentBrowser.md`,
  `LevelSystem.md`, `AssetImportFramework.md`, `MountedSourceWorkflows.md`, and
  `CompactAssetSerialization.md`.
- Key symbols and decisions: DAST v3 owns the explicit redirect summary and
  v4 remains the compact format; exact and resolved registry APIs are distinct;
  32 aliases are accepted; soft cache identity is authored path plus resolved
  package path; relocation never visits referencers; Fix Up is complete,
  provider-aware, and deletion-last; one journal/recovery protocol is shared by
  relocation and Fix Up.
- Open questions: none block Stage 1. UI wording, CLI spelling, and provider-
  specific implementation details remain local to their owning later stages
  and cannot weaken this contract.
- Stage 1 initial working set: `AssetSystem.h`, `AssetSystem.cpp`, new
  `AssetRedirector.h/.cpp`, and `PackageTests.cpp`. Expand only for a generated-
  reflection or build-metadata dependency identified by those files.
- Validation: `DevTool doc plan validate --scope all` passes on 2026-08-06.
  Stage 0 changes contracts only, so no configure, build, runtime launch, or
  native test is required.

### Stage 1: Add Redirector Persistence and Registry Resolution

Dependencies: Stage 0 contracts.

- [x] Add `DAssetRedirector` with generated reflection and enforce its single
  hard package-main-asset destination invariant.
- [x] Write/read bounded redirector registry metadata and validate it against
  body value and dependency table during package inspection/load.
- [x] Extend `FAssetData`, persistent registry snapshots, scan statistics, and
  cache schemas with entry kind and redirect destination.
- [x] Add exact lookup, reverse redirector index, non-loading resolution,
  complete chain results, stable errors, depth bound, and cycle detection.
- [x] Add direct-target normalization utilities and reject self, missing,
  corrupt, type-invalid, and forbidden destination mutations.
- [x] Keep ordinary assets and current moves behaviorally unchanged during this
  stage while proving redirect packages survive save, unload, rescan, restart,
  snapshot reuse, full validation, and cache corruption rebuild.
- [x] Add focused package/registry tests and end with the required stage
  handoff.

#### Acceptance Gate

- Registry discovery identifies a redirector and its destination from bounded
  header data without constructing objects.
- Exact lookup exposes redirectors while resolution returns the final real
  metadata without loading packages.
- Valid chains resolve deterministically; every malformed terminal state fails
  with its selected code and no partial registry projection.

#### Stage 1 Handoff

- Baseline commit: `0b5c86baf4bf4782391bd580669f3ab6f5037ed4`.
- Working set: `AssetSystem.h/.cpp`, new `AssetRedirector.h/.cpp`,
  `AssetCompatibility.cpp`, `AssetCore.h`, `AssetCore.dmodule`,
  `DerivedDataCache.h`, `PackageTests.cpp`, the retired
  `newer_format.dasset.hex` fixture, `AssetPackages.md`,
  `CompactAssetSerialization.md`, and this plan.
- Key symbols and decisions: DAST v3 is the only writer and the bounded reader
  accepts v2/v3; `DAssetRedirector` has one private hard destination;
  `FAssetRegistry::FindAssetExact`, `ResolveAssetPath`, and
  `FindRedirectorsTo` expose exact, resolved, and direct-reverse views without
  loading; 32 aliases succeed and a 33rd fails; registry cache schema 2 carries
  redirect metadata and the soft cache invalidates through the DAST version;
  `CreateAssetRedirector` is the controlled direct-to-final construction seam.
- Compatibility and cleanup: the streaming compatibility probe now understands
  both v2 and v3 headers and retains the source DAST version for typed field
  inspection. The obsolete fixture that treated v3 as an unsupported future
  version was deleted; the test now derives an actual v4 input from current
  bytes. Ordinary move behavior remains unchanged for Stage 2.
- Open questions: none block Stage 2. The compatibility `FindAsset` spelling
  remains as an exact-lookup forwarding seam until Stage 2 migrates normal
  loading/reference callers, after which it can be deleted.
- Validation on 2026-08-06: `DevTool test --target AssetPackageTests --agent`
  passes 59 tests; `DevTool test --target AssetCookTests --agent` passes 12
  tests; `DevTool test --target all --agent` passes the complete native suite.
  Documentation plan validation is recorded after this handoff update.

### Stage 2: Route Hard and Soft Loading Through Redirect Resolution

Dependencies: Stage 1 registry resolver.

- [ ] Make public asset loading resolve the requested path before package
  construction and validate the final real asset against the expected class.
- [ ] Route cross-package hard reference loading and typed soft resolve/load
  through the same resolver and diagnostics.
- [ ] Add the internal exact redirector load/inspection seam without exposing a
  redirector as an ordinary typed load result.
- [ ] Extend `FSoftObjectPtr` with resolved-cache identity and a checked
  AssetCore-only cache assignment path while preserving authored equality,
  hashing, snapshots, and serialization.
- [ ] Verify cache refresh, asset unload, repeated target moves, object
  collection, missing target, type mismatch, cycle, and depth overflow.
- [ ] Update editor soft-property inspection so redirected, loaded, unloaded,
  missing, and type-mismatched states remain distinguishable without loading.
- [ ] Add focused CoreDObject, AssetCore, reflection, Archive, and editor-model
  tests and end with the required stage handoff.

#### Acceptance Gate

- Hard and soft references authored to an old path load the final real object
  transparently after unload and restart.
- A soft value retains its old serialized path while safely caching an object
  whose package is at the resolved path.
- Exact tools can inspect the redirector itself, but gameplay/editor asset
  callers never receive it in place of the requested asset type.

### Stage 3: Replace Move with Atomic Batch Relocation

Dependencies: Stage 2 transparent loading.

- [ ] Introduce relocation analysis/token/revalidation/application/restoration
  APIs and the shared journaled mutation primitive.
- [ ] Implement collision-safe staging and one-step publication for real asset
  packages, generated redirectors, asset-owned companion files, loaded-package
  paths/object names, and registry projection.
- [ ] Implement direct-to-final upstream alias compression and same-object
  redirector destination reclamation.
- [ ] Split asset-owned payload relocation and transient observer contracts out
  of the old move contribution API.
- [ ] Convert Content Browser and level-document callers to a single batch API;
  add one editor transaction for single asset, multi-asset, folder, Undo, and
  Redo relocation.
- [ ] Ensure successful moves do not query reference-index completeness, read
  referencer packages, modify soft paths, save external stores, or depend on
  referencer writability.
- [ ] Remove sequential editor reverse-move rollback and the old single-asset
  publication path once all callers are converted.
- [ ] Add failure injection for every staging/publication/compensation boundary,
  focused move/Content Browser tests, and the required stage handoff.

#### Acceptance Gate

- A batch either publishes every real destination and old-path redirector under
  one registry revision or preserves the complete pre-move state.
- Moving an asset succeeds with stale/incomplete soft-reference derived data
  and read-only or malformed referencers because none is part of the move.
- Repeated moves produce direct aliases, move-back safely reclaims only a
  same-object redirector, and Undo/Redo restore exact persisted states.

### Stage 4: Build Unified Reference Index and Fix Up

Dependencies: Stage 3 redirect-producing relocation.

- [ ] Generalize package extraction/cache schemas into
  `FAssetReferenceIndex` with hard, soft, and redirect occurrence kinds.
- [ ] Generalize tagged DAST rewriting to hard and soft reference occurrences
  while preserving all unrelated and unknown field bytes.
- [ ] Implement Fix Up analysis, upstream closure, final-path mapping,
  fingerprint/write/dirty/compatibility preflight, immutable plan, revalidation,
  rewrite modes, publication, verification, and deletion.
- [ ] Add `IAssetReferenceStore` with deterministic enumeration and
  transaction-ready rewrite contributions; migrate project default level and
  import-record management paths.
- [ ] Ensure provider absence, index incompleteness, dirty loaded packages,
  compatibility risk, read-only inputs, malformed fields, changed
  fingerprints, publication failure, and verification failure leave selected
  redirectors valid.
- [ ] Add CLI/service-level seams for project-wide unattended Fix Up without
  implementing source-control automation.
- [ ] Remove soft-reference index APIs and cache files superseded by the unified
  graph after registry, Cook, editor queries, and tests use the new contract.
- [ ] Add focused package-codec, index, external-store, and failure-injection
  tests and end with the required stage handoff.

#### Acceptance Gate

- Strict Fix Up rewrites every indexed package and registered external
  occurrence directly to final paths, verifies zero incoming references, and
  only then deletes redirectors.
- Unloaded package Fix Up constructs no objects, invokes no `PostLoad`, and
  preserves non-reference bytes.
- Any inability to prove completeness prevents deletion but does not invalidate
  already-authored redirectors or future moves.

### Stage 5: Integrate Content Browser, Deletion, and Asset Owners

Dependencies: Stage 4 Fix Up service.

- [ ] Add redirector item kind, hidden-by-default projection, visibility
  filters, details/diagnostics, referencer navigation, and folder/global Fix Up
  commands.
- [ ] Add redirected-state presentation to reflected soft fields and asset-path
  owners without silently canonicalizing authored values.
- [ ] Add actionable destination-collision messaging for create, import,
  duplicate, rename, move, and reimport workflows.
- [ ] Route default-level loading, import-record lookup/reimport, document open,
  and other selected persistent owners through resolution; classify retained
  path-only owners explicitly.
- [ ] Update deletion analysis/token/Undo/Redo for redirect hard blockers,
  redirector closure, exact alias restoration, and explicit target-plus-alias
  deletion warnings.
- [ ] Route thumbnail, selection, editor document, viewport/session, and other
  rebuildable state through committed mutation revisions or move observers.
- [ ] Verify editor restart, hidden folders containing only redirectors,
  multi-panel refresh, selection repair, and failed Fix Up UX.
- [ ] Add focused editor workflow/deletion/import tests and end with the
  required stage handoff.

#### Acceptance Gate

- Users can move assets without referencer repair, reveal redirectors on demand,
  inspect their state, and perform folder/project Fix Up through one shared
  service.
- Delete and Undo/Redo preserve redirector safety and exact registry/file state.
- Every persistent non-package path owner either resolves and participates in
  Fix Up or is documented as intentionally non-reference identity.

### Stage 6: Canonicalize Cooked Output and Remove the Legacy Architecture

Dependencies: Stage 5 complete authoring workflow.

- [ ] Make Cook roots, hard/soft traversal, type validation, and produced package
  serialization resolve to final asset paths.
- [ ] Exclude redirector packages from normal cooked publication and fail if a
  runtime root/reference remains redirected or unresolved after canonicalization.
- [ ] Verify default-level and every registered external runtime root contribute
  canonical final identity without modifying authored files.
- [ ] Remove any remaining `FAssetMoveExternalStore`, additional-package move contributions,
  move-time soft/hard referencer rewrites, `.movebak` ownership, stale-index
  move blockers, superseded cache schemas, compatibility shims, and obsolete
  tests.
- [ ] Update lasting Asset Packages, Content Browser, Cook, package-format,
  editor workflow, and import/source documentation; mark the old soft-reference
  plan only as historical provenance.
- [ ] Run package/plan/document validation, focused native suites, the complete
  native test set, and a successful full `all` build through DurinDevTool.
- [ ] Audit the final diff and public API for duplicate exact/resolved paths,
  accidental implicit path mutation, remaining single-move loops, and authored
  redirectors entering runtime output.
- [ ] End with the required final stage handoff and completion evidence.

#### Acceptance Gate

- Cooked manifests and package bytes contain only final real asset identities
  and no redirector package is required at runtime.
- No production move path rewrites referencers or registers external storage in
  the relocation transaction.
- All lasting documentation, generated code, registry/cache schemas, editor
  workflows, focused suites, full native tests, and full build agree on the new
  redirector model.

## Validation Matrix

| Area | Required coverage |
| --- | --- |
| Redirect package | save/load, header/body agreement, one hard destination, self/inner/transient/null rejection, dependency emission, unload/restart |
| Registry | exact lookup, resolved lookup, reverse aliases, persistent snapshot reuse, full validation, corrupt cache rebuild, revision atomicity |
| Resolution | direct alias, chain, compressed chain, missing target, cycle, depth limit, type mismatch, no package load, structured diagnostics |
| Soft cache | authored/resolved path split, equality/hash/bytes unchanged, refresh, unload, GC, repeated move, move-back, stale object-path invalidation |
| Hard/soft load | loaded/unloaded target, hard dependency through redirect, explicit soft load, final-type validation, no redirect object leakage |
| Relocation | single, rename, multi-asset, folder, cross-directory, companion files, repeated move, move-back, mapped collision, unrelated redirect collision |
| Relocation failure | stale plan, read-only source/destination, staging failure, publish failure, registry failure, observer behavior, compensation and recovery state |
| Independence | incomplete/corrupt soft index, read-only referencer, malformed referencer, missing external provider, unloaded referencers, no referencer save/load |
| Reference graph | hard/soft/redirect edges, direct/fixed/Array/Map/struct routes, fingerprints, bounds, cache invalidation, deterministic ordering |
| Fix Up | one/many/folder/global aliases, upstream closure, final mapping, loaded clean package, dirty blocker, unloaded lossless rewrite, external stores |
| Fix Up failure | incomplete index, provider absence, compatibility risk, read-only file, stale fingerprint, malformed field, partial publication, verification failure |
| Deletion | redirect blocks target, alias-only delete rejection, target-plus-alias batch, soft dangling warning, Undo/Redo exact restoration |
| Editor | hidden/show filters, details, navigation, redirected property state, collision messaging, batch move transaction, multi-panel refresh, restart |
| Owners | default level, import record lookup/reimport, companions, thumbnails, documents, viewport/session settings, retained non-reference paths |
| Cook | redirected root, hard/soft redirected dependency, repeated alias, missing/cycle/type errors, canonical produced bytes, redirect exclusion |
| Qualification | DHT/generated code, package/registry/core/editor/import/cook suites, full native tests, document/plan validation, full `all` build |

## Definition of Done

- Moving an asset or folder publishes real destinations and old-path
  redirectors atomically without reading, loading, rewriting, or saving arbitrary
  referencers.
- Old hard and soft references resolve transparently after unload, restart, and
  registry-cache rebuild while soft authored paths remain byte-stable until Fix
  Up.
- Registry callers explicitly choose exact identity or redirect resolution; no
  mutation code accidentally treats a final target as the file occupying the
  requested path.
- Normal moves maintain direct-to-final aliases, reject corrupt/cyclic or
  unrelated collisions, and safely support same-object move-back.
- One unified reference graph and tagged package codec support deterministic
  hard/soft/redirect queries, Fix Up, Cook, and diagnostics without target
  loading.
- Strict Fix Up is complete, fingerprint-bound, provider-aware, and atomic; a
  redirector is deleted only after zero remaining incoming persistent
  occurrences are proven.
- Asset-owned payload relocation, external persistent references, and transient
  observers use separate contracts with no callback participating in the wrong
  transaction boundary.
- Deletion, editor Undo/Redo, import/reimport, default-level settings, Content
  Browser, and cooked output all implement the documented redirect semantics.
- Cooked output contains canonical real asset paths and does not require
  authored redirector packages at runtime.
- The old move-time rewrite architecture, sequential rollback, external move
  stores, obsolete cache schemas, compatibility surfaces, tests, and docs are
  removed rather than maintained beside the replacement.
- Required documentation validation, focused suites, full native tests, and a
  successful full `all` build pass under the same Agent Build Profile.

## Deferred Follow-ups

- Core Redirects for native/reflected type and property renames.
- Asset consolidation and replace-references workflows that deliberately
  redirect one live object identity to another.
- Async loading, streamable handles, priority, cancellation, bundles, and
  residency scopes.
- Soft references to subobjects and external object paths within one package.
- Network replication or remote/content-addressed asset resolution.
- Automated source-control checkout/checkin for unattended Fix Up.
- Optional compact cooked alias tables for products that intentionally expose
  old authoring paths at runtime; the default remains canonical redirect-free
  output.
- Compact package serialization work beyond the minimal explicit redirect
  metadata owned by this plan.

## Related Documentation

- [Documentation entry point](../README.md)
- [Asset Packages](../Runtime/Assets/AssetPackages.md)
- [Content Browser](../Editor/Architecture/ContentBrowser.md)
- [Workspace Projects](../Workspace/WorkspaceProjects.md)
- [Level System](../Runtime/World/LevelSystem.md)
- [Asset Import Framework](../Editor/Architecture/AssetImportFramework.md)
- [Mounted Source Workflows](../Editor/Guides/MountedSourceWorkflows.md)
- [Soft Asset References Plan](SoftAssetReferences.md) — completed historical
  implementation evidence only; its move contract is replaced by this plan.
- [Compact Asset Serialization Roadmap](../Roadmaps/CompactAssetSerialization.md)
- [Build and Run](../Development/Build/BuildAndRun.md)

## Related Code

- `Engine/Source/Runtime/CoreDObject/Public/DObject/AssetPath.h`
- `Engine/Source/Runtime/CoreDObject/Public/DObject/SoftObjectPtr.h`
- `Engine/Source/Runtime/CoreDObject/Private/DObject/SoftObjectPtr.cpp`
- `Engine/Source/Runtime/CoreDObject/Public/DObject/Package.h`
- `Engine/Source/Runtime/CoreDObject/Private/DObject/Package.cpp`
- `Engine/Source/Runtime/AssetCore/Public/AssetSystem.h`
- `Engine/Source/Runtime/AssetCore/Private/AssetSystem.cpp`
- `Engine/Source/Runtime/AssetCore/Private/AssetCompatibility.cpp`
- `Engine/Source/Editor/AssetImportCore/Private/ImportRecordIndex.cpp`
- `Engine/Source/Editor/DurinEd/Private/Editor/ReflectedPropertyView.cpp`
- `Engine/Source/Editor/LevelEditor/Private/Assets/EditorAssetMoveCoordinator.h`
- `Engine/Source/Editor/LevelEditor/Private/Assets/EditorAssetMoveCoordinator.cpp`
- `Engine/Source/Editor/LevelEditor/Private/Panels/ContentBrowserModel.cpp`
- `Engine/Source/Editor/LevelEditor/Private/Panels/ContentBrowserOperations.h`
- `Engine/Source/Editor/LevelEditor/Private/Panels/ContentBrowserOperations.cpp`
- `Engine/Tests/Native/AssetCoreTests/Private/PackageTests.cpp`
- `Engine/Tests/Native/AssetCoreTests/Private/ImportRecordTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/Editor/ContentBrowserModelTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/Editor/ContentBrowserItemViewTests.cpp`
