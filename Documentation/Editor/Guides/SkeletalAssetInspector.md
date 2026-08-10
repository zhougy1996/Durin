# Skeletal Asset Inspector

Summary: Import, inspect, preview, and reimport glTF/GLB skeletal asset graphs in the editor.

Last reviewed: 2026-08-11

## Import a skeletal scene

1. In the Content Browser, open the destination directory and choose **Import >
   Scene Source (FBX/glTF)**.
2. Select a mounted `.gltf`/`.glb` source, or ingest an external source and its
   external buffers into a mounted source destination.
3. Review every populated peer row. Each row names its output role, management
   action, destination, and CPU/GPU/disk estimate. Review warnings and captured
   source dependencies before confirming.
4. Confirm the import. The Content Browser reveals the published output
   directory. Skeleton, SkeletalMesh, AnimationClip, StaticMesh, Material, and
   Texture outputs are peers; there is no primary skeletal asset.

Use the **Skeletal assets** filter to isolate Skeleton, SkeletalMesh, and
AnimationClip packages. Their record actions always operate on the entire owning
record. Use **Reimport**, **Recreate Missing Outputs**, source repair, or record
navigation from any managed peer; do not treat one generated peer as an
independent source authority.

## Inspect and preview

Double-click an exact Skeleton, SkeletalMesh, or AnimationClip asset. Skeleton
documents show the parent-before-child hierarchy and reference translations.
Mesh documents show Skeleton compatibility, LOD 0 counts and bounds, sections,
palette size, material slots, and payload storage. Clip documents show Skeleton
compatibility, duration, counts, and clipped track rows with bone, path,
interpolation, and key count.

SkeletalMesh documents start in reference pose and list compatible clips from
the same import record. AnimationClip documents choose compatible same-record
meshes by stable path. Use **Frame Selection**, **Lit**, **Wireframe**, orbit,
pan, zoom, **Play/Pause**, **Loop**, **Rate**, **Reset**, and the timeline. These
controls belong to the open document and do not dirty the asset.

If no compatible same-record peer exists, metadata remains inspectable and the
preview reports that it is unavailable. Missing packages, incompatible peers,
payload failures, and render-resource failures likewise do not retain stale
geometry or pose.

## Reimport and qualify

Save packages normally, then reimport from any managed peer. Changed and
unchanged reimports publish one record graph revision; open documents and
SkeletalMesh thumbnails refresh from package, dependency, and render-resource
revisions. Missing outputs can be recreated transactionally, while removed
outputs remain explicit orphans.

For production handoff, save and reload the graph, cook the target, and launch
the Shipping game through the repository build profile. Runtime-only loading
uses the cooked Skeleton, SkeletalMesh, AnimationClip, and payloads without the
editor module, import records, source files, or DDC fallback.

## Related documentation

- [Skeletal Asset Editor architecture](../Architecture/SkeletalAssetEditor.md)
- [Mounted Source Workflows](MountedSourceWorkflows.md)
- [Asset Import Framework](../Architecture/AssetImportFramework.md)
- [Skeletal Animation Playback](../../Runtime/Animation/SkeletalAnimationPlayback.md)
