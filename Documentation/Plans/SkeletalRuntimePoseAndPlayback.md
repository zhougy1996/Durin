# Skeletal Runtime Pose and Playback Plan

Summary: Add deterministic clip sampling, one skeletal-mesh component playback owner, and bounded immutable pose-palette publication without implementing skeletal rendering.

Last reviewed: 2026-08-10

Status: Active
Completed:

## Current Status

The plan is active at Stage 0 against baseline
`4eed3955a02cd8b8b067f313ab1de8436a08886d`. S1 is complete: `DSkeleton`,
`DSkeletalMesh`, and `DAnimationClip` publish validated authored and detached
payloads with structural compatibility identity, deterministic glTF transforms,
mesh palette indices, inverse-bind matrices, DDC/cooked codecs, and runtime-only
load coverage.

The S2 entry audit confirmed the remaining runtime seam:

- animation tracks already provide unique bone/path targets, strictly ordered
  finite times, Step/Linear interpolation, continuous unit quaternions, and a
  stable Skeleton reference;
- Skeleton bones are parent-before-child and expose canonical local reference
  matrices, while Core supplies checked matrix-to-transform decomposition;
- SkeletalMesh payloads expose palette bone indices and aligned inverse-bind
  matrices as detached immutable data;
- world ticking already routes through actor-owned, tick-enabled components;
- no pose evaluator, playback clock, skeletal component, compatibility-bound
  runtime instance, or immutable palette candidate exists.

This plan selects `DSkeletalMeshComponent` as the long-lived public owner of
mesh/clip selection and mutable playback state. It introduces the final
component identity now, but does not create a scene proxy, render resource,
shader, RHI binding, or renderer path. A value-only `FSkeletalAnimationInstance`
performs evaluation from detached binding data, and the component publishes
complete immutable palette candidates for the later shared skeletal-rendering
plan.

This work can proceed in parallel with
[GPU Resource Transitions](GPUResourceTransitions.md). S2 does not touch RHI,
VulkanRHI, Renderer, shaders, render passes, or GPU synchronization. S3 remains
the explicit merge point and cannot begin merely because this plan completes.

## Goal

Allow one skeletal component to bind a compatible mesh and clip, evaluate a
deterministic local-to-component pose from the established assets, advance or
control playback through the existing component lifecycle, and atomically
publish a bounded immutable mesh-aligned pose palette without any render-thread
read of reflected objects.

## Scope

- A detached runtime skeleton/clip/mesh binding snapshot suitable for value-only
  evaluation.
- Deterministic reference-pose initialization, Step and Linear channel sampling,
  shortest-path quaternion interpolation, hierarchy composition, and
  mesh-palette construction.
- `FSkeletalAnimationInstance` ownership of bound inputs, time, play/pause/stop,
  loop state, non-negative play rate, and the latest complete candidate.
- `DSkeletalMeshComponent` ownership of reflected mesh/clip references and the
  animation instance, integrated with component registration, BeginPlay, tick,
  EndPlay, property validation, and teardown.
- Structural compatibility rejection and complete-or-null rebinding/publication.
- Deterministic pose, time, lifecycle, lifetime, cooked-runtime, and failure
  tests plus lasting runtime documentation.

## Non-Goals

- Skeletal scene proxies, renderer registration, vertex factories, shaders,
  skinning buffers, material/pass participation, visibility, shadows, or image
  validation; those belong to S3.
- RHI resources, descriptor bindings, upload commands, resource transitions, or
  Vulkan implementation.
- Clip blending, layers, state machines, root motion, animation events,
  retargeting, IK, control rigs, additive animation, or animation notifies.
- Reverse playback, arbitrary time warping, network replication, rollback, or
  gameplay-authoritative transform mutation.
- Worker scheduling as a requirement. Evaluation is value-only and movable to a
  worker later, but the first owner ticks synchronously on the owning game
  thread.
- Animation compression, streaming, runtime source import, FBX skeletal
  compatibility, or changes to S1 payload formats.
