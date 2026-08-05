# Soft Asset References Plan

Summary: Add typed persistent asset paths with non-owning loaded-object caches, explicit resolution, reflected serialization, registry indexing, and editor workflows without turning soft references into eager package dependencies.

Last reviewed: 2026-08-05

Status: Active
Completed:

## Current Status

- Planning is complete and Stage 0 is ready to begin as preparation work.
- The engine currently has no `TSoftObjectPtr<T>`, `FSoftObjectPtr`, or reflected
  soft-object property kind.
- Reflected `TObjectPtr<T>` fields are hard references. Cross-package values are
  serialized as asset paths, added to the package dependency table, loaded
  eagerly, retained by the loaded-package graph, and required to resolve.
- `TWeakObjectPtr<T>` is a non-owning handle to an already loaded object. It is
  not a persistent asset identity and is not recognized by DHT as a property.
- `FAssetPath` and plain strings provide unloaded identity in editor settings,
  import records, thumbnails, and service code, but loading, type checks, picker
  assignment, move repair, and failure handling are implemented manually.
- `FEditorAssetPickerConfig::CurrentSelectionPath` explicitly supports owners
  that keep a soft path, but selecting an entry still loads the asset before the
  owner can store its path.
- Stage 0 and the Core-only value contract may be prepared alongside the current
  reflection work. Production property-registration work must consume the
  completed [Reflected Struct Operations](ReflectedStructOperations.md) and
  [Typed Struct Property Registration](TypedStructPropertyRegistration.md)
  baselines and must not share a checkout writer with another DHT/CoreDObject
  migration.
- This plan is independent of DAST v3 compact encoding and does not reactivate
  the [Compact Asset Serialization Roadmap](../Roadmaps/CompactAssetSerialization.md).

## Goal

Provide one typed soft asset-reference abstraction that persists a canonical
main-asset path, optionally caches a non-owning handle to an already loaded
object, and resolves or loads only when explicitly requested. Soft references
must participate in reflection, property editing, package inspection, move and
delete analysis, cooking reachability, snapshots, and authored serialization
without keeping objects alive, forcing dependency packages to load, or making a
temporarily missing asset invalidate its owner package.

The result must replace ad hoc “string path plus manual `LoadAsset`” behavior
where soft-reference semantics are intended while preserving `TObjectPtr` for
genuinely hard runtime dependencies and `TWeakObjectPtr` for transient
already-loaded object observation.

## Scope

- Audit current hard object references, weak object handles, canonical asset
  paths, path strings, picker path mode, registry lookups, manual loads, and
  asset-move fixups to classify actual hard, weak, and soft semantics.
- Add CoreDObject-owned `FSoftObjectPath`, `FSoftObjectPtr`, and
  `TSoftObjectPtr<T>`-style value types, with final names frozen in Stage 0.
- Store a canonical `FAssetPath` for a package main asset and an optional
  `FWeakObjectPtr` cache; null is represented by an empty path and empty cache.
- Keep path identity and cached loaded state logically separate. Equality,
  hashing, snapshots, and serialization use the path only.
- Add AssetCore APIs for non-loading resolution of an already loaded package and
  explicit synchronous loading with type-safe checked results.
- Add a distinct `SoftObject` generated/runtime property kind,
  `FSoftObjectProperty`, and typed `FSoftObjectPropertyParams` registration.
- Teach DHT to recognize supported `TSoftObjectPtr<T>` declarations directly
  and when nested beneath existing Array and Map property schemas.
- Serialize soft references as canonical paths in runtime Archive, snapshots,
  package inspection, and authored DAST v2 field payloads without adding them to
  the hard package dependency table.
- Add a rebuildable soft-reference index that discovers package/path/type/field
  relationships from tagged asset fields without constructing objects or
  invoking `PostLoad`.
- Use the soft-reference index for referencer queries, asset move repair,
  deletion diagnostics, cook reachability, and editor navigation.
- Extend the editor asset picker with path-assignment mode so selecting a soft
  reference does not load the candidate asset.
- Support clear, browse/reveal, loaded-state display, explicit load, broken-path
  display, class filtering, Undo/Redo, and copy/paste for reflected soft fields.
- Migrate selected existing path-backed use cases whose intended semantics are
  demonstrably soft, beginning with the project default-level identity and the
  shared picker path workflow.
- Add focused CoreDObject, DHT, Archive, AssetCore, registry, move/delete, cook,
  editor-model, compatibility, and integration tests.

## Non-Goals

