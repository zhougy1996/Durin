# Skeletal Asset and Import Foundation Plan

Summary: Add bounded Skeleton, SkeletalMesh, and AnimationClip assets plus deterministic glTF import, reimport, derived data, cook, and runtime-load foundations without implementing playback or rendering.

Last reviewed: 2026-08-08

Status: Archived
Completed: 2026-08-08

## Current Status

All six stages are complete. The S1 child of the
[Skeletal Mesh and Animation Roadmap](../../../Roadmaps/Archive/2026-08/SkeletalMeshAndAnimation.md)
now supplies validated Skeleton, SkeletalMesh, and AnimationClip assets;
bounded direct glTF normalization; stable Scene peer publication; deterministic
DDC and cooked payloads; and runtime-only loading without import dependencies.

The lasting asset/storage and editor/import contracts have moved to their
owning documentation, and the roadmap records S1 completion plus a satisfied
S2 planning gate. S1 deliberately contains no playback clock, pose evaluator,
component, palette publisher, render resource, SceneProxy, skinning path, or
RHI work. No S2-S4 implementation plan was created.

## Goal

Make supported glTF skeletal source data a deterministic, bounded, and
transactional asset graph consisting of:

- one or more independent Skeleton assets with canonical reference
  hierarchies and structural compatibility identities;
- SkeletalMesh assets containing material bindings and validated geometry,
  influence, palette, bind, and bounds data that reference compatible
  Skeletons; and
- AnimationClip assets containing validated translation, rotation, and scale
  tracks that reference compatible Skeletons.

The editor can import and reimport those outputs through the existing Scene
provider. DDC and cook can publish and reload their external payloads, and a
runtime-only target can load the assets without source files, provider modules,
Assimp, or DDC fallback. Playback, pose evaluation, components, RHI resources,
and rendering remain later plans.

## Scope

- Define the first runtime `DSkeleton`, `DSkeletalMesh`, and
  `DAnimationClip` asset schemas, value representations, validation, and class
  registration.
- Define one structural Skeleton compatibility identity and explicit
  SkeletalMesh/AnimationClip references to a Skeleton.
- Extend StandardAssetImport's normalized result with format-neutral nodes,
  skins, mesh influences, inverse-bind data, and animation clips.
- Decode the selected glTF 2.0 skeletal subset from immutable captured source
  bytes with explicit limits, diagnostics, and cancellation.
- Preserve joint/weight/vertex correspondence without passing skeletal
  geometry through the static baked-node Assimp path.
- Extend the Scene provider's plan, preview, candidates, output identities,
  import record, typed exchange, and reconciliation for skeletal outputs.
- Add deterministic skeletal-mesh and animation payload codecs, DDC keys,
  cooked bulk descriptors, asset-level cook contributions, and transactional
  runtime decode.
- Add source, normalized-data, asset, provider, package, payload, cook, runtime
  closure, reimport, and malformed-input validation.
- Document lasting asset, payload, source-subset, and import contracts in their
  owning Runtime and Editor domains before completion.

## Non-Goals

- Pose evaluation, clip playback, animation instances, blending, root motion,
  events, animation graphs, state machines, retargeting, IK, or control rigs.
- SkeletalMeshComponent, SceneProxy, SceneInfo, bone-palette publication, RHI
  resources, skinning shaders, bounds updates during playback, or rendered
  previews.
- FBX skeletal import or DCC-specific bind-pose repair in the first source
  contract.
- Morph targets, animated morph weights, cameras, lights, constraints, or
  arbitrary glTF extension support.
- In-editor skeleton, mesh, or animation authoring; the first editor workflow
  is source import and reimport.
- Changing the current StaticMesh source schema, material behavior, geometry
  normalization, DMSH payload, render resources, or renderer.
- Complete project packaging orchestration beyond the existing asset-level cook
  seam.
- Production backward-compatibility readers for experimental skeletal schemas
  created before this plan completes. Repository fixtures and authored assets
  change with an intentionally replaced pre-release schema.

## Design Decisions and Invariants

### Layering and ownership

- `AssetImportCore` remains provider-neutral. New public generic contracts are
  allowed only when at least two providers need the same concept; skeletal
  value types and output policy belong to `StandardAssetImport`.
- `StandardAssetImport` owns glTF schema parsing, source-accessor decoding,
  normalized skeletal values, source diagnostics, output planning, and typed
  candidate construction. No glTF token, Assimp type, source buffer view, or
  provider identifier enters runtime asset headers or payload bytes.
- Runtime `Engine` owns Skeleton, SkeletalMesh, AnimationClip, their validation
  and compatibility rules, payload codecs, DDC builders, cook contributions,
  and cooked runtime decode.
- `AssetCore` continues to own package, DDC-object, DBLK, descriptor, manifest,
  and failure-atomic publication mechanics without interpreting skeletal
  payload records.
- Editor workers perform capture, parse, normalize, hash, validate, and encode
  on value-only state. Candidate `DObject` creation, package/registry mutation,
  typed exchange, and publication remain on the editor thread.

### Runtime asset graph

- `DSkeleton` is a package main asset containing a parent-before-child ordered
  reference hierarchy. Each bone has a canonical name, parent index or root
  sentinel, and finite local reference transform. The Skeleton stores a
  deterministic structural compatibility identity computed from the canonical
  hierarchy and reference-transform encoding.
- Bone array position is the canonical runtime bone index for one Skeleton
  schema. Persistent mesh and clip relationships are accepted only with an
  explicit Skeleton reference and matching compatibility identity; matching
  names alone are insufficient.
- `DSkeletalMesh` is a package main asset referencing exactly one `DSkeleton`.
  Its authored package contains the Skeleton reference, stable material slots,
  portable runtime metadata, and a logical cooked payload descriptor where
  applicable. Large geometry, influence, inverse-bind, section, and bound data
  lives in a skeletal-mesh payload.
- Inverse-bind matrices are SkeletalMesh palette data aligned to its referenced
  Skeleton indices. They do not live on the shared Skeleton because glTF skins
  may express mesh-specific bind relationships.
- `DAnimationClip` is a package main asset referencing exactly one
  `DSkeleton`. Its authored package contains the Skeleton reference, stable clip
  identity, duration/sample metadata needed for inspection, and a logical
  cooked payload descriptor. Large track, time, and value arrays live in an
  animation payload.
- Scene import outputs are peer main assets. The editor-only `DImportRecord`
  manages their source relationship; runtime dependencies exist only where a
  SkeletalMesh or AnimationClip references its Skeleton or a material asset.
