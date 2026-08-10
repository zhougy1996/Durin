# Native Gameplay Core Plan

Summary: Add the minimal native pawn, controller, possession, game-mode bootstrap, semantic control, movement, and view-target contracts required by one deterministic local-player session.

Last reviewed: 2026-08-11

Status: Completed
Completed: 2026-08-11

## Current Status

G1 completed on 2026-08-11. Engine now publishes the exact root gameplay
types, symmetric possession, bounded semantic intent, one abstract movement
authority, controller view targets, explicit World play/tick/restart results,
transactional native bootstrap, and one shared `Game` project-settings owner.
PIE and standalone use the same exact module/class resolver. Sandbox contains
only a nested reflected qualification fixture; concrete control, movement,
camera, and visual policy remains in G2.

The recorded pre-change revision was
`fdad9a1c3d57a7a6a6ba6f6d9836bb85090f4a81`. Focused baseline results were
WorldTests 62/62, ViewportTests 57/57, CoreFileSystemTests 32 passed with one
intentional skip from 33 cases, and AssetReferenceStoreTests 3/3. The baseline
also exposed one stale `IScene` mock; the fixture now implements
`UpdateSkeletalMeshDynamicData` and all ordinary World coverage remains green.

Final focused evidence is WorldTests 75/75 across 16 suites, ViewportTests
57/57, AssetReferenceStoreTests 7/7, SkeletalAssetTests 33/33, and the isolated
Sandbox cross-project module/reflection case. The final target-granularity
native suite passed all 55 registered target processes. Changed-document and
all-plan validation passed. Full `all` builds passed for both
`Win64-Debug-DurinEditor` and `Win64-Debug-DurinGame`.

Bounded process qualification passed with clean exit in threaded and inline
RHI modes. DurinEditor exercised embedded/new-window destinations with both
Level Start and Play From Camera, including pause, single-step, stop, and
editor-World restoration. DurinGame exercised an isolated native
start/tick/pause-step/restart/stop sequence and restored the host World. Both
diagnostics are explicit opt-ins and ordinary startup is unchanged.

The handoff commit uses subject
`feat(gameplay): add native local-player core` and records this plan plus
Stages 0 through 5 in its provenance lines.

## Goal

Provide a small native gameplay core in the root `Durin` namespace that can
start one local player in an already active level, possess and replace a pawn
symmetrically, translate one raw input snapshot into bounded source-neutral
control, expose one pawn-movement authority, select a controller view target,
restart the player, and tear the complete runtime-only session down without
changing ordinary Actor-only world behavior.

## Scope

- Root-namespace reflected `APawn`, `AController`, `APlayerController`,
  `AGameMode`, `APlayerStart`, and `DPawnMovementComponent` types.
- A bounded `FPawnControlIntent` value for move, look, jump-held, jump-pressed,
  and jump-released semantics.
- One authoritative possession operation with reciprocal transient
  associations and immediate cleanup on transfer, actor destruction, EndPlay,
  world stop, and level replacement.
- One explicit `DWorld` play request/result protocol for both lifecycle-only
  and native-game sessions, plus an explicit World tick context.
- Native game-mode selection, one local player controller, deterministic first
  valid player-start selection, default-pawn creation, restart, rollback, and
  session-owned actor cleanup.
- One Engine-owned project game-settings store for the default level, native
  module, and fully qualified game-mode class.
- Raw input processing by `APlayerController` before the Actor tick snapshot on
  each world tick that actually advances.
- One designated movement component per pawn, with velocity and semantic
  movement-update boundaries but no concrete ground solver.
- A controller view target resolved before the existing level primary-camera
  fallback.
- PIE and standalone integration, including explicit Play From Camera override,
  pause/single-step, focus loss, repeated sessions, restart, and failure
  rollback.
- Focused native tests and lasting Runtime, Level, PIE, and project-setting
  documentation.

## Non-Goals

- Sandbox pawn, controller, game mode, movement, camera composition, key
  bindings, tuning, graybox assets, or level authoring; those belong to G2.
- Script runtimes, generated script bindings, behavior registries, managed
  handles, or provider hooks.
- Actor Definition, Blueprint, prefab, archetype, editable class-default, soft
  class reference, or general Actor-template systems.
- `ACharacter`, capsule collision, skeletal rendering, animation, root motion,
  navigation, slope/step behavior, or a production movement solver.