- Replacing hard `TObjectPtr<T>` fields that are required immediately for a
  loaded asset to function, such as material parents, material texture values,
  static-mesh material bindings, or active component resources, without a
  separate lifecycle and streaming decision.
- Changing `TObjectPtr` GC behavior, hard package dependency emission, eager
  hard-dependency loading, or hard-reference unload blocking.
- Replacing `TWeakObjectPtr` for selections, viewports, callbacks, and other
  transient references to already loaded objects.
- Supporting external subobject paths in the first version. Soft references
  target package main assets, matching the current `FAssetPath` and external
  hard-reference boundary.
- Adding asynchronous package streaming, cancellation, priorities, bundles, or
  a UE-style streamable manager. The first version provides explicit synchronous
  loading and a non-loading resolution seam that a later async plan can reuse.
- Automatically loading on construction, assignment, `Get`, comparison,
  serialization, property display, GC, registry scan, or package `PostLoad`.
- Treating every asset-path string as a soft object reference. Source paths,
  output identities, document IDs, import provenance, and mount paths retain
  their existing domain types.
- Allowing arbitrary filesystem paths, unmounted package paths, or class names
  that cannot participate in the asset registry.
- Making a soft reference prevent package unload or object collection.
- Requiring a referenced asset to exist merely to deserialize and retain a
  syntactically valid soft path.
- Adding DAST v3 tables, opcodes, compression, default-relative encoding, or
  compact soft-reference indices.
- Introducing a general resource-handle, scene-entity reference, network object
  reference, or component/subobject reference system.
- Running production DHT, property-layout, or AssetSystem changes concurrently
  with another plan that owns the same checkout and files.

## Design Decisions and Invariants

### Reference Categories

- `TObjectPtr<T>` remains the reflected hard object reference. A cross-package
  hard reference contributes a hard dependency, resolves during owner-package
  loading, keeps its target reachable, and may block package unload.
- `TWeakObjectPtr<T>` remains a transient non-owning handle. It neither stores a
  path nor reloads an object after unload.
- `TSoftObjectPtr<T>` stores persistent identity and a non-owning cache. It does
  not contribute a GC reference, hard load edge, or unload blocker.
- `FAssetPath` remains the canonical main-asset path primitive. A soft path owns
  an `FAssetPath`; it does not duplicate path normalization or mount rules.
- Hard, weak, and soft references have distinct reflected/runtime types. A flag
  on `FObjectProperty` is not used to blur their serialization and GC semantics.

### CoreDObject and AssetCore Ownership

- CoreDObject owns the soft path/pointer value representation, typed wrapper,
  weak cache, logical equality, and reflected property metadata. It does not
  gain a dependency on AssetCore or filesystem/package loading.
- AssetCore owns registry lookup, loaded-package resolution, synchronous load,
  type validation against registry/runtime classes, soft-reference indexing,
  move/delete integration, and cook reachability.
- CoreDObject exposes no hidden global loader callback. Loading is an explicit
  AssetCore API taking a soft reference or soft path and returning
  `FAssetResult` plus the resolved object.
- Editor code builds on the same AssetCore APIs and index. It does not maintain
  a second path-to-object cache or silently convert soft references to hard
  pointers.

### Value and Cache Semantics

- The first-version `FSoftObjectPath` contains either no path or one valid
  canonical `FAssetPath` targeting a package main asset.
- `FSoftObjectPtr` contains the path plus an optional `FWeakObjectPtr` cache.
  The cache is an optimization and never changes logical identity.
- Default construction and assignment from `nullptr` produce the null state.
  Assigning a path stores it without loading. Assigning a main-asset object
  captures its package path and weak handle after validation.
- Assigning an inner object, transient object, package object, invalid path, or
  wrong typed object returns a checked failure and leaves the destination
  unchanged.
- `IsNull` tests path identity. `IsLoaded`/`Get` inspect only a valid loaded
  object matching the stored path and expected type; they never initiate I/O.
- Non-loading resolution may refresh the weak cache from an already loaded
  package. Explicit load may load the package and then refresh the cache.
- If the target is unloaded or collected, the weak cache becomes invalid while
  the path remains intact. A later explicit load can resolve the same identity.
- Equality and hashing compare canonical paths only. Two references remain
  equal when one has a live cache and the other does not.
- Copy/move preserves path identity. Callers synchronize concurrent mutation;
  weak-handle assignment and resolution retain the current game-thread
  restriction.

### Typed Resolution and Failure

- `TSoftObjectPtr<T>` requires `T` to derive from `DObject` and publishes its
  expected class through generated property metadata.
- Registry assignment checks the recorded asset class when available. Explicit
  load validates the constructed object with `IsA<T>` before updating the cache.