- Skeleton is package-only in the initial schema. SkeletalMesh and AnimationClip
  use distinct version-1 asset payload codecs and builder versions selected in
  Stage 0. Their authored, payload, builder, provider, and target-profile
  versions remain independent.

### Source contract

- The initial source path is `.gltf` and `.glb` through the built-in Scene
  provider. FBX and other Assimp formats continue to produce only their existing
  supported outputs.
- The supported subset includes indexed triangle primitives with finite
  positions and the existing material attributes, one glTF skin association per
  imported skeletal mesh output, joint hierarchy, optional inverse-bind matrix
  accessor with the glTF-defined default, `JOINTS_0`, `WEIGHTS_0`, and
  translation/rotation/scale animation channels targeting imported joints.
- `JOINTS_1`/`WEIGHTS_1`, morph targets, animation weight channels, and
  `CUBICSPLINE` interpolation are rejected with structured unsupported-feature
  diagnostics in version 1. `STEP` and `LINEAR` interpolation are preserved as
  explicit track metadata.
- Version 1 stores at most four influences per vertex. Zero-weight influences
  are removed; duplicate joint entries are merged; remaining weights are sorted
  by descending weight with joint index as the deterministic tie-break,
  renormalized, and rejected when no finite positive total remains. Any lossy
  normalization is diagnosed.
- Source component widths, normalized integer encodings, byte strides,
  offsets, sparse accessors, count relationships, and arithmetic are validated
  before allocation or access. Stage 0 selects the exact accepted accessor
  encodings and whether sparse accessors are implemented or rejected in the
  initial subset.
- Unknown optional extensions are diagnosed and preserved under the current
  Scene-provider policy where they cannot affect skeletal correctness. Unknown
  required extensions and extensions that alter a required skeletal semantic
  fail planning before candidates exist.
- Skeletal outputs never infer vertex/joint correspondence from equal counts,
  source declaration order outside its normative glTF relationship, or Assimp
  allocation order.

### Coordinate, hierarchy, and time semantics

- Normalized data uses Durin's selected axis, handedness, distance, quaternion,
  matrix, and time conventions. Source-to-Durin conversion applies coherently
  to mesh bind geometry, joint local reference transforms, inverse-bind
  matrices, and animation local values.
- Skeletal geometry remains in the selected mesh bind space. The importer does
  not recursively bake animated node transforms into vertices as the static
  geometry adapter does.
- A Skeleton hierarchy is canonicalized parent-before-child. It contains no
  cycle, duplicate source joint, invalid parent, unreachable required joint, or
  non-finite reference transform.
- Multiple skeleton roots are represented only through a deterministic
  synthetic-root or multiple-root policy selected and fixture-frozen in Stage
  0; implementation does not choose per file.
- Animation time is non-negative seconds from clip start. Key times are finite
  and strictly increasing within one channel after the selected duplicate-time
  policy. Rotation values are finite, non-zero quaternions canonicalized to the
  selected normalized/sign convention.
- Missing animation channels mean the bone's corresponding reference-pose
  component. Import does not synthesize per-frame samples or resample tracks in
  this plan.

### Limits and failure policy

- Stage 0 records explicit maximum source bytes, nodes, skins, bones per
  skeleton, skeletal outputs, vertices, indices, influences, animations,
  channels, keys, decoded bytes, diagnostics, payload records, and payload
  bytes. All readers use checked arithmetic and validate counts before
  allocation.
- Invalid references, hierarchy, accessors, weights, inverse binds, animation
  channels, compatibility, payload records, or package relationships reject the
  complete affected candidate. The provider does not silently emit a static
  mesh in place of a requested skeletal output.
- Planning and candidate construction produce no package or registry mutation.
  Multi-output publication uses the existing root-last bundle transaction.
  Failure leaves existing assets and record byte-equivalent to their prior
  authored fingerprints.
- A DDC read treats missing, incompatible, truncated, corrupt, wrong-profile,
  or structurally invalid payloads as safe misses only when authoritative
  rebuild inputs are available. Cooked runtime load has no source or DDC
  fallback and fails the asset transactionally.
- Decode publishes complete immutable CPU data only after descriptor,
  container, checksum, schema, count, cross-reference, finite-value, bounds,
  and complete-consumption validation succeeds.

### Stable output and reimport policy

- Stage 0 freezes deterministic stable identities for every Skeleton,
  SkeletalMesh, and AnimationClip output, including multi-skin and animation
  association behavior. Display names are sanitized presentation and never the
  sole reconciliation identity.
- The Scene provider places outputs under type directories selected in Stage 0
  and records them as managed peer outputs. It does not select a SkeletalMesh as
  the import record's primary output.
- Reimport reconstructs the complete normalized result from one immutable
  snapshot, reuses compatible managed identities, performs symmetric no-fail
  state exchange, and reports missing prior outputs as orphans. It never
  silently deletes an orphan or adopts an occupied destination.
- A changed Skeleton compatibility identity cannot leave a published mesh or
  clip referencing incompatible old data. The candidate graph is validated as
  one relationship set before root-last publication.

## Current Foundations and Gaps

| Concern | Current foundation | Required change |
| --- | --- | --- |
| Scene normalized data | Images, materials, slots, positions, normals, tangents, UVs, colors, and indices | Add format-neutral hierarchy, skin, influence, inverse-bind, and animation values with limits and validation |
| glTF adapter | GLB/JSON parsing, dependency capture, materials, images, samplers, primitive-to-static correlation | Decode skeletal accessors and relationships directly from captured glTF/GLB buffers without static-path transform baking |
| Assimp adapter | Supported static FBX/glTF geometry and material behavior | Preserve behavior unchanged; do not use Assimp ordering as the skeletal correspondence contract |
| Scene provider | Multi-output mesh/material/texture plans, candidates, import records, and reconciliation | Add stable Skeleton/SkeletalMesh/AnimationClip outputs and relationship validation |
| Runtime assets | StaticMesh, material, texture, source provenance, typed exchange, cook contributions | Add three skeletal asset classes, validated CPU values, compatibility, exchange, and load behavior |
| Storage | DAST packages, content-addressed DDC, DBLK, manifest, transactional decode | Add independent skeletal mesh and animation payload schemas, keys, descriptors, cook and runtime validation |
| Tests | Import, Scene provider, reimport, StaticMesh payload/cook, runtime closure, and Vulkan fixtures | Add licensed skeletal goldens, malformed input corpus, package/payload/cook round trips, and unchanged-static baselines |

## Implementation Stages

