# Domain Payload Inspection and Repair Plan

Summary: Join domain schema facts to editor, derived, and cooked payload state without a universal element registry.

Last reviewed: 2026-08-23

Status: Active
Completed:

## Current Status

Stage 0 is active. VolumeTexture and Texture2D now provide two production
consumers with different source ownership: VolumeTexture stores normalized
source voxels in `FEditorBulkData`, while Texture2D keeps mounted encoded source
files and transient decoded pixels. Both use domain-owned DDC/TXPL contracts and
the cooked DBLK service. Their shared diagnostic questions can now be extracted
without merging descriptors or codecs.

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

- [ ] Map existing construct-free and live diagnostics for both consumers and
  all three authorities.
- [ ] Separate facts available from package/container bytes from facts requiring
  a live domain object or source mount.
- [ ] Define repair classifications and mutation authority boundaries.
- [ ] Freeze missing, corrupt, stale, unsupported-schema, and orphan baselines.

#### Acceptance Gate

- Two consumers demonstrate the same diagnostic questions without requiring the
  same source descriptor, codec, or fallback policy.

### Stage 1: Add domain-qualified inspection summaries

- [ ] Introduce the narrow summary/hook surface selected by Stage 0.
- [ ] Report domain/schema/count/size/integrity and authority-specific
  placement/availability without a generic element schema.
- [ ] Keep package inspection construct-free and live diagnostics transactional.
- [ ] Add focused VolumeTexture and Texture2D inspection tests.

#### Acceptance Gate

- Tools can trace both consumers across editor, DDC, and cooked state with no
  physical path construction in domain code.

### Stage 2: Add repair and cleanup guidance

- [ ] Classify rebuild, reimport, restore companion, recook, remove orphan, and
  unsupported-schema outcomes with explicit owning authority.
- [ ] Preserve read-only inspection and require explicit mutation workflows.
- [ ] Update editor presentation and lasting contracts.
- [ ] Run focused workflows, aggregate build/tests, and docs validation.

#### Acceptance Gate

- Every diagnosed failure has an actionable owner and safe next operation;
  inspection itself publishes or deletes nothing.

## Related Documentation

- [Large Asset Payload Architecture](../Roadmaps/LargeAssetPayloadArchitecture.md)
- [Asset Packages](../Runtime/Assets/AssetPackages.md)
- [Asset Data Lifecycle and Storage](../Runtime/Assets/AssetDataLifecycle.md)
- [Texture System](../Runtime/Rendering/TextureSystem.md)
- [Volume Textures](../Runtime/Assets/VolumeTextures.md)

## Related Code

- `Engine/Source/Runtime/AssetCore/Public/Asset/PackageInspection.h`
- `Engine/Source/Runtime/AssetCore/Public/Asset/EditorBulkDataStorage.h`
- `Engine/Source/Runtime/AssetCore/Public/Asset/CookedAsset.h`
- `Engine/Source/Runtime/Engine/Public/Texture/Texture2D.h`
- `Engine/Source/Runtime/Engine/Public/Texture/VolumeTexture.h`
- `Engine/Source/Editor/DurinEd/Private/Editor/PropertyView.cpp`
