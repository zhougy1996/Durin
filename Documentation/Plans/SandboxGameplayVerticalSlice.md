# Sandbox Gameplay Vertical Slice Plan

Summary: Build the first Sandbox-owned graybox pawn, controller, movement, camera, and authored start on the completed native gameplay core.

Last reviewed: 2026-08-11

Status: Active
Completed:

## Current Status

Gameplay Foundation G1 is complete. Engine now owns the generic pawn,
controller, possession, game-mode bootstrap, semantic intent, movement, view
target, restart, PIE, and standalone contracts. Sandbox contains only the
cross-module reflection qualification fixture and still has no gameplay
policy or playable loop.

G2 is ready to implement entirely inside Sandbox. The entry baseline is the
completed [Native Gameplay Core](NativeGameplayCore.md), including exact
`Game.NativeModule`/`Game.GameModeClass` selection and deterministic first
`APlayerStart` bootstrap. Stage 0 must audit the smallest reusable graybox mesh
path and freeze tuning before production behavior changes.

## Goal

Make the Sandbox project start one deterministic local player in PIE and
DurinGame, move and jump a visible graybox pawn on the documented horizontal
ground plane, drive a pawn camera from semantic input, restart cleanly, and
stop without adding game-specific policy to Engine.

## Scope

- Sandbox-owned reflected pawn, player controller, game mode, and simple
  ground-movement component under `Durin::Sandbox`.
- C++ mapping from the existing raw input snapshot to `FPawnControlIntent`.
- One visible graybox body and one pawn-owned camera component.
- Fixed, documented first-slice movement, jump, look, and camera tuning.
- `Game.NativeModule: Sandbox` plus the exact fully qualified game-mode class.
- One authored player start in the existing Sandbox default level.
- Focused native tests and bounded PIE/standalone playable validation.

## Non-Goals

- New generic Engine gameplay abstractions or changes to the G1 ownership
  boundaries without a demonstrated defect.
- Arbitrary mesh collision, slopes, steps, capsules, rigid-body interaction,
  navigation, animation, root motion, or a production physics backend.
- Input assets, rebinding UI, gamepads, multiple local players, AI, replay,
  networking, replication, rollback, or prediction.
- Camera manager, spring arm, collision probes, blending, effects, or
  production-quality camera feel.
- Production character art, skeletal assets, animation, audio, UI, combat,
  inventory, saving, or level-design expansion.

## Design Decisions and Invariants

### Module and reflected identities

- Concrete types are `Durin::Sandbox::APlayerPawn`,
  `Durin::Sandbox::ADefaultPlayerController`,
  `Durin::Sandbox::ADefaultGameMode`, and
  `Durin::Sandbox::DSimpleGroundMovementComponent`.
- Sandbox exports and lists each reflected header in `Sandbox.dmodule`.
  Engine gains no Sandbox include, dependency, tuning value, or class branch.
- `Configs/Project.yaml` selects logical module `Sandbox` and exact class
  `Durin::Sandbox::ADefaultGameMode`. The qualification-only G1 fixture is
  never selected by project settings.

### Input and control

- `ADefaultPlayerController::BuildControlIntent` is the only Sandbox code that
  reads raw keys or mouse state. W/S produce forward move, A/D produce lateral
  move, Space produces held/pressed/released jump state, and mouse delta
  produces look intent.
- Opposing digital inputs cancel before the inherited bounded admission seam.
  Focus loss, disable, pause, single-step, restart, and stop retain the G1 reset
  semantics; Sandbox does not cache a second raw or semantic input state.
- Input scale and mouse sensitivity are named Sandbox constants with focused
  tests. G2 does not add an input-asset abstraction.

### Movement and ground limitation

- `DSimpleGroundMovementComponent` is the pawn's sole movement authority and
  owns horizontal acceleration/deceleration, vertical velocity, gravity, jump
  impulse, and the ground-plane contact decision through the inherited
  velocity value.
- The first solver integrates the pawn transform directly and clamps only
  against a fixed world-space ground height. It does not claim arbitrary
  collision, slopes, steps, sweeps, or interaction with authored meshes.
- Horizontal intent is interpreted in pawn yaw space. Jump is admitted only
  while grounded, and the press edge is consumed once. Delta time is bounded
  against non-positive input and tuning is deterministic across the focused
  frame-rate matrix.
