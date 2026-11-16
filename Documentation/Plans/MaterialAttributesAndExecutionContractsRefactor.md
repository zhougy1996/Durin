# Material Attributes and Execution Contracts Refactor Plan

Summary: Replace the fixed surface-output contract with UE-aligned material attributes, explicit compilation requirements, and separate material evaluation and render-pass execution.

Last reviewed: 2026-11-17

Status: Active
Completed:

## Current Status

Stages 0-2 completed on 2026-11-16. Typed attributes, per-entry compilation
requirements and canonical Shader/mesh execution requests now span translation,
Cook and renderer admission. Pass templates own entry bodies, GBuffer packing
and lighting execution. The five minimum source assets have been regenerated
for the Stage 3 graph/function generation at their original logical paths.
Validation and content disposition are recorded below. Stage 3 is in progress:
Texture2D/Cube values now follow UE's base-texture reference and typed MIR
boundary, with all four sampling modes, instance replacement, function/editor
support, source-free DMAT and Vulkan pixel validation. PostProcessInput0 and
read-only SceneDepth now traverse authoring, detached compilation, source-free
Cook decoding and Renderer RDG planning/binding. The scene-input and RDG tasks
are complete. Windows Stage 3 qualification is complete on 2026-11-17,
including Cube face-local orientation and numerical explicit-LOD vertex WPO.
Production Metal compilation/reflection remains a required open gate, so
Stage 3 as a whole is not complete. Packaged Game/content closure and final
integrated cost qualification remain in Stages 5 and 6.
The owner authorized the Stage 4 DefaultLit Specular slice while retaining that
Metal gap. Specular implementation and Windows forward/deferred qualification
are complete on 2026-11-17. The following Emissive-to-SceneColor slice uses
three RGBA8 GBuffer records plus existing RGBA16F SceneColor, retaining both
octahedral normals. The ClearCoat continuation implements a shared forward/
deferred layer and optional ModelData; Windows qualification completed on 2026-11-17.
Display-chain PostProcess integration remains open.
See the implemented [material contract](../Runtime/Rendering/Materials/MaterialSystem.md).

The 2026-11-16 rebase review below preserves the selected architecture, content
disposition and Stage 0 status. Later editor work must consume the new toolkit
ownership/close contract. Historical measurements keep their actual tested
revision; the rebase is not recorded as a new runtime qualification.

This is M14 of the [Material System roadmap](../Roadmaps/MaterialSystem.md).
Existing contracts and assets may change. Prefer UE-aligned names and boundaries
where they fit Durin ownership. Asset compatibility is not an acceptance gate:
content that is awkward to convert may be replaced with newly authored assets.
Every retained consumer must nevertheless resolve to valid new content.

## Goal

A material declares its domain, shading model, typed attributes and resources.
The compiler produces immutable evaluation code, binding layouts and explicit
per-entry requirements. Renderer combines that result with pass and geometry
implementations without interpreting authored graphs. Adding a supported
attribute, shading model, resource type or domain has one explicit extension
path instead of requiring independent fixed-eight-output edits throughout the
asset, function, compiler, Cook and renderer stack.

Retain stable parameter identities where useful, typed spatial/stage semantics,
detached worker snapshots, shared ShaderBuild/DDC ownership, immutable render
publication, dynamic instances and source-free cooked execution. Their concrete
interfaces and formats may change together with affected consumers.

## UE Reference and Verified Starting Points

The inspected installation is `D:/Programs/UE_5.8`, version 5.8.2, changelist
56702186, branch `++UE5+Release-5.8`, as recorded in `Engine/Build/Build.version`.
These are local source references, not repository dependencies or portable links:

| UE source relative to that installation | Relevant boundary |
| --- | --- |
| `Engine/Source/Runtime/Engine/Public/MaterialDomain.h` | Material domain is separate from shading model and blend policy. |
| `Engine/Source/Runtime/Engine/Public/Materials/MaterialAttributeDefinitionMap.h` and `Private/Materials/MaterialAttributeDefinitionMap.cpp` | Stable attribute IDs, value types, defaults, shader frequency, and attribute-specific blending. |
| `Engine/Source/Runtime/Engine/Public/Materials/MaterialIRTypes.h` and `MaterialIR.h` | Typed values/resources and texture-read operations with explicit sampling modes. |
| `Engine/Source/Runtime/Engine/Public/MaterialShared.h` | Compilation output records uniform expressions, scene-texture use and material behavior; shader maps track shader and vertex-factory dependencies. |
| `Engine/Source/Runtime/Engine/Public/MaterialShaderType.h` and `MeshMaterialShaderType.h` | Material shaders and geometry-dependent material shaders have distinct contracts and permutation selection. |
| `Engine/Source/Runtime/Engine/Public/Materials/MaterialExpressionSubstrate.h` | BSDF composition is a separate, substantially larger capability than extensible numeric material attributes. |

Durin already has typed Shader/Material/Mesh maps and Vertex Factory descriptors
in RenderCore. Reuse them. Its current material producer still stores eight
surface inputs in `MIR::FModule::FSurfaceRoot`, generates fixed surface packing
and pass entry bodies in `MaterialProgramGenerator.cpp`, and requests six
entries through persisted ordinal bits. `FMaterialProgramRequirements` records
selected context needs but not a complete per-entry scene-resource contract.
The current GBuffer recognizes StandardLit, and authored material resources are
principally Texture2D. These are starting constraints, not new runtime rules.

## Selected Scope and Non-Goals

Required scope includes attribute/root schemas, function attributes, compilation
analysis, generated module/pass separation, shader-map entry selection, typed
resources, scene inputs, renderer bindings, editor commands/UI, import recipes,
Cook and content replacement. Extension proofs are DefaultLit Specular,
ClearCoat, TextureCube and a bounded PostProcess domain.

Surface initially supports Unlit, DefaultLit and ClearCoat. Rename the existing
Lit semantic to DefaultLit in the new contract. Preserve the existing
Opaque/Masked/Translucent baseline; ClearCoat is initially opaque/masked only.
Unsupported combinations fail explicitly. PostProcess is unlit fullscreen
color evaluation at one fixed insertion point before tone mapping, consuming
linear HDR input and optional read-only scene depth. It has no mesh dependency.

This plan does not implement Water optical rendering, Substrate/closure graphs,
arbitrary user shader source, runtime-polymorphic pass plugins, Decal/Volume/UI
domains, compute materials, ray tracing, virtual texturing, bindless indexing,
texture streaming, OIT, general refraction or a new uniform-expression VM.
Do not expose unimplemented enum choices as working features. A later domain
can use the new extension path without being preimplemented here.

## Target Interfaces and Data Ownership

Names below are the selected design vocabulary. Use Durin naming/reflection
conventions; UE binary layouts, enum ordinals and serialization are not targets.
Stage 0 freezes exact signatures, ownership and supported combinations before
source edits. Any departure from these decisions must be recorded first.

| Interface or value | Selected responsibility |
| --- | --- |
| `EMaterialDomain` | Authored purpose: Surface or PostProcess in this plan. Independent of blend mode, shading model, shader frequency and pass identity. |
| `EMaterialProperty` and `FMaterialAttributeDefinitionMap` | Engine-owned definitions with stable attribute GUID, name, value type, default, stage and spatial semantics, applicability, and explicit blend behavior. Resolve serialized IDs through validated definitions; never persist table positions. |
| `FMaterialAttributes` and `FMaterialAttributeBinding` | Bounded typed attribute collection and GUID-keyed root links/defaults. Material Attributes Make/Get/Set and function ports share these definitions. Root selection supplies defaults and validates domain/model applicability. |
| `MIR::FModule` | Detached typed expressions, attribute roots and source provenance. Replace fixed-eight `SurfaceRoot` and special-case WPO storage with property roots; WPO remains explicitly Vertex/World Direction. |
| `FMaterialCompilationOutput` | Immutable active attributes, compiled parameter/resource layouts, per-entry requirements, interpolators, behavior flags and supported execution combinations. Evolve existing requirement values instead of introducing a parallel authority. |
| Typed texture values and `FTextureRead` | Texture dimension/sample type independent from sampling mode; carry sampler policy and fallback. Initial reads support Implicit, Level, Bias and Grad where legal; Cube has Float3 direction coordinates. |
| `FMaterialSceneTextureRequirement` | Logical input, stage/pass availability, coordinate/depth/color semantics, filtering and required-versus-optional policy. Names refer to defined input snapshots, never arbitrary current render targets. |
| Existing material/mesh Shader maps and type descriptors | Exact entry keys using shader type, frequency, applicable Vertex Factory, target and required permutation. Replace persisted six-entry ordinal assumptions with canonical typed entry descriptions; retain compatible stage-artifact sharing and atomic map admission. |
| Material evaluation module and pass adapters | Generated code evaluates selected properties. Pass shaders own entry signatures, coverage/depth behavior and attachments; Vertex Factories supply geometry; shading-model modules interpret surface data. |
| Renderer GBuffer codecs and model data | Explicit versioned transport per supported model, with shared semantic inputs to forward and deferred shading. Attribute collections do not become a per-pixel dictionary. |

Definitions are deterministic engine-owned data, frozen before compilation.
This is not a mutable plugin registry. Separate semantic metadata from editor
presentation so workers and Renderer never consult reflected UI state.
Unknown IDs, duplicate bindings, invalid types and illegal domain/model/stage
combinations produce source-located diagnostics.

Attribute blending is semantic: normals require defined normalization/composition;
discrete selectors and incompatible resource types cannot silently use numeric
Lerp. Preserve defaults and disconnected authored intent. Numeric attributes
lower to compact typed shader structs; only applicable reachable values occupy
the compiled representation.

Engine owns authored semantics, normalization, material compilation output and
Cook assembly. RenderCore owns generic shader identities, maps and Vertex
Factory descriptors; ShaderBuild owns compilation and shader-artifact caching.
Renderer owns pass selection, RDG inputs, model codecs and resource binding.
MaterialEditor owns presentation over the shared graph command API. Extend
existing contracts rather than creating another scheduler, resource cache or
graph mutation API.

## Asset and Artifact Transition

Compatibility adapters are optional, disposable tooling, not runtime features.
The selected default is new-format content, with conversion only where it is
simple and useful. No dual old/new evaluator or legacy cooked loader is required.

Stage 0 inventories every base material, instance, function, collection, recipe,
default/error resource, preview, thumbnail fixture and material reference in
Engine, Sandbox, RoadWeaver and selected plugins, including Water. Classify each
as **convert**, **recreate**, or **remove with all references**. Record the old
logical identity, replacement identity, dependent packages and validation route.
Untracked or external user content is outside automatic deletion/rewrite scope;
document its explicit conversion or recreation requirement.

Prefer preserving package paths and compatible parameter/node/function-port
GUIDs when converting. Recreated content may use new paths and IDs: rewrite
parents, instance overrides, graph function calls, Level/mesh assignments,
soft defaults and Cook roots together. Do not blindly transfer overrides by
display name. Rebuild instances when the parent schema changes incompatibly.
Import recipes and asset-authoring utilities must create new-format content so
reimport or fixture generation cannot restore retired schemas.

Migrate minimal fixtures with each owning source stage; Stage 5 closes the full
inventory. Runtime may reject old authored schemas with an actionable recreation
diagnostic. Reject obsolete DMAT, MIR/translation-cache, shader-map/pass and
shader-library artifacts using all relevant identities/versions. Re-Cook from
the new content into an isolated generation; do not purge unrelated caches.
No runtime source compilation may compensate for missing cooked entries.

## Implementation Stages

Stages 0-2 have completed. Checkboxes require stage-specific evidence,
not completion of this planning document. Stage N depends on the prior stage
unless a narrower dependency is stated. One checkout has one source/build writer.

### Stage 0: Freeze contracts, inventory and qualification baseline

Outcome: an executable contract and content transition specification before any
production refactor. Authorized by the owner's Stage 0-only instruction.

- [x] Audit current source and test consumers in every project declared by `Durin.dworkspace`, plus plugins, editor and content generators. Confirm ownership against current code rather than old roadmap wording.
- [x] Freeze attribute IDs/types/defaults/applicability/blend semantics, root and function schemas, Domain/model/blend matrix, exact Shader entry keys, binding ownership and version transitions.
- [x] Freeze PostProcess input/output color semantics, insertion point, scene-depth convention, viewport/subrect coordinates, pass resource availability and failure behavior. Preserve current exposure semantics explicitly.
- [x] Freeze ClearCoat's bounded single-normal model, attribute defaults, direct/IBL equations, forward/deferred transport and precision; a separate coat normal is deferred. Freeze Specular mapping with a default reproducing current F0 = 0.04.
- [x] Complete the convert/recreate/remove inventory and choose representative authored fixtures for each retained behavior and extension proof.
- [x] Capture baseline images, shader entry counts, compilation/cache observations, material/descriptor sizes and GBuffer footprint. Record exact targets, revision, camera/light/time settings, backend, hardware, raw evidence paths and numerical image/performance budgets before changing production code.
- [x] Separate unchanged-content parity cases from deliberately recreated appearances. Record existing failures independently; old waived gates are not passes for this plan.

Exit: all decisions above are recorded, every content owner/reference category is
accounted for, and qualification budgets are numerical and reproducible. Open
choices must be resolved here before their dependent implementation starts.

### Stage 1: Replace fixed outputs with typed material attributes

Outcome: assets, functions, compiler roots and editor commands use one attribute
definition authority. Depends on Stage 0.

- [x] Implement Domain, property definitions, attribute bindings and aggregate values; replace fixed eight-field root indexing and the separate WPO root representation across normalization, codecs, identity and diagnostics.
- [x] Migrate Make/Get/Set attributes, function signatures/expansion, defaults, stage/space checks and static-selector reachability. Retain explicit VertexInterpolator semantics and bounded graph/function validation.
- [x] Update shared graph commands, root pins, canvas, preview, instance controls, Undo/Redo, clipboard and save/reload for active and inactive attributes. Domain/model changes must retain authored intent while rejecting invalid active combinations. Preserve the current ToolkitManager ownership, profile settings and close-time preview release described in the rebase review.
- [x] Update basic import/authoring recipes and convert or recreate the minimum DefaultLit, Unlit, masked, function, instance and WPO fixtures needed to keep development executable.
- [x] Verify deterministic serialization/identity, property order independence, default equivalence, unknown/duplicate IDs, illegal stages/types, attribute blend behavior, function diagnostics and transaction rollback.

Exit: no authoritative fixed-eight output array or positional serialization
remains in the new path. Existing surface behavior and WPO execute through the
new schema; resource/pass extensions may remain unavailable until later stages.

