# SplineMesh and Native Construction Stage 0 Handoff

Summary: Freeze the implementation choices, affected ownership surfaces, reference workloads, and validation baseline for `SplineMeshAndNativeConstruction.md`.

Last reviewed: 2026-08-12

## Native Construction Contract

The persistent authored set is `NativeDefault + Instance`; the all-live set is
that authored set plus `Generated`. `AActor` will expose those two meanings as
`GetAuthoredComponents()` and `GetOwnedComponents()`. Existing
`FindComponent*` APIs retain all-live lookup semantics. Generated components
are held only in a non-reflected `GeneratedComponents` collection, use
`EComponentCreationMethod::Generated`, and are excluded from object-graph
archives, duplication source graphs, independent transaction capture, package
dirtying, rename/delete/duplicate/reorder commands, and authored component
comparison.

`FActorGeneratedComponentKey` is a value containing a namespace `FName` and a
stable `FGuid`. The SplineMesh actor uses namespace `SplineSegment` and the
outgoing start point ID. A key has one exact component class for a construction
generation. Duplicate keys, invalid keys, class mismatches, acquisition
failure, or an incomplete desired set fail the generation.

One game-thread reconstruction follows this order:

1. Reject a destroyed owner; coalesce a request made by an active pass.
2. Create a context over the last committed generated set.
3. Run `OnNativeConstruct(Context)`, reusing exact key/class matches and staging
   new unregistered candidates.
4. Validate the entire desired set before mutating the committed set.
5. Attach candidates, publish the new set, register candidates, and dispatch
   BeginPlay when the actor is beginning/playing.
6. Retire unclaimed old entries in inverse order: EndPlay, unregister, detach,
   destroy.
7. Run at most one coalesced follow-up pass.

Candidate failure destroys candidates and leaves the previous set registered
and live. Destruction suppresses follow-up work and retires all generated
components once. Spawn constructs after authored defaults exist and before
BeginPlay; successful load, graph duplication/PIE cloning, Undo/Redo replay,
and reflected/native authored mutation request the same public reconstruction
path. Reconstruction never marks a package dirty.

The current ownership audit is anchored in:

- `AActor::OwnedComponents`, `InstanceComponents`, `CreateDefaultComponent`,
  `AddInstanceComponent`, `RemoveOwnedComponent`, `SetHidden`, `BeginPlay`,
  `EndPlay`, `BeginDestroy`, and component lookup/name generation;
- `DActorComponent` registration, play, tick, pending-kill, and destruction;
- `DLevel::SpawnActorInternal`, `PostLoad`, actor destruction and level/world
  registration/play traversal;
- CoreDObject reflected graph serialization and `DuplicateObjectGraph`;
- Level Editor component tree add/duplicate/delete/rename, hierarchy display,
  viewport picking/visualizers, selection, Details, and Undo/Redo;
- World, LevelAuthoring, EditorHierarchy, EditorProperty, Viewport, and Spline
  fixtures that currently assume every owned component is authored.

## Deformation Contract

`FSplineMeshParams` uses double-precision component-local positions, Hermite
derivatives, radians, two-dimensional cross-section scale/offset, normalized
up, canonical LOD 0 forward minimum/maximum, `X/Y/Z` forward axis, and linear
attribute interpolation by default. `SmoothStep` uses `t²(3-2t)` including
exact endpoint values. A non-finite field or a non-positive canonical forward
extent is rejected atomically. A zero up vector normalizes to +Z.

Longitudinal `t` is `(sourceForward-min)/(max-min)`, clamped to `[0,1]`.
Hermite position and derivative use the existing Spline basis without an
implicit length multiplier. Perpendicular source axes follow the cyclic order:
`X -> (Y,Z)`, `Y -> (Z,X)`, `Z -> (X,Y)`. The frame satisfies
`Forward × Side = Up`. Up is projected off Forward; a singular projection uses
the least-aligned cardinal axis with X/Y/Z tie order. A zero derivative falls
back to the endpoint chord and then the selected source forward axis. Positive
roll rotates Side toward Up. UV and color values are passed through unchanged
by later vertex-domain work.

