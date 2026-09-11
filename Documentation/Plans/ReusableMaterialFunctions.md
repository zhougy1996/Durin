# Reusable Material Functions Plan

Summary: Add typed material function assets and replace expanded imported PBR graphs with texture-driven StandardPBR calls, including editor workflows and explicit asset migration.

Last reviewed: 2026-09-11

Status: Active
Completed:

## Current Status

Stage 0 completed on 2026-09-11. The interface and bounds are frozen below;
the 22 source packages have recorded identities, references and SHA-256 hashes.
MaterialVulkanTests retains shader baselines, and SceneImportVulkanTests now
retains actual imported PBR, UV1/transform, packed-source/independent-map and
masked captures with resource counts. Audit schema drift and registered struct
alias misclassification are corrected. Stage 1 completed on 2026-09-11: typed function
assets, stable declarations/call records, base-typed reference serialization and
bounded detached closure capture are implemented. Root-material call admission,
compiler expansion, multi-output lowering and expression source metadata are
implemented, including Surface Get/Set and nested diagnostic qualification.
Stages 2-5 have not started.

The 2026-09-11 prerequisite refactor changes current compilation failure and
admission rejection to retire the owner's accepted renderable generation and
publish ErrorMaterial. Pending work retains a valid prior generation. This
updates the shared lifecycle contract without completing a stage of this plan.

Prerequisite validation: the default `all` build passed and MaterialTests passed
143/143. The affected selection passed 81/83 targets; the two texture fixture
failures were then corrected and independently passed. MaterialVulkanTests (1/1)
now rejects old thumbnail captures after allocation failure retires publication.
TextureCookIntegrationTests (3/3) cooks its sample material before entering
cooked-only runtime, then loads the artifact and binds the texture on a transient
instance without compilation or package mutation. Together these runs cover all
83 selected targets. Evidence is under `Build/.agent-state/logs/`:
`20260911-151707-641828-36276-cmake.log`,
`20260911-151749-246446-18452-ctest.log`,
`20260911-152543-389311-10264-MaterialVulkanTests.log`, and
`20260911-152515-821753-36484-TextureCookIntegrationTests.log`.

Selected by the user on 2026-09-11 after the RenderProxy parameter pass-through
fix (`5b1ea1344`). The initial commit recorded the implementation proposal only;
Stage 0 execution is now complete. The user permits aggressive
schema changes and upgrades or modification of the small existing asset set.
This authorization permits a coordinated content migration; it does not require
maintaining a permanent legacy compiler or silently discarding authored edits.
The user subsequently selected an abstract `DMaterialFunctionInterface` asset
base, with `DMaterialFunction` as its first concrete implementation. Function
instances remain deferred; this is a design update, not completed implementation.

This is M11 of the [Material System roadmap](../Roadmaps/MaterialSystem.md).
[Material Instance Shader Variants](MaterialInstanceShaderVariants.md) has
landed its implementation and remains active for final qualification. Consume its
landed property, variant publication, Cook and import contracts. M11 can proceed
independently; reconcile compiler snapshot, dependency, Cook and import changes
before editing shared seams. There is one
source/build writer per checkout.

## Goal

ImportedSurface exposes texture parameters, scalar/vector parameters and UV inputs
feeding one ordinary StandardPBR function call and a Surface output. Sampling,
channel extraction, normal processing and PBR composition live in reusable
function assets. Authors can open functions, reuse them with independent inputs,
and modify selected Surface attributes without expanding the entire function.

The runtime proxy remains parameter-identity agnostic. Functions disappear into
compiled material programs; Renderer never loads function assets or graph nodes.

## Existing Foundations and Change Sites

- `Engine/Source/Runtime/Engine/Public/Materials/MaterialProgramTypes.h` has
  program schema 4, typed Texture2D and Surface values, MakeSurface, and links
  identified by source node GUID plus positional output index. Function calls
  need stable interface identities beyond those positional indices.
- `Engine/Source/Runtime/Engine/Private/Materials/MaterialProgramTypes.cpp`
  constructs the expanded MakePBRMaterialProgram graph. Preserve its intentional
  shading behavior while moving composition into standard function assets.
- `Engine/Source/Runtime/Engine/Public/Materials/MaterialProgramCompiler.h` and
  the private compiler/lifecycle files own detached snapshots, deterministic IR,
  shared compile work and generation-safe publication. Extend these boundaries.
- `Engine/Source/Editor/AssetForgeBuiltins/Private/ImportedSurfaceMaterial.cpp`
  creates the expanded parent and compares its whole program with the built-in
  graph. `SceneDirectImport.cpp` consumes the shared parent. Replace exact graph
  equality with an explicit standard-asset revision and required interface check.
- MaterialEditor already has shared semantic commands, reflected presentation,
  transactions, clipboard and diagnostics. Generalize this graph ownership seam
  for function documents instead of adding a separate canvas implementation.

## Selected Design

### Function assets and stable interfaces

