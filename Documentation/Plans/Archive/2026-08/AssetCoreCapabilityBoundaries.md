# AssetCore Capability Boundaries Plan

Summary: Separate AssetCore public capabilities so runtime consumers do not inherit authoring, inspection, and Cook implementation contracts.

Last reviewed: 2026-08-21

Status: Archived
Completed: 2026-08-21

## Current Status

All stages are complete. Public declarations and consumers now use separate
runtime, authoring, Cook, and tools entry points. Shared package identity,
inspection, persistence, reference traversal, runtime cooked payload, and Cook
publication have separate leaf headers; `Package.h` remains a compatibility
aggregate. AssetCore and two final full repository builds passed, and all 95
AssetPackageTests passed. Changed-document and all-plan validation passed.
Physical module decomposition is deferred because all current implementations
share the private catalog/runtime state and AssetCore introduces no
authoring-only third-party dependency for a runtime target to shed.

## Goal

- Give runtime loading, authored mutation, Cook publication, and offline tools
  distinct public entry points.
- Keep value types in the narrowest owning headers so public Engine asset types
  do not include unrelated AssetCore capabilities.
- Preserve source compatibility through deliberate forwarding headers where it
  does not perpetuate ambiguous ownership.
- Decide physical module decomposition from target and dependency evidence after
  the public capability boundary is measurable.

## Scope

- AssetCore public headers and their internal include graph.
- AssetCore and direct consumer includes under Engine and Sandbox.
- Long-lived asset documentation describing the resulting public boundary.
- A bounded evaluation of whether authoring or Cook code must move to another
  binary module now.

## Non-Goals

- No `.dasset`, catalog-cache, reference-cache, Cook manifest, or bulk-container
  format changes.
- No load, save, mutation, compatibility, or Cook behavior changes.
- No replacement of the process-wide default asset runtime in this plan.
- No renaming of public C++ types solely for cosmetic consistency.

## Design Decisions and Invariants

- Entry points are named by capability rather than relative size; no new
  `AssetMinimal.h` catch-all is introduced.
- `Asset.h` is the ordinary runtime-consumer entry point.
- Authoring mutation and mounted-source operations are not reachable through
  `Asset.h`.
- Cook construction/publication and offline compatibility/resave tools have
  explicit entry points.
- Engine public headers include the narrow leaf contract they use instead of an
  AssetCore umbrella.
- Existing behavior, serialized formats, ABI-relevant layouts, and thread
  contracts remain unchanged.

## Current Foundations and Gaps

- V4 package readers, writers, and version policy are already private.
- `AssetTools.h` already distinguishes compatibility and canonical-resave APIs,
  but includes the current all-capability `Asset.h`.
- `CookedAsset.h` combines runtime payload descriptors and loading with Cook
  container encoding, manifest publication, and `FCookContext`.
- `Package.h` combines runtime load-report types, package persistence, and
  full-package inspection.
- `Mutation.h` contains extension registration, relocation, fix-up, deletion,
  creation, and saving in one 400-line contract.

## Implementation Stages

### Stage 0: Establish the capability map

- [x] Classify public types and functions as runtime, authoring, Cook, or tools.
- [x] Record the selected entry-point and leaf-header graph.
- [x] Identify direct consumers that currently depend on transitive includes.

#### Acceptance Gate

- The plan passes the repository plan validator.
- Every current public declaration has one selected capability owner.

### Stage 1: Split narrow leaf contracts

- [x] Separate runtime package/load types from persistence and inspection APIs.
- [x] Separate cooked payload descriptors/runtime loading from Cook construction
  and publication APIs.
- [x] Split authoring mutation declarations where a smaller cohesive boundary is
  required to remove unrelated dependencies.
- [x] Keep compatibility forwarding headers only where they provide a clear
  migration path.

#### Acceptance Gate

- AssetCore builds with no public header relying on accidental transitive
  includes.
- Runtime leaf headers do not expose authoring mutation, package writing,
  compatibility probing, or Cook publication APIs.

### Stage 2: Publish entry points and migrate consumers

- [x] Make `Asset.h` the runtime entry point.
- [x] Add explicit authoring and Cook entry points and narrow `AssetTools.h`.
- [x] Replace umbrella includes in public Engine headers with exact leaf headers.
- [x] Migrate implementation consumers to the narrowest practical entry point.

#### Acceptance Gate

- Game/runtime consumers can load assets without seeing authoring or Cook APIs.
- Engine public asset headers no longer include `Asset.h` solely for cooked
  descriptor or Cook-context declarations.
- Direct AssetCore consumers compile after the migration.

### Stage 3: Evaluate the binary ownership boundary

- [x] Measure remaining runtime target dependencies on authoring, Cook, and tool
  implementations.
- [x] Split a binary module only if it removes code or dependencies from runtime
  targets without introducing a reverse dependency or duplicating shared state.
- [x] Record the selected outcome and rationale.

#### Acceptance Gate

- The module decision is supported by target dependency and implementation-state
  ownership evidence.
- Any selected module split preserves dependency direction and runtime behavior.

### Stage 4: Validate and document the contract

- [x] Update authoritative asset documentation with the entry-point and ownership
  rules.
- [x] Run changed-document and all-plan validation.
- [x] Build AssetCore and all directly affected modules.
- [x] Run the registered AssetCore test target and broader coverage justified by
  the final dependency blast radius.

#### Acceptance Gate

- Documentation validation passes.
- Selected builds and tests pass.
- No package-format or runtime behavior change is present.

## Validation Matrix

| Concern | Validation |
| --- | --- |
| Plan lifecycle and links | Changed-document and all-plan validation |
| Public include self-sufficiency | AssetCore and direct-consumer builds |
| Package/load behavior | Registered AssetCore package tests |
| Cross-module API migration | Full build when public Engine headers change |
| Runtime/editor boundary | Target dependency review and runtime-variant build evidence |

## Definition of Done

- Public entry points map unambiguously to runtime, authoring, Cook, and tools.
- Runtime consumers no longer receive authoring or offline-tool contracts through
  `Asset.h`.
- Public Engine asset headers use narrow AssetCore leaf contracts.
- The physical module decision and lasting ownership rules are documented.
- Required builds, tests, and documentation validation pass.

## Deferred Follow-ups

- Instance-based asset runtimes for isolated tools and parallel tests.
- A unified structured error model for existing `bool` plus output-diagnostic
  APIs.

## Related Documentation

- [Asset Packages](../../../Runtime/Assets/AssetPackages.md)
- [Asset Catalog And Mutation](../../../Runtime/Assets/AssetCatalogAndMutation.md)
- [Asset Data Lifecycle](../../../Runtime/Assets/AssetDataLifecycle.md)
- [Code Modules](../../../Workspace/CodeModules.md)

## Related Code

- `Engine/Source/Runtime/AssetCore/Public/Asset.h`
- `Engine/Source/Runtime/AssetCore/Public/AssetTools.h`
- `Engine/Source/Runtime/AssetCore/Public/Asset/`
- `Engine/Source/Runtime/AssetCore/Private/`
- `Engine/Source/Runtime/AssetCore/AssetCore.dmodule`