The checked-in CPU fixtures cover straight identity mapping for all axes,
curved/twisted/scaled/offset interpolation, reverse or zero derivatives,
parallel/zero up, endpoint exactness, orthonormal handedness, invalid finite
publication, closed-path seam correction, and a deterministic dense bounds
corpus.

For canonical cross-coordinate maxima `c0,c1`, endpoint scale maxima `s0,s1`,
and endpoint absolute offset maxima `o0,o1`, the expansion is:

```text
r = hypot(c0*s0 + o0, c1*s1 + o1)
```

Linear and smoothstep attributes remain inside their endpoint component-wise
intervals. Rotation preserves cross-section length, so every transformed
cross-section point lies within radius `r`. Cubic centerline positions lie in
the convex hull of the four equivalent Bezier controls; expanding that hull's
AABB by `r` therefore contains every deformed vertex of every LOD when all LOD
bounds are contained by the canonical source cross-section bounds. CPU tests
use `1e-8` containment tolerance; float shader parity uses `2e-5` normalized
position and basis tolerance unless Stage 4 image evidence selects a stricter
bound.

## Renderer Integration Baseline

As rechecked on 2026-08-12 after rebasing onto `dev`,
`RendererLightSceneContract.md` is Completed and
`ComputeRendererIntegration.md` remains Active. SplineMesh Stage 4 therefore
consumes the completed 320-byte per-view forward-light payload, including the
deterministic budget of at most one directional and four shared point/spot
lights, plus the existing environment payload. It must not restore the removed
single-light ABI or reconstruct light state per draw. Compute integration still
owns the future FXAA storage-intermediate/output transition, so Stage 4 must
recheck that plan immediately before editing shared prepared-view, shader-map,
pipeline, base-pass, or final-output code.

The affected renderer surface is the explicit proxy kind and typed `FScene`
membership, SceneInfo attach/update/remove, primitive visibility, Static and
Skeletal prepared geometry, vertex declarations/factories, material shader-map
identity, effective pipeline/sort keys, opaque/masked/translucent execution,
LOD selection, per-view lighting, counters, editor observation, resource reload,
and StaticMesh render-state retirement tests. The new cache discriminator is
`EVertexDeformationDomain::{Local,Spline,Skeletal}`; material identity alone
can never alias these domains. Deformation updates copy params and bounds into
one FIFO scene mutation and retain primitive/source resource identity.

## Frozen Workloads and Budgets

The reference source meshes are a 1 m straight 8-vertex box (12 triangles,
LOD0) and a 1 m road strip (256 vertices/254 triangles at LOD0, 64/62 at LOD1,
16/14 at LOD2). Paths are 8 straight segments, 32 curved/twisted segments, and
128 stress segments. The target is the repository's configured Windows Vulkan
adapter at 1920x1080, with a 32-segment continuous control-point drag sampled
for 300 updates after warm-up.

- reconstruction plus CPU derived data: p95 <= 4 ms for 32 segments;
- incremental geometry edit: no component identity/source GPU allocation
  change and p95 <= 2 ms excluding collision;
- GPU frame delta against equivalent undeformed draws: p95 <= 0.35 ms for 32
  road segments at 1080p;
- generated component retained CPU memory excluding shared asset/picking/
  collision payload: <= 4 KiB per segment;
- collision rebuild: p95 <= 8 ms per road segment and <= 32 MiB retained for
  the 128-segment stress path;
- editor drag must not miss more than one 16.67 ms frame because of synchronous
  reconstruction; exceeding collision budget requires the bounded strategy
  decision required by Stage 6.

## Baseline Commands

The frozen focused baseline passed on 2026-08-12 using
`Win64-Debug-DurinEditor`: `WorldTests` 97/97, `LevelAuthoringTests` 11/11,
`SplineTests` 24/24, `StaticMeshTests` 68/68, `MaterialTests` 78/78,
`RendererSceneContractTests` 8/8, `ViewportTests` 86/86,
`PhysicsSceneTests` 43/43, and `SceneImportVulkanTests` 1/1. No pre-existing
failure was observed in the frozen baseline.