Introduce an Engine-owned abstract `DMaterialFunctionInterface` asset base and
derive `DMaterialFunction` from it. The base is specific to material functions,
not a general script interface. It provides the read-only function signature,
dependency queries and owning-thread entry point for a detached compilation
snapshot. It does not require an editable graph or own graph presentation.

`DMaterialFunction` owns the bounded reflected function graph, ordered typed
input/output declarations and editor presentation, and implements the base
contract. Graph mutations and transactions target this concrete graph owner;
callers and dependency resolution consume the abstract contract. Keep snapshot
creation on the owning thread and reuse the existing compile lifecycle rather
than introducing per-subclass scheduling or runtime virtual shader execution.

FunctionCall references `DMaterialFunctionInterface` through the existing asset
reference machinery, not a concrete `DMaterialFunction` reference. Validation,
reference enumeration, asset picking and snapshot closure use that declared base
type, while creation offers only supported concrete types. Do not require callers
to downcast to obtain the signature or effective compilation snapshot.

The first implementation contains only `DMaterialFunction`. Reserve the base
boundary for a future `DMaterialFunctionInstance`, but do not add instance assets,
override storage or inherited-function semantics in this plan. This class-level
interface is separate from the stable input/output port identities below.

Reuse common node/link values, but give functions their own terminals rather than
pretending that every function has a material Surface root.

FunctionInput, FunctionOutput and FunctionCall are generic graph concepts. Calls
hold an asset reference and input bindings keyed by stable interface GUIDs;
output connections identify the stable function output GUID. Built-in opcode pins
may remain positional. Reordering or renaming function interfaces preserves
connections; deleting/retyping an interface yields a located diagnostic rather
than reconnecting by index. Removed bindings remain recoverable through Undo and
must not be silently reassigned to another interface.

Support the existing numeric types, Texture2D and Surface, multiple outputs and
nested calls. Declare exact types; no generics, overload resolution or arbitrary
shader code. Functions do not implicitly inject material parameters: their
internal graph uses input ports and constants. Exposed parameter declarations
remain owned by the root material, preserving instance override identity and
preventing same-name parameter collisions between calls.

Unconnected optional inputs use declared defaults; required inputs diagnose a
missing binding. Keep preview fixtures separate from production defaults.
Numeric literals, texture fallback/sampling policy and default Surface attributes
have explicit typed representations. A static fallback expression may use other
function inputs without introducing cycles: per-map UV defaults to the common UV,
which defaults to UV0. This needs no general static-switch feature.

### Texture-driven standard library

TextureParameter remains one generic texture-object node. FunctionCall accepts
that object; TextureSample2D performs sampling inside the function. Preserve the
existing sampler and fallback association when passing texture inputs through
nested calls. Define the precedence of input defaults and caller sampling state
before implementation; never infer it from PBR GUIDs.

Create ordinary standard-library assets for UVTransform, normal sampling/strength,
packed ORM sampling, and StandardPBR. Lower-level functions may be called directly.
StandardPBR accepts textures plus factors and UVs and outputs Surface. Common
inputs are shown first; independent per-map UVs and less common settings are
advanced inputs. Do not expose the entire expanded implementation as call pins.

Preserve all currently imported channels, opacity/mask behavior, emissive factors,
UV channels/transforms, sampler states and texture fallbacks. No-texture inputs
have explicit neutral behavior: flat normal, white multiplicative samples and
appropriate emissive defaults. A missing authored connection and a temporarily
unavailable texture resource are distinct states; resource fallback must still
work without recompilation.

Stage 0 freezes the importer channel matrix. Provide a bounded StandardPBR entry
for the current independent-map layout and a packed ORM entry where warranted by
actual imports; both reuse composition helpers. Do not pack existing images or
change their import settings just to fit the function interface. Packed ORM uses
one sample with three outputs when texture, UV and sampling requirements match.
Different UVs or sampling requirements must not be merged. Shared function code
alone is not evidence of shared texture fetches.

Color, normal and data sampling expectations belong to explicit graph/resource
contracts. Validation may use actual texture declarations and sample requirements;
it must not restore GUID-based usage rejection, CPU clamps or normalization.
Retain the current intentional PBR clamps and safe normal behavior inside the
library graph/shared shading primitives. StandardPBR is not a new hardcoded PBR
opcode. Parameter names and function names do not select compiler behavior.

### Surface composition

Reuse Surface and MakeSurface. Add typed GetSurfaceAttributes and
SetSurfaceAttributes operations with explicitly selected attribute pins.
Unmodified fields pass through, defaults remain deterministic, and arithmetic
cannot consume Surface values. A StandardPBR result can have Emissive or Normal
replaced without duplicating its graph. Surface blending and Material Layers are
outside this plan.

### Compilation and dependencies

Resolve the bounded transitive function closure on the owning thread into an
immutable value snapshot. Workers receive no reflected objects or asset loads.
Validate missing references, recursive calls, incompatible ports and limits on
call depth, dependency count, total expanded nodes, texture resources and payload.
Freeze numerical limits in Stage 0 against existing compiler/resource bounds.

