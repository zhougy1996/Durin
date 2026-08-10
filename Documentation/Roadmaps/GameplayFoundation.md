# Gameplay Foundation Roadmap

Summary: Establish a native C++ gameplay foundation around possession, semantic control, graybox movement, camera ownership, and deterministic PIE startup without requiring scripting or production character assets.

Last reviewed: 2026-08-11

Status: Active
Completed:

## Current Status

The Actor/Component, World/Level, PIE, raw game-input, camera, static-mesh, and
limited ground-plane physics foundations exist. `Sandbox` currently contains
only its runtime shell and a cross-module qualification fixture; it has no
gameplay policy or playable loop.

Milestones G0 and G1 are complete. The native core now provides exact reflected
gameplay roots, possession, semantic control, movement and view-target
boundaries, transactional bootstrap/restart, shared project settings, and
qualified PIE/standalone hosts. G2 is active through the
[Sandbox Gameplay Vertical Slice](../Plans/SandboxGameplayVerticalSlice.md)
plan. It owns the first concrete keys, movement tune, camera composition,
graybox visual, and authored start entirely inside Sandbox.

| Milestone | Status | Child plan |
| --- | --- | --- |
| G0: Namespace-safe reflected type resolution | Complete | [DHT Namespace-Aware Type Resolution](../Plans/DHTNamespaceAwareTypeResolution.md) |
| G1: Native possession and player bootstrap | Complete | [Native Gameplay Core](../Plans/NativeGameplayCore.md) |
| G2: Sandbox graybox playable slice | Active | [Sandbox Gameplay Vertical Slice](../Plans/SandboxGameplayVerticalSlice.md) |
| G3: Evidence-gated framework extraction | Conditional | Create only after a second concrete consumer exists |

## Outcome

Durin can enter PIE or a standalone game, select a native game mode, create one
local player controller and default pawn at a player start, establish
possession, translate raw device state into semantic pawn control, move and
jump a graybox pawn, drive its camera, and tear the session down without stale
runtime state.

The required program delivers:

- minimal native `APawn`, `AController`, `APlayerController`, `AGameMode`,
  `APlayerStart`, and `DPawnMovementComponent` contracts in the root `Durin`
  namespace;
- one explicit possession owner and symmetric possess/unpossess lifecycle;
- a semantic control boundary that can later be produced by players, AI,
  replay, networking, or scripts without exposing raw key codes to a pawn;
- one Sandbox-owned graybox pawn, player controller, game mode, movement
  implementation, and camera composition;
- deterministic PIE/standalone startup, pause/single-step behavior, restart,
  teardown, and focused regression coverage; and
- lasting runtime/editor documentation for the contracts proven by the slice.

## Scope

- Native C++ gameplay lifecycle on the game thread.
- Single local player creation, possession, release, and restart.
- Default pawn and player-start selection for one active world and level.
- Raw `FGameInputState` consumption by the local player controller.
- A bounded semantic pawn-control value for move, look, jump transitions, and
  future source-neutral production.
- A general pawn-movement component boundary plus one intentionally simple
  Sandbox ground-movement implementation.
- Existing `DCameraComponent` composition and a minimal controller view-target
  contract suitable for one local player.
- Placeholder static geometry and the existing limited physics foundation for
  the first playable slice.
- Focused runtime, PIE, input, lifecycle, and standalone validation.

## Non-Goals

- C#, Lua, Verse, visual graphs, script hosting, hot reload, or generated script
  bindings.
- Actor Definition, Blueprint, prefab, archetype, editable class-default, or
  general authored Actor-template systems.
- Requiring skeletal rendering, animation graphs, root motion, production
  character meshes, or completed character art.
- `ACharacter`, capsule collision, a production character-movement solver,
  slope/step handling, climbing, swimming, vehicles, or navigation.
- Enhanced-input assets, rebinding UI, split screen, multiple local players,
  gamepads, touch, or platform input abstraction beyond the selected slice.
- AI controllers, behavior trees, gameplay abilities, attributes, inventory,
  quests, save games, networking, replication, rollback, or prediction.
- A player-camera manager, spring-arm collision, camera blending, cinematic
  cameras, or a general camera-effects stack.
- Turning Sandbox-specific movement, input mapping, or camera tuning into an
  Engine contract before another consumer proves the common behavior.

## Program Decisions and Invariants

### Names and domain ownership