Stage 1 content refinement: recreate DefaultWater together with the four Engine
foundation assets. The component's saved soft default otherwise fails the new
graph version boundary, so it belongs to the minimum executable WPO consumer
set. Use the existing explicit Water authoring fixture with unchanged wave and
logical-asset identities; this adds no Water optical or renderer functionality.

#### Stage 1 implementation and acceptance receipt

Validated the Stage 1 working tree based on `766f38d30`, committed with this
receipt, using windows-msvc-x64 / Win64-Debug-DurinEditor, Debug, Tracy and
18 build jobs. Runtime/editor authorities are the linked
[material system](../Runtime/Rendering/Materials/MaterialSystem.md),
[expression building](../Runtime/Rendering/Materials/MaterialExpressionBuilding.md)
and [graph operations](../Editor/Architecture/Materials/MaterialGraphOperations.md)
contracts.

- One immutable twelve-property registry owns GUIDs, types, defaults, stages,
  spatial semantics, active-property selection and blend policy. Root bindings,
  aggregate values, function defaults and Get/Set selections use stable IDs.
  MIR 9 contains typed attribute roots including WPO; canonical root ordering
  is independent of binding order. Normal blending lowers to safe normalization
  after Lerp. Missing retained wiring, invalid selectors, unknown/duplicate IDs,
  incompatible types/stages and unsupported configurations reject atomically.
- Public aggregate/model names are MaterialAttributes and DefaultLit. Graph,
  output, function, translation, producer and DMAT versions follow the frozen
  Stage 1 table; clipboard schema is 10. ClearCoat and PostProcess identities
  are declared but execution remains unavailable. Specular/coat values retain
  authoring intent without execution bindings. Existing compact surface pass
  adapters remain for Stage 2; pass contract 13 and layout 4 are unchanged.
- Recreated DefaultMaterial, UVTransform, SampleNormal, SampleORM and DefaultWater
  at their original paths. DefaultMaterial retains its empty-root default
  appearance. `asset material-foundation` recreates the four Engine assets;
  the documented isolated Water authoring probe recreates DefaultWater and
  passed its CPU material/query comparison. The temporary fixture admission was
  removed and `Durin.dworkspace` restored byte-for-byte. Remaining examples and
  the full corpus/reference/Cook closure remain Stage 5 work; older graph
  versions reject rather than silently lose fields.
- Final workspace `all` passed; log
  `Build/.agent-state/logs/20261116-023840-099839-39672-cmake.log`.
  `@domain=material,kind=feature --report` passed all 13 targets; final CTest log
  `20261116-023715-948012-23512-ctest.log` in the same directory. The final owner
  compiler run passed 110 cases (`20261116-023601-972031-17136-MaterialCompilerTests.log`).
  Coverage includes functions/defaults/expansion, strict WPO typing, ID/order
  determinism, graph editing/Undo/Redo/clipboard, instances, persistence, Cook,
  preview close-time release and asynchronous publication/rollback. A native
  debugger identified a sequence-dependent fixture cleanup defect: the prebound
  proxy test now collects World children while their stack-owned Engine outer
  is still alive; the full runtime target passes all 99 cases in one process.
- SceneImportTests (34), RoadSceneIntegrationTests (5), AssetPackageReloadTests
  (22) and RendererSceneContractTests (173) passed. Their receipts are
  `20261116-022042-717270-34628-SceneImportTests.log`,
  `20261116-022126-937517-13172-RoadSceneIntegrationTests.log`,
  `20261116-022145-314972-18992-AssetPackageReloadTests.log` and
  `20261116-022226-564815-38984-RendererSceneContractTests.log` under the same
  log directory. RoadViewportQualificationTests compiled; its GPU qualification
  was not selected. DevTool asset Python tests passed 30 cases.
- VolumetricCloudSceneVulkanTests passed with `DURIN_LOCAL_LIGHT_CAPTURES`
  pointing to `Build/AgentValidation/MaterialAttributesAndExecutionContracts/Stage1/captures`.
  All nine Final/Forward/Local raw files for 0/1/4 lights are byte-identical to
  the Stage 0 baseline, including the explicitly converted Unlit fixture.
  Receipt: `20261116-022315-681728-4264-VolumetricCloudSceneVulkanTests.log`.
  WaterSurfaceRenderingVulkanTests passed both the independent CPU/GPU WPO proof
  and actual component scene after DefaultWater recreation; receipt
  `20261116-022948-429360-30716-WaterSurfaceRenderingVulkanTests.log`.
  Subsequent changes only tightened malformed authoring/editor admission and
  fixture cleanup; generated shading and captured scene inputs are unchanged.

Raw parity hashes, captures, source fingerprint and copied reports are under
ignored `Build/AgentValidation/MaterialAttributesAndExecutionContracts/Stage1/`.
No new timing qualification or native Metal GPU result is claimed. Stage 0's
known cold/warm binding-count failure remains recorded for Stage 2; it was not
reclassified as a pass. No Stage 2 execution-contract or later rendering feature
was implemented.

### Stage 2: Separate evaluation modules from Shader and pass execution

Outcome: one analyzed material module composes with explicit Shader/Pass/Vertex
Factory contracts. Depends on Stage 1.

Implementation clarification: per-entry `CollectionIDs` are separate from material
parameter IDs, so collection dependencies do not masquerade as material fields.
Built-in vertex requests use a shared pass-zero transport; renderer composition
adds the actual consuming pass/factory key. Explicit registered factory reuse and
fixed-source factories retain their existing opt-in behavior.


- [x] Move pass entry bodies, GBuffer packing and lighting execution out of the expression generator into owned shader modules/templates. Keep expression generation focused on typed attribute evaluation and resource reads.
- [x] Derive `FMaterialCompilationOutput` after normalization/static selection, with per-entry property closure, active parameters/resources, interpolators, behavior and context requirements. Shadow/hit-proxy entries request only needed property roots and vertex effects.
- [x] Replace six-bit persisted entry assumptions with typed canonical entry requests using existing RenderCore maps/descriptors. Select supported entries, preserve artifact reuse, and publish only complete compatible maps.
- [x] Version producer/runtime identities and migrate renderer planning, PSO precache, binding reflection, fallback, resource recovery and Cook stage admission together.
- [x] Qualify Local and Spline geometry, direct/indirect consumers, forward/GBuffer, masked/opaque shadows, hit proxies, previews and thumbnails. Include cancellation, stale completion, failed compilation and device-resource retry.

Exit: adding an evaluation property does not require adding pass bodies to the
generator; unsupported entry combinations fail deterministically. Inactive
branches and unrelated surface properties do not create per-entry bindings.

### Stage 2 implementation and validation receipt

Completed on 2026-11-16 from `5704379c0` plus the Stage 2 working tree.
The final source SHA-256 manifest, copied reports, raw CPU samples and captures
are under ignored `Build/AgentValidation/MaterialAttributesAndExecutionContracts/Stage2/`.
Profile: Win64-Debug-DurinEditor, MSVC 19.44, 18 build jobs; native Vulkan on the
same RTX 3090 host and settings as Stage 0. Subsequent edits are documentation only.

- Evaluation generation composes `SurfacePasses.slang` and
  `SurfacePassResources.slang`; their bytes are captured dependencies.
  `FMaterialCompilationOutput` is shared by detached translation and immutable
  shader maps. Per-entry closures cut interpolation boundaries, preserve WPO,
  and exclude unrelated surface parameters/resources from coverage entries.
- Typed requests persist stable Shader names and full existing RenderCore keys,
  not stage-mask ordinals. Translation revalidates analysis before consumer
  identity rebinding; Cook validates supported selections, complete keys and
  reflected resource subsets before publication. Collection schemas restore
  before Cook admission. Renderer/PSO composition preserves independent fixed
  factories and explicit built-in transport reuse; inactive opaque mask
  thresholds canonicalize. HitProxy cache uses the relevant entry contexts.
  New metadata capacity participates in existing retained-memory accounting.
- Authored graph/output/function/instance versions remain 6/4/3/2 and layout 4.
  MIR/pass/DMAT are 10/14/17, translation schema 6, normalization/generator
  revisions 10/18 and adapter 15. Prior compiled/Cook products require rebuilding.
  DefaultMaterial, UVTransform, SampleNormal, SampleORM and DefaultWater require
  no further source migration. Full corpus closure remains Stage 5.
- Negative tests cover missing/duplicate/unknown entries, target/pass/output ABI,
  factory identity, invalid requirement IDs/semantics/behavior and unavailable
  scene contracts. Masked Shadow/HitProxy cannot bind a BaseColor-only texture;
  a coverage-only build removes it from the module layout. Unrelated pixel Time
  reads do not create coverage/vertex context requirements. Cancellation tests
  now read owner state under its mutex and allow owner release after terminal
  publication; live subscriptions still retain distinct owners and complete once.

Passing receipts below are in `Build/.agent-state/logs/`:

| Coverage | Result | Receipt |
| --- | --- | --- |
| Workspace `all` | Passed, final shared API/consumer gate | `20261116-040249-382199-17304-cmake.log` |
| Material feature set | All 13 targets | `20261116-035544-045051-21432-ctest.log` |
| Final MaterialCompilerTests | 111 cases | `20261116-035948-343704-12292-MaterialCompilerTests.log` |
| Shader contracts / Cook composition | 72 / 1 cases | `20261116-035610-593121-37784-RenderShaderContractTests.log`, `20261116-035620-480897-27280-RenderShaderCookIntegrationTests.log` |
| Renderer scene / Scene import / Road scene | 173 / 34 / 5 cases | `20261116-035631-446101-3816-RendererSceneContractTests.log`, `20261116-035645-507550-14768-SceneImportTests.log`, `20261116-035719-390437-45420-RoadSceneIntegrationTests.log` |
| Local/Spline, direct/indirect, PSO, shadows, HitProxy | All 21 mesh Vulkan cases | `20261116-040014-504036-27704-StaticMeshRenderPreparationVulkanTests.log` |
| Resource reload/recovery | 2 Vulkan cases | `20261116-040026-160581-38416-RendererResourceReloadVulkanTests.log` |
| Preview/thumbnail / Water | 14 / 2 Vulkan cases | `20261116-040029-157053-33592-ThumbnailVulkanTests.log`, `20261116-040037-026717-46952-WaterSurfaceRenderingVulkanTests.log` |
| Final scene parity | 1 case; all 9 raw files equal Stage 0 and Stage 1 | `20261116-040557-217139-36852-VolumetricCloudSceneVulkanTests.log` |

Three serial quiet CPU runs selected StagedBuildWorkloadBaseline,
MaximumGraphLayoutLatency and ContractsStage0Snapshot with qualification mode
and one test slot. Raw 60-row compiler samples and 100 layout samples per run
are retained in `cpu-1` through `cpu-3`; receipts are
`20261116-040404-180432-31196-ctest.log`,
`20261116-040417-242577-20344-ctest.log` and
`20261116-040430-410783-17344-ctest.log`.
Layout medians were 12203/12340/12435 us and p95 13042/13636/13460 us.
Small miss/disk/retained medians ranged 492977-502356 / 15097-15983 /
3760-4117 us; function medians ranged 542532-544753 / 27731-28288 /
15026-15478 us. All frozen CPU budgets passed; warm modes compiled zero times.
The staged baseline intentionally retains its original single bulk submission
for equivalent-workload comparison; production sparse-entry work is separately
covered by compiler tests (6 cold entries, 0 warm, compatible vertex sharing).

Per-entry code bytes and descriptor counts exactly match Stage 0, including
Forward's 22 named descriptors. The old expectation of 21 is corrected against
that independent snapshot and its explicit set-0 LocalLights binding. The
ColdAndWarmCompilerBaseline and instance variant probe passed; final receipt
`20261116-040443-504733-42692-ctest.log`, full output `contract-cost.log`.
Total SPIR-V remains 182708 bytes; root DMAT is 190990 versus 188127 bytes
(+1.52%, including new contract metadata). Instance DMAT is 1561950 bytes;
4 effective identities remain shared, with 934796 retained bytes in this probe.
No new GBuffer attachment or scene copy is introduced. Native Metal GPU and GPU
timing qualification are not claimed; offline Metal compilation/reflection and
codec coverage remain in the passing owner tests. Stage 3 remains unstarted.

### Stage 3: Add typed sampling and explicit scene-resource contracts

Outcome: resource reads have validated types, stages and Renderer-owned inputs.
Depends on Stage 2.

- [x] Generalize material texture parameter values/layouts to support Texture2D and TextureCube, reusing existing texture asset/build/residency facilities. Migrate instance overrides, functions, reflection, fallback resources and cooked references.
- [x] Implement Implicit/Level/Bias/Grad read modes with dimensional coordinate validation. Explicit Level works in Vertex and Pixel; implicit derivatives, Bias and Grad are Pixel-only for this scope. Reject illegal combinations before Shader compilation.
- [x] Implement logical scene inputs for PostProcessInput0 and read-only SceneDepth, with declared filtering and conversion helpers. Do not expose an ambiguous writable current SceneColor.
- [x] Carry entry requirements through source-free Cook loading. Renderer material planning resolves them to counted resources, RDG declarations and pass bindings; unavailable required inputs fail locally without stale bindings.
- [x] Qualify Windows/Vulkan cube face-local orientation and mip selection, numerical vertex texture WPO, per-instance resource replacement, missing resources, scene UV/depth reconstruction, subrects, resizing and incompatible-domain rejection; inspect SPIR-V reflection. See the 2026-11-17 Windows receipt below.
- [ ] Qualify production Metal compilation/reflection for Texture2D/Cube sampling, explicit-LOD vertex entries and PostProcess scene-input layouts on the supported macOS toolchain. Windows MSL text inspection does not satisfy this gate; see the handoff below.

Exit: TextureCube and explicit sampling traverse authoring-to-Cook execution;
scene reads have validated lifetimes and no attachment read/write feedback.
Texture3D/Array and additional scene inputs remain future extensions.

### Stage 3 UE interface alignment decision

The scene-input continuation follows UE 5.8's `UMaterialExpressionSceneTexture`
(logical ID, coordinates and filtering) and ScreenPass texture/view-rect ownership.
Scene reads remain detached logical values; Renderer resolves graph-local inputs
and declares typed RDG reads with a distinct output. Stage 3 introduces the
minimal PostProcess compilation entry needed to validate these reads, while the
per-view effect slot and display-chain integration remain Stage 4 work.
Because the Cube slice published intermediate artifacts, scene-node payloads and
domain execution advance graph/function to 9/6, MIR/translation to 13/9,
normalization/generator/adapter to 13/21/18, pass/DMAT to 17/20. Stage 4 must use
subsequent generations when its representations change. Layout stays at 5.

