# Skeletal Runtime Pose and Playback Plan

Summary: Add deterministic clip sampling, one skeletal-mesh component playback owner, and bounded immutable pose-palette publication without implementing skeletal rendering.

Last reviewed: 2026-08-10

Status: Archived
Completed: 2026-08-10

## Current Status

S2 completed against baseline `4eed3955a02cd8b8b067f313ab1de8436a08886d`
and was delivered as one squashed implementation commit rebased onto `dev`.
`DSkeletalMeshComponent` is the
long-lived public owner of mesh/clip selection and mutable playback state;
`FSkeletalAnimationInstance` evaluates deterministic poses from detached binding
data and atomically publishes complete immutable palette candidates.

Sampling, hierarchy and palette math, loop/clamp time, controls, revisions,
prospective rebinding, candidate lifetime, component/world lifecycle, cooked
runtime-only ownership, and glTF/GLB equivalence are implemented and qualified.
The lasting contract is [Skeletal Animation Playback](../../../Runtime/Animation/SkeletalAnimationPlayback.md).

S2 adds no scene proxy, render resource, shader, RHI binding, renderer path, or
GPU synchronization. S1-S2 and Rendering M1-M3 now provide readiness evidence
for the shared S3/M4 entry gate, but S3 remains unselected until the roadmap's
current Renderer/RHI/product entry conditions are re-inspected.

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

- [x] Record the exact `DSkeletalMeshComponent` reflected properties, runtime
  controls, tick enablement, BeginPlay/EndPlay behavior, and no-proxy S2 state.
- [x] Define the detached binding and immutable candidate shapes, count/byte
  bounds, revision rules, and complete-or-null commit sequence.
- [x] Freeze reference-matrix decomposition policy and create goldens for
  parent/child composition, non-uniform scale, rotations, and invalid shear or
  singular input.
- [x] Freeze Step/Linear interval selection, exact-key behavior, quaternion
  interpolation, loop boundaries, pause/play/stop, zero delta, large delta, and
  non-negative play-rate behavior.
- [x] Freeze mesh-bind, component-space, palette-index, and inverse-bind matrix
  order against the repository glTF and GLB fixtures.
- [x] Name focused test suites and record the S2 working-set handoff before
  implementation expands.

#### Acceptance Gate

- One reviewed contract fixes the public component/instance/candidate shapes,
  every sampling/time edge has one expected result, transform goldens fix the
  matrix convention, and no unresolved decision can change Stage 1 storage.

#### Frozen Stage 0 contract

The following contract was reviewed against Core's column-vector, `[column][row]`,
`T * R * S` convention, the glTF decoder's `ParentGlobal * Local` composition,
the S1 `Contract.gltf`, `ContractExternal.gltf`, and `Contract.glb` fixtures,
and the existing component lifecycle. Later stages may add private helpers but
must not change these public shapes or semantics without reopening Stage 0.

##### Public ownership and controls

- `DSkeletalMeshComponent final : DPrimitiveComponent` reflects editable
  `TObjectPtr<DSkeletalMesh> SkeletalMesh`,
  `TObjectPtr<DAnimationClip> AnimationClip`, `bool bAutoPlay = true`,
  `bool bLooping = true`, and `float PlayRate = 1.0f`.
- Its runtime surface is `SetSkeletalMesh`, `SetAnimationClip`, `Play`, `Pause`,
  `Stop`, `Seek`, `SetLooping`, `SetPlayRate`, playback/time queries, and
  `GetLatestPosePalette`. Operations that can fail return `bool` and accept an
  optional diagnostic string. Asset setters and reflected edits prepare the
  whole prospective mesh/clip binding before committing either reference.
- The constructor enables component ticking. `OnRegister` prepares the current
  binding and publishes its time-zero pose. `BeginPlay` applies `bAutoPlay`.
  `TickComponent` advances only while registered, begun, alive, and tick-enabled.
  `EndPlay`, `OnUnregister`, and destruction stop future advancement and detach
  the instance binding; already returned candidates remain valid. Re-registration
  prepares a fresh time-zero binding. `CreateSceneProxy` returns null throughout
  S2 and the component issues no render command.
- `FSkeletalAnimationInstance` is the only mutable playback owner. The component
  contains it by value and never mirrors instance time, play state, revision, or
  candidate state in reflected fields.

##### Detached binding and immutable publication

- `FSkeletalAnimationBinding` contains only value data: compatibility identity;
  parent indices; decomposed reference-local TRS; optional counted immutable clip
  payload ownership; mesh-bind matrix; counted palette bone indices; and aligned
  inverse-bind matrices. It contains no `DObject`, package, registry, source,
  component, scene, renderer, or RHI reference.
