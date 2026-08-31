# Package And Object Path Identity Plan

Summary: Separate package, top-level asset, and subobject identities and support multiple independently addressable top-level assets per package.

Last reviewed: 2026-08-31

Status: Active
Completed:

## Current Status

The repository currently uses `FAssetPath` for a mounted package identity such
as `/Game/Objects/Test`, gives one designated main asset that same object path,
and formats every other export as `/Game/Objects/Test:Root.Component`.
`DPackage::Asset`, `MainExportId`, one package-level asset class, and
package-only soft references make that main asset a format and runtime
invariant rather than an editor convention.

The selected replacement separates the three identities:

- package path: `/Game/Objects/Test`;
- top-level asset path: `/Game/Objects/Test.Test`;
- complete object path: `/Game/Objects/Test.Test:Root.Component`.

The 2026-08-31 revision removed the preliminary one-public-main-asset
constraint after comparing it with the intended UE ownership model. Package is
now the closure and residency container; independently addressable assets are
its package-outer exports. “Primary Asset” remains a separate future Asset
Manager policy rather than a replacement name for `MainExportId`.

Stage 0 is complete. The public API and main-asset assumptions are inventoried
below, DAST v9 is reserved for this cutover, and executable CoreDObject contract
tests freeze the three structural path kinds. A construct-free audit of all 25
maintained DAST v8 packages found one package-outer export per package, no soft
references, no redirectors, and only exact former-main hard imports. Every
maintained package is therefore deterministically convertible; fixtures will
cover the additional package-outer, null/soft, redirect, malformed, and
cross-package cases that the maintained corpus does not contain.

Stage 1 is complete. `FPackagePath`, `FTopLevelAssetPath`, and `FObjectPath`
provide the frozen grammar; `DPackage` automatically registers, strongly
retains, enumerates, name-checks, reparents, and retires every direct persistent
export; `DObject::GetObjectPath()` derives the selected top-level root; and soft
paths now store exact complete object identity. The former reflected
`DPackage::Asset` owner is gone. A non-owning v8 main selector and the
short-lived `FAssetPath` source alias remain only as bounded adapters for the
Stage 2/3 cutover.

Focused `PackageLinkerContractTests` and `CoreObjectTests` pass, covering path
round trips and failures plus multi-asset retention, rename collision,
retirement, nested object paths, and exact subobject soft references. Stage 2
is next: add DAST v9 tables and the construct-free v8 converter.

## Goal

Give every persisted or diagnostic object identity an unambiguous structural
meaning while making Package a container rather than an asset alias:

- mounted package lookup, package metadata, dependency edges, files, and Cook
  closure ownership use a package path;
- each export whose Outer is the Package is an independently addressable
  top-level asset with its own asset name, class, redirect state, and Registry
  record;
- asset catalog identity uses the top-level asset path rather than aliasing the
  owning package;
- objects below a top-level asset use a subobject path relative to that asset;
- soft references store a complete object path and derive their package-level
  Registry edge without losing the addressed object;
- package load and publication own one closure, while asset lookup and object
  resolution select an exact top-level asset or subobject inside that closure;
- object-path formatting and parsing round-trip canonically and never conflate
  a package, top-level export, and subobject.

The final implementation must leave one production package version, one
canonical path grammar, and no compatibility aliases whose meaning remains
ambiguous.

## Scope

- CoreDObject path value types, parsing, formatting, hashing, ordering, and
  `DObject::GetObjectPath()`.
- Package/linker validation for package names, export names, soft-reference
  values, top-level asset records, and package-level dependency projection.
- `DPackage` ownership and residency without a distinguished `Asset` pointer or
  serialized `MainExportId`.
- Engine capture/application, soft resolution, load, relocation, redirector
  fix-up, deletion analysis, inspection, Cook, and canonical resave.
- AssetRegistry catalog/cache/query records whose current `FAssetPath` fields
  actually identify packages.
- Editor and Developer callers that display, select, serialize, or mutate
  package/object identities.
- A bounded offline current-corpus conversion and the maintained authored
  package corpus.
- Focused contract, migration, asset-operation, Registry, Cook, and editor
  consumer validation plus lasting documentation.

## Non-Goals

