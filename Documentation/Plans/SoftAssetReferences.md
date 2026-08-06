# Soft Asset References Plan

Summary: Add typed persistent asset paths with non-owning loaded-object caches, explicit resolution, reflected serialization, registry indexing, and editor workflows without turning soft references into eager package dependencies.

Last reviewed: 2026-08-06

Status: Completed
Completed: 2026-08-06

## Current Status

- Stage 5 is complete. The final repository audit found no additional
  production owner that should be converted mechanically: reflected
  `TObjectPtr` fields remain hard dependencies, `TWeakObjectPtr` sites remain
  transient observations, and remaining asset paths/manual loads belong to
  explicit document, import/source, thumbnail, open, or runtime-resource
  boundaries. The default level remains the selected persistent soft-path
  migration.
- `FSoftObjectPath`, `FSoftObjectPtr`, `TSoftObjectPtr<T>`, reflected
  `FSoftObjectProperty`, explicit typed resolution/loading, bounded Archive and
  DAST persistence, rebuildable indexing, Cook traversal, target-move repair,
  and editor path assignment now form one documented contract.
- Per the simplified deletion decision, delete analysis, tokens, revalidation,
  Undo, and Redo remain hard-reference-only. Deleting a soft target preserves
  source bytes as a dangling path; explicit load and Cook report the missing
  target at their existing boundaries.
- The project default level is stored in editor/runtime memory as
  `TSoftObjectPtr<DLevel>`, remains a canonical path string in YAML, assigns
  without loading, opens through explicit typed load, publishes settings
  atomically, and follows target moves through a reversible external-store
  action. Other hard, weak, import/source, document/navigation,
  thumbnail, and built-in resource owners retain their recorded semantics.
- Final validation passes DHT 186/186, `AssetPackageTests` 54/54,
  `CoreObjectTests` 70/70, `AssetCookTests` 12/12,
  `EditorPropertyTests` 27/27, `EditorAssetWorkflowTests` 51 plus one
  privilege-dependent skip, and `WorldTests` 62/62. The full `all` build and
  all-native run pass 1,020 tests with zero failures, two expected skips, and
  one disabled benchmark. The previously recorded new-level baseline test no
  longer reads the mutable Sandbox project: it now authors, saves, unloads, and
  reconstructs an isolated test level with one stable Engine mesh dependency.
- Completion history is intentionally squashed against pre-feature baseline
  `5a569f31`; the stage handoffs below retain the implementation decisions and
  validation evidence without requiring intermediate commits.
- Archive and DAST v2 persist the frozen bounded null/path payload for direct,
  fixed-array, Array, and Map-value soft fields; weak-cache state never enters
  bytes or hard package dependencies.
- Package inspection extracts typed soft occurrences without constructing owner
  objects, invoking `PostLoad`, resolving targets, or changing residency. The
  registry publishes separate sorted target-to-referencer and deduplicated
  source-to-target queries.
- `AssetRegistry/SoftReferences.bin` is an independent schema-1 derived cache
  keyed by complete size/time/content-hash package fingerprints plus extractor
  schema 1. Cache miss, incompatible/corrupt bytes, full validation, content
  change, save, source move, and source deletion rebuild or invalidate affected
  entries without publishing partial sources.
- Default Cook reachability traverses hard dependencies plus indexed soft
  targets and validates missing/type-mismatched targets. Runtime package load,
  unload guards, and `FAssetData::Dependencies` remain hard-edge-only.
- Reflected `TObjectPtr<T>` fields are hard references. Cross-package values are
  serialized as asset paths, added to the package dependency table, loaded
  eagerly, retained by the loaded-package graph, and required to resolve.
- `TWeakObjectPtr<T>` is a non-owning handle to an already loaded object. It is
  not a persistent asset identity and is not recognized by DHT as a property.
- `FAssetPath` and plain strings provide unloaded identity in import records,
  thumbnails, documents, operations, and service code whose owning subsystem
  intentionally controls loading, move, and failure behavior.
- `FEditorAssetPickerConfig` preserves loaded-object assignment for hard fields
  and exposes a separate registry-path assignment mode for soft/path owners.
- [Reflected Struct Operations](ReflectedStructOperations.md),
  [Typed Struct Property Registration](TypedStructPropertyRegistration.md), and
  [Reflected Container Operations](ReflectedContainerOperations.md) are
  complete. Soft-object registration consumes their typed property,
  capability-aware value, and recursive container descriptor contracts.
- This plan is independent of DAST v3 compact encoding and does not reactivate
  the [Compact Asset Serialization Roadmap](../Roadmaps/CompactAssetSerialization.md).

## Goal

Provide one typed soft asset-reference abstraction that persists a canonical
main-asset path, optionally caches a non-owning handle to an already loaded
object, and resolves or loads only when explicitly requested. Soft references
must participate in reflection, property editing, package inspection, move
repair, dangling deletion behavior, cooking reachability, snapshots, and authored serialization
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
- Use the soft-reference index for referencer queries, asset move repair, cook
  reachability, and editor navigation. Deletion transactions intentionally do
  not consume it in version one.
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
- Deletion analysis and transactions ignore soft references completely. Soft
  paths may dangle by design; explicit load and Cook diagnose a missing target
  at their normal boundaries. Any future warning UI must remain outside the
  deletion token, revalidation, Undo, and Redo contracts.
- Project/editor settings that adopt `FSoftObjectPath`, including the default
  level, participate in the same move-coordination contract even when their
  storage is YAML rather than a DAST field.

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

- Stage 0 completed as documentation, inventory, and characterization work and
  changed no production API or serialized content.
- Struct operations, typed struct registration, and reflected container
  operations are established baselines. Completion history is intentionally
  squashed against `5a569f31`; the stage handoffs document how each later stage
  consumed the preceding contract while extending the existing typed-layout
  dispatch and recursive container descriptor model.
- Stages 1 through 4 are single-writer migrations across CoreDObject, DHT,
  AssetCore, and editor consumers. They must not overlap another migration that
  edits the same working set in one checkout.
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

## Stage 0 Frozen Contract and Audit

### Baseline and Reference-Model Check

- Stage 0 audited baseline `5a569f31` (`feat(reflection): complete container
  operations migration`). The checkout was clean, and the completed StructOps,
  typed struct registration, and container-operation handoffs had no open
  questions.
- The audit found 26 reflected `TObjectPtr` fields, 11 production
  `TWeakObjectPtr` use sites outside the wrapper implementation, 26 non-test
  asset-load call sites outside the AssetCore load implementation, and one
  `CurrentSelectionPath` owner: the project default-level picker.