The 2026-11-16 continuation explicitly prioritizes UE alignment for primary
interfaces and data structures. UE 5.8.0's MaterialExpressionTextureBase uses
`TObjectPtr<UTexture>`, while MaterialIRTypes distinguishes Texture2D and
TextureCube. Adopt the same boundary: reflected material values reference
`DTexture`; detached declarations carry an explicit texture dimension, validated
against the asset class, MIR type and compiled layout. This supersedes the frozen
checked-variant storage choice below. It does not permit unchecked downcasts,
unknown dimensions or replacement of a 2D declaration with a Cube override.
Sampler policy and read mode remain independent from resource dimension.

The first sampling slice already published intermediate Stage 3 artifacts.
Changing the resource value and declaration representations must reject those
products as well. Advance graph/function to 8/5, instance to 3, MIR/translation
to 12/8, normalization/generator/adapter to 12/20/17, pass/DMAT to 16/19 and
layout to 5. The scene-input continuation subsequently consumed
MIR/translation 13/9, normalization/generator/adapter 13/21/18 and pass/DMAT 17/20.
The Stage 4 Specular generations are therefore MIR/translation 14/10,
normalization/generator/adapter 14/22/19 and pass/DMAT 18/21, as in the table below.
Minimum source content is regenerated in the same change. Historical receipts
remain associated with their original generations.

### Stage 3 incremental implementation

The 2026-11-16 continuation uses UE source at
`E:/Programs/Epic Games/UE_5.8`. Its `Engine/Build/Build.version` reports 5.8.0,
CL 53629095, branch `++UE5+Release-5.8`. The older installation and qualification
measurements above remain historical records, not evidence for this installation.
`MaterialExpressionTextureSample.h` supplies the source comparison for mip mode
and derivative operand ownership.

The first Stage 3 slice adds Implicit/Level/Bias/Grad to existing Texture2D
sample expressions and detached `MIR::FTextureRead`. Only selected operands
enter MIR; inactive authored inputs retain their links/defaults. Level permits
Vertex/Pixel, while Implicit/Bias/Grad require Pixel. Coordinate, LOD and gradient
signatures validate before source generation. Cache/canonical identities include
the mode, and translation records carry the typed immediate. The graph's shared
ReplaceExpression command edits mode through existing Undo/Redo ownership.

This slice starts the frozen Stage 3 graph/function, MIR/translation,
normalization/generator/adapter and pass/DMAT generations (7/4, 11/7, 11/19/16,
15/18). Instance and resource-layout versions remain at their Stage 2 values
until their Stage 3 representation changes land. Existing source material/function
packages require recreation before loading. DefaultMaterial, UVTransform,
SampleNormal, SampleORM and DefaultWater have been regenerated from their current
recipes at their existing paths. Full corpus/source-free Cook acceptance remains
open. No Stage 3 checkbox is closed
by this slice. Cube, Renderer scene-resource planning, Vulkan pixel/mip checks
and backend reflection acceptance remain required.

#### Sampling slice validation receipt

Validated the working tree based on `440e84730787bbe8c301b8eb69bbe8008d767a05`
on 2026-11-16 with windows-msvc-x64 / Win64-Debug-DurinEditor, MSVC 19.44,
Debug, Tracy and 14 build jobs. The final source/test/asset SHA-256 manifest is
`Build/AgentValidation/MaterialAttributesAndExecutionContracts/Stage3/source-sha256.json`.

- `test @domain=material,kind=feature --report` passed all 13 targets after
  regenerating the five minimum assets. Final CTest receipt:
  `Build/.agent-state/logs/20261116-132639-898308-39860-ctest.log`;
  build receipt `20261116-132546-767388-39860-cmake.log` in the same directory.
  Coverage includes all four read modes, coordinate/stage/operand rejection,
  distinct Level/Bias identities, detached translation round-trip, real Vulkan
  Shader compilation and per-entry texture reflection, Level WPO in both Local
  and Spline entries, graph-stripped DMAT encode/decode, inactive-input retention,
  mode Undo/Redo/clipboard, package round-trip, functions, instances, lifecycle,
  preview and thumbnails. This is compilation/reflection and CPU contract evidence,
  not cube/mip pixel or native GPU execution evidence.
- Updated StandardMaterialFunctions' sample construction and the catalog pin
  matrix for the additional retained operands. Initial feature failures exposed
  these consumers and obsolete minimum source packages; those failures are
  corrected, not waived. Unbound function texture fallbacks validate selected
  operands before constant folding. The frozen texture compatibility fixtures
  remain unchanged.
- Recreated the four Engine assets with `asset material-foundation --project
  Sandbox/Sandbox.dproject`. Engine's executable BaseModules entry cannot serve
  as a dynamic asset-tool project module; the successful Sandbox host required
  its current module binary. The earlier failed host invocations wrote no assets.
- The isolated Water author-material/material probe rebuilt DefaultWater and
  passed its CPU wave/material/query comparison. Receipts are `water-author.log`
  and `water-material.log` under the Stage 3 evidence root. Its temporary fixture
  admission was removed and `Durin.dworkspace` restored byte-for-byte. Only the
  Editor fixture/plugin targets were built; no Game/Cook or Water GPU pass is
  claimed from this check.
- Workspace consumer checks passed: RoadSceneIntegrationTests (5 cases), receipt
  `20261116-132813-930683-20620-RoadSceneIntegrationTests.log`, and
  SandboxGameplayTests (13 cases), receipt
  `20261116-132825-704278-41444-SandboxGameplayTests.log`, both under
  `Build/.agent-state/logs/`. Their builds refreshed the current consumer binaries.
  The changed-document validator passed for both updated documents.
- The first sandbox build hit the workflow's MSVC C1902 PDB-service error;
  subsequent builds ran outside the sandbox. No GPU timing, numerical parity
  qualification, new-mode Metal reflection, full workspace build or final
  source-free Game acceptance is claimed. These remain with the relevant stage
  and final acceptance gates.

#### Cube resource continuation and validation receipt

The continuation based on `43289f3df9644d34f46a01f2003bc2986da575f2`
implements the UE-aligned decision above.
`FMaterialTextureValue` retains `TObjectPtr<DTexture>` and a checked dimension;
public texture setters/getters use the base type. Declarations, function ports,
MIR, compact layouts, reflection, Cook schemas and Renderer binding retain the
dimension. Null values preserve Cube identity. Value-only and instance setters
reject dimension changes. Cube samples use Float3 directions/gradients and
reject normal-map usage and flat-normal fallback. Renderer supplies typed
white/black cubes and applies independent W addressing. Existing texture build,
residency and reference publication remain authoritative.

Validated on 2026-11-16 with windows-msvc-x64 / Win64-Debug-DurinEditor,
MSVC 19.44, Debug, Tracy and 14 build jobs. Raw Cube sources, MSL inspection,
Water receipts and the source/test/asset SHA-256 manifest reside under
`Build/AgentValidation/MaterialAttributesAndExecutionContracts/Stage3/Cube/`.

- All 13 material feature targets passed. CTest receipt:
  `Build/.agent-state/logs/20261116-175032-895327-5756-ctest.log`.
  This includes Cube graph/catalog pins, Undo/Redo, functions and preview wrappers,
  null Cube package reload with W addressing and gradients, all four modes in
  both dimensions, detached translation and graph-stripped DMAT round-trips,
  shader identities/reflection, instance validation and lifecycle behavior.
  The final equivalent parameter-definition simplification was rebuilt and the
  focused texture-read suite passed (`20261116-175207-557758-35880-MaterialCompilerTests.log`).
- Workspace `all` passed after the shared interface migration; receipt
  `Build/.agent-state/logs/20261116-180032-149207-43532-cmake.log`.
  This broad build covers foundational reflected headers and cross-project
  consumers that owner tests alone cannot establish.
- The new bounded Vulkan Cube test passed, checking six axis-aligned faces,
  two explicit mip levels, dynamic instance resource replacement, typed null
  white/black recovery and rejection of a 2D replacement/flat-normal fallback.
  The full StaticMeshRenderPreparationVulkanTests target passed all 22 cases;
  receipt `20261116-175453-320083-9384-StaticMeshRenderPreparationVulkanTests.log`.
  Level WPO is compiled/reflected for Local and Spline entries; this is not a
  numerical WPO pixel-displacement proof or face-local corner-orientation proof.
- Migrated consumers passed: SceneImportTests (34), SceneImportVulkanTests (2),
  RenderShaderContractTests (72), RoadSceneIntegrationTests (5) and
  SandboxGameplayTests (13). Their receipts under `Build/.agent-state/logs/`
  are `20261116-175534-371523-44736-SceneImportTests.log`,
  `20261116-175611-892854-19812-SceneImportVulkanTests.log`,
  `20261116-175753-786725-26264-RenderShaderContractTests.log`,
  `20261116-175831-584628-6976-RoadSceneIntegrationTests.log` and
  `20261116-175848-787838-42044-SandboxGameplayTests.log`.
  `affected --explain` expanded to all tests because shared fixture headers and
  runtime assets changed. The bounded material, Shader, Renderer, import and
  cross-project selections cover the changed behavior; unrelated engine services
  were not selected. Later LF normalization changes source bytes without changing
  semantics; passing results above remain applicable to those normalized inputs.
  RendererResourceReloadVulkanTests also passed both cases after normalization;
  receipt `20261116-180029-192392-46076-RendererResourceReloadVulkanTests.log`.
- Recreated the four Engine assets and DefaultWater at their original paths.
  The isolated Water author/material comparison passed; its temporary workspace
  admission was restored byte-for-byte. No Water Game/Cook or GPU result is
  claimed from that CPU probe.
- Offline SDK Slang generated MSL for all four Cube fragment modes. Inspection
  found `texturecube<float>` at texture 4 and its sampler at 2 in every entry;
  sources and `metal-source-inspection.json` preserve that evidence. This is
  supplemental MSL inspection, not production Metal reflection or execution.
  The older SDK frontend rejected vertex `SV_StartInstanceLocation`; native
  Metal compilation/reflection and WPO qualification require the supported
  macOS toolchain. No shader semantics were changed to bypass that limitation.
- Changed-document validation and `git diff --check` passed for the final change.

The first two Stage 3 implementation tasks are complete. Scene inputs, resource
planning/RDG lifetimes, subrect/resizing/depth proofs, native Metal acceptance
and complete source-free Game/Cook execution remain open. The earlier sampling
receipt is historical and does not claim these continuation inputs.

#### Scene inputs and RDG continuation receipt

Validated the working tree based on
`716f05246c399f402688d1be0e8bc4f780d53dd7` on 2026-11-16, using
windows-msvc-x64 / Win64-Debug-DurinEditor, MSVC 19.44, Debug, Tracy and 14 jobs.
The implemented contract is [Material scene inputs](../Runtime/Rendering/Materials/MaterialSceneInputs.md).
UE comparison used this session's local 5.8.0 installation, CL 53629095,
`MaterialExpressionSceneTexture.h`, `MaterialIRTypes.h` and Renderer `ScreenPass.h`.

- Logical IDs, coordinates and filtering are reflected authoring values; MIR
  retains typed scene-read payloads. Domain policy has one shared configuration
  validator. Execution legality follows static selection/inactive-root removal,
  including function calls. PostProcess compilation emits one fragment entry
  without mesh/VF/lighting requests. Grouped and per-stage requests agree.
- Source-free DMAT retains canonical entry requirements, including Input0 alpha
  preservation. Cooked admission checks the package root's domain. Renderer
  resolves graph-owned texture/view-rect values, validates descriptors and
  projection rows, declares exact reads and a separate raster output, and builds
  counted views/samplers/uniform bindings inside resolver authority. Foreign
  graph handles, missing inputs, zero rectangles and invalid projections reject.
- All 13 material feature targets passed, including scene authoring/package
  reload, functions inheriting caller-domain legality, inactive-root selection,
  reflection, translation and graph-stripped DMAT. Receipt:
  `Build/.agent-state/logs/20261116-191314-889436-46344-ctest.log`.
- The isolated MaterialSceneTexturesVulkanTests target passed on NVIDIA GeForce
  GTX 1060 6GB / Vulkan 1.4.312. It decodes Game-profile Cook payloads, shuts down
  ShaderBuild before GPU execution, then verifies six material variants across
  two input sizes, subrects, Point/Linear differences, boundary clamping,
  NormalZ/ReversedZ oblique reconstruction, actual sampled texel centers,
  no-geometry validity and alpha preservation. Missing/foreign/zero inputs and
  injected allocation failure publish no stale output; subsequent planning and
  draws succeed. Final receipt:
  `Build/.agent-state/logs/20261116-191920-549506-42376-MaterialSceneTexturesVulkanTests.log`.
  The final log contains no Vulkan validation errors. This is source-free program
  execution in the Editor test host, not a packaged Game executable run.
- Workspace `all` passed for the new shared material/RDG declarations and
  reflected schemas; receipt
  `Build/.agent-state/logs/20261116-191829-556061-30064-cmake.log`.
  Recreated DefaultMaterial, the three standard functions and DefaultWater at
  their existing paths. Water author/material CPU comparison passed; temporary
  fixture admission was restored byte-for-byte. Raw Water logs and final
  source/test/asset SHA-256 inventory are under
  `Build/AgentValidation/MaterialAttributesAndExecutionContracts/Stage3/Scene/`.
- Affected analysis selected broad module consumers. The bounded material feature
  set, isolated native scene proof and workspace build cover this continuation.
  At the user's request to finish quickly, no further GPU suites, performance
  qualification, native Metal reflection, packaged Game or full corpus Cook
  were added. The remaining Stage 3 qualification checkbox stays open, including
  earlier Cube corner-orientation/numerical WPO and native Metal gaps. Stage 4
  still owns the per-view effect slot and final display-chain integration.

#### Windows qualification closure and Metal handoff on 2026-11-17

Validated `a49e474ac31e381a1d5dfc59ceaf6f91f38659cb` plus this test change
using windows-msvc-x64 / Win64-Debug-DurinEditor, MSVC 19.44, Debug, Tracy
and 18 build jobs. GPU: NVIDIA GeForce RTX 3090, Vulkan 1.4.351;
validation layer from Vulkan SDK 1.4.357.0. This is correctness qualification,
not a new timing measurement or a relabeling of earlier hosts' receipts.

- Extended `CubeMaterialFacesMipsInstanceReplacementAndFallbacks` with an
  independently specified inverse cube projection and four distinct face-local
  texels. All 24 face/quadrant directions pass, in addition to the existing
  six face centers at two mip levels, instance replacement, white/black missing
  resource recovery and incompatible-dimension/fallback rejection.
