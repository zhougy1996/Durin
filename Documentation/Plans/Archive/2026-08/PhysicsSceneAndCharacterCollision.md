# Physics Scene And Character Collision Plan

Summary: Add layered AetherCore/Aether physics modules, a UE-shaped World query boundary, StaticMesh bodies, and capsule-based Sandbox movement against authored graybox geometry.

Last reviewed: 2026-08-11

Status: Archived
Completed: 2026-08-11

## Current Status

Stages 0-5 are complete. `AetherCore` now owns Engine-independent shapes,
filters, handles, validation, and reference query geometry; `Aether` owns the
deterministic `FPhysicsScene`; Engine exposes reflected collision profiles,
body setup/instance/component types, one scene per World, and UE-shaped trace,
sweep, and overlap entry points. Qualified `/Engine/Models/Box` assets publish
shared Box collision while instances retain distinct handles and render-only
components remain `NoCollision`.

The Sandbox pawn now owns a Pawn-profile Capsule and moves through bounded
World sweeps with penetration recovery, sliding, walkable-floor state, landing,
ceiling response, ramp traversal, and step solving. The fixed `Z = 0` fallback
and `GameplayTuning::GroundHeight` were removed; an empty World therefore lets
the pawn fall. Collision inspection is opt-in, captures at most 4096 bodies
with no snapshot work while disabled, and the Level Editor Collision overlay
draws Box/Sphere/Capsule wire shapes plus the latest impact normal.

Baseline revision: `0488efb68101a0476d68a9b87b5c24ab7bb895a2`.
Focused validation passed: PhysicsSceneTests 9/9, WorldTests 79/79,
StaticMeshTests 52/52, and SandboxGameplayTests 9/9. Final native
`DevTool.bat test --target all` passed after a focused rerun confirmed an
existing concurrency-soak fluctuation. Full Win64-Debug-DurinEditor and
Win64-Debug-DurinGame `all` builds passed, as did editor PIE and standalone
native-gameplay lifecycle smokes against `Sandbox/Sandbox.dproject`. The asset
compatibility baseline found 26 supported DAST v4 packages, and changed/all
documentation validation passed across 106 documents.

## Goal

- Establish the one-way runtime dependency chain
  `Core -> AetherCore -> Aether -> Engine` before physics types become embedded
  in World and component APIs.
- Give every `DWorld` one authoritative runtime `FPhysicsScene` for collision
  registration and synchronous scene queries.
- Let an ordinary `AStaticMeshActor` participate in collision through its
  existing `DStaticMeshComponent`, without putting broadphase, narrowphase, or
  movement policy inside the component.
- Give `DStaticMesh` reusable asset-owned simple collision through
  `DBodySetup`, and give each `DPrimitiveComponent` one instance-owned
  `FBodyInstance`.
- Replace the Sandbox player's fixed `Z = 0` clamp with capsule sweeps that
  land on platforms, stop at walls and ceilings, traverse supported ramps and
  steps, and retain one movement/velocity authority.
- Freeze public names and ownership seams that remain suitable for later
  complex collision, overlap events, character framework extraction, and a
  replaceable rigid-body backend.

## Scope

- A new `AetherCore` runtime module containing Engine-independent collision
  shapes, physics handles, low-level filter data, query-hit values, geometry
  conventions, and exact reference math.
- A new `Aether` runtime module containing `FPhysicsScene`, immutable body
  descriptors, the deterministic body store, broadphase/narrowphase query
  orchestration, and the first built-in query implementation.
- Engine-owned collision channels, response containers, profiles, query
  parameters, `FHitResult`, and `FOverlapResult`, with explicit conversion to
  and from AetherCore values and handles.
- One game-thread-owned `FPhysicsScene` per `DWorld`, with deterministic body
  registration, removal, transform/shape updates, and synchronous
  line-trace, sweep, and overlap queries.
- UE-shaped `DWorld` entry points including `LineTraceSingleByChannel`,
  `SweepSingleByChannel`, and `OverlapMultiByChannel`, plus
  `GetPhysicsScene()` for bounded engine integration.
- `DBodySetup` and simple aggregate geometry sufficient for Box, Sphere, and
  Capsule shapes; the initial authored StaticMesh path uses Box geometry.
- One `FBodyInstance` on `DPrimitiveComponent`, collision-enabled state,
  object channel, response container, profile name, and runtime scene handle.
- `DShapeComponent`, `DBoxComponent`, and `DCapsuleComponent` with reflected
  editable dimensions and component-lifecycle synchronization.