- Unreal Engine was used only as a reference-model check. Its
  [`FSoftObjectPtr`](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/CoreUObject/FSoftObjectPtr)
  likewise combines an on-disk path with weak loaded state and does not affect
  garbage collection, while its
  [`FAssetRegistryDependencyOptions`](https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/AssetRegistry/FAssetRegistryDependencyOptions)
  distinguish hard package references that must load with the owner from soft
  package references that need not. Durin retains that separation.
- Unreal Engine's
  [`FSoftObjectPath`](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/CoreUObject/FSoftObjectPath)
  also supports subobjects and an `Untracked` cook opt-out. Durin version 1
  intentionally diverges: it targets only package main assets and every
  reflected soft reference contributes cook reachability.

### Hard and Weak Reference Inventory

Every reflected hard field has an immediate ownership, residency, or runtime
resource reason. None is a Stage 0 soft-migration candidate.

| Classification | Audited fields | Decision |
| --- | --- | --- |
| Package and object-graph ownership | `DPackage::Asset`; `DLevel::Actors`, `PrimaryCameraActor`; `AActor::RootComponent`, `OwnedComponents`, `InstanceComponents`; `AStaticMeshActor::StaticMeshComponent`; `ASkyBoxActor::SkyBoxComponent`; `ADirectionalLightActor::LightComponent`; `ACameraActor::CameraComponent`; `DSceneComponent::AttachParent`, `AttachChildren` | Retain hard. These are inner objects or structural graph edges, not independently loadable package-main-asset identities. |
| Active render resources | `DMaterialInstance::Parent`; `FMaterialParameterValue::TextureValue`; `FStaticMeshMaterialSlot::DefaultMaterial`; `DStaticMeshComponent::StaticMesh`, `OverrideMaterials`; `DSkyBoxComponent::TextureCube` | Retain hard. `PostLoad`, proxy construction, and active component rendering require the targets immediately. Lazy conversion requires a separate streaming and fallback lifecycle. |
| Explicit transient residency | `DTextureCubePreviewComponent::TextureCube`; `DEditorEngine::EditorWorld`, `PlayWorld`, `EditorLevel`, `RetiredPlayWorlds`, `RetiredPlayLevels`; `DWorld::CurrentLevel`; `DEngine::MainWorld` | Retain hard and transient. These fields deliberately keep active or render-fenced objects alive. |

The 11 weak use sites are actor/component customization targets, three spline
editor callback caches, and viewport observations of a level or selected
component. They are transient observations of already loaded objects, have no
persistent asset identity, and remain `TWeakObjectPtr`. No reflected weak field
exists.

### Path, Picker, and Manual-Load Inventory

| Domain | Current storage and load behavior | Decision |
| --- | --- | --- |
| Project default level | `MLevelEditor` owns `DefaultLevel` and `PendingDefaultLevel` strings; `DGameEngine` reparses and explicitly loads the setting; the move coordinator rewrites the string and YAML separately. | Migrate the identity to `TSoftObjectPtr<DLevel>` while preserving YAML as a canonical path string. Runtime/editor open remains an explicit load. |
| Shared picker | `CurrentSelectionPath` displays an unloaded identity, but the single `AssignSelection(DObject*)` callback forces every selected registry candidate through `LoadAsset`. | Add a distinct path-assignment mode. Keep object-assignment mode and its load for hard properties. |
| Registry/package/move/delete data | `FAssetData`, package headers and inspections, move pairs, delete tokens, and destination validation use `FAssetPath` as domain identity. | Retain `FAssetPath`; these records describe packages or operations rather than object-reference values. |
| Import and source tracking | Import outputs, record paths, primary outputs, source-reference indices, relocation snapshots, tombstones, and management-owner fields use `FAssetPath` plus policy/class/provenance data. | Retain their domain types. Heterogeneous import ownership and historical tombstones are not typed object-reference fields. They may consume common query/load helpers later. |
| Editor documents and navigation | Pending level opens, workspace requests, selected/reveal paths, content-browser operations, and asset destinations are short-lived command or UI identities. | Retain strings or `FAssetPath`; explicit user open is the correct load boundary. |
| Thumbnail pipeline | Request fingerprints, scheduler keys, dependency nodes, upload results, and generation inputs use immutable `FAssetPath` values; cold jobs load explicitly. | Retain `FAssetPath`. The path is a cross-thread job key, and the pipeline owns separate residency and cancellation state. |
| Built-in runtime resources | Default material and studio environment use fixed paths, load during subsystem startup, and the default material is an explicit cook root and GC root. | Retain explicit hard startup loading. These are required resources rather than optional authored references. |

The 26 audited load call sites group into ten explicit editor open/preview
operations, ten import/source-management operations, two thumbnail cold loads,
three runtime startup/default loads, and the shared picker. Only picker loading
performed for a path owner is removed. The game/editor default-level load is
retained but routed through the explicit typed soft-load API.

### Selected Migration and Deferral Decisions

- The first production owner is the optional project default level. It is a
  typed `DLevel` identity, is allowed to be missing without invalidating project
  settings, loads only when the editor opens it or game startup activates it,
  contributes a project cook root, and must follow asset moves.
- The shared picker path callback is the first workflow migration. It is a seam,
  not an asset owner: registry discovery and class filtering yield an
  `FAssetPath`, and path mode never loads merely to assign it.
- Stage 2 and Stage 3 introduce direct, fixed-array, Array, and Map-value soft
  fields in focused fixtures. No current production reflected `TObjectPtr`
  field is converted merely to exercise the property kind.
- Import outputs remain deferred because one record may describe heterogeneous
  asset classes and managed, referenced, or detached lifecycle policy. A later
  import-specific plan may adopt `FSoftObjectPath` for only the referenced
  subset after preserving output ownership and tombstone behavior.
- Inactive editor tabs, thumbnails, built-in defaults, material/mesh resources,
  and active components do not become soft merely to reduce residency. Each
  requires its own unload, fallback, cancellation, or streaming contract first.

### Version-1 Core Value Contract

- Public names are `FSoftObjectPath`, `FSoftObjectPtr`, and
  `TSoftObjectPtr<T>`, declared by CoreDObject. `T` must derive from `DObject`.
  No implicit string constructor is provided.
- `FSoftObjectPath` owns one `FAssetPath`. Empty storage is null; non-null
  storage is already canonical, names a registered-mount main asset, and has no
  filesystem-existence requirement. `TryCreate` follows `FAssetPath`
  validation, while construction from an existing `FAssetPath` cannot fail.
- `FSoftObjectPtr` owns an `FSoftObjectPath` and one `FWeakObjectPtr` cache.
  `TSoftObjectPtr<T>` uses composition and typed access; its physical layout is
  not a reflection ABI and generic code uses emitted accessors.
- Default construction, `nullptr`, and `Reset` clear both path and cache.
  Assigning a path stores it without registry lookup or loading and clears the
  cache. Assigning the same path follows the same deterministic cache-clear
  rule.