- Input assets, rebinding, enhanced-input actions, multiple local players,
  gamepads, touch, AI, replay recording, networking, replication, rollback, or
  prediction.
- Player state, camera manager, spring arm, camera blending, collision probes,
  or a general camera-effects stack.
- Runtime enumeration or automatic loading of every `.dproject` module,
  multiple gameplay modules, plugin discovery, or a generalized subsystem
  registry.
- Unrelated engine-wide refactors that do not strengthen gameplay ownership,
  startup failure propagation, tick ordering, or teardown. API and file-level
  refactors inside those boundaries are explicitly allowed.

## Design Decisions and Invariants

### Names, reflection, and module ownership

- Framework types use the exact root identities `Durin::APawn`,
  `Durin::AController`, `Durin::APlayerController`, `Durin::AGameMode`,
  `Durin::APlayerStart`, `Durin::DPawnMovementComponent`, and
  `Durin::FPawnControlIntent`.
- The new reflected headers are exported by Engine and listed explicitly in
  `Engine.dmodule`; `EngineFwd.h` gains only the corresponding forward
  declarations.
- Concrete game classes remain owned by a project module and may use a nested
  namespace such as `Durin::Sandbox`. Physical module names never determine or
  rewrite reflected class identity.
- Project runtime game settings have one owner and one section:

  ```yaml
  Game:
    DefaultLevel: /Game/Levels/NewLevel
    NativeModule: Sandbox
    GameModeClass: Durin::Sandbox::ADefaultGameMode
  ```

- `Game.DefaultLevel` replaces the semantically incorrect
  `Editor.DefaultLevel` route. The Engine-owned store is used by standalone
  startup, the Level Editor, and the external default-level reference
  contributor; there is no second YAML parser or independently cached value.
- `Game.NativeModule` is the logical module name passed to `FModuleManager`;
  `Game.GameModeClass` is an exact fully qualified reflection identity passed to
  `FindClassByQualifiedName` only after the module is ready and newly loaded
  reflected objects have been processed.
- Missing `Game` or a missing module/class pair is a supported lifecycle-only
  mode. The repository-owned Sandbox setting is migrated atomically from
  `Editor.DefaultLevel` to `Game.DefaultLevel`; the old YAML route is not a
  second long-term read path. If exactly one gameplay scalar is empty, the
  module cannot load, the class is missing, abstract,
  non-constructible, or not derived from `AGameMode`, the configured bootstrap
  fails with the module, class, project-settings route, and reason. It never
  falls back to a short name or another class.

### World play protocol, bootstrap, and rollback

- World play is entered only through an explicit `FWorldPlayRequest` and
  observable `FWorldPlayResult`. A null game-mode class means lifecycle-only
  play; a non-null class requests native gameplay. Existing no-argument World
  BeginPlay call sites are migrated rather than retained as a parallel public
  protocol.
- World ticking uses `FWorldTickContext`, which carries delta time and an
  optional raw game-input snapshot. Pause/single-step admission and gameplay
  input preparation occur before the ordinary Actor tick snapshot. Preview and
  test hosts pass no input source.
- `DWorld` privately owns one `FNativeGameplaySession` value containing the
  game mode, local controller, default pawn, runtime-created Actor set, and
  view-target override state. It is the only gameplay-session publication
  point and is not a `DObject`, registry, or second lifetime owner.
- A native-game play request validates the active stopped world, level,
  selected game-mode class, game-mode-provided controller/pawn classes, and
  player start before publishing the private session.
- `AGameMode` selects its player-controller and default-pawn classes through
  native virtual functions. The base implementations select
  `APlayerController` and `APawn`; there are no reflected class-default fields
  or authored template objects.
- The first valid non-destroying `APlayerStart` in stable level actor order is
  authoritative. A configured game start with no valid player start fails;
  there is no silent origin spawn.
- Bootstrap spawns the game mode, one player controller, and one default pawn
  as ordinary level-owned Actors, copies the selected start transform to the
  pawn, establishes possession, publishes the private World session
  pointers, and only then enters the existing World BeginPlay lifecycle.
- Every validation or spawn failure destroys partial runtime actors in reverse
  creation order, clears associations and cached pointers, leaves the World in
  `Stopped`, and preserves pre-existing level Actors.
