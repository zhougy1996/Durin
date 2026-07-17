# Play In Editor

The Level Editor can run the current in-memory level without modifying its asset.
Use the **Play** menu or these shortcuts:

- `F5`: start or stop Play
- `F6`: pause or resume
- `F7`: advance one frame while paused

Play creates a transient copy of the current level, switches the scene viewport to
the copied level's primary camera, and begins the world/actor/component runtime
lifecycle. Unsaved editor changes are included in the copy. Runtime changes are
discarded when Play stops.

Editing panels and level document actions are disabled during Play. Focus the
scene viewport to route keyboard and mouse input to gameplay; moving focus back to
editor UI disables gameplay input and clears held input state.