- Checked object assignment accepts null or a package main asset only. It
  rejects packages, inner objects, transient/unpackaged objects, invalid package
  paths, and objects that fail the typed `IsA<T>` constraint. Success captures
  the package path and weak handle atomically; failure leaves both old path and
  old cache unchanged and returns `false` plus an optional diagnostic.
- `IsNull` observes only the path. `Get` and `IsLoaded` inspect only the weak
  cache, verify that the live object is still the main asset at the stored path,
  and apply the typed class check; they never access the filesystem or registry.
- Equality, ordering where required, hashing, snapshots, and serialization use
  the path only. Cache population cannot make an edit, change a hash, dirty an
  owner, or alter Undo/Redo state.
- Copy preserves the path and current weak handle. Move transfers both and
  leaves the source null. Collection or unload invalidates only the weak handle;
  path identity survives and may be resolved again.
- Path-only operations and independent copies may cross threads. Mutating one
  instance still requires external synchronization. Object assignment, `Get`,
  cache refresh, loaded-only resolution, and synchronous loading retain
  `FWeakObjectPtr`'s game-thread requirement.

### CoreDObject and AssetCore API Boundary

- CoreDObject exposes value construction, checked path/object assignment,
  cache-only inspection, and reflection accessors. It includes no AssetCore
  header, registry query, filesystem call, loader callback, or global loading
  hook.
- AssetCore exposes `ResolveSoftObject` and `LoadSoftObject` operations plus
  typed `TSoftObjectPtr<T>` wrappers. Both take an explicit
  `ESoftObjectNullPolicy::{Reject,Allow}`; the default is `Reject` so accepting a
  null result is visible at the call site.
- Loaded-only resolution returns a checked result record containing
  `ESoftObjectResolveState::{Null,NotLoaded,Loaded}`, an `FAssetResult`, and the
  resolved object. `Null` is success only under `Allow`; `NotLoaded` is a
  successful non-I/O observation, not `EAssetError::NotFound`.
- Loaded-only resolution first accepts a still-valid cache, otherwise consults
  `FindLoadedPackage` and its main asset. It may use registry class metadata for
  an early mismatch but never loads, scans mounts, or probes the filesystem.
- Explicit load delegates to the existing AssetCore package load path, validates
  both registry and runtime class where available, and refreshes the weak cache
  only after path, main-asset identity, and type checks succeed.
- Invalid path returns `InvalidPath`; missing target returns `NotFound`; wrong
  registry or runtime class returns `TypeMismatch`; package/dependency/corruption
  failures retain their existing `FAssetResult` codes. No failure clears or
  replaces a previously valid path/cache.
- Core path assignment accepts a syntactically valid missing target. Editor
  candidate assignment checks registry metadata without loading; paste accepts
  a missing canonical path but rejects a known registry type mismatch. Only an
  explicit resolve/load establishes loaded state.

### Reflection and DHT Contract

- Add `EPropertyGenFlags::SoftObject`, `EPropertyParamLayout::SoftObject`, a
  distinct field cast flag, `FSoftObjectProperty`, and
  `FSoftObjectPropertyParams`. A flag on `FObjectProperty` is not used.
- `FSoftObjectPropertyParams` fixes the kind/layout and contains common field
  schema, a required expected-class resolver, and either an offset or paired
  mutable/const soft-value accessors. It carries no hard-object-wrapper bit,
  struct lifecycle table, or generic layout placeholders.
- `FSoftObjectProperty` exposes typed logical-value access and path copy/assign
  operations. Direct, accessor-backed, fixed-array, Array-inner, and Map-value
  descriptors share the same expected-class contract.
- DHT accepts only `TSoftObjectPtr<T>` and `Durin::TSoftObjectPtr<T>` where `T`
  resolves to a reflected `DObject` class. It accepts direct fields, existing
  fixed arrays, and values recursively nested under supported default-form Array
  and Map descriptors up to the established four-container limit.
- Raw `FSoftObjectPtr`, wrapper aliases, wrapper pointers/references, const or
  volatile wrappers, unresolved/non-object targets, soft Map keys, and
  unsupported container forms fail in DHT with source-qualified diagnostics.
- DHT emits the existing capability-aware container element operations for
  `TSoftObjectPtr<T>`. Map-value soft references are supported; Map keys are
  rejected even though path hashing exists, because move repair must not mutate
  key identity in place.
- Soft properties never enter GC reference schemas, strong-reference discovery,
  or package unload-blocker analysis. Their weak cache is not a hidden struct or
  container strong reference.

### Archive, Snapshot, and DAST v2 Contract

- Archive, reflected snapshots, copy/paste payloads, and authored DAST serialize
  only null/path identity. Weak handles, loaded state, object addresses, registry
  records, and package pointers never enter bytes.
- The DAST v2 serialized type signature is
  `SoftObject:<ExpectedQualifiedClass>:v1`. Existing recursive signatures wrap
  it unchanged, for example `Array<SoftObject:<Class>:v1>` and
  `Map<...,SoftObject:<Class>:v1>`.
- One soft value payload uses the existing DAST v2 scalar encoding:
  `uint8 0` and no remaining bytes for null, or `uint8 1`, a `uint64` UTF-8 byte
  count, and exactly that canonical path for non-null. Fixed-array values are
  concatenated in declared order; dynamic Array and Map retain their existing
  count and canonical-entry grammars.
- A path string is bounded by the existing `MaximumPackageStringBytes` of
  1 MiB, so a direct non-null payload is at most 1,048,585 bytes. Unknown tags,
  truncation, overlong strings, malformed bytes, and trailing payload bytes are
  `CorruptFile`; a decoded non-canonical path is `InvalidPath`; signature/class
  mismatch is `TypeMismatch`.
- Saving never validates target existence, serializes a cache, or inserts a
  soft path into `FPackageFile::Dependencies`. The DAST v2 header, dependency
  count, and dependency entries are byte-for-byte unchanged; no version bump or
  DAST v3 claim is made.
- Loading a valid path does not resolve it. A missing or unloaded target remains
  stored, the cache starts empty, and the owner package succeeds. An older
  package that omits a newly added field receives the C++ default/null value;
  unknown-field preservation and compatibility-risk handling remain active.
- Package inspection gains bounded readers for direct and recursively nested
  soft payloads. Inspection and index extraction do not construct owner objects,
  invoke `PostLoad`, query target files, or load target packages.

### Derived Soft-Reference Index Contract

- `FAssetRegistry` owns a separate soft-reference projection. Hard
  `FAssetData::Dependencies` remains unchanged. Public queries expose sorted
  target-to-referencer records and deduplicated source-to-soft-target paths; no
  soft query changes `IsPackageReferenced`.
- Each occurrence records source package, `FAssetPackageFingerprint`, source
  object id/class, declaring type, top-level field name, expected class, target
  path, and a typed container route. Routes distinguish direct/fixed-array
  indices, Array indices, and Map values identified by the existing canonical
  key token; a deterministic display path is derived from the route.