### Stage 0: Freeze the source, asset, and storage contract

Outcome: implementation begins from fixture-proven source relationships,
coordinate semantics, output identities, limits, and versioned payload
boundaries rather than assumptions about glTF or Assimp behavior.

- [x] Select repository-owned or redistributable minimal `.gltf` and `.glb`
  fixtures covering one skinned mesh, multiple mesh primitives, non-identity
  mesh/joint transforms, nontrivial inverse binds, four influences, STEP and
  LINEAR translation/rotation/scale tracks, multiple animations, and material
  relationships. Record source and license in the fixture directory.
- [x] Add malformed or generated fixtures for invalid joint indices, cyclic or
  disconnected hierarchy, count mismatch, zero/NaN weights, non-finite bind
  matrices, invalid animation targets, unordered/duplicate key times, truncated
  accessors, unsupported interpolation, unsupported secondary influence sets,
  required extensions, and resource-limit overflow.
- [x] Freeze exact source-to-Durin formulas with golden mesh positions, joint
  local reference transforms, inverse-bind matrices, and animation values for
  identity, rotated, translated, scaled, and handedness-changing cases.
- [x] Freeze the multi-root, multi-skin, mesh-instance, shared-skeleton, and
  animation-to-skeleton output policy. Record explicit accepted and rejected
  shapes instead of relying on adapter behavior.
- [x] Freeze stable output identities, type directories, collision behavior,
  compatibility hash encoding, bone canonicalization, influence normalization,
  duplicate key-time handling, rotation sign canonicalization, and optional
  inverse-bind defaults.
- [x] Select exact accessor encodings and sparse-accessor policy for the initial
  subset. Record the distinction between unsupported, malformed, lossy, and
  resource-limit diagnostics.
- [x] Record bounded limits for every source, normalized, asset, and payload
  collection plus total decoded bytes, using existing Scene/StaticMesh limits
  where their semantics match.
- [x] Freeze version-1 SkeletalMesh and AnimationClip payload magic, schemas,
  builder identities, DDC key inputs, target/profile fields, logical payload
  identities, authored metadata, and cook stripping.
- [x] Characterize current accepted StaticMesh outputs and Scene provider
  diagnostics for the shared glTF fixtures so subsequent stages can prove no
  unplanned static-path change.
- [x] Record the exact Engine, StandardAssetImport, AssetCore, test, and fixture
  working set plus direct symbol ownership for Stage 1.

#### Acceptance Gate

- Every selected success fixture has a human-readable golden hierarchy,
  geometry/influence mapping, bind data, clip channels, output graph, and
  compatibility identity; every rejected fixture names one stable diagnostic
  category.
- Coordinate and inverse-bind goldens prove the selected convention with more
  than an identity transform, and applying the bind pose reconstructs the
  expected source-space relationship within a recorded tolerance.
- Payload/version/limit/output decisions are singular and reviewed; no Stage 1
  code must choose between competing ownership, source, or persistence models.
- Existing StaticMesh and Scene import baselines pass or any pre-existing
  failure is recorded without being attributed to this plan.
- The stage handoff records baseline commit, working set, symbols, decisions,
  open questions, and validation outcome.

#### Stage 0 Handoff

- Baseline commit: `ca6eed295a1d67a1ed3035a04cea4c40a1c9068c`
  (`docs(skeletal): define asset and animation roadmap`).
- Completed working set:
  `Engine/Tests/Data/AssetImport/Skeletal/`,
  `Engine/Tests/Native/AssetCoreTests/Private/AssetImportTests.cpp`, and this
  plan. The fixture directory contains three complete contract containers,
  three static projections, a reproducible generator, exact numeric and
  compatibility goldens, licensing, 18 malformed sources, and their manifest.
- Stage 1 working set: add runtime-owned headers and implementations under
  `Engine/Source/Runtime/Engine/Public/SkeletalMesh/`,
  `Engine/Source/Runtime/Engine/Private/SkeletalMesh/`,
  `Engine/Source/Runtime/Engine/Public/Animation/`, and
  `Engine/Source/Runtime/Engine/Private/Animation/`; register them through
  `Engine/Source/Runtime/Engine/CMakeLists.txt`; add focused asset tests under
  `Engine/Tests/Native/EngineTests/Private/` and that target's `CMakeLists.txt`.
  `AssetCore`, `StandardAssetImport`, and the fixture generator are read-only
  dependencies during Stage 1 unless a concrete missing generic seam is proven.
- Direct symbol ownership: Engine owns `DSkeleton`, `DSkeletalMesh`,
  `DAnimationClip`, canonical bone/material/clip summaries, detached payload
  values, validation, hard Skeleton references, and symmetric imported-state
  exchange. Existing `DStaticMesh`, its imported-state exchange,
  `FCookedPayloadDescriptor`, reflected object references, package archive, and
  Core math/hash APIs are the reference seams; no source/glTF type enters the
  new public runtime headers.
- Frozen decisions: glTF-to-Durin basis is `(-z,x,y)`; multi-root skins receive
  `$DurinRoot`; canonical traversal is parent-first/source-index order; one
  Skeleton is emitted per skin without coalescing; one SkeletalMesh is emitted
  per node/mesh association; clips are animation/skin pairs; stable identities
  and type directories are fixture-defined; compatibility is XXH3-128 over
  `DSKC` encoding version 1 and has golden
  `be0f679ef83133e5acfab7f12b688f54`; influence, rotation, key-time, accessor,
  limit, diagnostic, payload, DDC, logical cook, and stripping policies are
  singular in the fixture README.
- Open questions: none block Stage 1. The exact private split between authored
  asset implementation and detached payload validation may follow existing
  Engine conventions, but it may not change the frozen wire/source contract.
- Validation: fixture regeneration was byte-deterministic across 31 files; all
  18 malformed files parse as JSON and have one manifest category; compatibility
  bytes match the golden hash. `AssetImportTests` passed 14/14, including the
  new three-container static projection regression. `TextureTests` with
  `FSceneImportTests.*` passed 10/10. At the baseline commit, the complete
  skeletal contract triggers pre-existing Assimp access violation `0xc0000005`,
  while removing skeletal fields succeeds; this is recorded in the fixture
  README and must be eliminated by Stage 2's format-owned decoder rather than
  attributed to this stage.

### Stage 1: Introduce validated runtime asset schemas

Outcome: Engine can construct, validate, serialize, reference, exchange, and
reload Skeleton, SkeletalMesh, and AnimationClip authored state without source
parsing, payload persistence, playback, or rendering.

