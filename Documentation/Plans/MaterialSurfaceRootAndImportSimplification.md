# Material Surface Root And Import Simplification Plan

Summary: Unify material authoring around one Surface root, centralize surface evaluation rules, and generate imported graphs only for features present in the source material.

Last reviewed: 2026-09-14

Status: Active
Completed:

## Current Status

Planning only; implementation has not started. This plan records the user's
request for one Surface root shared by default, new, and imported materials,
without a second visible Material Output or a fully expanded PBR template.

The current baseline is commit `88aaedfb1`, following `42df4b322` and
`41fcd8de4`:

- New materials and DefaultMaterial use default property values and the existing
  Material Output root. The renderer supports Lit/Unlit surface shading and
  Opaque/Masked/Translucent blending, but no material-domain selector.
- Ordinary texture creation uses one combined texture sample parameter. Normal
  creation uses a texture parameter feeding SampleNormal. Ordinary sample outputs
  are RGB, R, G, B, A, RGBA; unused RG/resource outputs are advanced.
- ImportedSurface has been rebuilt with direct per-property outputs. It has
  82 expression nodes, 48 parameter owners, and one DecodeImportedNormalRG call.
  Removing ImportedSurfaceValues exposed its factor composition and clamps in
  the parent graph; it did not simplify the entire authoring model.
- SceneDirectImport creates material instances using one shared ImportedSurface
  parent and sets a fixed parameter set. Conditional graph generation therefore
  requires changing parent selection and override publication together.

The previous [parameter ownership plan](MaterialGraphOwnedParameters.md) is
completed. Preserve its single-owner parameter model; do not reopen it or restore
hidden parameter tables as a shortcut.

## Goal

Present one **Surface** root accepting final Base Color, Normal, Metallic,
Roughness, Ambient Occlusion, Emissive, Opacity, and Opacity Mask values. This
node is both the authoring interface and the graph's final-output anchor.
Default, new, and imported materials share this same contract.

Ordinary texture assignment must be equally simple whether performed by hand or
by importing a model. Additional visible nodes must correspond to actual authored
features, rather than bookkeeping or universal renderer rules.

## Selected Design

### One visible root, with a real compiler boundary

- Display the existing final-output role as Surface. Do not create a generic
  function call followed by another visible Material Output node.
- Retain one nondeletable root per base material. It owns final property bindings,
  fallback literals, presentation, and access to material settings. Do not give
  material instances editable graphs.
- The compiler still needs an explicit final-output anchor to select reachable
  expressions, validate types, and generate the appropriate shader passes.
  Hiding that implementation distinction must not remove it.
- An ordinary function returning a Surface aggregate remains composable data;
  it is not automatically the graph root. Existing aggregate-mode authoring must
  remain an alternative input mode of the same root, with no duplicate root or
  simultaneous conflicting per-property bindings.
- Keep domain, shading model, and blend mode distinct. Surface is the supported
  material domain; Lit/Unlit are shading models, not other domains. Do not add
  nonfunctional Post Process, UI, Decal, Volume, or Light Function choices.
  Their renderer contracts belong to subsequent plans.

### Final values and shared surface evaluation

The root consumes final values, not separate Factor/Sample pairs. It has no
implicit texture lookup or hidden parameter lookup. Normal accepts a decoded
tangent-space direction; normal texture decoding remains inside SampleNormal or
an explicit equivalent function, so raw normal-map channels are not decoded twice.

Create one Engine-owned surface evaluation contract used by manual and imported
materials, previews, forward/deferred paths, masked/depth/shadow passes, and Cook.
Put renderer-required numerical handling at that boundary, rather than emitting
Saturate/Clamp nodes only in imported graphs. Do not implement this as a magic
function asset GUID or duplicate the policy in editor recipes and shaders.

Stage 0 must distinguish genuine renderer requirements from historical recipe
choices. In particular, freezing the roughness floor, HDR Base Color/Emissive
handling, opacity ranges, normal normalization, and nonfinite-value behavior is
a prerequisite to moving operations. A blanket clamp of every input is not an
acceptable implementation. Imported factor multiplication, emissive composition,
and artist-selected normal blending are authored operations, not universal output
rules. Preserve or explicitly account for their semantics during reconstruction.

### Default, new, and imported materials

