# Aether Cooked Collision Geometry Plan

Summary: Add versioned shared convex and triangle-mesh collision payloads, deterministic asset cooking and acceleration, and render-independent StaticMesh world queries.

Last reviewed: 2026-08-12

Status: Archived
Completed: 2026-08-12

## Current Status

M0-M2 of the
[Aether Physics Evolution Roadmap](../../../Roadmaps/AetherPhysicsEvolution.md) are
complete. `FPhysicsScene` already provides generation-checked storage, hybrid
broad phases, Reference/Production/Compare execution, complete primitive and
compound dispatch, bounded scratch, and reconciled diagnostics.
`FCollisionGeometryRef` is an immutable shared two-word body reference, and
`DBodySetup` caches one primitive resource per authored revision.

StaticMesh already has deterministic source hashing, DMSH derived data,
companion-bulk cooking, transactional reimport, cooked-only runtime loading,
and a read-only Inspector. Collision does not participate in those paths:
arbitrary meshes remain collision-free, BodySetup stores only one primitive,
and AetherCore has no hull/mesh payload, asset BVH, or disk reconstruction seam.

M3 is active. Stage 0 completed on 2026-08-12 from entry revision `1501a569`.
Eleven executable characterization tests now freeze the source/policy matrix,
representative fixture corpus, hull input rules, deterministic 32-byte BVH
node and eight-triangle leaf, 64-depth/128-stack limits, complete 18-cell
algorithm/reference matrix, DCOL version-one key/payload bytes and corruption
classes, transactional failure, inspection fields, and accepted resource caps.

The 100,352-triangle Debug prototype produced a 3,261,872-byte encoded payload,
3,261,676 logical retained bytes, an 11,289,836-byte estimated builder peak, a
449,744,900 ns measured tree build, and a 563,356,600 ns measured complete
prototype encode. Its sparse outside query performed one node test and zero
feature tests versus 100,352 Reference feature tests.
The conservative two-million-triangle worst-case runtime equation is
119,999,968 bytes, below the accepted 256 MiB payload cap; the corresponding
estimated builder working set stays below 512 MiB.

Stage 1 completed on 2026-08-12. AetherCore now owns validated immutable convex
hull and triangle-mesh resources with stable feature access, exact bounds,
identity, retained-byte accounting, and unchanged two-word references. The
Reference path covers ray, sweep, and overlap for Box, Sphere, and Capsule
queries through hull plane/support tests, double-sided mesh ray tests, closest
triangle features, Box/triangle SAT, and bounded feature advancement. Focused
PhysicsScene validation passes 38/38 including all 18 target/operation/query
cells, malformed resources, stable ordinals, and 128 deterministic randomized
positive-scale transforms.

Stage 2 completed on 2026-08-12. AetherCore now captures canonical position/
index spans without render or picking ownership, builds permutation-stable
QuickHull topology, cleans triangle sources in source order, and emits the
frozen 32-byte-node/eight-triangle-leaf BVH with outward float bounds. Production
uses bounded near-first ray traversal and swept/local query bounds with complete
Reference fallback on structural failure. The 100,352-triangle qualification
resource contains 32,767 nodes at depth 15, retains 4,270,912 bytes, and has a
23,147,864-byte estimated builder peak. Sparse outside work is one node and zero
features; 32 randomized rays and all shape sweep/overlap cells have zero mismatch,
false negative, fallback, overflow, unsupported, or non-convergence.

Stage 3 completed on 2026-08-12. `DBodySetup` now reflects stable source mode,
query policy, build revision, and build status while retaining independent
simple/complex immutable caches. `DStaticMesh` captures detached LOD 0 position/
index bytes, builds the frozen canonical collision key, reads/writes the separate
`StaticMeshCollision/Objects` namespace, and publishes collision only after all
candidate work succeeds. The production key and 336-byte tetra DCOL bytes match
both Stage 0 golden hashes. Explicit authoring, DDC rebuild/hit, policy-only
invalidation, transactional failed mode changes, and reversible imported-state
exchange pass; PhysicsScene 42/42, StaticMesh 66/66, and AssetImport 17/17 pass.

Stage 4 completed on 2026-08-12. Cook now publishes independently versioned DMSH
and optional required DCOL payloads in one deterministic companion transaction,
while cooked BodySetup metadata retains only source mode, query policy, and the
exact DCOL descriptor. Runtime loads and validates both detached candidates
before publication, reconstructs the immutable collision resource without source,
DDC, importers, render initialization, or editor modules, and treats every
required descriptor/payload failure as a hard asset-load failure. Deterministic
recook package, companion, and manifest bytes match; StaticMesh 67/67,
AssetCook 12/12, PhysicsScene 42/42, and AssetImport 17/17 pass, and the
`Win64-Debug-DurinGame` launcher builds successfully. Stage 5 scene policy,
diagnostics, and inspection integration is complete. Feature geometry now
publishes through BodyInstance into the unchanged scene body record, scene bounds
come from transformed immutable asset bounds, and policy revisions republish or
remove bodies without changing component filters. Two-World sharing, replacement,
bounded 256-triangle debug samples, and read-only Inspector facts pass.