- World stop routes the existing reverse Actor EndPlay snapshot
  first, then removes only its runtime-created pawn, controller, and game mode
  and clears private session state. Lifecycle-only play has the same Actor
  semantics without constructing a gameplay session.
- Restart keeps the local player controller, unpossesses and removes its old
  default pawn when still present, reselects a current player start, spawns the
  game-mode-selected pawn class, possesses it, and updates the default view
  target atomically. Failure leaves the controller alive and unpossessed with
  an actionable result; it does not resurrect the old pawn.
- World and Level remain the only lifetime and membership owners. The private
  session records the Actors it created for rollback and teardown but never
  replaces Level membership, the Level destruction path, or object ownership.

### Possession

- `AController` is an Actor and owns the only public `Possess`/`UnPossess`
  mutation boundary. `APawn` exposes its reciprocal controller but cannot
  attach itself directly.
- Both sides store transient `TObjectPtr` values. Possession never makes either
  Actor the other's Outer, attachment parent, or structural child.
- Before any mutation, possession validates both Actors, current level/world
  membership, destruction state, pending-kill state, and the World's ending
  state. A rejected operation preserves both existing pairs.
- Repossessing the current pawn is a successful no-op. A valid transfer first
  detaches the controller's old pawn and the target pawn's old controller, then
  attaches the new pair so no public callback observes crossed associations.
- Unpossess is idempotent and clears both sides even when one side is already
  stale. Destroying a controller or pawn through `DLevel`, ending either Actor,
  ending the World, or replacing the Level routes the same detach primitive.
- `AActor` gains one protected virtual `OnActorDestroyed()` notification,
  invoked exactly once by `DLevel::DestroyActor` after destruction is accepted
  and before EndPlay, component teardown, or membership removal. Its default is
  a no-op; Pawn and Controller use it to route the same detach primitive. This
  also covers never-begun Actors without coupling Level to concrete gameplay
  classes.

### Semantic control and tick ordering

- `FGameInputState` remains the raw Engine-owned device snapshot. Only
  `APlayerController` or a derived player controller reads `EKey`, mouse button,
  mouse delta, or focus identities.
- `FPawnControlIntent` contains two-dimensional move and look axes plus
  jump-held, jump-pressed, and jump-released booleans. The pawn admission
  boundary rejects non-finite values and clamps each axis to `[-1, 1]`.
- World tick retains its pause/single-step decision. On a tick that will
  advance, it asks the local player controller to build and submit one intent
  from the current raw snapshot before taking the normal forward Actor tick
  snapshot. A paused tick that does not advance produces no semantic intent.
- `DEngine::Tick()` continues to clear raw transitions after the World call.
  Focus loss and input disable continue to reset raw held and transition state.
  A single-step therefore observes at most one current raw transition set, and
  no jump edge is replayed on a later tick.
- `AController` exposes the protected source-neutral submit operation used by
  `APlayerController`; future AI or replay controllers can submit the same
  value without manufacturing raw key state.
- `APawn` accepts at most one pending intent per advancing tick and consumes it
  once in its tick. Consumption resets axes and transition flags so the value
  cannot leak across pause, unpossess, restart, or a second tick without a new
  producer.

### Pawn movement

- `APawn` always owns a root `DSceneComponent`, so player-start placement is
  valid without a concrete visual or movement implementation.
- A pawn designates at most one owned `DPawnMovementComponent` as movement
  authority through a protected setter. A foreign-owner, destroying, or second
  component is rejected without changing the existing authority.
- `DPawnMovementComponent` is abstract. It exposes its pawn owner, velocity,
  and one semantic movement-update virtual entered by `APawn` with the consumed
  intent and delta time.
- The base component does not integrate gravity, collide, move the Actor, or
  reuse `DPhysicsComponent`. G2 owns the first concrete horizontal acceleration,
  gravity, landing, and jump policy and must state the ground-plane limitation.

### View target and camera fallback

- `APlayerController` owns one transient Actor view target. Assignment accepts
  only a live Actor in the controller's current level/world; clearing is always
  allowed.
- A newly possessed pawn becomes the default view target. An explicit host
  override may replace it, and restart selects the replacement pawn unless the
  host supplies a new explicit override.
- Engine camera resolution remains viewport-client first. Its fallback order
  becomes the local player controller's valid view-target
  `DCameraComponent`, then the level's primary `ACameraActor`, then the existing
  identity/no-camera behavior.
