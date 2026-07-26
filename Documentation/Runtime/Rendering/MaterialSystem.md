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

The current shader contract has exactly five built-in declarations:
`BaseColor`, `BaseColorTexture`, `Opacity`, `SpecularStrength`, and `Shininess`.
Their GUIDs are permanent because serialized overrides must survive renames and
because renderer extraction currently addresses this fixed contract directly.
This does not make five a general material-system limit; user-authored
declarations and a compiled renderer layout are deferred work.

## Renderer Boundary

- `DStaticMesh` owns the ordered material-slot definitions. Each slot has a
  persistent GUID, a mesh-local display name, exact import matching metadata,
  a source material index, and an optional default material. Imported order is
  the current runtime order; sections store only compact slot indices.
- Reimport reconciles identities by unique exact source name first and by a
  conservative unambiguous source-index fallback second. Reorder preserves
  identities, while removed or ambiguous slots do not silently transfer an
  assignment to another surface.
- `DStaticMeshComponent` persists a sparse collection of GUID-keyed material
  overrides. Resolution for each current slot is component override, mesh
  default, then empty `FMaterialRenderData` for renderer fallback. Overrides
  whose GUID is absent from the current mesh remain serialized as explicit
  orphans but do not resolve, bind dependencies, or reach the scene proxy.
- Material-side code resolves the five built-in GUIDs into immutable
  `FMaterialRenderData`. The renderer never performs GUID or `FName` lookup and
  never reads reflected material objects.
- Scene-proxy construction walks the current mesh slots in order and emits one
  compact `FMaterialRenderUpdate` snapshot per slot. A mesh assignment or
  rebuilt mesh render layout replaces the proxy; parameter-only material and
  parent changes update the existing proxy in place.
- A material update context batches changed roots, computes the affected loaded
  material closure from canonical parent chains, advances each affected
  material version once, and scans one stable loaded-object snapshot for
  static-mesh component slots that currently resolve to an affected material.
  Multiple roots merge dirty flags before any component update, and an inherited
  change adds the parent-chain dirty flag.
- The component scan reads only current mesh defaults and component overrides.
  One changed material emits an update for every current slot that resolves to
  it; duplicate slot use does not repeat the component scan. A component-wide
  revision orders rapid changes across independent slots and rejects stale
  render commands.
- The static mesh shader implements a fixed directional-light Blinn-Phong path
  plus an unlit viewport mode. Missing values preserve the orange fallback
  material and renderer-owned default textures.

## Dependency And Invalidation Model

- Material dependencies are forward-only. `DMaterialInstance::Parent` is the
  canonical relationship, and dependency tests walk that chain iteratively with
  a cycle guard. A material depends on itself; a base material has no other
  material dependency.
- Loaded direct-child and transitive-dependent queries scan a stable
  `GDObjectArray` snapshot on the game thread and return sorted,
  generation-safe object handles. They do not load assets, retain dependents, or
  expose the live object array during callbacks.
- Materials and static meshes own no reverse component collections, and
  instances and components keep no registered-value mirrors for Parent, mesh,
  or material assignments. Loading, reflected edits, transactions,
  duplication, destruction, and garbage collection therefore have no
  registration-reconciliation step.
- Material mutation flushes an Engine-owned `FMaterialUpdateContext`
  synchronously unless a caller supplies a context to batch several roots.
  Flush takes one loaded-object snapshot, computes the complete affected
  material set, scans loaded static-mesh components once, and resolves each
  handle immediately before use. Repeated flush without new roots is a no-op.
- Static-mesh render-data changes use a separate on-demand loaded-component
  scan and select components whose current mesh assignment equals the changed
  mesh. Rebuilding render state then resolves current slot definitions,
  defaults, overrides, and orphans directly from canonical storage.
- Deterministic counters expose roots, scanned objects, tested and affected
  materials, scanned components, and updated slots. These counters characterize
  batching and scan cost; they are diagnostics, not a persistent dependency
  index.

## Compatibility Boundary

Legacy scalar/vector/texture maps have no compatibility properties or implicit
migration. The asset loader skips their incompatible field records with a
warning. A legacy base material therefore retains constructor-provided canonical
defaults, while a legacy instance retains its compatible `Parent` reference and
has no old map overrides.

Static-mesh components persist only the GUID-keyed override collection. The
former index-shaped `Materials` collection, slot-zero `Material` mirror, and
unused override version field have been removed. If an older package still
contains those field records, the field-table loader skips them as unknown data;
they are not migrated.

## Static Mesh Vertex Contract

- Static meshes retain up to four UV channels. Missing channels are filled with `(0, 0)` while the LOD records how many source channels were present.
- Tangents are stored as `xyz` plus a handedness sign in `w`; the bitangent is reconstructed as `cross(N, T) * sign`.
- Missing normals and tangents are generated deterministically. Missing vertex colors use linear white, so they do not change the material base color.
- Each imported node-mesh instance becomes a section. Sections keep contiguous index ranges and reference a stable source material slot; source material assets are not created automatically.

## Design Rules

- Components and assets use `DMaterialInterface`; renderer code consumes only render data and shader maps.
- Mesh slot GUID is the only persistent component-assignment identity. Display
  names, source indices, current vector positions, and section indices are not
  persisted override identities.
- Parameter GUID is authoritative persistent identity; `FName` is lookup and
  display-facing identity, never serialized override identity.
- A valid definition set contains no invalid or duplicate GUIDs, `None` names,
  duplicate names, or deviations from the canonical built-in schema.
- Instances override parameters without duplicating declarations or the
  parent's shader program.
- Static properties belong in shader-map/permutation keys; dynamic parameters belong in uniform/resource bindings.
- Material object mutation crosses to the rendering thread through explicit
  update commands. Replacing the material assigned to a component still
  rebuilds its scene proxy because the set of render snapshots changes.

The prioritized implementation backlog and current editor/rendering limitations are tracked in `Documentation/Plans/MaterialSystem.md`.
