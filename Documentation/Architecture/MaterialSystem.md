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

- `DStaticMeshComponent` owns per-slot material overrides. Static mesh sections
  reference imported material slots, and the scene proxy snapshots one
  `FMaterialRenderData` value per slot.
- Material-side code resolves the five built-in GUIDs into immutable
  `FMaterialRenderData`. The renderer never performs GUID or `FName` lookup and
  never reads reflected material objects.
- `FStaticMeshSceneProxy` receives compact render-data snapshots. Material and
  parent changes propagate through explicit render commands and update the
  existing proxy in place.
- The static mesh shader implements a fixed directional-light Blinn-Phong path
  plus an unlit viewport mode. Missing values preserve the orange fallback
  material and renderer-owned default textures.

## Compatibility Boundary

Legacy scalar/vector/texture maps have no compatibility properties or implicit
migration. The asset loader skips their incompatible field records with a
warning. A legacy base material therefore retains constructor-provided canonical
defaults, while a legacy instance retains its compatible `Parent` reference and
has no old map overrides.

## Static Mesh Vertex Contract

- Static meshes retain up to four UV channels. Missing channels are filled with `(0, 0)` while the LOD records how many source channels were present.
- Tangents are stored as `xyz` plus a handedness sign in `w`; the bitangent is reconstructed as `cross(N, T) * sign`.
- Missing normals and tangents are generated deterministically. Missing vertex colors use linear white, so they do not change the material base color.
- Each imported node-mesh instance becomes a section. Sections keep contiguous index ranges and reference a stable source material slot; source material assets are not created automatically.

## Design Rules

- Components and assets use `DMaterialInterface`; renderer code consumes only render data and shader maps.
- Parameter GUID is authoritative persistent identity; `FName` is lookup and
  display-facing identity, never serialized override identity.
- A valid definition set contains no invalid or duplicate GUIDs, `None` names,
  duplicate names, or deviations from the canonical built-in schema.
- Instances override parameters without duplicating declarations or the
  parent's shader program.
- Static properties belong in shader-map/permutation keys; dynamic parameters belong in uniform/resource bindings.
- Material object mutation crosses to the rendering thread through explicit update commands. Replacing the material assigned to a component still rebuilds its scene proxy because the dependency binding changes.

The prioritized implementation backlog and current editor/rendering limitations are tracked in `Documentation/Plans/MaterialSystem.md`.