- `DStaticMesh::BodySetup`, `DStaticMeshComponent::GetBodySetup()`, and one
  qualified simple Box body setup for `/Engine/Models/Box` shared by the
  graybox StaticMesh instances.
- `AStaticMeshActor` defaulting its root StaticMesh component to the
  `WorldStatic` collision profile when usable body geometry exists; standalone
  `DStaticMeshComponent` retains `NoCollision` by default.
- A Sandbox player capsule independent of the graybox visual, and a bounded
  sweep-based replacement for fixed-plane movement.
- Blocking floor, wall, ceiling, raised-platform, rotated-Box ramp, and stair
  behavior needed by the current test scene, including initial penetration
  recovery and deterministic hit ordering.
- Collision debug visualization, reflected Details behavior, focused native
  tests, lasting runtime documentation, and PIE/standalone qualification.

## Non-Goals

- Dynamic rigid-body simulation, mass/inertia, forces, impulses, constraints,
  joints, ragdolls, destruction, sleeping, continuous rigid-body simulation,
  or an asynchronous physics tick.
- Selecting or integrating a third-party physics backend in this plan. The
  initial query implementation remains replaceable behind `FPhysicsScene`.
- An `AetherEditor` module, backend-specific Aether module, or plugin/provider
  discovery protocol before a second implementation demonstrates the need.
- Treating the existing `DPhysicsComponent` ground-plane integrator as the new
  body or character authority. It remains a legacy baseline until a later
  rigid-body migration plan retires or replaces it.
- Complex triangle-mesh, convex-decomposition, heightfield, skeletal per-bone,
  deforming-mesh, or runtime-generated collision in the first slice.
- Reusing the editor-only viewport picking scene index as runtime collision,
  or exposing render-scene proxies and render LOD choice through collision APIs.
- Production networking, prediction, rewind, replay, navigation, avoidance,
  moving platforms, root motion, crouch, swimming, flying, or custom movement
  modes.
- A complete UE-compatible `ACharacter` class hierarchy in this plan. The
  query/body names are frozen now; generic character ownership is extracted
  only when its construction and spawn-origin contracts are selected.
- Collision import UI, UCX naming, automatic convex generation, StaticMesh
  collision editing, or per-project collision-profile configuration.
- Making every `DStaticMeshComponent` blocking by default; player visuals,
  thumbnails, previews, and other render-only consumers stay collision-free
  unless their owner or caller opts in.

## Design Decisions and Invariants

### Module topology and names

- `AetherCore` is the low-level foundation module and exports through
  `AETHERCORE_API`. It may depend on `Core` only. It never includes or forward
  declares `DWorld`, `DObject`, Actor, component, asset, renderer, editor, or
  Sandbox types.
- `Aether` is the physics-runtime module and exports through `AETHER_API`. It
  depends on `Core` and `AetherCore` and owns `FPhysicsScene`, scene body
  storage, query dispatch, and the built-in broadphase/narrowphase
  implementation.
- `Engine` publicly depends on `Aether` and `AetherCore` because World and
  component public contracts expose the complete `FPhysicsScene` and
  `FCollisionShape` names. Neither Aether module depends on `Engine`,
  `CoreDObject`, `AssetCore`, `RenderCore`, or `RHI`.
- `AetherCore` owns `FCollisionShape`, `FPhysicsActorHandle`, low-level filter
  values, and `FPhysicsQueryHit`. These types carry numeric handles or bounded
  opaque user tokens, never Actor/component pointers.
- `Aether` owns `FPhysicsScene` and accepts Engine-independent immutable body
  descriptors. It returns AetherCore handles and query hits and does not map
  them to gameplay objects.
- `Engine` owns `DBodySetup`, `FBodyInstance`, `FCollisionQueryParams`,
  `FCollisionResponseParams`, `FCollisionObjectQueryParams`, `FHitResult`,
  `FOverlapResult`, collision profiles/channels, `DWorld` query facades, and
  every DObject/component integration. `FBodyInstance` is the stable mapping
  between one Engine component and one Aether physics-actor handle.
- Source roots are `Engine/Source/Runtime/AetherCore` and
  `Engine/Source/Runtime/Aether`. Public semantic headers remain grouped below
  `Public/Collision/` and `Public/Physics/`; public class names do not acquire
  `Aether` prefixes merely because of their module ownership.
- `Aether` and `AetherCore` are distinct registered module names, directories,
  libraries, export surfaces, and test boundaries. `AetherCore`, `Aether`,
  `PhysicsCore`, and `Physics` are not interchangeable aliases.

### Public naming

- The World-owned scene type is `FPhysicsScene`. `FPhysScene` and
  `FCollisionScene` are not aliases and do not enter the public API.