- Added `ExplicitTextureLODVertexDisplacementMatchesNumericalDepth` for both
  Texture2D and Cube. Known blue/zero mip data drives WPO at time 0/1 under
  identity view/projection. GPU depth at four interior samples across indexed
  and non-indexed Local geometry matches independent world-z expectations
  0.5/0.75 within the frozen 1e-4 position budget; all 32 recorded depth
  observations have zero absolute error on this host. Dynamic mip/time updates
  retain the accepted material program and render-proxy program identity.
  This is a Local vertex numerical proof; existing Local/Spline compilation
  and renderer regression coverage remains separately recorded above.
- Full `StaticMeshRenderPreparationVulkanTests` passed all 23 cases; receipt
  `Build/.agent-state/logs/20261117-000726-838305-16296-StaticMeshRenderPreparationVulkanTests.log`.
- `MaterialCompilerTests` with
  `FMaterialTextureReadTests.*:FMaterialSceneTextureTests.*` passed all three
  cases. Both dimensions and all four pixel read modes, explicit Level vertex
  stages, illegal stage/domain/filter rejection, detached translation,
  graph-stripped Game-profile DMAT and SPIR-V resource reflection are covered.
  Receipt: `Build/.agent-state/logs/20261117-000807-360628-27164-MaterialCompilerTests.log`.
- `MaterialSceneTexturesVulkanTests` passed its source-free scene-input/RDG
  case on this host, including filtering, subrects, resizing, both depth
  conventions, alpha preservation and local failure/retry behavior. Receipt:
  `Build/.agent-state/logs/20261117-001009-023892-5148-MaterialSceneTexturesVulkanTests.log`.
  ShaderBuild is shut down before GPU execution. This remains execution in the
  Editor test host, not packaged Game qualification.
- Raw 96x96 float depth captures for all eight dimension/mip/time combinations,
  numerical observations, copied receipts and source hashes are under ignored
  `Build/AgentValidation/MaterialAttributesAndExecutionContracts/Stage3/Windows/`.
  Set `DURIN_MATERIAL_STAGE3_EVIDENCE_DIR` to an existing output directory when
  reproducing the mesh test to retain the depth captures. No runtime, asset,
  format or shared-header input changed; `affected --explain` selects only the
  mesh Vulkan target. The compiler and scene targets additionally refresh the
  Windows Stage 3 contracts. No new workspace `all`, full corpus Cook, packaged
  Game, application startup or performance qualification is claimed here.
  The initial sandbox build failed with MSVC C1902; the same launcher outside
  the sandbox built successfully, without source/toolchain workarounds.

Remaining Metal acceptance is explicit and is not waived by this Windows work:

- Use the supported macOS editor shader toolchain to compile the production
  Metal output and inspect native reflection for Texture2D/Cube and sampler
  dimension/count/index agreement, all four pixel sampling modes, explicit
  Level vertex sampling for Local/Spline entries, and the no-mesh PostProcess
  Input0/depth entry with its counted resources and layout.
- `MetalShaderQualificationTests` is macOS/editor-only and is excluded from
  this Windows registry. Its material fixture still contains the retired
  `EMaterialShadingModel::Lit` spelling and baseline-only variants in
  `MetalMaterialQualificationTests.mm`; migrate it to DefaultLit and the
  canonical selected entry contracts, and add the Stage 3 resource/domain
  matrix before claiming native Metal coverage. Merely rerunning the old
  fixture cannot close the gate.
- Earlier offline MSL inspection is supplemental only; its older frontend's
  vertex `SV_StartInstanceLocation` rejection remains historical. Production
  Metal compilation/reflection was not run here. Native Metal GPU pixel tests
  remain additional coverage under the acceptance policy, not a replacement
  for the required compilation/reflection gate.

Stage 3 stays in progress solely for this backend acceptance gate. This receipt
does not silently relax Stage 4's dependency or the later packaged Game/content,
final workspace build and numerical cost gates.

### Stage 4: Prove shading-model and domain extension

Outcome: new attributes, a second Lit model and a non-mesh domain execute without
reintroducing fixed Surface or geometry assumptions. Depends on Stage 3.

2026-11-17 ClearCoat implementation decision: the owner authorized the next
Windows slice while retaining native Metal acceptance. Use the frozen single-normal
GGX/Schlick layer equations below and exactly bypass the layer when weight is zero.
ModelData is optional RGBA8: R=coat weight, G=clamped coat perceptual roughness,
B/A=zero. It is MRT4, following existing SceneColor MRT3. Low model bits are
1=DefaultLit and 2=ClearCoat; bit 7 retains authored Specular identity. Views with
eligible ClearCoat use one common five-output render-pass layout for all participating
draws; shader outputs and PSO layouts participate in identity. Default-only views
keep four MRT outputs and 12 GBuffer bytes/pixel; coat views use 16, excluding
SceneColor/depth. Reject unsupported attachment counts and translucent ClearCoat;
never silently select DefaultLit. Preserve both octahedral normals, base specular-AA,
Emissive ownership and authored asset versions. Advance MIR/translation to 15/12,
normalization/generator/adapter to 15/24/21 and pass/DMAT to 20/23; layout stays 5.

ClearCoat precision refinement before qualification closure: retaining the existing
RGBA8 dual-octahedral normals means a coating lobe can amplify their permitted
normal error. The initial off-axis metallic c=1/rc=0.4 sample differed by 0.00513
between paths (the old absolute floor was 0.005), while every codec channel
remained within its frozen bound. Do not silently widen that floor or change
normal storage. For this new model, compare each GPU path to independent CPU
lighting using that path's actual resolved inputs: full forward values versus
captured/decoded GBuffer values. Keep <=max(0.005,0.01*abs(reference)) residual,
<=1/510 model channels, <=1 degree normals, and exact c=0 DefaultLit identity
within the same transport. Also check the forward/deferred difference against
the independently predicted transport difference, with <=0.005 residual, and
retain raw HDR/CSV evidence. This refines the new ClearCoat cross-path gate below;
it does not alter DefaultLit baseline gates or promise image equality beyond
existing compact transport precision.

2026-11-17 follow-up decision: the owner authorized Emissive-to-SceneColor
alignment before ClearCoat. Eligible opaque/masked Base Pass draws write linear
HDR Emissive and alpha one directly to the existing RGBA16F SceneColor alongside
three RGBA8 GBuffer targets. Keep both octahedral normals. GBuffer costs become
12 color bytes/pixel, excluding the already-owned SceneColor and depth. Bootstrap
loads this color seed; production deferred lighting adds only direct/indirect
lighting with RGB additive blending and preserves destination alpha. It never
samples its own SceneColor attachment. Forward/Unlit/translucent composition
continues to add Emissive once. Isolated lighting and Emissive/MaterialInputs debug may read the
seed before production writes; only explicit debug requests allocate a debug
output. Remove the dedicated Emissive texture and ordinary downstream reads.
Preserve current authoring limits and exposure semantics. Publish generator/
adapter 23/20, pass/DMAT 19/22 and translation schema 11; MIR/normalization remain
14 and layout remains 5. Later representation changes require later generations.
The Stage 3/4 Metal gates stay open; this owner-authorized slice does not waive them.

2026-11-17 scope decision: the owner explicitly authorized the DefaultLit
Specular slice on Windows while retaining Stage 3's Metal acceptance gap.
This is a bounded exception to the prior-stage dependency, not completion or
waiver of Metal qualification. ClearCoat and display-chain PostProcess remain
outside this slice. The owner subsequently requested UE-aligned GBuffer packing.
Local UE 5.8.2 (CL 56702186) `DeferredShadingCommon.ush` and `GBufferInfo.cpp`
store Metallic/Specular/Roughness in GBufferB RGB. Supersede the frozen extra-attachment rule for Specular: retain
four targets, pack BaseColor/AO together, and Metallic/Specular/Roughness/model
flags together. Opaque/masked effective opacity is always one and needs no stored
channel. Keep Durin's existing two octahedral normals and HDR emissive target.
A high transport flag distinguishes authored Specular from the exact default
0.5, preserving baseline lighting without midpoint UNorm8 bias; this is an
explicit Durin compatibility detail, not UE's binary layout. The optional fifth
ModelData attachment remains reserved for later ClearCoat, outside this slice.

- [x] Add DefaultLit Specular with the Stage 0 mapping and compatibility default; qualify direct and environment lighting in Windows forward/deferred paths. Metal qualification remains open under the owner's bounded continuation decision.
- [x] Align Emissive with SceneColor before ClearCoat; qualify single application, HDR/alpha/exposure, debug, RDG ordering and failure recovery on Windows while retaining both octahedral normals and the Metal gap.
- [x] Add the selected ClearCoat model, coat weight/roughness attributes, model identity and explicit GBuffer encode/decode. Share surface semantics across forward and deferred; qualify zero/full coat and roughness endpoints and reject translucent ClearCoat. Windows-qualified; native Metal acceptance remains open.
- [ ] Implement one ordered PostProcess material slot before tone mapping with PostProcessInput0 and optional depth. A neutral material preserves input; a color transform and depth visualization demonstrate actual execution. No mesh Vertex Factory or surface-lighting resources are requested.
- [ ] Define domain-appropriate preview and failure behavior: a failed PostProcess compile retains a complete last-known-good generation while pending, then bypasses the effect with diagnostics on failure; never bind a mesh ErrorMaterial to a fullscreen domain. Validate the selected publication policy against existing lifecycle contracts.
- [ ] Verify domain/model switches, parameter-only updates without compilation, graph Undo/Redo/save/reopen, cooked Editor/Game parity and distinct shader identities for semantically different contracts.

Exit: all three extension proofs produce independently checked pixels and valid
Cook output. ClearCoat and PostProcess unsupported paths are explicit, without
silently falling back to DefaultLit or Surface.

#### ClearCoat implementation and Windows receipt

Validated `26fcff65f` plus this change on 2026-11-17, windows-msvc-x64 /
Win64-Debug-DurinEditor, MSVC 19.44, Debug, 18 build jobs; NVIDIA GeForce
RTX 3090, Vulkan 1.4.351, SDK 1.4.357.0 validation layer.

The frozen single-normal layer is shared by forward and deferred direct/local/IBL
helpers. ClearCoat activates its two scalar attributes only in consuming pixel
entries and rejects translucent configurations. Material compiler tests prove
entry parameter reachability, model/output identity, masked admission, inactive
parameter pruning and graph-stripped Win64 Game-profile DMAT round-trip.

Default-only views retain three RGBA8 records plus existing RGBA16F SceneColor;
coat views add optional RGBA8 ModelData at MRT4. All participating draws use one
matching view layout, including DefaultLit in a mixed view. Shader/PSO identity
and automatic precache cover the four/five-target variants. ModelData stores
weight/roughness in R/G, B/A zero; flag model 2 and authored-Specular bit 7 are
independent. Both octahedral normals and Emissive-to-SceneColor remain intact.
AO/contact/debug accept both Lit model identities without requiring coat channels.

CPU closed-form checks and real Vulkan HDR pixels cover c=0/0.5/1, roughness
0.045/0.1/0.4/1 and metallic 0/1 under directional, point, spot and environment
lighting; separate oblique CPU goldens exercise Fresnel attenuation and AO. A named
forward-only qualification factory exercises opaque/masked forward shader output
without admitting translucent coat or introducing a production fallback. Zero
coat equals DefaultLit byte-for-byte in each matching transport. Mixed-view
DefaultLit pixels remain unchanged. Captured ModelData and normals determine
independent deferred references under the precision refinement above; complete
forward values determine forward references. Parameter updates retain accepted
program identity. Raw HDR/record captures, numerical CSV and receipts live in
ignored `Build/AgentValidation/MaterialAttributesAndExecutionContracts/Stage4/ClearCoat/`.

Cache generations are MIR/translation 15/12, normalization/generator/adapter
15/24/21 and pass/DMAT 20/23. Layout 5 and authored custom versions are unchanged;
derived programs require rebuilding/recook, without source asset recreation.

All Windows receipts below are under `Build/.agent-state/logs/`:

| Validation | Result | Receipt |
| --- | --- | --- |
| `StaticMeshRenderPreparationVulkanTests` | All 26 cases; shared lighting, mixed layout, emission/WPO regression, PSO ownership and ClearCoat opaque/masked precache | `20261117-021124-655111-33120-StaticMeshRenderPreparationVulkanTests.log` |
| `@domain=material,kind=feature --test-jobs 2` | All 13 targets; source-free Cook/Game-profile decode, authoring, instances, compiler and oblique CPU goldens | `20261117-021722-265036-41832-ctest.log` |
| `RenderShaderContractTests` | All 73 cases, including optional ModelData slot 20 and no production SceneColor seed sampler | `20261117-015319-909779-26556-RenderShaderContractTests.log` |
| `EditorRenderingTests` | All 93 cases, including model codec, normals and four/five MRT byte budgets | `20261117-021825-010688-14996-EditorRenderingTests.log` |
| `RendererSceneContractTests` | All 173 cases; optional attachment metadata and producer/resource contracts | `20261117-021838-397289-30856-RendererSceneContractTests.log` |
| `VolumetricCloudSceneVulkanTests` | Offscreen/Present, resource retry, graph schedules and nine Stage 0 captures within frozen precision | `20261117-021148-265879-38804-VolumetricCloudSceneVulkanTests.log` |
| `EditorGridVulkanTests` | All 7 cases; resource generation/device invalidation/recovery and Present/resize | `20261117-021205-325067-47212-EditorGridVulkanTests.log` |
| Workspace `all` | Engine, Sandbox and RoadWeaver compile/link integration passed for shared material/MRT/mesh headers | `20261117-021209-947982-41828-cmake.log` |

The 576 HDR observations have maximum model residuals 0.000218958 directional,
0.000239760 point, 0.000222743 spot and 0.000126205 IBL. Raw cross-path differences
reach 0.00537109; after the independently predicted transport difference, the
maximum residual is 0.000422. Both the unchanged channel bounds and the refined
ClearCoat precision gate pass. Six of nine Stage 0 captures are byte-identical;
the other three each differ in one component: one SDR code value, or one HDR half
ULP (maximum absolute HDR difference 0.0000610352). Alpha is identical. Preserve
these measured differences rather than claiming all baseline bytes are unchanged.

Affected analysis selects broad Engine/Renderer consumers. The bounded material,
shader, renderer, changed GPU routes and workspace build cover this continuation.
The shader receipt remains applicable because shader sources, reflection fixtures,
compiler/environment and relevant material compile inputs did not change afterward.
Final GPU receipts contain no Vulkan validation errors. No timing qualification,
full corpus Cook or packaged Game executable is claimed. Native Metal compilation/reflection
and GPU acceptance remain open: migrate the macOS qualification fixture first,
then verify coat attribute resources, five MRT outputs/capability admission,
RGBA8 codec, preserved normals, shared lighting, c=0 identity and model switching.
Windows SPIR-V and source-free execution do not waive these native backend gates.
PostProcess display integration, remaining editor/content/Game closure and final
integrated cost/timing qualification remain later work; Stage 4 is not complete.