- Null values emit no record. Repeated target paths in different elements emit
  separate occurrence records so move rewrite can verify every location.
- Authoritative identity remains the DAST field payload. The derived cache lives
  under the AssetRegistry derived-data directory as `SoftReferences.bin`, has
  its own magic/schema version, and keys each source entry by the complete
  size/time/content-hash `FAssetPackageFingerprint` plus the extractor schema
  version.
- Save, move, delete, registry add/remove, full rescan, package fingerprint
  change, or extractor schema change invalidates the affected source entries.
  Cache miss or corruption discards the cache and re-extracts authoritative
  package bytes; cache failure is never authored-data loss.
- Extraction accepts at most four container levels, 100,000 soft occurrences
  per package, 1,000,000 occurrences in one persisted snapshot, a 1 MiB target
  path, and a 4 KiB derived display path. Exceeding a bound reports a deterministic
  index diagnostic and does not publish a partial source entry.
- Published records sort lexically by target, source, object id, declaring type,
  field, and container route. Extraction during the version-1 synchronous
  registry scan may read package bytes and reflection metadata but performs no
  object construction, `PostLoad`, render work, target registry scan, or target
  load.

### Cook, Move, Delete, and Editor Policies

- Every non-null reflected soft path contributes cook reachability by default.
  No `Untracked`, editor-only, optional-cook, or metadata opt-out is added in
  version 1 because the audit found no production case requiring one. Missing
  or type-mismatched reachable targets fail cook, while soft cycles terminate
  through the normal visited set and do not imply runtime eager-load cycles.
- `BuildCookReachability` accepts explicit roots. The repository does not yet
  have a complete project packaging driver, so Stage 4 keeps the default level
  as the canonical typed root source rather than adding a process-global mutable
  root registry. A future project discovery driver must include the non-null
  default level; built-in material roots retain their existing separate policy.
- Move planning requires a complete, current soft index. Loaded referencers are
  verified through live reflected values and saved from memory; unloaded
  referencers are fingerprint-checked and rewritten through parsed tagged
  payloads. No blind string or byte replacement is permitted.
- Target bytes, every rewritten referencer, loaded soft-value/cache changes,
  registry/index publication, and registered external stores form one staged
  operation. External stores such as project YAML contribute reversible
  snapshot/apply/rollback work without making AssetCore depend on editor code.
  Collision, stale fingerprint, read-only source, parse failure, or publication
  failure leaves all paths, files, loaded values, settings, and indices at the
  old identity.
- Deletion transactions intentionally ignore the soft-reference index. Hard
  referencers retain existing blocker behavior, while soft paths remain authored
  unchanged and may become dangling. Explicit resolution then reports a missing
  target and a Cook root reaching that path fails its normal missing-target
  validation. Optional deletion-time diagnostics are deferred outside the
  transaction rather than widening its token, revalidation, or rollback state.
- The picker adds mutually exclusive object-assignment and path-assignment
  callbacks. Path mode receives registry identity, applies class/prefix filters,
  supports null, and performs no `LoadAsset`. Hard object properties retain
  object mode.
- Reflected soft fields display null, unloaded, loaded, missing, and known
  type-mismatch states. Reveal uses the path; Load/Open is explicit; clear and
  path paste participate in the existing proposal, Undo/Redo, dirty, and save
  pipeline. Cache-only changes produce no edit event.

## Implementation Stages

### Stage 0: Audit Uses and Freeze Soft-Reference Semantics

- [x] Inventory reflected hard object fields and classify whether each is
  required at owner load, optional/lazy, editor-only, or currently ambiguous.
- [x] Inventory `FAssetPath` and asset-path string storage, manual `LoadAsset`
  calls, `CurrentSelectionPath` users, registry-only lookups, and move/delete
  fixups; distinguish object references from source/provenance/document paths.
- [x] Record the first migration candidates and explicitly list hard references
  that must not be converted.
- [x] Freeze the public type names, null state, path ownership, weak-cache
  behavior, equality/hash semantics, game-thread restrictions, and assignment
  failure rules.
- [x] Freeze the CoreDObject/AssetCore API boundary for loaded-only resolve and
  explicit synchronous load.
- [x] Freeze `SoftObject` property metadata, expected-class resolution, DHT
  accepted spellings, unsupported forms, and nested-container policy.
- [x] Freeze the DAST v2 type signature, payload grammar, bounds, compatibility
  behavior, and proof that the hard dependency header layout remains unchanged.
- [x] Define the derived soft-reference index schema, fingerprint/cache
  ownership, invalidation rules, corruption recovery, and extraction budgets.
- [x] Freeze move rewrite, deletion-warning, default cook reachability, missing
  target, and type-mismatch policies.
- [x] Characterize current default-level picker selection and prove where it
  loads solely to recover a path.
- [x] End the stage with a handoff listing the baseline commit, inventory,
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

#### Stage 0 Handoff

- Baseline commit: `5a569f31` (`feat(reflection): complete container operations
  migration`). Stage 0 changed only this plan. The next-stage implementation
  working set is CoreDObject soft values/property metadata, DHT parsing and
  generated registration, AssetCore resolution/serialization/inspection/index
  and move/delete paths, the shared picker/reflected property editor, default-
  level settings and runtime loading, and focused native/DHT/editor fixtures.
- Inventory: all 26 reflected hard fields are classified and retained; all 11
  weak use sites remain transient observations; 26 non-test asset-load call
  sites and path-backed storage domains were grouped. The default-level owner
  and shared picker path mode are the only selected production migrations.
- Frozen symbols and formats: `FSoftObjectPath`, `FSoftObjectPtr`,
  `TSoftObjectPtr<T>`, `FSoftObjectProperty`, `FSoftObjectPropertyParams`,
  `EPropertyGenFlags::SoftObject`, `EPropertyParamLayout::SoftObject`,
  `ESoftObjectNullPolicy`, `ESoftObjectResolveState`, `ResolveSoftObject`,
  `LoadSoftObject`, the `SoftObject:<ExpectedQualifiedClass>:v1` signature, and
  the null/path payload grammar above.
- Decisions: path identity is canonical and persistent; cache state is weak,
  game-thread-only, and logically invisible; resolve and load are explicit
  AssetCore operations; soft fields are excluded from GC and hard dependencies;
  Map values but not keys are supported; index data is bounded derived data;
  reflected soft edges cook by default; move repair is atomic; deletion remains
  hard-reference-only and leaves dangling soft paths; project default-level
  YAML participates as an external reversible store. The deletion decision was
  simplified before Stage 4 implementation.
