# Volumetric Cloud Scene Contract

Summary: Defines reflected global-cloud authoring, deterministic scene selection, immutable render-thread publication, and the P1 Renderer handoff.

Modules: Engine, Renderer

Last reviewed: 2026-08-21

## Authoring boundary

`AVolumetricCloudActor` owns one default root
`DVolumetricCloudComponent`. Generic reflection-driven Details exposes enabled
state, priority, required base/detail `DVolumeTexture` assets, optional weather
`DTexture2D`, layer bounds, maximum distance, density frequencies and offsets,
coverage, erosion, extinction, light extinction, and ambient contribution.

Component vectors use reflected `FVector2f`/`FVector3f` authoring values and
copy directly into the P1 float snapshot at publication. Finite setters clamp
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

## Identity, selection, and mutation

Each component serializes a persistent `FGuid` and allocates a nonserialized
runtime instance ID. Class defaults allocate neither. Duplication preserves the
persistent ID but receives a new instance ID.

The render-thread registry selects eligible candidates by:

1. priority descending;
2. persistent GUID ascending;
3. component object path ascending; and
4. runtime instance ID ascending.

Every complete publication increments a component revision. Add/replace accepts
only a revision newer than the stored entry; removal requires an exact revision
match. Duplicate and stale commands are no-ops, so an old unregister cannot
remove a newer replacement. Disabled, hidden, invalid, or missing-input state
publishes an ineligible replacement and allows the next candidate to take over.
Scene release clears all entries and counted references on the render thread.

## Thread and ownership boundary

`FVolumetricCloudSceneData` contains plain selection and physical values plus
three `FRHITextureReferenceRef` values. `FVolumetricCloudSceneProxy` owns one
immutable value and `FVolumetricCloudSceneInfo` owns the proxy. No actor,
component, reflected object, mutable container, raw backend handle, render
target, shader, history, or pipeline crosses from Engine authoring to the
render thread.

During view preparation Renderer copies the selected snapshot, resolves base
and detail as `Texture3D` and weather as `Texture2D`, derives the to-light vector
and radiance from the selected directional light, and supplies its own density
sampler and scene depth. The remaining authored values map field-for-field into
the frozen P1 parameter block. Renderer retains the P1 primary/light sample
counts and transmittance cutoff.

## Validation boundary

`VolumetricCloudSceneContractTests` covers defaults, clamping, identity,
serialization, registration, deterministic selection, ineligible fallback,
stale mutation rejection, removal, and exact parameter translation without GPU
initialization. `VolumetricCloudSceneVulkanTests` drives the production scene
path from a real actor and volume assets through offscreen, Present, resize,
invalid-input, compute, and fragment routes in inline and threaded execution.
Generic property editing uses the standard transaction system; specialized
cloud panels and previews remain deferred.

## Related documents

- [Volumetric cloud spatial rendering](VolumetricCloudSpatialRendering.md)
- [Volume textures](../Assets/VolumeTextures.md)
- [Volumetric Cloud Rendering roadmap](../../Roadmaps/VolumetricCloudRendering.md)
