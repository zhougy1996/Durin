# Play In Editor Architecture

Summary: Define editor-to-runtime world duplication, session ownership, input, ticking, and teardown.

Modules: LevelEditor, Engine, Launch

This document defines the ownership, isolation, input, and restoration contracts
for Play In Editor (PIE). User-facing controls are documented in
`Documentation/Editor/Guides/PlayInEditor.md`.

## World Isolation

`DEditorEngine` keeps the persistent editor world separate from a transient PIE
world. Starting Play duplicates only the level's owned object tree; references
to assets outside that tree remain shared.

The editor level is detached from the active scene without being destroyed. The
PIE level is then registered and begun, and the viewport falls back to the PIE
level's primary camera. Stopping reverses the transition after draining
scene-removal render commands, then marks the complete transient PIE world
hierarchy as garbage through the Outer index. Objects intended to survive the
session must be explicitly reparented before retirement.

## Session State

PIE supports Playing and Paused states plus single-frame stepping. Runtime
changes are discarded with the transient world and do not dirty the editor
level package unless the user explicitly applies reflected editable values
through the session's source/runtime object map.

Structural ownership and runtime-only objects are excluded from Apply. During
Play, Outliner and Details bind to the runtime world in read-only mode.

Play can use the level's primary camera or a transient camera built from the
editor view. Rendering can target the embedded scene viewport or a dedicated
Mona window; the engine retains and restores the editor viewport across the
latter session.

## Physics And Input

`DPhysicsComponent` is the initial runtime physics layer. It integrates linear
velocity and gravity and resolves a horizontal ground plane. `DWorld` owns the
simulation enable flag so pause, single-step, PIE, standalone games, and console
control all share the same lifecycle. This is a foundation rather than a
general collision backend.

Gameplay code reads the current key, mouse-button, mouse-position, mouse-delta,
and wheel state from `GEngine->GetGameInputState()`. Standalone games receive
the native window input stream. PIE enables that stream only while its embedded
scene viewport is focused.

## Editor Integration

The active workspace receives Save, Undo, and Redo commands from the host.
Shared reflected-property transactions remain editor-wide across document and
project transitions and are cleared at explicit PIE lifecycle boundaries.

## Related Documentation

- `Documentation/Editor/Guides/PlayInEditor.md`
- `Documentation/Editor/Architecture/ReflectedPropertyEditing.md`
- `Documentation/Runtime/Core/RuntimeLifecycle.md`
- `Documentation/Runtime/Rendering/ViewportRendering.md`