- A target without a camera, outside the active level, pending destruction, or
  no longer valid is ignored rather than cached as the active camera.
- PIE Level Start uses the possessed pawn's target. Play From Camera keeps the
  existing transient `PIE_EditorCamera` as an explicit session view-target
  override after native bootstrap, so the editor command retains its meaning.

### Threading and lifetime

- All gameplay bootstrap, possession, input mapping, movement, view-target,
  restart, and teardown work is GameThread-only.
- No gameplay Actor or component owns `FGameInputState`, a module interface,
  an editor object, or a render-scene resource.
- The selected native project module remains loaded under the existing module
  manager lifetime and reverse shutdown order. Gameplay session teardown and
  object drain complete before normal module unload; this plan adds no hot
  unload path.

## Current Foundations and Gaps

| Area | Existing foundation | Gap owned by this plan |
| --- | --- | --- |
| Reflection | G0 lexical namespace lookup, qualified runtime identities, generated constructors, derived-class validation | Export and qualify the new Engine gameplay types |
| Module loading | Logical-name dynamic loading after `DObjectInit` processes newly loaded reflection before `StartupModule` | No project gameplay module/class selection seam |
| Project settings | `Project.yaml` supplies `Editor.DefaultLevel`, but standalone and editor parse/cache it separately | No Engine-owned game-settings model, correct shared route, or native gameplay validation |
| Actor/Component | Level-owned actors, default components, safe BeginPlay/Tick/EndPlay mutation, deterministic destruction | No Pawn, Controller, movement authority, or association cleanup |
| World | One active level, stable play state, pause/single-step, level replacement, forward tick and reverse teardown | No explicit game-session bootstrap, cached roles, rollback, restart, or runtime-actor cleanup |
| PIE | Transient level duplication, editor restoration, optional camera override, focus-gated input, retired render lifetime | No project game-mode resolution or gameplay rollback/re-entry evidence |
| Input | Enabled/focused raw key, mouse, held, transition, delta, and per-engine-tick reset | Raw state is exposed directly; no semantic player-controller phase or one-use pawn intent |
| Movement | Actor transforms and limited `DPhysicsComponent` ground-plane integration | No pawn movement contract separated from a concrete solver |
| Camera | Viewport-client view, level primary camera, camera components, PIE editor-camera spawn | No controller-owned view target or possessed-pawn camera resolution |
| Tests | World lifecycle mutation, pause/step, duplication, actor destruction, camera fallback | No gameplay core, native bootstrap, possession, input-edge, restart, or repeated PIE matrix |

## Implementation Stages

### Stage 0: Freeze the native gameplay contract and regression baseline

Dependencies: completed Gameplay Foundation G0 and the audited Engine/World/PIE
seams recorded above.

- [x] Record the source revision and focused baseline results for World
  lifecycle, World mutation, viewport fallback, project loading, and PIE input
  focus behavior.
- [x] Add compile-time and reflection fixtures for the exact root gameplay
  identities, inheritance graph, abstract movement class, transient
  associations, and cross-module derived game-mode lookup.
- [x] Add failing focused tests that state the possession, bootstrap rollback,
  stable player-start, restart, semantic input, pause/single-step, view-target,
  and teardown expectations before production behavior changes.
- [x] Freeze the `FWorldPlayRequest`, `FWorldPlayResult`, and
  `FWorldTickContext` shapes, lifecycle-only request semantics, private session
  publication point, and the `Game` project-settings schema.
- [x] Add explicit regression cases proving lifecycle-only requests create no
  gameplay actors and a missing native module/class pair preserves editor and
  standalone level play.
- [x] Freeze public result/error surfaces for configured bootstrap, possession,
  restart, and view-target rejection; every failure needed by tests must be
  observable without log scraping.

#### Acceptance Gate

- The exact API, class/config identities, failure categories, tick order, and
  compatibility behavior in this plan are represented by focused tests, while
  the unchanged World, viewport, and project-setting baseline remains green.

### Stage 1: Add roles and authoritative possession

Dependencies: Stage 0.

- [x] Add reflected `APawn`, `AController`, and `APlayerController` headers and
  implementations, Engine reflection metadata, exports, and forward
  declarations.
- [x] Give `APawn` its default root scene component and add transient reciprocal
  pawn/controller associations.