- `FSkeletalPosePalette` contains `uint64 Revision`, normalized
  `float SampleTimeSeconds`, the compatibility identity, and a counted
  `std::vector<FMatrix4f> Matrices`. Publication and access use
  `std::shared_ptr<const FSkeletalPosePalette>`.
- Bone count is in `[1, MaximumSkeletonBones]`. Track/key/payload limits remain
  the S1 animation limits. Palette count is in `[1, BoneCount]`, is exactly the
  inverse-bind count, and palette bytes cannot exceed
  `MaximumSkeletonBones * sizeof(FMatrix4f)`. All size products and additions
  are checked before allocation. The binding retains only the clip track payload
  and mesh palette subset needed by evaluation, not vertex streams.
- An unbound instance has revision zero and a null candidate. Every successful
  bind/rebind publishes exactly one complete time-zero candidate and increments
  revision. A later evaluation increments revision only when normalized sample
  time or bound inputs change. Play/pause/rate/loop changes alone, paused ticks,
  zero delta, zero rate, and seeks to the current normalized time do not publish.
  Revision overflow rejects the operation and retains live state.
- Candidate construction is prospective: validate and allocate binding data;
  evaluate all local and component matrices; construct and validate every palette
  entry; allocate the immutable candidate; then swap binding, playback state,
  revision, and candidate once. Any failure leaves every prior field unchanged.

##### Reference decomposition and matrix goldens

- Each float reference-local matrix is widened to `FMatrix`, decomposed through
  `TryMakeTransformFromMatrix`, and accepted only when translation, normalized
  rotation, and scale are finite; every absolute scale component is greater than
  `kSmallNumber`; and recomposing `T * R * S` matches the widened source within
  `1e-5` per element. This accepts rotations, reflections, and finite non-uniform
  scale, but rejects singular scale, perspective, and unsupported shear.
- Each sampled local TRS is converted with `T * R * S`. Component matrices are
  evaluated in parent-before-child order as `ParentComponent * Local`; a root's
  component matrix is its local matrix. A parent with translation `(1,0,0)`,
  +90-degree Z rotation, scale `(2,1,1)`, and a child local translation `(1,0,0)`
  therefore places the child at `(1,2,0)`. The resulting finite matrix is valid
  even when later child rotation makes the composed matrix non-decomposable.
- For palette entry `i`, with `b = PaletteBoneIndices[i]`, the exact order is
  `inverse(MeshNodeBindTransform) * BoneComponent[b] * InverseBindMatrices[i]`.
  The mesh-bind inverse is checked once while preparing the binding. A mesh bind
  translated by `10`, reference bone translated by `12`, and inverse bind
  translated by `-2` produce identity; animating the bone to `13` produces a
  palette translation of `1`. Palette order follows `{2, 0}` as `{bone 2,
  bone 0}` and never canonical or source-joint order. The three S1 container
  variants must produce tolerance-identical reference and sampled palettes.

##### Sampling and playback table

| Case | Frozen result |
| --- | --- |
| No track for a channel | Use that channel from the reference-local TRS. |
| One key, or sample before first/after last key | Use the sole/nearest endpoint value. |
| Step between keys | Use the preceding key; an exact key uses that key. |
| Linear vector between keys | Component-wise interpolation; exact keys return the stored value. |
| Linear quaternion between keys | Normalize endpoints, negate the second when dot is negative, shortest-path slerp, normalize the result; exact keys return the normalized stored value. |
| Bind or `Seek(0)` | Publish/evaluate the time-zero clip pose, or reference pose with no clip. |
| `Play` / `Pause` | Change play state only; pose, time, and revision do not change. |
| `Stop` | Pause and normalize to zero; publish only when this changes sample time. |
| Finite delta `0`, paused state, or rate `0` | No time or revision change. |
| Non-finite or negative delta | Reject the tick and retain time, play state, revision, and candidate. |
| Play rate | Accept finite values `>= 0`; reject negative/non-finite prospectively. |
| Looping | Compute in double precision and normalize with modulo to `[0, Duration)`; exact positive multiples map to zero, including large deltas. |
| Non-looping | Clamp to `[0, Duration]`; reaching the terminal pose pauses after publishing it once. |
| Seek | Require finite input; apply the same loop normalization or non-loop clamp as ticking and publish only on a time change. |

Duration is the validated positive finite clip duration. Intermediate clock math
uses double precision; the published sample time is the canonical finite float
used for track lookup. Reference-only bindings remain paused at time zero.

