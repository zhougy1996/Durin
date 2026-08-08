# Skeletal glTF Contract Fixtures

These repository-authored CC0 fixtures freeze version 1 of Durin's skeletal
glTF source, normalization, output-graph, asset, and payload contracts. Run
`python generate_fixtures.py` from this directory to reproduce every generated
file. `Contract.gltf`, `ContractExternal.gltf`, and `Contract.glb` contain the
same source graph using a data URI, an external buffer, and a GLB BIN chunk.
`ExpectedContract.json` is the human-readable numeric golden and
`SkeletonCompatibilityV1.bin` is the exact compatibility-hash input.
`StaticProjection.*` removes only skins, animations, and joint/weight
attributes from the same geometry/material/node graph and freezes the existing
static importer baseline.

The success graph contains two material primitives instanced by two mesh nodes,
two skins over the same four joints, non-identity mesh and joint transforms,
nontrivial inverse binds, an omitted inverse-bind accessor, four influences,
duplicate influences, float and normalized integer accessors, multiple roots,
and two animations with STEP and LINEAR translation/rotation/scale tracks.
Files under `Malformed/` are deterministic mutations of that graph;
`MalformedManifest.json` records their required diagnostic categories.

## Source and Coordinate Contract

- Version 1 accepts glTF 2.0 `.gltf` and `.glb` roots captured through the
  existing Scene-provider snapshot. Buffers may be an in-root data URI, a GLB
  BIN chunk, or a captured contained external dependency. Source readers never
  reopen a dependency after snapshot freeze.
- glTF values are right-handed, +Y up. Durin is +X forward, +Y right, +Z up.
  The fixed change-of-basis matrix `C` maps a source position `(x, y, z)` to
  `(-z, x, y)`. Matrices use column vectors and convert as
  `M_durin = C * M_gltf * inverse(C)`.
- Mesh positions, normals, and tangent directions are converted by `C`.
  Tangent handedness and triangle winding are negated/reversed because
  `det(C) = -1`. Skeletal geometry remains in its mesh node's local bind space;
  no scene-node transform is baked into its vertices.
- Each SkeletalMesh stores its converted global mesh-node bind transform.
  Joint reference transforms and animation-local transforms use the same basis
  conversion. A glTF inverse-bind matrix converts as
  `IBM_durin = C * IBM_gltf * inverse(C)`. An omitted inverse-bind accessor is
  exactly one identity matrix per source joint.
- Translation values use `C`. Rotation values are normalized and converted via
  their rotation matrix. Scale components map `(sx, sy, sz)` to
  `(sz, sx, sy)`. The numeric matrices, geometry, binds, and track values are in
  `ExpectedContract.json`; comparisons use absolute tolerance `1e-5`.
- At the reference pose, for every palette entry and mesh instance,
  `inverse(meshNodeBind) * jointGlobalBind * inverseBind` is the expected glTF
  mesh-local joint matrix after basis conversion. The contract fixture checks
  a rotated root, translated joints, nonuniform scale, and two non-identity
  mesh nodes rather than relying on an identity-only example.

## Accepted Source Subset

- Nodes use a finite TRS or a finite, decomposable matrix, never both. A skin's
  joint nodes must be unique, acyclic, reachable from its declared skeleton or
  active scene, and connected through finite static ancestors. Intervening
  non-joint nodes are collapsed into the next joint's effective local reference
  transform. Animation of such an intervening node is unsupported in version 1.
- Multiple joint roots are represented by one deterministic synthetic bone
  named `$DurinRoot`, with identity transform and parent `-1`. A source bone
  with that name is escaped to `$DurinRoot_1` before ordinary duplicate-name
  suffixing. A single source root does not receive a synthetic bone.
- Canonical bone order is parent-before-child depth-first order. Roots and
  siblings use ascending source-node index. Empty bone names become
  `Bone_<sourceNodeIndex>`; duplicate names receive `_1`, `_2`, and so on in
  canonical order. The canonical index is independent of `skin.joints` order;
  JOINTS values and inverse binds are remapped through the source-joint table.
