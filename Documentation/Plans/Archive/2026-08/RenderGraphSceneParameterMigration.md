# Render Graph Scene Parameter Migration Plan

Summary: Migrate every production scene contributor to narrow typed inputs and outputs backed by graph-owned pass parameters.

Last reviewed: 2026-08-29

Status: Archived
Completed: 2026-08-29

## Current Status

Milestone 4 is complete. All twelve production scene contributors accept
feature-owned narrow inputs, return typed outputs, and are wired directly by
the stable-order composer. All thirteen production scene passes allocate
immutable graph-owned parameter objects with feature-local resource schemas;
producer-created typed values now carry completion between contributors.

The broad contributor context, frame-wide resource and execution-channel bags,
ten persistent-input helper copies, neighboring production `Graph.Use*`
declarations, and the non-parameterized scene feature-pass helper are removed.
Contact-shadow fragment and compute routes retain composed graph/shader
binding; the other feature recorders consume pass-resolved physical resources
through their existing typed shader contracts. Route selection, retained
backing publication, output roots, telemetry, failure/recovery behavior, and
rendered Vulkan paths remain covered by the validation recorded below.

Completion decision: direct graph/shader composition remains at the
contact-shadow pilot boundary. Extending composition through every nested
feature renderer would move shader-submission ownership out of those renderers,
contradicting this plan's renderer-ownership invariant and combining Milestone
4 with a renderer API redesign. The implemented contract instead makes the
pass parameter field the sole graph-access authority and preserves each
feature renderer's typed shader binding after pass-scoped resolution.

Milestone 5 is ready for a separate bounded authoring-contract and diagnostics
plan; this migration does not remove RenderGraph compatibility APIs outside
production scene authoring.

## Goal

Make every production scene contributor receive only the immutable prepared
services and upstream graph capabilities it actually consumes, return a typed
output that the explicit composer wires to downstream contributors, and
declare every pass resource/value dependency through one graph-owned parameter
object. Remove the broad contributor context, frame-wide resource/channel
bags, repeated persistent-input helpers, and scene-side manual declaration
authority without changing rendering behavior.

## Scope

- Narrow typed input and output structures for all twelve production scene
  contributors in their existing stable composition order.
- Feature-local graph parameter structures for directional shadow, GBuffer,
  ambient occlusion, contact-shadow visibility, cloud shadow, deferred
  directional lighting, base scene, cloud spatial, cloud composite, scene
  color, post process, and editor assistance passes.
- Parameter-derived texture, buffer, attachment, typed-value, token, and
  shader-resource declarations using the existing RenderCore metadata model.
- Direct typed output wiring in `FSceneFrameGraphComposer`, including optional
  disabled routes and output-root completion.
- Removal of `FSceneFrameGraphContributorContext`,
  `FSceneFrameGraphResources`, `FSceneFrameGraphExecutionChannels`, repeated
  persistent-input helpers, and production manual `Graph.Use*` declarations.
- Preservation of prepared route/fallback ownership, renderer services,
  retained backing requests, result publication, telemetry, temporal commit or
  abort, resize, multi-view, and Editor assistance behavior.
- Structural, renderer, RHI/Vulkan, rendered-output, recovery, and synchronized
  CPU/GPU qualification plus lasting documentation and roadmap handoff.

## Non-Goals

- Changing scene feature algorithms, shaders, material evaluation, topology
  selection, fallback policy, render-target formats, draw/dispatch work, or
  telemetry meaning.
- Moving renderer services, mutable publications, complete scene topology, or
  the prepared render plan into pass parameter objects.
- Replacing the explicit stable-order composer with a plugin registry, service
  locator, runtime-polymorphic contributor list, child graphs, or blackboard.
- Redesigning the canonical graph compiler, composed shader binding, external
  import registry, typed value storage, RHI transitions, Vulkan descriptors,
  or resource recovery.
- Retiring low-level non-parameterized RenderGraph APIs outside production
  scene authoring; their wider compatibility disposition belongs to Milestone
  5.
- Adding transient aliasing, async compute, split barriers, queue scheduling,
  parallel recording, pass merging, compiler reordering, or persistent graph
  reuse.
- Combining this migration with Milestone 5 public authoring enforcement or
  speculative performance optimization.