- Open questions: none. UE reference behavior confirmed the selected persistent-
  path/weak-state and hard/soft dependency split; subobjects, async streaming,
  cook opt-outs, import-output adoption, and resource residency remain explicit
  non-goals or deferred follow-ups.
- Validation: targeted source searches and direct code inspection verified the
  inventory, picker-time `LoadAsset`, default-level YAML/runtime flow,
  fingerprinted package inspection, hard-only move/delete/unload behavior, and
  completed reflection/container baselines. Plan validation passed; no build or
  runtime test was required because Stage 0 changed no production source or
  serialized fixture.

### Stage 1: Add Core Soft Path and Pointer Values

Dependencies: Stage 0 contract and handoff.

- [x] Add the selected `FSoftObjectPath`, `FSoftObjectPtr`, and
  `TSoftObjectPtr<T>` declarations in CoreDObject without an AssetCore include or
  loader callback, implicit string construction, or exposed wrapper-layout ABI.
- [x] Implement null/path/object assignment, reset, path access, weak-cache
  access, loaded-state queries, logical equality, hashing, copy, and move.
- [x] Validate assignment from a main asset and reject inner, transient,
  package, invalid-path, and wrong-type objects without destination mutation.
- [x] Add `ESoftObjectNullPolicy`, `ESoftObjectResolveState`, the checked
  loaded-only resolve result, and AssetCore `ResolveSoftObject`/
  `LoadSoftObject` APIs with typed wrappers and stable result mapping.
- [x] Refresh caches only after path and type validation; preserve path/cache on
  failed load according to the frozen contract.
- [x] Verify target unload/collection invalidates the weak cache without erasing
  path identity or blocking the unload.
- [x] Add focused CoreDObject/AssetCore tests for null, valid, missing, unloaded,
  already-loaded, wrong-type, copy/move, equality/hash, unload/reload, and
  game-thread constraints.
- [x] End the stage with a handoff listing the baseline commit, working set,
  public symbols, ownership decisions, result codes, and focused test results.

#### Acceptance Gate

- CoreDObject soft values compile and operate without depending on AssetCore.
- No non-loading operation performs file I/O or package loading.
- Explicit load is type-safe, refreshes only the cache, and preserves logical
  path identity through unload/reload.
- Soft references add no GC edge and do not prevent target-package unload.
- Invalid assignment/load cases return stable failures and do not partially
  mutate an existing valid value.

#### Stage 1 Handoff

- Baseline commit: pre-feature `5a569f31`; the completion squash incorporates
  the Stage 0 contract with this implementation. The working set was the new CoreDObject soft-value header and
  source, the CoreDObject umbrella/forward declarations, AssetCore's public
  system API and implementation, and the existing CoreDObject/AssetCore native
  test fixtures.
- Public CoreDObject symbols are `FSoftObjectPath`, `FSoftObjectPtr`, and
  `TSoftObjectPtr<T>`. Logical identity, comparison, ordering, and hashing use
  only the canonical `FAssetPath`; `FWeakObjectPtr` is a non-owning cache.
  `TrySetObject` may replace path and cache after full main-asset validation,
  while `TrySetLoadedObject` may refresh only a matching existing path.
- Public AssetCore symbols are `ESoftObjectNullPolicy`,
  `ESoftObjectResolveState`, `FSoftObjectResolveResult`,
  `TSoftObjectResolveResult<T>`, `ResolveSoftObject`, and `LoadSoftObject`.
  Resolve uses only the weak cache, in-memory registry metadata, and the loaded-
  package map. Load alone enters the existing package-loading path.
- Ownership and thread decisions: CoreDObject has no AssetCore include or load
  callback; soft caches add no GC edge or unload blocker. Path-only operations
  and independent copies may cross threads. Object assignment, cache refresh,
  loaded inspection, resolve, and synchronous load retain the game-thread
  check.
- Result mapping: rejected null and invalid identity use `InvalidPath`; known
  registry/runtime class mismatches use `TypeMismatch`; unknown registered
  classes use `UnknownClass`; missing files use `NotFound`; package and
  dependency failures preserve their existing `FAssetResult` code. Successful
  unloaded observation is `NotLoaded`, not an error. Every failure leaves the
  reference path/cache unchanged.
- Validation: `CoreObjectTests` passed 69/69 and `AssetPackageTests` passed
  42/42 under `Win64-Debug-DurinEditor-Tests`. Coverage includes null, invalid
  path, valid/missing/unloaded/already-loaded references, wrong types, main-
  asset validation, copy/move, logical equality/hash, worker-carried copies,
  game-thread enforcement, collection, unload, and reload. No full editor build
  was required because Stage 1 changes no user-visible editor behavior.
- Open questions: none. Stage 2 can treat the Core values and AssetCore API as
  stable and add typed reflection/accessors without depending on their physical
  layout.

### Stage 2: Add Typed Reflection, DHT, and Property Editing

Dependencies: Stage 1 and the completed Reflected Struct Operations, Typed
Struct Property Registration, and Reflected Container Operations handoffs.

- [x] Add the `SoftObject` generation flag, runtime property class, cast flag,
  typed parameter record, construction dispatch, and descriptor validation.
- [x] Add typed mutable/const value access for direct and accessor-backed soft
  properties without exposing wrapper layout to generic consumers.
- [x] Teach DHT parsing and symbol resolution to recognize valid direct,
  fixed-array, nested Array, and Map-value soft declarations.
- [x] Emit concise typed soft-property metadata with expected-class resolution
  and no positional hard-object or generic lifecycle placeholders.
- [x] Add deterministic source-qualified diagnostics for raw wrappers,
  non-object target types, unresolved classes, unsupported qualifiers, soft Map
  keys, and excessive nesting.
- [x] Exclude soft properties from GC schema assembly and hidden-strong-reference
  accounting.
- [x] Extend property equality, snapshots, copy construction/assignment, change
  paths, and detached value storage to compare/copy the logical path while
  ignoring cache state. User-facing path clipboard actions remain part of the
  later editor workflow integration.
- [x] Extend the shared asset picker and reflected property view with path-only
  assignment, unloaded/broken state, clear, reveal, and explicit load actions.
- [x] Add DHT exact-output/negative tests and CoreDObject/editor-model tests for
  direct, nested, null, loaded, unloaded, missing, typed, and Undo/Redo cases.
- [x] End the stage with a handoff listing the baseline commit, descriptor ABI,
  DHT diagnostics, generated fixtures, editor behavior, and validation results.

#### Acceptance Gate

- Direct, fixed-array, and supported nested `TSoftObjectPtr<T>` fields register
  as `FSoftObjectProperty` with the correct expected class and no strong GC
  schema.
- Unsupported declarations fail in DHT rather than in generated C++ or runtime
  registration.
- Generated metadata is concise, type-safe, and byte-deterministic.
- The editor can select and clear a soft field without loading the selected
  asset; explicit load remains a separate visible action.