- A skinned node must reference exactly one skin and one mesh. Every primitive
  must be indexed TRIANGLES and must contain finite POSITION, JOINTS_0, and
  WEIGHTS_0 attributes with equal counts. NORMAL, TANGENT, TEXCOORD_0 through
  TEXCOORD_3, COLOR_0, and material relationships follow the existing static
  subset but are decoded directly for skeletal geometry.
- POSITION/NORMAL are float32 VEC3, TANGENT is float32 VEC4, animation outputs
  are float32 VEC3/VEC4, inverse binds are float32 MAT4, and animation times are
  float32 SCALAR. Indices accept unsigned byte, unsigned short, or unsigned int.
  JOINTS_0 accepts non-normalized unsigned byte or unsigned short VEC4.
  WEIGHTS_0 and texture coordinates accept float32 or normalized unsigned
  byte/unsigned short values of the required vector width. `COLOR_0` accepts
  those encodings as VEC3 or VEC4, and VEC3 receives alpha `1`.
- Missing normals are generated from converted indexed geometry. Missing
  tangents are generated from UV0 and the final normals, with a deterministic
  orthogonal fallback for missing or degenerate UV0. A missing primitive
  material reference resolves to one implicit default material distinct from
  every declared source material.
- Accessor and buffer-view byte offsets and byte strides are honored when
  aligned and in range. Sparse accessors, JOINTS_1/WEIGHTS_1, morph targets,
  morph-weight animation, non-triangle primitives, CUBICSPLINE, and accessors
  with an unlisted component/type/normalization combination are unsupported.
- STEP and LINEAR sampler modes are stored per track. Times are finite,
  non-negative seconds and must be strictly increasing. Duplicate or decreasing
  times are malformed; version 1 does not merge, reorder, or resample them.
  Missing bone channels mean the corresponding reference-pose component.
- Rotation keys must be finite and non-zero. They are normalized, the first key
  is sign-canonicalized so `(w, z, y, x)` is lexicographically positive, and
  later keys are sign-flipped when their dot product with the preceding stored
  key is negative. A zero dot product uses the same lexical rule.

## Influence Contract

- Version 1 stores no more than the four slots supplied by JOINTS_0/WEIGHTS_0.
  A joint slot is first remapped to the canonical bone index. Non-finite or
  negative weights and out-of-range source-joint indices are malformed.
- Exact zero weights are removed and duplicate canonical bone indices are
  merged in double precision. Remaining entries sort by descending merged
  weight, then ascending canonical bone index. A vertex with no finite positive
  total is malformed.
- Entries divide by the double-precision total and are converted to float32.
  The float32 residual required to make the stored sum exactly `1.0f` is added
  to the first entry, then the result is revalidated as finite and positive.
  A source sum differing from one by more than `1e-6`, a duplicate merge, or a
  nonzero residual emits one `LossyNormalization` warning per primitive, capped
  by the common diagnostic limit. Deterministic examples are in the golden.

## Multi-Skin and Output Policy

- Version 1 creates one Skeleton per source skin. Structurally identical skins
  are not coalesced, although their compatibility identities are equal. This
  keeps reimport identities anchored to normative source associations and
  leaves cross-source deduplication to a later explicit feature.
- One SkeletalMesh is produced for each skinned node/mesh association. All
  supported primitives of that mesh become material sections in one LOD0
  payload. Reusing a mesh from two nodes with the same skin therefore creates
  two SkeletalMeshes with separate mesh-node bind transforms. Associating one
  mesh definition with different skins is rejected in version 1 because the
  primitive joint table would have ambiguous skin ownership; authors duplicate
  the mesh definition when the geometry is intentionally shared across skins.
- One AnimationClip is produced for every animation/skin pair when all channels
  target joints in that skin. A channel targeting a non-joint node, or a set of
  channels that cannot map wholly to one skin, is unsupported for the affected
  scene in version 1. Missing channels are not materialized.
