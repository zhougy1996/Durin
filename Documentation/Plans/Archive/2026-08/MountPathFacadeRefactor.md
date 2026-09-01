# Mount Path Facade Refactor Plan

Summary: Replace the broad `PathUtilities` namespace with explicit `FPaths` and `FMountPaths` facades while preserving mount and path behavior.

Last reviewed: 2026-08-30

Status: Archived
Completed: 2026-08-30

## Current Status

All implementation stages and acceptance gates are complete.
`FPaths` now owns the physical-path algorithms, `FMountPaths` owns the unchanged
process-wide registry implementation, and mount types live directly in
`Durin`. Mutable registry helpers are available only from
`Misc/MountPathTestSupport.h` under `Durin::Testing`; focused coverage verifies
nested fixture restoration and publication-state preservation. All repository
C++ consumers have migrated and `Misc/LexicalPath.h` has been removed.

The temporary compatibility layer was omitted during implementation because
all repository-owned consumers could be migrated atomically in this isolated
checkout. This avoids landing a transient second public surface while retaining
the plan's single-registry requirement.

Before editing, `CoreFileSystemTests` passed 45/45 and `AssetPackageTests`
passed 131/131. After migration, `CoreFileSystemTests` passes 46/46, including
the new nested-fixture regression, and `AssetPackageTests` passes 131/131. The
full configured `all` build and all 74 default native-test targets pass. The
stale skeletal-scene lifecycle assertion encountered during qualification was
migrated to the established explicit cooked-mesh blocking-load contract; its
focused integration case passes before the full selection.

## Goal

Make the common call sites short and domain-specific without introducing a
`Durin::Path` name that can be hidden by ordinary `Path` variables:

```cpp
Durin::FPaths::IsLexicalDescendantPath(Path, Root, true);
Durin::FMountPaths::ResolveAssetPath(VirtualPath);
```

At completion:

- `FPaths` owns engine/project directory queries and general physical-path
  algorithms.
- `FMountPaths` is the process-wide static facade for virtual mount lookup,
  asset-path resolution, dependency checks, and registry publication.
- Mount-related value and error types live directly in `Durin` and retain
  self-describing `Mount` names.
- Test-only registry mutation is separated from the production facade.
- No production or test source references `PathUtilities`.

## Scope

This plan changes public C++ organization, declarations, includes,
implementations, and repository-owned call sites for the existing path and
mount APIs. It also updates authoritative documentation when it names the
affected C++ entry points.

The following are non-goals:

- Changing mount lookup, normalization, dependency, publication, or failure
  semantics.
- Supporting multiple live mount registries or dependency-injected registry
  instances.
- Changing virtual path syntax, default mount definitions, package identity,
  or serialized data.
- Broadly renaming local path variables or unrelated asset APIs.
- Preserving `PathUtilities` as a permanent compatibility surface.

## Selected Design

### Public facades

`FPaths` remains a non-instantiable static facade. The declarations currently
in `Misc/LexicalPath.h` become static `FPaths` operations while retaining their
behavior-revealing names:

- `TryMakeLexicalRelativePath`
- `IsLexicalDescendantPath`
- `TryResolveContainedPath`

`FMountPaths` is a non-instantiable static facade for the current process-wide
mount service. It initially preserves the existing operation names so that the
refactor does not combine ownership changes with semantic API redesign:

- `GetRegisteredMountPoints`
- `FindMountForVirtualPath`
- `ResolveAssetPath`
- `ClassifyAssetPath`
- `CheckMountDependency`
- `PublishMountRegistry`
- `ValidateDefaultMountPoints`
- `InitDefaultMountPoints`

`ProjectContentMountRoot` becomes a member of `FMountPaths`, keeping the
constant beside the mount contract rather than adding another global name.

### Public types

The existing types move directly into `Durin`:

- `FMountPoint`
- `FMountLookupResult`
- `FMountPathResult`
- `FAssetPathResult`
- `FMountPolicyResult`
- `EMountOwner`
- `EMountPathError`

`EPathExistence` becomes `EMountPathExistence` because the unqualified type is
otherwise too broad at `Durin` scope. This is a naming-only change; its values
and default behavior remain unchanged.

### Test support

Mutable registry helpers do not become production methods on `FMountPaths`.
Move `FScopedMountRegistryFixture` and `RegisterMountPointForTests` into a
dedicated public Core test-support header under `Durin::Testing`. Preserve the
existing scoped save/restore and single-root registration semantics. The
header remains exported from Core because tests in multiple modules consume
the fixture, but production source must not include it.

### Compatibility policy

The first implementation stage supplies deprecated-in-intent, unannotated
`PathUtilities` aliases and forwarding functions so downstream repository
layers can migrate in bounded commits. These shims must introduce no second
registry and must delegate to the canonical facades. They are removed in the
final stage; no lasting source-compatibility promise is made.

## Implementation Stages

### Stage 0: Freeze the inventory and baseline

- [x] Record every declaration, definition, include, and qualified use of
  `PathUtilities`, `Misc/Paths.h`, and `Misc/LexicalPath.h`, grouped by Core,
  runtime consumers, developer/editor/program consumers, and tests.
- [x] Identify unqualified uses made visible by `using namespace Durin` so the
  migration does not rely only on textual `Durin::PathUtilities` matches.