- A syntactically valid path may remain stored when the registry entry or file
  is missing. Non-loading resolve reports “not loaded” separately from invalid
  path; explicit load returns a stable not-found or type-mismatch error.
- Null resolve/load succeeds with a null result only when the calling API
  explicitly permits null. Invalid path, missing target, wrong class, dependency
  load failure, and package corruption are distinct results.
- No failed assignment, resolution, or load clears a previously valid path or
  cache unless the caller explicitly requests reset.
- There is no implicit fallback asset in the soft-pointer layer. Higher-level
  systems may apply an explicit default or error asset after observing failure.

### Reflection and Generated Registration

- `EPropertyGenFlags::SoftObject` and `FSoftObjectProperty` are separate from
  hard `Object` properties and carry an expected-class resolver plus soft-value
  accessors.
- `FSoftObjectPropertyParams` is a distinct typed parameter record. Its
  construction API fixes the property kind and accepts only common field
  schema, expected-class resolution, optional metadata/accessors, and the
  selected soft-value operations.
- DHT recognizes `TSoftObjectPtr<T>` and `Durin::TSoftObjectPtr<T>` after proving
  that `T` resolves to a reflected `DObject` class.
- Raw `FSoftObjectPtr`, malformed templates, non-object target types, references,
  pointers to wrappers, unsupported qualifiers, and unresolved class symbols
  fail in DHT with source-qualified diagnostics.
- Generated initializers do not expose the physical common-base parameter layout
  or repeat hard-object, struct, enum, container, or generic lifecycle fields.
- Direct properties and nested Array/Map inner/value descriptors use the same
  typed soft property record. Soft Map keys are unsupported in the first version
  because loading identity is not intended to define mutable map key behavior.
- Soft properties do not compile into GC reference schemas. A cached weak handle
  is never traced as a strong reference or reported as a hidden strong struct
  reference.

### Serialization and Compatibility

- Runtime Archive and property snapshots serialize the canonical soft path, not
  the weak handle, object address, loaded state, package pointer, or registry
  metadata.
- Authored DAST v2 writes a distinct tagged soft-object field payload containing
  null or one canonical path. Loading stores the path and leaves the cache empty
  unless the target package was already loaded and non-loading resolution is
  explicitly requested later.
- Saving a soft field does not add the path to `FPackageFile::Dependencies`.
  Loading an owner package therefore does not load or require the soft target.
- The existing hard dependency header remains hard-only. Soft references are
  discovered from length-delimited tagged field payloads by a derived index, so
  this plan does not change DAST v2 header layout or claim the future DAST v3
  format number.
- Stage 0 freezes the exact DAST v2 type signature, null encoding, payload bound,
  compatibility-inspection representation, and behavior of older packages that
  omit the new property.
- A missing soft target is preserved as a dangling but inspectable path. Invalid
  path bytes, overlong strings, malformed null tags, trailing bytes, and a
  serialized property/type mismatch fail deterministically.
- Unknown-field preservation and compatibility-risk handling remain intact.
  Inspection can read a soft path without constructing the owner object or
  loading the target.
- DAST v3 may encode soft references through compact tables later, but it must
  preserve the same logical hard-versus-soft dependency distinction.

### Soft-Reference Index and Dependency Policy

- Asset registry hard dependencies retain their current eager-load meaning.
  They are never widened to include soft paths.
- A separate rebuildable soft-reference index maps target `FAssetPath` values to
  source package, declaring type, property path, expected class, and container
  context where available.
- Index extraction reads and validates tagged package fields without object
  construction, `PostLoad`, render-resource creation, or target loading. Results
  are cached by package fingerprint and invalidated on save, move, delete,
  rescan, or incompatible schema changes.
- Index corruption or cache miss triggers deterministic re-extraction from the
  authoritative package; the index is not authored content.
- Soft references do not make `IsPackageReferenced` return true and do not block
  ordinary target-package unload.
- By default, a valid reflected soft reference contributes cook reachability so
  the target is available for explicit runtime load. Stage 0 records any
  existing path-backed cases that require an explicit non-cook metadata policy;
  absence of such evidence means no opt-out is added in this plan.
- Runtime loading still occurs only when requested. Cook reachability and eager
  package loading are distinct policies.

### Move, Rename, and Delete Semantics

- Asset moves query the soft-reference index and rewrite affected serialized
  soft fields from the old canonical path to the new canonical path.
- Rewrites operate on the tagged soft payload or through the normal reflected
  edit/save path, never by blind byte replacement. They verify expected old
  identity, field kind, type signature, payload bounds, and source fingerprint.
