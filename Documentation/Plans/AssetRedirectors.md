# Asset Redirectors Refactor Plan

Summary: Replace eager move-time reference rewriting with first-class asset redirectors, transparent path resolution, atomic batch relocation, and explicit fix-up workflows.

Last reviewed: 2026-08-06

Status: Active
Completed:

## Current Status

- Design is selected and implementation has not started.
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

## Implementation Stages

Every stage ends with a compact handoff recording its baseline commit, working
set, key symbols and decisions, open questions, and validation outcome. A later
stage treats completed handoffs as established context and expands their
working set only when targeted validation shows a direct dependency gap.

### Stage 0: Freeze Redirect and Mutation Contracts

Dependencies: none.

- [ ] Inventory current move, batch move, reference-index, package-header,
  deletion-token, cook, default-level, import-record, and companion-file seams
  using the bounded working set named in this plan.
- [ ] Assign the package envelope version and exact bounded encoding for
  redirector kind/destination metadata; record interaction with the compact
  serialization roadmap without adopting its unrelated encoding scope.
- [ ] Freeze public names and ownership for exact lookup, resolution result,
  redirect type, reference graph, relocation plan/token, Fix Up plan/modes,
  payload relocator, reference store, and move observer.
- [ ] Freeze the 32-hop limit, structured error taxonomy, direct-to-final chain
  invariant, same-object destination reclamation rules, and forbidden
  retargeting behavior.
- [ ] Define transaction states, staging ownership, revalidation inputs,
  compensation ordering, recovery metadata, and game-thread boundaries shared
  by relocation and Fix Up.
- [ ] Define authored and cooked compatibility policy explicitly. No current
  move API or cache schema is preserved accidentally.
- [ ] Define fixtures for direct redirects, repeated moves, move-back, folder
  batches, collision, cycle/corruption, hard/soft paths, external stores,
  deletion, Undo/Redo, registry restart, and cook canonicalization.
- [ ] End with the required stage handoff.

#### Acceptance Gate

- One unambiguous contract covers exact identity, resolution, persistence,
  caching, batch publication, Fix Up, deletion, cooking, and editor behavior.
- Wire format, ownership, failure, ordering, thread, and recovery decisions are
  selected before production implementation starts.
- The historical soft-reference plan is referenced only as replaced current
  behavior, not as future acceptance criteria.

### Stage 1: Add Redirector Persistence and Registry Resolution

Dependencies: Stage 0 contracts.

- [ ] Add `DAssetRedirector` with generated reflection and enforce its single
  hard package-main-asset destination invariant.
- [ ] Write/read bounded redirector registry metadata and validate it against
  body value and dependency table during package inspection/load.
- [ ] Extend `FAssetData`, persistent registry snapshots, scan statistics, and
  cache schemas with entry kind and redirect destination.
- [ ] Add exact lookup, reverse redirector index, non-loading resolution,
  complete chain results, stable errors, depth bound, and cycle detection.
- [ ] Add direct-target normalization utilities and reject self, missing,
  corrupt, type-invalid, and forbidden destination mutations.
- [ ] Keep ordinary assets and current moves behaviorally unchanged during this
  stage while proving redirect packages survive save, unload, rescan, restart,
  snapshot reuse, full validation, and cache corruption rebuild.
- [ ] Add focused package/registry tests and end with the required stage
  handoff.

#### Acceptance Gate

- Registry discovery identifies a redirector and its destination from bounded
  header data without constructing objects.
- Exact lookup exposes redirectors while resolution returns the final real
  metadata without loading packages.
- Valid chains resolve deterministically; every malformed terminal state fails
  with its selected code and no partial registry projection.

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
- [ ] Remove `FAssetMoveExternalStore`, additional-package move contributions,
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
