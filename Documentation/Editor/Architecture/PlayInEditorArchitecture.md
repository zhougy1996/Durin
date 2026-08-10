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

The editor level is detached from the active scene without being destroyed.
After duplication, PIE reads the same Engine-owned project `Game` settings as
standalone startup, loads the optional native module, and resolves only the
configured fully qualified `AGameMode`. It then starts the duplicate with an
explicit World play request. A configured resolution or bootstrap failure
retires the duplicate and restores the editor world, viewport, input, and
source/runtime object maps without publishing Playing. Stopping reverses a
successful transition after draining
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

Native play publishes one local player controller, its possessed pawn, and a
controller-owned view target. Level Start uses the newly possessed pawn as the
default view target. Play From Camera creates the existing transient
`PIE_EditorCamera` and passes it as an explicit bootstrap view-target override.
Rendering can target the embedded scene viewport or a dedicated Mona window;
the engine retains and restores the editor viewport across the latter session.
Destroying the target clears it immediately, after which normal camera fallback
applies.

## Physics And Input

`DPhysicsComponent` is the initial runtime physics layer. It integrates linear
velocity and gravity and resolves a horizontal ground plane. `DWorld` owns the
simulation enable flag so pause, single-step, PIE, standalone games, and console
control all share the same lifecycle. This is a foundation rather than a
general collision backend.

`FGameInputState` remains the Engine-owned raw key, mouse-button,
mouse-position, mouse-delta, and wheel snapshot. Only the local
`APlayerController` or a derived player controller translates that snapshot
into `FPawnControlIntent`; Pawn and movement code never read raw key identities.
Standalone games receive the native window input stream. PIE enables that
stream only while its embedded scene viewport is focused. Focus loss, input
disable, pause, stop, and session replacement clear raw or pending semantic
state at their ownership boundaries. A paused frame produces no intent;
single-step admits one current input snapshot and consumes its transitions
once.

The World owns pause, single-step, native restart, possession teardown, and
runtime Actor cleanup. PIE owns only host isolation and restoration. Repeated
sessions therefore load the same settings and bootstrap path but receive fresh
runtime roles and no retained controller, pawn, view target, or semantic input.
The opt-in `--editor-pie-lifecycle-smoke` process diagnostic repeats this
contract for embedded/new-window and Level Start/Play From Camera combinations
after full editor initialization; it is never enabled by ordinary startup.

## Editor Integration

The active workspace receives Save, Undo, and Redo commands from the host.
Shared reflected-property transactions remain editor-wide across document and
project transitions and are cleared at explicit PIE lifecycle boundaries.

## Related Documentation

- `Documentation/Editor/Guides/PlayInEditor.md`
- `Documentation/Editor/Architecture/ReflectedPropertyEditing.md`
- `Documentation/Runtime/Core/RuntimeLifecycle.md`
- `Documentation/Runtime/Rendering/ViewportRendering.md`