Expand calls before existing normalization and layout derivation. Namespace
internal node identities by call path so separate invocations cannot collide.
Lower multi-output calls and Surface access deterministically; prune unreachable
outputs and reuse shared expressions where valid. Keep a separate diagnostic
source map from lowered expressions to root call path, function asset and node.
Authored GUID/order/presentation changes must not disturb equivalent shader keys.

Dependency revision and compile identity are distinct. Track every referenced
function/interface for invalidation; hash the effective compiled semantics and
compiler versions for artifact identity. Edits to code, interfaces or defaults
invalidate affected callers, while presentation-only edits do not compile.
Dynamic root parameter values and bound texture identities stay out of shader
keys. Test warm/cold compilation and two calls with different bindings.

Extend existing reference enumeration, reload/relocation/deletion handling and
compile lifecycle. Pending function edits retain the complete accepted
program/layout/value contract when available; current compilation failure or
admission rejection retires it and publishes the existing error terminal.
This prerequisite follows the shared Material System failure contract and does
not advance a function implementation stage. Stale closure
snapshots cannot publish. Cook resolves function dependencies ahead of compilation;
cooked runtime loads accepted artifacts without authored functions or a compiler.
Do not add another scheduler, DDC or render publication path.

### Editor and asset migration

Support function creation/opening, interface editing, function-call insertion,
enter-function navigation and previews through the shared command surface.
Canvas and structured operations use the same validation, transaction and
clipboard contracts. Commands bind function pins by stable identity. Diagnostics
navigate through nested calls to the originating node. Surface outputs can preview
as a material; numeric/texture outputs use an explicit preview wrapper.

Bump affected authored schemas and compiler versions once the new formats are
locked. Keep any reader for the prior format bounded to an explicit upgrade path;
normal authoring, compilation and Cook use the new representation. Do not retain
parallel legacy and function-based PBR production implementations after migration.

Inventory actual packages and source-controlled storage in Stage 0, including
external LFS-backed content. Record old identity, path, references and disposition.
Migrate the shared ImportedSurface parent to a small call graph, retaining root
parameter GUIDs and material/instance paths where possible. Preserve override
values, parent chains, material slots, property flags and sampler/UV settings.
Known expanded templates can be converted automatically. Modified graphs remain
valid independent graphs after schema upgrade; use reviewed per-asset edits when
replacing their subgraphs, never overwrite them based only on a familiar GUID.

Standard assets have explicit revision/provenance. Bootstrap creates missing
assets; upgrade recognizes supported revisions, and reports incompatible edited
interfaces without repeatedly restoring a built-in graph. Use existing package
save/catalog operations, persist function dependencies first, and make retries
idempotent. Record saved/failed packages and a recoverable checkpoint for interrupted
multi-package migration. Rebuild derived artifacts instead of treating them as
source assets. Re-run migration to verify no additional changes.

## Stage 0 Execution Record

### Frozen function representation

Keep `FMaterialProgramLink.SourceNodeId` and `SourceOutputIndex` for built-in
nodes; add `SourceOutputId` for function outputs. A function output link requires
a valid output GUID and zero positional index. Built-in links require an invalid
output GUID. Never interpret a missing function GUID as positional port zero.
Calls store a base-typed asset reference and a separate array of
`{InputId, ExpectedType, Source}` bindings. Store expected output types on call
interface records too, so retyping diagnoses even when the consuming operation
would accept both types. Renaming/reordering changes display order only.
FunctionInput/FunctionOutput terminals store their declaration GUID. Duplicate
port GUIDs, duplicate bindings and orphan bindings diagnose at the call/port.

Each declaration stores GUID, exact type, name, display order, advanced flag,
required flag and a tagged default. Default alternatives are numeric literal,
texture fallback plus sampling policy, Surface attributes, another input GUID,
or UV0. Input references must match types; validate their directed graph for
cycles before substitution. No arbitrary expression language is needed for
defaults. Functions own nodes and terminals separately from material Surface
outputs and cannot declare root parameters.

Texture values carry their resource origin, sampler and fallback through every
call. A connected caller value takes precedence as one complete tuple, including
when its resource is null or temporarily unavailable. Only an absent connection
uses the callee's default tuple. A nested call must never replace a connected
texture's fallback with its own default. Default sampling is the existing
`FMaterialSamplerState{}` contract. Per-sample usage requirements remain explicit
and cannot depend on standard-library names or parameter GUIDs.

