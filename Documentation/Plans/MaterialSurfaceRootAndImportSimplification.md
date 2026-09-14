# Material Surface Root And Import Simplification Plan

Summary: Unify material authoring around one Surface root, centralize surface evaluation rules, and generate imported graphs only for features present in the source material.

Last reviewed: 2026-09-14

Status: Completed
Completed: 2026-09-14

## Current Status

Stage 0 source and mounted-content audits are complete at execution baseline
`2d3eef2fa1af59da8d7a86c428398bd28fc0765b`. Stages 1 through 3 implementation and
validation are complete. Manual and imported normals now use one combined sample
owner with an explicit decoded Normal output. Source-authoritative reimport
replaces generated edits and retains ownership checks, rollback and live references.
Stages 4 and 5 are complete: fresh project inventories, generated-parent roundtrips
and Cook/load, independent render comparisons, inspected canvas/GPU captures,
registry-selected tests, workspace all build and documentation validation pass.
The final acceptance receipt below records reused tests and unavailable historical
screenshots explicitly.
The frozen decisions and validation receipts below govern subsequent stages.
Stage 1 implements final-value evaluation and compiler invalidation. Its generated
parent's redundant policy and exact recipe checkpoint changed together: the one
already-backed-up parent was reconstructed during Stage 1, ahead of the broader
Stage 4 reconstruction. No mounted assets reference it and no saved instance
overrides exist. Ordinary reusable function graphs remain unchanged. This ordering
avoids leaving the installed parent incompatible with the recipe validator between
stages. The historical ImportedSurface has 65 expression nodes and 48 owners.
New imports no longer select it or bootstrap it when opening the import dialog.

The historical pre-execution baseline is commit `88aaedfb1`, following `42df4b322` and
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

Reimport handles structural transitions by selecting the source-required parent
and replacing the complete generated instance parameter and static-setting state.
It does not preserve edits to generated meshes, textures or material instances,
retain orphan overrides, or compare against previous imported values. Stable
source/output identity authorizes replacement; matching paths alone do not.
Keep custom copies outside the import output directory. Shared generated parents
remain immutable dependencies and are never overwritten to change one instance.
Parent selection and output replacement use the staged import transaction, with
rollback on failure and reference refresh on success. Outputs no longer present
in the source are retained rather than automatically deleting referenced assets.

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

- [x] Centralize final-property validation/evaluation in Engine and connect every
  affected shader/pass path, including aggregate function outputs and cooked runtime.
- [x] Remove import-only implementations of renderer policy without removing real
  factor/normal/UV authoring operations.
- [x] Update compiler identity and derived-data invalidation where necessary.
- [x] Test equivalent manual/imported inputs and boundary values independently of
  the previous graph layout, including defaults, HDR, masked alpha, and normals.

Gate: equal inputs produce equal surface behavior across creation paths and passes;
no duplicate sampling, decoding, or output clamping is introduced.

### Stage 2: Unify root authoring and default creation

- [x] Present one Surface root with material identity/settings, supported property
  inputs, and appropriate Lit/Unlit and blend-mode feedback.
- [x] Make DefaultMaterial and new-material creation use the same default contract.
- [x] Preserve aggregate/per-property switching, fallback restoration, validation,
  selection, layout, diagnostics, Apply/Discard, and atomic Undo/Redo.
- [x] Keep unused controls compact while retaining connected/editable data; never
  silently discard bindings when a property becomes inactive.
- [x] Capture and inspect actual editor output for empty/default materials, a
  direct texture connection, a normal function, and narrow-window editing.

Gate: no authoring path requires two visible output nodes or Factor/Sample pairs.

### Stage 3: Generate minimal imported parents and instances

- [x] Implement deterministic structural parent creation/reuse and staged dependency
  publication in AssetForgeBuiltins and SceneDirectImport.
- [x] Generate only required sample, value, factor, normal, and UV branches; preserve
  texture usage, samplers, packed channels, and source alpha behavior.
- [x] Replace unconditional fixed-schema override writes with selected-parent-aware
  publication, retaining logical parameter identity where roles survive.
- [x] Cover reimport, deduplication, missing resources, failed publication, rollback,
  and structural parent changes that reset generated edits while preserving
  unrelated assets and references.