##### Stage 1 working-set handoff

- Baseline: `c9197cf2`; Stage 0 changes only this plan.
- Initial implementation set: `Animation/SkeletalAnimation.h/.cpp`, existing
  Skeleton/SkeletalMesh/AnimationClip public payloads, Engine module source
  lists, and a new `SkeletalAnimationTests.cpp`. Stage 2 may extend the same
  files for the instance; Stage 3 adds `Components/SkeletalMeshComponent` and
  lifecycle tests. Reflection output remains generated, never hand-authored.
- Focused suites are `FSkeletalPoseEvaluatorTests`,
  `FSkeletalAnimationInstanceTests`, and `DSkeletalMeshComponentTests` in
  EngineTests, with S1 fixture/runtime regressions kept in their existing suites.
- Key decisions: retain counted immutable payload ownership; copy/decompose only
  skeleton reference data and mesh palette data; use matrix hierarchy
  composition; use the exact palette formula above; preserve complete-or-null
  publication and old-candidate lifetime.
- Open questions: none that can change Stage 1 storage. Runtime diagnostic wording
  and private helper placement may be selected during implementation.
- Stage 0 validation: source inspection matched Core math, glTF decoding,
  component registration/play/teardown, S1 limits, and fixture goldens. No build
  was required because Stage 0 changed documentation only.

### Stage 1: Build detached runtime inputs and the deterministic evaluator

- [x] Add bounded detached reference-pose data derived transactionally from a
  validated Skeleton without changing S1 serialized formats.
- [x] Implement track lookup and Step/Linear sampling over immutable clip
  payloads, including shortest-path normalized quaternion results.
- [x] Compose local transforms to component space in parent-before-child order
  and construct the mesh-aligned palette selected in Stage 0.
- [x] Reject incompatible, missing, malformed, overflowed, non-decomposable, or
  non-finite candidates without publishing partial arrays.
- [x] Add pure evaluator tests for reference fallback, individual/mixed
  channels, hierarchy, exact keys, intervals, terminal time, looping, palette
  order, fixture goldens, and failure retention.

#### Acceptance Gate

- A value-only evaluator produces byte-stable or tolerance-frozen finite goldens
  for the supported corpus, reads no DObjects during evaluation, and rejects
  every malformed binding before publication.

#### Stage 2 working-set handoff

- Baseline: the completed Stage 0 contract. Stage 1 working set is
  `Animation/SkeletalAnimation.h/.cpp`, `SkeletalAnimationTests.cpp`, this plan,
  and the EngineTests CMake source list.
- Key symbols: `FSkeletalAnimationBinding`, `FSkeletalPosePalette`,
  `BuildSkeletalAnimationBinding`, and `EvaluateSkeletalPose`.
- Decisions implemented: asset reads end at prospective binding construction;
  reference TRS, clip payload ownership, mesh-bind inverse, palette indices, and
  inverse binds are detached; evaluator output is assigned only after every
  finite palette matrix exists; matrix hierarchy and palette order match Stage 0.
- Open questions: none for Stage 2 storage. The instance will own normalization,
  controls, revision assignment, and the latest shared candidate without
  changing evaluator input/output shapes.
- Validation: `DevTool.bat test --target SkeletalAssetTests` under
  `Win64-Debug-DurinEditor-Tests` passed. Coverage includes reference
  fallback, hierarchy/non-uniform scale, mixed Step/Linear TRS, exact and terminal
  keys, quaternion normalization, mesh-bind/inverse-bind order, palette order,
  failure retention, and incompatible clip rejection. Loop clock behavior remains
  intentionally in Stage 2's instance tests.

### Stage 2: Add the animation instance and playback controls

- [x] Implement `FSkeletalAnimationInstance` as the sole mutable owner of a
  detached binding, normalized time, play/pause state, looping, play rate,
  revision, and latest immutable candidate.
- [x] Implement prospective mesh/clip binding, reference-pose/no-clip behavior,
  `Play`, `Pause`, `Stop`, seek if selected by Stage 0, and finite delta
  validation.
- [x] Publish only complete counted candidates and retain the previous binding,
  time, and candidate after failed rebind or evaluation.
- [x] Add tests for repeated tick determinism, large-delta wrapping, non-looping
  completion, zero rate, controls, incompatible clips, asset payload lifetime,
  and retained old candidates across later revisions.

#### Acceptance Gate

- The instance owns one coherent playback state, produces deterministic
  revisions and times, survives source DObject/payload replacement through its
  detached ownership, and never exposes a partially written pose.

