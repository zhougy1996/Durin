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
- Components bind each distinct material that currently resolves from an
  override or mesh default. One changed material therefore emits an update for
  every current slot that resolves to it. A component-wide revision orders
  rapid changes across independent slots and rejects stale render commands.
- The static mesh shader implements a fixed directional-light Blinn-Phong path
  plus an unlit viewport mode. Missing values preserve the orange fallback
  material and renderer-owned default textures.

## Compatibility Boundary

Legacy scalar/vector/texture maps have no compatibility properties or implicit
migration. The asset loader skips their incompatible field records with a
warning. A legacy base material therefore retains constructor-provided canonical
defaults, while a legacy instance retains its compatible `Parent` reference and
has no old map overrides.

Static-mesh components have an explicit compatibility path. Schema version zero
loads the former index-shaped `Materials` collection and slot-zero `Material`
mirror after the referenced mesh definitions are available, converts in-range
entries to GUID overrides, and retains excess entries as diagnosed orphans. New
saves write the GUID schema version and clear both legacy values. The two legacy
reflected fields remain private and non-editable because removing them would make
the current field-table loader skip the only migration inputs. They may be
removed only when version-zero static-mesh component assets are no longer a
supported input or a replacement package migration facility can read them.

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
- Material object mutation crosses to the rendering thread through explicit update commands. Replacing the material assigned to a component still rebuilds its scene proxy because the dependency binding changes.

The prioritized implementation backlog and current editor/rendering limitations are tracked in `Documentation/Plans/MaterialSystem.md`.
