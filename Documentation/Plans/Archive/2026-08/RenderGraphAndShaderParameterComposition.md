# Render Graph and Shader Parameter Composition Plan

Summary: Unify graph dependency declarations and reflected shader bindings on one graph-owned pass parameter object for graphics and compute.

Last reviewed: 2026-08-29

Status: Archived
Completed: 2026-08-29

## Current Status

Roadmap Milestone 3 is complete. RenderCore graph resource metadata can now
carry a reflected `Texture`, `StorageImage`, or `StorageBuffer` role on the
exact same wrapper field. Shader declarations mark graph-required bindings,
cached reflection continues to own descriptor coordinates, and callback-
lifetime `FRenderGraphShaderParameters` scopes enforce exact owner identity,
pass domain, binding type, optional/array completeness, graph access, and
physical backing before one atomic RHI parameter command. Texture subresource
ranges become counted exact views and buffer byte ranges remain exact through
RHI/Vulkan replay. Captures retain the stable field path plus shader binding
name and type.

`Scene.ContactShadowVisibility` is the bounded pair of production pilots. Its
fragment route binds five graph-owned SRVs from
`FContactShadowGraphicsPassParameters`; its compute route binds the same five
SRVs plus one graph-owned UAV from `FContactShadowComputePassParameters`.
Renderer-created uniform buffers still use ordinary typed shader parameters,
but neither route copies graph textures into that object. Route selection,
attachments, graph topology, telemetry, draw/dispatch work, and fallback
ownership remain unchanged.

Validation passed for 94 `RenderContractTests`, 41
`RenderShaderContractTests`, 39 `RendererSceneContractTests`, the focused
composition/production fixtures, and the complete Vulkan
`DirectionalShadowBaselineVulkanTests` qualification, which exercises both
compute and forced-fragment contact-shadow routes. The complete `all` target
builds successfully. The repository-wide `fast-all` build remains blocked in
unrelated `EditorAssetWorkflowTests` code by its missing
`AssetTools/IAssetTools.h` include. Lasting Render Graph and Shader Parameters
contracts and the owning roadmap record the landed behavior. Milestone 4 entry
evidence is satisfied and remains separate contributor-migration work.

## Goal

Make one immutable graph-owned pass parameter object the authoritative source
for both Render Graph resource declarations and reflected shader resource
bindings. A migrated callback must bind graphics or compute shaders directly
from exact declared parameter members without constructing a second RHI-only
resource table, while preserving the existing compiler, reflection, RHI,
Vulkan, failure, capture, and rendering contracts.

## Scope

- A composable metadata contract in which Render Graph and shader reflection
  views describe fields of the same standard-layout C++ parameter object.
- Shader-visible graph texture and buffer fields for SRV/UAV access, exact
  texture subresources and buffer ranges, required and optional forms, fixed
  arrays, and nested parameter structures.
- Pass-scoped graphics and compute shader submission that resolves graph
  handles to counted RHI views only while the owning callback executes.
- Validation of graph use, pass domain, shader binding type, reflected array
  extent, field identity, optional availability, and access compatibility.
- Stable field-path diagnostics and capture evidence connecting one graph use
  to its shader binding role without physical addresses.
- Synthetic RenderCore/ShaderFoundation coverage plus one bounded graphics
  production pilot and one bounded compute production pilot.
- Preservation of current scene topology, shader bytecode/reflection ABI,
  draw/dispatch work, descriptor publication, output, failure, recovery, and
  accepted CPU/GPU budgets.
- Lasting Render Graph and Shader Parameters contract updates and roadmap
  Milestone 3 handoff.

## Non-Goals

- Migrating every scene contributor, removing
  `FSceneFrameGraphContributorContext`, or completing roadmap Milestone 4.
- Redesigning Vulkan descriptor layouts, pools, snapshot caching, pipeline
  ownership, bindless policy, or native synchronization.
- Adding partially bound descriptor arrays or treating null descriptors as a
  portable fallback policy.