- UE binary compatibility, UE API compatibility beyond the selected path
  semantics, or adoption of UE redirect, PIE, world-partition, or IoStore
  behavior.
- Adding an Asset Manager `PrimaryAssetId` concept. Primary/secondary asset
  policy is independent of Package containment and Registry identity.
- Treating the package leaf as an implicit asset selection. Callers that need
  an object must supply or receive an exact top-level/object path.
- A permanent multi-version runtime reader, migration-on-load, or accepting
  legacy spellings in new saves.
- Changing physical `.dasset`/`.dbulk` ownership or mounted package-to-file
  resolution.

## Selected Decisions

- Introduce an explicit mounted package-path type. The final API must not use
  `FAssetPath` to mean both a package and an object; a short-lived source alias
  is allowed only inside a buildable migration stage and is removed before the
  final gate.
- Represent a top-level asset path structurally as package path plus export
  name. Represent a complete object path as that top-level identity plus an
  optional ordered subobject-name chain. Do not repeatedly split canonical
  strings at service boundaries.
- Canonical string forms are `/Mount/Package`,
  `/Mount/Package.Asset`, and `/Mount/Package.Asset:Child.Grandchild`.
  Package paths reject object suffixes; top-level paths reject subobject
  suffixes; complete object paths validate every component and reject empty,
  ambiguous, noncanonical, or trailing separators.
- `DPackage::GetPackagePath()` returns the package identity. `DPackage` does not
  designate one `Asset`; every serializable export whose Outer is the Package
  is a top-level asset and its `GetObjectPath()` includes its own name after
  `.`. Descendants use the relative Outer chain after `:`.
- The linker summary owns package identity and shared dependency/bulk facts,
  not one asset class, redirect destination, or main export. The Registry
  section contains a canonical list of top-level asset records, each binding
  an export id to object path, class, and optional redirect destination.
- `FSoftObjectPath` stores a complete object identity rather than a package
  alias. Resolution loads the owning package, selects the addressed top-level
  asset, follows asset redirects, then resolves the subobject chain with exact
  type checks.
  Package-level Registry soft edges retain only the parsed owning package.
- Asset redirectors are top-level asset records and target exact object paths;
  package relocation remains a separate closure operation. Catalog asset keys
  are top-level object paths, while dependency, file, cache, residency, and
  publication keys remain package paths.
- Cross-package imports identify the exact target top-level asset rather than
  assuming a destination main export. Package-level hard dependency projection
  derives the owning package from that exact import identity.
- Public loading separates `LoadPackage(package path)` from exact asset/object
  resolution. Compatibility convenience APIs may exist only during staged
  migration and must not infer an asset from the package leaf at the final
  gate.
- Because existing DAST v8 soft-reference values encode package-only strings,
  the persistent change advances DAST. The converter maps the validated v8
  `MainExportId` to one top-level asset record, recognizes any additional
  package-outer exports as additional top-level assets, rewrites every non-null
  v8 soft reference to the exact former main-asset path, rebuilds package-level
  edges, and emits only the new canonical format.
- Conversion is detached, bounded, deterministic, fingerprinted, stale-checked,
  rollback-safe, and verified by byte-identical read/write re-emission. A value
  that cannot be mapped unambiguously fails without output.
- Ordinary discovery, load, save, mutation, Cook, and resave never invoke the
  converter implicitly. After corpus migration they select only the new
  package version.

## Stage 0 Identity And Migration Inventory

The frozen path contract is case-sensitive bytewise identity with valid UTF-8,
no escaping, and rejection of `/`, `\\`, `.`, and `:` inside a component.
Package, asset, and subobject separators are structural and cannot be quoted.
Each component is limited to 1,024 bytes and a complete path to 1 MiB. Null is
represented only by a default-invalid value. Failed parsing leaves the output
unchanged. Equality, ordering, and hashing use the canonical complete spelling.