#### Emissive-to-SceneColor implementation and Windows receipt

Validated `01211743f` plus this change on 2026-11-17 on windows-msvc-x64 /
Win64-Debug-DurinEditor, MSVC 19.44, Debug, 18 build jobs, NVIDIA GeForce
RTX 3090 and Vulkan 1.4.351 with SDK 1.4.357.0 validation layers.

- Base Pass writes nonnegative linear HDR emission and alpha one to existing
  RGBA16F SceneColor; background retains view clear color/alpha. Bootstrap loads
  it and production deferred adds lighting only, preserving destination alpha.
  Sky bootstrap tests the convention-appropriate far depth without depth writes;
  pure contact/shadow diagnostic output replaces RGB through a separate blend
  variant. Production SPIR-V reflection omits the seed binding; RDG has no SceneColor
  shader-read feedback. Isolated diagnostics read the seed before lighting.
- Removed the standing R11G11B10 attachment and contact/AO/deferred ordinary
  reads. The three RGBA8 GBuffer records cost 12 bytes/pixel, or 24,883,200
  bytes at 1920x1080, excluding existing SceneColor/depth. Both octahedral
  normals, Specular packing and authored HDR bounds remain unchanged.
  The former R11 quantization is intentionally replaced with RGBA16F precision.
- Emissive/MaterialInputs debug snapshots before lighting only when selected. Failed debug
  preparation retains authoritative SceneColor; production failure remains
  RendererResourcesUnavailable and retry uses the existing transactional slots.
- New GPU pixels cover opaque/masked DefaultLit, Unlit and alpha translucency,
  single application, direct lighting plus emission, independent debug output,
  clear/opaque alpha, EV -2/0/+2, centered subrects and 64/96 resize. The fixture
  contains two coincident translucent draws, so expected alpha overlays are
  independently composed twice. Raw HDR captures and CSV observations live in
  ignored `Build/AgentValidation/MaterialAttributesAndExecutionContracts/Stage4/Emissive/`.
- Cache generations: MIR/translation 14/11, normalization/generator/adapter
  14/23/20, MaterialPassContract/DMAT 19/22; layout 5 and authored asset versions
  remain unchanged. No asset recreation is required. This supersedes the
  Specular receipt's four-target transport and representation generations.

All receipts below are under `Build/.agent-state/logs/`:

| Windows validation | Result | Receipt |
| --- | --- | --- |
| `StaticMeshRenderPreparationVulkanTests` | All 25 cases; emission, MaterialInputs/Emissive debug, Specular, WPO and grouping | `20261117-013648-041838-29872-StaticMeshRenderPreparationVulkanTests.log` |
| `RenderShaderContractTests` | All 73 cases; production seed binding absent, three-record shader decode | `20261117-013706-516600-45868-RenderShaderContractTests.log` |
| `@domain=material,kind=feature --test-jobs 2` | All 13 targets; compiler, Cook/Game-profile DMAT, editor and instances | `20261117-012625-395327-41156-ctest.log` |
| `VolumetricCloudSceneVulkanTests` | Offscreen/Present, RDG dependencies, failures/retry and nine Stage 0 captures byte-identical | `20261117-013737-022587-36724-VolumetricCloudSceneVulkanTests.log` |
| `EditorGridVulkanTests` | All 7 cases; targets/shader generation/device/retry and Present resize | `20261117-013743-232771-45192-EditorGridVulkanTests.log` |
| `DirectionalShadowBaselineVulkanTests.ContactShadowRunsAndDarkensNearFieldBounded --mode qualification` | Bounded correctness case; emission, both depth conventions, subrect, debug, direct/shadow/IBL parity and dynamic sky | `20261117-013754-284753-16920-ctest.log` |
| `EditorRenderingTests` | All 91 cases; HDR layout/budget and editor consumers | `20261117-013758-465679-21464-EditorRenderingTests.log` |
| `RendererSceneContractTests` | All 173 cases; aliases, producer failure and removed contact input | `20261117-013810-981314-37960-RendererSceneContractTests.log` |
| Workspace `all` | Engine, Sandbox and RoadWeaver compile/link integration passed | `20261117-013712-624274-45364-cmake.log` |

Affected analysis selects broad module consumers. The bounded material, shader,
renderer and changed-GPU routes above cover semantics; workspace `all` covers
shared MRT/codec/API consumers in all three declared projects. Later sky/debug
changes do not alter the passing material compiler/Cook inputs, so that receipt
is reused. Updated recovery fixtures explicitly finish material compilation;
retired format/byte/dependency assertions and an obsolete vector accessor were
migrated without widening tolerances. No Vulkan validation errors were observed
in the passing final GPU runs. This is correctness coverage, not a
new timing qualification, packaged Game run or full corpus Cook. Native Metal
compilation/reflection and GPU acceptance remain open; the macOS handoff above
also needs the new RGBA16F fourth MRT, three-record decode, additive/preserved-alpha
pipeline, sky fragment depth output and absence of production seed binding. ClearCoat is the next Stage 4
implementation slice; Stage 4 overall remains incomplete.

#### DefaultLit Specular implementation and Windows receipt

Validated `2f2394693` plus this change on 2026-11-17 using
windows-msvc-x64 / Win64-Debug-DurinEditor, MSVC 19.44, Debug, Tracy and
18 build jobs. GPU: NVIDIA GeForce RTX 3090 / Vulkan 1.4.351, Khronos layer
SDK 1.4.357.0. The final native mesh log contains no Vulkan validation errors.

- DefaultLit activates the existing stable Specular attribute. Numeric roots,
  functions, editor commands, instance values and per-entry reachability use
  the shared registry. Generated surface evaluation clamps Specular to 0..1
  and uses 0.5 for nonfinite values. Direct, local and environment lighting
  share `F0=lerp(0.08*saturate(Specular),BaseColor,Metallic)`; Unlit/PostProcess
  exclude the property. Parameter updates retain accepted/proxy program identity.
- Four-target transport groups Metallic/Specular/Roughness/flags and
  BaseColor/AO, following UE's semantic packing. Default-only and mixed views
  both remain 16 color bytes/pixel, with no added target or descriptor. The
  authored-Specular flag preserves exact default 0.5 decoding. Two octahedral
  normals and the separate HDR emissive target remain Durin-specific; changing
  SceneColor ownership to match UE's Base Pass is outside this slice.
- Independent aligned CPU direct/IBL equations pass the 1e-4 budget. The GPU
  fixture checks Specular -1/0/0.25/0.5/1/2, dielectric/metallic endpoints,
  opaque deferred and alpha-one translucent solid forward execution, direct
  directional light and filtered environment lighting. All 144 HDR channel
  observations pass `max(0.005,0.01*abs(reference))`; maximum absolute errors
  are 0.003752381 direct and 0.000126206 IBL. Packed Specular/F0 errors satisfy
  1/510 and 0.08/510. The mixed view's default region is byte-equivalent in HDR.
- All nine final/forward/local-light captures are byte-identical to the actual
  Stage 0 raw files in `Stage0/captures`, not replacement baselines. Current
  captures, full hashes, numerical CSV, copied receipts and source hashes are
  under ignored
  `Build/AgentValidation/MaterialAttributesAndExecutionContracts/Stage4/Specular/UE/`.
  Set `DURIN_MATERIAL_SPECULAR_EVIDENCE_DIR` to an existing directory to retain
  the mesh fixture's HDR/CSV output; use `DURIN_LOCAL_LIGHT_CAPTURES` for scene parity.

Passing receipts are under `Build/.agent-state/logs/`:

| Selection | Result | Receipt |
| --- | --- | --- |
| `@domain=material,kind=feature --test-jobs 2` | All 13 targets, including Specular reachability/identity, graph-stripped Game-profile DMAT, CPU reference and editor/instance coverage | `20261117-004008-422859-48380-ctest.log` |
| `StaticMeshRenderPreparationVulkanTests` | 24 cases, including Specular and earlier Stage 3 Cube/WPO checks | `20261117-004359-442033-17420-StaticMeshRenderPreparationVulkanTests.log` |
| `RendererSceneContractTests` / `RenderShaderContractTests` | 173 / 72 cases | `20261117-004321-297108-44868-RendererSceneContractTests.log`, `20261117-004613-522804-16824-RenderShaderContractTests.log` |
| `VolumetricCloudSceneVulkanTests` | 1 case; all nine captures equal Stage 0 | `20261117-004514-720008-38708-VolumetricCloudSceneVulkanTests.log` |
| `RoadSceneIntegrationTests` / `SandboxGameplayTests` | 5 / 13 cases | `20261117-004753-692041-26724-RoadSceneIntegrationTests.log`, `20261117-004800-926231-40964-SandboxGameplayTests.log` |
| Workspace `all` / `GBufferQualificationTests` build | Shared API and cross-project build passed; migrated timing fixture compiles | `20261117-004203-685878-45756-cmake.log`, `20261117-004737-291514-14056-cmake.log` |

MIR/translation are 14/10; normalization/generator/adapter are 14/22/19;
pass/DMAT are 18/21. Authored graph/function/instance and layout remain
9/6/3 and 5. Old compiled generations reject and must be rebuilt; no authored
asset recreation is needed for this slice. Subsequent Stage 4 representation
changes must advance beyond these published generations. The GBuffer timing
fixture's pre-existing uppercase GLM component spelling was repaired while
migrating its roughness channel assertion; timing execution was not selected.

Affected analysis spans Engine/Renderer module consumers. The bounded material,
Shader, renderer, Vulkan and cross-project coverage above plus the `all` build
covers this slice; unrelated engine services were not rerun. Later changes are
documentation only. Native Metal compilation/reflection (now also including the
Specular and packed-GBuffer shaders), packaged Game/corpus Cook and final cost/
timing qualification remain open. Stage 4 as a whole is not complete.

### Stage 5: Replace workspace content and retire legacy contracts

Outcome: every retained workspace consumer uses new content and artifacts.
Depends on Stages 1-4; incremental inventory updates occur throughout them.

- [ ] Close each inventory row with converted/recreated/removed assets and repaired references, including instances, functions, import parents, standard functions, defaults/error materials, examples, Water and test fixtures.
- [ ] Regenerate content from supported recipes; round-trip it and verify reimport/recreation produces the new schema. Check overridden values by declared identity or explicit replacement mappings.
- [ ] Cook representative Engine, Sandbox and RoadWeaver content and the opt-in Water fixture into new generations. Verify source-free Game loads with authored graphs/compiler services unavailable, and resolve all required Shader/texture dependencies.
- [ ] Remove obsolete root/field adapters, ordinal assumptions and duplicate semantics. If a one-shot conversion utility remains useful, keep it editor/tool-only with explicit supported input versions.
- [ ] Test old authored/cooked artifacts for clear rejection, missing replacements for actionable diagnostics, and all repaired soft references/Cook roots for reachability. No dangling reference is accepted as an intentional appearance change.

Exit: inventory has no unresolved retained consumer. New and regenerated content
works without a legacy evaluator, old cooked products or runtime compilation.

### Stage 6: Qualify the integrated contract and publish lasting documentation

Outcome: final-state correctness, integration and cost evidence, with no deferred
required gate hidden by a completed checklist. Depends on Stage 5.

- [ ] Run the final relevant coverage in the acceptance matrix below, reusing passing evidence only when relevant inputs/environment are unchanged and applicability is recorded.
- [ ] Run the workspace `all` build after shared-interface migration. This explicit final gate covers cross-project foundational headers, reflected schemas, generated metadata and shader/build inventory consumers that bounded owner tests cannot fully cover.
- [ ] Compare unchanged baseline scenes and the approved recreated/new fixtures against Stage 0 budgets. Record compilation/cache behavior, exact Shader entries, descriptors, GPU transport footprint and runtime upload work; investigate regressions before acceptance.
- [ ] Publish implemented semantics in owning Runtime/Rendering and Editor documents; update the roadmap and content authoring guidance. Keep execution receipts/raw captures under ignored `Build/` artifact directories.
- [ ] Record final tested inputs, selections, backend limits, migration inventory disposition and remaining optional work. Complete this plan only after every required gate passes or the owner explicitly changes its acceptance scope.

## Acceptance Matrix and Execution Policy

Follow [build selection](../Agents/BuildAndRun.md) and
[test selection/result reuse](../Agents/Testing.md). Discover current registered
targets at implementation time; do not invent target names or automatically run
all native tests. The matrix specifies required behavior, not repeated full-suite
runs after every stage.

| Area | Required final evidence |
| --- | --- |
| Schema and language | Round-trip/order determinism; stable IDs/defaults; functions and attribute blend semantics; malformed/cyclic/unknown/duplicate/type/stage/domain rejection; bounded expansion. |
| Compilation and identity | Per-entry reachability and resources, complete reflection/layout agreement, static-selection identity changes, no parameter-value recompilation, compatible artifact sharing, cancellation/stale-result rejection and complete-generation publication. |
| Surface rendering | Local/Spline and retained/direct/indirect paths where supported; Opaque/Masked/Translucent baseline, WPO/custom data/interpolation, forward/deferred, shadows, hit proxies, preview and thumbnails. ClearCoat is qualified only on its declared matrix. |
| New functionality | Independent expected values for Specular and ClearCoat; Cube direction/mips; explicit-LOD vertex displacement; neutral/color/depth PostProcess output, viewport subrects and resizing. |
| Lifetime and failure | Parent/texture/function edits, unload/reload, unavailable required inputs, missing cooked entries, compile failure, resource allocation failure/retry and device recreation do not mix layouts/code or retain stale inputs. |
| Editor and content | Shared commands and canvas, Undo/Redo, clipboard, Apply/save/reopen, instance overrides, import/reimport, closed asset inventory and resolved Level/mesh/default/Cook references. Retained closed toolkits release previews during OnClose/PreExit without waiting for destruction or GC; active draws retain a safe local preview lease. |
| Cook and Game | New-format round-trip and corruption/version rejection; warm/miss Cook; graph-stripped source-free Game execution for new models/domain/resources; Water stays a consumer regression fixture, not an optical feature. |
| Workspace integration | Owning and affected consumer targets across every project in `Durin.dworkspace`, plus selected plugin inventories; final workspace `all` build. |
| Backends and cost | Win64 Vulkan native correctness; SPIR-V and Metal compilation/reflection for changed layouts/entry families; Stage 0 numerical image and cost budgets with raw evidence. Native Metal GPU execution is additional coverage unless separately selected, and cannot be claimed from offline reflection. |