## Design Decisions and Invariants

- **The composer remains the only frame wiring authority.** It keeps stable
  contributor order and connects explicit returned outputs to the next typed
  input. Contributors do not discover peers or mutate a shared channel bag.
- **Inputs expose capabilities, not the frame.** A contributor input may hold
  the renderer/prepared feature data it needs, exact upstream graph handles or
  typed values, selected routes, dimensions, and small immutable flags. It may
  not embed the old context, complete resource set, complete topology, or
  unrelated services.
- **Outputs are feature-owned and immutable after return.** Each output carries
  only downstream graph handles, typed completion values, tokens, or selected
  publication data. Disabled optional routes return an explicit empty/disabled
  form chosen before downstream parameter assignment.
- **Pass parameters remain the sole declaration authority.** Every migrated
  pass uses parameterized `AddPass`; no neighboring manual `Graph.Use*` block
  supplements it. Attachments, logical values, output tokens, and shader-backed
  graph resources retain distinct typed wrappers.
- **Shader-visible graph access stays parameter-owned.** Contact-shadow routes
  use composed graph/shader fields directly. Other feature callbacks resolve
  only their declared parameter fields and hand the resulting physical
  resources to the existing typed feature-renderer shader contract; they do
  not capture graph handles or maintain a neighboring declaration table.
- **Persistent inputs are exact, not blanket declarations.** The ten repeated
  helper copies disappear. Each pass parameter object names only the selected
  environment/default/shadow/cloud resource actually consumed by its route.
- **Migration follows dependency order.** Producer outputs land before their
  consumers. A slice may temporarily adapt a typed output into the old composer
  storage, but no contributor may regain broad-context access and the adapter
  must be removed by Stage 5.
- **Topology and fallback finish before contribution.** The composer continues
  to choose contact/cloud routes, environment candidates, target availability,
  and output presentation before it constructs narrow inputs. Optional graph
  members do not make policy decisions inside callbacks.
- **Renderer ownership stays local.** Feature recorders/renderers continue to
  own shader selection, PSOs, work recording, telemetry, publications, and
  recovery-sensitive state. RenderCore receives only graph declarations and
  execution callbacks.
- **Failure remains atomic.** Invalid typed wiring, absent required upstream
  output, malformed metadata, unavailable backing, or composed binding failure
  prevents the affected recording and final publication; it cannot silently
  select a different feature route.
- **Parity is structural and rendered.** Stable pass names/order, normalized
  uses/dependencies/transitions, output roots, descriptor bindings,
  draw/dispatch counts, images/readbacks, telemetry, and accepted budgets are
  gates for each slice, not cleanup deferred to the end.

## Current Foundations and Gaps

| Area | Reusable foundation | Gap owned by this plan |
| --- | --- | --- |
| Pass parameters | Graph-owned immutable storage, nested/optional/array metadata, exact attachments/resources/values, canonical lowering, pass-scoped resolution | Closed: all thirteen production scene passes use feature-local parameter schemas |
| Shader binding | Graphics/compute graph-required fields, exact SRV/UAV ranges, reflection validation, atomic composed submission | Contact shadow remains directly composed; other feature renderers bind physical resources resolved only from exact pass fields |
| External resources | Builder-owned canonical imports, retained backing publication, exact selected environment fallback | Closed: retained handles flow through narrow producer outputs and consumer inputs |
| Logical results | Typed graph values have one writer, declared readers, graph lifetime, and deterministic dependencies | Closed: producers create and return their own completion values |
| Scene composition | One explicit stable-order composer, twelve coherent contributors, routes selected before compile | Closed: contributors receive feature-owned inputs and return typed outputs |
| Pass declarations | Parameter metadata covers exact resources, values, tokens, and attachments | Closed: production manual declarations and persistent-input helpers are absent |
| Observation | Pointer-free captures, parameter field paths, binding names/types, telemetry, rendered fixtures, synchronized timing gates | Structural contracts and representative Vulkan suites cover the migrated whole-scene surface |
| Runtime safety | Complete-or-null backing, compile-before-recording failures, result commit/abort, Vulkan state validation, recovery tests | Recovery and resource-reload coverage passes with graph-owned parameters scoped to graph execution |

## Implementation Stages