- Target move, referencer rewrites, registry publication, index updates, and
  selected project-setting updates form one staged operation with rollback on
  failure. No source package is left pointing at a half-published destination.
- Loaded soft references update their path and invalidate or refresh their weak
  cache consistently. Unloaded referencers are repaired without running their
  constructors or `PostLoad` when the tagged rewrite path is sufficient.
- Deletion analysis reports soft referencers separately from hard blockers.
  Soft references are allowed to dangle by design, so they warn and support
  navigation/repair but do not automatically block deletion unless a higher
  level policy explicitly promotes one.
- Project/editor settings that adopt `FSoftObjectPath`, including the default
  level, participate in the same move-coordination contract even when their
  storage is JSON rather than a DAST field.

### Editor and Authoring Behavior

- The shared asset picker gains a path-selection callback distinct from its
  loaded-object assignment callback.
- Candidate discovery and class filtering use `FAssetRegistry` metadata. A user
  selecting a soft field assigns the candidate path without calling
  `Asset::LoadAsset`.
- Reflected soft fields display null, unloaded, loaded, missing, and type-mismatch
  states without conflating them. Broken paths remain visible and copyable.
- Browse/reveal uses the stored path. An explicit Load/Open action may invoke
  AssetCore and reports failure without changing the stored reference.
- Clearing, assigning, move repair, and paste participate in the normal
  reflected edit proposal, Undo/Redo, dirty-state, and save pipeline.
- Hard `FObjectProperty` editing retains its current load-and-assign behavior.
  The picker mode is selected by property semantics, not by whether a candidate
  happens to be loaded.

### Concurrency and Scheduling Boundary

- Stage 0 is documentation, inventory, and characterization work and may proceed
  in parallel with Reflected Struct Operations.
- Core value-type work that does not change generated registration may proceed
  in a separate worktree after Stage 0, but integration still follows the
  repository's single-writer rule.
- Typed property and DHT work begins only after Typed Struct Property
  Registration has frozen the common parameter dispatch boundary.
- Soft property implementation and Reflected Container Operations must not edit
  DHT/CoreDObject/AssetCore concurrently in the same checkout. Prefer completing
  Soft Asset References first so the container contract can treat SoftObject as
  an ordinary nested value kind; otherwise complete the container migration
  before starting this plan's overlapping stages.
- The math API facade may proceed independently while preserving reflected math
  identities and serialized schemas.

## Current Foundations and Gaps

### Foundations

- `FAssetPath` already validates canonical virtual asset paths beneath registered
  mounts and provides equality and hashing.
- `FWeakObjectPtr` already provides a compact non-owning loaded-object cache with
  game-thread assignment/resolution rules.
- `TObjectPtr<T>` and `FObjectProperty` establish typed class validation,
  reflected object access, GC tracing, editor assignment, and hard-reference
  serialization patterns.
- AssetCore already serializes cross-package hard references as canonical main-
  asset paths and distinguishes internal, external, and null references.
- DAST v2 fields are tagged and length-delimited, and package inspection can read
  object-reference payloads without materializing objects.
- AssetRegistry already records package fingerprints, class names, and hard
  dependencies and supports unloaded candidate discovery.
- Asset move/delete analysis, atomic package publication, compatibility
  inspection, and editor asset-picking seams already exist.
- The editor picker already accepts a current-selection path and sorts registry
  candidates without requiring them to be loaded.

### Gaps

- There is no value that combines persistent typed path identity with a weak
  loaded-object cache.
- DHT recognizes `TObjectPtr<T>` but not soft or weak object wrappers.
- Object property metadata cannot distinguish eager hard references from lazy
  persistent references.
- Current external `TObjectPtr` loading always resolves the target and fails the
  owner package when it is missing.
- `FAssetPath` is not a reflected soft-object property, carries no expected
  class, and offers no common resolve/load API.
- Path-backed owners manually duplicate registry lookup, `LoadAsset`, class
  checks, errors, move repair, picker plumbing, and serialization.
- The editor picker loads a selected asset before invoking its assignment
  callback even when the owner only wants the path.
- AssetRegistry has no separate soft dependency/referencer projection and no
  cache keyed by tagged-field fingerprints.
- Moves cannot centrally find and rewrite every soft path; deletion analysis
  cannot distinguish a dangling-allowed soft referencer from a hard blocker.
- Cook reachability and runtime eager loading are currently both inferred from
  the same hard dependency list.
