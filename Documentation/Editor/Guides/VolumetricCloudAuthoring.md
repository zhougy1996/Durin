# Author Volumetric Clouds

Summary: Import and inspect volume inputs, configure the global cloud, and diagnose its rendered view.

Last reviewed: 2026-09-03

## Inspect volume inputs

Import a supported row-major PNG atlas through Content Browser, then open the
resulting `DVolumeTexture`. Texture Editor shows build/source metadata and an
exact slice preview. Select the mip, `XY`/`XZ`/`YZ` axis, slice, and channel to
check stored density values. A failed or unavailable build clears the preview;
use the existing Content Browser reimport or source-repair action, then reopen
or refresh the document.

Base density is the required large-scale shape input. Detail density is the
required erosion input. Weather is an optional two-dimensional coverage
control.

## Create and configure the cloud

In the World Outliner, use **Add Actors** and choose Volumetric Cloud. Select
the actor to edit its component. Expand only the Details groups needed for the
current task; property search automatically opens groups containing matches.
The compact **Cloud status** row reports one of:

- **Active**: this eligible component wins the stable scene selection.
- **Ignored**: another eligible component has the higher selection priority.
- **Ineligible**: the shared Engine diagnostic explains the first invalid or
  missing value.

Assign Base and Detail volumes, optionally assign Weather, and tune the Layer,
Mapping and Motion, and Optical Response groups. Use the reveal icon beside an
assigned asset to locate it in Content Browser. These are ordinary reflected
properties, so save/reload, duplication, undo/redo, and world reopen use the
standard editor behavior.

## Choose viewport quality and debug output

Open the viewport view-mode menu and find **Volumetric Clouds**. The available
tiers are Performance, High, Epic, and Reference; **High (Default)** is the
normal production selection. This choice belongs to the viewport session and
is not saved into the cloud or world.

The viewport debug menu provides Radiance, Transmittance, Temporal Status, and
Shadow Visibility. Choose **Reset Cloud Debug View** to return to Lit. Temporal
Status uses green for accepted history, amber for rejected history, and gray
when unavailable. Shadow Visibility is white for unshadowed receivers, dark for
cloud-shadowed receivers, and gray when its source is unavailable.

The rendering-statistics panel reports the last completed cloud route and
fallback reason, target/output extent, sample work, temporal state, and GPU
memory bytes. GPU time reads unavailable by design because the editor adds no
live timing query or rendering-thread wait; use the named qualification gate
for timing evidence.

## Troubleshooting

- If no cloud appears, start with the Details eligibility message and reveal
  each required input.
- If the component is Ignored, change its selection priority or disable/remove
  the winning cloud.
- If a debug source is gray, confirm that temporal history or cloud-shadow
  visibility exists for the current route.
- If rendering falls back from compute, inspect the route reason in rendering
  statistics; fallback remains a valid production path.

## Related documentation

- [Volumetric cloud authoring architecture](../Architecture/VolumetricCloudAuthoring.md)
- [Volume textures](../../Runtime/Assets/VolumeTextures.md)
- [Volumetric cloud scene contract](../../Runtime/Rendering/VolumetricCloudSceneContract.md)