- Asset-owned reusable collision is `DBodySetup`; instance-owned body state is
  `FBodyInstance`.
- Low-level query values use AetherCore's `FCollisionShape` and
  `FPhysicsQueryHit`. Engine gameplay queries use `FCollisionQueryParams`,
  `FCollisionResponseParams`, `FCollisionObjectQueryParams`, `FHitResult`, and
  `FOverlapResult`.
- Filtering uses `ECollisionEnabled`, `ECollisionChannel`,
  `ECollisionResponse`, and `FCollisionResponseContainer`. Initial built-in
  profile names are `NoCollision`, `BlockAll`, `WorldStatic`, `Pawn`, and
  `Trigger`; `Trigger` reserves naming but event dispatch remains deferred.
- Shape components use `DShapeComponent`, `DBoxComponent`,
  `DSphereComponent`, and `DCapsuleComponent`. Stage 2 may implement only the
  shapes required by its acceptance gate, but later additions do not rename
  the hierarchy.
- World query names follow the UE-readable operation/result/filter form:
  `LineTraceSingleByChannel`, `SweepSingleByChannel`, and
  `OverlapMultiByChannel`. New query variants extend this family rather than
  creating a parallel naming convention.

### World and scene ownership

- Each live `DWorld` constructs exactly one `FPhysicsScene` before components
  from its current Level can register and destroys it only after those
  components unregister. PIE and the editor World therefore never share
  registered body instances or mutable acceleration state.
- `DWorld` owns the Aether `FPhysicsScene` instance, but `FPhysicsScene` never
  owns or calls back through a `DWorld` pointer. Engine supplies immutable body
  descriptions and maps returned handles through its `FBodyInstance` state.
- `DWorld` is the gameplay query facade. Ordinary gameplay and movement code
  do not iterate components and do not depend on private `FPhysicsScene`
  containers or a future backend.
- The first implementation is game-thread-only. Registration, mutation, and
  synchronous queries assert or reject off-thread entry; no lock or task is
  presented as support for concurrent mutation.
- `DWorld::IsPhysicsSimulationEnabled()` gates simulation stepping only.
  Query-only collision remains available when simulation is disabled, paused,
  or not yet implemented.
- The initial broadphase may use a deterministic flat body set plus world-AABB
  rejection because the graybox body count is bounded. Handles, public query
  results, and tie-breaking do not expose that choice, so a tree or backend
  acceleration structure can replace it without changing callers.

### Geometry and body ownership

- `DBodySetup` belongs to the source asset and stores immutable reusable simple
  collision plus cook/version identity. Components never mutate shared body
  geometry to represent an instance transform.
- `FBodyInstance` belongs to one `DPrimitiveComponent`. It stores per-instance
  enable/profile/response state and one transient scene handle, but does not
  own shared StaticMesh vertices or render resources.
- `DStaticMeshComponent::GetBodySetup()` returns its mesh's setup. Changing the
  StaticMesh, body setup revision, collision profile, component transform, or
  registration state synchronizes exactly one scene entry.
- `DPrimitiveComponent::OnRegister`, `OnUnregister`, and
  `OnUpdateTransform` remain the common lifecycle hooks. Render visibility and
  render-scene presence do not enable, disable, or filter collision.
- Registration with no enabled collision or no valid body geometry is a clean
  no-body state. A later valid mesh/profile update can create the body without
  re-creating the component.
- Initial StaticMesh collision is explicit. `/Engine/Models/Box` owns one
  simple local Box derived and verified from its source bounds; arbitrary
  imported meshes do not silently use render bounds or triangles as blocking
  geometry.
- Mirrored, non-finite, singular, or unsupported shape transforms are rejected
  before scene mutation with a deterministic component diagnostic. Stage 0
  freezes the exact accepted scale set needed by current authored geometry.

### Query and filtering contract

- Query inputs and outputs are value types. A query never retains caller spans,
  ignored-object arrays, callback state, or output references.
- Channel response is resolved from both sides. `Ignore` produces no result,
  `Overlap` produces overlap results only in an API that requests them, and
  `Block` participates in the closest blocking result.
- `SweepSingleByChannel` returns the closest blocking hit. Equal-time hits use
  a stable physics-body handle tie-break so Actor storage order and hash-table
  order cannot change gameplay.
- Queries support ignored Actors and components. Character sweeps always ignore
  the moving pawn and its owned components, preventing a registered Capsule
  from hitting itself.
- `FHitResult` distinguishes no hit, initial penetration, and swept blocking
  hit and reports normalized time, distance, location, impact point, impact
  normal, penetration depth, Actor, and `DPrimitiveComponent`.