- Logical edits, snapshots, Undo/Redo, and equality remain unchanged when only
  weak-cache state changes.

#### Stage 2 Handoff

- Baseline commit: pre-feature `5a569f31`; the completion squash incorporates
  the preceding soft-value contract. The working set covered CoreDObject property registration/runtime access and
  snapshots, DHT parsing/writing/fixtures, the DurinEd picker/property view,
  LevelEditor host callbacks, and focused Core/editor tests.
- Descriptor ABI: `EPropertyGenFlags::SoftObject`,
  `EClassCastFlags::FSoftObjectProperty`, and
  `EPropertyParamLayout::SoftObject` are distinct append-only values.
  `FSoftObjectPropertyParams::Create<T>` and `WithAccessors<T>` fix the kind,
  typed wrapper conversion, lifecycle/copy operations, and expected-class
  resolver; generated C++ does not spell generic lifecycle or hard-object
  placeholders. Runtime construction validates the typed layout before creating
  `FSoftObjectProperty`.
- Value access and ownership: runtime consumers reach `FSoftObjectPtr` through
  typed mutable/const wrapper converters for direct, accessor-backed, static-
  array, Array-inner, and Map-value storage. Equality, detached copies, and
  snapshots use only `FSoftObjectPath`; snapshot reference roots stay empty.
  GC schema compilation intentionally has no SoftObject operation, so a live
  weak cache does not retain its target.
- DHT contract: direct and qualified `TSoftObjectPtr<T>`, fixed arrays, nested
  Arrays, and Map values are supported through depth 4. Stable failures are
  `DHT-SOFT001` (raw/untyped wrapper), `DHT-SOFT002` (alias/unsupported direct
  spelling), `DHT-SOFT003` (wrapper or target qualifiers/pointer/reference),
  `DHT-SOFT004` (non-object target), `DHT-SOFT005` (unresolved class), and
  `DHT-SOFT006` (Map key); excessive nesting remains `DHT-CONT005`.
- Editor behavior: the shared picker has separate loaded-object and asset-path
  assignment modes. Reflected soft rows display null, unloaded, loaded, missing,
  and type-mismatch states, retain broken paths, clear transactionally, reveal
  registry entries, and load only through the explicit action. Open is available
  only for an already loaded target. Hard object rows retain eager object
  assignment. The default-level picker now uses path mode, while its storage and
  move-repair migration remain Stage 4.
- Stage boundary: `CapturePropertyValue`/`RestorePropertyValue` use a path-only
  snapshot branch. Persistent Archive and DAST use return an explicit unsupported
  error for SoftObject until Stage 3 adds the frozen payload, inspection, hard-
  dependency exclusion, and compatibility rules.
- Validation: DHT passed 186/186; `CoreObjectTests` passed 70/70;
  `EditorPropertyTests` passed 27/27; `EditorShellTests` passed 27/27; and
  `DevTool build --target all --agent` completed under
  `Win64-Debug-DurinEditor-Tests`. Coverage includes exact generated output,
  negative diagnostics, direct/accessor/fixed/Array/Map access,
  cache-insensitive equality/snapshots, no GC retention, detached copy, state
  inspection, explicit load, path-only assignment, and Undo/Redo.
- Open questions: none. Stage 3 should preserve the descriptor ABI and extend
  only persistent Archive/DAST/inspection and derived-index behavior.

### Stage 3: Serialize Soft Fields and Build the Reference Index

Dependencies: Stage 2 property metadata and value access.

- [x] Extend the Stage 2 path-only snapshot branch with bounded persistent
  Archive serialization of null/path soft values, sticky nested errors, and no
  implicit resolution.
- [x] Add the frozen `SoftObject:<ExpectedQualifiedClass>:v1` signature,
  null/path DAST v2 payload writer, reader, compatibility inspection, and
  malformed-input diagnostics.
- [x] Confirm serialization never writes weak handles or adds soft paths to the
  hard `Dependencies` collection.
- [x] Add package-inspection support that extracts soft path, expected class,
  declaring type, property path, and container context without object creation.
- [x] Implement the fingerprinted rebuildable soft-reference index,
  `SoftReferences.bin` cache schema, frozen extraction bounds, invalidation,
  cache corruption fallback, and deterministic ordering.
- [x] Expose target-to-referencer and source-to-soft-target queries separately
  from hard dependency queries.
- [x] Integrate default soft cook reachability without changing runtime eager
  load or unload-blocking decisions.
- [x] Add fixtures for old packages without soft fields, valid unresolved paths,
  wrong registered/runtime types, unknown fields, truncated/overlong payloads,
  cache miss/corruption, and no-target-load index scans.
- [x] Verify saving/loading an owner package with an unloaded or missing soft
  target succeeds and preserves exact logical identity.
- [x] End the stage with a handoff listing the baseline commit, payload contract,
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

#### Stage 3 Handoff

- Baseline commit: pre-feature `5a569f31`; the completion squash incorporates
  the preceding reflected-property contract. The working set covered CoreDObject Archive property
  serialization; AssetCore DAST signatures/codecs, package inspection,
  registry/index/cache and Cook queries; focused package fixtures; and the
  long-lived Asset Packages runtime contract.
- Payload contract: Archive and authored DAST write `uint8 0` for null or
  `uint8 1`, a `uint64` UTF-8 byte count, and exactly one canonical path. The
  path bound is 1 MiB. `SoftObject:<ExpectedQualifiedClass>:v1` composes inside
  existing fixed-array, Array, Map-value, and struct field grammars. Load
  validates into temporary identity before assignment and never resolves a
  target; loaded state and weak handles are absent from bytes.
- Dependency boundary: `SerializeValue` writes soft payloads without inserting
  into `FPackageFile::Dependencies`. Hard package headers, eager dependency
  loads, `IsPackageReferenced`, unload blockers, and existing move/delete hard
  checks remain unchanged. Cached versus unloaded values produce identical
  package bytes.
- Inspection/index API: `ExtractSoftAssetReferences` consumes
  `FAssetPackageInspection` plus reflection metadata and emits
  `FSoftAssetReference` occurrences with source fingerprint/object/class,
  declaring type, top-level field, expected class, target path, typed container
  route, and deterministic display path. It constructs no owner object, calls
  no `PostLoad`, and performs no target lookup or load.
- Cache/query schema: `AssetRegistry/SoftReferences.bin` uses `SRIX`, cache
  schema 1, DAST format 2, and extractor schema 1. Source keys carry complete
  file size, stable write ticks, and XXH3-128 content hash. Frozen bounds are
  four container levels, 100,000 occurrences per package, 1,000,000 per
  snapshot, 1 MiB target paths/Map-key tokens, and 4 KiB display paths. Public
  APIs are `GetSoftReferences`, `FindSoftReferencers`, `FindSoftTargets`, and
  `BuildCookReachability`.
