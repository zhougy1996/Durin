# Play In Editor

The Level Editor can run the current in-memory level without modifying its asset.
Use the **Play** menu or these shortcuts:

- `F5`: start or stop Play
- `Ctrl+F5`: play from the level start in a separate window
- `F6`: pause or resume
- `F7`: advance one frame while paused

The Play menu also offers **Play From Camera**, the equivalent start modes in a
separate game window, and a physics toggle. Play From Start uses the level's
primary camera. Play From Camera creates a transient camera at the current editor
view and never adds that camera to the editor level.

Play creates a transient copy of the current level, switches the scene viewport to
the copied level's primary camera, and begins the world/actor/component runtime
lifecycle. Unsaved editor changes are included in the copy. Runtime changes are
discarded when Play stops.

The World Outliner switches to the runtime world during Play. Actor and component
selection remains available, while structural operations and Details values are
read-only. Content and level document operations remain disabled. Focus the scene
viewport to route keyboard and mouse input to gameplay. In new-window mode, input
is enabled only while the game window is active; closing that window stops Play.

## Applying runtime changes

**Apply Selected Runtime Changes** and **Apply All Runtime Changes** explicitly
copy reflected `Edit` properties from actors that originated in the editor level.
Internal runtime references are mapped back to their editor counterparts. Actor
ownership arrays, transient values, runtime-spawned actors, and the temporary Play
From Camera actor are not copied. Applying marks the editor package dirty; Stop
still discards every runtime value that was not applied.

## Console commands

- `pie.play`
- `pie.stop`
- `pie.pause [on|off|toggle]`
- `pie.step`
- `pie.physics <on|off>`
- `pie.apply`
- `pie.status`

The reflected `Physics Component` supplies the current built-in physics baseline:
linear velocity, gravity, restitution, and collision with a configurable horizontal
ground plane. It runs in PIE and standalone game worlds and obeys the world physics
toggle. Arbitrary mesh collision, constraints, angular dynamics, and a third-party
physics backend remain future work.