- `DPhysicsComponent` is not added to the pawn because it would introduce a
  second velocity and transform authority. Its separate Actor baseline remains
  unchanged.

### Pawn visual and camera

- `APlayerPawn` owns its inherited root, one movement component, one
  `DStaticMeshComponent` graybox visual, and one `DCameraComponent`.
- The graybox asset is Sandbox-owned and intentionally simple. Missing visual
  data must not invalidate possession or movement; it produces an actionable
  asset diagnostic and leaves the transform-only pawn usable.
- Look intent applies yaw to the pawn and bounded pitch to the camera
  composition. The camera component is discoverable on the possessed pawn, so
  the existing controller view target and Engine fallback require no special
  Sandbox branch.

### Bootstrap, authoring, and teardown

- `ADefaultGameMode` selects the Sandbox controller and pawn classes through
  the existing native virtuals. It does not own or cache spawned instances.
- The existing default level receives one intentional `APlayerStart`; stable
  Actor order remains the authoritative start selection.
- Restart keeps the controller and replaces the pawn through `DWorld`; Sandbox
  code does not implement a parallel restart or possession path.
- PIE Level Start uses the pawn camera, Play From Camera retains the explicit
  host override, and stop removes only runtime gameplay Actors. Authored level
  state and settings remain stable across repeated sessions.

## Current Foundations and Gaps

| Area | G1 foundation | G2 gap |
| --- | --- | --- |
| Bootstrap | Exact module/class resolver, game mode, player start, rollback, restart | Configure Sandbox class and author one start |
| Control | Raw player-controller boundary and bounded one-use intent | Map Sandbox keys and mouse |
| Movement | Abstract single pawn-owned authority and velocity | Implement documented ground-plane solver |
| Camera | Possessed-pawn view target and camera-component fallback | Compose pawn camera and look policy |
| Visual | Static-mesh component and asset pipeline | Supply one Sandbox graybox mesh/material |
| Validation | World/PIE/standalone diagnostics and full G1 matrix | Prove the complete playable slice and tuning |

## Implementation Stages

### Stage 0: Freeze the playable contract and baseline

Dependencies: completed G1.

- [ ] Record the G1 source revision and focused Sandbox, World, viewport, and
  asset baseline.
- [ ] Audit the smallest current Sandbox-owned graybox asset path and record
  whether a new imported asset or existing suitable placeholder is selected.
- [ ] Freeze class identities, key/mouse mapping, movement axes, fixed ground
  height, acceleration, speed, gravity, jump, yaw, pitch, and camera offset.
- [ ] Add failing focused fixtures for reflection, mapping, movement,
  frame-rate behavior, camera, bootstrap, restart, and teardown.
- [ ] Confirm the existing authored default level can accept one player start
  without unrelated asset migration.

#### Acceptance Gate

- Every concrete identity, tune, asset, input, movement, camera, and authored
  level change is explicit, and the unchanged G1 baseline is green.

### Stage 1: Add Sandbox gameplay roles and configuration

Dependencies: Stage 0.

- [ ] Add and reflect the Sandbox game mode, player controller, pawn, and
  movement component.
- [ ] Select the Sandbox controller and pawn from the game mode and install the
  movement component as the pawn's single authority.
- [ ] Migrate `Project.yaml` to the exact Sandbox native module/class pair.
- [ ] Add one stable player start to the Sandbox default level.
- [ ] Prove exact cross-module lookup, transactional spawn, possession,
  restart, and runtime-only teardown with the concrete classes.

#### Acceptance Gate

- PIE and standalone resolve the exact Sandbox game mode and publish one
  controller/pawn pair at the authored start; failure leaves no partial roles.

### Stage 2: Implement input, movement, visual, and camera

Dependencies: Stage 1.

- [ ] Implement the fixed raw-to-semantic mapping only in the Sandbox player
  controller.
- [ ] Implement deterministic horizontal acceleration/deceleration, gravity,
  ground contact, and single-edge jump in the movement component.
- [ ] Add the graybox static-mesh visual with actionable missing-asset behavior.
- [ ] Add pawn yaw, bounded camera pitch, and the fixed camera composition.
- [ ] Prove focus reset, pause suppression, single-step, restart, frame-rate
  consistency, ground-plane limitation, camera selection, and Play From Camera
  override behavior.

#### Acceptance Gate