- [x] Implement the single validate-detach-attach possession operation,
  idempotent unpossess, valid transfer, and rejection without partial mutation.
- [x] Add the exactly-once Actor destruction notification and route Actor
  EndPlay, Level destruction, World teardown, and level replacement through the
  same detach primitive, including never-begun Actors.
- [x] Prove ordinary Actor spawn, destruction, component lifecycle, attachment,
  serialization, and iteration remain unchanged.

#### Acceptance Gate

- Focused tests prove reciprocal pairing, idempotence, transfer, invalid
  cross-level/world rejection, pending-destroy rejection, pawn/controller
  destruction in and out of play, reverse EndPlay, and zero association state
  after level replacement or world retirement.

### Stage 2: Add semantic control and the movement boundary

Dependencies: Stage 1.

- [x] Add `FPawnControlIntent` with finite/clamped axes and explicit jump held,
  press, and release state.
- [x] Add source-neutral controller submission, player-controller raw input
  production, pawn admission, and exactly-once consumption.
- [x] Replace World tick call sites with `FWorldTickContext` and process local
  player raw state before the ordinary Actor tick snapshot only when the World
  advances.
- [x] Add abstract `DPawnMovementComponent`, single-authority pawn association,
  velocity state, and semantic movement-update dispatch.
- [x] Clear pending semantic state on unpossess, possession transfer, pause
  boundaries as applicable, restart, EndPlay, destruction, and failed
  bootstrap.
- [x] Keep raw key/mouse identities out of Pawn and movement headers, generated
  reflection data, and tests below the player-controller boundary.

#### Acceptance Gate

- Focused tests prove finite/clamped intent, press/hold/release behavior,
  exactly-once edge consumption, controller-before-pawn ordering, paused tick
  suppression, one single-step delivery, focus/disable reset, source-neutral
  submission, movement-owner rejection, and no concrete physics behavior in
  the base movement component.

### Stage 3: Implement transactional game-mode bootstrap and restart

Dependencies: Stages 1-2.

- [x] Add reflected `AGameMode` and `APlayerStart`, including native controller
  and pawn class selection and stable player-start choice.
- [x] Replace World play call sites with the explicit request/result/stop
  protocol and add the private gameplay-session value plus local-player restart.
- [x] Validate all configured and game-mode-selected classes before publication,
  spawn game mode/controller/pawn through the Level path, place the pawn at the
  chosen start, possess, and then enter ordinary BeginPlay.
- [x] Implement reverse rollback for missing start, invalid class, spawn,
  transform, and possession failures, preserving every pre-existing Actor.
- [x] Remove only runtime-created gameplay actors after the reverse EndPlay
  pass and make a second start on the same level produce one fresh session.
- [x] Add the Engine-owned `Project.yaml` game-settings store; migrate
  `Editor.DefaultLevel` to `Game.DefaultLevel`; route standalone, Level Editor,
  and the external reference contributor through it; and remove duplicate
  parsing/caching.
- [x] Resolve the exact native module/class pair with module readiness, fully
  qualified class lookup, derived-class and constructibility validation, and a
  lifecycle-only result when the pair is absent.

#### Acceptance Gate

- Focused tests prove deterministic first-start selection, one controller and
  pawn, correct transform, BeginPlay ordering, missing-start and spawn rollback,
  explicit restart success/failure, repeated start/stop, preservation of level
  Actors, exact qualified class selection after module load, and actionable
  partial/invalid setting failures.

### Stage 4: Integrate hosts and controller camera ownership

Dependencies: Stage 3.

- [x] Route `DGameEngine` default-level startup through the shared settings and
  explicit World play request; a configured failure propagates an actionable
  result to the highest practical startup owner and leaves the World stopped.
- [x] Route `DEditorEngine::StartPlaySession()` through the same resolver after
  level duplication and before publishing Playing; on failure, restore the
  editor world, viewport, input, object maps, and transient world ownership.
- [x] Add controller view-target assignment and Engine camera resolution before
  the existing level primary-camera fallback.
- [x] Preserve Level Start pawn targeting and apply the existing transient
  editor camera as the explicit Play From Camera override.
- [x] Preserve embedded and new-window focus gating, raw transition reset,
  pause, single-step, stop, retired render fences, and editor-world restoration.
- [x] Add repeated PIE coverage for start, pause, step, stop, restart, configured
  failure, destroyed pawn/view target, fallback camera, and re-entry without
  stale input or gameplay pointers.