- [x] Assert representative graph budgets: no expressions for an unparameterized
  default, one sample for a plain color texture, one fetch for compatible packed
  channels, and no expanded default UV or universal eight-map scaffold.

Gate: a plain imported texture is no more complex than its manual equivalent;
materials with different required structures do not share an incompatible parent.

### Stage 4: Reconstruct generated content and verify rendering

- [x] Back up and reconstruct only the inventoried generated parents/instances;
  preserve custom assets and verify retained parameter values and references.
- [x] Rebuild DefaultMaterial and generated import content from the new contracts.
- [x] Verify source load/save, duplication, reimport, graph-stripped Cook/load, and
  affected project content inventories; regenerate affected cooked outputs.
- [x] Compare actual preview and rendered results for plain color, normal, packed
  ORM, nonidentity factors/UVs, emissive, masked, translucent, and Unlit cases.
- [x] Include a previously nontrivial imported material and a simple material in
  both visual and graph-complexity acceptance; document approved semantic differences.

Gate: generated assets are usable, expected appearance is verified, and graph
simplification has not moved costs into redundant hidden work.

### Stage 5: Complete integration and documentation

- [x] Run registry-selected affected native targets and relevant GPU coverage.
- [x] Complete workspace all builds after shared Engine API changes, including
  affected Editor/Game targets and project consumers.
- [x] Update the authoritative runtime, graph-authoring, and regeneration guidance.
- [x] Record exact validation receipts, final graph examples, reconstruction results,
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
select a new parent. Reimport matches stable source/output identities and replaces
all generated output state from the source. No previous parameter/value snapshot,
user-edit merge, orphan preservation or split/merge reconciliation is required.
This is the user-selected simplification of 2026-09-14. The prior receipt-based
merge design is superseded, including its historical implementation checkpoints.

New parents and replacement outputs remain private candidates, compiled before
publication and saved in the same atomic bundle. Reused parents are read-only
dependencies. Rollback discards candidates and restores replaced authored files
and registry entries. Unrelated outputs are never overwritten; copies for custom
authoring should live outside the scene output directory. Removed source outputs
remain available to retained references; automatic cleanup is outside this stage.

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

## Stage 1 Receipt

The final evaluator is implemented in `Material/SurfaceMaterial.slang` and called
from both root modes in `MaterialProgramGenerator.cpp`. Compiler envelope 9
invalidates prior compiled/Cook identities; serialized program schemas are
unchanged. Removed the unused factor/sample shader API after its consumer audit.
The generated import recipe now contains 65 nodes, with no Saturate/Clamp nodes;
normal decoding/blending, factor multiplication, emissive addition and independent
UV owners remain authored operations. Existing ordinary function assets retain
their exact bytes and implementations.

Reconstructed only `Engine/Content/Materials/ImportedSurface.dasset`, after checking
its SHA-256 against the Stage 0 backup. The replacement is 56,753 bytes, SHA-256
`FBE52CA9E8ADE0E8230ED1BBAD1A51CABED4BD4F33BB1DD6B3371D238366EA1C`.
DefaultMaterial and all seven function assets remain unchanged. The 48 retained
parameter GUIDs and compiled layouts are unchanged; expression identities differ
because redundant numerical operations were removed. Structural parent selection,
instance reconciliation, compact UI and broader Cook regeneration remain pending.

Validation on Win64-Debug-DurinEditor:

- `MaterialTests`: 194/194 passed before recipe reconstruction. Final registry set
  `@domain=material+asset-import,kind=feature+integration`: 6/6 targets passed after
  reconstruction (AssetImportTests, MaterialTests, MaterialThumbnailTests,
  MaterialVulkanTests, SceneImportTests, SceneImportVulkanTests), 98.18 seconds.
  Receipt: `Build/.agent-state/logs/20260914-033400-092323-36788-ctest.log`.
- New production thumbnail coverage compares raw/expected final values in both
  root modes across Lit/Unlit and all three blend modes, distinguishes HDR
  Emissive 2 from 1, and verifies runtime Infinity/NaN recovery from finite
  parameter operands. Existing old-expanded/imported recipe image parity,
  independent map/UV overrides, normal and missing-resource coverage passed.