- Tests do not establish unloaded round trips, dangling preservation, no-eager-
  load behavior, weak-cache invalidation, typed mismatch, path-only picker
  assignment, soft move repair, or cook-versus-load separation.

## Implementation Stages

### Stage 0: Audit Uses and Freeze Soft-Reference Semantics

- [ ] Inventory reflected hard object fields and classify whether each is
  required at owner load, optional/lazy, editor-only, or currently ambiguous.
- [ ] Inventory `FAssetPath` and asset-path string storage, manual `LoadAsset`
  calls, `CurrentSelectionPath` users, registry-only lookups, and move/delete
  fixups; distinguish object references from source/provenance/document paths.
- [ ] Record the first migration candidates and explicitly list hard references
  that must not be converted.
- [ ] Freeze the public type names, null state, path ownership, weak-cache
  behavior, equality/hash semantics, game-thread restrictions, and assignment
  failure rules.
- [ ] Freeze the CoreDObject/AssetCore API boundary for loaded-only resolve and
  explicit synchronous load.
- [ ] Freeze `SoftObject` property metadata, expected-class resolution, DHT
  accepted spellings, unsupported forms, and nested-container policy.
- [ ] Freeze the DAST v2 type signature, payload grammar, bounds, compatibility
  behavior, and proof that the hard dependency header layout remains unchanged.
- [ ] Define the derived soft-reference index schema, fingerprint/cache
  ownership, invalidation rules, corruption recovery, and extraction budgets.
- [ ] Freeze move rewrite, deletion-warning, default cook reachability, missing
  target, and type-mismatch policies.
- [ ] Characterize current default-level picker selection and prove where it
  loads solely to recover a path.
- [ ] End the stage with a handoff listing the baseline commit, inventory,
  selected contracts, migration candidates, overlapping working set, open
  questions resolved in the plan, and validation evidence.

#### Acceptance Gate

- Every selected migration candidate has one documented hard/weak/soft reason;
  path-like values outside asset-reference semantics are excluded.
- The type, reflection, serialization, registry, cook, move/delete, editor, and
  thread contracts have one selected behavior each.
- DAST v2 can carry the new tagged field without adding soft paths to the hard
  dependency header or claiming the deferred DAST v3 format.
- The index can be rebuilt without loading target packages or constructing owner
  objects and is never the only copy of authored identity.
- Production APIs and serialized content remain unchanged during Stage 0.

### Stage 1: Add Core Soft Path and Pointer Values

Dependencies: Stage 0 contract and handoff.

- [ ] Add the selected `FSoftObjectPath`, `FSoftObjectPtr`, and
  `TSoftObjectPtr<T>` declarations in CoreDObject without an AssetCore include or
  loader callback.
- [ ] Implement null/path/object assignment, reset, path access, weak-cache
  access, loaded-state queries, logical equality, hashing, copy, and move.
- [ ] Validate assignment from a main asset and reject inner, transient,
  package, invalid-path, and wrong-type objects without destination mutation.
- [ ] Add the AssetCore non-loading resolve and explicit synchronous load APIs
  with typed wrappers and stable result mapping.
- [ ] Refresh caches only after path and type validation; preserve path/cache on
  failed load according to the frozen contract.
- [ ] Verify target unload/collection invalidates the weak cache without erasing
  path identity or blocking the unload.
- [ ] Add focused CoreDObject/AssetCore tests for null, valid, missing, unloaded,
  already-loaded, wrong-type, copy/move, equality/hash, unload/reload, and
  game-thread constraints.
- [ ] End the stage with a handoff listing the baseline commit, working set,
  public symbols, ownership decisions, result codes, and focused test results.

#### Acceptance Gate

- CoreDObject soft values compile and operate without depending on AssetCore.
- No non-loading operation performs file I/O or package loading.
- Explicit load is type-safe, refreshes only the cache, and preserves logical
  path identity through unload/reload.
- Soft references add no GC edge and do not prevent target-package unload.
- Invalid assignment/load cases return stable failures and do not partially
  mutate an existing valid value.

### Stage 2: Add Typed Reflection, DHT, and Property Editing

Dependencies: Stage 1 and the completed Reflected Struct Operations and Typed
Struct Property Registration handoffs.

- [ ] Add the `SoftObject` generation flag, runtime property class, cast flag,
  typed parameter record, construction dispatch, and descriptor validation.
- [ ] Add typed mutable/const value access for direct and accessor-backed soft
  properties without exposing wrapper layout to generic consumers.
- [ ] Teach DHT parsing and symbol resolution to recognize valid typed soft
  declarations and nested Array/Map descriptors.