#### Acceptance Gate

- PIE and standalone host fixtures select the same configured game-mode class;
  successful sessions publish one coherent controller/pawn/view target;
  configured failures publish none; Level Start and Play From Camera retain
  their documented views; and repeated PIE teardown restores the editor with no
  stale possession, input, camera, module, or runtime-world state.

### Stage 5: Document and qualify the G1 handoff

Dependencies: Stages 1-4.

- [x] Publish lasting possession, explicit World play/tick, bootstrap, semantic
  input, movement ownership, view-target, Actor destruction, project-setting,
  and failure contracts in their
  Runtime, Level, PIE, viewport, and workspace documentation owners.
- [x] Verify generated reflection output uses the intended qualified identities
  and Sandbox or another nested consumer can derive from the root framework
  types without spelling workarounds.
- [x] Run the smallest affected native targets during development, then the
  final full native-test suite because shared World/Engine lifecycle and
  multiple test targets are crossed.
- [x] Complete a full `all` build because PIE behavior is user-visible and the
  change crosses Engine, DurinEd, Launch, and a late-loaded project module.
- [x] Run bounded editor PIE smokes for embedded/new-window and Level Start/Play
  From Camera, plus a standalone native-bootstrap start/tick/stop smoke in the
  normal and inline-RHI diagnostic modes.
- [x] Record final validation and commit provenance in Current Status, close the
  G1 roadmap milestone, and open G2 only after every G1 acceptance gate passes.

#### Acceptance Gate

- Lasting documentation and implementation agree; focused and full native
  validation pass; the full build passes; editor and standalone smokes start,
  tick, pause/step where applicable, restart, render through the selected
  camera, stop, and exit cleanly; and the Gameplay Foundation roadmap can hand
  G2 a stable generic API with no Sandbox policy in Engine.

## Validation Matrix

| Boundary | Required evidence |
| --- | --- |
| Reflection and modules | Exact root identities, abstract/derived flags, cross-module native game mode, fully qualified lookup after module readiness, and reverse shutdown safety |
| Possession | Pair, no-op, transfer, invalid world/level, destruction before/during play, EndPlay, level switch, and repeated teardown |
| Bootstrap | Missing-config compatibility, exact configured selection, stable first start, class/start/spawn failure rollback, repeated start/stop, and preservation of authored Actors |
| Restart | Controller persistence, old-pawn removal, new start transform, possession/view update, and deterministic unpossessed failure state |
| Input | Raw-only player-controller access, finite/clamped intent, press/hold/release, focus reset, pause suppression, single-step, and one-use edges |
| Movement | One pawn-owned authority, owner rejection, velocity API, semantic dispatch, and absence of concrete collision/ground policy |
| Camera | Viewport client, valid controller target, level primary fallback, invalid/destroyed target, Level Start, Play From Camera, and restart |
| PIE lifecycle | Duplicate, configured bootstrap, pause/step, embedded/new-window input, stop, retired world, editor restore, and repeated re-entry |
| Standalone lifecycle | Project settings, module load, default level, game start, bounded ticks, stop, object/module/render drain, and clean exit |
| Compatibility and scope | Ordinary Actor lifecycle/tests unchanged; no Sandbox tuning, script/template, skeletal, networking, AI, or general registry dependency |

## Definition of Done

- The six planned native framework types and `FPawnControlIntent` exist with
  the exact root identities and generated reflection support.
- Controller and pawn associations are always reciprocal when observable and
  clear deterministically on transfer, destruction, stop, or level change.
- One configured native game mode can transactionally create one local player
  at one player start, restart its pawn, and remove all session-created Actors.
- A lifecycle-only play request and missing native gameplay pair remain valid;
  `Editor.DefaultLevel` has one migrated replacement at `Game.DefaultLevel`;
  invalid configuration or bootstrap failure never publishes a partial
  session.
- Raw input ends at the player-controller boundary and one bounded semantic
  intent reaches the pawn/movement boundary exactly once per advancing tick.
- The controller camera precedes the level primary fallback without changing
  viewport-client precedence or Play From Camera behavior.
- PIE and standalone share the same native module/class resolver and produce no
  stale gameplay, input, camera, object, render, or module state after teardown.
- Engine contains no Sandbox key mapping, movement tune, camera tune, visual,
  or concrete gameplay class.