Stage 6 completed on 2026-08-12. The complete Debug native aggregate and full
Debug Editor `all` build pass; focused Debug suites remain green and the full
Release PhysicsScene matrix passes 43/43. The visible Editor PIE lifecycle and
hidden Editor lifecycle both exit normally, cooked-only runtime tests reconstruct
DCOL without source/DDC/render resources, and Debug plus Shipping Game launchers
build. The 100,352-triangle grid retains 4,270,912 bytes with a 23,147,864-byte
estimated builder peak; sparse Production work remains one node/zero features
against 100,352 Reference features, and 10,000 bodies retain one shared identity.
Lasting Runtime Collision, Inspector, and roadmap contracts now record the
completed M3 result and deliberate M4-M7 deferral. A direct Game hardware smoke
was attempted in both Debug and Shipping but stopped during Vulkan capability
initialization before project asset or collision loading; this pre-collision RHI
startup failure is recorded as an environment/runtime limitation rather than
misattributed to DCOL behavior.

## Goal

Make explicitly authored StaticMesh collision available to editor, PIE, and
cooked standalone runtime as an immutable, versioned, shared resource whose
query cost follows touched asset features rather than all triangles. Preserve
existing World query compatibility, remain independent of render-resource
lifetime, and reuse repository DDC and Cook publication contracts.

## Scope

- AetherCore immutable convex-hull and indexed triangle-mesh payloads.
- Deterministic builders from an explicit canonical StaticMesh LOD 0 source
  snapshot and a deterministic asset-local triangle BVH.
- Primitive Ray, Sweep, and Overlap against hull and mesh targets with stable
  feature selection and flat Reference evidence.
- BodySetup simple/complex selection, build settings, revisions, resource
  caches, and explicit failure state.
- StaticMesh collision DDC key/schema, cooked-bulk descriptor, source-change
  invalidation, transactional reimport, Cook, and runtime reconstruction.
- Resource, BVH, feature, Cook/load, fallback, memory, and timing diagnostics.
- Read-only collision facts in StaticMesh Inspector and bounded collision
  overlay detail.
- Randomized, adversarial, imported-asset, PIE, cooked-runtime, standalone,
  many-instance, and gameplay qualification.

## Non-Goals

- Automatic collision generation for every imported mesh; existing assets stay
  collision-free unless a source mode is explicitly authored.
- General post-import mesh editing, convex decomposition, multiple hulls,
  per-triangle materials/filters, or a writable collision editor.
- Reusing render buffers, render LOD selection, render readiness, or the editor
  picking BVH as collision truth.
- Dynamic/deformable meshes, skeletal collision, heightfields, terrain, SDFs,
  or runtime triangle cooking.
- Rigid-body simulation, events, CCD, workers, snapshots, or backend providers.
- New World query method families or public triangle/face IDs in gameplay hits.
- Mirrored, singular, or non-finite physics transforms.

## Design Decisions and Invariants

### Ownership and publication

- Module direction remains `Core -> AetherCore -> Aether -> Engine`.
  AetherCore owns immutable collision values/algorithms; Engine owns BodySetup,
  source capture, DDC, serialization, Cook, and inspection; AssetCore remains
  the generic object-store and cooked-bulk publisher.
- A StaticMesh BodySetup owns collision settings and optional simple and
  complex logical resources. Components and scene bodies retain only
  `FCollisionGeometryRef`; instances never duplicate asset arrays or BVH nodes.
- Collision builders receive an explicit canonical LOD 0 position/index value
  snapshot. They never borrow `FStaticMeshRenderData`, RHI objects, or editor
  picking acceleration.
- Imported meshes default to `None`. Authoring explicitly selects
  `ConvexHullFromLOD0` or `TriangleMeshFromLOD0`; the built-in Box retains its
  primitive setup. Reimport preserves settings and transactionally publishes
  render and collision candidates together.

### Simple-versus-complex policy

- Low-level query input gains `Default`, `Simple`, and `Complex` as a value-only
  complexity request. Existing World calls publish `Default` and preserve
  behavior and source compatibility.
- BodySetup owns `SimpleOnly`, `ComplexOnly`, or `SimpleAndComplex`. `Default`
  resolves to valid Simple first, then explicitly authored Complex. Explicit
  Simple or Complex requests never silently substitute the other resource.
- M3 may add complexity to existing parameter structs but adds no parallel
  World method family. Filters, ignore behavior, response, body ordering, and
  one-overlap-result-per-body remain unchanged.

### Geometry and feature semantics

- Convex geometry is one closed finite hull with deterministic vertex, plane,
  edge, and face order. Construction rejects fewer than four non-coplanar
  points, invalid topology/winding, and output beyond Stage 0 limits.