### Stage 0: Freeze contributor signatures, migration slices, and parity oracles

- [x] Inventory all twelve contributor entry points and thirteen pass callbacks,
  recording every context field read, graph resource/value/token consumed or
  produced, manual use, shader-bound resource, route, fallback, publication,
  renderer service, and side effect.
- [x] Specify the narrow input and typed output type for each contributor and
  the feature-local parameter structure for each pass, including optional
  disabled routes, exact attachments/ranges, completion values, and output root.
- [x] Freeze the stable dependency-order migration slices and any temporary
  adapter boundary; require each adapter to name its removal stage and forbid
  new broad-context or manual-use callers.
- [x] Capture manual-versus-parameterized normalized graph structure, pass
  order, dependencies, transitions, lifetimes, culling, shader bindings,
  command identities, publications, telemetry, and rendered/readback oracles
  for representative disabled, fragment, compute, and Editor routes.
- [x] Freeze graph build/compile/execute CPU, capture bytes, parameter storage,
  retained/peak bytes, RHI command counts, synchronized GPU, and full-frame
  qualification budgets from the current production routes.
- [x] Add focused structural fixtures that fail when a contributor signature
  accepts the legacy context, a production pass lacks parameters, a migrated
  pass adds manual authority, or typed outputs are wired absent/wrong/foreign.

#### Acceptance Gate

- Every context field and legacy declaration has one selected narrow owner and
  removal slice; every pass has one exact parameter and output contract.
- Structural and rendered parity oracles cover the active graphics/compute,
  optional, fallback, output, failure, resize, multi-view, and Editor surface.
- No unresolved signature, adapter lifetime, route policy, or budget decision
  remains before the first production migration.

### Stage 1: Establish narrow contributor and typed-output composition

- [x] Add feature-owned input/output vocabulary beside the contributors, with
  explicit optional-route results and direct graph value/resource ownership.
- [x] Teach the composer to construct narrow inputs and consume returned typed
  outputs in stable order without moving route/fallback decisions into features.
- [x] Replace pre-created frame-wide execution channels with producer-created
  typed values or exact composer-owned roots while preserving one writer and
  declared reader semantics.
- [x] Keep renderer services and mutable final publications outside parameter
  objects; pass only the minimum service references required by each feature.
- [x] Add compile-time and focused scene-contract coverage for signature
  narrowness, output type matching, disabled outputs, wrong-order/missing
  producers, and graph-lifetime boundaries.
- [x] Prove that introducing the typed wiring layer alone leaves current graph
  captures, work, output, telemetry, and failure behavior unchanged.

#### Acceptance Gate

- The composer can connect contributors through typed outputs without a
  mutable frame-wide channel lookup or implicit producer discovery.
- Missing, wrong-type, foreign-graph, or disabled required outputs fail before
  affected recording with stable contributor/pass/field diagnostics.
- The interface layer adds no pass, dependency, transition, draw/dispatch, or
  publication change before resource-declaration migration begins.

### Stage 2: Migrate primary geometry and shadow producers

- [x] Migrate Directional Shadow and Ambient Occlusion to narrow inputs,
  returned outputs, parameter-derived values/textures/attachments, and exact
  persistent inputs.
- [x] Finish GBuffer's contributor signature/output migration while preserving
  its already parameterized attachment and completion declaration.
- [x] Finish Contact Shadow's contributor signature/output migration while
  preserving its composed fragment/compute shader bindings and route parity.
- [x] Remove manual `Use*`, repeated persistent-input helpers, and broad context
  reads from these contributors in the same slice.
- [x] Compare normalized captures, shader bindings, draw/dispatch work,
  disabled/failure routes, outputs, telemetry, resize, multi-view, and Vulkan
  qualifications for these producers.

#### Acceptance Gate

- Directional Shadow, GBuffer, Ambient Occlusion, and both Contact Shadow routes
  expose only narrow inputs/typed outputs and contain no manual declaration
  authority or graph-resource shader mirror.
- Their outputs wire directly to downstream typed inputs with identical graph
  structure, rendered results, failure atomicity, and accepted budgets.
- No producer callback can resolve an undeclared, foreign, or unavailable
  resource through captured frame-wide state.

### Stage 3: Migrate lighting and base-scene consumers