Host application startup is not a default gate. Use focused native rendering and
editor fixtures; if an application-hosted test is essential, name that bounded
case and follow the test workflow before execution. Timing qualification needs
the prescribed exclusive GPU environment; unavailable timing remains unmeasured,
not passed. Numerical budgets must be fixed in Stage 0; final acceptance cannot
choose tolerances after observing a regression.

The initial planning change required document validation only. Stage 0's later
authorization covers the baseline builds/tests recorded below. The owner subsequently authorized Stages 1 and 2 implementation, validation and
commit, then requested continuation of this plan on 2026-11-16. That continuation
authorizes the remaining stages; stage completion still requires its exit gates.

## Stage 0 Frozen Specification

### Audit and ownership

Production baseline is `eb29e77e43306d2a121c2cdb69931175991c51be`.
All measurements use that production code; the snapshot probe and timing output
are qualification-only additions in this Stage 0 change. A repository-wide
tracked-file asset census found 28 `.dasset` files, including three frozen texture
Cook fixtures. Source/test searches covered all three workspace projects, not
only Engine. The first symbol census contains 168 files; raw lists and the
asset bytes/hashes/type/path-string inventory are under the evidence root below.

| Owner / source roots relative to repository | Migration responsibility |
| --- | --- |
| `Engine/Source/Runtime/Engine/{Public,Private}/Materials` | Authored graph/root/function/instance schemas, validation, normalization, generation, compile/Cook identities, defaults and render publication. |
| Engine `Rendering`, `StaticMesh`, `SplineMesh`, `Texture`, asset preparation/Cook/reload | Geometry/material consumers, counted texture references, Cube values, package dependencies and source-free readiness. |
| `Engine/Source/Runtime/RenderCore/{Public,Private}/Shader` | Extend existing typed identities/maps and Vertex Factory contracts; keep Engine reflected types out. |
| `Engine/Source/Runtime/ShaderBuild` | Existing source compilation, dependency fingerprints, artifact sharing/DDC and reflection. No second compile scheduler. |
| `Engine/Source/Runtime/Renderer/Private/Renderers`, `Resources`; `Engine/Shaders/Slang` | Surface bindings, mesh planning/PSO precache, frame resources, PostProcess, GBuffer and lighting implementations. |
| `Engine/Source/Editor/MaterialEditor` | Domain/model/root controls, shared commands, functions, preview, clipboard and transactions. |
| `Engine/Source/Editor/AssetForgeBuiltins`; `Engine/Source/Programs/DurinAssetTool` | Imported material recipes, standard functions, default/template authoring and offline asset services. |
| Other editor material consumers and `Engine/Tests` | Mesh assignments, defaults, asset-family editors, previews/thumbnails, transient graphs, reflection, GPU/Cook/lifecycle fixtures. |
| `Engine/Plugins/Water`; `Engine/Tests/Fixtures/WaterSurface` | Preserve wave conventions while rebuilding material roots and Cook recipes. No optical change. |
| `Sandbox/Source`, `Sandbox/Tests`, `Sandbox/Content` | No direct authored-material API hits in the initial symbol census; retained material/mesh/Level references and runtime defaults still require content validation. |
| `RoadWeaver/Source`, `RoadWeaver/Tests`, `RoadWeaver/Content` | Production scene geometry/default material consumption; `RoadViewportQualificationTests.cpp` authors transient graphs, and `RoadSceneIntegrationTests.cpp` exercises mesh slots and saved Levels. |

Current code already defines `EMaterialDomain { Surface = 0 }` and
`GetMaterialDomainOutputPins` in `MaterialExpressions.h`, and `DMaterial`
serializes Domain. Extend/move this authority instead of adding a second enum.
Current standard functions are exactly UVTransform, SampleNormal and SampleORM;
the roadmap's historical five-function description is not the current inventory.

### Properties, identities and supported combinations

Freeze `EMaterialDomain : uint8 { Surface = 0, PostProcess = 1 }` and
`EMaterialShadingModel : uint8 { DefaultLit = 0, Unlit = 1, ClearCoat = 2 }`.
Use UE-style `EMaterialProperty` names below. These enum values are lookup keys;
serialized root connectivity uses attribute GUIDs. Durin GUIDs deliberately do
not claim UE asset interoperability.

Attribute GUIDs are the following four-word literal constants. The first three
words are `0x4d415454, 0x52505250, 0x00000001`; the table freezes the fourth
word in hexadecimal. They are not recomputed from enum/table order. Never reuse
an ID after retirement. Parameter IDs and function-port IDs remain independent.

| Property | ID word 4 | Type / default | Stage and applicability | Evaluation / attribute blend |
| --- | --- | --- | --- | --- |
| `MP_BaseColor` | `0x00000001` | Float3 / (0.5,0.5,0.5) | Pixel; Surface Lit models | Linear reflectance, clamp 0..1; component Lerp. |
| `MP_Normal` | `0x00000002` | Float3 / (0,0,1) | Pixel; Surface Lit models; Tangent Normal | Safe normalization after Lerp; RNM remains an explicit composition node. |
| `MP_Metallic` | `0x00000003` | Float / 0 | Pixel; Surface Lit models | Clamp 0..1; Lerp. |
| `MP_Roughness` | `0x00000004` | Float / 0.5 | Pixel; Surface Lit models | Clamp 0.045..1; Lerp. |
| `MP_AmbientOcclusion` | `0x00000005` | Float / 1 | Pixel; Surface Lit models | Clamp 0..1; Lerp. |
| `MP_EmissiveColor` | `0x00000006` | Float3 / (0,0,0) | Pixel; both domains, all models | Nonnegative HDR, existing finite channel limits; Lerp. Sole color root for Unlit and PostProcess. |
| `MP_Opacity` | `0x00000007` | Float / 1 | Pixel; Surface Translucent | Clamp 0..1; Lerp. |
| `MP_OpacityMask` | `0x00000008` | Float / 1 | Pixel; Surface Masked | Clamp 0..1; Lerp; discard strictly below authored threshold. |
| `MP_WorldPositionOffset` | `0x00000009` | Float3 / (0,0,0) | Vertex; Surface; World Direction | Finite displacement; Lerp. No implicit bounds/collision/normal updates. |
| `MP_Specular` | `0x0000000a` | Float / 0.5 | Pixel; Surface Lit models | Clamp 0..1; Lerp. |
| `MP_ClearCoat` | `0x0000000b` | Float / 1 | Pixel; Surface ClearCoat | Clamp 0..1; Lerp. |
| `MP_ClearCoatRoughness` | `0x0000000c` | Float / 0.1 | Pixel; Surface ClearCoat | Clamp 0.045..1; Lerp. |

Surface DefaultLit/Unlit accept Opaque, Masked and Translucent; ClearCoat accepts
Opaque/Masked. PostProcess requires Unlit/Opaque, no depth test/write, no mesh
usage, WPO, mask or shadow. It preserves input alpha rather than exposing a
separate alpha root. Domain belongs to the base material and is inherited without
an instance override, matching UE's asset boundary. Shading model/blend retain
explicit static-instance override intent; runtime dynamic instances cannot
switch static settings without selecting a separately compiled configuration.

Inactive known attributes retain authored values but create no normal execution
bindings; diagnostics distinguish inactive properties from invalid graphs.
Editor diagnostic view modes may explicitly demand BaseColor even where normal
lighting is bypassed. Unknown IDs, duplicate IDs, or illegal active root values
reject atomically. Domain and shading-model selectors are not interpolated graph
values. The registry is immutable after engine startup.

One deliberate UE alignment changes Surface Unlit from the current
`BaseColor + Emissive` to EmissiveColor-only. Converted parity fixtures explicitly
compute `saturate(old BaseColor) + old Emissive`, including the old default base
contribution. Newly authored Unlit defaults to black. This is a content recipe
change, not permission to silently change a retained fixture's image.

### Frozen data shapes and entry interfaces

The following signatures describe required C++ value shapes, not implementation
added by Stage 0. Names use existing Durin types where possible:

```cpp
struct FMaterialAttributeDefinition {
    FGuid AttributeID;
    EMaterialProperty Property;
    FMaterialValueSemantics Semantics;
    FMaterialProgramLiteral DefaultValue;
    EMaterialAttributeBlend Blend; // Linear or NormalizedNormal
};
struct FMaterialAttributeBinding {
    FGuid AttributeID;
    FMaterialNumericInput Input; // retained literal and optional expression link
};
struct FMaterialAttributes {
    std::vector<FMaterialAttributeBinding> Bindings; // authored, bounded, sorted on capture
};
class FMaterialAttributeDefinitionMap {
public:
    static auto Find(FGuid) -> const FMaterialAttributeDefinition*;
    static auto GetID(EMaterialProperty) -> FGuid;
    static auto GetActiveProperties(EMaterialDomain, EMaterialShadingModel,
        EMaterialBlendMode) -> std::vector<EMaterialProperty>;
};
struct FMaterialAttributeRoot {
    FMaterialAttributes Properties;
    FMaterialExpressionInput Attributes; // optional aggregate connection
};
struct FMaterialConfiguration {
    EMaterialDomain Domain;
    FMaterialStaticProperties Properties;
};
// DMaterial authoring API; both calls validate before mutating or publishing.
auto SetMaterialConfiguration(const FMaterialConfiguration&)
    -> FMaterialProgramValidationResult;
auto SetMaterialExpressions(std::span<DMaterialExpression* const>,
    const FMaterialAttributeRoot&) -> FMaterialProgramValidationResult;
```

The real definition also owns applicability and finite-value policy from the
table; editor labels/categories are a separate projection. Root storage is one
`FMaterialAttributes` plus an optional aggregate-expression link. Aggregate mode
and active per-property connections remain mutually exclusive. Retained
disconnected defaults survive switching modes. Bounds: 64 root bindings,
existing 256-node/1024-link authored limit, existing function expansion limits.

Rename the MIR aggregate type Surface to MaterialAttributes and replace
`FSurfaceRoot`/separate WPO with a sorted vector of typed `FAttributeRoot`
values: AttributeID, semantics, literal/expression reference and source location.
Flatten function calls before analysis. Function ports preserve GUID, type,
stage/space constraints and required/default status; attribute defaults are
literal-only typed sets, with no expression links. Resource ports carry a typed
texture value and its sampler/fallback. No worker follows a live asset pointer.

`FMaterialCompilationOutput` owns Domain/model, static settings, parameter and
collection layouts, typed Shader-entry requests and `FMaterialEntryRequirements`
per entry. Each requirement contains active attribute IDs, parameter IDs,
resource indices, logical scene inputs, interpolator layout and behavior flags
(WPO, mask, tangent/context needs). Analyze after static selection; shader
reflection must validate the actual binding plan. Conservative execution needs
and reflected bindings are distinguished, never silently substituted.

Freeze the scene/input requirement value shape as follows; containers contain
owned values, never renderer resources or authored object pointers:

```cpp
enum class EMaterialSceneTextureId : uint8 { PostProcessInput0, SceneDepth };
enum class EMaterialSceneTextureFilter : uint8 { Point, Linear };
struct FMaterialSceneTextureRequirement {
    EMaterialSceneTextureId InputId;
    EMaterialSceneTextureFilter Filter;
    bool bRequired = true;
};
struct FMaterialEntryRequirements {
    std::vector<FGuid> AttributeIDs;
    std::vector<FGuid> ParameterIDs;
    std::vector<uint16> ResourceIndices;
    std::vector<FMaterialSceneTextureRequirement> SceneTextures;
    std::vector<FMaterialValueSemantics> Interpolators;
    bool bUsesWPO = false;
    bool bUsesMask = false;
    FMaterialProgramRequirements Context;
};
```

Per-entry compilation output pairs the appropriate existing typed Shader key
with its requirements. The outer output owns compiled layouts, collection
schemas, supported configuration and canonical entry sequence. Keep returned
configuration/root inspection const; mutation flows through the shared graph
commands and atomic validated APIs above. Instance parent/override edits retain
the existing atomic entry point and cannot override Domain.

Retain `FMaterialShaderPermutationIdentity` and
`FMeshMaterialShaderPermutationIdentity`. An entry key contains stable Shader
type name, source entry, target, frequency, permutation, pass-contract version
and output-layout identity; a mesh entry additionally carries the existing
VertexFactoryType/MeshPassKey. Request sets are sorted and deduplicated by full
key, not a new positional mask. Pixel artifacts independent of a factory may
remain shared across Local/Spline; do not blindly form a Cartesian product.

Freeze these initial Shader type/source-entry pairs (implementation may place
the source in owned modules, but keeps these identities): SurfaceForward /
FragmentMain, SurfaceGBuffer / GeometryFragmentMain, SurfaceShadow /
ShadowFragmentMain, SurfaceHitProxy / HitProxyFragmentMain, LocalMaterialVertex /
VertexMain, SplineMaterialVertex / SplineVertexMain, and PostProcessMaterial /
PostProcessMaterialMain. HitProxy is Editor-only. PostProcess uses the existing
fixed fullscreen vertex implementation and a material fragment entry, without
Local/Spline requests. Shadow needs WPO and optional OpacityMask; HitProxy needs
WPO/coverage/ID. Dynamic parameter values never enter entry or PSO identity.

Binding ownership: pass adapters provide View/scene inputs, geometry supplies
GPUScene/deformation data, lighting adapters provide lights/environment/shadows,
and the material layout supplies numeric/resource values and collections. Reuse
the existing set-0 pass/context and set-1 material convention. Preserve the
16-byte reserved material header; numeric fields use current 16-byte slots.
Texture/sampler declarations retain deterministic compact allocation starting
at existing material binding 32. Scene textures never consume authored parameter
IDs or material texture slots. Typed Renderer bindings own counted references;
no reflected pointers enter the render thread.

`FTextureRead` stores texture expression, coordinate expression, mode and optional
LOD/Bias or DDX/DDY operands. Texture2D uses Float2, Cube uses a finite nonzero
direction Float3; arrays/3D remain unavailable. Implicit, Bias and Grad are
Pixel-only; Level is Vertex/Pixel. Coordinate space for Cube is an explicit
author choice, not an automatic World transform. Add AddressW to sampler policy
with Repeat default. Cube permits white/black six-face fallback; reject the 2D
FlatRGNormal fallback. The initial storage decision used a checked Texture2D/TextureCube variant. The
Stage 3 UE interface alignment decision above supersedes that representation
with a validated DTexture reference and explicit detached dimension.

Version transitions are frozen below. Stage 5/6 do not change formats unless a
newly discovered incompatibility is documented first. Stage-specific generations
cannot consume prior intermediate products. Collection/texture-package formats
remain unchanged unless their own representation changes.