- Moving shader selection, topology, fallback selection, material resolution,
  uniform construction, PSO ownership, or feature algorithms into RenderCore.
- Folding attachments, samplers, uniform buffers, push constants, typed graph
  values, and shader SRV/UAVs into one undifferentiated wrapper kind.
- Replacing the canonical Render Graph compiler, shader reflection source of
  truth, counted RHI view model, or Vulkan validation authority.
- Physical transient aliasing, async compute scheduling, split barriers,
  parallel command recording, pass merging, compiler reordering, or persistent
  graph reuse.
- Changing rendered output, pass topology, draw/dispatch counts, telemetry
  meaning, or qualification thresholds as incidental migration work.

## Design Decisions and Invariants

- **One allocation and object, two structural views.** Render Graph metadata
  and shader metadata may remain separate types, but every composed entry names
  the same owning structure, byte offset, element layout, and stable field path.
  A callback-owned mirror parameter object does not satisfy this plan.
- **Logical handles remain frozen storage.** Graph-owned parameters store graph
  handles and exact ranges, never execution-lifetime `FRHITexture*`,
  `FRHIBuffer*`, descriptor records, or backend handles. Physical resolution
  happens only through the current pass capability.
- **Graph declaration remains authoritative for access.** Shader composition
  annotates an existing graph resource field; it does not append a hidden use
  or create a second dependency path. Dependency, lifetime, culling, ordering,
  and transitions continue to come from the canonical lowered graph use.
- **Reflection remains authoritative for binding coordinates.** Set, binding,
  array element, and native descriptor type continue to come from compiled
  shader reflection and cached `FShaderParameterBinding` records. Graph
  metadata does not encode backend descriptor coordinates.
- **Binding is pass-scoped and member-exact.** The composed submission path
  accepts the immutable owning parameter object and its active resolver. It may
  resolve only exact members declared by that pass and only while its callback
  executes; copied wrappers, raw handles, foreign objects, and cross-pass use
  fail before RHI submission.
- **Access compatibility is explicit.** SRV bindings require a graph read or
  read/write declaration and shader-readable access for the active pass
  domain. UAV bindings require graph write or read/write authority and matching
  unordered-access capability. A declared attachment is not implicitly a
  shader binding, and incompatible aliases in one pass remain invalid.
- **Shader subsets are allowed, undeclared resources are not.** Multiple shader
  types or stages may consume deterministic subsets of one parameter object.
  Every graph-backed reflected resource in a migrated shader must map to one
  compatible field; graph-only attachments, values, and ordering fields need
  not appear in shader metadata.
- **Required descriptors remain complete.** A disengaged optional graph field
  is permitted only when the selected shader does not require that binding.
  If active reflection requires the field, absence fails before draw/dispatch.
  Renderer-owned candidate/fallback selection remains before graph submission;
  this plan does not introduce portable null descriptors.
- **Arrays are exact and ordered.** Fixed arrays lower and bind in increasing
  element order. Graph extent, shader metadata extent, and reflection extent
  must agree for an active binding; partially populated required arrays fail
  deterministically.
- **Non-graph shader inputs stay typed.** Samplers, uniform-buffer views, and
  other non-graph resources may remain ordinary shader parameter fields on the
  same object when they do not represent a graph resource. Their existing
  reflection and RHI validation remains intact.
- **Failure is atomic.** Malformed composed metadata, incompatible graph/shader
  capabilities, unavailable required fields, wrong shader frequency/domain,
  or incomplete physical backing invokes no affected callback submission,
  draw, dispatch, descriptor write, or partial result publication.
- **Diagnostics are stable.** Errors and captures name pass, owning structure,
  field path, graph use/access, shader name/frequency, binding name/type, and
  array element where relevant. They contain no pointer values, allocation
  order, RTTI spelling, or backend object identity.
- **Compatibility is bounded.** Existing `SetShaderParameters` with ordinary
  RHI-pointer parameter structs remains available for unmigrated renderers.
  A migrated pass does not construct such a mirror for graph-backed fields.

## Current Foundations and Gaps