- Non-finite query transforms, directions, dimensions, or deltas produce a
  deterministic no-hit result without partially modifying scene state.
- Start-overlap recovery is bounded. It never loops until clear, teleports an
  unbounded distance, or silently falls back to the global `Z = 0` plane.

### Character movement boundary

- The Sandbox pawn keeps exactly one `DPawnMovementComponent` as transform and
  velocity authority. `DPhysicsComponent` is not added to the pawn.
- Player collision is a dedicated `DCapsuleComponent`; the visible
  `DStaticMeshComponent` never supplies player movement collision.
- The first migration preserves the existing foot-origin PlayerStart
  convention by offsetting the Capsule from the pawn root by its half height.
  Movement sweeps the Capsule's world shape and applies the accepted delta to
  the pawn root, so the visual and camera retain one attachment transform.
- Horizontal acceleration/deceleration, gravity, jump impulse, maximum delta,
  yaw-relative input, look behavior, and one-use jump semantics remain the
  existing Sandbox contract unless collision requires an explicitly recorded
  adjustment.
- Movement performs a bounded number of sweeps per frame. It resolves the first
  hit, consumes the traveled fraction, projects the remainder along blocking
  surfaces, and stops when remaining displacement or progress is below a
  frozen tolerance.
- Floor state comes from a downward Capsule query. A hit is walkable only when
  its normal satisfies the configured walkable-floor threshold; jump admission
  uses that result rather than Actor `Z`.
- A landing clears only downward velocity, a ceiling hit clears only upward
  velocity, and a wall hit does not erase supported tangential velocity.
- Stair traversal uses a bounded step-up/move/step-down attempt with explicit
  maximum step height. Rotated Box ramps use their geometric surface normal;
  they are not approximated as horizontal platforms.
- Until moving-platform support is selected, floor bodies must be WorldStatic.
  Current Actor tick order is not presented as a general pre-/during-/post-
  physics scheduling contract.

### Failure, persistence, and compatibility

- Adding collision fields is an authored-asset and Level schema change. Old
  packages load through class defaults: standalone StaticMesh components remain
  `NoCollision`, while `AStaticMeshActor` roots receive the selected
  `WorldStatic` default only when their mesh provides valid collision geometry.
- `DBodySetup` simple geometry and identity participate in StaticMesh save,
  load, derived-data invalidation, cook, audit, reimport/exchange, and rollback
  contracts. Render-only reimport may not leave a stale body revision attached.
- Body registration/removal is idempotent across Level replacement,
  render-scene replacement, PIE duplication, EndPlay, component destruction,
  asset mutation, and World destruction.
- Collision errors do not remove rendering. An invalid body remains absent
  from `FPhysicsScene`, exposes a bounded diagnostic, and can recover after a
  valid edit.

## Current Foundations and Gaps

| Area | Existing foundation | Gap owned by this plan |
| --- | --- | --- |
| Modules | Layered Core, RenderCore, Renderer, RHI, and Engine runtime modules | No physics foundation/runtime split or one-way Aether dependency chain |
| World | One current Level, component registration, PIE isolation, pause/single-step, physics-enable flag | No world-owned physics scene, body registry, queries, or collision lifecycle |
| Primitive components | Common register/unregister/transform hooks and stable render-scene identity | No body instance, collision profile, shape provider, or physics-scene handle |
| StaticMesh | CPU LOD0 vertices/indices/bounds and picking ray hierarchy | No independently owned simple collision, body setup, collision cook identity, or component binding |
| Authoring | Ordinary `AStaticMeshActor` graybox pieces and a reusable create/update service | Authored pieces are render-only and expose no collision profile or debug view |
| Pawn | One root, one movement authority, semantic intent, bounded deterministic integration | No Capsule, scene sweep, floor query, wall/ceiling resolution, ramp, or step behavior |
| Physics baseline | `DPhysicsComponent` integrates velocity/gravity against one horizontal plane | No reusable collision and an incompatible second authority for character movement |
| Tests | World/component lifecycle, StaticMesh data, Sandbox frame-rate and jump/land coverage | No body lifecycle, query geometry/filtering, package persistence, or authored-level collision evidence |

## Implementation Stages

### Stage 0: Freeze collision names, geometry semantics, and baselines

Dependencies: existing World/component lifecycle, StaticMesh asset pipeline,
Sandbox gameplay tests, and the authored graybox Box convention.

- [x] Record the source revision and focused baseline for World lifecycle,
  component registration, StaticMesh save/load/derived data, Sandbox gameplay,
  PIE, and the current graybox level package.