Freeze limits at 256 authored nodes and 1024 links per document; retain the
built-in eight-pin signature limit. Function signatures allow 64 inputs and 16
outputs, independently of built-in pin arrays. Allow 16 nested calls, 64 distinct
function dependencies, 4096 expanded nodes and 16384 expanded links, bounded
before pruning. Lowered expression depth remains 64. Limit each document to
1 MiB canonical payload and the detached closure to 8 MiB. Retain 128 root
parameters, 64 render resources, 256 render fields, 16 KiB uniform payload and
64 diagnostics of at most 512 bytes. Device/compiler admission still applies
its stricter sampled-image and sampler limits (default policy 16 each, with
existing renderer reservations); 64 is not an advertised device texture budget.
Expansion must use its own bounded intermediate representation instead of
silently raising the authored-document limit.

Reserve authored program schema 5, function schema 1 and function presentation
schema 1. Keep material presentation schema 2 and parameter schema 2: root
parameter storage and node-position representation do not change. Reserve IR 4,
generator 5 and compiler envelope 7 for the lowering change. Retain pass contract
2 and layout version 4 unless implementation changes their wire semantics.
Keep DMAT payload 5 when lowered artifacts remain wire-identical; advance the
material Cook recipe from 3 to 4 to invalidate old dependency recipes. DAST v9
remains the container format. These reservations are design decisions, not yet
changes to production constants.

### Standard library ports and import channel matrix

`StandardPBR` has eight Texture2D inputs named `<Role>Texture`, eight factors
named as the roles below, Float2 `UV`, and eight advanced Float2 `<Role>UV`
inputs. All are optional. `UV` defaults to UV0; each map UV defaults to `UV`.
The sole output `Surface` has type Surface. Normal and Emissive factors use
Float3, as does BaseColor; the remaining factors use Float. Common display pins
are BaseColorTexture, BaseColor, NormalTexture, Metallic, Roughness and UV.
Other pins are advanced. Persist deterministic standard port GUIDs once in the
library asset definitions; display text never computes identity at runtime.

| Role | Factor default | Texture default | Sample channels and current composition |
| --- | --- | --- | --- |
| BaseColor | `(0.5,0.5,0.5)` | White, Color | RGB multiplied by saturated factor |
| Normal | `(0,0,1)` | FlatRGNormal, Normal | RG decode followed by safe RNM with factor |
| Metallic | `0` | White, DataMask | saturated B times saturated factor |
| Roughness | `0.5` | White, DataMask | saturated G times saturated factor; clamp to `[0.045,1]` |
| AmbientOcclusion | `1` | White, DataMask | saturated R times saturated factor |
| Emissive | `(0,0,0)` | Black, Color | nonnegative RGB plus nonnegative factor |
| Opacity | `1` | White, DataMask | saturated A times saturated factor |
| OpacityMask | `1` | White, DataMask | saturated R times saturated factor |

`UVTransform` takes Float2 UV (UV0), Scale `(1,1)`, Offset `(0,0)` and Float
Rotation `0` in radians; output is `rotate(UV * Scale) + Offset`. ImportedSurface
keeps all existing per-role channel/scale/offset/rotation parameter GUIDs and
uses UVChannel plus UVTransform calls outside its one StandardPBR call.
`SampleNormal` takes Texture2D (FlatRGNormal), Float2 UV (UV0), Float Strength
`1` and Float3 Normal `(0,0,1)`; imported already-scaled textures use strength 1.
`SampleORM` takes Texture2D (White, DataMask) and Float2 UV (UV0), with Float
Occlusion/Roughness/Metallic outputs reading R/G/B from one sample.
`StandardPBR_ORM` replaces the independent metallic/roughness/AO textures and
UVs with ORMTexture and ORMUV, retaining their individual factors and sharing
composition helpers. Other inputs match StandardPBR.

Actual `SceneImport.cpp` translation is independent-map production: glTF
metallic/roughness is split into Blue/Green derived packages; occlusion derives
Red, opacity derives Alpha, normal strength may be baked into ScaledNormal,
and emissive factor is baked into ScaledColor. Do not apply baked factors twice.
Do not collapse those packages during migration. The packed entry is reusable
for authored packed inputs; selecting it for a future import requires an actual
shared texture/UV/sampler contract, not merely a common source image.

### Storage, migration route and shared seams

The selected workspace contains Engine, Sandbox and RoadWeaver projects.
Sandbox and RoadWeaver each mount their own Content as `/Game`; package paths
must therefore always be recorded with project ownership. Source packages are
DAST v9 files in their Content directories. Git LFS uses
`D:/Studyspace/Durin-LfsStorage`; relevant external objects are texture `.dbulk`
companions, not a second editable material catalog. Test fixtures and historical
branch archives are not production migration targets.