- `GBufferQualificationTests --mode qualification`: passed after shared evaluator
  changes. Receipt: `Build/.agent-state/logs/20260914-033156-335442-40112-ctest.log`.
  Use this for correctness coverage; timing is diagnostic because an exclusive
  external-application-free GPU lane was not established.
- Final workspace `all` build passed, receipt
  `Build/.agent-state/logs/20260914-033602-036463-33688-cmake.log`.
- Local reconstruction and follow-up mounted recipe inventories are retained under
  `Documentation/Local/MaterialSurfaceBaseline/stage1-*.txt`.

## Stage 2 Receipt

The existing final anchor is displayed as Surface with material identity and
current shading/blend modes. Selecting it or clearing node selection exposes
Surface Settings in Details. The supported domain is Surface only; Lit/Unlit,
blend, masked cutoff, two-sided state and depth-write policy remain distinct.
These controls submit through the existing reflected-property transaction boundary
on the working document. Inactive cutoff data and all inactive graph bindings are
retained. Only inactive input label styling changes; values remain editable.
Terminal identity, compiler reachability and aggregate/per-property exclusion are
unchanged. No serialized graph version or duplicate output node was introduced.

DefaultMaterial and fresh material creation already use the same Engine default
program. Existing creation, default, aggregate, fallback, transaction and session
tests were retained and passed. Source/default material assets were not rewritten
for the presentation change.

Actual ImGui draw-data captures (using the existing headless rasterizer) were
inspected for empty Surface, direct Base Color sample, SampleNormal call, and a
900-pixel-wide Unlit/Masked window with settings and retained normal connection.
They are retained at `Documentation/Local/MaterialSurfaceBaseline/Stage2Images/`
as `surface-empty.png`, `surface-texture.png`, `surface-normal.png` and
`surface-narrow-unlit.png`. This is rendered editor canvas evidence, not a native
desktop-window capture. The new fixture also asserts that switching shading/blend
properties leaves the complete bound program unchanged.

Validation on Win64-Debug-DurinEditor:

- `MaterialTests`: 194/194 passed in 105.694 seconds, receipt
  `Build/.agent-state/logs/20260914-034433-685517-32600-MaterialTests.log`.
- Final canvas hover gating change: `FMaterialGraphOperationsTests.*`, 46/46
  passed in 25.054 seconds, receipt
  `Build/.agent-state/logs/20260914-034638-552567-33132-MaterialTests.log`.
- Final workspace `all` build passed, receipt
  `Build/.agent-state/logs/20260914-034704-129488-14268-cmake.log`.

## Stage 3 In-Progress Receipt

The following checkpoints are historical. Their user-override reconciliation
requirements are superseded by the source-authoritative contract above.

The independent `ImportedSurfaceRecipe` builder now generates deterministic
programs and logical-role owner mappings. Root defaults require no owners;
identity texture branches use one combined sample; nonidentity values and UV
fields expose only required owners. Equal resource/usage/sampler/coordinate
groups share direct channel outputs. Resource identities, sampler values and
numeric values do not enter the structural encoding. Normal decoding currently
uses the explicit Engine DecodeNormalRG operation without a flat-normal blend;
final encapsulation and import integration remain pending.

The focused `StructuralImportRecipesExposeOnlyRequiredOwners` test passes,
including numeric-value independence, packed/split UV groups, and normal-map
addition/removal. Receipt:
`Build/.agent-state/logs/20260914-035901-860888-34032-MaterialTests.log`.
The workspace `all` build passes:
`Build/.agent-state/logs/20260914-035933-692296-27352-cmake.log`.
This is a generator checkpoint only: new imports still select the historical
parent. Parent provenance/persistence, actual source mapping, reimport ownership
reconciliation, transactional publication and their acceptance tests remain open.
No Stage 3 checklist item or gate is marked complete by this checkpoint.

The next checkpoint adds editor-only `FMaterialImportProvenance` to material
interfaces, including recipe/key, stable source/output identity and last imported
logical-role/owner/value records. This is a reconciliation receipt, not an
authorable parameter schema. Saving/loading preserves it without changing graph
or compile revisions. The setter bounds receipt sizes and rejects duplicate roles.
The independent reconciliation routine transfers user edits across owner splits,
rejects conflicting merges, retains disappearing user roles as diagnosed orphans,
and removes obsolete importer-owned overrides. Actual scene publication does not
yet invoke these routines.