- Triangle geometry owns finite float positions, validated uint32 triplets,
  stable source triangle ordinals, local bounds, and one immutable BVH.
  Degenerate/duplicate-index triangles are removed deterministically and
  counted; entirely degenerate input fails.
- Mesh collision is double-sided. Stored winding defines the geometric normal;
  reported normals face incoming motion or the overlapping query shape.
- Target ties use `(time, triangle ordinal)` or the stable hull-feature
  ordinal. Compound child selection remains outside that tie; World closest
  results still use `(time, body handle)` and Overlap emits one result per body.
- Internal hits carry geometry kind, child index, and feature ordinal for
  diagnostics/tests. Gameplay result reflection remains unchanged.

### Algorithms and asset acceleration

- Reference scans triangles in stable ordinal order. Production traverses only
  BVH leaves intersecting a conservative local query bound and then executes
  the same qualified leaf test. Scene traversal knows no asset node or feature.
- The BVH is deterministic contiguous binary storage built by longest centroid
  axis, triangle ordinal final tie-break, and at most eight triangles per leaf.
  Stage 0 freezes node encoding, outward bounds, layout, scratch, and bytes.
- Ray/triangle is finite and double-sided. Sphere/Capsule distance and overlap
  use closest triangle features; Box/Triangle uses complete SAT. Sweeps use
  exact finite separation with bounded conservative advancement and the M2
  non-convergence contract.
- Convex targets use M2 support mapping; primitive analytic paths stay intact.
  Compare must prove complete result parity within frozen M2 tolerances.
- Traversal scratch is fixed or geometry-retained and bounded. Overflow,
  malformed state, unsupported dispatch, and non-convergence are explicit.
  Complete Reference fallback is exceptional; qualified workloads record none.

### Persistence, versions, and invalidation

- Collision has an independent payload ID, magic, schema, builder version, and
  `StaticMeshCollision/Objects` DDC namespace. It is not a DMSH render chunk.
- The canonical key includes collision builder/schema/platform, exact source
  identity, import-space settings, collision mode/settings, and canonical LOD
  0 geometry fingerprint. Paths, timestamps, DDC paths, render revisions, and
  process identities are excluded.
- Authored packages serialize intent/settings; rebuildable bytes stay in DDC.
  Cook copies validated bytes under a second logical descriptor in the package
  companion. Cooked packages omit source metadata and DDC keys.
- Encoding is deterministic little-endian data with explicit aligned ranges.
  Readers transactionally validate enums, counts, arithmetic, overlap, hashes,
  topology, bounds, node reachability, and allocation limits.
- Source/import/collision settings, builder version, or canonical LOD 0 changes
  invalidate collision identity. Materials, thumbnails, render readiness, and
  component transforms do not.
- Editor DDC corruption is a safe miss only with rebuild inputs. Cooked runtime
  has no source/DDC fallback: required collision failure rejects the StaticMesh
  without partial render or collision publication.

### Limits, failures, diagnostics, and inspection

- Stage 0 freezes measured hard limits. Initial ceilings are 256 MiB per
  collision payload, 2,000,000 retained triangles, and 256 output vertices for
  one hull. Raising them requires a recorded budget revision before Stage 1.
- Invalid input/topology, overflow, all-degenerate input, hull/allocation/limit
  failure, corrupt cache, or incompatible cooked data preserves prior state. A
  new asset remains non-collidable; no bounds or render-triangle fallback is
  synthesized.
- Diagnostics distinguish source/retained/removed triangles, vertices, hull
  features, nodes/leaves/depth, build time, payload/runtime bytes, cache/Cook/
  load status, node/leaf/feature tests, pruning, scratch, fallback,
  unsupported, and non-convergence. Disabled query diagnostics retain the M2
  no-allocation/no-log/no-atomic contract.
- StaticMesh Inspector remains read-only and reports mode/policy, counts,
  bounds, bytes, versions, cache/Cook status, last diagnostic, and revision/
  geometry coherence. Debug capture shows bounds and a capped hull/triangle
  sample without copying a whole mesh each frame.

## Current Foundations and Gaps

| Area | Existing foundation | M3 gap |
| --- | --- | --- |
| Scene/query | Compare oracle, hybrid broad phase, stable primitive/compound results | No convex/mesh target or asset work counters |
| Geometry | Shared immutable primitive/compound ref with bounds and bytes | No topology, arrays, BVH, or feature identity |
| BodySetup | Primitive authoring, revision, resource cache | No source/settings, dual resource, status, or descriptor |
| StaticMesh | Canonical LOD data, deterministic DDC/Cook, transactional reimport | Collision is not a detached build output |
| Asset data | Object store, companion bulk, descriptors, atomic publication | No collision key/schema/descriptor/loader |
| Acceleration | Scene BVHs and editor-only render-owned picking BVH | No serialized runtime asset BVH |
| Algorithms | Primitive matrix, support mapping, SAT/features, bounded casts | No hull builder or primitive/triangle matrix |
| Editor | Read-only Inspector and bounded collision overlay | No collision facts/failures/mesh visualization |