Dependencies: Stage 0's hierarchy, compatibility, authored/payload split,
limits, and version identities.

- [x] Add reflected `DSkeleton`, `DSkeletalMesh`, and `DAnimationClip` classes
  and module registration without adding editor/import dependencies to Engine.
- [x] Implement canonical bone, hierarchy, Skeleton compatibility, material
  slot, clip summary, source-independent payload descriptor, and runtime asset
  reference value types selected in Stage 0.
- [x] Add detached validated CPU representations for skeletal mesh and clip
  payload data. Keep RHI resources, mutable playback state, and source schema
  out of these types.
- [x] Enforce complete graph invariants: parent order, unique/canonical names,
  finite transforms, structural compatibility, valid palette indices, material
  sections, finite bounds, valid track targets, increasing times, explicit
  interpolation, and bounded allocation.
- [x] Implement symmetric, prepared, no-fail imported-state exchange for each
  asset whose Scene reimport can replace authored state. Reverse restores the
  exact pre-publication state and Finalize retires only detached old state.
- [x] Add authored save/load, duplicate, object-reference, unknown-field,
  compatibility, and failed-validation tests. Loading invalid relationships or
  state publishes no partial live asset.

#### Acceptance Gate

- Clean authored package round trips preserve exact Skeleton compatibility,
  asset references, material/clip metadata, payload descriptors, and canonical
  ordering.
- Malformed hierarchy, incompatible references, invalid values, or failed
  exchange preparation leaves the prior live state unchanged and produces an
  asset-qualified error.
- Engine and runtime-only dependency inspection contains no
  `AssetImportCore`, `StandardAssetImport`, Assimp, glTF parser, or editor image
  decoder edge.
- Existing package, StaticMesh, material, texture, and reflection tests retain
  their established behavior.
- The stage handoff records baseline commit, working set, symbols, decisions,
  open questions, and validation outcome.

#### Stage 1 Handoff

- Baseline commit: `201ddf0d7a3888e729167e6eca427b974e0605d6`
  (`test(skeletal): freeze import contract fixtures`).
- Completed working set:
  `Engine/Source/Runtime/Engine/Public/SkeletalMesh/Skeleton.h`,
  `Engine/Source/Runtime/Engine/Private/SkeletalMesh/Skeleton.cpp`,
  `Engine/Source/Runtime/Engine/Public/SkeletalMesh/SkeletalMesh.h`,
  `Engine/Source/Runtime/Engine/Private/SkeletalMesh/SkeletalMesh.cpp`,
  `Engine/Source/Runtime/Engine/Public/Animation/AnimationClip.h`,
  `Engine/Source/Runtime/Engine/Private/Animation/AnimationClip.cpp`,
  `Engine/Source/Runtime/Engine/Engine.dmodule`,
  `Engine/Tests/Native/EngineTests/Private/SkeletalAssetTests.cpp`,
  `Engine/Tests/Native/EngineTests/Private/Materials/MaterialSchemaAndEditingTests.cpp`,
  `Engine/Tests/Native/EngineTests/CMakeLists.txt`, and this plan.
- Key symbols and ownership: Engine owns reflected `DSkeleton`,
  `DSkeletalMesh`, and `DAnimationClip`; canonical `FSkeletonBone` and exact
  float32-row `FSkeletonTransform`; authored summaries and hard Skeleton
  references; detached `FSkeletalMeshPayloadData` and
  `FAnimationClipPayloadData`; complete validation; and prepared symmetric
  `FSkeletonImportedStateExchange`, `FSkeletalMeshImportedStateExchange`, and
  `FAnimationClipImportedStateExchange` transactions. No source, parser,
  provider, DDC, playback, or RHI state enters these runtime schemas.
- Decisions: Skeleton compatibility uses the Stage 0 `DSKC` version-1 bytes
  exactly. Reference transforms persist four canonical matrix rows because the
  frozen independently quantized float32 matrix cannot be reconstructed
  losslessly from TRS. Initialization canonicalizes every matrix component to
  float32 before validation and hashing. SkeletalMesh/clip payloads are
  immutable shared CPU values and remain deliberately absent after authored
  package load or ordinary duplication; authored summaries, descriptors,
  material metadata, compatibility, and hard references persist. Exchange
  preparation validates both complete states; Commit/Reverse only swap
  prepared fields and cannot fail. DHT's hermetic scanner currently treats an
  unmatched C++ digit separator before a reflection macro as a character
  literal, so reflected headers use equivalent undecorated limit literals.
- Open questions for Stage 2: none block implementation. The format-owned glTF
  decoder must construct exact canonical float32 matrices rather than a lossy
  TRS round trip, and must retain the frozen diagnostic/limit categories while
  leaving the Assimp static projection untouched.
- Validation: `SkeletalAssetTests` passed 6/6, covering the compatibility
  golden, invalid-state non-mutation, all three exchange types, hard-reference
  authored package round trips, exact descriptor/metadata/ordering
  persistence, duplication, and source-independent reflection. Regression
  targets passed: `AssetPackageTests` 105/105, `CoreObjectTests` 79/79,
  `StaticMeshTests` 49/49, `TextureTests` 62/62, and `MaterialTests` 78/78.
  Runtime source/dependency inspection found no `AssetImportCore`,
  `StandardAssetImport`, Assimp, glTF, editor decoder, or source-schema edge in
  the new Engine-owned files. `git diff --check` passed.

### Stage 2: Normalize the supported glTF skeletal subset

Outcome: StandardAssetImport converts captured `.gltf` and `.glb` bytes into
bounded format-neutral skeletal values with fixture-exact semantics and no
runtime or StaticMesh behavior change.

Dependencies: Stage 1 value/invariant contracts and Stage 0 source goldens.

- [x] Extend normalized Scene data with nodes, skins, skeletal primitive data,
  inverse binds, and clips using value-only types and explicit resource limits.
- [x] Decode required glTF buffers, buffer views, accessors, nodes, meshes,
  skins, joints, inverse binds, and animation samplers/channels directly from
  the immutable captured snapshot according to the Stage 0 subset.
- [x] Apply the selected source-to-Durin conversion coherently and canonicalize
  hierarchy, output associations, influences, rotations, and track ordering.
- [x] Validate every index/count/range/stride/component type, checked byte
  calculation, finite value, hierarchy edge, palette relationship, track
  target, key time, and interpolation mode before publishing normalized data.
- [x] Poll the existing import cancellation scope during bounded parse,
  accessor decode, normalization, canonicalization, and validation loops.
  Cancellation publishes no partial result.
