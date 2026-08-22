# Volumetric Cloud Scene Authoring

Summary: Define reflected cloud-component eligibility, actionable Details status,
and recovery across volume-texture resource replacement.

Modules: Engine, Renderer, DurinEd

Last reviewed: 2026-08-21

## Component contract

`AVolumetricCloudActor` owns one `DVolumetricCloudComponent`. Base and Detail
`DVolumeTexture` references are required; Weather `DTexture2D` is optional. The
remaining reflected layer, density-mapping, optical, priority, and enable values
are authored through the generic Details panel. No cloud-specific editor is
required for this contract.

`DiagnoseVolumetricCloudEligibility` is the shared Engine authority. The first
failure wins in this order: disabled, hidden owner, missing Base, invalid or
unbuilt Base, missing Detail, invalid or unbuilt Detail, invalid layer, invalid
maximum distance, invalid density mapping, invalid optical parameters, then
ready. Missing Weather does not make a cloud ineligible. The result contains a
stable reason, eligibility bit, and corrective message.

The component exposes the message as an `Edit`, `ReadOnly`, `Transient` string.
Generic Details refreshes the derived value before drawing, so it is never saved
as authored state and cannot diverge through editing. Component construction,
registration, visibility changes, property edits, setters, and scene publication
also refresh it.

## Resource recovery

Scene data carries the stable `FRHITextureReference` objects for assigned Base
and Detail assets. Authored eligibility and texture assignment are frozen in the
scene proxy, while Renderer rechecks whether each assigned reference currently
resolves before selecting the active cloud. Successful import or reimport updates
the existing reference, so a formerly unbuilt asset becomes renderable without
toggling another component property. Failed replacement preserves the referenced
last-known-good resource; unload or deletion clears it and removes the candidate
from active selection.

Renderer receives no diagnostic string or editor state. It consumes only the
immutable cloud values, counted texture references, identity/revision fields, and
eligibility bit.

## Related Documentation

- [Volume textures](../Assets/VolumeTextures.md)
- [Volumetric cloud spatial rendering](VolumetricCloudSpatialRendering.md)

## Related Code

- `Engine/Source/Runtime/Engine/Public/Components/VolumetricCloudComponent.h`
- `Engine/Source/Runtime/Engine/Public/Engine/VolumetricCloudSceneProxy.h`
- `Engine/Source/Runtime/Renderer/Private/Scene.cpp`
- `Engine/Source/Editor/DurinEd/Private/Editor/PropertyView.cpp`
