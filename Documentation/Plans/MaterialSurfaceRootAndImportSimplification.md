# Material Surface Root And Import Simplification Plan

Summary: Unify material authoring around one Surface root, centralize surface evaluation rules, and generate imported graphs only for features present in the source material.

Last reviewed: 2026-09-14

Status: Active
Completed:

## Current Status

Stage 0 source and mounted-content audits are complete at execution baseline
`2d3eef2fa1af59da8d7a86c428398bd28fc0765b`. Implementation has not started.
The frozen decisions and validation receipts below govern subsequent stages.

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

- [x] Audit current manual, default, and imported evaluation, including actual
  shader-side clamping and the operations emitted by ComposeSurfaceValue.
- [x] Record an input policy table covering defaults, units/space, valid ranges,
  nonfinite handling, Lit/Unlit relevance, and each affected render pass.
- [x] Classify every current import operation as source-authored behavior,
  renderer policy, or removable identity work; identify any intended visual changes.
- [x] Freeze structural parent keys, generated asset provenance, instance exposure
  rules, reimport transitions, and the treatment of the existing ImportedSurface path.
- [x] Inventory affected assets and all source/test consumers across projects in
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

## Stage 0 Decisions And Evidence

### Evaluation audit and selected boundary

`MaterialProgramGenerator.cpp` returns property values or the aggregate directly.
Forward, thumbnail, GBuffer and masked-shadow fragments all call that generated
function; opaque shadow needs no material evaluation. Cook stores these compiled
stages. `EvaluateMaterialSurface` in `SurfaceMaterial.slang` has no callers in
workspace source/shaders: its old factor/sample API is not the runtime boundary.
`FilterSpecularRoughnessFromVariance` currently clamps roughness to 0.045..1 and
recovers nonfinite roughness to 0.5; direct BRDF evaluation also clamps roughness.
GBuffer base color is RGBA8_UNORM, and emissive is R11G11B10_FLOAT. Thus preserving
unbounded Base Color only in forward rendering would violate pass parity.

The Engine generator will apply one final-value evaluator to either root mode.
Reusable Surface aggregates remain raw composable data until they reach this
boundary. All authored literals/parameter defaults continue to reject nonfinite
values; the runtime rules below additionally handle expression overflow and NaN.
No texture fetch, normal decode, vertex-color multiply or parameter lookup occurs
at this boundary. The table describes evaluated values, not destructive edits to
the retained authored inputs.

| Input | Root default | Units/space and evaluated range | Nonfinite expression handling | Pass relevance |
| --- | --- | --- | --- | --- |
| Base Color | (0.5,0.5,0.5) | Linear RGB reflectance, saturate to 0..1 | Replace each nonfinite component with 0.5 | Lit forward/GBuffer; Unlit forward |
| Normal | (0,0,1) | Decoded tangent direction; safe normalization; squared length threshold 1e-8 | Invalid vector or nonfinite squared length becomes flat normal | Lit forward/GBuffer, then world frame |
| Metallic | 0 | Unitless, saturate to 0..1 | 0 | Lit forward/GBuffer |
| Roughness | 0.5 | Perceptual roughness, 0.045..1 | 0.5 | Lit forward/GBuffer before derivative AA |
| Ambient Occlusion | 1 | Unitless visibility, saturate to 0..1 | 1 | Lit environment lighting/GBuffer |
| Emissive | (0,0,0) | Nonnegative scene-linear RGB; HDR retained up to (65024,65024,64512), the finite GBuffer channel maxima | Replace each nonfinite component with 0 | Lit and Unlit forward/GBuffer |
| Opacity | 1 | Straight alpha, saturate to 0..1 | 1 | Translucent forward; retained for other modes |
| Opacity Mask | 1 | Coverage, saturate to 0..1; reject strictly below cutoff | 1 | Masked forward, GBuffer/depth, shadow; Lit and Unlit |