- Invalidation/recovery: incremental scan computes content hashes before reuse;
  full validation re-extracts. Save/load publication, source move/delete,
  registry reconciliation, fingerprint changes, extractor mismatch, cache miss,
  and cache corruption update or rebuild the projection. A failed extraction
  emits an index diagnostic and publishes no partial source entry; cache writes
  are derived-data failures and never authored-data loss.
- Cook policy: `BuildCookReachability` adds every indexed non-null soft target
  to the hard dependency closure, rejects missing and registered-class-
  mismatched targets, and terminates cycles through a visited set. It does not
  load packages or promote soft references into runtime hard edges.
- Compatibility fixtures cover exact signatures and null/path bytes, direct/
  fixed/Array/Map routes, old omitted fields, unknown fields, unloaded and
  missing targets, registered/runtime type mismatch, malformed/truncated/
  overlong/trailing payloads, cache hit/content-hash miss/corruption recovery,
  source save/move/delete invalidation, no-target-load scanning, weak-cache byte
  exclusion, soft cycles, and unchanged hard unload behavior.
- Validation: `AssetPackageTests` passed 49/49, `CoreObjectTests` passed 70/70,
  and `AssetCookTests` passed 12/12, and a full `all` build completed under
  `Win64-Debug-DurinEditor-Tests`. The all-native run completed with one failure
  out of 1,014 tests: the out-of-scope World baseline test
  `FNewLevelBaselineTests.RecreatedLevelMatchesCapturedLogicalManifest` failed
  with mismatched actor ordering/rotation and a stopped RenderCore command;
  isolated rerun reproduced the same failure. No Stage 3 focused test failed.
- Open questions at handoff: none. Stage 4 owns target-move atomic rewrite,
  loaded-cache invalidation during repair, and project default-level
  storage/move integration. The later Stage 4 simplification keeps deletion
  hard-only rather than adding soft warnings to its transaction. Move repair
  uses the recorded fingerprints, typed routes, and tagged payload bounds rather
  than blind byte replacement.

### Stage 4: Integrate Move, Delete, and Selected Path Owners

Dependencies: Stage 3 authoritative field and index contracts.

- [x] Add soft referencers to move planning and atomically rewrite validated
  loaded and unloaded DAST fields from old path to new path.
- [x] Integrate registry/index publication, reversible external-store
  contributions, and rollback so target move and every selected soft-reference
  rewrite commit or restore together.
- [x] Keep deletion analysis, tokens, revalidation, Undo, and Redo hard-only;
  deleting a softly referenced target deliberately leaves a dangling path.
- [x] Update loaded soft values after moves and invalidate stale caches without
  converting them to hard references.
- [x] Migrate the project default-level identity to the selected soft-path value
  contract. Stage 2 already removed picker-time loading through the shared path
  mode; this stage owns stored-type conversion and move/repair integration.
- [x] Route default-level move repair through the shared move contract while
  preserving project-settings atomic save/rollback.
- [x] Audit other Stage 0 candidates and migrate only those with accepted soft
  semantics; record explicit reasons for retained manual paths.
- [x] Add move collision, stale fingerprint, read-only referencer, partial
  publication failure, rollback, loaded/unloaded referencer, missing target,
  dangling deletion, Cook failure, and project-setting tests.
- [x] Verify editor reveal, explicit load, broken-reference repair, class
  filtering, and Undo/Redo across a target move and application restart.
- [x] End the stage with a handoff listing the baseline commit, migrated owners,
  retained paths, move/delete policies, rollback evidence, and validation.

#### Acceptance Gate

- Moving a target updates every indexed soft DAST field and selected project
  setting atomically or restores the entire operation.
- Unloaded referencers are repaired without invoking their constructors,
  `PostLoad`, or target loads when tagged rewrite is valid.
- Deletion transaction state is unchanged by soft references; deleting a target
  leaves source bytes untouched and subsequent explicit load/Cook diagnostics
  report the dangling path at their normal boundaries.
- The default-level picker stores and moves canonical identity without loading
  the selected level merely to assign it.
- Retained `TObjectPtr`, `TWeakObjectPtr`, `FAssetPath`, and string uses have
  recorded semantics rather than being mechanically converted.

#### Stage 4 Handoff

- Baseline commit: pre-feature `5a569f31`; the completion squash incorporates
  the preceding persistence/index contract. The working set covered AssetCore move planning/publication,
  package move fixtures, the Level Editor move coordinator and project-setting
  owner, editor/runtime default-level loading, and long-lived asset, Content
  Browser, and level-system contracts.
- Move policy: a move requires an error-free current soft index. Loaded packages
  are scanned through live reflection; matching values retain exact rollback
  snapshots, receive the new path, clear weak caches, and save from memory.
  Unloaded indexed sources must match their complete size/time/XXH3-128
  fingerprint and writable state, then parse and rewrite only compatible tagged
  direct, fixed-array, Array, Map-value, and struct payloads. No owner
  construction, `PostLoad`, target-resolution side effect, or blind byte/string
  replacement occurs.
- Atomic publication: target, hard referencers, loaded/unloaded soft
  referencers, contributor packages and companion files, loaded-package map,
  complete registry/soft-index projection, dirty flags, and ordered external
  stores share backups and reverse rollback. Collision, stale fingerprint,
  read-only input, parse/schema failure, atomic file failure, or external-store
  failure restores old files, paths, live values, settings, and projections.
- Migrated owner: `MLevelEditor::DefaultLevel` and its pending edit are
  `TSoftObjectPtr<DLevel>`. YAML remains `Editor.DefaultLevel: <canonical path>`
  and now publishes atomically. Picker path mode never loads. Editor startup and
  `DGameEngine` use explicit typed soft load. The move coordinator registers one
  reversible project-setting action, so the YAML and in-memory value follow a
  level move inside AssetCore's transaction.
- Retained owners: all Stage 0 classified `TObjectPtr` hard fields,
  `TWeakObjectPtr` observations, import/source ownership records, registry and
  operation identities, editor document/navigation commands, thumbnail job
  keys, and required built-in runtime resources retain their original types and
  load/residency policies. No additional production field was converted.
- Deletion policy: by explicit simplification, AssetCore deletion analysis,
  batch tokens, revalidation, unload/staging, Undo, and Redo do not query the
  soft index. Source packages remain byte-identical and may dangle. Explicit
  soft load reports missing, deletion Undo naturally restores resolution, and
  Cook fails only when a selected root reaches the missing target. Optional
  warning UI is deferred outside the transaction.
- Cook boundary: reflected soft paths retain Stage 3 default reachability. The
  repository still lacks a project discovery/packaging driver; the typed default
  level is the canonical non-DAST root source for that future driver rather than
  a new process-global mutable root registry.