- [x] Emit stable structured diagnostics for unsupported, lossy, malformed,
  missing-dependency, unsafe-path, and resource-limit outcomes without exposing
  third-party diagnostic strings as the contract.
- [x] Preserve existing static geometry/material/image results and diagnostics
  byte-for-byte where the new skeletal output has no specified reason to change
  them.
- [x] Add normalized success, malformed, cancellation, determinism, source
  containment, and limit tests for `.gltf` and `.glb`.

#### Acceptance Gate

- Every Stage 0 success fixture produces the exact canonical hierarchy,
  geometry/influences, inverse binds, clips, output associations, and
  diagnostics in repeated synchronous and asynchronous preparation.
- Every malformed/unsupported fixture fails at the selected boundary with no
  out-of-range read, unchecked allocation, partial normalized result, or raw
  Assimp/glTF-parser diagnostic contract.
- Joint/weight correspondence is derived from normative glTF relationships and
  cannot pass by total-count or Assimp-allocation-order coincidence.
- Existing StaticMesh normalized-output and Scene source tests remain
  unchanged except for explicitly asserted new skeletal records.
- The stage handoff records baseline commit, working set, symbols, decisions,
  open questions, and validation outcome.

#### Stage 2 Handoff

- Baseline commit: `c22e76c6b894c42999b2c9b0de09b99850bebc10`
  (`feat(skeletal): add runtime asset schemas`).
- Completed working set:
  `Engine/Source/Editor/StandardAssetImport/Public/ImportedScene.h`,
  `Engine/Source/Editor/StandardAssetImport/Private/ImportedSceneInternal.h`,
  `Engine/Source/Editor/StandardAssetImport/Private/ImportedScene.cpp`,
  `Engine/Source/Editor/StandardAssetImport/Private/GltfSceneAdapter.cpp`,
  `Engine/Source/Editor/StandardAssetImport/Private/GltfSkeletalDecoder.h`,
  `Engine/Source/Editor/StandardAssetImport/Private/GltfSkeletalDecoder.cpp`,
  `Engine/Source/Runtime/Engine/Public/SkeletalMesh/SkeletalMesh.h`,
  `Engine/Source/Runtime/Core/Private/Misc/Name.cpp`,
  `Engine/Tests/Native/AssetCoreTests/Private/AssetImportTests.cpp`,
  `Engine/Tests/Native/CoreTests/Private/NameTests.cpp`, and this plan.
- Key symbols and ownership: StandardAssetImport owns value-only
  `FImportedSceneNode`, `FImportedSkeletonData`,
  `FImportedSkeletalMeshData`, and `FImportedAnimationClipData`, plus the
  bounded `ImportGltfSkeletalData` decoder. The adapter captures every buffer
  before direct decoding and gives Assimp a sanitized, self-contained static
  projection. Runtime Engine types remain source-format independent.
- Decisions: version 1 uses the Stage 0 fixed glTF-to-Durin basis, exact
  float32 matrix canonicalization, source-index stable identities, canonical
  parent-first bone order, deterministic four-weight normalization, and one
  clip per compatible animation/skin pair. Tree traversal is iterative and
  records Euler intervals so maximum-sized valid hierarchies do not consume
  the C++ call stack or require repeated ancestor walks. The Assimp projection
  strips skeletal relationships and deterministically reuses the first
  float32 UV accessor when its current normalized-integer path would produce
  non-finite values, preserving the frozen static baseline while the direct
  skeletal payload retains the normative UVs. A direct dependency audit also
  found and repaired the Core invariant that FName entry zero must be reserved
  for `None`; without it, a synthetic root created as the process's first name
  displayed correctly but compared as None. Explicit payload equality replaces
  the deleted default comparison caused by `FBox`.
- Open questions for Stage 3: none block implementation. Provider planning
  must carry these immutable values in typed state and create Skeletons before
  dependent SkeletalMeshes and AnimationClips. Stage 3 should extend the
  existing asynchronous Scene equivalence/cancellation test with the skeletal
  graph so cancellation is observed through the same coordinator scope polled
  by this decoder.
- Validation: `AssetImportTests` passed 16/16, including exact canonical values
  and equal normalized outputs for data-URI `.gltf`, external-buffer `.gltf`,
  and `.glb`; all 18 frozen malformed/unsupported categories; source
  containment and resource limits; deterministic associations; and exact
  static-projection parity. `AssetImportCoreTests` passed 24/24, including the
  coordinator cancellation scope used by the decoder. Regressions passed:
  `CoreUtilityTests` 78/78, `SkeletalAssetTests` 6/6,
  `StaticMeshTests` 49/49, and `TextureTests` 62/62. `git diff --check`
  passed.

### Stage 3: Publish stable Scene-provider asset graphs

Outcome: the built-in Scene provider previews, imports, and reimports Skeleton,
SkeletalMesh, and AnimationClip peers through the existing transaction and
record model.

Dependencies: Stage 1 asset exchange and Stage 2 normalized source values.

- [x] Extend Scene planning with stable skeletal output identities, selected
  type directories, asset classes, create/replace/collision policy,
  relationships, resource estimates, and typed immutable provider state.
- [x] Construct detached Skeleton, SkeletalMesh, and AnimationClip candidates
  in dependency order without loading or mutating the destination packages.
- [x] Resolve material/texture outputs through existing Scene relationships and
  make every SkeletalMesh/AnimationClip reference its candidate or existing
  compatible Skeleton by exact managed identity.
- [x] Validate the complete candidate graph, structural compatibility,
  destination revisions, occupied paths, and management preconditions before
  publication.
- [x] Integrate typed prepared state exchange, root-last bundle publication,
  record fingerprints, stable provider state, and record-index updates without
  designating a primary output.
- [x] Reconcile create, replace, unchanged, removed/orphaned, relocated, and
  occupied outputs deterministically. Never silently delete or adopt an output.
- [x] Extend preview and diagnostics with skeletal counts, clip durations,
  influence normalization, unsupported features, estimated authored/payload sizes,
  relationships, collisions, missing outputs, and orphans.
- [x] Add import/reimport determinism, stale-plan, collision, rollback,
  relocation, cancellation, provider-unload, project-switch, and editor-shutdown
  tests for skeletal output graphs.

#### Acceptance Gate

- Importing a supported fixture publishes the selected stable peer assets and
  one import record; every runtime reference resolves to the final compatible
  asset and the record has no primary output.
- A repeated unchanged reimport is deterministic. Changed source replaces only
  planned managed state; removed outputs become reported orphans; collision,
  stale-plan, cancellation, or publication failure leaves all packages, loaded
  identities, registry state, record, and record index unchanged.