| Area | Reusable foundation | Gap owned by this plan |
| --- | --- | --- |
| Graph parameters | Graph-owned aligned storage, immutable parameterized `AddPass`, exact texture/buffer/value wrappers, nested/optional/array traversal, field paths, and pass-scoped resolution | Shader-visible fields require callback-side resolution and copying into a second object |
| Shader metadata | Compile-time `FShaderParametersMetadata`, stable member offsets, included metadata, reflection validation, cached bindings, typed graphics/compute submission | Metadata reads ordinary RHI resources and cannot consume graph wrappers from the owning pass object |
| RHI views | Counted texture/buffer views retain resources and carry exact native interpretation through replay | No composed path converts exact declared graph ranges into those views during shader submission |
| Validation | Graph declaration validation and shader reflection validation each fail deterministically | No cross-validation proves that binding kind, graph use/access, pass domain, optionality, and array extent agree |
| Scene authoring | GBuffer proves graph-owned parameterized pass execution; production renderers use typed shader parameters | Graph pass parameters and shader `FParameters` remain separate at production call sites |
| Observation | Graph captures own parameter field paths; shader initialization reports reflection mismatches | No normalized evidence connects a graph field path to the reflected binding it supplies |
| Backend | Graphics/compute shader parameters flow through RHI to Vulkan descriptor snapshots with complete occupancy validation | Backend correctness cannot detect an omitted or weaker graph declaration before recording |

## Implementation Stages

### Stage 0: Freeze composition, pilot, and parity contracts

- [x] Inventory Render Graph and shader parameter metadata layouts, macros,
  included/nested traversal, resource wrapper forms, cached binding records,
  typed submission overloads, graphics/compute frequency rules, and current
  failure points.
- [x] Inventory representative production callbacks that both declare graph
  resources and build shader resource structs; record field-by-field duplicate
  texture/buffer, range, access, optional, array, sampler, and uniform data.
- [x] Select one bounded graphics pilot and one bounded compute pilot from the
  current scene contributors, with their exact shader classes, active routes,
  fallback ownership, callback boundaries, and reasons they exercise the
  required surface without becoming a whole-scene migration.
- [x] Specify the public composed member vocabulary, metadata ownership and
  discovery API, shader-subset rules, nested/included layout rules, exact
  texture/buffer view construction, and pass-scoped submission signature.
- [x] Freeze the compatibility matrix for SRV/UAV binding type versus graph
  resource kind, use, RHI access, pass domain, range, optionality, and fixed
  array extent, including deterministic error text.
- [x] Freeze manual-versus-composed structural oracles and production pilot
  captures, dependencies, transitions, descriptor identities, draw/dispatch
  counts, images/readbacks, failures, recovery, CPU, memory, and synchronized
  GPU budgets.
- [x] Add focused failing fixtures for mismatched owner layout/offset, binding
  kind, access/domain, required optional, array extent, copied/foreign member,
  wrong shader frequency, missing backing, and duplicate declaration authority.

#### Acceptance Gate

- Every supported composed field has one selected storage, graph lowering,
  reflection, physical resolution, RHI view, failure, and diagnostic contract.
- Graphics and compute pilots have frozen duplicate-field inventories and
  structural/rendered parity oracles, and neither pilot expands into Milestone 4.
- Unsupported null, partially-bound, dynamic-array, attachment-binding, and
  backend descriptor behaviors are explicit rather than left to call sites.

### Stage 1: Compose graph and shader metadata on one object

- [x] Add structural metadata support that lets shader resource declarations
  reference texture/buffer graph wrapper fields on the exact same owning C++
  parameter object without changing canonical graph lowering.
- [x] Validate owning structure size/alignment, member offset/extent,
  wrapper/resource kind, nested and included paths, unique field authority, and
  deterministic traversal order across both metadata views.
- [x] Preserve ordinary sampler, uniform-buffer, and non-graph shader fields on
  the composed object through existing shader metadata and reflection rules.
- [x] Build a normalized composition record that links stable graph field paths
  to cached shader bindings while retaining shader reflection as the source of
  descriptor coordinates.