- Editor preview controls or production import/inspection workflows.

## Design Decisions and Invariants

### Ownership and component boundary

- `DSkeletalMeshComponent` is the sole public mutable owner for one mesh, one
  optional clip, time, play/pause state, looping, and play rate. S3 extends this
  same component with rendering instead of introducing a second skeletal
  playback component.
- The component derives from `DPrimitiveComponent` to preserve its eventual
  stable scene identity, transform, visibility, and material boundary. During
  S2 it publishes no scene proxy and records no render commands.
- `FSkeletalAnimationInstance` is a non-reflected value owner embedded by the
  component. It owns mutable playback/evaluation state but holds only detached
  immutable binding inputs after a successful bind.
- Reflected `DSkeleton`, `DSkeletalMesh`, and `DAnimationClip` objects are read
  only while preparing a binding candidate on their owning thread. Evaluation
  and published palettes contain no `DObject`, package, registry, source-format,
  or RHI references.
- Mesh or clip replacement is prospective: validate and fully construct the new
  binding/reference candidate first, then commit once. Failure retains the
  previous valid binding, time, and published palette.

### Compatibility and detached inputs

- The mesh's Skeleton is authoritative for the component. A clip is bindable
  only when its Skeleton reference and compatibility identity match that mesh
  Skeleton structurally; name equality is irrelevant.
- The detached binding owns copied parent indices and decomposed reference TRS,
  counted clip payload storage, counted mesh payload storage, palette bone
  indices, inverse-bind matrices, and the compatibility identity required for
  diagnostics.
- Every reference matrix must decompose through Core's checked transform path
  into finite translation, normalized rotation, and finite non-degenerate
  scale before an animated binding can publish. Unsupported shear or singular
  transforms fail the candidate without mutating live state.
- Counts remain bounded by the S1 Skeleton, AnimationClip, and SkeletalMesh
  limits. Allocation arithmetic is checked before candidate storage is
  resized.

### Sampling and pose semantics

- Each sample begins from the Skeleton reference local transform. A track
  replaces only its declared translation, rotation, or scale channel; missing
  channels retain the reference value.
- A one-key track is constant. Step selects the preceding key. Linear performs
  component-wise vector interpolation and normalized shortest-path quaternion
  slerp. Exact key times return the exact stored key after normalization.
- Valid sample time is finite. Non-looping playback clamps to `[0, Duration]`
  and pauses on the terminal pose. Looping playback normalizes to the half-open
  interval `[0, Duration)`; exact positive multiples of Duration resolve to
  zero. Zero/negative duration clips remain invalid under the S1 contract.
- The initial plan supports finite play rates greater than or equal to zero.
  Negative rate and reverse traversal are deferred. Paused ticks do not advance
  time or publish redundant revisions.
- Parent-before-child order is authoritative. Local-to-component composition
  uses the repository matrix convention frozen by Stage 0 goldens. Palette
  entries follow `PaletteBoneIndices`, never raw source joint order.
- Stage 0 freezes the exact mesh-bind/inverse-bind multiplication order against
  the existing glTF/GLB fixture corpus before implementation. The public
  candidate shape does not change after that gate.

### Publication, lifetime, and failure

- Each successful state-changing evaluation builds a complete
  `FSkeletalPosePalette` candidate and publishes it as
  `std::shared_ptr<const ...>` with monotonic revision, normalized sample time,
  compatibility identity, and mesh-palette-aligned finite matrices.
- Readers retain a candidate independently of the next tick, component rebind,
  asset replacement, EndPlay, or component destruction. No candidate references
  the component or its DObjects.
- Invalid delta time, failed decomposition, incompatible assets, missing
  payloads, non-finite math, overflow, or palette mismatch does not publish a
  partial candidate. Runtime diagnostics identify the component/asset boundary
  and the failed invariant while retaining the previous valid candidate.