`MaterialTests` passes 197/197 in 100.670 seconds, including receipt roundtrip and
split/merge/removal tests:
`Build/.agent-state/logs/20260914-040407-007743-31464-MaterialTests.log`.
The shared Engine API checkpoint passes the workspace `all` build:
`Build/.agent-state/logs/20260914-040551-730606-38612-cmake.log`.

Publication inspection found an additional integration requirement:
`SavePackagesAtomically` rejects graph-private packages and, on Registry failure,
commits authored files with `ContentCommittedProjectionPending` instead of rolling
them back. The scene transaction must add a coordinated persistence/live-graph
publication boundary; merely invoking this existing save API cannot satisfy the
frozen rollback gate. Its current behavior for unrelated callers must be preserved.

The actual scene import path now selects/reuses deterministic parents under
`/<destination mount>/Materials/ImportedParents/Surface_v1_<digest>`, matching
provenance, exact program and static settings while allowing presentation edits.
New parents are compiled with their instance variants and included in the same
save bundle. Reused parents are not saved or authored. The editor dialog no longer
initializes the historical Engine parent. Instances write only selected graph
owners and persist the source identity and logical-role receipt.

Metallic/Roughness retain one linear packed image; compatible occlusion shares
its resource and direct channel output. Resource derivation/usage now determine
texture output deduplication instead of the semantic label. Source UV and sampler
values are published. Mask factor is applied to Mask rather than inactive Opacity;
textureless Emissive retains its source value. Normal strength and emissive baking
are not applied twice. Nonidentity occlusion strength, previously ignored, now
bakes `1 + strength * (occlusion - 1)` into its own derived linear texture. This
source-semantics correction intentionally prevents sharing that derived sample.

Validation receipts for actual import integration:

- `SceneImportTests`: 9/9 passed in 26.803 seconds,
  `Build/.agent-state/logs/20260914-041733-137524-35336-SceneImportTests.log`.
  Includes real packed ORM, parent reuse/mutation rejection, selected owner counts,
  UV transforms, mask factor and textureless emissive. Final nondefault sampler
  assertions pass in the focused source-values test,
  `Build/.agent-state/logs/20260914-041822-693292-35896-SceneImportTests.log`.
- `SceneImportVulkanTests`: passed in 5.813 seconds,
  `Build/.agent-state/logs/20260914-041635-372729-26832-SceneImportVulkanTests.log`.
  The test now unloads/reloads the actual generated parent, exercises subsequent
  instance edits and retains independent render controls. Reported compiled costs:
  simple texture/factor uses 1 resource, 3 uniform fields, 64 uniform bytes;
  nontrivial source uses 6 resources, 13 uniform fields, 224 uniform bytes.
- Workspace `all` passes,
  `Build/.agent-state/logs/20260914-041828-241904-38268-cmake.log`.

The next transaction checkpoint replaces registered creation with graph-private
packages. Core object-graph replacement now accepts null-current additions,
reserves their paths invisibly and publishes additions/replacements together.
Its optional synchronous persistence callback runs after final validation and
before the non-failing in-memory commit. Callback failure leaves the operation
prepared for retry or abort. Save admission accepts private packages only with
an owning active publication token and mandatory Registry-failure rollback.
Ordinary saves retain their existing projection-pending semantics.

Scene import compiles private parents/instances explicitly, saves the complete
candidate closure inside that callback, and registers it only after success.
Failure discards the candidates; late cancellation is checked before persistence.
Directory/staging/package/root/Registry failure injection verifies each requested
failure was reached, every candidate remained invisible throughout save, cleanup
left no output registration, and retry succeeded. The retry's plain texture graph
has exactly one sample node and one owner. This fixture uses the agreed Engine
defaults for the other properties, rather than silently dropping glTF factors.

Final transaction checkpoint receipts:

- `CoreObjectReplacementTests`: 20/20, including mixed additions/replacements,
  failed persistence/retry and abort releasing reservations;
  `Build/.agent-state/logs/20260914-042625-748477-32112-CoreObjectReplacementTests.log`.
- `SceneImportTests`: 10/10 in 27.673 seconds;
  `Build/.agent-state/logs/20260914-043119-441337-30184-SceneImportTests.log`.
  Final private-save admission guard: focused rollback/retry test passed,
  `Build/.agent-state/logs/20260914-043431-207376-40516-SceneImportTests.log`.
