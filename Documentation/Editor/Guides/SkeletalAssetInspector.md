# Skeletal Asset Inspector

Summary: Import a glTF/GLB skeletal scene once, then inspect its independent skeletal assets in the editor.

Last reviewed: 2026-08-26

## Import a skeletal scene

1. In the Content Browser, open the destination directory and choose **Import >
   Scene Source (FBX/glTF)**.
2. Select a mounted `.gltf`/`.glb` source, or ingest an external source and its
   external buffers into a mounted source destination.
3. Review the static settings and destination. Complete parsing, output
   discovery, collision checks, and construction start only after confirmation.
4. Confirm the import. The Content Browser reveals the published output
   directory. Skeleton, SkeletalMesh, AnimationClip, StaticMesh, Material, and
   Texture outputs are peers; there is no primary skeletal asset.

Use the **Skeletal assets** filter to isolate Skeleton, SkeletalMesh, and
AnimationClip packages. Generated outputs are ordinary independent assets.
Scene import is creation-only: it provides no whole-scene reimport, missing-output
recreation, repair, or reverse ownership navigation. Import a revised source to
a fresh destination.

## Inspect and preview

Double-click an exact Skeleton, SkeletalMesh, or AnimationClip asset. Skeleton
documents show the parent-before-child hierarchy and reference translations.
Mesh documents show Skeleton compatibility, LOD 0 counts and bounds, sections,
palette size, material slots, and payload storage. Clip documents show Skeleton
compatibility, duration, counts, and clipped track rows with bone, path,
interpolation, and key count.

SkeletalMesh documents start in reference pose. AnimationClip documents expose
metadata but do not automatically discover a matching mesh. Use **Frame
Selection**, **Lit**, **Wireframe**, orbit, pan, and zoom for an available mesh.
Playback controls apply only when a compatible mesh/clip pair has been explicitly
bound by the host. These controls do not dirty the asset.

Drag a SkeletalMesh asset from the Content Browser into an editable Scene
Viewport to create a Skeletal Mesh Actor at the drop location. The actor starts
in reference pose. Assign a compatible AnimationClip and playback settings on
its SkeletalMeshComponent when the level needs animated playback; Skeleton,
and AnimationClip assets are not directly placeable.

When no explicit compatible peer is bound, metadata remains inspectable and
animation playback is unavailable. Missing packages, incompatible peers,
payload failures, and render-resource failures likewise do not retain stale
geometry or pose.

## Qualify

For production handoff, save and reload the assets, cook the target, and launch
the Shipping game through the repository build profile. Runtime-only loading
uses the cooked Skeleton, SkeletalMesh, AnimationClip, and payloads without the
editor module, source files, or DDC fallback.

## Related documentation

- [Skeletal Asset Editor architecture](../Architecture/SkeletalAssetEditor.md)
- [Mounted Source Workflows](MountedSourceWorkflows.md)
- [Asset Import Framework](../Architecture/AssetImportFramework.md)
- [Skeletal Animation Playback](../../Runtime/Animation/SkeletalAnimationPlayback.md)