- [x] Freeze the exact `AetherCore` and `Aether` module identities, source
  roots, export macros, public/private dependency edges, header ownership, and
  Engine-facing conversion boundary in module-graph fixtures.
- [x] Add compile-time and reflection fixtures for the exact
  `FPhysicsScene`, `DBodySetup`, `FBodyInstance`, collision value types,
  component types, enums, and `DWorld` method names selected by this plan.
- [x] Freeze coordinate, dimension, Capsule half-height, Box extent, normal,
  contact offset, skin width, minimum movement, penetration-recovery,
  walkable-floor, and maximum-step conventions in executable tests.
- [x] Characterize `/Engine/Models/Box` source and cooked bounds, current
  positive non-uniform transforms, the rotated ramp, stairs, platform seams,
  PlayerStart foot origin, and pawn visual dimensions.
- [x] Freeze `DBodySetup` ownership, serialization/cook versioning, StaticMesh
  mutation invalidation, default-profile migration, and failure diagnostics
  before changing an asset schema.
- [x] Add failing reference fixtures for ray/Box, Capsule/Box overlap,
  Capsule/Box sweep, initial penetration, stable equal-time selection, channel
  filtering, ignored owner, and invalid input.
- [x] Add failing Sandbox movement fixtures for raised landing, perimeter wall,
  ceiling, rotated ramp, stair limit, airborne jump rejection, and absence of a
  fallback `Z = 0` floor when no collidable body exists.

#### Acceptance Gate

- Public names, type ownership, serialization, geometry conventions,
  tolerances, filter behavior, hit ordering, character-origin semantics, and
  the first supported graybox transforms are represented by failing fixtures;
  the unchanged focused baseline remains green.

### Stage 1: Add AetherCore, Aether, and the World query contracts

Dependencies: Stage 0 module graph, public names, and query reference fixtures.

- [x] Register `AetherCore` and `Aether` in the Engine project/module graph,
  add their module descriptors and CMake targets, and prove the exact
  `Core -> AetherCore -> Aether -> Engine` dependency order in generated and
  native module metadata.
- [x] Add AetherCore collision shapes, opaque stable actor handles, low-level
  filter values, physics query hits, validation, and reference geometry math
  without an Engine, DObject, AssetCore, or rendering dependency.
- [x] Add Aether `FPhysicsScene`, immutable body descriptors, deterministic
  body storage, and atomic register, unregister, transform, shape, and filter
  operations with idempotent stale-handle refusal, without an Engine pointer
  or gameplay-object result.
- [x] Implement exact first-slice Ray/Box, Capsule/Box, and overlap reference
  paths, including arbitrarily rotated positive-scale Box bodies and bounded
  initial-penetration reporting.
- [x] Add `DWorld::GetPhysicsScene()`, `LineTraceSingleByChannel`,
  `SweepSingleByChannel`, and `OverlapMultiByChannel`; add Engine-owned query
  parameters/results and convert them to AetherCore filters, handles, and hits
  without exposing scene storage or narrowphase helpers.
- [x] Implement two-sided channel responses, ignored Actor/component filters,
  closest-blocking selection, overlap collection, and stable tie-breaking.
- [x] Prove queries remain available while simulation is disabled or the World
  is paused and reject off-thread or non-finite mutation/query input without
  corrupting the scene.

#### Acceptance Gate

- `AetherCore` and `Aether` build as separate one-way modules with no forbidden
  Engine/render/asset dependency. An isolated `DWorld` owns one leak-free
  `FPhysicsScene`; deterministic value-type queries return exact results for
  the frozen geometry/filter matrix; another World and a retired World cannot
  observe or mutate its bodies.

### Stage 2: Bind body setup and body instances to components

Dependencies: Stage 1 Aether scene registration and Engine query APIs;
qualified StaticMesh save/load and mutation baselines from Stage 0.

- [x] Add reflected `DBodySetup` with simple aggregate geometry, revision,
  validation, save/load, derived-data/cook participation, and immutable
  published shape access.
- [x] Add `FBodyInstance` to `DPrimitiveComponent` with collision-enabled,
  profile, object-channel, response, owner, and transient scene-handle state.
- [x] Add `GetBodySetup()`, collision-shape construction, body creation,
  destruction, and update hooks across register, unregister, transform,
  property edit, and owner destruction.
- [x] Add reflected `DShapeComponent`, `DBoxComponent`, and
  `DCapsuleComponent`; reserve `DSphereComponent` in the public hierarchy and
  implement it if required by the frozen query matrix.