- Reference pose is a valid candidate for a valid mesh with no clip. `Stop`
  resets time to zero and publishes the reference or time-zero clip pose
  according to the frozen Stage 0 control contract.
- Ordinary component ticks are ordered by the existing actor/world lifecycle;
  no revision-based stale-work cancellation or background task graph is added
  without a later independently reordered producer.

### Parallel-work boundary

- The expected implementation working set is Engine animation, skeletal asset
  runtime snapshots, `DSkeletalMeshComponent`, reflection output, EngineTests,
  and selected asset/runtime regressions.
- This plan does not edit RHI, VulkanRHI, Renderer, shaders, render passes,
  descriptor layouts, GPU resources, or transition tests. Those belong to the
  concurrent RHI plan or later S3.
- The plans use separate worktrees and build roots. Their full-build gates may
  run independently, but no two writers may share one checkout or build tree.
- Completion of both plans creates readiness evidence, not automatic authority
  for S3. The rendering roadmaps' M1-M3 entry requirements and current RHI
  binding limits must still be re-inspected when S3 is selected.

## Current Foundations and Gaps

| Area | Foundation | Gap closed by this plan |
| --- | --- | --- |
| Skeleton | Parent-before-child bones, canonical local reference matrices, compatibility identity, bounded validation, and runtime load. | No detached decomposed runtime skeleton or evaluated pose. |
| Clip | Compatible Skeleton reference, bounded unique tracks, Step/Linear interpolation, ordered times, continuous unit quaternions, and immutable payload. | No sampler, clock, loop/control state, or pose goldens. |
| SkeletalMesh | Compatible Skeleton, detached vertex/influence payload, palette bone indices, inverse binds, and mesh bind transform. | No mesh-aligned runtime palette candidate. |
| Components | Actor-owned registration/play/tick/teardown and primitive render-state lifecycle are established. | No skeletal component or complete-or-null animation binding. |
| Math | Matrix/transform/quaternion operations and checked matrix decomposition exist. | Sampling, hierarchy composition, bind-space order, and finite-result policy are not frozen for animation. |
| Thread boundary | Detached asset payloads and counted immutable candidates are established patterns. | No pose candidate that can outlive component ticks without `DObject` reads. |
| Validation | S1 fixture, payload, DDC, cook, runtime-only, import, and transform goldens pass. | No sampled-pose, time-control, component lifecycle, candidate lifetime, or palette failure coverage. |

## Implementation Stages

### Stage 0: Freeze ownership, sampling, and pose math

- [ ] Record the exact `DSkeletalMeshComponent` reflected properties, runtime
  controls, tick enablement, BeginPlay/EndPlay behavior, and no-proxy S2 state.
- [ ] Define the detached binding and immutable candidate shapes, count/byte
  bounds, revision rules, and complete-or-null commit sequence.
- [ ] Freeze reference-matrix decomposition policy and create goldens for
  parent/child composition, non-uniform scale, rotations, and invalid shear or
  singular input.
- [ ] Freeze Step/Linear interval selection, exact-key behavior, quaternion
  interpolation, loop boundaries, pause/play/stop, zero delta, large delta, and
  non-negative play-rate behavior.
- [ ] Freeze mesh-bind, component-space, palette-index, and inverse-bind matrix
  order against the repository glTF and GLB fixtures.
- [ ] Name focused test suites and record the S2 working-set handoff before
  implementation expands.

#### Acceptance Gate

- One reviewed contract fixes the public component/instance/candidate shapes,
  every sampling/time edge has one expected result, transform goldens fix the
  matrix convention, and no unresolved decision can change Stage 1 storage.

### Stage 1: Build detached runtime inputs and the deterministic evaluator

- [ ] Add bounded detached reference-pose data derived transactionally from a
  validated Skeleton without changing S1 serialized formats.
- [ ] Implement track lookup and Step/Linear sampling over immutable clip
  payloads, including shortest-path normalized quaternion results.