- [x] Reject malformed or inconsistent composition during graph/pass or shader
  preparation before a compiled pass can record work.
- [x] Cover flat, nested, included, graphics-stage subset, compute, graph-only,
  shader-only non-graph, duplicate, overlapping, and reordered metadata cases.

#### Acceptance Gate

- One allocation can expose graph and shader metadata for the same wrapper
  fields, and both views agree on owner layout and stable field identity.
- Graph compilation produces the same canonical uses, dependencies,
  transitions, lifetimes, and culling as the existing parameter-only oracle.
- Equal composed declarations produce byte-stable pointer-free normalized
  records and errors; malformed layouts publish no pass or shader binding.

### Stage 2: Bind declared graph resources through the pass capability

- [x] Add a typed composed shader-submission path that accepts the selected
  shader, immutable owning pass parameters, and active pass resolver.
- [x] Resolve graph texture/buffer wrappers to exact counted RHI views using the
  declared subresource or byte range, while retaining the existing resource
  lifetime and replay behavior.
- [x] Merge resolved graph-backed resources with ordinary sampler,
  uniform-buffer, and other supported shader fields without exposing backend
  descriptor coordinates to the callback.
- [x] Enforce exact member identity, pass ownership, callback lifetime, shader
  frequency, graphics/compute domain, read/write capability, and complete
  physical backing before forwarding any resource list to RHI.
- [x] Preserve cached reflection bindings, complete descriptor occupancy,
  graphics/compute PSO ownership checks, command replay, and Vulkan snapshot
  canonicalization.
- [x] Cover multiple shaders/stages consuming subsets of one object, repeated
  binding, culled passes, preparation failure, callback failure, and compiled
  graph destruction.

#### Acceptance Gate

- A migrated callback binds graph-backed shader resources directly from its
  immutable pass parameter object and cannot observe or submit an undeclared,
  copied, foreign, unavailable, or incompatible member.
- Exact ranges and array elements reach the same RHI/Vulkan descriptor records
  as the manually resolved oracle, with no callback-owned mirror resource table.
- Invalid composition or backing invokes no affected RHI parameter command,
  descriptor materialization, draw, dispatch, or result publication.

### Stage 3: Complete SRV/UAV, optional, array, and diagnostics coverage

- [x] Implement and validate texture/buffer SRV and UAV combinations for
  graphics and compute, including read, write, and read/write graph authority.
- [x] Implement fixed arrays and nested/included composed fields with exact
  ordered binding and reflection extent validation.
- [x] Implement optional route fields so inactive shader subsets may omit them
  while active required reflection fails deterministically when unavailable;
  do not introduce null or partially-bound descriptor semantics.
- [x] Reject access weakening, attachment-as-binding ambiguity, overlapping
  incompatible ranges, duplicate shader aliases, missing graph declarations,
  missing reflected bindings, and reflection type/extent mismatches.
- [x] Extend dumps/captures or shader inspection evidence with the normalized
  pass/field-to-shader-binding relationship while preserving existing manual
  output when no composition exists.
- [x] Measure composed metadata traversal, resolution, capture growth, command
  resource counts, and descriptor snapshot behavior against the Stage 0 gates.

#### Acceptance Gate

- Table-driven tests cover every supported resource, use/access, domain,
  optional, array, nested, shader-subset, and failure combination.
- Captures and errors identify the exact pass, field, graph capability, shader,
  binding, and array element without addresses or unstable type spellings.
- Composition adds no second compiler or descriptor policy and remains inside
  frozen structural, CPU, memory, and capture budgets.

### Stage 4: Migrate bounded graphics and compute production pilots

- [x] Convert the selected graphics pilot to one graph-owned composed parameter
  object and remove its callback-side graph-resource-to-shader-resource mirror.
- [x] Convert the selected compute pilot through the same public composition
  and submission path, including its route selection and UAV/SRV authority.
- [x] Keep contributor signatures, frame-wide context, pass topology, shader
  selection, fallback policy, feature algorithms, attachments, typed results,
  telemetry, and publication behavior otherwise unchanged.
