# Sandbox Gameplay

Summary: Define the first Sandbox-owned playable graybox pawn, controls,
movement limits, camera composition, and native bootstrap.

Modules: Sandbox

## Startup and ownership

`Sandbox/Configs/Project.yaml` selects native module `Sandbox` and exact game
mode `Durin::Sandbox::ADefaultGameMode`. The game mode selects
`ADefaultPlayerController` and `APlayerPawn`; the existing World bootstrap
spawns and possesses them at the first authored `APlayerStart` in stable Actor
order. `NewLevel` contains one intentional start at world location `(0, 0, 0)`.

The pawn owns exactly one `DSimpleGroundMovementComponent`, one
`DStaticMeshComponent`, and one `DCameraComponent`. Restart retains the player
controller and replaces the pawn through `DWorld`. Stop removes only the
runtime game mode, controller, and pawn, leaving authored level Actors intact.

## Controls

`ADefaultPlayerController::BuildControlIntent` is the only Sandbox code that
reads physical input:

| Input | Semantic result |
| --- | --- |
| W / S | forward / backward movement |
| A / D | left / right movement |
| Space | jump held, pressed, and released state |
| Mouse X | pawn yaw |
| Mouse Y | camera pitch, with upward motion looking up |

In editor Play, click the rendered game surface to capture the mouse before
using mouse look. Press `Escape` to release it without stopping Play; click the
game surface again to resume mouse input. Embedded and new-window Play use the
same rule. Focus loss, pause, stop, or closing the Play window also releases
the cursor and clears held gameplay input.

Opposing digital inputs cancel. Digital movement scale is `1.0`; mouse delta
uses `0.1` intent units per pixel and is admitted through the Engine's bounded
one-sample control seam. Focus loss, pause, single-step, restart, and stop use
the shared Engine reset and one-use semantics; Sandbox keeps no duplicate input
cache.

## Movement and camera tuning

The fixed first-slice tune is:

| Setting | Value |
| --- | ---: |
| Ground height | `Z = 0` |
| Maximum horizontal speed | `6 units/s` |
| Horizontal acceleration | `24 units/s²` |
| Horizontal deceleration | `32 units/s²` |
| Gravity | `-20 units/s²` |
| Jump impulse | `8 units/s` |
| Maximum simulated delta | `0.05 s` |
| Look scale | `4°` per intent unit |
| Camera pitch range | `-80°` to `80°` |
| Camera offset | `(-6, 0, 3)` |

Movement input is interpreted in pawn-yaw space. Horizontal acceleration uses
an analytic constant-rate step so the focused 30/60/120 Hz matrix reaches the
same result. Gravity and jump use one vertical velocity; a press edge can jump
only while grounded and is consumed once. Non-finite, zero, or negative delta
does not advance the solver, and positive delta is capped at the documented
maximum.

The pawn applies yaw to its root and pitch to its camera. Level Start therefore
uses the possessed pawn's discoverable camera through the generic controller
view target. Play From Camera remains an explicit host override and does not
change Sandbox camera ownership.

## Visual and limitations

The pawn loads Sandbox-owned `/Game/Models/GrayboxPawn`, imported from
`/Game/Sources/Models/GrayboxPawn.obj`. The unit box is scaled to
`(0.5, 0.5, 1.0)` and offset to `(0, 0, 1)` so its base rests on the ground
plane. If the asset cannot load, the pawn logs the exact path and asset error;
possession, transform-only movement, and camera operation remain valid.

`DSimpleGroundMovementComponent` clamps only to the fixed world-space ground
height. It performs no sweep and does not support arbitrary mesh collision,
capsules, slopes, steps, moving platforms, rigid bodies, or a production
physics backend. `DPhysicsComponent` is intentionally absent so the pawn has
one velocity and transform authority.