- [x] Add `DStaticMesh::BodySetup`,
  `DStaticMeshComponent::GetBodySetup()`, and body recreation after mesh/body
  revision changes without coupling the collision lifetime to render readiness.
- [x] Author and audit one exact local Box body setup for
  `/Engine/Models/Box`; prove every transformed graybox Box instance shares
  the setup while owning a distinct `FBodyInstance`.
- [x] Give `AStaticMeshActor` the `WorldStatic` default and retain
  `NoCollision` for independently created StaticMesh components, previews,
  thumbnails, and the player's visual.
- [x] Add reflected Details editing and collision wire visualization for the
  supported body shapes without changing visibility, selection, or viewport
  picking semantics.
- [x] Prove save/reload, old-package defaults, reimport/exchange rollback,
  Level replacement, render-scene replacement, PIE duplication, undoable
  property edits, component destruction, and World teardown leave no stale or
  duplicated body.

#### Acceptance Gate

- Authored Box StaticMesh actors register one queryable WorldStatic body from
  shared asset geometry; component edits synchronize it exactly once; render-
  only StaticMesh consumers remain collision-free; persistence and lifecycle
  tests show no stale, cross-World, or leaked instance state.

### Stage 3: Replace fixed-plane Sandbox movement with Capsule sweeps

Dependencies: Stage 2 Box and Capsule bodies, channel profiles, and current
graybox-level collision registration.

- [x] Add a pawn-owned `DCapsuleComponent` using the `Pawn` profile, attach the
  visual and camera under the existing root convention, and keep the visual
  StaticMesh collision disabled.
- [x] Replace `DSimpleGroundMovementComponent` fixed-height tests and clamps
  with World Capsule sweeps while preserving its one-authority and bounded-delta
  contracts.
- [x] Implement bounded initial-overlap recovery, swept horizontal/vertical
  movement, remaining-delta consumption, surface sliding, landing, ceiling
  rejection, and stable velocity updates.
- [x] Implement downward floor query, walkable-normal classification,
  grounded-state caching valid for one movement update, floor snap, and jump
  admission from collision state.
- [x] Implement bounded step-up/move/step-down and rotated-ramp traversal using
  the frozen maximum step and walkable-slope policies.
- [x] Preserve yaw-relative input, acceleration/deceleration, gravity, jump,
  mouse look, camera composition, pause/single-step, restart, and focused
  30/60/120 Hz expectations where collision-free motion is equivalent.
- [x] Remove `GameplayTuning::GroundHeight` and every player-movement fallback
  to a global horizontal plane; no collidable floor means the pawn falls.
- [x] Add focused movement tests for floor, wall, corner slide, ceiling,
  platform edge/fall, raised landing, ramp thresholds, stair thresholds,
  penetration recovery, high-delta tunneling resistance, and self-ignore.

#### Acceptance Gate

- The Sandbox pawn collides through its Capsule with the current Box-authored
  test scene, can reach supported platforms/ramps/stairs, cannot cross blocking
  walls or ceilings, falls without collision geometry, and retains exactly one
  movement/velocity authority with no `Z = 0` policy.

### Stage 4: Qualify authored scenes, debugging, and lifecycle behavior

Dependencies: Stage 3 playable movement and Stage 2 persisted collision.

- [x] Add one deterministic native integration fixture that constructs the
  representative graybox floor, walls, raised platform, rotated ramp, stairs,
  and PlayerStart from ordinary Actors and validates the playable route.
- [x] Add debug display for body shape, bounds, profile/channel, blocking hit,
  impact normal, floor result, and penetration recovery with zero work when
  disabled.
- [x] Verify collision Details edits are transaction-safe and update PIE only
  through existing source/runtime rules; stopping PIE restores the editor World
  body scene unchanged.
- [x] Exercise embedded and new-window PIE pause, single-step, restart, stop,
  Level Start, and repeated re-entry with no stale body handles or grounded
  state.
- [x] Exercise standalone start, bounded movement, restart, stop, World/Level
  replacement, object drain, and clean exit.
- [x] Measure query/body counts and worst-case first-slice sweep work in the
  representative level; record evidence before selecting a non-flat broadphase.
- [x] Publish lasting collision/body/World-query contracts under Runtime and
  update Sandbox gameplay documentation to remove the ground-plane limitation.

#### Acceptance Gate

- The authored test scene is playable in PIE and standalone with inspectable
  collision, deterministic teardown/re-entry, transaction-correct editing, and
  measured query behavior that does not require an unplanned acceleration
  structure for the bounded first slice.

### Stage 5: Complete cross-target validation and handoff

Dependencies: Stages 1-4 and lasting documentation updates.