| Version authority | Baseline | Stage 1 | Stage 2 | Stage 3 | Stage 4 Specular | Stage 4 Emissive | Stage 4 ClearCoat |
| --- | --- | --- | --- | --- | --- | --- | --- |
| MaterialGraph / Output / Function custom versions | 5 / 3 / 2 | 6 / 4 / 3 | same | 9 / 4 / 6 | same | same | same |
| MaterialInstance custom version | 2 | 2 | 2 | 3 | 3 | 3 | 3 |
| MIR / TranslationSchema | 8 / 4 | 9 / 5 | 10 / 6 | 13 / 9 | 14 / 10 | 14 / 11 | 15 / 12 |
| Normalization / Generator / CompileAdapter revisions | 8 / 16 / 13 | 9 / 17 / 14 | 10 / 18 / 15 | 13 / 21 / 18 | 14 / 22 / 19 | 14 / 23 / 20 | 15 / 24 / 21 |
| MaterialPassContract / DMAT payload schema | 13 / 15 | 13 / 16 | 14 / 17 | 17 / 20 | 18 / 21 | 19 / 22 | 20 / 23 |
| RenderLayout identity and compiled-layout version | 4 / 4 | same | same | 5 / 5 | same | same | same |

The existing shader-library container need not change solely because material
contracts change: new entry/material/source identities and Cook inventories must
prevent satisfying a new request with an obsolete artifact. Add a container
version only if its representation changes. Old authored files fail custom-version
admission with a recreate/convert diagnostic; optional tooling reads the old
schema separately. No production legacy graph evaluator is retained.

### PostProcess and scene inputs

Add one optional material slot to the existing per-view post-process settings;
Engine retains the asset and publishes an immutable material proxy/reference.
Renderer admits one fullscreen material pass after normal HDR scene composition
(including sorted translucency and cloud composition) and before the existing
PostProcess display mapping. GBuffer/AO/isolated-light debug routes bypass the
material slot. Editor assistance follows the existing final output ordering.

PostProcessInput0 is a distinct read-only snapshot of the composed linear HDR
scene in RGBA16F. Write a separate RGBA16F output only when the effect is active;
never sample the active attachment. Output RGB is nonnegative linear HDR,
finite-clamped to 65504, alpha copied from Input0. Apply neither gamma nor
exposure inside material evaluation. Existing display mapping applies
`exp2(clamp(ExposureEV,-16,16))` exactly once, followed by its current filmic
mapping/output transfer and FXAA policy. A neutral effect preserves Input0 and
must not double-compose clouds or translucency.

SceneDepth names the completed scene geometry depth for that view; it excludes
depthless translucency. Point sampling is required. Expose device depth and a
linear positive view-depth conversion, with the existing NormalZ/ReversedZ
projection convention; clear/no-geometry yields an explicit invalid flag and
linear depth 0, not infinity. Decode using projection/view helpers rather than
assuming forward Z. Scene inputs are Pixel-only in PostProcess in this plan.
Functions containing reads inherit the caller's legality.

ViewportUV is [0,1] over the active subrect. For pixel center p and subrect
(Origin,Extent): ViewportUV=(p-Origin)/Extent and
TextureUV=(Origin+ViewportUV*Extent)/TextureExtent. Clamp sampled centers to
the subrect's half-texel interior; account for backend orientation exactly once
in the existing fullscreen adapter. Zero extents reject the pass. Input0 allows
point/linear filtering (linear default), depth allows point only.

`FMaterialSceneTextureRequirement` records InputId, sampling policy and whether
required; all connected reads are required in this scope. Renderer declares RDG
reads and separate output attachment writes from these requirements. A pending
compile retains a complete compatible last-known-good effect; a failed compile,
missing required input or allocation failure bypasses the effect with a local
diagnostic and preserves the normal frame. Retry on later frames. Surface keeps
its existing pending/failed ErrorMaterial policy. No fullscreen draw may bind
the Surface ErrorMaterial or an incomplete/stale generation.

### Specular, ClearCoat and GBuffer

Use UE's familiar scalar Specular mapping:
`F0 = lerp(float3(0.08 * saturate(Specular)), BaseColor, Metallic)`.
Specular=0.5 reproduces the existing nonmetal F0=0.04. No IOR property is added.
Preserve existing base GGX/direct/IBL helpers and specular-AA policy.

ClearCoat is a bounded single-normal dielectric layer, not UE's full refracted
bottom-layer implementation. Let c=saturate(ClearCoat), rc=clamp(CoatRoughness,
0.045,1), and Fc(x)=0.04+0.96*(1-saturate(x))^5. Use existing GGX D and Smith
visibility with alpha=rc^2, the same shading normal for both lobes and existing
shadow attenuation. For each punctual/directional light:

```text
Base = existing DefaultLit direct BRDF contribution with authored Specular
Coat = GGX_D(rc,NoH) * SmithVisibility(rc,NoL,NoV) * Fc(VoH) * Li * NoL
Result = (1-c*Fc(NoV)) * (1-c*Fc(NoL)) * Base + c*Coat
```

For IBL, evaluate existing base IBL at base roughness/Specular, and the coat
specular from the existing prefiltered environment at rc with LUT term
`0.04*A(NoV,rc)+B(NoV,rc)`. Combine as
`(1-c*Fc(NoV))^2 * BaseIBL + c*CoatIBL`; apply the existing AO convention to
both. Add EmissiveColor once after lighting. At c=0, code must take the exact
DefaultLit path. No second normal, refracted bottom direction, absorption,
area-light specialization, or claim of pixel parity with UE ClearCoat is made.

The 2026-11-17 Emissive decision supersedes the original four-record layout:
keep three RGBA8 GBuffer targets and existing RGBA16F SceneColor as fourth MRT.
Model identity uses the existing flag byte: 0 invalid, 1 DefaultLit, 2 ClearCoat.
The 2026-11-17 owner decision above supersedes the Specular part of this frozen
layout: BaseColor/AO share one RGBA8 target, Metallic/Specular/Roughness/flags
share another, and opaque/masked opacity decodes as one. Bit 7 identifies authored
Specular; without it decode exactly 0.5. All Specular views retain three records
plus SceneColor.
The ClearCoat slice adds one optional RGBA8 ModelData target for coat
weight/roughness in R/G and zero B/A on ClearCoat pixels;
clamp/UNorm8 encode. Default-only views omit it. A ClearCoat draw without the required target is rejected. MRT/output-layout variants participate in
Shader/PSO identity; a view with the target uses a matching common GBuffer layout
for its participating draws. Inspect target-count capability before admission.

Baseline GBuffer transport is 12 bytes/pixel; the ModelData extension is
16, excluding existing RGBA16F SceneColor and 4-byte depth in both cases. No
ModelData/fifth MRT attachment is allocated for a default-only view. ModelData quantization error is at most 1/510 per
normalized attribute; Specular's dielectric F0 error at most 0.08/510. Preserve
current normal transport bounds (1 degree) and other channel formats. Forward
and deferred share model equations; codecs belong to Renderer, not the graph.

## Stage 0 Content Disposition

This is the selected disposition of all 28 tracked packages, not a claim that
their replacement has happened. Paths below are repository-relative physical
paths; keep existing logical object identities unless stated. The raw inventory
records exact physical hashes, classes and length-prefixed logical path strings.
That scan is a package census/reference-candidate inventory, not a substitute
for Stage 5's loader/Cook dependency validation. Retain means no material schema
change to unrelated payload; resave only if an actual referencing field changes.

| Package / group | Decision and replacement | Consumers / validation |
| --- | --- | --- |
| `Engine/Content/Materials/DefaultMaterial.dasset` | Recreate new DefaultLit root at the same object path; defaults preserve current appearance. | DefaultMaterialService, unassigned mesh slots, previews and Game default Cook root. |
| `Engine/Content/Materials/Functions/UVTransform.dasset` | Convert function schema; preserve signature/port IDs and numeric behavior. | Standard-function authoring/imported UV recipes; function round-trip and UV pixels. |
| `Engine/Content/Materials/Functions/SampleNormal.dasset` | Convert signature/resource/default schema; preserve ports and RNM behavior. | Normal recipes/functions; decode/strength/composition tests and Cook. |
| `Engine/Content/Materials/Functions/SampleORM.dasset` | Convert signature/resource/default schema; preserve ports and channels. | Import ORM recipes; sampled channel/reflection/function Cook tests. |
| `Engine/Plugins/Water/Content/Materials/DefaultWater.dasset` | Recreate via updated `AuthorWaterMaterial.h` and `WaterMaterialRecipe.cpp`, same logical identity and waves. | Water component soft default, UV0/custom data/WPO/normal convention, Editor/Game fixture Cook. |
| `Sandbox/Content/Materials/Metal/M_MetalRust.dasset` | Recreate new DefaultLit graph at the same logical identity, preserving texture roles and two-layer rust appearance where present in current graph. | Sandbox mesh/Level assignments, material editor reopen and texture-dependent image fixture. |
| `Engine/Content/Models/Box.dasset` | Retain geometry; validate default material selection/slots. | Graybox Level, primitive and preview fixtures. |
| `Engine/Content/Models/Sphere.dasset` | Retain geometry; validate default/override slots. | Material previews/thumbnails. |
| `Engine/Content/Models/SplineBox.dasset` | Retain geometry; validate default slots. | RoadWeaver Level and Spline material path. |
| `Engine/Content/Renderer/DefaultStudioCube.dasset` | Retain Cube asset and current logical path. | Sky/default lighting and new Cube-resource fixture. |
| `Engine/Content/Renderer/ThumbnailStudioCube.dasset` | Retain Cube asset. | Thumbnail/preview environment. |
| `Sandbox/Content/Materials/Metal/Metal053C_1K-JPG_Color.dasset` | Retain Texture2D. | Recreated rust BaseColor. |
| `Sandbox/Content/Materials/Metal/Metal053C_1K-JPG_Metalness.dasset` | Retain Texture2D. | Recreated rust Metallic. |
| `Sandbox/Content/Materials/Metal/Metal053C_1K-JPG_NormalDX.dasset` | Retain Texture2D and normal convention. | Recreated rust normal path. |
| `Sandbox/Content/Materials/Metal/Metal053C_1K-JPG_Roughness.dasset` | Retain Texture2D. | Recreated rust Roughness. |
| `Sandbox/Content/Textures/TEX_StoneHead.dasset` | Retain Texture2D. | Existing project content/reference reachability. |
| `Sandbox/Content/Textures/TEXCUBE_PureSky_512x512.dasset` | Retain Cube asset. | Graybox sky and new Cube parameter alternative. |
| `Sandbox/Content/Volumes/VolumetricCloud/VT_Cloud_Base_Voronoi_128.dasset` | Retain VolumeTexture; no Texture3D material support implied. | Existing cloud composition regression. |
| `Sandbox/Content/Volumes/VolumetricCloud/VT_Cloud_Detail_Voronoi_64.dasset` | Retain VolumeTexture. | Existing cloud composition regression. |
| `Sandbox/Content/Models/GrayboxPawn.dasset` | Retain geometry; convert referencing material slots only if necessary. | Pawn defaults/Level rendering. |
| `Sandbox/Content/Gameplay/PlayerPawnDefaults.dasset` | Retain and validate GrayboxPawn soft reference. | Default gameplay asset Cook closure. |
| `Sandbox/Content/Levels/NewLevel.dasset` | Retain; validate all assignments/default resolution. | Level open/save/Cook. |
| `Sandbox/Content/Levels/GrayboxStage15.dasset` | Convert/resave material-reference fields if changed; preserve actors, textures, cloud assets and transforms. | Graybox rendering and Level Cook/load. |
| `RoadWeaver/Content/Levels/L_RoadNet.dasset` | Convert/resave material-reference fields if changed; preserve spline geometry and object identity (current export name `NewLevel`). | Road scene/viewport tests and Game Cook. |
| `RoadWeaver/Content/NewRoadNet.dasset` | Retain road graph; no material payload rewrite. | Road graph/scene integration. |
| `Engine/Tests/Native/EngineTests/Data/TextureCookedBaseState/CookedPackages/Game/Texture2D.dasset` | Retain frozen texture compatibility fixture. | Texture cooked-base-state tests; do not rebaseline as material content. |
| `Engine/Tests/Native/EngineTests/Data/TextureCookedBaseState/CookedPackages/Game/TextureCube.dasset` | Retain frozen texture compatibility fixture. | Same; Cube material support must not rewrite it. |
| `Engine/Tests/Native/EngineTests/Data/TextureCookedBaseState/CookedPackages/Game/VolumeTexture.dasset` | Retain frozen texture compatibility fixture. | Same; no material Volume domain implied. |

No tracked material-instance or material-collection package was found. Their
transient graphs, parent chains, defaults and generated Cook products remain
required migration/test categories. No tracked package is selected for deletion.
Recreated material objects keep paths so retained references can resolve; if
implementation chooses a different identity, first update this disposition and
rewrite every dependent path/override as a single task.

Additional generated/transient content dispositions:

- Convert all Engine material/function/instance/collection test builders,
  including `TypedMaterialGraphTestFixture`, `ExplicitMaterialProgramTestFixture`,
  `MaterialVariantTestFixture`, `StandardMaterialFunctionTestFixture` and
  `SceneRenderQualification`; preserve semantic inputs, including explicit
  conversion of old Unlit color behavior.
- Update AssetForgeBuiltins imported-surface/Assimp/glTF recipes and the three
  standard-function interfaces. Reimport recreates new-format imported parents;
  generated temporary import packages are not source assets to preserve.
- Recreate asset-independent Surface ErrorMaterial and transient preview and
  thumbnail materials through the new contracts. PostProcess uses bypass on
  failure, never this Surface terminal.
- Convert RoadWeaver's transient viewport material builder. Keep its content
  bindings/defaults; run the registered RoadSceneIntegrationTests and selected
  RoadViewportQualificationTests when their owning changes land.
- Create new Stage 4 fixtures under a test-only logical root
  `/Game/MaterialContracts/`: `M_Specular`, `M_ClearCoat`, `M_Cube`, `M_VertexLOD`,
  `M_PostNeutral`, `M_PostColor`, `M_PostDepth`. Generate them in ignored test/Cook
  output; they do not require shipping project assets.

## Stage 0 Baselines and Acceptance Budgets

Evidence root (ignored):
`Build/AgentValidation/MaterialAttributesAndExecutionContracts/Stage0/`.
It contains `assets.json`, `assets-summary.txt`, `consumers.txt`,
`publication-and-binding-consumers.txt`, `material-qualification.log`,
reconstructed `compiler-samples.csv` (60 intact rows), `contracts.txt`,
`contracts.txt.layout.csv` (100 ordered raw samples), `contracts-layout-final.log`,
`toolchain.txt`, `summary.json`, first/repeat
captures, a generated `ForwardMaterial.slang`, and copied test reports.
Raw reports are not committed; these concise receipts and reproduction inputs
are. Evidence remains required for future comparisons.

