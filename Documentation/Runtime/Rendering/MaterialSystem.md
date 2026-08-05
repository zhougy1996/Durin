# Material System

Durin's material architecture keeps declaration ownership, instance resolution,
editor presentation, and renderer consumption at explicit boundaries.

## Parameter Domain

- `DMaterialInterface` is the common asset/component-facing contract. Persistent
  parameter identity is an `FGuid`; public human/API lookup uses
  case-insensitive `FName`.
- Runtime Engine owns `FMaterialParameterDefinition`, including identity, name,
  type, base value, display metadata, ordering, presentation, numeric range, and
  texture-usage hint. The Material Editor consumes this schema and does not own
  a parallel descriptor table.
- `DMaterial` stores one ordered reflected definition collection. Definition
  identity, type, order, and metadata are canonical; only the nested values are
  editable.
- `DMaterialInstance` references a parent material interface and stores one
  ordered collection of GUID/value overrides. Resolution walks the current
  instance, its parent instances, and the root material, and reports the object
  that supplied the value. Parent cycles are rejected.
- Parent changes preserve unmatched overrides as orphans for explicit editor
  removal. Orphans are never resolved into render data.
- `DMaterial` owns one reflected static-property set: blend mode, shading model,
  two-sided state, depth-write policy, and masked-opacity threshold. Instances
  inherit that complete set through the canonical parent chain; they do not
  store static overrides.
- Resolved static properties form a versioned shader-map identity (blend mode,
  shading model, and mask threshold) nested in a pipeline identity (shader map,
  two-sided state, and depth-write policy). The renderer lazily caches shader
  maps by shader identity and solid/wireframe pairs by pipeline identity, so a
  pipeline-only change does not rebuild the shader map.
- The identity split does not yet implement the render-pass policies. Cached
  entries still use the fixed opaque, depth-writing, two-sided pipeline;
  visible blend, mask, culling, and depth behavior belongs to a future
  render-pass plan.

The current shader contract is the canonical metallic/roughness PBR schema.
It declares BaseColor, tangent-space Normal, Metallic, Roughness, Ambient
Occlusion, Emissive, Opacity, and OpacityMask constants and Texture2D roles.
Every texture role also owns a UV-channel, UV-scale, and UV-offset parameter.
Their GUIDs are permanent because serialized overrides must survive renames.
Engine resolution compiles the declarations into the versioned v2 render
layout identified by `MaterialRenderLayoutV2Id`; the layout owns compact
uniform offsets and eight resource indices, while the asset schema version
remains a separate persistent compatibility boundary. User-authored
declarations and compiled layouts remain deferred work.

## Renderer Boundary

- `DStaticMesh` owns the ordered material-slot definitions. Each slot has a
  mesh-local user-facing `Name`, exact imported `SourceName`, original
  `SourceMaterialIndex`, and an optional default material. The vector index is
  the stable slot identity; sections store only that compact index.
- Reimport matches unique non-empty exact source names first and unique source
  indices second. Matched entries keep their existing index, user name, and
  default while source evidence is refreshed. Removed entries remain reserved,
  new entries append, and section construction consumes the explicit imported-
  to-stable map rather than searching historical source indices.
- `DStaticMeshComponent` persists a positional `OverrideMaterials` array.
  Resolution for each current index is non-null component override, mesh
  default, then an empty `FMaterialRenderData` whose representation is the
  deterministic renderer fallback. Mesh assignment preserves the complete
  array: shared indices apply immediately, entries beyond a smaller mesh are
  dormant, and a later larger mesh reactivates them. Dormant entries bind no
  dependency and never reach the scene proxy; Clear All removes them too.
- Material-side code resolves the canonical built-in GUIDs into one immutable
  `FMaterialRenderRepresentation` carried by `FMaterialRenderData` alongside
  the static shader/pipeline identity. `FMaterialRenderRepresentationBuilder`
  is the only GUID-to-layout compilation seam. The renderer consumes the
  validated v2 binding contract and never performs GUID or `FName` lookup or
  reads reflected material objects.
- `FStaticMeshRenderer` accepts only the exact supported v2 field table. It
  decodes the compact uniform bytes and resource slot through
  `TryGetMaterialRenderV2Binding`; an unsupported layout emits a
  `ShaderBinding` resource diagnostic and uses a complete default snapshot
  before shader-map or pipeline selection.
- Scene-proxy construction walks the current mesh slots in order and retains
  one stable `FMaterialRenderProxyRef` binding per slot. A mesh assignment or
  rebuilt mesh render layout replaces the proxy; dynamic parameter, static
  identity, and parent changes publish through the existing proxy in place.
- Dirty flags classify the publication that a material mutation produces:
  dynamic parameters and proxy-safe static properties publish a new immutable
  local layer, while parent changes publish a new parent-proxy identity and
  local version. The render thread resolves inherited state lazily.