## Implementation Stages

### Stage 0: Freeze sources, formats, layouts, algorithms, and budgets

Dependencies: completed M2; current StaticMesh DDC, reimport, Cook, Inspector,
and runtime-load contracts.

- [x] Record one source revision and rerun focused PhysicsScene, StaticMesh DDC/
  Cook/import/reimport, and Sandbox baselines.
- [x] Add tetrahedron/cube hulls and open, closed, non-manifold, duplicate,
  degenerate, thin, large-coordinate, reversed, clustered, 100k-plus triangle,
  real imported world, and many-instance fixtures.
- [x] Freeze source modes, policy resolution, default compatibility, explicit
  failure, feature ties, normals, penetration, and scale semantics in tests.
- [x] Prototype hull and BVH layouts; freeze ordering, nodes/leaves, outward
  rounding, depth, builder/traversal scratch, and retained-byte equations.
- [x] Prototype the complete primitive/hull/triangle operation matrix against
  brute-force/high-precision evidence; freeze tolerances and iteration caps.
- [x] Freeze key bytes, payload fields/ranges, descriptor ID, corruption cases,
  compatibility policy, and one deterministic golden payload hash.
- [x] Measure cook time, peak memory, payload/runtime bytes, tree quality, and
  Reference/Production work; accept or revise initial ceilings before Stage 1.
- [x] Freeze transactional failure, counters/equations, Inspector fields,
  capture caps, and render-independence assertions.

#### Acceptance Gate

- Ownership, query, ordering, format, invalidation, limit, and failure choices
  have executable characterization or a failing fixture.
- One hull/BVH layout fits accepted limits and produces deterministic output.
- Existing primitive, StaticMesh, Cook, and Sandbox behavior remains green.

#### Stage 0 Handoff

- Entry baselines passed on `Win64-Debug-DurinEditor`: PhysicsScene 34/34,
  StaticMesh 52/52, AssetCook 12/12, AssetImport 17/17, and SandboxGameplay
  12/12. The added Stage 0 contract suite passes 11/11.
- Imported assets default to source mode `None`. Explicit modes are stable
  values 1 `ConvexHullFromLOD0` and 2 `TriangleMeshFromLOD0`; Default query
  selects valid Simple first and explicit Simple/Complex never substitutes.
- Mesh nodes are 32 bytes with outward-rounded float bounds. Leaves hold at
  most eight stable source ordinals; trees stop at depth 64 and traverse with a
  128-entry stack. Hull plane, half-edge, and face records are each 16 bytes.
- The accepted caps are 256 MiB runtime payload, 512 MiB builder peak,
  2,000,000 retained triangles, 256 vertices for one hull, and 256 debug
  triangles. The maximum-count runtime equation is 119,999,968 bytes.
- The 100,352-triangle fixture encodes to 3,261,872 bytes and builds in the
  recorded Debug profile with depth at most 15. A sparse outside bound performs
  one Production node test and zero feature tests; dense traversal reconciles
  every node and all 100,352 retained source ordinals.
- DCOL uses magic `0x4c4f4344`, key/schema/builder version 1, 64-byte header,
  32-byte chunk entries, 16-byte alignment, at most eight chunks, and an
  independent payload ID. The golden key hash is
  `31049dc20de3b54a742c931cb587ce92`; the tetrahedron prototype is 336 bytes
  with hash `e18caaa3799e0c65edea7a0af09edbf1`.
- Required chunks are ordered positions, indices, stable ordinals, and nodes.
  Magic/schema/platform/count/alignment/size/checksum/range/data corruption is
  rejected. Unknown required chunks remain incompatible; any future optional
  chunk policy requires a deliberate readable-schema revision.
- Convex input must be finite, closed, consistently oriented, manifold, and
  non-coplanar. Triangle meshes remove degenerate/duplicate-index triangles in
  source order, preserve retained source ordinals, accept open/non-manifold
  surfaces, and fail if nothing collidable remains.
- Hull ray uses plane clipping; hull overlap/cast uses support mapping. Mesh ray
  is double-sided; Sphere/Capsule use closest features; Box overlap uses SAT;
  mesh sweeps use bounded feature advancement capped at 32 iterations. Feature
  ties use time then source ordinal, tangency blocks only when motion enters,
  and only finite strictly positive scale is accepted.
- Inspector freezes 12 read-only facts covering mode/policy, triangle counts,
  bounds, bytes, versions, cache/Cook state, and revision coherence. Failure
  leaves prior output unchanged and debug capture never exceeds its fixed cap.

### Stage 1: Extend immutable geometry and the Reference oracle

Dependencies: Stage 0 resource, topology, query, and limit contracts.

- [x] Add immutable hull and triangle-mesh payload kinds without increasing the
  qualified scene body-record size.
- [x] Add validated construction, stable test/debug feature access, exact local
  bounds, identity, and complete retained-byte accounting.