- [x] Migrate Volumetric Cloud Shadow, Deferred Directional Lighting, and Base
  Scene contributor signatures and typed outputs in dependency order.
- [x] Declare graph access metadata for every shader-visible texture/buffer and
  parameterize all values, attachments, and optional fragment/compute routes.
- [x] Replace blanket persistent inputs with exact selected environment,
  default, shadow, GBuffer, ambient-occlusion, and contact-visibility fields.
- [x] Preserve isolated/production deferred selection, environment fallback,
  cloud-shadow disabled/fragment/compute behavior, and base-scene publication.
- [x] Remove the slice's legacy helpers/manual uses/context reads and validate
  capture, descriptor, work, image/readback, telemetry, recovery, and timing
  parity.

#### Acceptance Gate

- Cloud Shadow, Deferred Directional Lighting, and Base Scene depend only on
  explicit upstream typed outputs and selected prepared inputs.
- All shader-visible graph accesses originate from pass parameter fields;
  exact optional routes declare only resources they can access.
- Deferred/base output, environment fallback, telemetry, recovery, and budgets
  remain equivalent across supported route combinations.

### Stage 4: Migrate clouds, final color, post process, and Editor output

- [x] Migrate Volumetric Cloud Spatial, Volumetric Cloud Composite, Scene
  Color, Post Process, and Editor Assistance to narrow typed signatures and
  returned outputs.
- [x] Parameterize every remaining texture/value/token/attachment declaration
  and preserve typed shader binding for fragment and compute routes.
- [x] Preserve cloud weather/default resource selection, cloud disabled and
  fragment/compute/composite paths, scene-color composition, isolated debug,
  FXAA/display mapping, present/offscreen roots, and Editor overlay ordering.
- [x] Make final output completion/root ownership an explicit typed connection
  rather than a frame-wide token-channel mutation.
- [x] Remove the slice's helpers/manual uses/context reads and compare graph,
  descriptors, work, images/readbacks, HDR/output layout, Editor grid, failure,
  recovery, resize, multi-view, and synchronized budget evidence.

#### Acceptance Gate

- The entire directional-shadow-through-output chain uses narrow typed wiring
  and graph-owned parameter declarations in the existing stable order.
- Fragment/compute clouds, final post process, offscreen/present output, and
  Editor assistance preserve exact topology, bindings, output, telemetry,
  failure/recovery behavior, and accepted budgets.
- The final root is derived from an explicit typed output; no contributor
  discovers or mutates a global completion channel.

### Stage 5: Remove legacy scene authoring infrastructure

- [x] Delete `FSceneFrameGraphContributorContext`, the contributor declaration
  macro/signature that requires it, and every adapter retained by earlier slices.
- [x] Delete `FSceneFrameGraphResources` and
  `FSceneFrameGraphExecutionChannels` after all live handles/values are carried
  by feature outputs and narrow inputs.
- [x] Delete all repeated `DeclarePersistentGraphicsInputs` helpers and remove
  every production scene manual `Graph.UseTexture`, `UseBuffer`, `UseValue`,
  and `UseToken` declaration.
- [x] Bound or remove non-parameterized `AddSceneFrameFeaturePass` use in scene
  authoring while leaving RenderGraph's general compatibility API untouched.
- [x] Add negative/static enforcement preventing legacy types, helpers, manual
  declarations, graph-resource shader mirrors, or frame-wide capture from
  re-entering production contributors.
- [x] Run aggregate structural tests and inspect normalized captures to prove
  exactly one declaration/binding authority per scene pass.

#### Acceptance Gate

- Repository searches and structural tests find no production scene contributor
  context, resource/channel bag, persistent-input helper, manual `Graph.Use*`,
  or non-parameterized feature-pass declaration.
- Every pass has one immutable graph-owned parameter object and every
  contributor has one narrow typed input/output contract.
- Removing compatibility infrastructure changes no graph structure, command,
  output, telemetry, failure, recovery, CPU, memory, or synchronized GPU oracle.

### Stage 6: Qualify, document, and hand off Milestone 4

- [x] Run focused and aggregate RenderCore/ShaderFoundation/Renderer scene
  contracts, representative RHI/Vulkan validation, rendered-output and recovery
  suites, resize/multi-view/Editor coverage, production route qualifications,
  and the repository-required build tier.
