# Package And Object Path Identity Plan

Summary: Separate package, top-level asset, and subobject identities and adopt UE-style canonical object paths across authored assets and tooling.

Last reviewed: 2026-08-31

Status: Active
Completed:

## Current Status

The repository currently uses `FAssetPath` for a mounted package identity such
as `/Game/Objects/Test`, gives the package main asset that same object path, and
formats every other export as `/Game/Objects/Test:Root.Component`. This is
internally consistent only while persistent soft references target one package
main asset and every other export belongs to that asset's Outer tree.

The selected replacement separates the three identities:

- package path: `/Game/Objects/Test`;
- top-level asset path: `/Game/Objects/Test.Test`;
- complete object path: `/Game/Objects/Test.Test:Root.Component`.

This changes the persistent meaning and canonical spelling of non-null soft
object references. Under the repository's package-version policy it is a
canonical-value change, so it must not silently redefine DAST v8. The work will
introduce the next DAST version with a bounded construct-free v8 conversion,
migrate the maintained corpus, cut production over atomically, and then retire
the v8 production reader/writer route. No implementation stage has started.

## Goal

Give every persisted or diagnostic object identity an unambiguous structural
meaning while retaining the current one-public-main-asset package policy:

- mounted package lookup, catalog identity, dependency edges, files, and Cook
  closure ownership use a package path;
- the public main asset uses a top-level asset path containing its actual
  export name;
- objects below that asset use a subobject path relative to the top-level
  asset;
- soft references store a complete object path and derive their package-level
  Registry edge without losing the addressed object;
- object-path formatting and parsing round-trip canonically and never conflate
  a package, top-level export, and subobject.

The final implementation must leave one production package version, one
canonical path grammar, and no compatibility aliases whose meaning remains
ambiguous.

## Scope

- CoreDObject path value types, parsing, formatting, hashing, ordering, and
  `DObject::GetObjectPath()`.
- Package/linker validation for package names, export names, soft-reference
  values, and package-level dependency projection.
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

- Multiple independently discoverable public assets in one package. A package
  retains one Registry entry and one public main export in this plan.
- UE binary compatibility, UE API compatibility beyond the selected path
  semantics, or adoption of UE redirect, PIE, world-partition, or IoStore
  behavior.
- Hard references to arbitrary exports in another package. Existing
  cross-package hard imports continue to target the destination main export.
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
- `DPackage::GetPackagePath()` returns the package identity. A package main
  asset's `GetObjectPath()` includes its actual name after `.`. Descendants use
  the relative Outer chain after `:`. An object whose Outer chain does not
  terminate at the package main asset is rejected by authored package capture;
  this plan does not create a second public top-level asset route.
- `FSoftObjectPath` stores a complete object identity rather than a package
  alias. Resolution loads the owning package, follows package redirects, then
  resolves the top-level asset and subobject chain with exact type checks.
  Package-level Registry soft edges retain only the parsed owning package.
- Redirect destinations and catalog keys remain package identities. User-facing
  asset selections expose the main asset's complete object identity where an
  object is intended, and package identity where a file or closure is intended.
- Because existing DAST v8 soft-reference values encode package-only strings,
  the persistent change advances DAST. The converter derives the top-level
  asset name from the validated main export, rewrites every non-null v8 soft
  reference to a complete main-asset path, rebuilds package-level soft edges,
  and emits only the new canonical format.
- Conversion is detached, bounded, deterministic, fingerprinted, stale-checked,
  rollback-safe, and verified by byte-identical read/write re-emission. A value
  that cannot be mapped unambiguously fails without output.
- Ordinary discovery, load, save, mutation, Cook, and resave never invoke the
  converter implicitly. After corpus migration they select only the new
  package version.

## Implementation Stages

### Stage 0: Freeze the identity and compatibility contract

- [ ] Inventory every public path-bearing field and function in CoreDObject,
  AssetRegistry, Engine, AssetMaintenance, Cook, and Editor; classify each as
  package, top-level asset, complete object, physical file, or display-only.
- [ ] Inventory all maintained DAST v8 soft-reference values and prove whether
  each currently denotes the package main asset; record any null, redirect, or
  malformed cases that require explicit converter behavior.
- [ ] Freeze type names, canonical grammar, separator escaping policy, maximum
  component/path bounds, case rules, equality, hashing, ordering, and error
  diagnostics in focused contract tests before changing consumers.
- [ ] Freeze the next package version number and exact conversion matrix after
  confirming no concurrent package-format plan owns that version.
- [ ] Record the selected type/API inventory and migration findings in this
  plan before Stage 1 begins.

Completion condition: every existing path API and persisted path value has one
recorded target meaning, the corpus is proven convertible or blockers are
reported without mutation, and the new canonical grammar has executable tests.

### Stage 1: Establish CoreDObject path value types

- [ ] Add the explicit package, top-level asset, and complete object path types
  with structural storage, bounded parsers, canonical formatters, comparison,
  hashing, and null behavior.
- [ ] Migrate mount lookup and `DPackage` to the package-path type without
  changing physical resolution.
- [ ] Change `DObject::GetObjectPath()` to emit the top-level asset name and
  relative subobject chain, and reject authored objects outside the main asset
  Outer tree at the package boundary.