- [x] Remove manual graph uses only where the pilot parameter metadata becomes
  authoritative; do not migrate adjacent contributor chains to satisfy this
  plan mechanically.
- [x] Compare normalized captures, dependencies, transitions, RHI commands,
  descriptor identities, draw/dispatch counts, output images/readbacks,
  disabled/failure routes, recovery, resize, and multi-view behavior.
- [x] Run representative graphics and compute Vulkan validation and synchronized
  production-route qualification without raising frozen gates.

#### Acceptance Gate

- Both pilots bind graph-backed shader resources from their owning pass object
  and contain no duplicate RHI texture/buffer table for those fields.
- Graphics and compute routes preserve graph structure, shader bindings,
  output, telemetry, failure atomicity, recovery, and accepted budgets.
- The implementation proves the API surface needed by Milestone 4 without
  removing the broad contributor context or migrating unrelated passes.

### Stage 5: Harden, document, and hand off Milestone 3

- [x] Run focused and aggregate RenderCore/ShaderFoundation tests, Renderer
  scene contracts, representative RHI/Vulkan validation, rendered-output and
  recovery fixtures, pilot qualification, and the required build tier under
  repository guidance.
- [x] Record composed field/binding counts, capture size, dependency/transition
  structure, RHI parameter resources, descriptor snapshots, compile/execute
  CPU, memory, output, and synchronized GPU evidence against frozen gates.
- [x] Update the lasting Render Graph and Shader Parameters contracts with
  composition ownership, supported fields, binding, validation, diagnostics,
  compatibility, and failure rules.
- [x] Update the roadmap Milestone 3 state and record the evidence-based entry
  disposition for the separate Scene Parameter Migration plan.
- [x] Record exact validation, close only passed checklists, and prepare the
  repository-required plan/stage commit provenance.

#### Acceptance Gate

- Focused, aggregate, build, Vulkan, rendered-output, recovery, documentation,
  structural, memory, CPU, and synchronized GPU gates pass with recorded evidence.
- Code, tests, captures, lasting contracts, and the roadmap agree that one pass
  parameter object supplies graph declaration and shader binding in both pilots.
- Milestone 4 is either ready for a newly created bounded plan or remains
  proposed with its missing entry evidence stated explicitly.

## Validation Matrix

| Concern | Required evidence |
| --- | --- |
| Shared layout | Both metadata views agree on owner size/alignment, exact field offsets/extents, nesting/inclusion, and deterministic field paths |
| Graph parity | Manual/parameter-only/composed graphs preserve normalized uses, versions, dependencies, culling, lifetimes, transitions, and output roots |
| Reflection | Binding names, types, frequencies, array extents, shader subsets, missing fields, and cached binding construction match stable reflection fixtures |
| Capability | SRV/UAV versus graph kind/use/access/domain is table-tested; copied, raw, foreign, wrong-pass, wrong-frequency, and overlapping authority fail |
| Physical resolution | Exact texture subresources and buffer byte ranges become counted retained RHI views only during the owning callback |
| Optional and arrays | Inactive optional shader subsets are absent; active required fields and every required array element are complete; partial binding fails |
| RHI/Vulkan | Graphics/compute PSO ownership, command replay, descriptor occupancy, snapshot identity, validation layers, and completion-safe retention pass |
| Failure | Metadata, compile, backing, binding, callback, execution, device loss, and recovery paths issue no partial draw/dispatch/result publication |
| Production pilots | Graphics and compute captures, transitions, bindings, draw/dispatch identities, images/readbacks, telemetry, resize, and multi-view match frozen oracles |
| Budgets | Metadata/binding work, capture bytes, command resource counts, descriptor snapshots, CPU, memory, and synchronized GPU timings remain inside frozen gates |
| Documentation | Changed/all documentation plus all-plan and all-roadmap lifecycle validators pass after final edits |

## Definition of Done

- One graph-owned immutable parameter object can declare graph resources and
  supply reflected shader resource bindings for graphics and compute.