- [x] Run the smallest affected Engine and Sandbox native test targets during
  development, following the root native-test guidance.
- [x] Run final `--target all` native validation because the completed change
  adds two foundational runtime modules and crosses shared World/component/
  asset infrastructure plus the separate Sandbox gameplay target, so it
  cannot be covered by one native target.
- [x] Complete a full `all` build because collision Details/debug behavior and
  playable PIE movement are user-visible editor changes.
- [x] Run bounded editor PIE and standalone smokes against the saved Sandbox
  graybox Level from the same Agent Build Profile used for the full build.
- [x] Run StaticMesh/package compatibility audit and verify old supported
  assets and Levels load without unintended blocking collision.
- [x] Record validation and commit provenance in Current Status, close every
  passed checklist, move lasting contracts to owning documents, and set this
  plan Completed only after every acceptance gate passes.

#### Acceptance Gate

- Focused and full native validation, the full build, asset compatibility,
  saved-scene PIE, standalone movement, restart, teardown, and debug inspection
  all pass from one coherent profile, with no stale body, collision/profile
  migration surprise, or fixed-ground fallback.

## Validation Matrix

| Boundary | Required evidence |
| --- | --- |
| Module graph | Registered `AetherCore` and `Aether` targets, exact one-way `Core -> AetherCore -> Aether -> Engine` dependencies, correct export macros, and no forbidden reverse/render/asset edge |
| Public API | Exact complete `FPhysicsScene` name, UE-shaped body/query value names, correct AetherCore/Aether/Engine ownership, reflection identities, and no `FPhysScene`/`FCollisionScene` alias |
| World lifetime | One scene per World, construction before registration, removal before destruction, Level replacement, PIE isolation, and repeated teardown |
| Geometry | Ray/Box, Capsule/Box, arbitrary graybox rotation/positive scale, overlap, sweep, initial penetration, normals, and frozen tolerances |
| Filtering | Collision enabled modes, built-in profiles, two-sided responses, ignored owner/component, blocking versus overlap, and stable equal-time order |
| Body setup | Shared Box geometry, validation, version/revision, save/load, cook, audit, reimport/exchange, and rollback |
| Body instance | Register/unregister, transform/shape/profile update, no-body recovery, render-scene independence, destruction, and stale-handle refusal |
| Components | Shape dimensions, StaticMesh body lookup, AStaticMeshActor default, render-only NoCollision default, Details transaction, and debug visualization |
| Movement | Acceleration, gravity, landing, wall/corner slide, ceiling, edge fall, platform, ramp, stairs, jump, high delta, and no geometry/no floor |
| Gameplay lifecycle | Input/look parity, pause, single-step, restart, possession, Level Start, repeated PIE, standalone, and one transform/velocity authority |
| Compatibility | Existing packages, previews, thumbnails, material/static-mesh editors, picking, visibility, and rendering remain free of unintended collision behavior |
| Performance | Representative body/query counts, bounded sweep iteration, debug-off zero work, and evidence before broadphase replacement |

## Definition of Done

- `AetherCore` and `Aether` exist as distinct runtime modules: AetherCore owns
  Engine-independent physics values and math, Aether owns `FPhysicsScene` and
  query orchestration, and neither depends on Engine or rendering/assets.
- `DWorld` owns one complete-name `FPhysicsScene` and exposes deterministic
  UE-shaped synchronous collision queries without leaking backend storage.
- `DStaticMesh` owns shared `DBodySetup` geometry, and every colliding
  `DPrimitiveComponent` owns exactly one `FBodyInstance` registered in its
  World.
- `/Engine/Models/Box` provides qualified simple collision; ordinary
  `AStaticMeshActor` graybox pieces block the Pawn while preview/render-only
  StaticMesh components remain `NoCollision`.
- Box, Capsule, channel/profile, query, hit, overlap, lifecycle, persistence,
  and invalid-input contracts pass their focused test matrix.
- The Sandbox player uses an independent Capsule, lands and moves against the
  authored test scene, traverses supported ramps and stairs, and contains no
  global `Z = 0` ground assumption.
- PIE and standalone start, pause/step where applicable, restart, stop, and
  retire Worlds without stale bodies, grounded state, or cross-World queries.
- Collision shapes and hits are inspectable through bounded debug tooling;
  lasting behavior is documented outside this plan; required focused, full,
  build, compatibility, and runtime validation passes.

## Deferred Follow-ups

- A replaceable production rigid-body backend, dynamic bodies, forces,
  constraints, sleeping, async fixed-step scheduling, and migration or removal
  of the legacy `DPhysicsComponent`.