- [ ] Replace `FSoftObjectPath`'s package-only storage with complete object
  identity while keeping resolution out of CoreDObject.
- [ ] Add focused path and object-lifecycle tests covering main assets, nested
  objects, invalid separators, duplicate-looking names, ordering, hashing, and
  round trips.

Completion condition: CoreDObject expresses each identity without ambiguous
strings, its focused tests pass, and downstream modules compile through only
explicitly bounded temporary adapters.

### Stage 2: Add the new canonical package format and offline conversion

- [ ] Extend the format-neutral linker contract so soft-reference values carry
  validated complete object paths while Registry summaries carry derived
  package paths.
- [ ] Add the new canonical reader/writer policy and exact byte fixtures without
  changing unrelated section, BulkData, or object-value semantics.
- [ ] Implement a construct-free v8-to-current adapter that validates the full
  main/bulk closure, derives the main top-level identity from the main export,
  rewrites soft values, and rejects ambiguous input atomically.
- [ ] Extend AssetMaintenance plan/apply reporting with source/target
  fingerprints, stale checks, deterministic preview, rollback, and canonical
  re-emission verification.
- [ ] Cover null/main soft references, redirects, nested containers and maps,
  malformed inputs, stale plans, partial publication failure, and byte-identical
  repeated conversion.

Completion condition: supported v8 closures convert deterministically without
constructing objects, unsupported inputs remain byte-for-byte untouched, and
the new writer/reader pass exact round-trip qualification.

### Stage 3: Cut Engine and AssetRegistry over to structural identities

- [ ] Update live graph capture/application so soft references retain the full
  target identity and package summaries contain only parsed package-level
  dependency edges.
- [ ] Resolve a soft path by loading/following redirects for its package and
  then locating its exact top-level asset/subobject chain; preserve authored
  identity separately from the resolved package identity.
- [ ] Migrate Registry catalog, snapshots, cache fingerprints, dependency
  queries, and refresh to explicit package paths and the new production format.
- [ ] Update relocation, redirector fix-up, exact reference inspection,
  deletion, and atomic save transactions to rewrite only the appropriate path
  component and preserve subobject suffixes.
- [ ] Update Cook reachability and publication so package-level graph traversal
  is unchanged while serialized soft values keep complete object identity.
- [ ] Remove temporary path aliases and production v8 codec selection after all
  runtime consumers use the new contracts.

Completion condition: ordinary Engine and Registry operations select one new
format, exact-object soft references survive save/load and relocation, package
dependency queries remain package-level, and no production v8 reader/writer or
ambiguous `FAssetPath` API remains.

### Stage 4: Migrate tools, Editor, and maintained content

- [ ] Update AssetMaintenance, import/reimport, Content Browser, inspectors,
  thumbnails, transactions, and user-facing diagnostics to request and display
  the correct identity kind.
- [ ] Add a preview/apply project migration command for the new format using
  the established offline migration grammar and deterministic JSON reporting.
- [ ] Preview the complete maintained corpus, resolve every failure explicitly,
  apply the migration, and prove no maintained package remains on v8.
- [ ] Qualify editor-visible asset selection, rename/move, redirect fix-up,
  deletion blockers, canonical resave, source workflows, and restart behavior.
- [ ] Remove superseded v8 conversion glue only when no supported migration
  input requires it; keep any retained offline boundary private to Developer
  tooling and focused fixtures.

Completion condition: maintained assets and tools use the new identities,
editor workflows show canonical object paths without changing package/file
ownership, and repository searches find no unreviewed legacy spelling.

### Stage 5: Publish contracts and complete qualification

- [ ] Update the lasting asset-package, serialization, Registry/mutation,
  versioning, source-workflow, and user-facing documentation with the final
  identity grammar and ownership rules.
- [ ] Run focused CoreDObject, package-format, migration, Registry, asset
  operation, Cook, and affected editor tests according to the repository test
  workflow.
- [ ] Complete the broad native aggregate and full `all` build required for the
  shared CoreDObject API and package-format cutover.
- [ ] Run changed and all-plan documentation validation, record exact evidence,
  and close every acceptance gate before marking the plan complete.

Completion condition: code, maintained content, tests, and lasting contracts
agree on one structural identity model and one production package version, all
required validation passes, and the plan contains an evidence-backed handoff.

## Acceptance Gates

| Gate | Required evidence |
| --- | --- |
| Identity | Package, top-level asset, and complete object paths have distinct types and canonical round-trip tests. |
| Uniqueness | Main assets and every valid descendant produce unique paths; invalid or non-main Outer roots fail before publication. |
| Persistence | Complete soft-object targets survive canonical save/read/write and package-level Registry projection loses only the intentionally transient object suffix. |
| Migration | Every maintained v8 package is converted or explicitly rejected before apply; apply is stale-safe, rollback-safe, and byte-identically verified. |
| Operations | Load, soft resolve, relocation, redirect fix-up, deletion, inspection, Cook, and resave preserve the correct package/object components. |
| Cutover | Production policy exposes one reader/writer version and repository searches find no ambiguous path alias or runtime v8 fallback. |
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