Base Color outside 0..1, negative/overflow emissive, invalid normals, and
out-of-range scalar outputs will intentionally become consistent across passes
and authoring paths. HDR emission belongs in Emissive, including Unlit materials.
Keep the BRDF and specular-AA numerical safeguards: those routines have consumers
outside the graph boundary and AA must still bound its newly computed roughness.
Version the compiler envelope; do not change graph or package storage versions
solely for evaluation semantics.

### Import operation classification

- Preserve source-authored Base Color/alpha, metallic and roughness multiplication.
  Current `ComposeSurfaceValue` saturates factors before multiplication, and
  scalar samples before multiplication; for validated source values in 0..1 these
  are identity work. New generated recipes omit them and use final-value policy.
  Customized reusable functions retain their authored pre-multiply behavior.
- Roughness-floor Clamp and its two constants are renderer policy, moved to the
  boundary. Default factor owners, multiply-by-one, flat-normal RNM, zero-emissive
  Add, absent-map samples and identity UV owner scaffolds are removable work.
- Normal decode, nonidentity normal strength and explicit artist normal blending
  are authored operations. Preserve import-time normal-strength baking and decode
  once. Do not multiply the baked strength again.
- Existing emissive composition is nonnegative factor plus nonnegative sample.
  Import already bakes source emissive multiplication into ScaledColor textures;
  its graph factor is zero. Preserve the baked texture directly; a textureless
  source emissive factor must become an actual final value (currently omitted by
  SceneDirectImport). Do not globally turn artist-authored Add into Multiply.
- Source UV channel/scale/offset/rotation and complete sampler state must survive.
  SceneImport parses them, but SceneDirectImport currently publishes only texture
  object pointers. Restoring source sampling is an intentional correctness change.
- Current import derives separate metallic B, roughness G, occlusion R and alpha
  images. Compatible data channels will instead share one resource/sample with
  direct output slots. Never merge color-sRGB and linear-data resources merely
  because the source image matches. Preserve derivation/baking where required.
- Masked source alpha factor must bind Opacity Mask; the current importer writes
  only Opacity, leaving mask factor at one. Correct that mapping explicitly.

### Structural parent and publication contract

Use recipe version 1 with a fixed ordered, textual encoding prefixed
`Durin.ImportedSurface:1`. For the eight roles in surface-output enum order,
encode: default/value/sample mode; optional nonidentity factor operation;
sample-group index and direct channel slot; decode mode; and four UV exposure
bits (channel, scale, offset, rotation). Group indices are assigned by first role
occurrence. Resource grouping requires equal source resource derivation, color
space, sampler and coordinate semantics. Encode the resulting equality groups,
not source image identities, paths, floating-point values or serialized memory.
Static blend/cutoff/two-sided overrides remain instance properties and enter the
key only if they change generated property branches, such as alpha consumption.

Use a stable hash of this encoding in
`/Game/Materials/ImportedParents/Surface_v1_<digest>`; shared Engine content is not
an import destination. Persist recipe identifier, version and full canonical key
as editor-only generated provenance. Reuse requires matching provenance and exact
expected graph/schema; a modified/colliding parent produces a diagnostic instead
of being overwritten. Presentation edits are not structural conflicts. Additive
provenance fields are storage changes and must be tested through save/load.

Default-valued absent properties stay root literals. Nondefault textureless source
values expose numeric owners so equivalent structures with different values share
a parent. Identity factors on present textures expose no numeric owner; other
factors do. Identity UV fields remain local; nonidentity fields expose only their
required owners so ordinary numeric transform values remain outside the key.
Each sample group has one texture owner, using the first participating role's
existing registry GUID. Retained numeric/UV roles use existing registry GUIDs;
graph node identity must remain distinct from parameter identity. Overrides are
published against this explicit role-to-owner mapping, never the old 48-field set.