- [x] Confirm whether any non-test production target includes or calls the
  mutable test registry helpers.
- [x] Run the smallest existing Core lexical-path and mount/package test
  baselines according to the repository testing workflow, recording any
  pre-existing failures before editing declarations.
- [x] Complete the stage when the migration inventory accounts for all current
  call sites and the baseline evidence is recorded in `Current Status`.

### Stage 1: Introduce the canonical Core API

- [x] Add `FMountPaths` and move mount contract types to `Durin`, preserving
  ABI-relevant field order, enum values, result conversions, defaults, and
  exported function behavior.
- [x] Add the three general path algorithms to `FPaths` and keep their existing
  implementation behavior and platform case rules.
- [x] Split mount declarations from general path declarations so public header
  ownership matches the two public facades.
- [x] Omit the temporary compatibility surface because repository consumers
  migrated atomically; retain the existing single canonical registry state.
- [x] Add focused native coverage for the canonical registry and test fixture.
- [x] Complete the stage when Core and its focused native tests build and pass
  through the canonical entry points.

### Stage 2: Migrate production consumers

- [x] Migrate runtime modules first, replacing mount operations with
  `FMountPaths`, general physical-path operations with `FPaths`, and old nested
  types with their `Durin` names.
- [x] Migrate developer, editor, and program targets after runtime consumers so
  dependency direction remains explicit.
- [x] Replace direct `Misc/LexicalPath.h` includes with `Misc/Paths.h`; replace
  mount includes with the selected `Misc/MountPaths.h` public header.
- [x] Update tests colocated with each migrated production module when their
  compile surface changes, while leaving shared mutable-registry fixtures for
  Stage 3.
- [x] Confirm no production consumer includes the test-support header or calls a
  `ForTests` API.
- [x] Complete the stage when all production targets are free of
  `PathUtilities` and the affected runtime/editor/program build targets pass.

### Stage 3: Isolate and migrate mount test support

- [x] Publish the dedicated Core mount test-support header and move the scoped
  registry fixture and registration helper into `Durin::Testing`.
- [x] Migrate all native-test modules to the canonical mount types, facade, and
  test-support names.
- [x] Preserve fixture nesting, registry restoration, publication state, and
  failure reporting behavior; add focused regression coverage if those
  invariants are not already explicit.
- [x] Verify source searches find no repository-owned call to
  `RegisterMountPointForTests` and no use of
  `PathUtilities::FScopedMountRegistryFixture`.
- [x] Complete the stage when affected native-test targets compile and the
  focused Core, AssetCore, AssetRegistry, and Engine test selections pass.

### Stage 4: Remove compatibility surfaces and close the migration

- [x] Remove all `PathUtilities` aliases and forwarding functions.
- [x] Remove `Misc/LexicalPath.h` if it has no independent contract after the
  migration, and update build metadata or umbrella includes accordingly.
- [x] Search all repository-owned C++ and current documentation for stale
  `PathUtilities`, old test fixture, and `EPathExistence` references.
- [x] Update the lasting asset/path documentation to name `FMountPaths` only
  where concrete API names are required; keep behavioral mount contracts
  unchanged.
- [x] Run formatting and the repository-prescribed full build after the public
  Core API migration, followed by the complete applicable native-test set.
- [x] Run changed-document validation and all-plan validation.
- [x] Complete the stage only when compatibility code is gone, all acceptance
  gates pass, and lasting behavior is documented outside this plan.

## Acceptance Gates

- [x] `PathUtilities` has no declaration, definition, include dependency, or
  call site in repository-owned current C++ sources.
- [x] General path algorithms are callable only through `FPaths`; mount-domain
  operations are callable only through `FMountPaths` outside test support.
- [x] Production targets cannot reach mutable test registry helpers through the
  normal mount facade header.
- [x] Mount type layouts, enum values, default arguments, normalization,
  longest-prefix lookup, dependency checks, and error results remain
  behaviorally unchanged.
- [x] Existing virtual paths and package identities require no data migration.
- [x] The full configured build and applicable native tests pass according to
  the repository build and testing workflows.
- [x] Documentation validators pass with no new diagnostics.

## Related Code and Documentation

- `Engine/Source/Runtime/Core/Public/Misc/Paths.h`
- `Engine/Source/Runtime/Core/Public/Misc/MountPaths.h`
- `Engine/Source/Runtime/Core/Public/Misc/MountPathTestSupport.h`
- `Engine/Source/Runtime/Core/Private/Misc/Paths.cpp`
- `Engine/Source/Runtime/Core/Private/Misc/LexicalPath.cpp`
- `Engine/Source/Runtime/Core/Private/Misc/Project.cpp`
- `Engine/Tests/Native/CoreTests/Private/LexicalPathTests.cpp`
- `Engine/Tests/Native/AssetTests/Private/PackageTests.cpp`
- [Asset Packages](../../../Runtime/Assets/AssetPackages.md)
- [Asset Catalog and Mutation](../../../Runtime/Assets/AssetCatalogAndMutation.md)
- [Agent Build and Run Workflow](../../../Agents/BuildAndRun.md)
- [Agent Testing Workflow](../../../Agents/Testing.md)