- Foundational, frequently used game-framework types remain in the root
  `Durin` namespace beside `AActor`, `DWorld`, and `DActorComponent`:
  `APawn`, `AController`, `APlayerController`, `AGameMode`, `APlayerStart`, and
  `DPawnMovementComponent`.
- The public semantic control value is provisionally named
  `FPawnControlIntent`; G1 may refine its fields but may not expose physical key
  codes or Sandbox policy through the Engine API.
- Concrete game types live in `Durin::Sandbox` and use role-bearing short names,
  such as `APlayerPawn`, `ADefaultPlayerController`, `ADefaultGameMode`, and
  `DSimpleGroundMovementComponent`. Sandbox does not redeclare short names such
  as `APawn` or `AController` in a nested namespace.
- Module and namespace identity are independent. A future physical module split
  does not rename serialized reflected identities merely to mirror a directory.

### Ownership and lifecycle

- `AController` is an Actor, not an ActorComponent. It owns one current pawn
  association; a pawn exposes the reciprocal controller association without
  becoming the controller's structural child.
- Possession is changed only through one authoritative operation that detaches
  both sides before attaching the new pair. Repeated possession, self-
  possession, invalid world/level ownership, pending destruction, and session
  teardown have deterministic results.
- World, level, actor, and component lifetime remain owned by the existing
  Actor/Component system. Gameplay adds no second object registry, manual delete
  path, or lifecycle scheduler.
- The first product target is one game-thread local player. Threading,
  replication authority, and remote-player state are not implied by the native
  class names.

### Input and control

- `FGameInputState` remains a raw per-frame device snapshot owned by the Engine.
  Only the player-controller/input boundary reads key and mouse identities.
- Pawns and movement components consume semantic control intent. AI or replay
  can later produce the same intent without impersonating keyboard input.
- Edge-triggered actions are represented explicitly and consumed once per game
  tick. Pause and single-step preserve the existing World and PIE tick policy.
- The first mapping is Sandbox-owned C++ policy. No reflected input asset or
  general action-mapping framework is introduced by the required milestones.

### Movement, physics, and camera

- `DPawnMovementComponent` defines ownership, control consumption, velocity,
  and movement update boundaries; it does not claim production character-
  collision semantics.
- `DSimpleGroundMovementComponent` may use the existing horizontal ground-plane
  foundation to prove acceleration, gravity, landing, and jumping. Unsupported
  arbitrary collision remains explicit rather than being hidden behind a
  `DCharacterMovementComponent` name.
- The graybox pawn uses placeholder static geometry. Visual and animation state
  do not own movement authority.
- The existing `DCameraComponent` remains the camera primitive. The first slice
  needs a camera pivot and controller-selected view target, not a player-camera
  manager or spring arm.

### Scope control

- G0 must complete before reflected Gameplay/Sandbox types rely on nested-
  namespace references. Temporary fully qualified spellings are not accepted
  as a permanent workaround for an incorrect DHT lookup contract.
- Each implementation milestone extracts only behavior required by its current
  consumer. A second independently useful consumer is the entry gate for G3.
- Script-provider hooks, behavior registries, general prefab data, and managed
  handles are not placed in native gameplay APIs speculatively.

## Current Foundations and Gaps

| Area | Reusable foundation | Gap owned by this roadmap |
| --- | --- | --- |
| Reflection | Fully qualified runtime identities, generated helpers, reflected inheritance, serialization | Lexical scope-aware short/relative type resolution and ambiguity diagnostics |
| Actor model | Actor/component ownership, default components, BeginPlay/Tick/EndPlay, safe mutation and destruction | Pawn/controller roles and authoritative possession |
| World/Level | One active level, spawn/destroy, play state, pause/single-step, persistent actors | Game-mode bootstrap, player start, controller/default-pawn creation and restart |
| PIE | Transient duplicated world, input focus, camera fallback, teardown isolation | Gameplay bootstrap and player-camera selection inside the PIE lifetime |
| Input | Current key/mouse state and one-tick transitions | Semantic mapping and source-neutral pawn control |
| Movement | Actor transforms and lightweight gravity/ground-plane physics | Pawn movement contract and a controlled graybox implementation |
| Camera | `ACameraActor`, `DCameraComponent`, primary level camera | Controller view target and pawn camera composition |
| Rendering | Static mesh component and graybox-capable scene rendering | No required gap; skeletal rendering is not an entry dependency |
| Sandbox | Runtime module linkage to Core and Engine | No concrete game mode, controller, pawn, movement, camera, or playable loop |

