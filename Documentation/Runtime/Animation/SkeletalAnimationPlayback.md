# Skeletal Animation Playback

Summary: Define deterministic single-clip skeletal pose evaluation, playback ownership, and immutable palette publication.

Modules: Engine, SkeletalBuild, AssetForgeBuiltins

Last reviewed: 2026-08-30

## Ownership Boundary

`DSkeletalMeshComponent` is the public owner for one skeletal mesh, one optional
animation clip, persistent playback settings, and one
`FSkeletalAnimationInstance`. Assets remain immutable inputs: mutable time,
play state, looping, rate, revisions, and evaluated poses never live on
`DSkeleton`, `DSkeletalMesh`, or `DAnimationClip`.

SkeletalBuild registers the build function name
`Durin.GeometryBuild.AnimationClip`; its synchronous
session validates the complete animation payload against the Skeleton/target context.
AssetForgeBuiltins retains private Scene capture, clip naming, hard Skeleton
relationships, and transaction publication. Cache-only authored load never
invokes scene import, and a valid hit skips payload encoding and another store.

Cook projects the same target-qualified clip schema into the lazy
`PlatformData` BulkData field and preserves the hard Skeleton dependency and
compatibility identity. Cooked metadata load is range-free; binding setup locks,
decodes, validates, and transactionally publishes the clip payload without
source, importer, or DDC fallback.

Binding construction is the only playback operation that reads reflected asset
objects. A successful `FSkeletalAnimationBinding` detaches:

- the Skeleton compatibility identity, parent indices, and decomposed reference
  local transforms;
- immutable ownership of the optional clip track payload;
- the mesh-node bind transform and its checked inverse; and
- copied palette bone indices with aligned inverse-bind matrices.

The detached binding contains no object, package, registry, source/import,
component, scene, renderer, or RHI reference. Evaluation can therefore move to
a worker in a later change without changing its inputs, although the current
component evaluates synchronously through the owning game-thread tick.

## Compatibility and Binding

The mesh's Skeleton is authoritative. A clip is compatible only when its hard
Skeleton reference is valid and both the referenced and stored compatibility
identities equal the mesh Skeleton's structural identity. Asset names and
package paths do not establish compatibility.

Reference matrices must decompose into finite translation, normalized rotation,
and finite non-singular scale. Recomposition as `T * R * S` must reproduce the
canonical float matrix within `1e-5` per element. Singular, perspective,
unsupported shear, overflowed, missing, malformed, or non-finite inputs reject
the prospective binding.

Binding and rebinding are complete-or-null. The new detached inputs and their
time-zero pose are fully validated before the instance swaps state. Failure
retains the previous binding, time, play state, revision, and candidate. A
successful rebind starts at time zero; a component that was playing continues
playing when the new binding also has a clip.

## Sampling and Pose Evaluation

Every sample begins from the Skeleton reference-local transform. A track
replaces only its translation, rotation, or scale channel; absent channels keep
their reference values.

- A one-key track is constant. Samples outside a track's key interval use the
  nearest endpoint.
- Step interpolation uses the preceding key. An exact key time uses that key.
- Linear vectors interpolate component-wise.
- Linear rotations normalize their endpoints, choose the shortest quaternion
  path, perform spherical interpolation, and normalize the result. Exact keys
  use the normalized stored quaternion.

Durin uses column vectors and `[column][row]` matrix indexing. Local transforms
are `T * R * S`; parent-before-child component matrices are
`ParentComponent * Local`. A root component matrix is its local matrix.

For palette entry `i`, where `b = PaletteBoneIndices[i]`, the published matrix
is exactly:

```text
inverse(MeshNodeBindTransform) * BoneComponent[b] * InverseBindMatrices[i]
```

Palette order always follows the mesh palette, not Skeleton or source-joint
order. Every result must remain finite when narrowed to `FMatrix4f`; evaluation
never exposes a partial matrix array.

## Playback Clock and Controls

Clip duration is positive and finite under the asset contract. The instance
uses double precision for clock arithmetic and publishes the canonical float
sample time.

| Operation | Behavior |
| --- | --- |
| `Play` | Starts a bound clip without changing time or revision. Reference-only and unbound instances reject it. |
| `Pause` | Stops advancement without changing time or revision. |
| `Stop` | Pauses and returns to time zero; it publishes only when time changes. |
| `Seek` | Requires finite input, applies loop or clamp normalization, and publishes only when normalized time changes. |
| `Tick` | Requires finite, non-negative delta; paused, zero-delta, and zero-rate ticks are no-ops. |
| Play rate | Finite values greater than or equal to zero are supported. Reverse playback is unsupported. |

Looping time is normalized by modulo to `[0, Duration)`; exact positive
multiples resolve to zero. Non-looping time clamps to `[0, Duration]` and pauses
after the terminal pose publishes. Invalid clock input or failed evaluation
retains the complete previous state.

## Candidate Publication and Lifetime

`FSkeletalPosePalette` contains a non-zero monotonic revision within one bound
instance lifetime, normalized sample time, Skeleton compatibility identity, and
a counted mesh-palette-aligned matrix vector. The instance publishes it through
an atomic `std::shared_ptr<const FSkeletalPosePalette>`.

A reader may retain a candidate across later ticks, rebinding, asset payload
replacement, EndPlay, unregister, component destruction, or world replacement.
Candidates own all their data and never reference their producer or any asset.
Unbinding clears the producer's current candidate and resets its revision, but
does not invalidate candidates already held by readers.

Counts inherit the skeletal asset limits: at most 65,535 bones, palette count
between one and the bone count, aligned inverse-bind count, and at most
`MaximumSkeletonBones * sizeof(FMatrix4f)` candidate matrix bytes. Size
arithmetic is checked before allocation.

## Component Lifecycle

The component reflects editable SkeletalMesh, AnimationClip, auto-play,
looping, and non-negative play-rate properties. Runtime setters and reflected
edit drafts validate the whole prospective pair before changing live state.

- Construction enables component ticking.
- Registration prepares and publishes the time-zero binding when a mesh exists.
- BeginPlay recreates a binding after a prior EndPlay and applies auto-play.
- Actor/world tick advances only while the component is registered, begun,
  alive, and tick-enabled. World pause and single-step remain world-owned.
- EndPlay, unregister, and destruction detach the instance. Re-registration
  prepares a fresh time-zero binding.

Skeletal rendering is not part of this contract. `CreateSceneProxy` returns
null, and playback creates no render resource, render command, shader, or RHI
object. A later rendering plan consumes only retained immutable candidates and
must not read the component or assets on the rendering thread.

## Current Limits

The runtime supports one clip with Step or Linear translation, rotation, and
scale tracks. It does not implement blending, layers, state machines, root
motion, events, retargeting, IK, control rigs, reverse playback, compression,
streaming, worker scheduling, scene proxies, or skinning resources.

Focused contracts live in `SkeletalAssetTests` and
`SkeletalSceneLifecycleTests`. Runtime-only coverage cooks the repository glTF
and GLB graphs, removes authored content and DDC, loads the cooked assets, and
drives matching finite pose palettes through `DSkeletalMeshComponent`.