#### Stage 3 working-set handoff

- Baseline: the completed Stage 1 evaluator. Stage 2 extends
  `FSkeletalAnimationInstance` in `Animation/SkeletalAnimation.h/.cpp`, adds
  instance tests in `SkeletalAnimationTests.cpp`, and updates this plan.
- Key symbols and decisions: `Bind` constructs and evaluates prospectively;
  `Play`/`Pause` do not republish; `Stop`, `Seek`, and advancing `Tick` publish
  only changed sample times; loop math uses double modulo and a half-open float
  result; non-looping clamps and pauses at the terminal pose; play rate and delta
  validation are failure-atomic; unbind resets producer state while retained
  candidates survive. Latest candidates use an atomic shared-pointer publication.
- Open questions: none for component storage. Stage 3 wraps these controls and
  diagnostics without duplicating mutable playback fields.
- Validation: `DevTool.bat test --target SkeletalAssetTests` under
  `Win64-Debug-DurinEditor-Tests` passed. Instance coverage includes
  pause/zero-delta/no-revision behavior, exact and large-delta loops, terminal
  clamp/pause, zero and positive rates, negative loop seek, stop idempotence,
  invalid-delta and incompatible-rebind retention, old-candidate lifetime, and
  continued evaluation after source mesh/clip payload replacement.

### Stage 3: Integrate the skeletal-mesh component lifecycle

- [x] Add reflected `DSkeletalMeshComponent` mesh/clip and playback settings
  with prospective validation and package/property-edit behavior consistent
  with existing components.
- [x] Bind/unbind the instance during registration, BeginPlay, property/runtime
  changes, EndPlay, unregister, and destruction without double publication or
  stale ticking.
- [x] Route actor/world tick into the instance only while registered, begun,
  alive, and tick-enabled; preserve pause/single-step semantics supplied by the
  world.
- [x] Expose only counted immutable candidate access to future consumers. Keep
  `CreateSceneProxy` null and add no render-command or RHI path.
- [x] Add component lifecycle tests for registration, BeginPlay, tick order,
  pause, rebinding, incompatible edits, EndPlay, destruction, world replacement,
  and candidate survival.

#### Acceptance Gate

- One component drives one instance through the established world lifecycle,
  invalid prospective changes retain prior state, teardown is clean, candidates
  outlive their producer safely, and no Renderer/RHI dependency is introduced.

#### Stage 4 working-set handoff

- Baseline: the completed Stage 2 playback owner. Stage 3 adds
  `Components/SkeletalMeshComponent.h/.cpp`, its generated-reflection input in
  `Engine.dmodule`, the `EngineFwd.h` declaration, component tests in
  `SkeletalAnimationTests.cpp`, and this plan.
- Key symbols and decisions: `DSkeletalMeshComponent` owns the instance by value;
  runtime setters and edit drafts validate the whole mesh/clip pair; registered
  successful replacement publishes one time-zero binding and preserves playing
  state when the replacement has a clip; `OnRegister` prepares, `BeginPlay`
  autoplays, and EndPlay/unregister/destruction detach; world pause and single
  step remain world-owned; `CreateSceneProxy` always returns null.
- Open questions: none for qualification. Stage 4 must exercise cooked/runtime-
  only inputs, S1 import/static regressions, native shutdown, and the full Debug
  Editor build before moving contracts to Runtime documentation.
- Validation: `DevTool.bat test --target SkeletalAssetTests` under
  `Win64-Debug-DurinEditor-Tests` passed. Component coverage includes
  reflected property shape and rejected edit drafts, registration/BeginPlay/tick,
  runtime controls and failure retention, world pause/single-step/tick disable,
  EndPlay/restart/unregister, level replacement, destruction, no-proxy behavior,
  and retained candidates after producer teardown. No Renderer, RHI, shader,
  render-command, or scene-proxy implementation was added.

### Stage 4: Qualify runtime-only playback and publish lasting contracts

- [x] Run focused skeletal evaluator, instance, component, asset payload,
  cooked-runtime, and component/world lifecycle suites.
- [x] Re-run S1 glTF/GLB transform/import and StaticMesh regression coverage to
  prove payload and source behavior did not drift.
- [x] Validate Debug Editor and runtime-only ownership paths without source,
  import modules, or DDC fallback.
- [x] Complete the normal Debug Editor full build using repository guidance;
  no Vulkan runtime smoke is required because this plan adds no rendering path.