- Graph-backed shader fields lower once into the canonical compiler and resolve
  once through the exact pass capability; migrated callbacks own no mirror
  RHI texture/buffer table for those fields.
- SRV/UAV access, exact ranges, required/optional fields, fixed arrays,
  nested/included layouts, shader subsets, and non-graph shader resources have
  selected and tested contracts.
- Invalid metadata, access, domain, reflection, member identity, optional/array
  completeness, or backing fails deterministically before affected recording.
- Existing shader reflection, RHI counted views, Vulkan descriptors, graph
  semantics, output, telemetry, recovery, and accepted budgets remain intact.
- One bounded graphics and one bounded compute production pilot use the shared
  path with structural and rendered evidence.
- Lasting Render Graph and Shader Parameters documentation plus the roadmap
  reflect the landed contract and actual validation evidence.
- Changes are staged and committed with repository-required plan provenance
  after successful validation.

## Deferred Follow-ups

- Contributor-by-contributor narrow typed input/output migration, broad frame
  context removal, and complete production parameterization in Milestone 4.
- Parameter-aware scene inspection, authoring enforcement, and retirement or
  bounding of legacy manual APIs in Milestone 5.
- Uniform-byte block or push-constant unification beyond the existing typed
  shader resource submission contract.
- Bindless and partially-bound descriptor arrays if independently justified by
  renderer requirements and backend capability evidence.
- Physical transient aliasing, queue scheduling, split barriers, parallel
  recording, pass merging, compiler reordering, and persistent graph reuse,
  each under its separate evidence gate.

## Related Documentation

- [Render Graph Parameter-Driven Authoring Roadmap](../../../Roadmaps/Archive/2026-08/RenderGraphParameterDrivenAuthoring.md)
- [Render Graph Pass Parameters Foundation](RenderGraphPassParametersFoundation.md)
- [Render Graph External Registration and Typed Values](RenderGraphExternalRegistrationAndTypedValues.md)
- [Render Graph](../../../Runtime/Rendering/RenderGraph.md)
- [Shader Parameters](../../../Runtime/Rendering/ShaderParameters.md)
- [Graphics State and Bindings](../../../Runtime/Rendering/GraphicsStateAndBindings.md)
- [RHI Resource Views and Transfers](../../../Runtime/Rendering/RHIResourceViewsAndTransfers.md)
- [RHI Resource Transitions](../../../Runtime/Rendering/RHIResourceTransitions.md)
- [Renderer Frame Preparation and Render Graph Execution](../../../Runtime/Rendering/RendererFramePreparation.md)
- [Renderer Resource Recovery](../../../Runtime/Rendering/RendererResourceRecovery.md)
- [Testing](../../../Agents/Testing.md)
- [Build and Run](../../../Agents/BuildAndRun.md)

## Related Code

- `Engine/Source/Runtime/RenderCore/Public/RenderGraph.h`
- `Engine/Source/Runtime/RenderCore/Private/RenderGraph.cpp`
- `Engine/Source/Runtime/RenderCore/Public/Shader/Shader.h`
- `Engine/Source/Runtime/RenderCore/Private/Shader/Shader.cpp`
- `Engine/Source/Runtime/RHI/Public/RHIShaderParameters.h`
- `Engine/Source/Runtime/Renderer/Private/Renderers/SceneFrameGraphContributors.h`
- `Engine/Source/Runtime/Renderer/Private/Renderers/SceneFrameContactShadowVisibility.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/SceneFramePostProcess.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/ContactShadowRenderer.h`
- `Engine/Source/Runtime/Renderer/Private/Renderers/ContactShadowRenderer.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/PostProcessRenderer.h`
- `Engine/Source/Runtime/Renderer/Private/Renderers/PostProcessRenderer.cpp`
- `Engine/Tests/Native/RenderCoreTests/Private/RenderGraphTests.cpp`
- `Engine/Tests/Native/RenderCoreTests/Private/ShaderFoundationTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/RendererSceneContractTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/GBufferQualificationTests.cpp`