- A material publication owns one pending wave per proxy. Repeated edits before
  the render command is consumed replace that pending wave, so only the newest
  immutable state is applied. The command stream preserves publication order
  before later rendering commands consume the proxy. If a material is edited
  while render-command admission is stopped, the retained wave is replayed when
  the rendering thread starts; the first preview or scene-proxy consumer does
  not observe an uninitialized snapshot.
- Component material assignment is a binding update, not material-content
  invalidation. A component-wide revision orders rapid changes across
  independent slots and rejects stale render commands.
- The static mesh shader implements Cook-Torrance GGX direct lighting and
  split-sum image-based environment lighting, plus an unlit viewport mode.
  Missing values preserve the orange PBR fallback material and
  role-appropriate Renderer-owned white, black, or flat-normal textures.

## Versioned Render Representation

`FMaterialRenderRepresentation` is an Engine-owned immutable snapshot with
three parts: a `FMaterialRenderLayoutIdentity`, a validated uniform byte
payload, and counted RHI texture-reference resources. The current v2 layout is
352-byte, 16-byte-aligned data with 32 uniform fields and eight resource
fields; its exact field table is identified by `MaterialRenderLayoutV2Id`.
The immutable v1 factory, validator, and decoder remain available only as a
compatibility boundary; current materials publish v2 and current StaticMesh
draws consume only v2.

Construction validates the version and identity, field counts, compact-index
contiguity, types, sizes, alignment, non-overlapping ranges, finite values,
zero padding, and resource counts before publication. Invalid construction
returns the complete deterministic fallback representation and a diagnostic;
it never publishes a partially filled payload. The representation retains no
reflected object or raw texture pointer.

`FMaterialRenderProxy` resolves parent and local layers into a copied builder,
publishes only a complete representation, and keeps the existing cache,
coalescing, stale-update, startup-replay, and counted-resource contracts. A
material schema version on `DMaterial` and `DMaterialInstance` upgrades missing
v0 state to the current asset schema and rejects unsupported future versions;
that persistent version is intentionally not the transient render-layout
identity.

## Dependency And Invalidation Model

- Material dependencies are forward-only. `DMaterialInstance::Parent` is the
  canonical relationship, and dependency tests walk that chain iteratively with
  a cycle guard. A material depends on itself; a base material has no other
  material dependency.
- Loaded direct-child and transitive-dependent queries share one stable
  `GDObjectArray` snapshot helper on the game thread and return sorted,
  generation-safe object handles. They do not load assets, retain dependents, or
  expose the live object array during callbacks. The helper reports the query
  operation, snapshot count, scanned work, and result count for diagnostics.
- Materials and static meshes own no reverse component collections, and
  instances and components keep no registered-value mirrors for Parent, mesh,
  or material assignments. Loading, reflected edits, transactions,
  duplication, destruction, and garbage collection therefore have no
  registration-reconciliation step.
- Ordinary material mutation does not construct or flush a global material
  update context. It publishes directly to the stable material proxy, performs
  no `GDObjectArray` snapshot, and performs no component enumeration.
  Parent and descendant proxies observe inherited changes during their next
  render-thread resolution without a child enumeration.
- The removed global material update context had no production callers after
  proxy publication. Explicit structural work now uses the generic primitive
  and scene lifecycle APIs owned by the initiating subsystem; material queries
  remain loaded-runtime queries only. `FlushRenderingCommands()` remains the
  explicit visibility boundary for tests, import, preview, and save workflows.
- Editor hierarchy queries that must include unloaded assets belong to the asset
  registry and package systems, not the runtime material object relationship.
- Static-mesh render-data changes use a separate on-demand loaded-component
  scan and select components whose current mesh assignment equals the changed
  mesh. Rebuilding render state then resolves current slot definitions,
  defaults, and positional overrides directly from canonical storage.
- Proxy diagnostics expose publication, coalescing, resolution-cache hit/miss,
  stale-publication, and binding-update counts. Loaded relationship queries
  expose their own operation, snapshot, scan, and result diagnostics. Both are
  diagnostics, not a persistent dependency index.

## Compatibility Boundary

Legacy scalar/vector/texture maps have no compatibility properties or implicit
migration. The asset loader skips their incompatible field records with a
warning. Material schema v1 upgrades copy BaseColor, BaseColorTexture, and
Opacity by their permanent GUIDs into the canonical v2 schema. Removed base
SpecularStrength and Shininess values are discarded with a warning; instance
overrides for those GUIDs remain explicit unresolved orphans. They are never
reinterpreted as Metallic or Roughness.

Static-mesh components persist only the positional `OverrideMaterials`
collection, and StaticMesh slots persist no GUID or slot-schema version. The
former GUID-keyed override records and slot fields have no loader alias,
upgrade branch, or migration path. Authored packages using those schemas are
incompatible; repository content was recreated directly under the current
schema.

## Environment Lighting

