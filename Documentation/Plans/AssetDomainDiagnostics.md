# Asset Domain Diagnostics Plan

Summary: Replace the AssetRegistry-owned catch-all asset result with Core diagnostics, CoreDObject package-format ownership, and module-owned AssetRegistry and Engine results.

Last reviewed: 2026-09-01

Status: Completed
Completed: 2026-09-01

## Current Status

All stages are complete. Core now owns `FDiagnostic`; CoreDObject is the
sole source of canonical DAST format identity, versions, supported-reader policy,
and projection fingerprint; AssetRegistry exposes only its typed Registry result;
and Engine owns the runtime asset-operation result. AssetRegistry, Engine, and
ContentBrowser compile with explicit Registry-to-Engine translation. Changed
documentation and all-plan validation pass. `test affected` selected and passed
47 native-test targets covering Core, CoreDObject, AssetRegistry, Engine, asset
packages, Cook, editor workflows, and downstream runtime consumers.

## Goal

- Give Core one domain-neutral structured diagnostic value for logs, UI, tools,
  and cross-domain aggregation.
- Make CoreDObject authoritative for canonical package-format constants and
  reader policy.
- Give AssetRegistry a Registry-specific error/result contract.
- Make Engine own the existing runtime asset-operation error/result contract so
  existing Engine and editor APIs do not force that contract into AssetRegistry.
- Preserve dependency direction `Core -> CoreDObject -> AssetRegistry -> Engine`.

## Selected Boundaries

- `FDiagnostic` carries human-facing domain, code, severity, message, and optional
  context. It does not replace strongly typed domain error enums.
- `EAssetRegistryError` and `FAssetRegistryResult` cover scanning, bounded header
  projection, publication, snapshots, and Registry cache behavior.
- `EAssetError` and `FAssetResult` remain the Engine runtime asset-operation
  contract during this migration and move to an Engine-owned public header.
- Canonical DAST identity/version constants live in CoreDObject. AssetRegistry may
  expose no independent copy of those values.
- No new runtime module and no dependency edge from AssetRegistry to Engine are
  introduced.

## Implementation Stages

### Stage 0: Establish lower-level contracts

- [x] Add the Core structured diagnostic value and focused contract coverage.
- [x] Move canonical package-format constants and reader-policy helpers under
  CoreDObject ownership.
- [x] Update direct package-format consumers without retaining duplicate values.

### Stage 1: Split Registry and Engine results

- [x] Add the AssetRegistry-specific error/result contract and migrate all
  AssetRegistry public/private APIs to it.
- [x] Move the runtime asset-operation result contract into Engine and migrate
  Engine public headers to include their owning contract directly.
- [x] Convert Registry failures explicitly at Engine publication, refresh, and
  query boundaries.
- [x] Remove `AssetRegistry/Result.h` and prove AssetRegistry exports no
  `FAssetResult` or `EAssetError` contract.

### Stage 2: Validate and document the resulting boundary

- [x] Add or update focused tests for typed errors and structured diagnostics.
- [x] Update the implemented runtime asset contract and module-routing guidance.
- [x] Run changed-document validation and the repository-selected affected tests.
- [x] Record validation evidence, complete the plan, and commit the isolated
  implementation with exact Plan and Stage provenance.

## Acceptance Gates

- Core diagnostics contain no Asset, Registry, package, or Engine enum.
- CoreDObject is the sole authority for canonical DAST format identity and reader
  policy.
- AssetRegistry builds and tests without Engine and exposes only its own typed
  result.
- Engine and downstream consumers compile against an Engine-owned asset-operation
  result.
- No production include of `AssetRegistry/Result.h` remains.
- Documentation validators and affected native tests pass.

## Related Code

- [`Core public headers`](../../Engine/Source/Runtime/Core/Public)
- [`CoreDObject package format`](../../Engine/Source/Runtime/CoreDObject/Public/DObject/PackageFormat.h)
- [`AssetRegistry public headers`](../../Engine/Source/Runtime/AssetRegistry/Public/AssetRegistry)
- [`Engine asset public headers`](../../Engine/Source/Runtime/Engine/Public/Asset)
- [`Asset catalog and mutation contract`](../Runtime/Assets/AssetCatalogAndMutation.md)
