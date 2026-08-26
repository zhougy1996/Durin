# Skeletal Asset Editor

Summary: Defines editor ownership, document identity, preview, thumbnail, and lifecycle contracts for Skeleton, SkeletalMesh, and AnimationClip assets.

Last reviewed: 2026-08-26

## Ownership

`SkeletalMeshEditor` owns one read-only workspace and exact asset routes for
`DSkeleton`, `DSkeletalMesh`, and `DAnimationClip`. `DurinEd` continues to own
workspace hosting, preview scenes, auxiliary viewports, and the bounded rendered
thumbnail service. `AssetForge` owns Scene translation, planning, construction,
and publication. Scene outputs are independent after the creation transaction.

Documents are keyed by the workspace manager's class-qualified document key and
virtual asset path. Opening the same class and path focuses the existing tab;
different exact classes cannot alias. A document retains counted asset and
selected-peer references only while open. The workspace never reports dirty or
supports save/discard because all controls are session-only.

## Inspection and discovery

Skeleton hierarchy and animation track tables preserve canonical array order and
use ImGui clipping, so widget work is bounded by visible rows. Mesh inspection
uses immutable summary, payload, material, palette, compatibility, DDC, and
cooked metadata. Ordinary Content Browser cards use registry/package metadata
and do not load skeletal assets.

The editor performs no automatic Scene peer discovery. A SkeletalMesh
document can render its own reference pose. AnimationClip metadata remains
inspectable without inventing a mesh association; playback requires a mesh and
clip to be bound explicitly by a host that can validate Skeleton compatibility.

## Preview and playback

Each preview document owns one `Editor::FPreviewScene`, actor,
`DSkeletalMeshComponent`, light, viewport, and camera controller. The component
is the production runtime component and therefore owns binding validation,
reference pose, animation evaluation, palette publication, bounds, materials,
and render proxy creation. Play, pause, loop, rate, reset, and seek call that
component directly; camera, lit/wireframe, peer selection, and playback state
never mutate an authored package.

Binding is complete-or-unavailable. The previous clip is cleared before a new
mesh/clip pair is validated, and a failed binding clears both component inputs
before hiding the viewport. Inactive workspaces disable their auxiliary views.
Closing a tab releases its preview before asset references; module shutdown
removes the thumbnail provider before unregistering workspace routes.

## Thumbnail contract

Only exact `DSkeletalMesh` assets register a rendered provider. Its visual is a
transparent, elevated three-quarter, reference-pose, LOD 0 image using default
material slots. The persistent key includes the provider schema, preview fixture,
shader contract, fixed output settings, and the registry dependency closure
rooted at the mesh, which covers its Skeleton and material/texture dependencies.

Package fingerprints are captured before a cache miss loads the asset. The
session initializes production mesh resources, attaches a production skeletal
component to the shared preview scene, and validates its asset-local render
resource revision before publication. Skeleton and AnimationClip retain their
ordinary icons and never request rendered jobs.

## Related documentation

- [Asset Import Framework](AssetImportFramework.md)
- [Asset Thumbnails](AssetThumbnails.md)
- [Skeletal Asset Inspector guide](../Guides/SkeletalAssetInspector.md)
- [Skeletal Animation Playback](../../Runtime/Animation/SkeletalAnimationPlayback.md)
- [Skeletal Mesh Rendering](../../Runtime/Rendering/SkeletalMeshRendering.md)