- Stable identities are `skeleton:skin/<skin-index>`,
  `skeletal-mesh:node/<node-index>/mesh/<mesh-index>`, and
  `animation-clip:animation/<animation-index>/skin/<skin-index>`. Source array
  index is the reconciliation identity in version 1; names are presentation
  only. Reordering source arrays is an authored identity change.
- Outputs live under `Skeletons`, `SkeletalMeshes`, and `Animations` beneath
  the selected destination. Sanitized display names use skin, node/mesh, and
  animation names with the stable source indices as deterministic duplicate
  suffixes. Occupied unmanaged paths are collisions; import never adopts or
  overwrites them. Reimport resolves managed assets by stable identity, and a
  removed output is an orphan rather than an implicit deletion.
- Skeleton, SkeletalMesh, AnimationClip, material, texture, and existing static
  outputs are peer main assets under one `DImportRecord`. There is no primary
  skeletal output. Root-last publication validates all Skeleton relationships
  before any package becomes visible.
- FBX and other Assimp formats remain static-only. A glTF scene with valid
  skeletal data may still retain its established static outputs; adding the
  skeletal records must not change existing normalized static mesh bytes,
  material slots, images, dependencies, or pre-existing diagnostics.

## Compatibility Identity

Skeleton compatibility is `XXH3-128` over the complete
`SkeletonCompatibilityV1.bin` byte stream. Integers and float32 values are
little-endian. The stream contains:

1. ASCII magic `DSKC`, uint32 encoding version `1`, and uint32 bone count.
2. For each canonical bone: int32 parent (`-1` for a root), uint32 UTF-8 name
   byte count, the exact canonical name bytes, then the row-major 16 finite
   float32 entries of its canonical local reference matrix.

Before encoding, every matrix is validated as a finite decomposable TRS;
negative zero is replaced with positive zero and rotation sign is canonical.
Compatibility intentionally includes names and exact canonical float32
reference transforms. It excludes source indices, Skeleton asset path, inverse
binds, mesh node transforms, materials, and animation tracks. SkeletalMesh and
AnimationClip store both a hard Skeleton reference and this identity; both must
match. Name coincidence or identity coincidence without the hard relationship
is insufficient.

## Limits and Diagnostics

All limits apply before allocation and use checked arithmetic. A lower physical
container or package limit still wins.

| Collection | Version 1 limit |
| --- | ---: |
| Root source or one captured buffer | 2 GiB |
| Captured dependencies | 8,192 |
| Source nodes | 1,000,000 |
| Source meshes | 65,536 |
| Primitives per mesh | 65,536 |
| Source skins / Skeleton outputs | 4,096 |
| Bones per Skeleton, including synthetic root | 65,535 |
| SkeletalMesh outputs | 16,384 |
| Vertices per SkeletalMesh LOD0 | 100,000,000 |
| Indices per SkeletalMesh LOD0 | 300,000,000 |
| Sections per SkeletalMesh LOD0 | 65,536 |
| Material slots | 4,096 |
| Stored influences per vertex | 4 |
| Source animations | 4,096 |
| AnimationClip outputs | 65,536 |
| Tracks per AnimationClip | 65,536 |
| Keys per track | 16,777,216 |
| Total keys per AnimationClip | 100,000,000 |
| Diagnostics | 4,096 |
| Payload chunks/records | 64 |
| One decoded skeletal or animation payload | 8 GiB |
| Total normalized decoded bytes per scene | 16 GiB |

Stable source diagnostic categories are:

- `UnsupportedFeature`: valid glTF outside the selected subset. Unknown
  required extensions and semantics that affect skeletal correctness use this
  category. No skeletal candidate is produced.
- `MalformedSource`: invalid JSON relationships, references, ranges, counts,
  hierarchy, finite values, weights, binds, or times. No partial normalized
  skeletal result survives.
- `LossyNormalization`: a warning for a valid accepted source that required the
  specified deterministic influence or rotation normalization.
- `ResourceLimitExceeded`: a declared or decoded collection exceeds a versioned
  limit before allocation.

