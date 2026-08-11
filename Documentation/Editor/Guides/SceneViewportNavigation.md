# Scene Viewport Navigation

The Scene Viewport is used to inspect and navigate a level while editing. Its view is independent of the Camera Actors in the level, so you can move freely without changing the game camera or level content.

## Getting Started

After opening or creating a level, move the pointer over the Scene Viewport. Some keyboard controls require the viewport to have focus. If a shortcut does not respond, click inside the viewport first.

When a level is first opened or selected, the editor copies its initial view from the level's primary camera. From that point onward, the editor view and primary camera are independent.

## Free-Fly Navigation

Hold the right mouse button to enter free-fly navigation:

| Input | Action |
| --- | --- |
| Right-drag | Rotate the view |
| `W` / `S` | Move forward / backward |
| `A` / `D` | Move left / right |
| `Q` / `E` | Move down / up |
| `Shift` + movement key | Move faster |
| Mouse wheel while holding right mouse | Adjust the free-fly speed |

Movement keys can be combined. For example, holding `W` and `D` moves the view forward and to the right.

Starting a right-drag in the Scene Viewport also dismisses an open popup menu and returns focus to the viewport, so camera navigation continues with the same gesture.

## Orbit, Pan, And Dolly

| Input | Action |
| --- | --- |
| `Alt` + left-drag | Orbit around the current focal point |
| Middle-drag | Pan along the current view plane |
| Mouse wheel | Move toward or away from the focal point |

The focal point moves with the view during focusing, panning, and free-fly navigation. Orbit navigation always rotates around the current focal point.

## Focusing The Selected Actor

Select an Actor in the World Outliner, move the pointer over the Scene Viewport, and press `F`. The editor moves the view to the Actor and makes it the new focal point.

The current version focuses on the position of the Actor's root component at a fixed distance. This distance may not be ideal for especially large or small objects; use the mouse wheel to adjust it afterward.

Pressing `F` does nothing when no Actor is selected or the selected Actor has no root component.

## Viewport Editing Modes

The editing-mode selector in the viewport toolbar is separate from the render
mode and the `W`/`E`/`R` transform controls. Every document starts in Select
mode. Contextual modes appear only when their target is selected and editable.

Select mode owns normal Actor and component picking. Clicking a spline drawing
in Select mode selects its Actor/component but does not silently enter Spline
mode. Select the spline component in Details, then choose **Spline** from the
viewport editing-mode selector or click **Edit Spline** in Spline Details.

Entering Play In Editor or another read-only state exits a contextual mode and
cancels an active edit. Returning to editing uses Select mode until another mode
is explicitly activated.

## Transform Gizmo

Selecting one or more Actors displays a native 3D transform gizmo at the selection pivot. The pivot is the average world position of the selected Actor root components.

| Input | Action |
| --- | --- |
| `W` | Translation gizmo |
| `E` | Rotation gizmo |
| `R` | Scale gizmo |
| Left-drag a handle | Apply the selected transform |
| `Ctrl` while dragging | Temporarily enable snapping |
| `Esc` while dragging | Cancel and restore the starting transforms |
| `Ctrl+Z` / `Ctrl+Y` | Undo / redo the completed drag |

Use the viewport toolbar to switch between World and Local axes. The Snap menu configures translation, rotation, and scale increments. Gizmo mode, coordinate space, and snapping settings are stored in the editor session settings.

For multiple selected Actors, translation affects the group uniformly while rotation and scale operate around the shared pivot. When both a parent and its attached child are selected, the parent is transformed directly and the child follows through the attachment hierarchy, avoiding a double transform.

## Editing Splines

Spline mode displays the selected component's curve, control points as solid
box markers, and smaller box markers for manual tangent handles. Marker size and
click targets remain stable on screen as the camera moves. Selection is tied to
stable point identity, so reordering, Undo, and Redo do not switch the selection
to a different logical point.

| Input | Action |
| --- | --- |
| Click a point | Select only that point |
| `Ctrl` + click a point | Add or remove that point from the selection |
| Click a manual tangent handle | Select that handle |
| Click blank viewport space | Clear point/tangent selection |
| Double-click a curve segment | Insert and select a point without changing the segment shape |
| Left-drag the translation handle | Move selected points or the selected manual tangent |
| `F` | Focus the selected points/tangent; fall back to the spline Actor |
| `Delete` | Delete the selected points |
| `Ctrl+D` | Duplicate the primary selected point |
| `Insert` | Append a point |
| `Esc` during a drag | Cancel and restore the starting values |
| `Ctrl+Z` / `Ctrl+Y` | Undo / redo the completed edit |

Point multi-selection exposes one translation target per selected point.
Tangent editing targets one handle at a time. Spline targets support translation
only: `E` and `R` do not rotate or scale points. World/Local axes and temporary
Ctrl snapping use the same toolbar settings and gizmo behavior as Actor
transforms.

Manual-aligned tangents keep both tangent directions collinear while retaining
their authored magnitudes. Manual-broken tangents move independently. Automatic
tangent modes have no draggable handles; switch the selected point's tangent
mode in Details before authoring a handle.

Details shows component settings plus the selected point or points. Multi-point
position edits apply a shared delta and show a mixed-value state when values
differ. Details also provides append, duplicate, delete, reorder, open/close
loop, outgoing interpolation, and tangent-mode actions.

Escape is progressive in Spline mode. The first press cancels an active drag.
With no drag, it clears the point/tangent selection. A later press exits Spline
mode and returns to Select. Deleting the target component, changing documents,
or entering read-only mode performs the same safe cancellation and exit.

## Editor View Versus Game Camera

The editor view is used only for editing the scene:

- Moving the editor view does not move or rotate a Camera Actor.
- View navigation does not mark the level as modified.
- The editor view is not stored when the level is saved.
- The game continues to render through the level's primary Camera Actor.

To change the view used by the game, select a Camera Actor in the World Outliner and edit its Transform or camera properties in the Details panel.

## Mouse Capture During Play

Embedded Play starts with the cursor available for editor controls. Click the
rendered game image, outside the viewport toolbar or an open popup, to capture
and hide the cursor for continuous mouse look. The viewport badge indicates
when a click is required.

Press `Escape` to release the cursor while leaving Play active. The Play
toolbar is then available, and another click in the game image is required to
capture again. New-window Play follows the same click-to-capture and
`Escape`-to-release interaction. Pausing, switching focus to another window,
closing the Play window, stopping Play, or shutting down the editor always
releases the cursor; resuming or returning focus never captures automatically.

## Troubleshooting

### Keyboard movement does not respond

Make sure the pointer is over the Scene Viewport and click inside it to give it focus. The viewport does not process movement shortcuts while a text field is being edited.

### The mouse wheel changes speed instead of moving the view

While the right mouse button is held, the mouse wheel adjusts free-fly speed. Release the right mouse button to use the wheel for moving toward or away from the focal point.

### The view changes after switching levels

This is expected. When another level becomes active, the editor initializes its view from that level's primary camera. Subsequent editor navigation remains independent of the camera.

### Moving the editor view does not change the game view

The editor view and game camera use separate state. Modify a Camera Actor in the level to change the game view.

### Spline mode is not listed

Select the exact Spline component in Details and make sure the document is
editable. Spline mode is contextual and is hidden for an Actor-only selection,
another component type, and read-only/PIE state.

### A spline tangent handle is not visible

Only manual-aligned and manual-broken points expose tangent handles. Select the
point and change its tangent mode in Details.

### Rotation or scale does not affect spline points

Spline point and tangent targets intentionally support translation only. Use
`W` and drag the translation gizmo.