- [x] Record final contributor/pass/parameter counts, graph uses/dependencies/
  transitions, capture bytes, command/descriptors, retained/peak bytes, build/
  compile/execute CPU, images/readbacks, telemetry, and synchronized GPU
  evidence against Stage 0 gates.
- [x] Update lasting Render Graph and Renderer frame-preparation contracts with
  the implemented narrow contributor, typed output, parameter ownership,
  failure, and compatibility rules.
- [x] Update the roadmap Milestone 4 state and record evidence-based entry
  disposition for the separate Authoring Contract and Diagnostics plan.
- [x] Record exact validation, close only passed checklists, and prepare the
  repository-required plan/stage commit provenance.

#### Acceptance Gate

- Focused, aggregate, build, Vulkan, rendered-output, recovery, Editor,
  structural, CPU, memory, capture, and synchronized GPU gates pass with
  recorded evidence.
- Code, tests, captures, lasting contracts, and the roadmap agree that the full
  production scene is parameter-driven and uses narrow typed contributor wiring.
- Milestone 5 is either ready for a new bounded plan or remains proposed with
  its missing entry evidence stated explicitly.

## Completion Evidence

- Production inventory: twelve contributors, thirteen parameterized passes,
  ten removed persistent-input helper copies, and no remaining production
  scene `Graph.UseTexture`, `UseBuffer`, `UseValue`, or `UseToken` call.
- Build and aggregate contracts: `DevTool.bat build`,
  `RendererSceneContractTests` (39), `VolumetricCloudSceneContractTests` (7),
  `RenderContractTests` (94), and `RenderShaderContractTests` (41) passed.
- Vulkan/rendered paths: `DirectionalShadowBaselineVulkanTests`,
  `VolumetricCloudSceneVulkanTests`, `EditorGridVulkanTests`,
  `RendererResourceReloadVulkanTests`, and
  `HDRDisplayMappingQualificationTests --mode qualification` passed.
- Synchronized GPU gates: `GBufferQualificationTests --mode qualification`
  and `VolumetricCloudQualificationTests --mode qualification` passed after
  rejecting and rerunning explicitly unstable samples.
- Documentation: changed documentation, all documentation, all plans, and all
  roadmaps validators passed.
- `fast-all` was attempted but its build was blocked in unrelated existing
  `EditorAssetWorkflowTests` and `ContentBrowserWorkflowTests` targets by a
  missing `AssetTools/IAssetTools.h` include dependency. The affected Editor
  targets do not compile or link the migrated Renderer code; the required full
  build and all affected focused/aggregate targets above passed.

## Validation Matrix

| Concern | Required evidence |
| --- | --- |
| Contributor boundaries | All twelve contributors expose only selected prepared/services/upstream fields; forbidden broad-context and unrelated-field compile-time fixtures fail |
| Typed wiring | Every produced resource/value/token has one typed owner and explicit downstream consumer; missing, disabled, wrong-type, wrong-order, and foreign-graph wiring fail deterministically |
| Parameter authority | All thirteen scene passes use immutable graph-owned parameters; no production manual use or duplicate graph/shader resource table remains |
| Dependency parity | Normalized passes, resource/value uses, versions, dependencies, culling, lifetimes, transitions, and roots match frozen route-specific oracles |
| Shader binding | Contact-shadow graph resources retain composed field/binding identity; other feature renderers receive only resources resolved from exact parameter fields with matching access, range, optionality, and stage |
| Routes and fallback | Disabled/fragment/compute cloud/contact routes, environment/default selection, isolated deferred/debug, present/offscreen, and Editor paths declare only selected resources |
| Failure and lifetime | Invalid wiring/metadata, unavailable backing, callback/execution failure, graph destruction, device loss, and recovery publish no partial result and retain no graph object |
| Production parity | Draw/dispatch identity, images/readbacks, HDR/display/output layouts, telemetry, temporal commit/abort, resize, and multi-view isolation remain equivalent |
| Legacy removal | Searches plus structural tests reject contributor context, resource/channel bags, persistent-input helpers, manual scene `Use*`, and non-parameterized production feature passes |
| Budgets | Parameter storage/traversal, capture bytes, graph build/compile/execute median and p95, command/descriptors, retained/peak bytes, full-frame CPU, and synchronized GPU remain within frozen gates |
| Platform and docs | Required builds, RenderCore/Shader/Renderer/RHI/Vulkan suites, qualifications, changed/all docs, and all-plan/all-roadmap validators pass |