- [x] Implement hull support/topology behind AetherCore while preserving all
  primitive analytic selection.
- [x] Implement brute-force Box/Sphere/Capsule Ray/Sweep/Overlap against hull
  and mesh targets, stable ties, normals, penetration, and tangency semantics.
- [x] Add malformed/oversized rejection, bounded status, and feature counters.
- [x] Run focused AetherCore/PhysicsScene matrix and randomized transform tests.

#### Acceptance Gate

- Resources are immutable, shared, bounded, transform-correct, and reject all
  invalid classes transactionally.
- The complete matrix matches Stage 0 evidence with stable finite results.
- Primitive/compound results, body size, and sharing budgets remain unchanged.

#### Stage 1 Handoff

- `FCollisionGeometryRef` remains 16 bytes and primitive retained accounting
  remains 208 bytes. Feature resources use a detached immutable payload and do
  not add fields to the qualified 176-byte scene body record.
- Convex construction accepts 4-256 finite vertices and closed, consistently
  oriented, manifold, non-coplanar triangle topology. It normalizes retained
  faces outward. Triangle construction accepts up to 2,000,000 valid triangles
  and preserves explicit stable source ordinals.
- Public read-only access exposes kind, exact local bounds, stable vertices,
  indexed triangles, source ordinals, identity, and complete logical retained
  bytes. Invalid input returns no resource and cannot mutate an existing one.
- Hull distance uses outward face planes plus primitive support points; mesh
  distance uses Sphere/triangle and Capsule/triangle closest features plus
  Box/triangle SAT. Sweeps use at most 32 conservative-advancement iterations.
- Mesh ray tests are double-sided. Feature winners use time then source ordinal;
  overlap reports finite outward normals and positive penetration, and contact
  tangency blocks only for entering motion.
- Geometry and scene diagnostics now retain explicit feature-test work with
  saturating overflow behavior. No ordinary Stage 1 matrix case reports
  unsupported, non-converged, fallback, or counter overflow.
- `PhysicsSceneTests` passes 38/38 on `Win64-Debug-DurinEditor`, including the
  18 hull/mesh operation cells and 128 seeded randomized transform cases.

### Stage 2: Build and accelerate deterministic collision payloads

Dependencies: Stage 1 construction and Stage 0 builder/BVH layout.

- [x] Add canonical collision-source capture independent of RenderData/picking.
- [x] Implement deterministic hull cooking, stable topology, cleanup, limits,
  and diagnostics.
- [x] Implement triangle cleanup/ordinals and deterministic BVH construction,
  depth/size limits, and exact byte accounting.
- [x] Implement Production traversal with conservative bounds, near-first
  closest traversal, strict-safe pruning, bounded scratch, and complete fallback.
- [x] Reconcile asset node/leaf/feature work with scene and geometry counters.
- [x] Qualify parity, permutation determinism, membership, zero false negatives,
  ties, and overflow/allocation faults.

#### Acceptance Gate

- Identical canonical input emits identical topology, BVH, bytes, and facts.
- Production/Reference have zero mismatch; sparse work is local and qualified
  queries have zero fallback, overflow, unsupported, or non-convergence.
- Failed builds publish no partial resource and preserve previous state.

#### Stage 2 Handoff

- `BuildConvexHull` lexicographically canonicalizes at most 256 finite points,
  removes duplicates/interior points, builds outward incremental QuickHull
  faces, and retains deterministic 16-byte planes, half-edges, and face records.
- `BuildTriangleMesh` rejects invalid addressing, removes degenerate and
  duplicate-membership triangles in source order, preserves original source
  ordinals, and fails with `EmptyAfterCleanup` when no feature remains.
- BVH nodes are 32 bytes, bounds are outward-rounded floats, leaves retain at
  most eight triangle references, depth is capped at 64, and traversal uses a
  fixed 128-entry stack. Identical canonical input reproduces nodes and leaves.
- Builder results report success, invalid input, empty cleanup, limit, depth,
  and allocation failure distinctly with source/retained counts, depth, nodes,
  retained bytes, and estimated peak bytes. Failed builds return no resource.
- Production ray traversal is near-first and prunes nodes strictly beyond the
  closest time. Overlap uses the current primitive bound; sweep uses the union
  of start/end bounds, so no potentially touched feature is omitted.
- Scene diagnostics separately reconcile asset node, leaf, and feature tests.
  Structural traversal failure is bounded and falls back to Reference; every
  qualified Stage 2 query records zero fallback and zero counter overflow.
- The 100,352-triangle grid has 32,767 nodes, depth 15, 4,270,912 retained bytes,
  and a 23,147,864-byte estimated Debug builder peak. An outside query tests one
  node/zero features; a central hit tests fewer than 128 nodes/64 features.
- `PhysicsSceneTests` passes 42/42 on `Win64-Debug-DurinEditor`, including hull
  permutation determinism, topology twins, cleanup/failure status, BVH bytes,
  32 randomized zero-false-negative rays, and Reference/Production shape parity.