- [x] Document lasting pose, playback, component ownership, candidate lifetime,
  and compatibility contracts under the Runtime domain.
- [x] Update the skeletal roadmap with completion evidence and an explicit S3
  re-inspection handoff; do not activate S3 in this plan.

#### Acceptance Gate

- The full S2 validation matrix passes, runtime-only loading drives identical
  pose goldens, lasting documentation owns the shipped behavior, and S3 can
  consume immutable candidates without reading assets or components on the
  render thread.

#### Completion handoff

- Baseline and working set: the completed Stage 3 component integration; final
  changes add cooked playback qualification in `SkeletalSceneLifecycleTests.cpp`, publish
  `Runtime/Animation/SkeletalAnimationPlayback.md`, update Runtime navigation,
  update the skeletal roadmap, and close this plan.
- Key shipped symbols: `FSkeletalAnimationBinding`, `FSkeletalPosePalette`,
  `BuildSkeletalAnimationBinding`, `EvaluateSkeletalPose`,
  `FSkeletalAnimationInstance`, and `DSkeletalMeshComponent`.
- Validation profiles: `Win64-Debug-DurinEditor-Tests` plus the plan-gated
  focused `Win64-Shipping-DurinGame-Tests` runtime-variant check on the
  `windows-msvc-x64` Agent Build Profile.
- Commands and outcomes: `DevTool.bat test --target SkeletalAssetTests`,
  `DevTool.bat test --target SkeletalSceneLifecycleTests`, `DevTool.bat test
  --target AssetImportTests`, and `DevTool.bat test --target StaticMeshTests`
  passed; `DevTool.bat test --preset Win64-Shipping-DurinGame-Tests --target
  SkeletalAssetTests --agent` passed without editor/import modules; and
  `DevTool.bat build --target all --agent` completed the Debug Editor full build.
  Native targets shut down cleanly. The Shipping gate exposed and removed a
  quaternion-normalization side effect from a compiled-out assertion before the
  final successful reruns.
- Runtime-only evidence: the lifecycle test performs deterministic glTF/GLB
  import and cook, deletes authored Engine/Game content and DDC, loads cooked
  mesh/clip assets, drives them through `DSkeletalMeshComponent`, validates
  finite mesh-aligned palettes, and compares corresponding container poses
  within `1e-5`.
- Verified executable:
  `Engine/Binaries/Win64/Debug/Runtime/DurinEditor/DurinEditor.exe`.
- S3 handoff: do not activate automatically. Re-inspect current Renderer
  prepared-primitive/material/pass/visibility seams, RHI bindings and resource
  transitions, palette upload budget/lifetime, animated bounds policy,
  representative fixture, Vulkan validation scope, and the product decision
  that SkeletalMesh remains the Rendering M4 primitive.

## Validation Matrix

| Layer | Required evidence |
| --- | --- |
| Math/unit | Reference decomposition, Step/Linear sampling, quaternion slerp, exact-key/interval behavior, hierarchy composition, mesh palette order, finite results, and fixture goldens. |
| Instance | Bind/rebind atomicity, structural compatibility, play/pause/stop, loop/clamp, large/zero delta, play rate, revisions, and retained candidate lifetimes. |
| Component | Registration, BeginPlay, tick, world pause/single-step, property/runtime replacement, EndPlay, unregister, destruction, and no-proxy behavior. |
| Asset/runtime | Authored/DDC/cooked/runtime-only Skeleton, SkeletalMesh, and AnimationClip payload ownership plus unchanged S1 import and StaticMesh projections. |
| Qualification | Focused EngineTests and asset regressions, runtime-only playback coverage, full Debug Editor `all` build, and clean native-test shutdown. |

All build and test commands follow
[Build and Run](../../../Development/Build/BuildAndRun.md). The implementation handoff
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

- [Skeletal Mesh and Animation Roadmap](../../../Roadmaps/Archive/2026-08/SkeletalMeshAndAnimation.md)
- [Skeletal Asset and Import Foundation](SkeletalAssetAndImportFoundation.md)
- [Rendering Capability Expansion Roadmap](../../../Roadmaps/Archive/2026-08/RenderingCapabilityExpansion.md)
- [Asset Data Lifecycle and Storage](../../../Runtime/Assets/AssetDataLifecycle.md)
- [Asset Import Framework](../../../Editor/Architecture/AssetImportFramework.md)
- [Core Math](../../../Runtime/Core/Math.md)
- [Static Mesh Rendering](../../../Runtime/Rendering/StaticMeshRendering.md)
- [Build and Run](../../../Development/Build/BuildAndRun.md)

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