| Physical package or group | Disposition |
| --- | --- |
| Engine/Content/Materials/ImportedSurface.dasset | Inspect exact graph, preserve identity and 48 root parameter GUIDs; replace only recognized template with function call graph |
| Engine/Content/Materials/DefaultMaterial.dasset | Upgrade authored schema; preserve custom graph and identity |
| Sandbox/Content/Models/VintageLighter/Materials/vintage_lighter.dasset | Preserve instance identity, parent and overrides; verify deprecated-field upgrade |
| Sandbox/Content/Models/VintageLighter/Materials/vintage_lighter_alpha.dasset | Same, additionally verify alpha/static-property overrides |
| Engine/Content/Models/{Box,Sphere,SplineBox}.dasset | Preserve material slots; audit currently reports ImportedData struct-signature incompatibility |
| Sandbox/Content/Models/GrayboxPawn.dasset | Preserve material slots; same audit finding |
| Sandbox/Content/Models/VintageLighter/Meshes/vintage_lighter_1k.dasset | Preserve both material slots; same audit finding |
| Sandbox/Content/Models/VintageLighter/Textures/*.dasset (5) | Preserve source, derivation, usage, sampler associations and external bulk; no repacking |
| Sandbox/Content/Levels/GrayboxStage15.dasset | Preserve mesh/material references; verify after parent migration |
| RoadWeaver/Content/{Levels/L_RoadNet,NewRoadNet}.dasset | Preserve references and authored data; inspect embedded material consumers |
| Engine/Content/Renderer/DefaultStudioCube.dasset | Unchanged environment fixture |
| Sandbox/Content/Textures/{TEX_StoneHead,TEXCUBE_PureSky_512x512}.dasset | Unchanged texture sources and bulk |
| Sandbox/Content/Volumes/VolumetricCloud/{VT_Cloud_Base_Voronoi_128,VT_Cloud_Detail_Voronoi_64}.dasset | Unchanged volume sources and bulk |

Use AssetMaintenance inspection/planning and AssetTools creation/SavePackage
publication. Persist function dependencies first, then the parent, then any
instances requiring an authored resave. Record before/after package fingerprints,
old object paths and references, disposition and saved/failed status in a durable
migration checkpoint. Restart verifies already-saved fingerprints before skipping;
an edited package requires fresh inspection. Run canonical resave preview before
apply; a compatible header is not proof of matching template semantics. Derived
Cook/DDC artifacts are rebuilt. The source identity/reference inventory is linked
below; semantic graph recognition and idempotent executable migration remain
Stage 4 implementation work.

Owning changes span Engine Materials (types, validation, normalization, codegen,
snapshot/lifecycle, dependency routing and DMAT), AssetMaintenance/AssetTools
(inspection, migration and creation), AssetForgeBuiltins (bootstrap and scene
translation), MaterialEditor (documents, commands, clipboard, diagnostics and
previews), and the native material/import/Cook lanes. Renderer remains a lowered
program consumer. M13 already owns complete accepted instance generations,
effective static properties, root-parameter identity and DMAT v5. Function edits
must invalidate all affected effective variants through that lifecycle. M13 also
records that Scene import is creation-only: the reimport gates here cannot be
claimed through another fresh import and remain an explicit integration gap.

### Baseline receipts and outstanding evidence

On Win64-Debug-DurinEditor, SceneImportVulkanTests passed 1/1; receipt:
`Build/.agent-state/logs/20260911-155740-628394-15488-SceneImportVulkanTests.log`.
The focused compiler baseline passed 1/1; receipt:
`Build/.agent-state/logs/20260911-160105-915811-14708-MaterialTests.log`.
It measured 202 authored/IR nodes, eight sample expressions, 9870 canonical
bytes, 15087 generated source bytes, 126148 SPIR-V bytes and 130749 cooked bytes.
Source hash: `169173b413b631ef6784c9abac6f3f8e`. Existing reflection assertions
cover 24 forward bindings, 17 GBuffer bindings and no shadow bindings for this
opaque baseline. Shared texture object identity does not reduce the eight
authored sample expressions.

The final baseline-capture MaterialVulkanTests passed 1/1; receipt:
`Build/.agent-state/logs/20260911-160408-351842-10884-MaterialVulkanTests.log`.
Ten 64x64 PNGs are written under the test run's `ReusableMaterialFunctions`
directory. This run used `DURIN_TEST_KEEP_WORK=1`; a retained copy is under
`Build/.agent-state/evidence/ReusableMaterialFunctions-Stage0/ReusableMaterialFunctions/`.
Capture names distinguish a packed source with independent sample
bindings from the future single-fetch ORM implementation. The capture additions
restore all texture bindings before the existing graph-edit and pass checks.

Sandbox's raw audit is
`Build/.agent-state/logs/20260911-155833-526101-27144-DurinAssetTool.log`:
20 packages inspected, 15 compatible and five incompatible. The DevTool wrapper
rejects its deprecated-route entries because the native report omits
`customVersionGuid`, `sourceVersion`, `deprecatedBefore` and `migrationTargets`
required by its schema. Rebuilding DurinAssetTool does not correct the source
contract mismatch. This initial audit-tool defect is resolved in the completion
receipts below; it did not indicate a failed asset load or authorization issue.
The initial RoadWeaver audit succeeded and was retained in
`Build/ReusableMaterialFunctions-RoadWeaver-audit.json` (eight packages, three
shared Engine mesh incompatibilities). No source packages have been modified.

### Stage 0 completion receipts

The [source inventory](Evidence/ReusableMaterialFunctions-Inventory.json) records
all 22 packages, physical ownership, SHA-256 hashes, reflected objects and exact
reference routes. Engine packages are shared between projects and recorded once.
All object/reference inspections succeeded. This is a pre-migration checkpoint;
each package remains `Pending` until Stage 4 records its disposition and verifies
the before/after identity and references. No source package has been changed.

`DevTool asset identity-audit` exposes the existing native read-only inventory
through the normal runtime selection service. The audit JSON schema and human
renderer now match the native deprecated-route contract. Schema inspection
canonicalizes registered struct/enum/reference type aliases using its immutable
reflection catalog before comparing signatures, retaining the stored identity
in canonicalization evidence. This corrects the mesh false positives without
accepting unregistered types or changing stored package bytes.

Sandbox now audits 20/20 compatible and RoadWeaver 8/8 compatible. The Sandbox
canonical-resave preview marks the five aliased meshes and two material instances
ready, with the five derived textures skipped; no apply was performed. Receipts:
`Build/ReusableMaterialFunctions-Sandbox-audit.json`,
`Build/ReusableMaterialFunctions-RoadWeaver-audit.json`, and
`Build/ReusableMaterialFunctions-Sandbox-resave-preview.json`.

AssetPackageTests passed 148/148 including the new historical mesh struct alias
case, which checks compatibility, retained evidence and unchanged file bytes.
Receipt: `Build/.agent-state/logs/20260911-161816-064463-9716-AssetPackageTests.log`.
The final SceneImportVulkanTests passed 1/1; receipt:
`Build/.agent-state/logs/20260911-162116-831265-25916-SceneImportVulkanTests.log`.
Six imported PNGs are retained under
`Build/.agent-state/evidence/ReusableMaterialFunctions-Stage0/Imported-Final/`.
The PBR case imports `ImportedPbrContract.gltf`: metallic/roughness/AO derive
independent images from its packed source; it also exercises normal strength,
emissive, opacity/mask, UV1, transforms and non-default sampler addressing.
Both this masked case and the opaque texture/factor case report eight resource
fields, 40 uniform fields, 656 uniform bytes and 15087 generated source bytes.
The texture-free factor control and existing shader captures retain fallback
and UV baselines. The packed source baseline intentionally has eight sample
expressions; Stage 5 must separately demonstrate the new SampleORM single-fetch
implementation. Images were visually inspected to confirm visible geometry.

DevTool asset/command tests passed 29/29. A broader Python run also exposed a
pre-existing bootstrap manifest-count assertion (expected 10, actual 11); no
dependency manifests were changed by this work. The two command snapshot failures
introduced by adding identity-audit were corrected and passed in the focused run.
Native selection expands to most Engine targets for the private codec change;
the bounded package suite plus actual mounted audits and import GPU execution
cover its changed behavior. Full shared-API/editor validation remains a Stage 5
gate after function implementation.

## Stage 1 Execution Record

`DMaterialFunctionInterface` is abstract with no class default object. It exposes
signature, direct dependencies, revision and owning-thread snapshot creation;
`DMaterialFunction` owns the reflected graph and a separate presentation schema.
Default functions pass a typed Surface input through a FunctionOutput terminal.
Calls persist `TObjectPtr<DMaterialFunctionInterface>` in records keyed by node
GUID. Their input and output records preserve port GUIDs and expected types.
Common nodes contain no asset references; links now have `SourceOutputId`, and
function terminals have `FunctionPortId`. Root material graphs admit call records
with stable output GUIDs. Single-output built-ins require positional output zero;
GetSurfaceAttributes uses the stable surface-attribute enum index (0 through 7)
and an invalid function-output GUID.

Graph validation reuses the existing built-in node shape/payload validator and
adds function terminals, stable call output lookup, no-root-parameter checks and
bounded graph validation. Signature validation covers typed numeric, texture,
Surface, UV0 and input-reference defaults, required inputs and default cycles.
Deleted/retyped call ports report their stable GUID and function path.

`SnapshotMaterialFunctionClosure` consumes only the abstract contract and emits
pure-value snapshots sorted by asset path. It validates provider snapshots,
checks revision stability and retains all authored dependencies. It rejects
recursion, missing dependencies, excessive depth/count/payload and incompatible
call signatures without replacing an earlier output closure. Memoized shared
subtrees retain their height so reuse cannot bypass the depth bound. Repeated
root calls count once against the distinct-dependency limit. Snapshot diagnostics
carry function paths and call chains. Expansion emits separate expression source
metadata with authored node/port identity, function path and invocation chain.

Root integration keeps live function-call records on `DMaterial`, alongside
parameter declarations, rather than adding asset pointers to `FMaterialProgram`.
`SetMaterialProgramAndFunctionCalls` validates and commits graph and call records
atomically; instances inherit the root records. Compiler snapshots and lifecycle
submission capture detached calls and the complete closure on the owning thread.
Unavailable dependencies fail the current request and retire its generation.
Authored parameter dependency queries traverse call bindings, including instance
override availability and existing editor parameter operations.

Expansion uses a separate bounded intermediate, so authored graphs remain limited
to 256 nodes while expansion admits up to 4096 nodes before pruning. Invocation
namespaces keep independent inputs separate; output GUIDs forward multiple values
without duplicating shared callee expressions. Nested connected textures preserve
the root parameter identity and its resource/sampler/fallback association. Absent
texture inputs use the enclosing typed default and fold fallback samples to their
constant color. Surface defaults expand through ordinary MakeSurface nodes.
GetSurfaceAttributes selects visible outputs by an eight-bit mask; adding or
removing other visible attributes never renumbers a retained connection.
SetSurfaceAttributes has one positional base-Surface input and a separate bounded
list of up to eight typed attribute bindings. This permits all eight overrides
without widening the ordinary eight-input signature table. Duplicate/unknown
attributes, invalid selections, incompatible sources and cycles are rejected in
root and function graphs. Lowering forwards Get expressions and reconstructs
MakeSurface for Set, preserving unmodified fields. No Surface accessor reaches
shader generation. Both payloads participate in link/payload/depth bounds and
reference traversal, and serialize with the graph.

Pruned IR determines active parameter declarations. Pure compiler input validates
the complete closure, including unreachable recursive dependencies.

Program schema is now 5, IR 4, generator 5 and compiler envelope 7. Functions keep
schema 1. PostLoad upgrades only bounded, valid schema 4 graphs containing legacy
built-ins and no function calls, preserving declarations and graph identities.
Asset loaders retain the resulting canonical-resave recommendation without
marking the package dirty. Older/unknown versions remain unsupported. Cook recipe
versioning belongs to Stage 2. No production PBR builder or source package has
been migrated by this tranche.

Root expansion validation: MaterialTests passed 162/162, receipt
`Build/.agent-state/logs/20260911-171925-444840-19192-MaterialTests.log`.
The subsequent function suite passed 11/11, including independent invocation
inputs, multi-output deterministic identities, pre-pruning bounds, nested texture
defaults, instance snapshots, missing references, root reference serialization
and schema 4 load upgrade with a retained resave recommendation. Receipt:
`Build/.agent-state/logs/20260911-172150-654899-35560-MaterialTests.log`.
AssetPackageTests passed 148/148 after retaining PostLoad resave recommendations;
receipt: `Build/.agent-state/logs/20260911-172217-762821-28000-AssetPackageTests.log`.
The default workspace `all` build passed after root integration and versioning;
receipt: `Build/.agent-state/logs/20260911-172324-208058-320-cmake.log`.

Stage 1 exit qualification: MaterialTests passed 166/166 with Surface Get/Set
serialization, selected-output stability, all-eight overrides, untouched-field
preservation, invalid type/attribute/cycle rejection and nested diagnostics.
The texture-function fixture composes a sampled root texture into Surface and
compiles real shader entry points through the existing layout with one resource.
Owning-thread closure diagnostics now identify the document containing the node
and retain the root invocation; detached pre-expansion validation recovers that
path too. Receipt:
`Build/.agent-state/logs/20260911-173511-907317-31644-MaterialTests.log`.
The final workspace `all` build passed; receipt:
`Build/.agent-state/logs/20260911-173655-841131-27664-cmake.log`.
This completes Stage 1 only. Dependency edit propagation, stale closure admission,
per-owner source-map lifecycle, asset-operation integration and Cook admission
remain Stage 2 work; authoring UI and production content migration are later gates.

Validation: MaterialTests passed 158/158 before the final function-only bounds
and reference-enumeration assertions; receipt:
`Build/.agent-state/logs/20260911-164437-513249-36716-MaterialTests.log`.
The updated function suite passed 6/6 with exact base-class reference metadata,
abstract creation rejection, save/reload, detached snapshot lifetime, port
rename/reorder/delete/retype, default cycles, recursion and shared-subtree depth.
Final function receipt:
`Build/.agent-state/logs/20260911-165031-012621-26700-MaterialTests.log`.
The default `all` build passed for the Engine, Sandbox and RoadWeaver workspace;
receipt: `Build/.agent-state/logs/20260911-165123-839524-32580-cmake.log`.
The registry expands shared Engine header changes to most targets; MaterialTests
and the complete workspace build cover this tranche. GPU qualification remains
required after function lowering changes executable shaders.

## Implementation Stages

### Stage 0: Freeze interfaces and migration inventory

- [x] Enumerate material/function change sites, import channel layouts, existing
  packages and M13 overlaps; record the actual storage and upgrade execution path.
- [x] Freeze StandardPBR ports/defaults, sampler precedence, stable pin schema,
  bounded expansion limits and schema/version changes in this plan.
- [x] Capture current representative imported-material render results and resource
  counts; include independent maps, packed maps, missing textures and custom UVs.

Exit: concrete interface tables, per-asset dispositions and baseline fixtures;
no unresolved schema/default decisions block Stage 1.

### Stage 1: Function assets and compiler expansion

Depends on Stage 0.

- [x] Implement the abstract DMaterialFunctionInterface asset contract and concrete
  DMaterialFunction graph owner; call references and snapshot/dependency queries
  use the base without concrete downcasts. Verify abstract asset creation is
  rejected and base-typed references survive serialization and asset operations.
- [x] Implement typed stable interfaces, call bindings,
  snapshot closure, bounded expansion, multi-output lowering and source maps.
- [x] Add Surface Get/Set and validate nested numeric, texture and Surface calls.
- [x] Test round trips, reordered/renamed/deleted ports, recursion, missing assets,
  expansion limits, deterministic output and independent invocation inputs.

Exit: a programmatic texture-to-Surface function compiles through existing layouts;
malformed calls diagnose correctly and no worker reads live assets.

### Stage 2: Dependency lifecycle and cooked execution

Depends on Stage 1.

- [ ] Integrate function references with invalidation, asset operations, shared
  compile work, accepted generations, cancellation and shutdown.
- [ ] Extend Cook dependency admission and cache identity/versioning.
- [ ] Test nested edits, stale completion, compile failure/recovery, relocation,
  deletion, cold/warm cache and cooked loading without authored functions.

Exit: dependency edits safely update all callers and Cook artifacts are sufficient
for runtime. Reconcile shared seams with M13 before either plan edits them again.

### Stage 3: Function authoring workflow

Depends on Stages 1 and 2.

- [ ] Generalize MaterialEditor graph documents and semantic commands for functions.
- [ ] Implement port editing, call insertion/navigation, default visibility,
  preview wrappers, Surface editing and nested diagnostic navigation.
- [ ] Verify Undo/Redo, clipboard, save/reload and multi-document dependency edits
  through both structured commands and human canvas workflows.

Exit: authors can build and reuse a function without source edits or raw asset
patches; interface changes retain stable connections and actionable diagnostics.

### Stage 4: Standard PBR library and content upgrade

Depends on Stages 1-3.

- [ ] Author standard functions, replace Import graph construction/validation,
  and generate intentional compact graph presentation.
- [ ] Upgrade the inventoried assets and preserve instance bindings and slots;
  remove retired expanded PBR production builders and migrate their fixtures.
- [ ] Verify fresh import, reimport, missing resource fallback, save/reload,
  modified assets and interrupted/repeated migration.

Exit: ImportedSurface has one standard function call with inputs and Surface
output, no expanded sampling/math chain; every inventoried asset is accounted for.

### Stage 5: End-to-end qualification and handoff

Depends on Stage 4.

- [ ] Run bounded CPU/editor/import/Cook suites selected from the native registry.
- [ ] Qualify migrated and newly imported materials on supported forward, GBuffer
  and shadow paths, including StaticMesh/SplineMesh and masked materials.
- [ ] Compare Stage 0 renders and texture/resource counts; inspect generated code
  to confirm packed-map reuse and no unexpected sampling expansion.
- [ ] Build the editor; document implemented contracts in MaterialSystem and
  MaterialGraphOperations, record receipts, and complete M11 in the roadmap.

Exit: all gates pass, migration is repeatable, edited function assets propagate
correctly, and no runtime dependency on function graphs or PBR GUID rules remains.

## Validation and Handoff

Follow [Testing](../Agents/Testing.md) and [Build And Run](../Agents/BuildAndRun.md).
Discover exact target ownership through the registry during Stage 0. MaterialTests,
scene-import tests and material Vulkan/Cook coverage are candidate lanes, not a
substitute for registry selection. GPU visual qualification is a required Stage 5
gate because template execution and asset content change. Unavailable GPU access
must be reported as outstanding, not counted as a pass. This proposal-only commit
requires documentation validation, not compilation.

Each implementation commit updates the stage checklist and evidence and uses the
repository Plan/Stage trailers. Persist lasting contracts in the owning Runtime
and Editor documents only after implementation.

## References

- [Material System](../Runtime/Rendering/MaterialSystem.md)
- [Material Graph Operations](../Editor/Architecture/MaterialGraphOperations.md)
- [Asset Packages](../Runtime/Assets/AssetPackages.md)
- [Asset Catalog and Mutation](../Runtime/Assets/AssetCatalogAndMutation.md)
- [UE Material Functions](https://dev.epicgames.com/documentation/en-us/unreal-engine/unreal-engine-material-functions-overview): typed texture inputs and reusable function assets.
- [UE Material Attributes](https://dev.epicgames.com/documentation/unreal-engine/material-attributes-expressions-in-unreal-engine): grouped Surface output and selective attribute access.

UE references inform the authoring model; this plan owns Durin-specific schema,
compiler, dependency, parameter-scope and migration decisions.
