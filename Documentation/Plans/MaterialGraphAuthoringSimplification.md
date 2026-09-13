# Material Graph Authoring Simplification Plan

Summary: Replace parameter and UV wiring boilerplate with typed inline bindings and compact texture sampling while preserving material instance identities and explicit graph semantics.

Last reviewed: 2026-09-13

Status: Completed
Completed: 2026-09-13

## Current Status

Completed on 2026-09-13. The shipped ImportedSurface parent now contains eight
Texture Sample Parameter 2D nodes, an explicit normal decoder, a values-composition
call and Material Output: 11 visible elements instead of 66. All 48 declaration
GUIDs/types/defaults remain intact. The two inventoried VintageLighter instances
retain their original package bytes, eight/nine overrides and zero orphan overrides.
New imports use the same compact parent. Exact old-template migration and repeated
application passed; modified templates or standard-function bodies are preserved.

Program schema 6, function schema 2 and clipboard schema 5 are implemented with
bounded old-schema readers. Frozen schema-5/schema-1 fixtures match the compact
recipe's normalized IR, generated Slang and parameter layout exactly. Cooked
layout remains 4; no renderer ABI or compiler identity change was necessary.
Numeric bindings, retained UV settings, channel outputs, shared editor commands,
asset picking, advanced-input visibility and explicit parent navigation are implemented.

Validation receipts under `Build/.agent-state/logs`:

| Coverage | Result | Receipt |
| --- | --- | --- |
| Complete MaterialTests, including serialization, nested overrides and graph-stripped Cook/load | 188/188 passed | `20260913-165709-366899-23536-MaterialTests.log` |
| Final graph commands and advanced pin identity coverage | 42/42 passed | `20260913-170352-581076-22004-MaterialTests.log` |
| Expanded canvas capture, channel fan-out, UV extract/disconnect/undo | 1/1 passed | `20260913-170526-356721-12696-MaterialTests.log` |
| Scene import and edited-dependency migration rejection | 6/6 passed | `20260913-170010-323579-12636-SceneImportTests.log` |
| Forward Vulkan material rendering | 1/1 passed | `20260913-164822-660257-16376-MaterialVulkanTests.log` |
| Material thumbnails | 8/8 passed | `20260913-165120-240204-8356-MaterialThumbnailTests.log` |
| Directional shadow qualification with legacy/compact GBuffer captures | Both passed | `20260913-165310-387922-11280-ctest.log`, `20260913-165454-997198-23692-ctest.log` |
| Final all-project build (Engine, Sandbox, RoadWeaver) | Passed | `20260913-170427-422198-1680-cmake.log` |

DurinDevTool asset and command contract tests passed (31/31). Same-machine SHA-256
comparisons matched all 20 forward captures, 82 shadow baseline files and 11
GBuffer/material-input captures. The additional GBuffer baseline was captured using
the retained exact legacy recipe, then compared with the compact recipe in the same
qualification executable. Evidence, original package backups, eligibility inventories,
instance hashes and migration idempotence results are retained under
`Build/MaterialGraphAuthoringBaseline`.

The six canvas PNGs in that directory are software captures of actual ImGui canvas
and Details draw data, using unassigned-resource placeholders. Normal-zoom Details
shows the four independent UV bindings; captures also record channel fan-out,
extraction, inactive external UV settings and restored local settings. These are
headless UI evidence, not GPU thumbnail captures. Instance behavior is covered by
the parameter-panel and nested-instance regressions; the explicit Open Parent
Material control uses the existing asset workspace navigation.

Implementation contracts are maintained in [Material System](../Runtime/Rendering/MaterialSystem.md),
[Material Graph Operations](../Editor/Architecture/MaterialGraphOperations.md) and
[Canonical Resave](../Editor/Guides/CanonicalResave.md). This plan preserves the
instance configuration/publication/Cook lifecycle owned by
[Material Instance Shader Variants](MaterialInstanceShaderVariants.md).

## Goal

Connections describe computation and surface usage. Nodes expose editable assets
and meaningful operations. Defaults and parameter references may be authored on
typed inputs without requiring separate expression nodes. An instance retains
every existing texture, factor and independent UV override after migration.

Target for the complete eight-map ImportedSurface parent: ten expression nodes
plus Material Output, with no collapsed or invisible expression nodes counted as
savings. This is an authoring target, not a claim about reduced GPU instructions.