| Existing surface | Current meaning | Required replacement |
| --- | --- | --- |
| `FAssetPath`, mount lookup, package/files/cache/publication/Cook closure keys | Mounted package | `FPackagePath` |
| `FAssetData::PackagePath` and catalog map key | Package aliased as one asset | package metadata keyed by `FPackagePath`; asset records keyed by `FTopLevelAssetPath` |
| Registry dependency and referencer fields | Package edge | `FPackagePath` |
| Registry redirect destination and redirect resolution result | Package aliased as one asset | `FObjectPath` target and `FTopLevelAssetPath` catalog chain |
| `FSoftObjectPath`, serialized soft payload, exact reference target | Former main package alias | `FObjectPath`; dependency projection derives its `FPackagePath` |
| hard import `PackageName`/`ObjectName` | Destination package plus assumed main export | exact `FTopLevelAssetPath`; hard edge derives its package |
| `DPackage::GetPackagePath`, create/find/relocate | Package string or `FAssetPath` | typed `FPackagePath` |
| `DObject::GetObjectPath` and inspection object paths | display string with main-asset omission | typed/canonical `FObjectPath` for asset objects; display strings remain derived |
| physical paths and source hints | file-system identity | remain explicitly physical strings/paths |
| logical property/schema/diagnostic routes | display-only | remain strings and never enter identity parsing |

| Main-asset invariant | Exact multi-asset replacement |
| --- | --- |
| `DPackage::Asset`, `GetAsset()`, and `SetAsset()` | package-owned direct-export collection, exact name lookup, and automatic registration/retirement |
| `MainExportId` / `FPackageSummary::MainExport` | sorted complete top-level asset records, each bound to one export id |
| package `AssetClass` | class on each top-level asset record |
| package `bRedirect` / `RedirectDestination` | redirect state and exact object target on the corresponding top-level asset record |
| load/create returning the package main asset | `LoadPackage(FPackagePath)` plus exact `LoadObject(FObjectPath)` / top-level lookup |
| save/capture starting at `GetAsset()` | enumerate every registered package-outer asset and each descendant once |
| relocation renaming package and inferred leaf asset | package relocation rewrites only the package component; asset rename is a separate exact operation |
| deletion/fix-up treating one catalog entry as a package | distinguish exact top-level asset deletion from whole-package closure deletion |

No other active plan claims DAST v9. The conversion matrix is frozen as:

| v8 input | v9 result |
| --- | --- |
| validated `MainExportId` | one top-level record for that package-outer export |
| additional package-outer export | one additional independently addressable top-level record |
| descendant export | preserved beneath its nearest package-outer export |
| null soft reference | null `FObjectPath` |
| non-null v8 soft package path | exact former-main `FObjectPath` |
| cross-package hard import | exact former-main `FTopLevelAssetPath` |
| redirect destination | exact former-main `FObjectPath` |
| malformed, missing-main, ambiguous, or noncanonical value | conversion failure with no output |

The 2026-08-31 maintained-corpus audit covered 25 v8 `.dasset` files and eight
external `.dbulk` companions. All 25 passed full construct-free inspection;
each has exactly one package-outer export, none contains a soft reference or
redirect, and every recorded hard import targets the former main asset. The
offline converter fixtures, rather than authored corpus, own coverage for all
other rows in the matrix.

## Implementation Stages

### Stage 0: Freeze the identity and compatibility contract

- [x] Inventory every public path-bearing field and function in CoreDObject,
  AssetRegistry, Engine, AssetMaintenance, Cook, and Editor; classify each as
  package, top-level asset, complete object, physical file, or display-only.
- [x] Inventory every `DPackage::Asset`, `GetAsset()`, `MainExportId`,
  package-level asset-class/redirect field, and implicit package-to-main-asset
  load assumption; record its exact multi-asset replacement.
- [x] Inventory all maintained DAST v8 export topologies, soft-reference values,
  imports, and redirects. Prove which values denote the former main asset and
  record any additional package-outer export, null, malformed, or cross-package
  case that requires explicit converter behavior.
- [x] Freeze type names, canonical grammar, separator escaping policy, maximum
  component/path bounds, case rules, equality, hashing, ordering, and error
  diagnostics in focused contract tests before changing consumers.
- [x] Freeze the next package version number and exact conversion matrix after
  confirming no concurrent package-format plan owns that version.
- [x] Record the selected type/API inventory and migration findings in this
  plan before Stage 1 begins.

Completion condition: every existing path and main-asset API plus every
persisted identity has one recorded target meaning, the corpus is proven
convertible or blockers are reported without mutation, and the new canonical
grammar and top-level asset rules have executable tests.