### Stage 3: Integrate BodySetup, StaticMesh DDC, and reimport

Dependencies: Stage 2 build output and Stage 0 key/invalidation contracts.

- [x] Add reflected source mode, policy, normalized settings, independent build
  revision/status, and optional simple/complex caches to BodySetup.
- [x] Produce detached collision candidates only when explicitly authored.
- [x] Implement canonical key bytes and strict object-store read/write under the
  collision namespace, including safe misses.
- [x] Integrate source inspection, authored load/rebuild, unavailable-source
  cached load, and collision diagnostics.
- [x] Make imported-state exchange commit/reverse render, materials, BodySetup,
  collision, revisions, and diagnostics as one no-fail transaction.
- [x] Prove only collision-relevant edits invalidate geometry and registered
  components update exactly once after successful revisions.

#### Acceptance Gate

- Every DDC status follows asset-data rules and serializes no cache path or
  process identity.
- Reimport success/rollback preserves coherent render, collision, and bodies.
- Components/Worlds share one resource per asset revision without render or
  picking lifetime dependence.

#### Stage 3 Handoff

- Reflected source values remain `None=0`, `ConvexHullFromLOD0=1`, and
  `TriangleMeshFromLOD0=2`. Imported meshes remain `None` until an explicit
  `DStaticMesh::SetCollisionSourceMode` call succeeds.
- `DBodySetup` owns reflected source mode, Simple/Complex policy, independent
  build revision/status, authored revision, and transient immutable simple and
  complex caches. Default resolution selects valid Simple before Complex.
- Collision candidates copy finite LOD 0 positions and indices before build;
  published resources own no `RenderData`, picking acceleration, importer, or
  component lifetime. A failed build, encode, cache write, or mode change leaves
  the old mode, key, revision, and geometry identity unchanged.
- The canonical key includes source and geometry hashes, importer identity/
  version, axes, source mode, policy, normalized weld bits, builder/schema, and
  platform. Production emits the frozen 75 bytes and golden hash
  `31049dc20de3b54a742c931cb587ce92`.
- Collision DDC objects live only beneath `StaticMeshCollision/Objects` and use
  strict DCOL decode before publication. The tetra resource remains 336 bytes
  with hash `e18caaa3799e0c65edea7a0af09edbf1`; corrupt decode preserves output.
- A warm rebuild reports `CacheHit`; a cold explicit build reports `Rebuilt`.
  Missing/corrupt collision data is a safe miss while the detached LOD snapshot
  exists, including a render-DDC load when original source is unavailable.
- Query-policy edits change only collision key/state. Material-slot edits do
  not rebuild geometry. Successful publication uses one existing StaticMesh
  recreate context, so registered component render/physics state updates once.
- Imported candidates inherit authored collision settings, build detached
  collision with their render candidate, and exchange collision state together
  with source, materials, cooked descriptors, diagnostics, and render data.
  Commit/Reverse restores exact policy, key, build revision, and geometry.
- Focused validation passes PhysicsScene 42/42, StaticMesh 66/66, and
  AssetImport 17/17 on `Win64-Debug-DurinEditor`.

### Stage 4: Serialize, Cook, and load collision at runtime

Dependencies: Stage 3 lifecycle and Stage 0 persistent format.

- [x] Implement deterministic encode/decode and strict golden/corrupt fixtures.
- [x] Publish render and optional required collision descriptors in one
  companion transaction without merging schemas.
- [x] Serialize cooked BodySetup policy/descriptor while stripping source/DDC/
  editor state.
- [x] Validate render plus required collision completely before publishing either.
- [x] Cover missing/wrong/truncated/overlapping/oversized/hash/topology/BVH/
  manifest and mixed render/collision failures.
- [x] Prove runtime succeeds without source, DDC, importers, render init, or
  editor modules; verify deterministic recook hashes/manifests.

#### Acceptance Gate

- Identical inputs produce identical cache/cooked bytes and descriptors round-trip.
- Required failure is a hard asset load failure with no partial publication or
  source/DDC fallback.
- Editor, PIE, and standalone publish equal collision independent of rendering.

#### Stage 4 Handoff

- `DBodySetup` serializes the explicit collision source mode, query policy, and
  DCOL companion descriptor. Build revision/status, source import metadata, DDC
  keys, diagnostics, and immutable in-memory caches are excluded from cooked
  package state; successful runtime reconstruction publishes `CookedLoaded`.
- `DStaticMesh::AddToCook` contributes DMSH and, only for explicitly authored
  collision, DCOL as separate payload IDs and schemas to one `FCookContext`
  package. Descriptor selection is by ID rather than container order.
- Cooked loading verifies both descriptors against the DBLK table, decodes and
  reconstructs both detached payloads, checks DCOL mode/policy against BodySetup,
  and only then performs the no-fail render/collision publication path. Missing,
  mismatched, corrupt, or incompatible required collision rejects the asset with
  no source or derived-data fallback.
