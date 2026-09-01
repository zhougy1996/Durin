# Compact Object Paths And Soft Object Pointers Plan

Summary: Compact structural object paths around interned names and rebuild soft object pointers as lightweight persistent identities with invalidatable weak caches.

Last reviewed: 2026-08-31

Status: Archived
Completed: 2026-08-31

## Current Status

The compact identity contract and all implementation stages are complete.
`FPackagePath`, `FTopLevelAssetPath`, and
`FObjectPath` now occupy 12, 24, and 64 bytes on Win64, down from 40, 120, and
184 bytes. `FSoftObjectPath` and per-pointer resolved paths are removed;
`FSoftObjectPtr` and a representative typed wrapper occupy 80 bytes, down from
376 bytes, and store only authored identity, weak handle, and cache epoch.
Case-insensitive identity, first-spelling display, strict interned-name bounds,
deterministic ordering, allocation-free subobject iteration, exact cache
population, and the four cache states have focused contract coverage.

The parent [Package And Object Path Identity](../2026-09/PackageAndObjectPathIdentity.md)
plan supplied the bounded DAST v8-to-v9 migration required to clear the final
qualification gate. All 25 maintained packages now use DAST v9,
`asset check --baseline` accepts the complete corpus, the broad native
aggregate passes, and the full `all` build succeeds. The compact identity work
therefore unblocks the remaining source-API cutover in the parent plan.

## Goal

- Represent a complete object path with two interned names and one optional
  subobject string while preserving the package, top-level asset, and object
  type boundaries.
- Make logical asset identity case-insensitive, preserve one display spelling
  for serialization and diagnostics, and reject input before `FName` could
  truncate it.
- Remove `FSoftObjectPath`; a default-invalid `FObjectPath` represents a null
  soft reference, and non-null values use the ordinary exact object identity.
- Make `FSoftObjectPtr` a lightweight authored path plus invalidatable weak
  runtime cache. Redirect resolution remains an Asset Runtime service concern
  and never silently rewrites authored identity.
- Keep `TSoftObjectPtr<T>` a thin typed wrapper with no implicit load and a
  stable untyped reflection boundary.

## Scope

- Core and CoreDObject name/path storage, parsing, formatting, comparison,
  ordering, hashing, and null representation.
- `FSoftObjectPtr`, `TSoftObjectPtr<T>`, reflection access, Archive operations,
  weak-cache state, and cache invalidation.
- Exact soft-object resolve/load integration, redirect handling, serialization,
  DHT-generated property metadata, editor property views, and affected tests.
- A bounded maintained-content and fixture audit for case-only collisions,
  over-limit identities, and canonical spelling.
- Adaptation of paused DAST v9 code only where required by the new APIs.

## Non-Goals

- Advancing DAST v9 tables, conversion, production selection, Registry cutover,
  or maintained-content conversion beyond keeping current work compatible.
- Supporting external asset corpora, retaining the old case-sensitive path
  identity, or introducing dual lookup and serialization behavior.
- Adding asynchronous loading, soft class pointers, network handles, PIE path
  remapping, or a generalized `TPersistentObjectPtr` hierarchy.
- Making `Get()` load, resolve a redirect, or consult the Asset Registry.

## Selected Design

### Compact structural paths

`FPackagePath` owns one `FName` containing the mounted long package name.
`FTopLevelAssetPath` owns that package value plus one asset-name `FName`.
`FObjectPath` owns the top-level value plus one canonical UTF-8 subobject string
without the leading `:`. The subobject string is empty for a top-level asset
and otherwise uses `.` between validated relative object names.

The resulting logical layout is:

```cpp
class FObjectPath
{
    FTopLevelAssetPath AssetPath; // package FName + asset FName
    std::string SubobjectPath;    // empty or Root.Component
};
```

No path type caches a concatenated canonical string or an allocated vector of
subobject components. Formatting appends into a caller-provided builder when
available; `ToString()` returns an owned string. Subobject traversal uses a
non-owning component iterator over `SubobjectPath`.

### Case and validation

Path equality and hashing use `FName` comparison identity and are therefore
case-insensitive. The first accepted spelling remains the display spelling used
when rebuilding a path string. Deterministic serialized ordering compares the
case-folded name text and subobject text rather than process-local name-pool
indices.

The complete package name and top-level asset name must each fit in
`FName::MaxSize - 1` bytes. Path factories validate this before constructing an
`FName`; truncation is never accepted as canonicalization. Subobject syntax
retains explicit UTF-8, component, separator, depth, and total-length checks.
Discovery rejects two maintained files or records whose logical identities
differ only by display case instead of choosing one by filesystem order.

### Soft reference value and state

`FSoftObjectPath` is removed. Empty serialized text loads a default-invalid
`FObjectPath`; non-empty text must pass ordinary exact object-path validation.
The untyped value has this conceptual state:

```cpp
class FSoftObjectPtr
{
    FObjectPath AuthoredPath;
    FWeakObjectPtr WeakObject;
    uint64 CacheEpoch = 0;
};
```

`AuthoredPath` alone defines serialization, equality, ordering, and hashing.
The resolved redirect destination is a transient result returned by Asset
Runtime and is not duplicated in every soft pointer. Asset Runtime may populate
the weak cache only after verifying that the loaded object's exact
`FObjectPath` equals the transient resolved target and that its class is
compatible.

A CoreDObject-owned cache epoch invalidates all populated soft caches without
coupling CoreDObject to Asset Registry. Object identity mutation and Asset
Runtime changes that can alter redirect resolution advance the epoch through a
narrow invalidation API. `Get()` returns the weak object only when the authored
path is non-null, the cache epoch is current, the weak handle is live, and the
requested class matches; it performs no load or redirect resolution.

The observable states are:

- `Null`: no authored path;
- `Pending`: a path exists but no cache has been populated;
- `Valid`: the current-epoch weak cache resolves to a live compatible object;
- `Stale`: a cache was populated but its epoch or weak handle is no longer
  valid.

Copying a soft pointer copies its cache as an optimization. Moving transfers
the value and leaves the source null. Changing or resetting the authored path
always clears the cache. Comparison and hash results never depend on cache
state.

### Typed API and ownership

`TSoftObjectPtr<T>` remains composition over `FSoftObjectPtr`. Its public object
assignment accepts `T*`; only reflection and Asset Runtime use the untyped
base. `Get()` returns `T*`, never loads, and preserves the existing requirement
that `T` derive from `DObject`. Resolve and load remain explicit free service
operations.

The reflection ABI continues to access one untyped `FSoftObjectPtr`, but APIs
rename soft-path-specific accessors to ordinary path terms such as `GetPath()`
and `SetPath(FObjectPath)`. Temporary source adapters are allowed only within
the stage that migrates all callers and must be removed before that stage is
closed.

## Implementation Stages

### Stage 0: Freeze the compact identity contract

- [x] Inventory path and soft-pointer construction, access, comparison, hash,
  formatting, reflection, serialization, and resolve/load call sites affected
  by removal of cached strings and `FSoftObjectPath`.
- [x] Record supported Win64 size/copy baselines for `FPackagePath`,
  `FTopLevelAssetPath`, `FObjectPath`, `FSoftObjectPtr`, and representative
  `TSoftObjectPtr<T>` values.
- [x] Audit maintained assets and fixtures for case-only identity collisions,
  package or asset names that exceed the selected `FName` bound, and paths that
  depend on case-sensitive lookup.
- [x] Add contract tests for case-insensitive logical identity, preserved
  display spelling, strict rejection before `FName` truncation, null object
  paths, deterministic ordering, and subobject iteration.

Completion condition: the affected surface and maintained corpus are
inventoried, selected semantics are executable, and every discovered collision
or unsupported identity has an explicit repository-owned migration.

### Stage 1: Compact the structural path types

- [x] Store package and asset identity in `FName` values and store only the
  canonical relative subobject string in `FObjectPath`.
- [x] Replace cached full strings with builder-based formatting and owned
  `ToString()` results; replace stored subobject vectors with allocation-free
  component iteration.
- [x] Implement case-insensitive equality/hash and deterministic lexical
  ordering without relying on unstable name-pool indices.
- [x] Enforce mount, UTF-8, separator, component, `FName`, and total path bounds
  before committing output values; failed factories leave outputs unchanged.
- [x] Migrate CoreDObject callers and tests while preserving the distinct
  package, top-level asset, and complete object types.

Completion condition: CoreDObject uses the compact representation, path
contracts pass, no canonical full-path cache or subobject-name vector remains,
and measured Win64 value sizes are recorded against the Stage 0 baseline.

### Stage 2: Rebuild soft object pointer state

- [x] Remove `FSoftObjectPath` and migrate null soft serialization to the
  default-invalid `FObjectPath` representation.
- [x] Reduce `FSoftObjectPtr` to authored identity, weak cache, and cache epoch;
  remove the stored resolved path and package-only cache validation.
- [x] Add the global soft-cache invalidation seam and connect object identity
  mutation, redirect/catalog semantic changes, unload, and runtime shutdown to
  the required invalidation behavior.
- [x] Define and test `Null`, `Pending`, `Valid`, and `Stale`, including copy,
  move, reset, equality, ordering, hashing, collection, unload, and epoch
  invalidation.
- [x] Tighten `TSoftObjectPtr<T>` construction and assignment while retaining
  the untyped reflection/service access boundary and incomplete-type-safe
  declaration behavior.