- Synchronous and asynchronous execution publish equivalent authored results
  and diagnostics, and worker paths construct no `DObject` or RHI state.
- Existing StaticMesh-only and mesh/material/texture Scene workflows preserve
  their accepted output identities and behavior.
- The stage handoff records baseline commit, working set, symbols, decisions,
  open questions, and validation outcome.

#### Stage 3 Handoff

- Baseline commit: `b2a6cf8e9bf5496685a9bc25532fb7ed35a44c40`.
- Working set: `SceneImport.cpp/.h` and
  `StandardAssetImportProviders.cpp`; the Runtime SkeletalMesh and
  AnimationClip imported-state validation/exchange seams; the glTF influence
  final-float canonicalization fix; `SceneImportTests.cpp` and
  `SkeletalAssetTests.cpp`.
- Key symbols: `ESceneOutputKind`, `FSceneOutputData::SkeletonIdentity`,
  `MakeSceneProviderState`, `FSceneCandidate::SetProspectiveSkeleton`,
  `DSkeletalMesh::ValidateAgainstSkeleton`,
  `DAnimationClip::ValidateAgainstSkeleton`, and their prospective-Skeleton
  `PrepareImportedStateExchange` overloads.
- Decisions: Skeleton outputs are planned first, followed by the existing
  StaticMesh/material/texture graph, then SkeletalMesh and AnimationClip
  dependents. Candidate dependents store the final managed Skeleton pointer but
  validate against its detached prospective state; publication commits in
  dependency order and reverses in the executor's existing reverse order.
  The typed version-1 provider state records every stable identity, role, path,
  class, kind, source index, and Skeleton relationship. Provider-neutral action
  results are reconstructed in import-record output order and no primary output
  is selected.
- The Stage 2 decoder's normalized floating-point residual could disturb exact
  equal-weight ordering after conversion to `float`. Final stored influences
  are now re-sorted by weight descending and bone index ascending; frozen source
  values and diagnostics remain unchanged.
- Open questions for Stage 4: none block implementation. Authored payloads are
  intentionally still immediate shared CPU data with empty cooked descriptors;
  Stage 4 owns deterministic codecs, DDC identities, cooked companions, and
  runtime-only loading.
- Validation: `TextureTests` passed 66/66, including skeletal graph preview,
  hard references, provider-neutral output order, unchanged reimport,
  synchronous/asynchronous equivalence, stale collision zero-publication, and
  root-last byte restoration. `SkeletalAssetTests` passed 7/7, including
  prospective dependency exchange commit/reverse. `AssetImportTests` passed
  16/16, `AssetImportCoreTests` passed 24/24, and `StaticMeshTests` passed
  49/49. `git diff --check` passed.

### Stage 4: Add derived data, cook, and runtime payload loading

Outcome: SkeletalMesh and AnimationClip large data follows authored/DDC/cooked
ownership and loads transactionally in editor and runtime-only targets.

Dependencies: Stage 1 payload representations and Stage 3 authoritative source
and authored relationships.

- [x] Implement the Stage 0-selected version-1 skeletal-mesh payload codec with
  explicit little-endian records for geometry streams, influences, indices,
  sections, palette/inverse-bind data, bounds, counts, sizes, and checksum.
- [x] Implement the selected version-1 animation payload codec with explicit
  clip, track, interpolation, time, value, target, count, size, and checksum
  records.
- [x] Reject wrong magic/schema/profile, truncation, overflow, overlapping or
  out-of-range records, duplicate identities, invalid cross-references,
  non-finite values, invalid weights/transforms/times, trailing required data,
  and allocation-limit violations before publishing CPU data.
- [x] Build canonical DDC keys from exact source closure hashes, normalized
  provider settings/state, compatibility identity, builder/payload versions,
  and target platform/profile. Never serialize a DDC path.
- [x] Store immediate editor payloads under disposable DDC ownership; treat
  corrupt/incompatible objects as safe misses only when the import framework
  can provide authoritative rebuild inputs.
- [x] Add logical cooked payload descriptors and asset-level `AddToCook`
  contributions. Publish DBLK companions before packages through
  `FCookContext`, strip source/provider/editor-only metadata, and retain exact
  runtime Skeleton dependencies.
- [x] Load cooked packages and payloads without source, DDC, import modules, or
  Assimp fallback. Failure leaves the previous runtime CPU payload absent or
  unchanged according to the asset lifecycle contract.
- [x] Add byte-determinism, round-trip, malformed corpus, DDC hit/miss/corrupt,
  clean cook, manifest, source/Assimp removal, cooked corruption, relationship,
  and runtime-only dependency tests.

#### Acceptance Gate

- Two clean builds from identical source/settings/versions/profile produce
  byte-identical SkeletalMesh and AnimationClip DDC objects and cooked payload
  bytes.
- Editor reload can consume validated DDC payloads; safe misses rebuild only
  from authoritative captured source; cache write failure does not invalidate a
  complete in-memory candidate.
- Cooked Skeleton, SkeletalMesh, and AnimationClip packages plus required DBLK
  companions load in a runtime-only target after source files, DDC,
  `AssetImportCore`, `StandardAssetImport`, and Assimp are absent.
- Every malformed descriptor/container/payload variant fails transactionally
  with an asset-qualified diagnostic and no partial CPU data publication.
- Existing DMSH, TXPL, DBLK, manifest, StaticMesh, texture, and ordinary
  package-only cook behavior remains unchanged.
- The stage handoff records baseline commit, working set, symbols, decisions,
  open questions, and validation outcome.

#### Stage 4 Handoff

- Baseline commit: `c20421a4c4b3dd22f99614933bbd1a3633e236c1`.
- Working set: new `SkeletalDerivedData.h/.cpp`; Skeleton, SkeletalMesh,
  AnimationClip headers and implementations; Scene execution key construction;
  `SkeletalAssetTests.cpp`, `SceneImportTests.cpp`, and this plan.
- Key symbols: `SkeletalMeshPayloadMagic` (`DSKM`),
  `AnimationClipPayloadMagic` (`DANM`),
  `FSkeletalDerivedDataKeyInput`, `Encode/DecodeSkeletalMeshPayload`,
  `Encode/DecodeAnimationClipPayload`, the two payload-input fingerprint and
  DDC store pairs, and `AddToCook`/`LoadCookedPayload` on the three assets.