- Every stage acceptance gate and the final validation matrix pass, lasting
  behavior is documented outside this plan, and Gameplay Foundation G1 is
  ready to close.

## Deferred Follow-ups

- Sandbox `APlayerPawn`, `ADefaultPlayerController`, `ADefaultGameMode`,
  `DSimpleGroundMovementComponent`, C++ key mapping, graybox visual, camera
  composition, and playable validation are G2.
- Multiple native gameplay modules, module lists, runtime dependency discovery,
  project-settings UI, and configurable game-mode per level require a separate
  demonstrated need.
- Authored class defaults, soft class references, Actor templates, spawn
  parameters, and prefab/archetype systems remain separate roadmap decisions.
- Multiple local players, AI/replay producers, controller rotation, player
  state, input assets, networking, replication, and prediction remain deferred.
- A second concrete pawn, input source, or camera behavior is required before
  Gameplay Foundation G3 may extract another shared framework layer.

## Related Documentation

- [Gameplay Foundation Roadmap](../Roadmaps/GameplayFoundation.md)
- [Generated Reflection System](../Runtime/Core/ReflectionSystem.md)
- [Runtime Lifecycle](../Runtime/Core/RuntimeLifecycle.md)
- [Level System](../Runtime/World/LevelSystem.md)
- [Play In Editor Architecture](../Editor/Architecture/PlayInEditorArchitecture.md)
- [Viewport Rendering](../Runtime/Rendering/ViewportRendering.md)
- [Workspace And Projects](../Workspace/WorkspaceProjects.md)
- [Build And Run](../Development/Build/BuildAndRun.md)
- [Native C++ Tests](../Development/Build/NativeTests.md)
- [Actor Component System](Archive/2026-07/ActorComponentSystem.md)
- [Actor Lifecycle Mutation Safety](Archive/2026-07/ActorLifecycleMutationSafety.md)

## Related Code

- `Engine/Source/Runtime/Engine/Engine.dmodule`
- `Engine/Source/Runtime/Engine/Public/EngineFwd.h`
- `Engine/Source/Runtime/Engine/Public/Engine/Actor.h`
- `Engine/Source/Runtime/Engine/Private/Engine/Actor.cpp`
- `Engine/Source/Runtime/Engine/Public/Engine/World.h`
- `Engine/Source/Runtime/Engine/Private/Engine/World.cpp`
- `Engine/Source/Runtime/Engine/Public/Engine/Level.h`
- `Engine/Source/Runtime/Engine/Private/Engine/Level.cpp`
- `Engine/Source/Runtime/Engine/Public/Engine/Engine.h`
- `Engine/Source/Runtime/Engine/Private/Engine/Engine.cpp`
- `Engine/Source/Runtime/Engine/Public/Engine/GameEngine.h`
- `Engine/Source/Runtime/Engine/Private/Engine/GameEngine.cpp`
- `Engine/Source/Runtime/Engine/Public/Input/GameInputState.h`
- `Engine/Source/Runtime/Engine/Private/Input/GameInputState.cpp`
- `Engine/Source/Runtime/Engine/Public/Components/ActorComponent.h`
- `Engine/Source/Runtime/Engine/Public/Components/CameraComponent.h`
- `Engine/Source/Runtime/Engine/Public/Components/PhysicsComponent.h`
- `Engine/Source/Editor/DurinEd/Public/Editor/EditorEngine.h`
- `Engine/Source/Editor/DurinEd/Private/Editor/EditorEngine.cpp`
- `Engine/Source/Runtime/Core/Public/Misc/Project.h`
- `Engine/Source/Runtime/Core/Private/Misc/Project.cpp`
- `Engine/Source/Runtime/Core/Public/Modules/ModuleManager.h`
- `Engine/Source/Runtime/CoreDObject/Public/DObject/Class.h`
- `Engine/Source/Editor/LevelEditor/Private/Settings/ProjectDefaultLevelReferenceStore.cpp`
- `Engine/Source/Editor/LevelEditor/Private/Widgets/MLevelEditor.cpp`
- `Engine/Tests/Native/EngineTests/CMakeLists.txt`
- `Engine/Tests/Native/EngineTests/Private/World/`
- `Engine/Tests/Native/EngineTests/Private/Viewport/ViewportInteractionTests.cpp`
- `Sandbox/Configs/Project.yaml`