### Stage 1: Establish CoreDObject path value types

- [x] Add the explicit package, top-level asset, and complete object path types
  with structural storage, bounded parsers, canonical formatters, comparison,
  hashing, and null behavior.
- [x] Migrate mount lookup and `DPackage` to the package-path type without
  changing physical resolution.
- [x] Remove `DPackage::Asset` as the root/retention authority. Give Package an
  explicit collection/enumeration contract for all direct top-level assets and
  define their registration, GC retention, rename, collision, and retirement
  behavior.
- [x] Change `DObject::GetObjectPath()` to emit each package-outer top-level
  asset name and the relative subobject chain beneath that selected root.
- [x] Replace `FSoftObjectPath`'s package-only storage with complete object
  identity while keeping resolution out of CoreDObject.
- [x] Add focused path and object-lifecycle tests covering top-level assets, nested
  objects, multiple top-level assets, invalid separators, duplicate-looking
  names, ordering, hashing, GC retention, and round trips.

Completion condition: CoreDObject expresses each identity without ambiguous
strings, its focused tests pass, and downstream modules compile through only
explicitly bounded temporary adapters.

### Stage 2: Add the new canonical package format and offline conversion

- [ ] Extend the format-neutral linker contract so soft-reference values carry
  validated complete object paths while Registry summaries carry derived
  package paths.
- [ ] Replace the single `MainExportId`/asset-class/redirect summary with a
  canonical top-level asset table bound to exports; validate unique names,
  package Outer topology, per-asset class/redirect shape, and complete coverage.
- [ ] Add the new canonical reader/writer policy and exact byte fixtures without
  changing unrelated section, BulkData, or object-value semantics.
- [ ] Implement a construct-free v8-to-current adapter that validates the full
  main/bulk closure, maps the former main and every package-outer export to
  top-level asset records, rewrites soft/import/redirect values, and rejects
  ambiguous input atomically.
- [ ] Extend AssetMaintenance plan/apply reporting with source/target
  fingerprints, stale checks, deterministic preview, rollback, and canonical
  re-emission verification.
- [ ] Cover null/main soft references, redirects, nested containers and maps,
  malformed inputs, stale plans, partial publication failure, and byte-identical
  repeated conversion.

Completion condition: supported v8 closures convert deterministically without
constructing objects, every new top-level asset record is derived from validated
export topology, unsupported inputs remain byte-for-byte untouched, and the new
writer/reader pass exact round-trip qualification.

### Stage 3: Cut Engine and AssetRegistry over to structural identities

- [ ] Update live graph capture/application to enumerate all package-outer
  top-level assets, retain each complete object graph exactly once, and preserve
  full hard/soft target identity while package summaries contain only derived
  package-level dependency edges.
- [ ] Split package load from asset/object lookup. Resolve a soft path by loading
  its owning package, selecting the exact top-level asset, following asset-level
  redirects, and then locating its subobject chain; preserve authored identity
  separately from resolved object and package identity.
- [ ] Migrate Registry catalog, snapshots, cache fingerprints, dependency
  queries, and refresh to one package metadata record plus zero-or-more
  top-level asset records from the new production format.
- [ ] Update relocation, redirector fix-up, exact reference inspection,
  deletion, and atomic save transactions to rewrite only the appropriate path
  component, preserve asset/subobject suffixes, and distinguish asset deletion
  from whole-package closure deletion.
- [ ] Update Cook reachability and publication so package-level graph traversal
  and closure publication remain package-level while root selection and
  serialized references retain exact top-level/object identity.
- [ ] Remove temporary path aliases and production v8 codec selection after all
  runtime consumers use the new contracts; remove `GetAsset()` and implicit
  package-to-asset loading rather than retaining a hidden main-asset fallback.

Completion condition: ordinary Engine and Registry operations select one new
format, multiple top-level assets and exact-object references survive
save/load/relocation, package dependency queries remain package-level, and no
production v8 reader/writer, `MainExportId`, `DPackage::Asset`, implicit
main-asset load, or ambiguous `FAssetPath` API remains.

### Stage 4: Migrate tools, Editor, and maintained content

- [ ] Update AssetMaintenance, import/reimport, Content Browser, inspectors,
  thumbnails, transactions, and user-facing diagnostics to request and display
  the correct identity kind.