- `AssetPackageTests`: 149/149 in 22.439 seconds, covering old/new main and external
  bulk restoration and ordinary-save compatibility;
  `Build/.agent-state/logs/20260914-043258-353391-39396-AssetPackageTests.log`.
- `AssetPackageReloadTests`: 13/13;
  `Build/.agent-state/logs/20260914-043155-715935-23952-AssetPackageReloadTests.log`.
- `SceneImportVulkanTests`: passed in 5.919 seconds after private publication;
  `Build/.agent-state/logs/20260914-043211-239587-38288-SceneImportVulkanTests.log`.
- Final workspace `all` passes;
  `Build/.agent-state/logs/20260914-043438-700952-38948-cmake.log`.

Stage 3 remains open: existing scene outputs are still rejected and the
reconciliation routine is not yet invoked by reimport. Same-source admission,
stable-output replacement, native consumer refresh/retirement, reimport rollback,
normal encapsulation and final budget acceptance remain required. Stage 4 content
reconstruction and final visual/Cook acceptance remain separate obligations.

## Source-Authoritative Reimport Simplification Receipt

The user's 2026-09-14 scope revision is implemented. Reimport matches persisted
source/output identities in the destination and publishes newly built outputs in
the existing atomic save/replacement transaction. Material parameters and static
settings always come from the incoming source. Removed the imported-value history,
property baselines, override merge routine and orphan restoration API. Only recipe
and ownership identity remain persisted. Texture derivations that change identity
receive distinct outputs; removed outputs remain available to existing references.

The dialog describes overwrite behavior and directs custom assets outside the
output directory. Unrelated source/path collisions fail before publication;
shared structural parents are reused only when their exact recipe matches.
Native mesh render/physics bindings refresh after replacement, and external
material-instance variants recompile against their newly bound parent.

The scene fixture unloads/reloads ownership metadata, changes parent shape by
adding a normal map and removing a color factor, resets manual parameter/shading
edits, restores source values when the shape returns, preserves an independent
material, and rejects another source with the same filename. Staging, package
publication and Registry failure injection preserve old saved bytes and live
mesh/material references; successful retry replaces them together.

Registry-selected material/import validation covers AssetImportTests,
MaterialTests, MaterialThumbnailTests, MaterialVulkanTests, SceneImportTests and
SceneImportVulkanTests. Initial receipt:
`Build/.agent-state/logs/20260914-113248-967080-14480-ctest.log`.
Five targets passed, including both Vulkan targets; the thumbnail target observed
its asynchronous cache-read state before the expected invalid-parent diagnostic.
The isolated rerun reproduced this asynchronous test assumption. The test now
pumps frames with a bounded deadline before asserting the same failure state and
diagnostic; production thumbnail behavior is unchanged. All 8 thumbnail tests
pass in 15.987 seconds:
`Build/.agent-state/logs/20260914-113916-182350-39840-MaterialThumbnailTests.log`.
The other five passing target results remain applicable to the final code.
Final workspace `all` build passes:
`Build/.agent-state/logs/20260914-113940-864414-40236-cmake.log`.
Changed-document validation passes for the plan and material-system contract.

Stage 3 remains open for normal-sampling encapsulation. Stage 4 reconstruction,
visual/Cook acceptance and Stage 5 integration/documentation remain separate gates.

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

## Normal Encapsulation And Final Acceptance Receipt

Sampling output index 8 is decoded tangent-space Normal. It lowers to the same RG
swizzle and DecodeNormalRG as the explicit branch and reuses the fetch. Both manual
Surface Add Texture and the structural importer use this output. No new opcode,
hidden function asset dependency, second fetch, flat-normal blend, or retained
value merge is introduced. Existing SampleNormal functions remain unchanged.
Normal structural keys change from `n` to `n2`; this selects a new immutable parent
while leaving unrelated parent shapes and existing assets intact. Existing serialized
channel/resource indices and compiler semantics retain their meanings.

The normalization parity test matches complete canonical bytes/layout against an
explicit decode, verifies one fetch/one decode/no RNM blend, and rejects an invalid
output index. Graph tests cover advanced-pin visibility, one-node normal budgets,
and Undo/Redo. The actual compact canvas capture was inspected: color and Normal
outputs connect directly to one Surface root without overlapping nodes.

