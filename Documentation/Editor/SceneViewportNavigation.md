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

## Editor View Versus Game Camera

The editor view is used only for editing the scene:

- Moving the editor view does not move or rotate a Camera Actor.
- View navigation does not mark the level as modified.
- The editor view is not stored when the level is saved.
- The game continues to render through the level's primary Camera Actor.

To change the view used by the game, select a Camera Actor in the World Outliner and edit its Transform or camera properties in the Details panel.

## Troubleshooting

### Keyboard movement does not respond

Make sure the pointer is over the Scene Viewport and click inside it to give it focus. The viewport does not process movement shortcuts while a text field is being edited.

### The mouse wheel changes speed instead of moving the view

While the right mouse button is held, the mouse wheel adjusts free-fly speed. Release the right mouse button to use the wheel for moving toward or away from the focal point.

### The view changes after switching levels

This is expected. When another level becomes active, the editor initializes its view from that level's primary camera. Subsequent editor navigation remains independent of the camera.

### Moving the editor view does not change the game view

The editor view and game camera use separate state. Modify a Camera Actor in the level to change the game view.