- [ ] Add a preview/apply project migration command for the new format using
  the established offline migration grammar and deterministic JSON reporting.
- [ ] Preview the complete maintained corpus, resolve every failure explicitly,
  apply the migration, and prove no maintained package remains on v8.
- [ ] Qualify editor-visible asset selection, rename/move, redirect fix-up,
  per-asset and whole-package deletion blockers, canonical resave, source
  workflows, multi-asset package presentation, and restart behavior.
- [ ] Remove superseded v8 conversion glue only when no supported migration
  input requires it; keep any retained offline boundary private to Developer
  tooling and focused fixtures.

Completion condition: maintained assets and tools use the new identities,
editor workflows expose every top-level asset with canonical object paths while
preserving package/file ownership, and repository searches find no unreviewed
legacy spelling or implicit main-asset selection.

### Stage 5: Publish contracts and complete qualification

- [ ] Update the lasting asset-package, serialization, Registry/mutation,
  versioning, source-workflow, and user-facing documentation with the final
  identity grammar, multi-asset Package model, and ownership rules.
- [ ] Run focused CoreDObject, package-format, migration, Registry, asset
  operation, Cook, and affected editor tests according to the repository test
  workflow.
- [ ] Complete the broad native aggregate and full `all` build required for the
  shared CoreDObject API and package-format cutover.
- [ ] Run changed and all-plan documentation validation, record exact evidence,
  and close every acceptance gate before marking the plan complete.

Completion condition: code, maintained content, tests, and lasting contracts
agree on one structural identity model, a Package container with no distinguished
main asset, and one production package version; all required validation passes
and the plan contains an evidence-backed handoff.

## Acceptance Gates

| Gate | Required evidence |
| --- | --- |
| Identity | Package, top-level asset, and complete object paths have distinct types and canonical round-trip tests. |
| Package model | `DPackage` owns zero-or-more top-level assets without `Asset`/`MainExportId`; each direct export and descendant has a unique structural path. |
| Registry | One package projection publishes zero-or-more canonical top-level asset records while hard/soft dependency edges remain package-level. |
| Persistence | Complete soft-object targets survive canonical save/read/write, and each derived package-level Registry edge identifies the correct owning package without replacing the stored object target. |
| Migration | Every maintained v8 package is converted or explicitly rejected before apply; apply is stale-safe, rollback-safe, and byte-identically verified. |
| Operations | Package load, exact asset/object resolution, relocation, asset redirects, per-asset/package deletion, inspection, Cook, and resave preserve the correct identity components. |
| Cutover | Production policy exposes one reader/writer version and repository searches find no main-asset invariant, ambiguous path alias, or runtime v8 fallback. |
| Qualification | Focused tests, the required broad native aggregate, full build, corpus audit, and documentation validators pass. |

## Related Documentation

- [Asset Packages](../Runtime/Assets/AssetPackages.md)
- [Asset Catalog And Mutation](../Runtime/Assets/AssetCatalogAndMutation.md)
- [Versioning](../Runtime/Assets/Versioning.md)
- [Serialization](../Runtime/Core/Serialization.md)
- [Agent Build And Run Workflow](../Agents/BuildAndRun.md)
- [Agent Testing Workflow](../Agents/Testing.md)

## Related Code

- [Asset path](../../Engine/Source/Runtime/CoreDObject/Public/DObject/AssetPath.h)
- [Soft object path](../../Engine/Source/Runtime/CoreDObject/Public/DObject/SoftObjectPtr.h)
- [Object identity](../../Engine/Source/Runtime/CoreDObject/Public/DObject/Object.h)
- [Package linker](../../Engine/Source/Runtime/CoreDObject/Public/DObject/PackageLinker.h)
- [Package format](../../Engine/Source/Runtime/CoreDObject/Public/DObject/PackageFormat.h)
- [Asset Registry catalog](../../Engine/Source/Runtime/AssetRegistry/Public/AssetRegistry/Catalog.h)
- [Asset operations](../../Engine/Source/Runtime/Engine/Public/Asset/AssetOperations.h)
- [AssetMaintenance module](../../Engine/Source/Developer/AssetMaintenance/AssetMaintenance.dmodule)