The material/import registry selection in
`Build/.agent-state/logs/20260914-114941-418562-41528-ctest.log` passed five targets
and 197/198 MaterialTests cases. The remaining test expected the new disconnected
advanced pin to remain visible; corrected its assertion and added connected Normal
visibility coverage. Its focused rerun passes:
`Build/.agent-state/logs/20260914-115415-070713-7500-MaterialTests.log`.
No production change followed that selection. Additional generated-normal
save/reload, duplication, Cook cache-hit and graph-stripped runtime loading pass:
`Build/.agent-state/logs/20260914-115842-900607-32632-MaterialTests.log`.
The workspace all build passes:
`Build/.agent-state/logs/20260914-115432-834540-31016-cmake.log`.

Fresh Sandbox/RoadWeaver inventories contain 19/15 mounted packages, respectively,
with two shared materials, seven functions and no material instances. Inbound
material edges still consist only of StandardPBR/StandardPBR_ORM function calls and
ImportedSurface -> DecodeImportedNormalRG. DefaultMaterial remains a zero-node
root; ImportedSurface matches the exact 65-node current recipe. Stage 1 already
reconstructed that historical parent, so no further authored-file replacement is
needed. Scene fixtures generate and reload the new structural parents directly.

The historical ignored `Documentation/Local/MaterialSurfaceBaseline` directory is
absent in this checkout; its original screenshots cannot be treated as available
comparison evidence. Fresh authored-byte backups, SHA-256 inventories, identity
reports, Cook reports and the inspected canvas image are retained in
`Documentation/Local/MaterialSurfaceAcceptance`. All nine authored material/function
files remain unchanged. This does not recreate the missing historical captures;
independent render assertions and normalized explicit-decode parity provide the
semantic controls for current acceptance.

Both project Cook operations succeeded with full regeneration: Sandbox published
six packages (5,861,694 changed bytes); RoadWeaver published five packages
(2,401,881 changed bytes), explicitly including DefaultMaterial and ImportedSurface.
The generated-normal test separately verifies the new structural parent loads from
cooked data with one resource and no authored graph, function calls, IR or source.

Final retained GPU receipts:

- MaterialVulkanTests passes in 50.378 seconds:
  `Build/.agent-state/logs/20260914-115926-088518-30816-MaterialVulkanTests.log`.
  Exact current-run image comparisons cover reusable-function versus explicit
  graphs, neutral/independent/packed maps and UV/sampler variants. Pixel assertions
  cover normal/emissive response, masked cutoff and translucent alpha, Unlit,
  source reload and default rendering. Added retained images for those variants.
- SceneImportVulkanTests passes in 10.578 seconds after final budget assertions:
  `Build/.agent-state/logs/20260914-120238-153465-30800-SceneImportVulkanTests.log`.
  Actual reloaded imported simple and complex materials render successfully.
  The complex source graph has 26 nodes, 19 owners, six sample nodes, no function
  calls and a direct decoded Normal output. Its compiled layout has six resources,
  13 uniform fields and 224 uniform bytes. The simple texture/factor variant has
  one resource, three uniform fields and 64 uniform bytes. The plain identity
  texture and normal fixtures each require one node/owner; the default requires zero.
- Final workspace all build:
  `Build/.agent-state/logs/20260914-120309-591349-21584-cmake.log`.

Inspected retained default, normal, emissive, Unlit, masked, translucent, UV,
independent-map and simple/complex imported images. Existing independent numeric
and current-run equality controls passed; the new Normal output additionally
matches explicit-decode canonical bytes. The only intended visual changes remain
the Stage 0 final-value policy and documented source occlusion-strength correction.
All 35 current acceptance PNGs and the fresh source/Cook reports are retained under
`Documentation/Local/MaterialSurfaceAcceptance`; Git retains the implementation,
authored assets and reproducible capture tests. No legacy helper was removed because
ordinary function consumers remain supported. Supported domain remains Surface;
Lit/Unlit and Opaque/Masked/Translucent do not introduce additional material domains.

Long-lived contracts are updated in MaterialSystem, MaterialGraphOperations and
CanonicalResave. Changed-document and all-plan lifecycle validation complete the
handoff; prior passing tests are reused where final inputs remain unchanged.