## Reference and Selected Direction

UE distinguishes TextureObjectParameter (a replaceable resource passed into
functions) from TextureSampleParameter2D (a replaceable resource sampled into
channel outputs). Its sample UV input can be unconnected; TextureCoordinate
provides an explicit channel index and tiling when needed. See Epic's
[parameter expressions](https://dev.epicgames.com/documentation/en-us/unreal-engine/material-parameter-expressions-in-unreal-engine)
and [coordinate expressions](https://dev.epicgames.com/documentation/en-us/unreal-engine/coordinates-material-expressions-in-unreal-engine).

Adopt these semantic distinctions. Inline parameter bindings and per-sample
parameterized UV transforms below are Durin design choices, not claims that UE
implements the same storage or UI. Preserve the current generic compiled
parameter layout; never restore renderer-owned BaseColor/Roughness texture slots.

## Authoring Model

| Authoring element | Meaning and default interaction |
| --- | --- |
| Texture Object Parameter | Reference a named Texture2D declaration; output a resource, without sampling. Use for texture function inputs or sampling one resource at several coordinates. |
| Texture Sample | Sample a connected texture object; disconnected UV uses its retained UV settings. Texture input remains required. |
| Texture Sample Parameter 2D | Reference a material-owned Texture2D declaration and sample it in one node; show name, thumbnail, asset picker and channel outputs. |
| Texture Coordinates | Produce Float2 coordinates using inline channel/transform settings; use when sharing coordinates or feeding custom UV operations. |
| Numeric input | Show a literal or named parameter binding when unconnected; allow an explicit expression connection. |
| Material function | Reuse computation with explicit typed inputs and outputs; never implicitly declare caller parameters. |

Texture names and usage hints do not restrict destination pins. Output value
types and explicitly authored operations determine valid connections. Color
space, sampler state, fallback texture and normal decoding remain real data
interpretation policies; changing a wire must not silently change them.

The old TextureParameter opcode remains a resource reference and is presented
as Texture Object Parameter. Do not rename its persisted enum value merely to
change its label. Append new authoring opcodes; retired numeric slots stay retired.

### Inline input bindings

Introduce a common reflected numeric input binding used by ordinary-node inputs,
function-call inputs and coordinate settings. Proposed semantic shape:

```text
InputBinding
  optional source link
  retained fallback: None | typed Literal | Parameter GUID
```

A connection is active when present. Otherwise resolve the retained binding;
for function inputs with None, use the callee's declared optional default.
Required inputs with no source or retained binding still fail validation. Do not
invent zero defaults for every opcode. Node creation supplies only registered,
meaningful defaults. Surface and Texture2D retain their existing typed function
defaults and resource connections; numeric literals cannot stand in for them.

Parameter bindings are legal only in root material graphs and refer to existing
declarations with exact types. Function graphs use literals, declared function
defaults or FunctionInput connections; they cannot reach into a caller's root
declarations. Create/promote commands atomically create or reuse declarations
with the binding. No implicit declarations or duplicate stored parameter values.

Active inline parameter references participate in reachability, layout, override
resolution and invalidation exactly like active Parameter nodes. Inactive
fallback references remain serialized, reference-checked and protected against
accidental declaration deletion, but do not force a runtime binding. Existing
orphan override behavior applies; disconnecting restores the same GUID binding.
Inline references count toward existing parameter and serialized-size bounds.

Connecting an input retains its previous fallback. Disconnecting restores it.
Reset-to-default and remove-binding are distinct commands. For required inputs,
disconnect rejects when no valid retained value exists. Each gesture is one
validated transaction with Undo/Redo and no compile on rejected/no-op edits.

### UV defaults and independent overrides

Both sampling nodes retain a UV settings value: channel 0, scale (1,1), offset
(0,0), rotation 0 radians. Each field accepts a literal or numeric parameter
binding. A connected Float2 UV expression replaces the entire local UV settings;
the settings remain available for disconnect, are visibly inactive, and never
apply a second transform to externally computed coordinates.

The independent Texture Coordinates node uses the same settings and lowering.
Its channel field can additionally take an explicit scalar expression for
existing dynamic graphs. Advanced field-expression wiring on a sample is exposed
by extracting its settings into a Texture Coordinates node, preserving bindings.

Preserve existing channel selection, missing-channel recovery, radians and
transform order: scale, rotate about the origin, then offset. Stage 0 records
the exact supported channel range and fractional/out-of-range policy from the
current compiler and mesh path; this refactor does not silently change them.
Identity transforms may disappear only when they are literal constants, never
because today's parameter default happens to be identity.

Each imported map retains its own four existing UV parameter GUIDs. Eight fields
currently equal to UV0 are not permission to merge their override identities.
Sharing UVs is an explicit authoring action via one coordinate node and wires.

### Channel outputs and normal data

Sampling exposes stable output indices: 0 RGBA (Float4), 1 RGB (Float3), 2 R,
3 G, 4 B, 5 A (Float), and 6 RG (Float2). Keeping index 0 preserves existing
TextureSample2D link meaning. Output labels or visibility never determine index.
Ordinary links use SourceOutputIndex; function outputs retain SourceOutputId.
Validation must reject inappropriate combinations and nonexistent indices.

All outputs share one sample operation. Lower channel selection to ordinary
swizzles; multiple consumers must not emit independent fetches. Normal decoding
stays an explicit decoder consuming RG. This avoids equating a
texture name or destination with normal interpretation. Broader normal formats
are outside scope; preserve current RG decode and RNM behavior.

Implementation finding (2026-09-13): the shipped SampleNormal graph performs
strength-one encoded RG centering/scaling before DecodeNormalRG. Removing those
operations changes normalized arithmetic. The compact parent therefore uses an
additive DecodeImportedNormalRG function containing that exact arithmetic and
DecodeNormalRG; RNM remains in ImportedSurfaceValues. It still occupies one
explicit decoder node and retains the 11-element target. Frozen-v5 versus compact
IR, layout and generated Slang equality passed on 2026-09-13, receipt
`20260913-162856-876203-18824-MaterialTests.log`.

## Editor Experience

- Dragging a Texture2D into a root material creates a Texture Sample Parameter
  with an explicit unique declaration and editable default resource in one
  transaction. The creation menu also offers a resource-only parameter node.
- The title is the parameter name, with the operation shown as secondary text.
  Replace the always-visible declaration dropdown with a thumbnail/resource
  picker. Move reuse/rebind and rename into clearly labeled Details/actions.
- Inputs show literal controls or a parameter-name badge. Details provides
  Create Parameter, Bind Existing Parameter, Reset and Extract Parameter Node.
  Selecting another declaration never presents itself as choosing a usage.
- Samples show a concise UV summary, including an indicator for parameterized
  settings even at default values. Details exposes the four independent fields.
- Optional unconnected advanced pins can be hidden; connected pins are always
  visible. Selection, pin hit testing, link routing and diagnostics use stable
  identities, not the current visible row number.
- Extracting UV settings or a parameter produces normal editable nodes in one
  command. Inlining an explicit node is permitted only when its expression is
  representable and no other consumer loses it; never discard a shared source.
- Opening an instance continues to show grouped parameters, inherited values and
  override controls. Provide a clear Open Parent Material action. Instances do
  not acquire a graph, and editing a parent is never implicit.

## Imported Template

Create a new ordinary library function, provisionally `ImportedSurfaceValues`,
which accepts already-sampled values plus separate numeric factor inputs and
returns Surface. Preserve existing StandardPBR and StandardPBR_ORM signatures
and implementations for their current callers; the new function is additive.

The compact parent consists of eight Texture Sample Parameter nodes, one
DecodeImportedNormalRG function call, one ImportedSurfaceValues call and Material Output: 11 visible
elements rather than 66. The call binds eight factor parameters inline. Each
sample retains one texture GUID and four UV GUID bindings. All 48 existing
declarations retain identity, type, default, metadata and instance eligibility.

Connect RGB for BaseColor/Emissive, decoded RG for Normal, B for Metallic, G for
Roughness, R for AO/OpacityMask and A for Opacity, matching the existing recipe.
The new function retains the original factor saturation, roughness clamp,
emissive addition and RNM operations, including their order. This change must
not opportunistically correct or reinterpret the imported shading model.

Do not infer packed ORM reuse merely from equal texture paths. It requires
compatible resource/parameter identity, UV expression, sampler, fallback and
independent override intent. Automatic packing or source-channel import changes
are deferred. The 11-element acceptance target is for the full parent, not a
promise to specialize every instance or remove unused map branches.

## Compiler, Serialization and Migration

Compact authoring is semantic state, not an editor projection over hidden
parameter nodes. Engine validates and lowers inline bindings, sample parameters,
UV settings and selected outputs into existing ordinary expressions before
normalization. Functions still expand through the existing bounded closure.
No new renderer texture roles, runtime graph interpreter or second compiler.

Carry origin mappings from generated operations to node, pin or UV field for
diagnostics. Enforce bounds after expansion as well as before it. Canonical IR
must remain independent of labels, positions and equivalent inline/explicit
spelling; literal constants and parameter reads must remain distinguishable.

Bump authored program, function and clipboard schemas where binding/link meaning
changes; retain presentation separation. Design an explicit old-schema decoding
and upgrade route before raising version guards. A version bump alone is not
migration. Update compiler/envelope keys when normalization changes. Preserve
the cooked ABI if the final IR and parameter layout permit it; otherwise version
and recook explicitly, coordinated with the instance variants work.

General schema upgrade maps old links to connected bindings and preserves node
GUIDs, call port GUIDs, declarations, assets and positions. It does not simplify
arbitrary user graphs. Template compaction is a separate maintenance operation
for exact shipped graphs and compatible dependency implementations. Compare
function bodies/dependencies as well as signatures: edited UVTransform or PBR
implementations must not be replaced by assumed built-in behavior.

Inventory package/schema/provenance and instance overrides before writing. Emit
per-asset eligibility and skip reasons. Preserve edited templates and callers,
and allow explicit local inline/extract commands for those graphs. Save failures
restore graph, function calls, declarations, presentation and prior dirty state;
retain recoverable old package bytes. Re-running successful migration is a no-op.

## Implementation Stages

### Stage 0 Findings

- `SelectAuthoredUV` in MaterialProgramGenerator emits
  `(uint)clamp(floor(channel + 0.5), 0.0, 3.0)`: four supported channels,
  nearest integer with half steps toward positive infinity, then clamping.
  StaticMeshBuildOperations fills missing imported UV channels with (0,0),
  and StaticMeshBasePass forwards all four channels. Do not substitute UV0
  for missing channels during compact lowering.
- The current UVTransform function applies scale, rotation in radians about
  (0,0), then offset. Expanded origin records contain NodeId, FunctionAssetPath
  and CallPath; compact lowering must additionally identify the authored field.
- Reserve program schema 6, function schema 2 and clipboard schema 5 for this
  change. Keep declaration schema 2, graph presentation 2 and function
  presentation 1 unless actual storage changes require otherwise. Cooked layout
  remains 4; compiler identity/envelope changes require a separate compatibility
  review after normalized behavior is measured.
- Storage decision after code inspection: retain the serialized `Inputs` link
  arrays and append typed `InputDefaults` at matching indices. Function-call
  bindings likewise retain Source and append Default. This realizes the selected
  connection-plus-retained-value model without changing old field meanings or
  requiring deprecated array readers. Schema upgrades must require empty new
  fields in old-version payloads, then validate the candidate before promotion.
- DMaterial::PostLoad currently contains a bounded schema-4-to-5 upgrade before
  ValidateMaterialProgramWithFunctions. Extend this entrypoint with a bounded
  5-to-6 conversion, preserving the existing older route. DMaterialFunction has
  no PostLoad override today; add a 1-to-2 upgrade before its graph can be used
  by BuildFunctionSnapshot. New binding fields must not reinterpret serialized
  old `Inputs` link arrays: preserve them and initialize separate default storage.
  Cover reflected function-call input records
  as well as positional inputs. API setters continue rejecting unsupported input.
- Selected registry targets are MaterialTests, MaterialThumbnailTests and
  MaterialVulkanTests. Run MaterialTests first, then bounded GPU baseline and
  final affected coverage. MaterialCreationQualificationTests concerns resource
  creation failures and is not the initial semantic baseline target.
- The six frozen packages preserve the complete shipped function dependency
  set, including ORM. They are source fixtures, not yet evidence that loaded
  assets match their expected recipes. The current project also contains two
  VintageLighter material packages requiring runtime override inventory.

### Stage 0: Freeze fixtures and compatibility inventory

Outcome: executable baseline and a schema rollout that cannot strand old assets.

- [x] Inventory current material/function packages and template dependencies;
  record schemas, override GUIDs, node counts and edited implementations.
- [x] Capture baseline IR, parameter layouts and representative forward,
  GBuffer and masked-shadow images, including nonidentity independent UVs.
- [x] Record current UV channel edge behavior and origin mappings needed by
  lowering; specify exact version numbers and old-schema decode entrypoints.
- [x] Select affected native tests using [testing guidance](../Agents/Testing.md).

Gate: fixtures cover all eight roles, nested instances, UV0/UV1, sampler/fallback,
normal decode and edited standard-function dependencies before schema mutation.

### Stage 1: Typed bindings and authoring lowering

Depends on Stage 0. Outcome: compact programs validate and compile without UI.

- [x] Implement bindings, parameter reachability, stable multi-output typing,
  UV settings, compact sample nodes, lowering and source diagnostics.
- [x] Implement old-schema decoding, validation bounds and serialization tests
  for both root materials and functions; preserve the root-parameter boundary.
- [x] Prove inline/explicit equivalence, one-fetch channel fan-out, independent
  instance updates, inactive fallback restoration and unchanged cooked behavior.
- [x] Search all affected shared API symbols across source/test roots of every
  project in Durin.dworkspace and migrate all consumers.

Gate: roundtrips and negative validation pass, layouts retain parameter GUIDs,
and equivalent compact/explicit graphs produce equivalent normalized semantics.

### Stage 2: Authoring commands and canvas

Depends on Stage 1. Outcome: users can author compact graphs without raw storage edits.

- [x] Add inline controls, resource picking, named parameter actions, UV Details,
  output pins, explicit extraction and safe inlining to shared graph commands.
- [x] Migrate copy/paste, duplicate, delete, inspection, layout, Undo/Redo,
  function previews and diagnostic navigation to the new binding sources.
- [x] Verify foreign-root parameter remapping and function clipboard rejection;
  retain texture/callee references through GC, reload and transaction history.
- [x] Capture actual canvas/Details evidence for a newly constructed compact
  material, channel fan-out and UV extract/disconnect/undo at normal editing zoom;
  verify instance versus parent editing through parameter-panel regressions and
  explicit parent navigation. Headless captures do not claim live GPU thumbnails.

Gate: the same operations work through command tests and canvas, preserve
fallbacks and sharing, and create exactly one undo record per user action.

### Stage 3: Compact imported parent and explicit asset upgrade

Depends on Stage 2. Outcome: a newly imported scene uses the 11-element parent.

- [x] Add ImportedSurfaceValues and the compact parent recipe; preserve all
  existing declaration identities and standard-function signatures.
- [x] Add exact-graph/dependency eligibility, inventory reports, rollback and
  idempotent maintenance migration, including historical shipped templates.
- [x] Validate new import and existing nested instances, sampler and missing
  textures, independent map UVs, customized functions and modified-template skips.

Gate: 10 authored expression nodes plus output, all 48 declarations active for
the complete parent, no hidden replacement graph, and baseline shading parity.

### Stage 4: End-to-end qualification and contracts

Depends on Stage 3. Outcome: authoring, migrated assets and cooked projects agree.

- [x] Run affected project/test targets and a final `all` build using
  [build guidance](../Agents/BuildAndRun.md); serialize build ownership.
- [x] Compare captured same-environment images and normalized computations;
  investigate mismatches rather than accepting node-count improvements alone.
- [x] Verify forward, GBuffer, masked shadows, graph-stripped Cook/load and
  instance shader variants without runtime authored-graph dependencies.
- [x] Update implemented Material System and Material Graph Operations contracts;
  record migration/validation receipts and complete this plan only after gates pass.

## Ownership and Exclusions

Engine owns schemas, validation, parameter resolution, function expansion,
normalization and asset compatibility. MaterialEditor owns commands, controls,
clipboard, presentation and interaction tests. AssetForgeBuiltins owns the
standard function and imported parent recipes. DurinAssetTool owns the existing
maintenance entrypoint; extend it rather than adding a hidden load-time rewrite.
Renderer/RenderCore changes require evidence from the lowered contract, not UI
convenience. Native coverage starts with MaterialGraphOperationsTests,
MaterialFunctionTests, MaterialParameterPanelModelTests and the standard-material
fixtures, then follows the affected project targets.

Generic collapsible groups, automatic graph-wide common-subexpression rewrites,
automatic ORM conversion, static-switch permutations, new texture dimensions,
new normal encodings and scene reimport are outside this plan. The goal is a
smaller, explicit authoring model with preserved semantics and override intent.