Profile: windows-msvc-x64 / Win64-Debug-DurinEditor, Debug, Tracy enabled,
18 build jobs, MSVC 19.44.35228.0; processor identifier Intel64 Family 6 Model
151 Stepping 2. Shader compiler identity:
`ShaderBuild:e21b77147eb05392;adapter=5:3:7`, target `vulkan-spirv-1.5`.
Vulkan native run: NVIDIA GeForce RTX 3090, device 0x2204,
API 1.4.351, reported driver 0x9a100000, Khronos layer SDK 1.4.357.0.
Correctness ran on the ordinary host lane. Exclusive GPU ownership was not
established, so no GPU timing baseline or performance pass is claimed.

### Reproduction and results

Use the host DevTool launcher with the selected profile, following the linked
test workflow. Exact selections and capture controls:

- MaterialQualificationTests, mode qualification, report enabled; filter
  `FMaterialQualificationTests.StagedBuildWorkloadBaseline:FMaterialQualificationTests.MaximumGraphLayoutLatency:FMaterialQualificationTests.InstanceVariantPayloadBaseline`.
  All three passed. CTest receipt:
  `Build/.agent-state/logs/20261115-235726-604888-27240-ctest.log`.
- MaterialQualificationTests, mode qualification, report enabled; final filter
  `FMaterialQualificationTests.ContractsStage0Snapshot:FMaterialQualificationTests.MaximumGraphLayoutLatency`.
  Both passed. Set `DURIN_MATERIAL_CONTRACT_BASELINE` to an absolute report file
  (here the evidence root's `contracts.txt`). CTest receipt:
  `Build/.agent-state/logs/20261116-001421-393198-33868-ctest.log`.
  This final instrumentation records the ordered layout samples as well as
  compiler identity; prior runs are not substituted for that final source state.
- VolumetricCloudSceneVulkanTests, report enabled, ordinary registered direct
  host. Set `DURIN_LOCAL_LIGHT_CAPTURES` to absolute `captures/` and then
  `repeat/` directories. Both runs passed the single scene-route test:
  `20261115-235820-445636-16552-VolumetricCloudSceneVulkanTests.log` and
  `20261116-000604-730755-11664-VolumetricCloudSceneVulkanTests.log`, under
  `Build/.agent-state/logs/`.

The scene is the current `SceneRenderQualification.h` fixed receiver recipe:
96x64, camera (0,0,2500), forward normalize(1,0,0.1), vertical FOV 60 degrees,
near 0.1, far 500000, ReversedZ, clear (0.05,0.08,0.12,1). Receiver local
quad is x=0,y/z=+-40, translated (100,-35,2510), BaseColor (0.7,0.25,0.1),
two-sided Lit and otherwise default surface values. Lights are ordered IDs
23000..23003 at (135,-35+i,2510), alternating point/spot, intensities 1100/1800,
range 200; the fixture owns the other sun/cloud controls. Capture branch disables
cloud rendering. These constant graphs do not read Time: MaterialTimeSeconds
uses the renderer default and has no pixel influence. Future time-dependent
fixtures must set it explicitly to 0 and 1 seconds. Final/Forward captures are
RGBA8; isolated Local is raw RGBA16F. Forward is the existing Lit wireframe
route, not a claim of a solid-forward full-scene capture.

| Local lights | Final SHA-256 prefix | Forward prefix | Local HDR prefix |
| --- | --- | --- | --- |
| 0 | e58fd1d7b1e3726d | 7842eb82ef35ef12 | bccf5468013e4067 |
| 1 | 53c2bd1ef19eeb98 | 36aaab621f837a6a | b90d9a7ce640076b |
| 4 | 0baef1561063cf42 | 4c9041fd526c88b6 | 75d43b5e2d1a727a |

All nine first/repeat files are byte-identical. Full hashes are in summary.json.
The 4-light final PNG was visually inspected and shows the lit receiver; it is
not an empty-scene fallback. This is the current baseline, not reuse of a
historical acceptance image from another plan.

Synthetic PBR compiler fixture snapshot: 496 uniform bytes, 30 numeric fields,
6 texture fields; 6 Editor entries (Game removes HitProxy). C++ object sizes on
this ABI: FMaterialRenderLayout 64 bytes, FMaterialParameterValue 40,
FMaterialStaticProperties 12. These are measured host ABI observations, not
portable serialized sizes. Per-entry reflected descriptors/code bytes:

| Entry | Descriptors | SPIR-V bytes |
| --- | ---: | ---: |
| FragmentMain | 22 | 129160 |
| GeometryFragmentMain | 14 | 28144 |
| ShadowFragmentMain | 0 | 168 |
| HitProxyFragmentMain | 1 | 1008 |
| VertexMain | 5 | 6244 |
| SplineVertexMain | 6 | 17984 |

Three-workload staged compiler probe used five independently isolated cache
seeds per workload; modes are miss, disk reuse, retained reuse. Small-graph
candidate medians were 527171 / 15234 / 3210 microseconds; standard-function
graph medians were 591486 / 26522 / 14671. Each miss seed compiled once; disk
and retained reuse compiled zero times and recorded their expected hit. Shared
two-instance workload compiled once per miss seed, with the second consumer
using retained work. Do not aggregate that second consumer's near-zero producer
time into a claimed compiler speedup.

MaximumGraphLayoutLatency measured median 12726 and p95 13230 microseconds
in the final run (the earlier run was 12588/13249). Its 100 raw samples are
retained; the acceptance budgets below were fixed before the final probe run.
InstanceVariantPayloadBaseline: 8 owners, 4 effective
identities, 8 compatible instances, 13 instance requests, 15 total requests,
11 completions, 5 retained programs / 1118336 retained bytes, instance DMAT
1540898 bytes, root DMAT 188127 bytes. These counts are workload observations;
the entry-contract refactor may change physical request decomposition, but
semantic variants must remain shared rather than duplicated per instance.

Current GBuffer is 16 color bytes/pixel plus 4 depth bytes/pixel, excluding HDR
scene color and allocator padding. At 1920x1080 this is 33177600 color bytes
and 8294400 depth bytes. The optional new model attachment adds 8294400 bytes;
it must not be allocated in a default-only view.

### Frozen acceptance budgets

- Unchanged default Specular=0.5 scenes and the nine captured routes require
  byte-identical pixels on the same backend/toolchain/settings. A mismatch must
  be investigated and an explicit plan decision recorded before relaxing this
  gate. Never silently substitute a new baseline. Recreated assets with the same
  intended semantics still use this rule on their matching fixtures.
- New model fixtures use an independent CPU reference for the frozen equations:
  direct/IBL scalar error <=1e-4 before storage; final HDR channel error
  <=max(0.005,0.01*abs(reference)) including quantization. Forward/deferred use
  the same HDR bound. ModelData <=1/510, F0 <=0.08/510, normal <=1 degree.
  ClearCoat c=0 must exactly select the DefaultLit path with the same transport.
- PostNeutral requires identical output to bypass; PostColor with multiplier
  (0.5,1,2) must match the existing display mapper applied after the operation,
  within 1 RGBA8 code value. Test EV -2/0/+2, both depth conventions, view subrect
  and resized targets. Alpha is copied exactly. Depth reconstruction error is
  <=max(1e-4,1e-4*viewDepth) for finite geometry samples; background follows the
  invalid/zero-depth rule. VertexLOD is checked at t=0/1 and position error <=1e-4
  for representable fixture coordinates; Cube uses known face colors and mips.
- Default-only view adds zero GBuffer attachments/bytes and zero scene-texture
  copies. Active PostProcess uses at most one new full-resolution RGBA16F target
  (8 bytes/pixel), one fullscreen material draw and required declared reads.
  Extended Lit uses at most one extra RGBA8 attachment (4 bytes/pixel).
- On the equivalent synthetic fixture, material uniform payload stays <=496
  bytes and texture count <=6; adding fixed/default surface outputs cannot add
  runtime parameters. Each baseline entry's reflected descriptor count must
  not grow. Baseline default usage stays <=6 Editor / <=5 Game entries, and
  Local Forward-only stays 2. New model/domain entries are accounted separately.
  New descriptor/layout identities may differ, but bindings must remain exact.
- Equivalent material Shader code and cooked/retained bytes stay <=125% of the
  measured baseline, with expanded schema metadata reported separately. Numeric
  parameter updates compile zero Shaders; disk/retained warm modes compile zero;
  compatible instances share effective programs, and missing inactive branches
  allocate no resources. Source-only changes must not create duplicate entries.
- Same-host Debug CPU budgets: max-graph layout median <=16000 us, p95 <=20000 us;
  small miss/disk/retained candidate medians <=750000/40000/10000 us; standard
  function medians <=850000/65000/30000 us. Repeat the same cache seeds/workloads
  across three quiet CPU runs for an acceptance decision. Current measurements
  characterize one run and do not establish cross-host performance.
- GPU timing is not a Stage 0 pass. Before any later GPU performance claim,
  establish the exclusive lane required by the testing workflow and compare
  unchanged parent/new fixtures consecutively: median <=110% and p95 <=115% for
  unchanged routes, with matching pixels. New ClearCoat/PostProcess cost is
  reported separately, not compared as if it were unchanged shading.

### Validation gaps and independent diagnostics

The read-only DurinAssetTool identity-audit did not yield a usable report in
this session: Engine invocation failed; Sandbox invocation failed with the old
tool and remained idle after rebuilding the current tool, then was canceled.
The cause is unconfirmed. It is not counted as a passed loader/reference audit.
The complete tracked-file census and serialized name/type inspection establish
the disposition above; actual reference-edge extraction and new-content
load/Cook closure remain explicit Stage 5 gates, including repairing all actual
edges discovered there. No asset was loaded/resaved to work around this issue.

The selected three-case measurement run and final two-case snapshot/layout run
passed. A separate diagnostic selection of
`FMaterialQualificationTests.ColdAndWarmCompilerBaseline` failed its existing
Forward descriptor assertion: expected 21, actual 22. The baseline snapshot and
reflection show the current 22 descriptors; the assertion was already present
in the unchanged parent source. Receipt:
`Build/.agent-state/logs/20261116-001143-997311-35420-ctest.log`, with full output
copied to `existing-cold-warm-failure.log` under the evidence root. Do not use
this test as a passed qualification or change its expectation without examining
the entry's binding needs. Stage 2's reflection/consumer migration owns its
resolution. The diagnostic run also reported 182708 total SPIR-V bytes and
188127 root cooked bytes, consistent with the successful snapshot/variant run.
Earlier archived material unload failures and waived qualification are not
imported as current results.
Unselected Cook/editor/application tests, native Metal GPU execution, full
workspace builds and GPU timing were not run in Stage 0. Their later gates remain
as specified; Stage 0 changes no production interfaces requiring an `all` build.

## Rebase Review on 2026-11-16

Reviewed `b8d1aa0d7` against the original Stage 0 handoff `5799e35d2`.
The planning commits were replayed as `e73c76db3` and `b8d1aa0d7` on top of
`970621e8a` (`refactor(editor): align toolkit ownership and preview teardown`).
That inserted refactor changes editor ownership/settings/lifetime and their
tests. At review time it implemented no M14 stage; Stage 1 was still unstarted.

| Review area | Verified effect and plan disposition |
| --- | --- |
| Material compiler and formats | No diff in Engine Materials public/private code, RenderCore, ShaderBuild, Renderer or Slang shaders. Frozen properties, data shapes, model/domain/sampling decisions and version transition table stay unchanged. |
| Assets | Recomputed hashes match all 28 baseline packages; tracked package count remains 28 with no additions. Keep every disposition row and logical identity. Editor window/profile state is separate from material content. |
| Baseline probes and images | MaterialQualificationTests.cpp, SceneRenderQualification.h and the core material/Shader/render sources have no diff. Preserve the historical structural/reflection and image captures as reference evidence; do not relabel their tested revision as the rebased HEAD. |
| Editor ownership | FToolkitManager now owns admitted toolkits; reflected editor and host back-references do not create the old ownership chain. Extend current toolkits, without recreating subsystem-owned Session enumeration or lifetime APIs. |
| Preview release | MMaterialEditor::OnClose clears MaterialPreviews. PreExit closes editors and releases thumbnail previews before rendering teardown. MaterialPreviews use shared ownership; a draw holds a local shared lease against reentrant closing. New domain/model previews must obey the same lifetime. |
| Preferences and graph viewports | Use MaterialEditorProfileSettings and version-3 files under MaterialEditor/Profiles. Do not restore retired MaterialEditorSessionSettings or migrate obsolete window state as part of material asset conversion. |
| Tests and support | AssetEditorLifecycleTests replaces AssetEditorSessionTests. Material editor/function tests use current toolkit queries, and FMaterialPreviewHarness has idempotent explicit ShutdownRenderer. Update future fixtures through these seams; material graph editing sessions remain valid and are distinct from the retired asset-editor Session ownership API. |

The owning implemented editor rules are
[Asset Editors](../Editor/Architecture/Framework/AssetEditors.md).
Stage 1's root/command/UI migration and later domain previews must additionally
retain these focused regression scenarios:

- Closing a retained material toolkit releases its preview before toolkit
  destruction; Editor PreExit can then shut down Renderer before immediate GC.
- Closing during preview drawing cannot destroy the actively drawing preview.
- Material/function tab close, host transfer, reopen and preference callbacks
  use current IsEditorOpen/IsClosed and ToolkitManager membership semantics.
- Thumbnail preview release preserves registrations needed by a subsequent
  editor instance and does not keep a scene alive past renderer teardown.

MaterialPreviewTests and material graph interaction cases are registered under
MaterialEditorInteractionTests; function persistence is under
MaterialEditingPersistenceTests. Select those affected targets and the relevant
thumbnail/lifecycle cases when implementation touches their behavior, using
the current registry. The inserted commit reports its own 678-test and startup
validation; this review did not independently rerun or adopt that as M14 evidence.

This review is source/asset comparison and documentation maintenance only.
No new native build/test, timing measurement, Cook or startup smoke was run.
Changed editor host/preview lifecycle means the historical evidence does not
qualify the rebased editor's open/close/teardown behavior. Re-run the changed
editor cases at the affected implementation stage; CPU timing acceptance still
requires the existing three-run same-host protocol on the final relevant inputs.
The old 21-versus-22 descriptor assertion and unavailable identity-audit report
remain historical diagnostics, not newly resolved or newly reproduced failures.
No architecture redesign, asset recensus or Stage 0 restart is required by the
verified changes.