- [ ] Compose local transforms to component space in parent-before-child order
  and construct the mesh-aligned palette selected in Stage 0.
- [ ] Reject incompatible, missing, malformed, overflowed, non-decomposable, or
  non-finite candidates without publishing partial arrays.
- [ ] Add pure evaluator tests for reference fallback, individual/mixed
  channels, hierarchy, exact keys, intervals, terminal time, looping, palette
  order, fixture goldens, and failure retention.

#### Acceptance Gate

- A value-only evaluator produces byte-stable or tolerance-frozen finite goldens
  for the supported corpus, reads no DObjects during evaluation, and rejects
  every malformed binding before publication.

### Stage 2: Add the animation instance and playback controls

- [ ] Implement `FSkeletalAnimationInstance` as the sole mutable owner of a
  detached binding, normalized time, play/pause state, looping, play rate,
  revision, and latest immutable candidate.
- [ ] Implement prospective mesh/clip binding, reference-pose/no-clip behavior,
  `Play`, `Pause`, `Stop`, seek if selected by Stage 0, and finite delta
  validation.
- [ ] Publish only complete counted candidates and retain the previous binding,
  time, and candidate after failed rebind or evaluation.
- [ ] Add tests for repeated tick determinism, large-delta wrapping, non-looping
  completion, zero rate, controls, incompatible clips, asset payload lifetime,
  and retained old candidates across later revisions.

#### Acceptance Gate

- The instance owns one coherent playback state, produces deterministic
  revisions and times, survives source DObject/payload replacement through its
  detached ownership, and never exposes a partially written pose.

### Stage 3: Integrate the skeletal-mesh component lifecycle

- [ ] Add reflected `DSkeletalMeshComponent` mesh/clip and playback settings
  with prospective validation and package/property-edit behavior consistent
  with existing components.
- [ ] Bind/unbind the instance during registration, BeginPlay, property/runtime
  changes, EndPlay, unregister, and destruction without double publication or
  stale ticking.
- [ ] Route actor/world tick into the instance only while registered, begun,
  alive, and tick-enabled; preserve pause/single-step semantics supplied by the
  world.
- [ ] Expose only counted immutable candidate access to future consumers. Keep
  `CreateSceneProxy` null and add no render-command or RHI path.
- [ ] Add component lifecycle tests for registration, BeginPlay, tick order,
  pause, rebinding, incompatible edits, EndPlay, destruction, world replacement,
  and candidate survival.

#### Acceptance Gate

- One component drives one instance through the established world lifecycle,
  invalid prospective changes retain prior state, teardown is clean, candidates
  outlive their producer safely, and no Renderer/RHI dependency is introduced.

### Stage 4: Qualify runtime-only playback and publish lasting contracts

- [ ] Run focused skeletal evaluator, instance, component, asset payload,
  cooked-runtime, and component/world lifecycle suites.
- [ ] Re-run S1 glTF/GLB transform/import and StaticMesh regression coverage to
  prove payload and source behavior did not drift.
- [ ] Validate Debug Editor and runtime-only ownership paths without source,
  import modules, or DDC fallback.
- [ ] Complete the normal Debug Editor full build using repository guidance;
  no Vulkan runtime smoke is required because this plan adds no rendering path.
- [ ] Document lasting pose, playback, component ownership, candidate lifetime,
  and compatibility contracts under the Runtime domain.
- [ ] Update the skeletal roadmap with completion evidence and an explicit S3
  re-inspection handoff; do not activate S3 in this plan.

#### Acceptance Gate

- The full S2 validation matrix passes, runtime-only loading drives identical
  pose goldens, lasting documentation owns the shipped behavior, and S3 can
  consume immutable candidates without reading assets or components on the
  render thread.

## Validation Matrix