| Entry point | Required authored graph |
| --- | --- |
| DefaultMaterial | One Surface root using the agreed fallback values; no required expression nodes or parameter owners |
| New material | The same root and initial values as DefaultMaterial; independent subsequent edits |
| Imported material | The same root with only source-required texture, parameter, and composition branches |

Use these generation rules:

- A texture with identity UVs and identity factor uses one combined sample owner
  connected to the matching property. Do not add Constant, UVChannel, Swizzle,
  Multiply-by-one, or renderer-only clamp nodes around it.
- A property without a texture uses its root literal. Expose an owner when that
  value must vary across instances; do not create absent texture owners or white
  texture samples merely to preserve a universal schema.
- UV0 and identity scale/offset/rotation stay local to the sample. Add an explicit
  UV expression or exposed UV parameters only when the source requires them or
  the user requests that editability. Distinct UV transforms must stay distinct.
- A nonidentity imported factor gets the necessary authored composition. Preserve
  source color/alpha meaning, texture color space, and existing import-time baking;
  never apply a baked factor a second time.
- Normal sampling and decode stay encapsulated. Default flat-normal behavior must
  not require a dummy texture branch or an unnecessary normal blend.
- Packed channels share one fetch only when resource, coordinates, and sampling
  semantics match. Use direct channel outputs rather than mandatory swizzles.
- Optional future instance edits may require explicitly adding a parameter or
  choosing another parent shape; they must not force every import to expose all
  48 historical parameters.

### Imported parent selection and parameter identity

Replace the compulsory universal parent with reusable parents keyed by structural
features. The key includes texture presence, channel packing, required factor/UV
operations, and other inputs that actually change graph structure. Exclude texture
asset paths and ordinary parameter values so equivalent material structures can
share a parent. Freeze key fields, recipe versioning, deterministic asset naming,
and provenance before implementation; do not key on arbitrary serialized memory.

Imported outputs remain instances. Each selected parent exposes exactly the
parameters its instances require, with graph-owned stable IDs. Scene import must
publish only overrides declared by that parent. Texture absence must not become
an accidental black/white fallback sample or a dangling override.

Reimport must deliberately handle structural transitions such as adding/removing
a normal map, changing UV requirements, or switching packing. Reuse parameter IDs
for retained logical roles, preserve applicable overrides, and diagnose overrides
whose roles disappear. Parent selection and instance updates must participate in
the existing staged import transaction, including rollback and retry.

### Scope and compatibility

This is more than renaming Material Output or hiding rows. It changes the surface
evaluation boundary and removes unnecessary import graph structure. It does not
implement new material domains, new BRDFs, or a general shader language.

Retain ordinary reusable material functions and explicit advanced graph operations.
Do not delete customized materials/functions or silently rewrite open documents.
Inventory exact affected assets and inbound references, back up authored bytes,
and reconstruct generated content deliberately. Retire unused shipped helpers only
after proving they have no retained consumers. Do not add a permanent historical
recipe converter. Version semantic compiler/Cook identities if evaluation changes;
change storage versions only when the serialized contract actually changes.

## Implementation Stages

### Stage 0: Freeze evaluation rules and import structures

- [ ] Audit current manual, default, and imported evaluation, including actual
  shader-side clamping and the operations emitted by ComposeSurfaceValue.
- [ ] Record an input policy table covering defaults, units/space, valid ranges,
  nonfinite handling, Lit/Unlit relevance, and each affected render pass.
- [ ] Classify every current import operation as source-authored behavior,
  renderer policy, or removable identity work; identify any intended visual changes.
- [ ] Freeze structural parent keys, generated asset provenance, instance exposure
  rules, reimport transitions, and the treatment of the existing ImportedSurface path.
- [ ] Inventory affected assets and all source/test consumers across projects in
  Durin.dworkspace; capture reference, image, and override baselines.

Gate: no unresolved semantic or parent-ownership decision remains before coding.

### Stage 1: Implement the shared Surface evaluation contract

- [ ] Centralize final-property validation/evaluation in Engine and connect every
  affected shader/pass path, including aggregate function outputs and cooked runtime.
- [ ] Remove import-only implementations of renderer policy without removing real
  factor/normal/UV authoring operations.
