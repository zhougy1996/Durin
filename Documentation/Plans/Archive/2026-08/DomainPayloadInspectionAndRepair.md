# Domain Payload Inspection and Repair Plan

Summary: Join domain schema facts to editor, derived, and cooked payload state without a universal element registry.

Last reviewed: 2026-08-23

Status: Archived
Completed: 2026-08-23

## Current Status

All stages are complete. `InspectTexturePayloadPackage` provides construct-free
Texture2D and VolumeTexture summaries by combining domain field trees with
read-only editor storage inspection. `InspectTexturePayloads` joins live source,
DDC, cooked, decoded CPU, and GPU facts. The common texture-domain entry carries
schema/count/size/placement/provenance/state/repair facts but no generic element
schema, backend path, provider, or mutation callback.

Focused fixtures prove mounted-source and external-DABK consumers, missing and
corrupt companions, unsupported TXPL descriptors, orphan reporting, and live
source/DDC/decoded state. Texture editors expose the read-only lifecycle; all
reimport, rebuild, restore, recook, retry, resave, and cleanup actions remain
explicit authority workflows.

## Goal

Provide construct-free and live inspection that identifies the owning domain,
schema, logical size/count, integrity, placement/availability, provenance, and
actionable repair or cleanup for editor, derived, and cooked payloads.

## Scope

- Inventory existing package inspection, property UI, source diagnostics, DDC
  diagnostics, cooked descriptor validation, orphan cleanup, and repair APIs.
- Define a domain-qualified summary contract proven by VolumeTexture and
  Texture2D while keeping their source models distinct.
- Extend construct-free package inspection for domain schema plus physical
  editor/cooked storage facts where possible.
- Add live domain inspection for source/DDC/decoded/runtime state that cannot be
  recovered from package bytes alone.
- Provide actionable repair/cleanup classification without performing hidden
  mutation during inspection.
- Cover missing/corrupt/orphaned payloads and unknown schema versions.

## Non-Goals

- A universal element type registry, shared authored/DDC/cooked descriptor, or
  backend path exposed to domain callers.
- Automatic repair during reads, remote retrieval, async streaming, residency
  budgets, virtualization, or wire-format changes.
- Domain codec ownership in AssetCore.

## Implementation Stages

### Stage 0: Inventory reusable diagnostic questions

- [x] Map existing construct-free and live diagnostics for both consumers and
  all three authorities.
- [x] Separate facts available from package/container bytes from facts requiring
  a live domain object or source mount.
- [x] Define repair classifications and mutation authority boundaries.
- [x] Freeze missing, corrupt, stale, unsupported-schema, and orphan baselines.

#### Acceptance Gate

- Two consumers demonstrate the same diagnostic questions without requiring the
  same source descriptor, codec, or fallback policy.

### Stage 1: Add domain-qualified inspection summaries

- [x] Introduce the narrow summary/hook surface selected by Stage 0.
- [x] Report domain/schema/count/size/integrity and authority-specific
  placement/availability without a generic element schema.
- [x] Keep package inspection construct-free and live diagnostics transactional.
- [x] Add focused VolumeTexture and Texture2D inspection tests.

#### Acceptance Gate

- Tools can trace both consumers across editor, DDC, and cooked state with no
  physical path construction in domain code.

### Stage 2: Add repair and cleanup guidance

- [x] Classify rebuild, reimport, restore companion, recook, remove orphan, and
  unsupported-schema outcomes with explicit owning authority.
- [x] Preserve read-only inspection and require explicit mutation workflows.
- [x] Update editor presentation and lasting contracts.
- [x] Run focused workflows, aggregate build/tests, and docs validation.

#### Acceptance Gate

- Every diagnosed failure has an actionable owner and safe next operation;
  inspection itself publishes or deletes nothing.

## Related Documentation

- [Large Asset Payload Architecture](../../../Roadmaps/LargeAssetPayloadArchitecture.md)
- [Asset Packages](../../../Runtime/Assets/AssetPackages.md)
- [Asset Data Lifecycle and Storage](../../../Runtime/Assets/AssetDataLifecycle.md)
- [Texture System](../../../Runtime/Rendering/TextureSystem.md)
- [Volume Textures](../../../Runtime/Assets/VolumeTextures.md)

## Related Code

- `Engine/Source/Runtime/AssetCore/Public/Asset/PackageInspection.h`
- `Engine/Source/Runtime/AssetCore/Public/Asset/EditorBulkDataStorage.h`
- `Engine/Source/Runtime/AssetCore/Public/Asset/CookedAsset.h`
- `Engine/Source/Runtime/Engine/Public/Texture/Texture2D.h`
- `Engine/Source/Runtime/Engine/Public/Texture/VolumeTexture.h`
- `Engine/Source/Runtime/Engine/Public/Texture/TexturePayloadInspection.h`
- `Engine/Source/Editor/TextureEditor/Private/Widgets/MTextureEditor.cpp`
- `Engine/Source/Editor/TextureEditor/Private/Widgets/MVolumeTextureEditor.cpp`