- [ ] Emit concise typed soft-property metadata with expected-class resolution
  and no positional hard-object or generic lifecycle placeholders.
- [ ] Add deterministic source-qualified diagnostics for raw wrappers,
  non-object target types, unresolved classes, unsupported qualifiers, soft Map
  keys, and excessive nesting.
- [ ] Exclude soft properties from GC schema assembly and hidden-strong-reference
  accounting.
- [ ] Extend property equality, snapshots, copy/paste, change paths, and detached
  value storage to compare/copy the logical path while ignoring cache state.
- [ ] Extend the shared asset picker and reflected property view with path-only
  assignment, unloaded/broken state, clear, reveal, and explicit load actions.
- [ ] Add DHT exact-output/negative tests and CoreDObject/editor-model tests for
  direct, nested, null, loaded, unloaded, missing, typed, and Undo/Redo cases.
- [ ] End the stage with a handoff listing the baseline commit, descriptor ABI,
  DHT diagnostics, generated fixtures, editor behavior, and validation results.

#### Acceptance Gate

- Direct and supported nested `TSoftObjectPtr<T>` fields register as
  `FSoftObjectProperty` with the correct expected class and no strong GC schema.
- Unsupported declarations fail in DHT rather than in generated C++ or runtime
  registration.
- Generated metadata is concise, type-safe, and byte-deterministic.
- The editor can select and clear a soft field without loading the selected
  asset; explicit load remains a separate visible action.
- Logical edits, snapshots, Undo/Redo, and equality remain unchanged when only
  weak-cache state changes.

### Stage 3: Serialize Soft Fields and Build the Reference Index

Dependencies: Stage 2 property metadata and value access.

- [ ] Add bounded Archive and canonical snapshot serialization of null/path soft
  values with sticky nested errors and no implicit resolution.
- [ ] Add the frozen DAST v2 SoftObject type signature, tagged payload writer,
  reader, compatibility inspection, and malformed-input diagnostics.
- [ ] Confirm serialization never writes weak handles or adds soft paths to the
  hard `Dependencies` collection.
- [ ] Add package-inspection support that extracts soft path, expected class,
  declaring type, property path, and container context without object creation.
- [ ] Implement the fingerprinted rebuildable soft-reference index, persistent
  cache schema, bounds, invalidation, cache corruption fallback, and deterministic
  ordering.
- [ ] Expose target-to-referencer and source-to-soft-target queries separately
  from hard dependency queries.
- [ ] Integrate default soft cook reachability without changing runtime eager
  load or unload-blocking decisions.
- [ ] Add fixtures for old packages without soft fields, valid unresolved paths,
  wrong registered/runtime types, unknown fields, truncated/overlong payloads,
  cache miss/corruption, and no-target-load index scans.
- [ ] Verify saving/loading an owner package with an unloaded or missing soft
  target succeeds and preserves exact logical identity.
- [ ] End the stage with a handoff listing the baseline commit, payload contract,
  index/cache schema, query APIs, compatibility fixtures, and focused results.

#### Acceptance Gate

- Archive, snapshots, DAST v2, and inspection round-trip soft paths without
  serializing loaded state or loading targets.
- Owner-package load succeeds with an unloaded or missing syntactically valid
  soft target, while explicit resolution reports the correct later failure.
- Hard package headers and hard dependency loading behavior are unchanged.
- The soft-reference index is deterministic, bounded, fingerprinted,
  corruption-recoverable, and rebuildable without object construction.
- Cook reachability includes default soft targets while package loading and
  unload decisions continue to use hard edges only.

### Stage 4: Integrate Move, Delete, and Selected Path Owners

Dependencies: Stage 3 authoritative field and index contracts.

- [ ] Add soft referencers to move planning and atomically rewrite validated
  loaded and unloaded DAST fields from old path to new path.
- [ ] Integrate registry/index publication and rollback so target move and all
  selected soft-reference rewrites commit or restore together.
- [ ] Add soft referencers to deletion analysis as navigable warnings distinct
  from hard dependency blockers.
- [ ] Update loaded soft values after moves and invalidate stale caches without
  converting them to hard references.
- [ ] Migrate the project default-level identity to the selected soft-path value
  contract and remove picker-time loading performed only to recover its path.
- [ ] Route default-level move repair through the shared move contract while
  preserving project-settings atomic save/rollback.
- [ ] Audit other Stage 0 candidates and migrate only those with accepted soft
  semantics; record explicit reasons for retained manual paths.
- [ ] Add move collision, stale fingerprint, read-only referencer, partial
  publication failure, rollback, loaded/unloaded referencer, missing target,
  deletion warning, cook inclusion, and project-setting tests.