The studio image-based-lighting baseline is a hidden Engine asset at
`/Engine/Renderer/DefaultStudioEnvironment`, shared by level, preview, and
thumbnail rendering. It is neither a user material parameter nor scene
SkyBox state. The checked-in authoring payload contains deterministic
irradiance, GGX-prefiltered radiance, and a split-sum BRDF LUT generated only
by the independent `EnvironmentLightingBake` offline tool.

`DEnvironmentLighting` validates the versioned, checksummed authoring payload
and owns its Cook operation. Cook publishes the payload through the generic
package `.dbulk` companion without consulting DDC. Runtime loading accepts
only that cooked descriptor/payload pair. Renderer creates the RHI resources
on the render thread; an absent, invalid, or unavailable set resolves as one
black environment set, preserving direct lighting and Emissive. This internal
asset follows Engine content and asset-cook ownership rather than `DevTool`
build orchestration.

## Static Mesh Vertex Contract

- Static meshes retain up to four UV channels. Missing channels are filled with `(0, 0)` while the LOD records how many source channels were present.
- Tangents are stored as `xyz` plus a handedness sign in `w`; the bitangent is reconstructed as `cross(N, T) * sign`.
- Missing normals and tangents are generated deterministically. Missing vertex colors use linear white, so they do not change the material base color.
- Each imported node-mesh instance becomes a section. Sections keep contiguous index ranges and reference a stable source material slot; source material assets are not created automatically.

The render-resource ownership, physical stream layout, and vertex-factory
boundary are defined in [Static Mesh Rendering](StaticMeshRendering.md).

## Static Mesh Derived Data and Cooking

StaticMesh source provenance is an optional complete `FSourcePath` in any
allowed mount content directory and records the exact source hash, Assimp
importer version, and import axes. Source organization is independent of the
StaticMesh package path. Existing mounted sources are referenced without a
copy; external files require an explicit writable destination. Reimport reads
only the persisted source, while changing one reference, replacing shared
bytes, repair, and relocation are separate editor operations. Legacy
package-relative source fields are rejected. The
canonical DDC key also includes builder version 2, DMSH schema 3, and target
platform. A valid warm DDC object can load from persisted identity while source
and Assimp are unavailable.

DMSH schema 3 is a little-endian, checksummed chunk envelope for bounds, a
bounded material-slot count, LOD metadata, sections, vertex streams, and index buffers.
Readers bound all counts and ranges, reject invalid numeric data and indices,
skip only optional unknown chunks, and publish render data only after complete
validation. Schema 2 is rejected rather than converted. Cook uses stable payload ID
`6d9f79b5-7b68-4d91-a42c-2a6063fcab16`, strips source/import metadata, and
loads the DMSH payload only through the cooked package descriptor and DBLK
companion.

Material Preview acquires shared `/Engine/Models/Sphere` and
`/Engine/Models/Box` StaticMesh assets through the canonical
editor retention service. Multiple documents coalesce by virtual asset identity;
preview creation performs no transient OBJ import, and retained handles provide
the GC lifetime edge.

Content Browser Material and MaterialInstance thumbnails acquire the same
retained sphere and bind fully resolved `FMaterialRenderData`; they do not own a
second mesh import or a live viewport. Their cache key contains the material
package plus a deterministic, sorted, cycle-guarded closure of parent-material
and referenced-texture package fingerprints. A parent, local override, or
referenced texture change therefore regenerates the dependent preview, while a
warm PNG hit performs no asset load or render. Provider, scheduling, recovery,
and UI behavior belongs to the editor
[Asset Thumbnails](../../Editor/Architecture/AssetThumbnails.md) contract.

## Design Rules

- Components and assets use `DMaterialInterface`; renderer code consumes only render data and shader maps.
- Mesh slot vector index is the persistent component-assignment identity.
  Display names are renameable conveniences; imported names and source indices
  are reconciliation evidence; section indices consume the stable table.
- Parameter GUID is authoritative persistent identity; `FName` is lookup and
  display-facing identity, never serialized override identity.
- A valid definition set contains no invalid or duplicate GUIDs, `None` names,
  duplicate names, or deviations from the canonical built-in schema.
- Instances override parameters without duplicating declarations or the
  parent's shader program.
- Static properties are authored on base materials and inherited atomically by
  instances; per-instance static overrides require an explicit future
  permutation design.
- Static properties belong in shader-map/permutation keys; dynamic parameters
  belong in uniform/resource bindings. A dynamic-only update reuses the cached
  identity, while a static update causes the next draw to resolve or create the
  matching shader-map and pipeline entry.
- Material object mutation crosses to the rendering thread through explicit
  update commands. Replacing the material assigned to a component still
  rebuilds its scene proxy because the set of render snapshots changes.

Long-term sequencing and current editor/rendering limitations are tracked in
the [Material System Roadmap](../../Roadmaps/MaterialSystem.md); executable work
uses the bounded plans linked from that roadmap.