## Definition of Done

- All twelve production scene contributors accept narrow typed inputs, return
  typed outputs, and remain connected by one explicit stable-order composer.
- All thirteen scene passes declare resources, values, tokens, attachments, and
  shader-visible graph access through one immutable graph-owned parameter
  object with exact pass-scoped resolution.
- `FSceneFrameGraphContributorContext`, `FSceneFrameGraphResources`,
  `FSceneFrameGraphExecutionChannels`, persistent-input helper copies,
  production manual `Graph.Use*`, and non-parameterized feature-pass authoring
  are absent.
- Route/fallback decisions, graph topology, dependencies, transitions,
  descriptors, draw/dispatch work, output, telemetry, commit/abort, recovery,
  resize, multi-view, and Editor behavior match frozen oracles.
- Focused, aggregate, Vulkan, rendered-output, failure/recovery, build,
  structural, CPU, memory, capture, and synchronized GPU gates pass.
- Lasting Render Graph and Renderer contracts plus the roadmap describe the
  landed Milestone 4 behavior and the evidence-based Milestone 5 entry state.
- Changes are staged and committed with repository-required plan provenance
  after successful validation.

## Deferred Follow-ups

- Milestone 5 parameter-aware capture/inspection, new-feature authoring
  enforcement, public migration guidance, and compatibility API disposition.
- Uniform-byte block or push-constant composition beyond the existing typed
  shader submission contract.
- Public/plugin contributor injection or dynamic feature registries, if a real
  extensibility requirement independently justifies them.
- Physical transient aliasing, queue scheduling, split barriers, parallel
  recording, pass merging, compiler reordering, and persistent graph reuse,
  each under its roadmap evidence gate.

## Related Documentation

- [Render Graph Parameter-Driven Authoring Roadmap](../../../Roadmaps/Archive/2026-08/RenderGraphParameterDrivenAuthoring.md)
- [Render Graph Pass Parameters Foundation](RenderGraphPassParametersFoundation.md)
- [Render Graph External Registration and Typed Values](RenderGraphExternalRegistrationAndTypedValues.md)
- [Render Graph and Shader Parameter Composition](RenderGraphAndShaderParameterComposition.md)
- [Render Graph](../../../Runtime/Rendering/RenderGraph.md)
- [Shader Parameters](../../../Runtime/Rendering/ShaderParameters.md)
- [Renderer Frame Preparation and Render Graph Execution](../../../Runtime/Rendering/RendererFramePreparation.md)
- [Renderer Resource Recovery](../../../Runtime/Rendering/RendererResourceRecovery.md)
- [Testing](../../../Agents/Testing.md)
- [Build and Run](../../../Agents/BuildAndRun.md)

## Related Code

- `Engine/Source/Runtime/Renderer/Private/Renderers/SceneFrameGraphComposer.h`
- `Engine/Source/Runtime/Renderer/Private/Renderers/SceneFrameGraphComposer.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/SceneFrameGraphContributors.h`
- `Engine/Source/Runtime/Renderer/Private/Renderers/SceneFrameGraphTypes.h`
- `Engine/Source/Runtime/Renderer/Private/Renderers/SceneFrameDirectionalShadow.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/SceneFrameGBuffer.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/SceneFrameAmbientOcclusion.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/SceneFrameContactShadowVisibility.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/SceneFrameVolumetricCloud.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/SceneFrameDeferredDirectionalLighting.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/SceneFrameBaseScene.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/SceneFrameSceneColor.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/SceneFramePostProcess.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/SceneFrameEditorAssistance.cpp`
- `Engine/Tests/Native/RenderCoreTests/Private/RenderGraphTests.cpp`
- `Engine/Tests/Native/RenderCoreTests/Private/ShaderFoundationTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/RendererSceneContractTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/DirectionalShadowBaselineVulkanTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/GBufferQualificationTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/EditorGridVulkanTests.cpp`
