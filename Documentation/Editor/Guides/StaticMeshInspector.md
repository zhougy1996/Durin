# StaticMesh Inspector

The StaticMesh Inspector provides a read-only view of a StaticMesh asset. Use it
to inspect the rendered mesh, its bounds, material slots, and LOD 0 statistics
without changing the asset.

## Open And Manage Mesh Documents

In the Content Browser, double-click a StaticMesh asset. The editor opens it in
the **StaticMesh Inspector** workspace. Double-clicking the same asset again
activates its existing document; opening a different StaticMesh creates another
closable document.

Close a mesh document from its tab when you are finished. StaticMesh Inspector
documents never become dirty and do not enable Save. Closing one therefore does
not show a save confirmation or modify the asset package.

## Navigate The Preview

Move the pointer over the preview before using these controls:

| Input | Action |
| --- | --- |
| Left-drag | Orbit around the mesh |
| Middle-drag | Pan the focal point |
| Mouse wheel | Zoom in or out |
| **Frame Selection** | Restore the bounds-framed three-quarter view |
| **Wireframe** | Switch between solid and wireframe presentation |

Each open document owns an independent preview. Navigating or changing the
presentation in one document does not affect another document or the level
viewport.

## Read Mesh Information

The details pane reports the asset path, render-resource state, LOD count,
selected LOD, vertex, index, triangle, section and material-slot counts, plus
local bounds. The first inspector exposes reliable existing render data only;
it does not edit topology, collision, sockets, LODs, import settings, or
material assignments.

## Asset Changes And Unavailable Previews

Moving, deleting, or reimporting an asset can temporarily make an open preview
unavailable. The document keeps its identity and shows an unavailable state
until the asset has a valid compatible revision again. A refresh or changed
asset revision also replaces obsolete Content Browser thumbnail work; an old
thumbnail cannot overwrite the new result.

If a package is incompatible, the inspector leaves the current document active
and directs you to **Tools > Asset Compatibility Audit** for details. Missing
render resources, empty geometry, invalid bounds, or failed resource builds are
reported as unavailable rather than displaying stale geometry.

## Troubleshooting

### Double-click does not open the inspector

Confirm that the Content Browser item is an authored StaticMesh asset rather
than a source file or unsupported asset class. Only the exact StaticMesh asset
class is routed to this inspector.

### The preview controls do not respond

Move the pointer over the preview image. Orbit, pan, and wheel input are accepted
only while the preview is hovered.

### Save is disabled

This is expected. StaticMesh Inspector is intentionally read-only.