## Milestone Map

### G0: Namespace-safe reflected type resolution

Required: yes.

Dependencies: existing DHT qualified symbol model and export manifests.

Deliverable: DHT resolves base and property type references using deterministic
lexical C++ namespace scope, rejects true ambiguity with candidate diagnostics,
and preserves fully qualified serialized identities across same-module and
cross-module generation.

Entry gate: the current resolver limitation is reproduced and bounded by the
active child plan.

Exit gate: every acceptance gate in the
[DHT Namespace-Aware Type Resolution Plan](../Plans/DHTNamespaceAwareTypeResolution.md)
passes, generated output remains order-independent, and the full DHT/shared-
generation qualification required by repository guidance succeeds.

### G1: Native possession and player bootstrap

Required: yes.

Dependencies: G0.

Deliverable: root-namespace native framework types provide possession, one
local player, game-mode startup, player-start selection, default-pawn spawn,
restart, semantic control transfer, movement abstraction, and view-target
selection without game-specific tuning.

Entry gate: satisfied 2026-08-11. G0's handoff is stable; the Engine, World,
Level, PIE, input, camera, module-loading, project-setting, and test entry
points were audited. The selected configuration boundary is one
`Game.NativeModule` plus one fully qualified `Game.GameModeClass`, owned beside
the migrated `Game.DefaultLevel` setting and shared by PIE and standalone.

Exit gate: satisfied 2026-08-11. Focused and full native tests, full Editor/Game
builds, and threaded/inline runtime diagnostics prove possession symmetry,
rollback, restart, semantic input, camera fallback, repeated PIE restoration,
standalone native lifecycle, and unchanged ordinary Actor behavior. Long-lived
contracts are published in their Runtime, Level, PIE, viewport, and workspace
owners.

Completed child plan: [Native Gameplay Core](../Plans/NativeGameplayCore.md).

### G2: Sandbox graybox playable slice

Required: yes.

Dependencies: G1 and the existing PIE, static-mesh, camera, input, and limited
physics foundations.

Deliverable: Sandbox enters PIE and standalone play with one default controller
possessing one graybox pawn at a player start; move, look, jump, landing, camera
follow, pause/single-step, stop, and restart produce deterministic state.

Entry gate: satisfied 2026-08-11. G1 types, host integration, lifecycle tests,
and process diagnostics are complete; the selected graybox level and
placeholder visual require no skeletal asset or production collision backend.

Exit gate: a focused automated matrix plus editor and standalone smoke proves
the complete start-to-stop loop, repeated PIE sessions leak no control or input
state, and game-specific tuning remains outside Engine.

Active child plan: [Sandbox Gameplay Vertical Slice](../Plans/SandboxGameplayVerticalSlice.md).

### G3: Evidence-gated framework extraction

Required: no. Conditional on a second concrete controllable body, input source,
or camera behavior that exposes duplicated policy after G2.

Dependencies: completed G2 measurements and named consumers.

Possible deliverable: only the smallest proven shared contract for a second
pawn type, AI controller, vehicle, input mapping, camera boom, or richer
movement family.

Entry gate: two working consumers demonstrate duplicated semantics and agree on
ownership, lifecycle, failure behavior, and naming. Anticipated reuse alone is
not sufficient.

Exit gate: extracted behavior reduces duplication without expanding the
required baseline into scripting, templates, abilities, networking, or a
production character stack. If no consumer appears, record the milestone as
not activated when this roadmap completes.

## Child Plan Boundaries

| Plan | Owns | Must not own |
| --- | --- | --- |
| DHT Namespace-Aware Type Resolution | Symbol lookup semantics, parser/resolver propagation, diagnostics, caches, DHT tests and generation qualification | Pawn/controller design or gameplay APIs |
| Native Gameplay Core | Generic root-namespace types, possession, bootstrap, semantic control boundary, movement abstraction, view target, Engine tests | Sandbox key bindings/tuning, script hooks, production character movement |
| Sandbox Gameplay Vertical Slice | Concrete game types, graybox composition, input mapping, simple ground movement, camera tuning, playable validation | Generalizing one consumer into broad Engine systems |
| Conditional extraction | Evidence-backed common behavior from at least two consumers | Speculative framework layers or roadmap expansion without a named requirement |

## Program Validation Matrix

