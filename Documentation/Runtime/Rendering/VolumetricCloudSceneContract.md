# Volumetric Cloud Scene Contract

Summary: Defines reflected global-cloud authoring, deterministic scene selection, immutable render-thread publication, and the Renderer handoff.

Modules: Engine, Renderer, DurinEd

Last reviewed: 2026-09-03

## Authoring boundary

`AVolumetricCloudActor` owns one default root
`DVolumetricCloudComponent`. Generic reflection-driven Details exposes enabled
state, priority, required base/detail `DVolumeTexture` assets, optional weather
`DTexture2D`, layer bounds, maximum distance, density frequencies and offsets,
coverage, erosion, extinction, light extinction, and ambient contribution.

Component vectors use reflected `FVector2f`/`FVector3f` authoring values and
copy directly into the float snapshot at publication. Finite setters clamp
priority to `[-1000, 1000]`, layer altitude to plus or minus ten million world
units, distance to `[1, 10'000'000]`, frequencies to `[0.00000001, 1]`, offsets
to plus or minus one million, and optical scalars to `[0, 1]`. Non-finite edits
are rejected. An inverted layer is preserved for serialization and undo but
makes the candidate ineligible.

Base and detail assets require valid platform data and counted texture
references. Missing or invalid required inputs commit an ineligible candidate;
missing weather is valid and selects the Renderer-owned white fallback.
Sampling counts, target resolution, temporal policy, route selection, scene
depth, density sampler, and the qualification fragment override are not
authored properties.

## Eligibility and Resource Recovery

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

## Identity, selection, and mutation

Each component serializes a persistent `FGuid`. Class defaults allocate none,
and duplication preserves the persistent ID.

The render-thread registry selects eligible candidates by:

1. priority descending;
2. persistent GUID ascending;
3. component object path ascending.

Each rebuilt proxy receives a cloud-specific nonzero history key, independent
of registry ownership. `FVolumetricCloudSceneRegistry` keys membership by the
exact proxy pointer. Component render-state hooks call only
`FSceneInterface::RemoveVolumetricCloud(this)` followed by
`AddVolumetricCloud(this)`; Renderer-private `FScene` constructs the detached
complete Desc and commits the raw removal token only after command admission
succeeds.
Disabled, hidden, invalid, or missing-input state publishes an ineligible
rebuilt candidate and allows the next candidate to take over. Explicit Scene
release clears entries and counted references on the render thread before
deferred Scene deletion.

## Thread and ownership boundary

`FVolumetricCloudSceneData` contains physical values plus three
`FRHITextureReferenceRef` values. `FVolumetricCloudSceneProxyDesc` owns the
immutable data, persistent identity, selection key, and history key;
`FVolumetricCloudSceneInfo` owns the proxy after render-thread
attachment. The component retains only the non-owning proxy token needed for
exact removal. No actor,
component, reflected object, mutable container, raw backend handle, render
target, shader, history, or pipeline crosses from Engine-authored state to the
render thread.

During view preparation Renderer copies the selected Desc, combines its history
key with the selected lighting key, resolves base
and detail as `Texture3D` and weather as `Texture2D`, derives the to-light vector
and radiance from the selected directional light, and supplies its own density
sampler and scene depth. The remaining authored values map field-for-field into
the immutable spatial parameter block. Renderer owns sample counts and
transmittance cutoff; current quality policy is defined by
[Temporal Reconstruction](VolumetricCloudTemporalReconstruction.md).

## Validation boundary

`VolumetricCloudSceneContractTests` covers defaults, clamping, identity,
serialization, registration, deterministic selection, ineligible fallback,
ordered rebuild, exact-pointer removal, history invalidation, and exact parameter translation without GPU
initialization. `VolumetricCloudSceneVulkanTests` drives the production scene
path from a real actor and volume assets through offscreen, Present, resize,
invalid-input, compute, and fragment routes in inline and threaded execution.
Generic property editing uses the standard transaction system. Cloud Details
presentation and volume previews are defined by
[Volumetric Cloud Authoring](../../Editor/Architecture/VolumetricCloudAuthoring.md).

## Related documents

- [Volumetric cloud spatial rendering](VolumetricCloudSpatialRendering.md)
- [Volume textures](../Assets/VolumeTextures.md)
- [Volumetric Cloud Rendering roadmap](../../Roadmaps/Archive/2026-08/VolumetricCloudRendering.md)

## Related Code

- `Engine/Source/Runtime/Engine/Public/Components/VolumetricCloudComponent.h`
- `Engine/Source/Runtime/Engine/Public/Rendering/VolumetricCloudSceneProxy.h`
- `Engine/Source/Runtime/Renderer/Private/Scene.cpp`
- `Engine/Source/Editor/DurinEd/Private/Editor/PropertyView.cpp`