Existing missing-dependency, unsafe-path, cancellation, and material/image
categories retain their current meanings. A single failure is classified at
the earliest contract boundary in the order: source containment and limits,
structural/range validity, unsupported semantics, numeric canonicalization.
Third-party message text and Assimp allocation/order are never the contract.

## Authored and Payload Contract

- `DSkeleton` is package-only. It stores canonical bones and compatibility.
  `DSkeletalMesh` stores a hard Skeleton reference, compatibility identity,
  mesh-node bind transform, stable material slots, LOD0/section/bounds summary,
  source-independent payload descriptor, and editor-only rebuild metadata.
  `DAnimationClip` stores a hard Skeleton reference, compatibility identity,
  stable clip name, duration, track/key summary, payload descriptor, and
  editor-only rebuild metadata.
- Cook retains runtime hard references, compatibility, summaries, material
  bindings, and logical DBLK descriptors. It strips captured source paths and
  bytes, provider state, import-record state, normalized build inputs, physical
  DDC paths, and editor rebuild keys.
- Skeletal mesh payload magic is ASCII `DSKM` (`0x4D4B5344` little-endian),
  schema version 1, builder version 1, target platform, profile, flags, header
  size, chunk table, decoded/stored sizes, and XXH3-64 body checksum. Required
  chunks are metadata/bounds, sections, positions, vertex attributes, indices,
  influences, and palette/inverse binds.
- Animation payload magic is ASCII `DANM` (`0x4D4E4144` little-endian), schema
  version 1, builder version 1, target platform, profile, flags, header size,
  chunk table, decoded/stored sizes, and XXH3-64 body checksum. Required chunks
  are clip metadata, track records, key times, and typed key values.
- Both formats use 16-byte aligned non-overlapping chunks, zero padding,
  explicit counts and byte sizes, unknown-optional/unknown-required flags, full
  consumption, and the 8 GiB payload/64-record limits. No native struct image,
  pointer, `size_t`, reflected object, source token, or physical cache path is
  serialized.
- Skeletal DDC namespace/builder identities are
  `SkeletalMesh/Objects` / `Durin.SkeletalMesh.Builder.V1` and
  `AnimationClip/Objects` / `Durin.AnimationClip.Builder.V1`. Key input is the
  canonical little-endian tuple of builder and schema versions, target platform
  and profile, provider/parser version, exact captured source-closure hashes,
  normalized settings/state, output stable identity, Skeleton compatibility,
  and canonical payload-input fingerprint. The final key is XXH3-128.
- Logical cooked payload identities are
  `<asset-path>#SkeletalMeshPayload.v1` and
  `<asset-path>#AnimationClipPayload.v1`. Physical DDC/object paths never enter
  authored packages or cook manifests. DBLK companions publish before packages.

## Current Static Baseline

At baseline commit `ca6eed295a1d67a1ed3035a04cea4c40a1c9068c`, the full
contract reaches the current Assimp static geometry path and raises Windows
access violation `0xc0000005` before it can publish a normalized result. Removing
skins, animations, and joint/weight attributes eliminates the failure. This is
a recorded pre-existing skeletal-input gap, not an accepted static result;
Stage 2 must route the complete contract through the bounded format-owned
skeletal decoder without exposing an Assimp crash.

All three `StaticProjection` containers import through the current static path
as four mesh instances (two node instances times two primitives), two material
slots named `Red` and `Blue`, and no diagnostics. The data-URI, external-buffer,
and GLB results have equal material/mesh values; their root/dependency identities
and hashes differ by container as expected. The Stage 0 regression test freezes
those observations so later direct skeletal decoding cannot silently alter the
shared static geometry, node, or material behavior.

The projection also substitutes the contract's float32 UV accessor for its
normalized-uint16 UV accessor. At the same baseline, Assimp decodes the latter
to NaN; the complete fixture retains it as direct-decoder coverage, while the
projection deliberately characterizes only finite accepted static behavior.