- Decisions: both version-1 formats use a 64-byte explicit little-endian header,
  32-byte records, 16-byte aligned non-overlapping required chunks, zero
  padding, XXH3-64 body checksum, Win64/Game target fields, complete
  consumption, and transactional publication. Mesh uses seven chunks for
  metadata/bounds, sections, positions, attributes, indices, canonical
  influences, and palette/inverse binds; animation uses four chunks for clip
  metadata, contiguous track records, times, and typed values.
- DDC keys are XXH3-128 over builder/schema/target/profile, Scene provider
  identity/version, the exact ordered source-closure hash, normalized settings
  and typed provider-state hashes, stable output identity, Skeleton
  compatibility, and canonical payload-input fingerprint. The authored package
  stores only the rebuild key, never a physical path. Import writes are
  best-effort after a complete memory candidate; authored reload validates the
  DDC object and has no source fallback when authoritative inputs are absent.
- Cook publishes fixed type payload IDs inside each asset-qualified DBLK
  companion, writes the resulting logical descriptor into temporary package
  state, strips rebuild keys and source material metadata, restores editor
  state after serialization, and lets `FCookContext` publish companions before
  packages. Cooked-runtime mode checks descriptors and DBLK target/profile,
  loads the referenced Skeleton first, validates a detached CPU candidate, and
  never consults source, DDC, import modules, Assimp, or RHI.
- Open questions for Stage 5: none block qualification. S1 deliberately exposes
  immutable CPU payloads only; playback state, components, render resources,
  skinning, and RHI integration remain outside this plan and require the later
  evidence-gated S2 plan.
- Validation: `SkeletalAssetTests` passed 12/12 in both Debug DurinEditor and
  Shipping DurinGame, covering deterministic codecs, exact round trip,
  malformed headers/chunks/records/values, DDC hit/corruption/write failure,
  two byte-identical clean cooks, manifest/stripping, runtime dependency
  closure, and cooked corruption. A non-editor `Win64-Debug-DurinGame` Engine
  build passed. Regressions passed: `TextureTests` 66/66,
  `StaticMeshTests` 49/49, `AssetImportTests` 16/16,
  `AssetPackageTests` 105/105, `AssetCookTests` 12/12, and
  `AssetDerivedDataTests` 3/3. `git diff --check` passed.

### Stage 5: Qualify the S1 boundary and publish lasting contracts

Outcome: the non-rendering skeletal asset/import foundation is documented,
validated end to end, and ready for a just-in-time runtime playback plan.

Dependencies: Stages 0-4.

- [x] Move implemented Skeleton/SkeletalMesh/AnimationClip schemas,
  compatibility, payload, DDC, cook, runtime-load, source-subset, output, and
  reimport contracts into the owning Runtime Assets and Editor Architecture
  documentation.
- [x] Update the Skeletal Mesh and Animation Roadmap S1 status with completion
  evidence and re-inspect the S2 entry gate without creating the S2 plan.
- [x] Run focused CoreDObject, AssetCore, Engine asset/payload/cook,
  AssetImportCore, StandardAssetImport, Scene provider, editor workflow, and
  runtime dependency-closure tests through the repository build/test entrypoint.
- [x] Run deterministic repeated import/reimport/cook and runtime-only load
  workflows for representative `.gltf` and `.glb` success fixtures and the
  malformed corpus.
- [x] Complete a successful full `all` build because the Scene import workflow
  is user-visible, following the repository build instructions and selected
  Agent Build Profile.
- [x] Run the all-plan validator, inspect the complete task diff/status, and
  record any deliberately deferred source or product capability only in the
  roadmap's evidence-gated follow-ups.

#### Acceptance Gate

- All required focused, aggregate, deterministic, malformed-input,
  runtime-closure, import/reimport, cook/load, and full-build validation passes,
  or a pre-existing unrelated failure is isolated with evidence and does not
  weaken a plan invariant.
- Lasting behavior is documented outside this plan, S1 roadmap completion is
  evidence-backed, and the S2 entry gate can be evaluated from implemented
  contracts and the final handoff.
- No playback, component, render-resource, RHI, or rendering implementation has
  entered S1, and no speculative S2-S4 plan file has been created.
- The final handoff records baseline commit, complete working set, key symbols
  and decisions, open questions for S2, validation outcome, and verified editor
  executable from the selected Agent Build Profile.

#### Stage 5 Handoff

- Baseline commit: `53f6b2d376c51b7aa6721b39e66a7a5a98ab8872`.
- Complete working set: `AssetDataLifecycle.md`, `AssetImportFramework.md`,
  `SkeletalMeshAndAnimation.md`, `SceneImportTests.cpp`, the new
  `SkeletalSceneLifecycleTests.cpp` and its `EngineTests/CMakeLists.txt` target,
  and this plan. No S2, S3, or S4 plan file was created.
- Key symbols and published contracts: `DSkeleton`, `DSkeletalMesh`,
  `DAnimationClip`, compatibility encoding version 1, DSKM/DANM schema and
  builder version 1, skeletal DDC namespaces/key inputs, the Scene provider's
  version-1 source subset and stable peer identities, prospective-Skeleton
  dependency validation, dependency-order exchange, DBLK publication, and
  cooked-runtime transactional load. The new
  `GltfAndGlbPublishEquivalentSkeletalGraphsAcrossRepeatedReimport` regression
  directly qualifies provider publication across both root container forms;
  `GltfAndGlbCookDeterministicallyAndLoadRuntimeOnly` isolates the AssetManager
  restart and qualifies the complete import-to-runtime lifecycle.
- Decisions: S1 ends at immutable validated CPU payload access. Skeleton is
  package-only; mesh and clip keep exact hard Skeleton relationships plus
  compatibility identities across authored and cooked ownership. glTF skeletal
  decoding remains format-owned and FBX remains static-only. S2 is ready to be
  planned just in time, but no playback/component/rendering behavior is
  inferred from the asset classes.
- Open questions for S2: select the single owner of clip time, loop/rate state,
  compatibility binding, local pose evaluation, and immutable palette
  publication after re-inspecting current actor/component tick and detached
  Scene publication seams. Freeze sampling edge cases, pose composition order,
  and numeric goldens before implementation. None of these questions weakens
  the completed S1 boundary.