- Backend-specific modules built above `AetherCore`, provider selection through
  `Aether`, and an `AetherEditor` module. Their names and plugin boundaries
  require a concrete second backend or editor consumer.
- Generic `ACharacter` and `DCharacterMovementComponent`, Capsule-root spawn
  semantics, moving platforms, movement modes, crouch, root motion,
  networking, prediction, and replay.
- Complex StaticMesh collision, convex generation/decomposition, imported
  collision conventions, per-face physical materials, heightfields, terrain,
  skeletal bodies, and deforming geometry.
- Project-configurable collision channels/profiles and a collision-profile
  editor. Built-in first-slice profile names remain compatible inputs.
- Overlap begin/end and hit event dispatch, trigger gameplay, query delegates,
  asynchronous traces, batched queries, and contact callbacks.
- Broadphase tree/BVH or backend acceleration after representative
  measurements justify it; the flat first implementation is not a permanent
  performance promise.
- Editor StaticMesh collision authoring, visualization modes beyond the bounded
  component debug view, and graybox recipe collision overrides.
- Camera obstruction sweeps, navigation collision, AI avoidance, audio
  occlusion, particles, and any attempt to unify editor picking with runtime
  collision consumers.

## Related Documentation

- [Code Modules](../../../Workspace/CodeModules.md)
- [Sandbox Gameplay](../../../Runtime/Gameplay/SandboxGameplay.md)
- [Level System](../../../Runtime/World/LevelSystem.md)
- [Play In Editor Architecture](../../../Editor/Architecture/PlayInEditorArchitecture.md)
- [Static Mesh Rendering](../../../Runtime/Rendering/StaticMeshRendering.md)
- [Static Mesh Level Authoring](../../../Editor/Architecture/StaticMeshLevelAuthoring.md)
- [Native Graybox Scene Authoring Plan](../../NativeGrayboxSceneAuthoring.md)
- [Native Tests](../../../Development/Build/NativeTests.md)
- [Build And Run](../../../Development/Build/BuildAndRun.md)

## Related Code

- `Engine/Engine.dproject`
- `Engine/Source/Runtime/AetherCore/AetherCore.dmodule`
- `Engine/Source/Runtime/Aether/Aether.dmodule`
- `Engine/Source/Runtime/Engine/Engine.dmodule`
- `Engine/Source/Runtime/Engine/Public/Engine/World.h`
- `Engine/Source/Runtime/Engine/Private/Engine/World.cpp`
- `Engine/Source/Runtime/Engine/Public/Engine/Level.h`
- `Engine/Source/Runtime/Engine/Private/Engine/Level.cpp`
- `Engine/Source/Runtime/Engine/Public/Components/ActorComponent.h`
- `Engine/Source/Runtime/Engine/Public/Components/SceneComponent.h`
- `Engine/Source/Runtime/Engine/Public/Components/PrimitiveComponent.h`
- `Engine/Source/Runtime/Engine/Private/Components/PrimitiveComponent.cpp`
- `Engine/Source/Runtime/Engine/Public/Components/PhysicsComponent.h`
- `Engine/Source/Runtime/Engine/Public/Components/StaticMeshComponent.h`
- `Engine/Source/Runtime/Engine/Private/Components/StaticMeshComponent.cpp`
- `Engine/Source/Runtime/Engine/Public/StaticMesh/StaticMesh.h`
- `Engine/Source/Runtime/Engine/Public/StaticMesh/StaticMeshResources.h`
- `Engine/Source/Runtime/Engine/Private/StaticMesh/StaticMesh.cpp`
- `Engine/Source/Runtime/Engine/Public/Actors/StaticMeshActor.h`
- `Engine/Source/Runtime/Engine/Private/Actors/StaticMeshActor.cpp`
- `Engine/Source/Runtime/Engine/Public/Components/PawnMovementComponent.h`
- `Engine/Tests/Native/EngineTests/Private/World/WorldComponentTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/World/WorldPlayTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/StaticMeshDerivedDataContractTests.cpp`
- `Sandbox/Source/Runtime/Sandbox/Public/PlayerPawn.h`
- `Sandbox/Source/Runtime/Sandbox/Private/PlayerPawn.cpp`
- `Sandbox/Source/Runtime/Sandbox/Public/SimpleGroundMovementComponent.h`
- `Sandbox/Source/Runtime/Sandbox/Private/SimpleGroundMovementComponent.cpp`
- `Sandbox/Tests/Native/Private/SandboxGameplayTests.cpp`
- `Sandbox/Content/Levels/ThirdPersonTest.dasset`