- [ ] Update compiler identity and derived-data invalidation where necessary.
- [ ] Test equivalent manual/imported inputs and boundary values independently of
  the previous graph layout, including defaults, HDR, masked alpha, and normals.

Gate: equal inputs produce equal surface behavior across creation paths and passes;
no duplicate sampling, decoding, or output clamping is introduced.

### Stage 2: Unify root authoring and default creation

- [ ] Present one Surface root with material identity/settings, supported property
  inputs, and appropriate Lit/Unlit and blend-mode feedback.
- [ ] Make DefaultMaterial and new-material creation use the same default contract.
- [ ] Preserve aggregate/per-property switching, fallback restoration, validation,
  selection, layout, diagnostics, Apply/Discard, and atomic Undo/Redo.
- [ ] Keep unused controls compact while retaining connected/editable data; never
  silently discard bindings when a property becomes inactive.
- [ ] Capture and inspect actual editor output for empty/default materials, a
  direct texture connection, a normal function, and narrow-window editing.

Gate: no authoring path requires two visible output nodes or Factor/Sample pairs.

### Stage 3: Generate minimal imported parents and instances

- [ ] Implement deterministic structural parent creation/reuse and staged dependency
  publication in AssetForgeBuiltins and SceneDirectImport.
- [ ] Generate only required sample, value, factor, normal, and UV branches; preserve
  texture usage, samplers, packed channels, and source alpha behavior.
- [ ] Replace unconditional fixed-schema override writes with selected-parent-aware
  publication, retaining logical parameter identity where roles survive.
- [ ] Cover reimport, deduplication, missing resources, failed publication, rollback,
  and structural parent changes without orphaning unrelated instance edits.
- [ ] Assert representative graph budgets: no expressions for an unparameterized
  default, one sample for a plain color texture, one fetch for compatible packed
  channels, and no expanded default UV or universal eight-map scaffold.

Gate: a plain imported texture is no more complex than its manual equivalent;
materials with different required structures do not share an incompatible parent.

### Stage 4: Reconstruct generated content and verify rendering

- [ ] Back up and reconstruct only the inventoried generated parents/instances;
  preserve custom assets and verify retained parameter values and references.
- [ ] Rebuild DefaultMaterial and generated import content from the new contracts.
- [ ] Verify source load/save, duplication, reimport, graph-stripped Cook/load, and
  affected project content inventories; regenerate affected cooked outputs.
- [ ] Compare actual preview and rendered results for plain color, normal, packed
  ORM, nonidentity factors/UVs, emissive, masked, translucent, and Unlit cases.
- [ ] Include a previously nontrivial imported material and a simple material in
  both visual and graph-complexity acceptance; document approved semantic differences.

Gate: generated assets are usable, expected appearance is verified, and graph
simplification has not moved costs into redundant hidden work.

### Stage 5: Complete integration and documentation

- [ ] Run registry-selected affected native targets and relevant GPU coverage.
- [ ] Complete workspace all builds after shared Engine API changes, including
  affected Editor/Game targets and project consumers.
- [ ] Update the authoritative runtime, graph-authoring, and regeneration guidance.
- [ ] Record exact validation receipts, final graph examples, reconstruction results,
  and remaining material-domain limitations; complete and commit the plan.

Gate: every preceding gate is satisfied, documentation matches implementation, and
the user's default/new/imported material workflows use the unified Surface model.

## Execution References

- [Material system](../Runtime/Rendering/MaterialSystem.md)
- [Material graph operations](../Editor/Architecture/MaterialGraphOperations.md)
- [Asset reconstruction](../Editor/Guides/CanonicalResave.md)
- [Build and run guidance](../Agents/BuildAndRun.md)
- [Native test selection](../Agents/Testing.md)
- [Documentation workflow](../Agents/Documentation.md)

Primary implementation entry points are Engine's MaterialProgramTypes,
MaterialProgramValidation, MaterialProgramCompiler and MaterialProgramGenerator;
MaterialEditor's MaterialGraphCanvas, MaterialGraphOperations and
MaterialAssetCreation; and AssetForgeBuiltins' StandardMaterialFunctions,
ImportedSurfaceMaterial and SceneDirectImport. Confirm ownership and actual
downstream shader consumers in Stage 0 rather than treating this list as exhaustive.