- Fixtures cover loaded and unloaded direct/fixed/Array/Map repair, weak-cache
  invalidation, no unloaded construction, registry restart, collision, stale
  content fingerprint, read-only source, external-store success and partial
  publication failure, complete rollback, Cook reachability after move, and
  dangling deletion with missing-target Cook failure.
- Validation: `AssetPackageTests` passed 54/54, `CoreObjectTests` 70/70,
  `AssetCookTests` 12/12, `EditorPropertyTests` 27/27, and
  `EditorAssetWorkflowTests` passed 51 with one expected symlink-privilege skip.
  A full `all` build completed under `Win64-Debug-DurinEditor-Tests`. The
  all-native run had one failure out of 1,019 executed tests: the unchanged
  out-of-scope
  `FNewLevelBaselineTests.RecreatedLevelMatchesCapturedLogicalManifest` actor
  order/rotation and stopped RenderCore-command failure; isolated rerun
  reproduced it exactly.
- Open questions: none inside the soft-reference model. Stage 5 owns final
  qualification and documentation. Complete project packaging remains the
  pre-existing deferred asset-cooking orchestration boundary.

### Stage 5: Qualify and Document the Reference Model

Dependencies: Stage 4 integration and migration evidence.

- [x] Run a final repository audit for reflected object references, weak handles,
  soft pointers, asset paths, path strings, and manual `LoadAsset` calls.
- [x] Verify direct and nested soft fields for null, unloaded, loaded, missing,
  wrong-type, moved, deleted, cooked, copied, duplicated, snapshotted, and
  application-restarted cases.
- [x] Verify hard references still emit/load dependencies and block unload,
  while weak and soft references retain their distinct behavior.
- [x] Verify package inspection, compatibility audit, registry cache rebuild,
  source/target queries, cook traversal, move/delete analysis, editor picker,
  Undo/Redo, save/resave, and corrupted-input behavior.
- [x] Remove transitional path-picker branches, duplicated load-to-get-path
  helpers, obsolete manual move fixups, and legacy generated fixtures only after
  their owners use the shared contract.
- [x] Document the lasting hard/weak/soft decision guide, public APIs, reflection
  schema, load/error behavior, index ownership, and authoring workflow in the
  owning runtime/editor documentation.
- [x] Follow the repository build and test instructions for focused suites and a
  successful full `all` build with the required long-running timeouts.
- [x] End the stage with a handoff listing the baseline commit, final working
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

#### Stage 5 Handoff

- Baseline commit: pre-feature `5a569f31` (`feat(reflection): complete
  container operations migration`). At the user's request, Stages 0 through 5
  are delivered as one completion commit rather than retaining intermediate
  stage commits.
- Final working set: CoreDObject soft values, reflection, GC, Archive, and
  snapshots; DHT parsing and generation; AssetCore resolve/load,
  serialization, inspection, index/cache, Cook, move, and deletion boundaries;
  DurinEd and LevelEditor picker/default-level workflows; native/DHT fixtures;
  and the owning asset, reflection, GC, Content Browser, and level-system
  documentation.
- Audit outcome: reflected hard owners, transient weak observers, and remaining
  import/source, document/navigation, thumbnail, operation, and built-in
  resource paths retain their established semantics. No further mechanical
  conversion is justified. The shared picker has distinct object and path
  assignment modes, and the default-level owner no longer uses a duplicated
  load-to-get-path or manual move-fixup path. Compatibility fixtures remain
  because they are current evidence, not transitional generated artifacts.
- Lasting documentation: `AssetPackages.md` owns the hard/weak/soft decision
  guide, public APIs, persistence, index, Cook, move, and dangling-deletion
  behavior; `ReflectionSystem.md` owns the generated SoftObject schema;
  `GarbageCollection.md` owns non-retention; `ContentBrowser.md` owns deletion
  transaction scope; and `LevelSystem.md` owns default-level authoring.
- Qualification replaced the mutable Sandbox-backed new-level test with
  `FLevelAssetTests.ReconstructsIsolatedStaticMeshLevelAndDependencies`. The
  fixture creates its own writable mount and level, uses only the stable
  `/Engine/Models/Box` dependency, verifies authored transforms/components and
  the package dependency table, unloads both packages, then proves level load
  eagerly reconstructs the hard mesh dependency. A nested fatal assertion can
  no longer cascade into a null dereference. The WorldTests target no longer
  links StandardAssetImport or embeds a Sandbox content path.
- Validation: DHT pytest passed 186/186; `AssetPackageTests` 54/54,
  `CoreObjectTests` 70/70, `AssetCookTests` 12/12,
  `EditorPropertyTests` 27/27, `EditorAssetWorkflowTests` 51 plus one expected
  privilege skip, and `WorldTests` 62/62 passed. Documentation and all-plan
  validation passed. `DevTool build --target all --agent` succeeded under
  `Win64-Debug-DurinEditor-Tests`; `DevTool test --target all --agent` passed
  1,020 tests with zero failures, two expected skips, and one disabled
  benchmark.
- Open questions: none. Complete project packaging/root discovery, async soft
  loading, optional warning UI outside deletion transactions, subobject paths,
  and other items below remain evidence-gated follow-ups rather than gaps in
  the completed version-one reference model.

## Validation Matrix

| Area | Required coverage |
| --- | --- |
| Core values | null, canonical path, object assignment, inner/transient rejection, weak-cache refresh/invalidation, equality/hash, copy/move, and thread rules |
| Typed load | already loaded, unloaded, missing, wrong registry class, wrong runtime class, corrupt package, null policy, and unchanged value on failure |
| DHT | direct/fixed-array/nested typed soft fields, qualified spellings, unresolved/non-object targets, raw wrapper and alias rejection, qualifiers, soft Map key rejection, and deterministic output |
| Property metadata | typed params, expected class, accessors, kind-safe dispatch, no strong GC schema, and handwritten/generated parity |
| Serialization | Archive, snapshot, DAST v2 null/path, bounds, malformed/trailing bytes, unknown fields, missing targets, and no weak-cache bytes |
| Dependency behavior | hard dependency table unchanged, no eager soft loads, no soft unload blocker, separate soft queries, and default cook reachability |
| Reference index | extraction without construction, source/target projection, container context, fingerprint hit/miss, invalidation, corruption fallback, bounds, and deterministic cache |
| Move/delete | loaded/unloaded referencers, atomic rewrite, stale source, read-only failure, collision, rollback, hard-only deletion, dangling preservation, and registry publication |
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
  and move repair without constructing owner objects. Version-one deletion
  transactions do not consume the index.
- The editor assigns soft paths from registry metadata without loading, exposes
  explicit load/reveal/repair actions, and participates in normal edit history.
- Target moves rewrite indexed soft paths atomically, while deletion remains a
  hard-reference-only transaction and may leave soft paths dangling.
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