- The deterministic recook fixture proves byte-identical package, companion, and
  manifest output, equal authored/cooked geometry topology and retained bytes, cooked-only loading
  after source and DDC removal, and zero initialized render resources. It also
  proves mixed valid-render/invalid-collision and missing-collision hard failure.
  Existing DCOL and AssetCook fixtures cover golden bytes plus truncated,
  overlapping, oversized, checksum, topology, BVH, target, and manifest faults.
- Focused validation passes StaticMesh 67/67, AssetCook 12/12, PhysicsScene
  42/42, and AssetImport 17/17 on `Win64-Debug-DurinEditor`; the
  `Win64-Debug-DurinGame` `DurinLauncher` target also builds successfully.

### Stage 5: Integrate scene policy, diagnostics, and inspection

Dependencies: Stage 4 resources and Stage 2 qualified queries.

- [x] Publish resolved simple/complex geometry through BodyInstance without
  exposing payload internals or changing spatial-index ownership.
- [x] Preserve filter, ignore, motion, handle, bounds, ordering, and lifecycle
  across policy/revision changes.
- [x] Add unique resource, retained-byte, asset traversal/feature, fallback,
  and build/load diagnostics with O(1) scene capture/reset.
- [x] Add bounded hull/mesh debug detail and read-only Inspector collision facts.
- [x] Test registration, replacement, two Worlds, Level/World/PIE restart,
  policy/failure revisions, teardown, and render release with live queries.

#### Acceptance Gate

- Scene broad phase owns only the body bound and delegates features to AetherCore.
- Resource/work equations reconcile and debug/Inspector capture is bounded.
- Primitive/Box/Sandbox behavior remains unchanged; cooked collision survives
  render release and editor-independent runtime.

#### Stage 5 Handoff

- `FPhysicsScene::BuildBodyBounds` now transforms the exact immutable hull/mesh
  local bound into the existing one-bound scene representation. Feature arrays
  and BVH nodes remain asset-owned and `FBodyRecord` stays unchanged.
- StaticMesh components publish the BodySetup-selected simple/complex resource
  through the existing BodyInstance path and retain the exact published setup
  revision. Policy transitions remove or recreate only the scene body while
  preserving profile, filter, motion, component identity, and shared resource.
- Existing scene diagnostics already reconcile unique geometry resources,
  retained geometry bytes, asset node/leaf/feature work, pruning, fallback, and
  scratch in O(1) capture/reset. BodySetup now also retains exact DDC/cooked
  payload bytes for the bounded asset inspection snapshot.
- World collision debug snapshots retain the 4,096-body cap and add at most 256
  hull/mesh triangles across the snapshot. Level viewport overlays render only that sample;
  primitive debug shapes remain unchanged.
- `InspectCollision` returns value-only mode, policy, source/retained/removed
  triangle, BVH, bound, payload/runtime byte, version, cache/Cook, diagnostic,
  and revision-coherence facts. StaticMesh Inspector displays them read-only.
- Focused validation passes StaticMesh 68/68, PhysicsScene 43/43, and
  SandboxGameplay 12/12 on `Win64-Debug-DurinEditor`; the new lifecycle fixture
  covers feature queries, two Worlds, shared identity, policy removal/republication,
  exact published revision, replacement, and teardown.

### Stage 6: Qualify and complete handoff

Dependencies: Stage 5 end-to-end behavior and reconciled diagnostics.

- [x] Run the complete matrix across randomized/adversarial transforms,
  tangency, penetration, ties, compounds, filters, ignore, and policy fixtures.
- [x] Qualify small regressions, 100k-plus sparse/dense work, 10,000-instance
  sharing, build memory/time, payload bytes, DDC, reimport, Cook/load, PIE,
  standalone, and Sandbox.
- [x] Require zero mismatch/false negative and zero ordinary unsupported,
  non-convergence, overflow, or fallback; record controlled Release timings.
- [x] Prove asset work follows touched features while scene candidate gates hold.
- [x] Move lasting contracts to owning docs and update the roadmap with evidence
  and deliberate M4-M7 disposition.
- [x] Run focused targets throughout and final native `--target all` because M3
  crosses AetherCore, Aether, Engine physics/assets/components, AssetCore Cook,
  editor inspection/debug, and Sandbox.
- [x] Run a full `all` build for user-visible inspection/debug changes, verify
  that profile's editor executable, and validate all documentation.
- [x] Record revision, profile, versions, tests, budgets, measurements, and
  deferred limits before closing every passed checklist.

#### Acceptance Gate

- One handoff passes focused/full native tests, full build, docs, cooked runtime,
  PIE/standalone, render independence, sharing, parity, memory, and Release gates.
- M3 completes required query scalability without speculative conditional scope.

## Validation Matrix