- [ ] Verify editor reveal, explicit load, broken-reference repair, class
  filtering, and Undo/Redo across a target move and application restart.
- [ ] End the stage with a handoff listing the baseline commit, migrated owners,
  retained paths, move/delete policies, rollback evidence, and validation.

#### Acceptance Gate

- Moving a target updates every indexed soft DAST field and selected project
  setting atomically or restores the entire operation.
- Unloaded referencers are repaired without invoking their constructors,
  `PostLoad`, or target loads when tagged rewrite is valid.
- Soft referencers are visible during deletion analysis but do not become hard
  unload/deletion blockers by accident.
- The default-level picker stores and moves canonical identity without loading
  the selected level merely to assign it.
- Retained `TObjectPtr`, `TWeakObjectPtr`, `FAssetPath`, and string uses have
  recorded semantics rather than being mechanically converted.

### Stage 5: Qualify and Document the Reference Model

Dependencies: Stage 4 integration and migration evidence.

- [ ] Run a final repository audit for reflected object references, weak handles,
  soft pointers, asset paths, path strings, and manual `LoadAsset` calls.
- [ ] Verify direct and nested soft fields for null, unloaded, loaded, missing,
  wrong-type, moved, deleted, cooked, copied, duplicated, snapshotted, and
  application-restarted cases.
- [ ] Verify hard references still emit/load dependencies and block unload,
  while weak and soft references retain their distinct behavior.
- [ ] Verify package inspection, compatibility audit, registry cache rebuild,
  source/target queries, cook traversal, move/delete analysis, editor picker,
  Undo/Redo, save/resave, and corrupted-input behavior.
- [ ] Remove transitional path-picker branches, duplicated load-to-get-path
  helpers, obsolete manual move fixups, and legacy generated fixtures only after
  their owners use the shared contract.
- [ ] Document the lasting hard/weak/soft decision guide, public APIs, reflection
  schema, load/error behavior, index ownership, and authoring workflow in the
  owning runtime/editor documentation.
- [ ] Follow the repository build and test instructions for focused suites and a
  successful full `all` build with the required long-running timeouts.
- [ ] End the stage with a handoff listing the baseline commit, final working
  set, validation commands/results, lasting documentation, removed compatibility
  surfaces, and evidence-gated follow-ups.

#### Acceptance Gate

- The engine exposes one documented typed soft-reference abstraction and one
  reflected SoftObject property path from declaration through editor and disk.
- Hard, weak, and soft references have independently tested GC, load, unload,
  persistence, dependency, move, delete, and cook behavior.
- No soft operation performs implicit package loading or turns a dangling path
  into owner-package load failure.
- Soft-reference indices and caches are rebuildable derived data; canonical path
  payloads remain the authored source of truth.
- Selected ad hoc path owners use the shared contract, and remaining manual
  paths are demonstrably not soft object references or are explicitly deferred.
- Focused suites and a successful full `all` build validate tools, generated
  code, runtime modules, AssetCore, editor consumers, and tests together.

## Validation Matrix

| Area | Required coverage |
| --- | --- |
| Core values | null, canonical path, object assignment, inner/transient rejection, weak-cache refresh/invalidation, equality/hash, copy/move, and thread rules |
| Typed load | already loaded, unloaded, missing, wrong registry class, wrong runtime class, corrupt package, null policy, and unchanged value on failure |
| DHT | direct/nested typed soft fields, qualified aliases, unresolved/non-object targets, raw wrapper, qualifiers, soft Map key rejection, and deterministic output |
| Property metadata | typed params, expected class, accessors, kind-safe dispatch, no strong GC schema, and handwritten/generated parity |
| Serialization | Archive, snapshot, DAST v2 null/path, bounds, malformed/trailing bytes, unknown fields, missing targets, and no weak-cache bytes |
| Dependency behavior | hard dependency table unchanged, no eager soft loads, no soft unload blocker, separate soft queries, and default cook reachability |
| Reference index | extraction without construction, source/target projection, container context, fingerprint hit/miss, invalidation, corruption fallback, bounds, and deterministic cache |
| Move/delete | loaded/unloaded referencers, atomic rewrite, stale source, read-only failure, collision, rollback, deletion warning, dangling repair, and registry publication |
| Editor | registry-only picker assignment, null/unloaded/loaded/broken states, reveal, explicit load, clear, paste, class filter, Undo/Redo, dirty/save, and restart |
| Migration | default level, retained hard resources, retained transient weak handles, excluded source/import/document paths, and manual-path audit |
| Integration | package save/load/resave, inspection, compatibility audit, duplication, GC, cook graph, move/delete workflows, focused native suites, and full all build |