Completion condition: no `FSoftObjectPath` or per-pointer resolved path remains,
soft values have deterministic path-only value semantics, and cached access is
non-loading, typed, weak, and invalidation-safe.

### Stage 3: Integrate exact runtime resolution and serialization

- [x] Migrate Archive, linker values, package capture/application, DHT metadata,
  property lifecycle operations, and editor property views to direct
  `FObjectPath` soft values.
- [x] Resolve redirects to transient exact `FObjectPath` results, load the
  owning package explicitly, select the exact top-level asset/subobject, and
  populate the weak cache only after exact path and class validation.
- [x] Preserve authored identity across successful resolve/load and redirect
  traversal; explicit mutation and relocation remain the only operations that
  rewrite serialized paths.
- [x] Adapt the paused DAST v9 implementation and its tests to the new APIs
  without advancing its format stage or adding a second wire representation.
- [x] Remove all temporary `FSoftObjectPath`, package-main soft-reference, old
  accessor, and package-only resolved-cache adapters introduced or retained
  during migration.

Completion condition: runtime, reflection, serialization, editor, and paused
v9 code use direct exact object paths; resolve/load behavior is explicit and
redirect-safe; repository search finds no obsolete soft-path layer or ambiguous
package-only cache validation.

### Stage 4: Qualify the refactor and unblock DAST v9

- [x] Re-run the maintained corpus audit and migrate repository-owned spelling
  or content conflicts without adding an external compatibility route.
- [x] Run focused Core, CoreDObject, package/linker, Asset Runtime, Registry,
  reflection, DHT, and affected editor tests according to the repository test
  workflow.
- [x] Run the required broad native aggregate and full build for the shared
  Core/CoreDObject ABI change according to the repository build workflow.
- [x] Update lasting asset-path, reflection, garbage-collection, and asset
  package contracts; run changed and all-plan documentation validation.
- [x] Record final layouts, validation evidence, and the handoff that allows
  Stage 3 of Package And Object Path Identity to resume.

Completion condition: maintained content and all affected runtime/editor paths
use one compact case-insensitive identity model, required validation passes,
lasting contracts are updated, and the parent DAST v9 plan is explicitly
unblocked.

## Acceptance Gates

| Gate | Required evidence |
| --- | --- |
| Layout | `FObjectPath` contains two interned names and one subobject string with no duplicated canonical string or component vector; before/after Win64 sizes are recorded. |
| Identity | Case-only variants compare and hash equal, accepted display spelling round-trips, ordering is deterministic, and over-limit names fail without truncation. |
| Soft value | `FSoftObjectPath` and stored resolved paths are absent; null, comparison, hash, copy, move, and serialization depend only on authored `FObjectPath`. |
| Cache | Weak cache state is non-owning and non-loading, exact-target population is enforced, and collection, unload, identity mutation, redirect changes, and epoch invalidation cannot return a stale object. |
| Type safety | `TSoftObjectPtr<T>` exposes typed object operations while reflection and Asset Runtime retain one bounded untyped access path. |
| Compatibility | Every maintained repository asset and fixture is accepted or migrated; no dual case mode or general external-content compatibility layer is added. |
| Qualification | Focused tests, required broad native aggregate, full build, corpus audit, and documentation validators pass. |

## Related Documentation

- [Package And Object Path Identity](../2026-09/PackageAndObjectPathIdentity.md)
- [Asset Packages](../../../Runtime/Assets/AssetPackages.md)
- [Garbage Collection](../../../Runtime/Core/GarbageCollection.md)
- [Reflection System](../../../Runtime/Core/ReflectionSystem.md)
- [Serialization](../../../Runtime/Core/Serialization.md)
- [Agent Build And Run Workflow](../../../Agents/BuildAndRun.md)
- [Agent Testing Workflow](../../../Agents/Testing.md)

## Related Code

- [Name](../../../../Engine/Source/Runtime/Core/Public/Misc/Name.h)
- [Asset path](../../../../Engine/Source/Runtime/CoreDObject/Public/DObject/AssetPath.h)
- [Soft object pointer](../../../../Engine/Source/Runtime/CoreDObject/Public/DObject/SoftObjectPtr.h)
- [Weak object pointer](../../../../Engine/Source/Runtime/CoreDObject/Public/DObject/WeakObjectPtr.h)
- [Archive](../../../../Engine/Source/Runtime/CoreDObject/Private/DObject/Archive.cpp)
- [Asset loading](../../../../Engine/Source/Runtime/Engine/Public/Asset/Load.h)
- [Asset runtime](../../../../Engine/Source/Runtime/Engine/Private/Asset/AssetRuntime.cpp)
- [Durin Header Tool property parser](../../../../Engine/Source/Programs/DurinHeaderTool/durin_header_tool/parser/property_parser.py)