| Boundary | Required evidence |
| --- | --- |
| Geometry | Hull/mesh validity, sharing, identity/features, bounds/bytes, scale, invalid/oversized rejection, and body size |
| Queries | Complete primitive matrix, policies, normals, tangency, penetration, ties, compounds, filters, ignore, and World ordering |
| Builders/BVH | Deterministic hull/topology/cleanup/ordinals/bytes, membership, bounds/depth/scratch, pruning, sparse/dense quality, and zero false negatives |
| Format | Golden keys/payloads, independent versions/ID, strict corruption validation, compatibility, target mismatch, and deterministic recook |
| DDC/reimport | All cache statuses, invalidation/stability, exchange/rollback, and component revision update |
| Cook/runtime | Optional absence, required descriptor, transaction/manifest, cooked-only load, no source/importer/editor/render dependency, and no partial publish |
| Resources | Peak/retained memory, caps, many-instance sharing, no render/picking ownership, teardown, and two Worlds |
| Diagnostics/editor | Build/cache/Cook/load and node/feature equations, O(1) capture, bounded overlay, read-only facts, failures, replacement, and PIE restart |
| Performance/gameplay | Small regression, 100k-plus Release work/time, 10,000 instances, existing Box/Sandbox, PIE, and standalone |
| Ownership | One-way modules, no World family expansion, no scene asset-node knowledge, no render truth, simulation/workers/backend, or public feature ID |

## Definition of Done

- Explicit StaticMeshes build deterministic hull or mesh collision from a
  canonical snapshot without borrowing render state.
- Payloads are independently versioned, strictly validated, bounded, cached,
  cooked, and reconstructed at runtime without source/DDC fallback.
- Resources are immutable/shared and scene instances do not duplicate geometry.
- Reference and Production match; Production is asset-accelerated with bounded
  scratch and zero qualified false negatives or exceptional statuses.
- Policy, revisions, rollback, component/World/PIE/standalone lifecycle, and
  render independence pass integration gates.
- Memory/build/traversal/timing/cache/Cook/debug/Inspector facts meet budgets.
- Lasting docs, roadmap disposition, full validation/build, executable check,
  and clean committed handoff are complete.

## Deferred Follow-ups

- Writable collision editing, multiple hulls, convex decomposition, and
  per-feature materials.
- Public child/face IDs until a gameplay consumer freezes compatibility.
- Heightfields/terrain, deformable/skeletal collision, runtime cooking, and
  streaming collision partitions.
- Conditional simulation/events (M4-M5), workers (M6), and providers (M7).

## Related Documentation

- [Aether Physics Evolution Roadmap](../../../Roadmaps/AetherPhysicsEvolution.md)
- [Aether Geometry And Narrowphase Plan](AetherGeometryAndNarrowphase.md)
- [Aether Scene Query Acceleration Plan](AetherSceneQueryAcceleration.md)
- [Runtime Collision](../../../Runtime/Physics/Collision.md)
- [Static Mesh Rendering](../../../Runtime/Rendering/StaticMeshRendering.md)
- [Asset Data Lifecycle](../../../Runtime/Assets/AssetDataLifecycle.md)
- [StaticMesh Inspector](../../../Editor/Guides/StaticMeshInspector.md)
- [Asset Derived Data and Cooking Plan](../2026-07/AssetDerivedDataAndCooking.md)
- [Native Tests](../../../Development/Build/NativeTests.md)

## Related Code

- `Engine/Source/Runtime/AetherCore/Public/Collision/CollisionGeometry.h`
- `Engine/Source/Runtime/AetherCore/Private/Collision/CollisionGeometry.cpp`
- `Engine/Source/Runtime/Aether/Public/Physics/PhysicsScene.h`
- `Engine/Source/Runtime/Aether/Private/Physics/PhysicsScene.cpp`
- `Engine/Source/Runtime/AssetCore/Public/DerivedDataObjectStore.h`
- `Engine/Source/Runtime/AssetCore/Public/CookedAsset.h`
- `Engine/Source/Runtime/Engine/Public/Physics/BodySetup.h`
- `Engine/Source/Runtime/Engine/Private/Physics/BodySetup.cpp`
- `Engine/Source/Runtime/Engine/Public/StaticMesh/StaticMesh.h`
- `Engine/Source/Runtime/Engine/Public/StaticMesh/StaticMeshDerivedData.h`
- `Engine/Source/Runtime/Engine/Private/StaticMesh/StaticMesh.cpp`
- `Engine/Source/Runtime/Engine/Private/StaticMesh/StaticMeshDerivedData.cpp`
- `Engine/Source/Runtime/Engine/Private/Components/StaticMeshComponent.cpp`
- `Engine/Source/Editor/StaticMeshEditor/Private/Widgets/MStaticMeshInspector.cpp`
- `Engine/Tests/Native/EngineTests/Private/Physics/PhysicsSceneTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/StaticMeshDerivedDataContractTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/StaticMeshDerivedDataCacheTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/StaticMeshCollisionStage0Tests.cpp`
- `Engine/Tests/Native/AssetCoreTests/Private/CookedAssetTests.cpp`
- `Sandbox/Tests/Native/Private/SandboxGameplayTests.cpp`