- Validation: `CoreObjectTests` passed 79/79; `AssetPackageTests` 105/105;
  `AssetCookTests` 12/12; `AssetDerivedDataTests` 3/3;
  `AssetImportCoreTests` 24/24; `AssetImportTests` 16/16;
  `SkeletalAssetTests` 12/12 in Debug DurinEditor and 12/12 in Shipping
  DurinGame; `StaticMeshTests` 49/49; `TextureTests` 67/67, including equivalent
  data-URI `.gltf`/`.glb` publication and two stable reimports per container;
  `SkeletalSceneLifecycleTests` 1/1, including byte-identical clean cooks and
  runtime-only loads after deleting authored Engine/Game content and DDC;
  and `EditorAssetWorkflowTests` completed with 78 passed and one reported
  skip. The selected `Win64-Debug-DurinEditor-Tests` Profile completed a full
  `all` build and produced
  `Engine/Binaries/Win64/Debug/Runtime/DurinEditor/DurinEditor.exe`. The
  all-plan validator and final diff/status checks passed.

## Validation Matrix

| Area | Cases | Required result |
| --- | --- | --- |
| Source subset | `.gltf`, `.glb`, external/embedded buffers, selected accessor encodings, supported interpolation, optional/default inverse binds | Exact normalized goldens from immutable captured bytes |
| Coordinates | identity, translation, rotation, scale, handedness change, nontrivial bind and animated transforms | Geometry, reference pose, inverse binds, and tracks use one documented Durin convention |
| Hierarchy and compatibility | single/multiple roots policy, parent ordering, shared joints, multi-skin policy, incompatible skeletons | Canonical deterministic hierarchy; invalid or incompatible relationships reject before publication |
| Influences | 1-4 weights, zeros, duplicate joints, ties, normalization, invalid joints, zero/NaN totals | Deterministic bounded four-influence output or stable structured failure |
| Animation | multiple clips, STEP/LINEAR TRS, missing channels, rotations, duplicate/unordered time policy, unsupported cubic/morph channels | Exact track metadata and values or selected unsupported/malformed diagnostic |
| Provider | preview, stable identities, collisions, create/replace/unchanged, orphan, relocation, stale plan, rollback, cancellation | One deterministic peer graph and root-last failure-atomic publication |
| Authored assets | save/load, duplicate, hard references, compatibility identity, source stripping, unknown/incompatible fields | Exact canonical state or transactional rejection |
| Payload codecs | round trip, deterministic bytes, wrong magic/version/profile, truncation, overflow, invalid records/values/checksums | Complete validated CPU data or no publication |
| DDC and cook | hit, miss, corrupt, write failure, two clean cooks, manifest, source/provider removal | Correct authored/DDC/cooked ownership and byte-deterministic output |
| Runtime closure | cooked load with no source, DDC, import modules, Assimp, or editor decoders | Required assets and CPU payloads load through runtime-only dependencies |
| Regression | existing StaticMesh/Scene materials/textures, DMSH/TXPL, package, cook, editor workflows | No unplanned output, diagnostic, dependency, or lifecycle change |

## Definition of Done

- `DSkeleton`, `DSkeletalMesh`, and `DAnimationClip` have validated,
  reflected, package-loadable runtime identities and exact compatibility
  relationships.
- The supported glTF subset produces canonical hierarchy, skeletal geometry,
  influences, inverse binds, and animation tracks without relying on Assimp
  allocation order or static node-transform baking.
- Scene import and reimport publish stable peer asset graphs through existing
  plans, candidates, exchanges, records, reconciliation, and root-last bundle
  semantics.
- SkeletalMesh and AnimationClip DDC/cooked payloads are deterministic,
  bounded, checksummed, transactionally decoded, and independent of physical
  DDC paths.
- Runtime-only cooked loading requires no source, DDC fallback, provider,
  Assimp, editor decoder, `AssetImportCore`, or `StandardAssetImport`.
- Unsupported and malformed source/payload data produces stable diagnostics and
  no partial authored, registry, record, CPU payload, or cooked publication.
- Existing StaticMesh, material, texture, package, DDC, cook, Scene import, and
  editor behavior remains valid.
- Lasting contracts are documented in their owning domains, the roadmap records
  S1 completion, validation passes, and the implementation commit carries exact
  plan/stage provenance.

## Deferred Follow-Ups

- Runtime pose representation, clip sampling, time/loop control, compatibility
  binding, and immutable palette publication belong to roadmap S2 and receive a
  plan only after this plan completes.
- Skeletal component, skinning vertex factory, palette buffers, SceneProxy,
  SceneInfo, materials/passes/visibility/shadows, and rendered bounds belong to
  the shared roadmap S3/Rendering M4 plan after both entry gates complete.
- Content Browser thumbnails, skeletal inspector, animation preview, and
  production workflow qualification belong to S4 after the rendering seams are
  stable.
- FBX, retargeting, root motion, events, blending/graphs, IK, compression,
  streaming, compute skinning, and crowd optimization remain evidence-gated
  roadmap follow-ups.

## Related Documentation

- [Skeletal Mesh and Animation Roadmap](../../../Roadmaps/Archive/2026-08/SkeletalMeshAndAnimation.md)
- [Rendering Capability Expansion Roadmap](../../../Roadmaps/Archive/2026-08/RenderingCapabilityExpansion.md)
- [Asset Import Framework](../../../Editor/Architecture/AssetImportFramework.md)
- [Mounted Source Workflows](../../../Editor/Guides/MountedSourceWorkflows.md)
- [Asset Data Lifecycle and Storage](../../../Runtime/Assets/AssetDataLifecycle.md)
- [Asset Packages](../../../Runtime/Assets/AssetPackages.md)
- [Core Math](../../../Runtime/Core/Math.md)
- [Build and Run](../../../Development/Build/BuildAndRun.md)

## Related Code

- `Engine/Source/Editor/AssetImportCore/Public/AssetImportCore.h`
- `Engine/Source/Editor/StandardAssetImport/Public/ImportedScene.h`
- `Engine/Source/Editor/StandardAssetImport/Public/SceneImport.h`
- `Engine/Source/Editor/StandardAssetImport/Private/GltfSceneAdapter.cpp`
- `Engine/Source/Editor/StandardAssetImport/Private/AssimpSceneGeometry.cpp`
- `Engine/Source/Editor/StandardAssetImport/Private/SceneImport.cpp`
- `Engine/Source/Runtime/Engine/Public/StaticMesh/StaticMesh.h`
- `Engine/Source/Runtime/Engine/Public/StaticMesh/StaticMeshDerivedData.h`
- `Engine/Source/Runtime/AssetCore/Public/AssetSystem.h`
- `Engine/Source/Runtime/AssetCore/Public/CookedAsset.h`
- `Engine/Tests/Native/AssetCoreTests/Private/AssetImportTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/Texture/SceneImportTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/StaticMeshPayloadCodecTests.cpp`