| Layer | Required evidence |
| --- | --- |
| Math/unit | Reference decomposition, Step/Linear sampling, quaternion slerp, exact-key/interval behavior, hierarchy composition, mesh palette order, finite results, and fixture goldens. |
| Instance | Bind/rebind atomicity, structural compatibility, play/pause/stop, loop/clamp, large/zero delta, play rate, revisions, and retained candidate lifetimes. |
| Component | Registration, BeginPlay, tick, world pause/single-step, property/runtime replacement, EndPlay, unregister, destruction, and no-proxy behavior. |
| Asset/runtime | Authored/DDC/cooked/runtime-only Skeleton, SkeletalMesh, and AnimationClip payload ownership plus unchanged S1 import and StaticMesh projections. |
| Qualification | Focused EngineTests and asset regressions, runtime-only playback coverage, full Debug Editor `all` build, and clean native-test shutdown. |

All build and test commands follow
[Build and Run](../Development/Build/BuildAndRun.md). The implementation handoff
must record the exact profile, test filters, commands, and outcomes actually
used.

## Definition of Done

- `DSkeletalMeshComponent` is the one public owner for compatible mesh/clip
  selection and mutable single-clip playback state.
- Evaluation uses detached immutable inputs, deterministic documented sampling,
  parent-before-child composition, and bounded mesh-palette construction.
- Invalid inputs or runtime math never publish partial state and never destroy
  the previous valid binding/candidate.
- Immutable counted candidates survive later ticks, rebinding, EndPlay, and
  component destruction without holding DObjects or RHI resources.
- World/component lifecycle, authored/DDC/cooked/runtime-only paths, and S1
  regressions pass.
- No Renderer, shader, RHI, Vulkan, scene-proxy, or skinning implementation is
  introduced.
- Stable contracts move to Runtime documentation and the roadmap records a
  bounded S3 handoff.

## Deferred Follow-ups

- Skeletal scene proxy, render resources, vertex factory, material/pass/
  visibility integration, GPU palette binding, dynamic bounds, shadows, and
  animated image validation (shared S3).
- Editor playback/inspection/import workflow and production qualification (S4).
- Clip blending/layering/state machines, root motion, events, retargeting, IK,
  control rigs, reverse playback, and gameplay/network authority.
- Worker scheduling, cached pose evaluation, animation compression/streaming,
  and compute skinning until measured evidence activates them.

## Related Documentation

- [Skeletal Mesh and Animation Roadmap](../Roadmaps/SkeletalMeshAndAnimation.md)
- [Skeletal Asset and Import Foundation](Archive/2026-08/SkeletalAssetAndImportFoundation.md)
- [Rendering Capability Expansion Roadmap](../Roadmaps/RenderingCapabilityExpansion.md)
- [Asset Data Lifecycle and Storage](../Runtime/Assets/AssetDataLifecycle.md)
- [Asset Import Framework](../Editor/Architecture/AssetImportFramework.md)
- [Core Math](../Runtime/Core/Math.md)
- [Static Mesh Rendering](../Runtime/Rendering/StaticMeshRendering.md)
- [Build and Run](../Development/Build/BuildAndRun.md)

## Related Code

- `Engine/Source/Runtime/Engine/Public/SkeletalMesh/Skeleton.h`
- `Engine/Source/Runtime/Engine/Public/SkeletalMesh/SkeletalMesh.h`
- `Engine/Source/Runtime/Engine/Public/Animation/AnimationClip.h`
- `Engine/Source/Runtime/Engine/Private/Animation/AnimationClip.cpp`
- `Engine/Source/Runtime/Engine/Public/Components/ActorComponent.h`
- `Engine/Source/Runtime/Engine/Public/Components/PrimitiveComponent.h`
- `Engine/Source/Runtime/Engine/Private/Engine/Actor.cpp`
- `Engine/Source/Runtime/Engine/Private/Engine/World.cpp`
- `Engine/Source/Runtime/Core/Public/Math/TransformDecomposition.h`
- `Engine/Tests/Native/EngineTests/Private/SkeletalAssetTests.cpp`
- `Engine/Tests/Native/AssetCoreTests/Private/AssetImportTests.cpp`