Adding/removing maps, identity transitions, UV requirements and packing changes
select a new parent. Reimport must reconcile by stable source output identity,
preserve surviving user overrides, retain disappearing user roles as diagnosed
orphans, and remove only obsolete importer-owned overrides. Persist the last
imported role/value snapshot to distinguish user edits from importer values.
For shared-to-split texture groups, use the saved logical-role mapping to transfer
applicable overrides; never infer equivalence from display names alone.

The current `ImportSceneAssets` explicitly rejects existing outputs and provides
no scene reimport transaction. Stage 3 therefore includes extending staged
publication/reconciliation, not calling the create-only entry point as reimport.
New parents are private candidates, compiled before publication and saved with
instances/textures/meshes in the same atomic bundle. Reused parents are read-only
dependencies. Rollback must discard newly created parents and restore every
replaced output and registry entry; retries must not mutate unrelated imports.

The historical `/Engine/Materials/ImportedSurface` is no longer selected by new
imports after Stage 3. Preserve customized content; reconstruct/retire only the
exact shipped recipe after reference audit. It is not a permanent converter or
an alias for every structural parent. Ordinary standard functions remain reusable;
retirement of unused shipped helpers requires the Stage 4 reference check.

### Inventory and baseline receipts

Both `asset material-functions` and `asset identity-audit` succeeded for
`Sandbox/Sandbox.dproject` and `RoadWeaver/RoadWeaver.dproject`. Their shared Engine
mount covers `Engine/Engine.dproject`; Engine alone is not an asset-tool project.
The combined mounted inventory contains 21 physical packages, two materials,
seven functions and zero material instances. DefaultMaterial has zero nodes and
owners; ImportedSurface has 82 nodes, one call and 48 owners. Both audits report
the exact current recipe/dependencies. There are no saved instance overrides to
reconstruct in the checked-in project content.

Exact material assets are `Engine/Content/Materials/DefaultMaterial.dasset`,
`ImportedSurface.dasset`, and `Functions/{UVTransform,SampleNormal,SampleORM,
StandardPBR,StandardPBR_ORM,ImportedSurfaceValues,DecodeImportedNormalRG}.dasset`.
The complete material inbound-edge inventory is:

- StandardPBR -> SampleNormal.
- StandardPBR_ORM -> SampleNormal and SampleORM.
- ImportedSurface -> DecodeImportedNormalRG.
- No other mounted package references these material/function assets.

Source/test searches covered Engine, Sandbox and RoadWeaver roots. Direct recipe
consumers are AssetForgeBuiltins (StandardMaterialFunctions, ImportedSurfaceMaterial,
SceneImport, SceneDirectImport), LevelEditor SceneImportDialog, DurinAssetTool,
StandardMaterialFunctionTestFixture, MaterialFunctionTests, SceneImportTests,
SceneImportVulkanTests, MaterialVulkanTests, MaterialGraphOperationsTests and
MaterialThumbnailRendererTests. Engine compiler/types/validation/generator and
MaterialEditor graph/creation own the shared contracts. Renderer SurfaceMaterial,
StaticMeshRenderer, GBufferRenderer, the generated fragments and deferred lighting
are downstream evaluation consumers. No additional project-owned recipe API
consumers were found. Historical GraphAuthoringV5 rejection fixtures are retained.

Local pre-change evidence is under `Documentation/Local/MaterialSurfaceBaseline`:
two material inventories, two full reference reports, byte copies of all nine
material/function assets, `asset-hashes.json` (SHA-256 and byte counts), and
22 PNG captures in `Images`. These ignored working receipts must remain available
until reconstruction and visual comparison finish; the Git baseline also retains
the authored bytes. Inspected default and independent-map captures show the
expected gray and red test geometry, respectively.

`DevTool.bat test MaterialVulkanTests` passed 1/1 at the baseline. The retained
capture run with `DURIN_TEST_KEEP_WORK=1` also passed 1/1 in 24.693 seconds;
receipt `Build/.agent-state/logs/20260914-031928-372220-31912-MaterialVulkanTests.log`.
These are baseline tests, not implementation acceptance or editor screenshots.

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