| Contract | Focused evidence | Program outcome |
| --- | --- | --- |
| DHT identity | Scope, ambiguity, same/cross-module, container and generated-code tests | Namespaced game types serialize and generate deterministically |
| Possession | Pairing, transfer, invalid world, destruction and repeated-operation tests | Controller and pawn never disagree about ownership |
| Bootstrap | Game mode, start selection, spawn failure, restart and teardown tests | One local player starts and stops predictably |
| Input | Raw-to-semantic mapping, press/hold/release and focus tests | Pawn behavior contains no physical key dependency |
| Movement | Acceleration, gravity, landing, jump and bounded failure tests | Graybox motion is deterministic without claiming full collision support |
| Camera | View-target selection, fallback, transform and session tests | PIE and standalone render from the controlled pawn coherently |
| Lifecycle | pause, single-step, stop/re-enter PIE, actor destruction and level switch | No stale controller, pawn, input, camera, or movement state survives |
| Scope | dependency and symbol audit | No script, skeletal, Actor-template, networking, or speculative subsystem becomes required |

## Risks and Control Gates

- A globally unique DHT short name can conceal incorrect lexical lookup until a
  second namespace adds the same short name. G0 adds deliberate collisions and
  removes insertion-order or global-uniqueness dependence.
- Copying UE's full class hierarchy would front-load networking, character,
  camera, and player-state policy. G1 admits only types exercised by G2.
- Existing `DPhysicsComponent` is intentionally limited. G2 must report that
  boundary and may not market ground-plane behavior as general collision.
- Coupling movement to a skeletal component would block graybox iteration and
  invert ownership. The pawn/movement contract remains visual-independent.
- GameMode selection can accidentally create a second project/plugin framework.
  G1 Stage 0 must select one minimal bootstrap seam from existing project/world
  ownership and defer configurable Actor templates.
- Editor smoke alone can miss lifecycle drift. Repeated PIE and standalone
  automation are required before G2 closes.

## Completion Criteria

- Required milestones G0, G1, and G2 pass their exit gates.
- PIE and standalone execute the graybox player loop without scripts or
  skeletal assets.
- Possession, semantic control, bootstrap, movement abstraction, camera
  selection, and teardown have focused automated evidence.
- General framework types remain in `Durin`; concrete game policy remains in
  `Durin::Sandbox`.
- G3 is either completed from named evidence or explicitly recorded as not
  activated.
- Lasting DHT, Runtime, World, and PIE behavior is documented in the owning
  domains, and completed child plans retain validation and commit provenance.

## Related Documentation

- [Generated Reflection System](../Runtime/Core/ReflectionSystem.md)
- [Level System](../Runtime/World/LevelSystem.md)
- [Runtime Lifecycle](../Runtime/Core/RuntimeLifecycle.md)
- [Play In Editor Architecture](../Editor/Architecture/PlayInEditorArchitecture.md)
- [Viewport Rendering](../Runtime/Rendering/ViewportRendering.md)
- [Workspace And Projects](../Workspace/WorkspaceProjects.md)
- [Build And Run](../Development/Build/BuildAndRun.md)
- [Native C++ Tests](../Development/Build/NativeTests.md)
- [Actor Component System Plan](../Plans/Archive/2026-07/ActorComponentSystem.md)

## Related Code

- `Engine/Source/Programs/DurinHeaderTool/durin_header_tool/resolver/reflection_resolver.py`
- `Engine/Source/Programs/DurinHeaderTool/durin_header_tool/parser/reflection_parser.py`
- `Engine/Source/Runtime/Engine/Public/Engine/Actor.h`
- `Engine/Source/Runtime/Engine/Public/Engine/World.h`
- `Engine/Source/Runtime/Engine/Public/Engine/Level.h`
- `Engine/Source/Runtime/Engine/Public/Input/GameInputState.h`
- `Engine/Source/Runtime/Engine/Public/Components/ActorComponent.h`
- `Engine/Source/Runtime/Engine/Public/Components/CameraComponent.h`
- `Engine/Source/Runtime/Engine/Public/Components/PhysicsComponent.h`
- `Engine/Source/Editor/DurinEd/Private/Editor/EditorEngine.cpp`
- `Engine/Tests/Native/EngineTests/Private/World/`
- `Engine/Source/Programs/DurinHeaderTool/tests/test_reflection_generation.py`
- `Sandbox/Source/Runtime/Sandbox/`