## Definition of Done

- `TSoftObjectPtr<T>` stores canonical typed asset identity plus a non-owning
  cache and never loads implicitly.
- CoreDObject owns value/reflection semantics while AssetCore owns resolve/load,
  registry, index, move/delete, and cook integration without a dependency cycle.
- DHT generates one distinct typed SoftObject property kind for valid direct and
  supported nested declarations and rejects unsupported forms precisely.
- Soft references serialize as bounded canonical paths, remain valid while
  unloaded or missing, and never enter the hard dependency header.
- Owner packages load without soft targets; explicit typed load reports stable
  results and refreshes only weak cache state.
- GC and package unload ignore soft caches as strong edges.
- The derived reference index supports deterministic queries, cook reachability,
  move repair, and deletion diagnostics without constructing owner objects.
- The editor assigns soft paths from registry metadata without loading, exposes
  explicit load/reveal/repair actions, and participates in normal edit history.
- Target moves rewrite indexed soft paths atomically, and deletion analysis keeps
  soft warnings distinct from hard blockers.
- The default-level path workflow uses the shared soft-path contract, while
  genuinely hard, weak, and non-reference path uses remain unchanged.
- Lasting documentation, focused tests, generated output, runtime integration,
  and the full build all agree on the final hard/weak/soft reference model.

## Deferred Follow-ups

- Asynchronous soft-asset loading, cancellation, priorities, bundles, streaming
  handles, and residency scopes.
- External subobject paths and typed component/object-within-package references.
- A general asset manager policy for primary assets, labels, bundles, and
  explicit runtime-optional cook exclusions.
- Network-replicated soft references and remote/content-addressed asset sources.
- Cross-project or plugin-optional references whose mounts may not be registered
  in the current workspace.
- Bulk editor repair UI for broken or type-mismatched soft references.
- DAST v3 compact soft-reference tables and default-relative encoding under the
  Compact Asset Serialization roadmap.

## Related Documentation

- [Documentation entry point](../README.md)
- [Reflected Struct Operations Plan](ReflectedStructOperations.md)
- [Typed Struct Property Registration Plan](TypedStructPropertyRegistration.md)
- [Reflected Container Operations Plan](ReflectedContainerOperations.md)
- [Compact Asset Serialization Roadmap](../Roadmaps/CompactAssetSerialization.md)
- [Build and Run](../Development/Build/BuildAndRun.md)

## Related Code

- `Engine/Source/Runtime/CoreDObject/Public/DObject/AssetPath.h`
- `Engine/Source/Runtime/CoreDObject/Public/DObject/ObjectPtr.h`
- `Engine/Source/Runtime/CoreDObject/Public/DObject/WeakObjectPtr.h`
- `Engine/Source/Runtime/CoreDObject/Public/DObject/DObjectGlobals.h`
- `Engine/Source/Runtime/CoreDObject/Public/DObject/DurinPropertyTypes.h`
- `Engine/Source/Runtime/CoreDObject/Private/DObject/Property.cpp`
- `Engine/Source/Runtime/CoreDObject/Private/DObject/Archive.cpp`
- `Engine/Source/Runtime/CoreDObject/Private/DObject/GCReferenceSchema.cpp`
- `Engine/Source/Programs/DurinHeaderTool/durin_header_tool/parser/reflection_parser.py`
- `Engine/Source/Programs/DurinHeaderTool/durin_header_tool/writers/reflection_source_writer.py`
- `Engine/Source/Runtime/AssetCore/Public/AssetSystem.h`
- `Engine/Source/Runtime/AssetCore/Private/AssetSystem.cpp`
- `Engine/Source/Runtime/AssetCore/Private/AssetCompatibility.cpp`
- `Engine/Source/Editor/DurinEd/Public/Editor/EditorAssetPicker.h`
- `Engine/Source/Editor/DurinEd/Private/Editor/EditorAssetPicker.cpp`
- `Engine/Source/Editor/DurinEd/Private/Editor/ReflectedPropertyView.cpp`
- `Engine/Source/Editor/LevelEditor/Private/Widgets/MLevelEditor.cpp`
- `Engine/Source/Editor/LevelEditor/Private/Assets/EditorAssetMoveCoordinator.cpp`
- `Engine/Tests/Native/CoreDObjectTests/Private/ReflectionTypeTests.cpp`
- `Engine/Tests/Native/AssetCoreTests/Private/PackageTests.cpp`