- The visible pawn moves, jumps, lands, and looks through semantic intent with
  one movement and camera authority, and tests state the solver's limitations.

### Stage 3: Qualify and document the slice

Dependencies: Stages 1-2.

- [ ] Publish Sandbox gameplay controls, tuning, ground-plane limitation,
  camera, bootstrap, restart, and failure behavior in lasting documentation.
- [ ] Run the smallest affected targets during development and the final full
  native suite because Engine tests, Sandbox tests, assets, and hosts cross.
- [ ] Complete full DurinEditor and DurinGame `all` builds.
- [ ] Run bounded threaded and inline-RHI PIE/standalone smokes for move, look,
  jump, land, pause/step, restart, Play From Camera, stop, and repeated entry.
- [ ] Record validation and commit provenance, complete G2 in the roadmap, and
  leave G3 conditional until a second concrete consumer exists.

#### Acceptance Gate

- The Sandbox default level is a deterministic playable graybox slice in PIE
  and standalone, all validation passes, and no game-specific policy moved into
  Engine.

## Validation Matrix

| Boundary | Required evidence |
| --- | --- |
| Reflection/config | Exact nested identities, module readiness, configured game mode |
| Bootstrap | Authored start, concrete role selection, possession, rollback, restart |
| Input | W/S/A/D cancellation, jump edges, mouse look, focus and pause reset |
| Movement | Acceleration, speed, gravity, landing, jump, delta bounds, frame-rate matrix |
| Camera | Pawn view, yaw/pitch bounds, Level Start, Play From Camera, restart |
| Visual | Graybox loads and renders; missing asset remains actionable and non-structural |
| Lifecycle | PIE/standalone start, pause/step, stop, repeated entry, clean exit |
| Scope | No Engine tuning or Sandbox branches; no implied production collision |

## Definition of Done

- Sandbox config selects one concrete native game mode that spawns and
  possesses one visible pawn at the authored start.
- Raw input becomes bounded semantic control only in the Sandbox controller.
- The pawn moves and jumps through one documented ground-plane movement
  authority and drives one pawn-owned camera.
- Restart, pause/step, Play From Camera, stop, and repeated sessions preserve
  the completed G1 ownership and cleanup guarantees.
- Focused/full tests, Editor/Game builds, runtime smokes, documentation, and
  roadmap handoff all pass with no new generic Engine layer.

## Deferred Follow-ups

- Arbitrary collision, capsules, slopes, steps, moving platforms, and a
  production physics backend remain separate work.
- Input assets, rebinding, gamepads, camera effects, spring arms, and camera
  collision require demonstrated product needs.
- Skeletal visuals, animation, audio, UI, combat, saving, AI, networking, and
  authored gameplay content remain later gameplay milestones.
- G3 remains unopened until a second concrete controllable body, input source,
  or camera behavior demonstrates duplicated policy.

## Related Documentation

- [Gameplay Foundation Roadmap](../Roadmaps/GameplayFoundation.md)
- [Native Gameplay Core](NativeGameplayCore.md)
- [Level System](../Runtime/World/LevelSystem.md)
- [Play In Editor Architecture](../Editor/Architecture/PlayInEditorArchitecture.md)
- [Play In Editor Guide](../Editor/Guides/PlayInEditor.md)
- [Viewport Rendering](../Runtime/Rendering/ViewportRendering.md)
- [Workspace And Projects](../Workspace/WorkspaceProjects.md)
- [Build And Run](../Development/Build/BuildAndRun.md)
- [Native C++ Tests](../Development/Build/NativeTests.md)

## Related Code

- `Sandbox/Source/Runtime/Sandbox/Sandbox.dmodule`
- `Sandbox/Source/Runtime/Sandbox/Public/`
- `Sandbox/Source/Runtime/Sandbox/Private/`
- `Sandbox/Configs/Project.yaml`
- `Sandbox/Content/Levels/NewLevel.dasset`
- `Sandbox/Tests/Native/`
- `Engine/Source/Runtime/Engine/Public/Actors/`
- `Engine/Source/Runtime/Engine/Public/Components/PawnMovementComponent.h`
- `Engine/Source/Runtime/Engine/Public/Gameplay/PawnControlIntent.h`
- `Engine/Tests/Native/EngineTests/Private/World/NativeGameplayCoreTests.cpp`
