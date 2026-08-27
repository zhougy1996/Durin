# Render Graph Pass Parameters Foundation Plan

Summary: Make typed pass parameters the single declaration source for graph dependencies, access, attachments, and pass-scoped resource resolution.

Last reviewed: 2026-08-28

Status: Active
Completed:

## Current Status

Stage 0 is ready to begin. The completed Render Graph already compiles exact
imperative `UseTexture`, `UseBuffer`, `UseToken`, attachment, and managed-use
declarations into dependencies, lifetimes, culling, transition batches, and
deterministic captures. Existing shader parameter infrastructure provides
compile-time member metadata, typed parameter structures, reflected resource
validation, and typed graphics/compute submission.

The selected path adds graph-specific metadata and graph-owned parameter
storage in RenderCore, lowers parameter fields to the existing canonical use
model, and preserves the current explicit APIs for unmigrated callers. A
GBuffer production pilot proves created textures, managed color/depth
attachments, a typed logical token, optional topology, pass-scoped resolution,
and exact scene capture parity without attempting the full scene migration.

No implementation work or validation evidence has been recorded yet.

## Goal

Establish the minimum complete parameter-driven pass contract needed for later
UE-style Render Dependency Graph authoring:

- allocate one typed pass parameter object with graph lifetime;
- derive graph uses and execution capabilities from its static metadata;
- freeze the object when it is submitted to `AddPass`;
- lower declarations to the existing graph compiler without changing its
  dependency, culling, lifetime, transition, or scheduling semantics;
- expose parameter field paths in validation and capture diagnostics; and
- migrate one bounded production contributor so a pass has one resource
  declaration authority rather than a callback plus a separate `Use*` block.

## Scope

- RenderCore graph parameter member types for textures, buffers, logical
  tokens, color attachments, depth/stencil attachments, and managed texture
  access.
- Static metadata describing member field path, offset, optionality, exact
  range, read/write intent, access, discard, load/store, and result access.
- A builder-owned aligned parameter arena with explicit construction,
  destruction, freeze, compile-transfer, execute, and graph-destruction rules.
- A parameterized `FRenderGraphBuilder::AddPass` overload that traverses one
  parameter object and appends canonical internal uses before compilation.
- Pass-scoped typed resolution that rejects handles absent from the submitted
  parameter structure while preserving the current `FRenderGraphPassResources`
  execution boundary during this plan.
- Deterministic parameter field paths in compile errors, dumps, captures, and
  use records.
- Structural and equivalence coverage in RenderCore tests for all new member
  forms and invalid declarations.
- Migration of `FGBufferGraphContributor` as the single production pilot,
  including removal of its obsolete copied persistent-input helper and manual
  use block.
- Representative Renderer scene capture, failure, output, and Vulkan
  validation sufficient to prove pilot parity.
- Updates to lasting Render Graph documentation for the implemented foundation
  and to the owning roadmap status.

## Non-Goals

- Migrating contributors other than GBuffer or removing the broad
  `FSceneFrameGraphContributorContext`, frame-wide resources, or channel bag.
- Unifying graph parameter fields directly with shader descriptor binding;
  composing both metadata views is a later roadmap milestone.
- Moving external texture/buffer deduplication from the scene composer into
  RenderCore, changing imported resource ownership, or redesigning final-state
  policy.
- Replacing untyped graph tokens and Renderer-owned result storage with generic
  typed graph values beyond the bounded token wrapper needed by the pilot.
- Changing GBuffer algorithms, formats, attachment actions, render-pass
  layouts, shaders, PSOs, topology policy, telemetry, failure semantics, or
  output.
- Rewriting dependency compilation, stable topological ordering, culling,
  logical transient allocation, backing resolution, transition synthesis,
  RHI state validation, or Vulkan layout lowering.
- Physical transient aliasing, async compute, multiple queues, split barriers,
  pass merging, scheduling reordering, parallel command recording, persistent
  graph reuse, or a public pass registry.
- Removing the legacy non-parameterized `AddPass` and `Use*` APIs while other
  production callers still require them.
- Introducing a frame-wide parameter structure, mutable blackboard, generic
  contributor service locator, or nested graph.

## Design Decisions and Invariants

1. **Parameter metadata lowers to the existing compiler model.** The new layer
   produces the same canonical texture, buffer, token, attachment, and managed
   uses already accepted by `FRenderGraphBuilder`. It does not implement a
   second dependency or barrier compiler.
2. **One parameter object describes one pass.** It is allocated by the builder,
   populated before `AddPass`, logically frozen at submission, retained through
   compiled execution, and destroyed with graph-owned storage. A parameterized
   pass cannot replace or mutate the object after submission.
3. **Graph metadata is independent from shader descriptor metadata in this
   plan but uses the same structural pattern.** Both systems describe typed C++
   fields through static metadata. This plan must not make later composition on
   one parameter object impossible, but it does not change shader binding ABI.
4. **Public wrappers carry intent in their type or metadata.** Texture/buffer
   handles are not accepted as unqualified raw members. Each graph member
   declares read/write direction, exact access family, range, discard, and
   attachment actions required to reproduce the current `Use*` call.
5. **Optional means absent use.** An unassigned optional member produces no
   graph use. It does not select fallback, infer topology, or silently grant a
   default resource.
6. **Parameter traversal is deterministic and allocation-free after builder
   storage is reserved.** Static metadata owns no per-frame strings. Field
   paths in diagnostics are derived in stable declaration order and captured
   as owning strings only where the existing diagnostic/capture value requires
   ownership.
7. **Mixing declaration authorities is rejected.** A parameterized pass may
   not manually append an overlapping declaration. During migration the old
   APIs remain valid only for non-parameterized passes. The GBuffer pilot
   deletes its previous use declarations in the same stage that adopts the new
   path.
8. **Execution remains pass-scoped.** `FRenderGraphPassResources` remains the
   physical lookup boundary. The parameter resolver provides typed convenience
   over it and validates that every lookup originates from the submitted
   parameter object; callbacks do not gain access to arbitrary graph handles.
9. **Compile and backing failure remain atomic.** Invalid metadata, foreign
   handles, illegal access/domain combinations, missing producers, or absent
   retained backing prevent all pass callbacks and RHI transition recording.
10. **GBuffer is a semantic pilot, not a feature redesign.** Its pass identity,
    created resources, managed attachments, depth/color load/store, logical
    completion ordering, callback behavior, capture topology, and rendered
    output remain exact.
11. **The current graph budget remains truthful.** Metadata traversal cost and
    any capture growth are measured against the existing structural and CPU
    ceilings. A budget increase requires independent evidence and is not an
    automatic consequence of this refactor.
12. **The roadmap owns future sequencing.** External registration, generic
    typed values, graph/shader parameter composition, complete scene migration,
    and advanced execution optimizations are not pulled into this plan when an
    isolated API or pilot inconvenience appears.

## Current Foundations and Gaps

| Area | Current foundation | Plan gap |
| --- | --- | --- |
| Graph declarations | Public exact `UseTexture`, `UseBuffer`, `UseToken`, attachment, and managed-resource APIs | Uses are authored after `AddPass` and can drift from the callback's captures and lookups |
| Compilation | Deterministic validation, versions, minimal hazard frontier, culling, lifetimes, transitions, budgets, and atomic failure | No parameter metadata traversal or field-level declaration provenance |
| Execution | `FRenderGraphPassResources` rejects foreign and undeclared lookup and resolves retained physical resources | Callbacks manually repeat each handle lookup and no typed parameter capability is available |
| Storage | Builder and compiled graph own resources, uses, callbacks, and captures for the graph lifetime | No aligned graph-owned arena or destructor registry for typed pass parameter objects |
| Shader metadata | Static field metadata, nested/included layouts, reflected validation, and typed graphics/compute binding | Graph handles and access intent are not represented; direct shader composition is deliberately deferred |
| Scene GBuffer | One contributor creates four targets, adds one pass, declares four managed color attachments, managed depth, and one completion token | The contributor expands a broad Context and contains an unused copied persistent-input helper plus a separate manual use block |
| Diagnostics | Stable dump/capture records pass/resource/use/dependency/transition identities | Uses cannot identify the parameter field that declared them |
| Tests | Extensive RenderGraph structural tests, hidden-resource lookup test, managed-attachment tests, and Renderer scene captures | No graph parameter layout, lifetime, lowering-equivalence, mutation, or pilot parity fixtures |

## Implementation Stages

### Stage 0: Freeze the parameter contract and parity oracle

- [ ] Inventory every current `FRenderGraphBuilder::Use*` declaration form,
  normalized internal use record, compile diagnostic, dump/capture field, and
  `FRenderGraphPassResources` lookup rule that parameter lowering must preserve.
- [ ] Freeze the GBuffer pilot's pass/resource/token identities, attachment
  load/store and result access, dependencies, logical lifetimes, transition
  batches, culling disposition, budget statistics, callback result, failure
  behavior, and representative rendered output.
- [ ] Select the public wrapper vocabulary and static metadata schema for
  texture, buffer, token, color attachment, depth/stencil attachment, managed
  texture, optional member, and nested-parameter forms.
- [ ] Specify builder-owned parameter construction, alignment, destruction,
  freeze, compile-transfer, callback capture, and graph-lifetime rules,
  including the initially supported C++ type traits.
- [ ] Specify how parameter field paths enter canonical uses, compile errors,
  dump/capture output, and equality fixtures without destabilizing existing
  pointer-free diagnostics.
- [ ] Specify compatibility and rejection rules for legacy `AddPass`, manual
  `Use*`, parameterized `AddPass`, overlapping declarations, post-submission
  mutation, and unsupported parameter layouts.
- [ ] Add or update focused contract fixtures that fail before implementation
  when parameter lowering, lifetime, capability, or GBuffer parity is absent.

#### Acceptance Gate

- Every currently supported graph declaration maps to one selected parameter
  member form or is explicitly deferred with no GBuffer dependency.
- Parameter ownership, metadata, immutability, compatibility, diagnostics, and
  pilot parity have one non-conflicting selected contract.
- The baseline capture and test oracle can detect missing/extra uses,
  dependencies, transitions, callbacks, or output changes.

### Stage 1: Implement graph parameter metadata and storage

- [ ] Add RenderCore graph parameter member metadata with stable declaration
  order, byte offset, field name, optionality, resource kind, use/access,
  range, discard, attachment load/store, and managed result access.
- [ ] Add typed public parameter wrappers and declaration helpers that preserve
  graph-local handle ownership and cannot be confused with resolved RHI
  resources.
- [ ] Add compile-time/static validation for supported layouts and runtime
  validation for malformed dynamic conditions that cannot be rejected by the
  type system.
- [ ] Add `FRenderGraphBuilder::AllocParameters<T>()` backed by aligned
  graph-owned storage with exactly-once construction and destruction.
- [ ] Transfer parameter storage safely into a successful compiled graph and
  release it correctly on builder destruction, compile failure, execution
  failure, and normal completion.
- [ ] Cover trivial and non-trivial supported parameter lifetime, alignment,
  nesting, arrays, optionals, graph destruction, and compile-transfer cases.

#### Acceptance Gate

- Supported parameter types have deterministic complete metadata and correctly
  aligned graph-lifetime storage.
- Sanitized/debug native tests observe no leak, double destruction,
  use-after-free, post-graph access, or unstable field ordering across success
  and every early-exit path.
- Shader parameter declaration and binding behavior remains unchanged.

### Stage 2: Lower parameterized passes into canonical graph uses

- [ ] Add the immutable parameterized `AddPass` overload and attach one
  parameter metadata/object pair to its pass record.
- [ ] Traverse texture, buffer, token, attachment, managed-resource, optional,
  array, and nested members into the existing canonical declaration functions
  without duplicating compiler semantics.
- [ ] Reject foreign handles, invalid ranges, illegal use/access/domain pairs,
  conflicting or duplicate fields, unsupported layouts, and mixed manual/
  parameter declaration authority before recording.
- [ ] Preserve exact dependency, value-version, hazard-frontier, culling,
  lifetime, final-state, and transition results for equivalent manual and
  parameterized synthetic graphs.
- [ ] Extend use provenance, errors, dumps, and owning captures with stable
  parameter field paths while retaining pointer-free deterministic output.
- [ ] Measure parameter allocation and traversal against the RenderGraph
  foundation compile budget and add a bounded regression fixture.

#### Acceptance Gate

- Table-driven equivalence fixtures cover every supported member form and
  compare complete compiled graph structure against the manual API oracle.
- Invalid parameterized graphs fail before execution with the pass and exact
  field path in the diagnostic, and no transition or callback records.
- Equal parameter declarations produce byte-for-byte stable dump/capture
  output and remain within the accepted structural and CPU budgets.

### Stage 3: Add typed pass-scoped parameter resolution

- [ ] Add a typed resolution view over `FRenderGraphPassResources` that accepts
  only members from the current pass parameter object and produces the
  corresponding RHI texture/buffer or attachment view.
- [ ] Preserve null behavior only for declared optional members; unavailable
  required retained backing remains an execution-preparation failure rather
  than callback-visible absence.
- [ ] Reject attempts to resolve a raw foreign handle, a parameter member from
  another pass, a wrong resource kind, or a member not declared for execution.
- [ ] Define callback signatures and captures so the immutable parameter object
  and typed resolver outlive recording but cannot escape the compiled graph.
- [ ] Extend tests for graphics, compute, copy, attachment, optional, culled,
  unavailable, and wrong-pass resolution.

#### Acceptance Gate

- A parameterized callback can obtain every resource it declared and no other
  graph resource.
- Hidden or cross-pass access fails deterministically in focused tests without
  weakening the existing `FRenderGraphPassResources` contract.
- Compile/cull/backing failure invokes no parameterized callback and exposes no
  partially resolved resource table.

### Stage 4: Migrate the GBuffer production pilot

- [ ] Define feature-owned GBuffer pass parameters beside the contributor with
  four optional managed color attachments, managed depth, and completion-token
  write capability.
- [ ] Convert `FGBufferGraphContributor` to parameterized `AddPass` and typed
  pass-scoped resolution while preserving feature-specific record inputs and
  callback behavior.
- [ ] Remove the contributor's unused `DeclarePersistentGraphicsInputs` copy
  and its manual `UseToken`/managed-attachment declaration block.
- [ ] Reject any remaining mixed declaration authority for the GBuffer pass and
  verify its callback captures no undeclared graph resource handle.
- [ ] Compare production captures for disabled, required, isolated-deferred,
  present, offscreen, resize, and injected target-preparation failure routes.
- [ ] Run focused Renderer and Vulkan integration/rendered-output validation
  selected under repository testing guidance.

#### Acceptance Gate

- GBuffer has one parameter-driven declaration authority and no local manual
  graph-use or persistent-input helper code.
- Pass/resource/token names, scheduled topology, dependencies, lifetimes,
  managed attachment semantics, transitions, culling, result status,
  telemetry, failures, and rendered output match the Stage 0 oracle.
- Unmigrated contributors continue to use the compatibility path unchanged.

### Stage 5: Harden, document, and hand off the foundation

- [ ] Run the focused RenderCore parameter/lowering/lifetime suite, complete
  RenderCoreTests target, selected Renderer scene contracts, representative
  Vulkan validation and rendering fixtures, and the required build tier from
  repository guidance.
- [ ] Record compile/execute CPU, capture size, graph statistics, transition,
  and output evidence against frozen budgets; resolve any regression rather
  than silently raising the scene budget.
- [ ] Update the lasting Render Graph contract with parameter ownership,
  metadata, lowering, immutability, execution capability, compatibility, and
  diagnostic rules.
- [ ] Update the roadmap current status, Milestone 1 state, and next child-plan
  entry-gate disposition from actual validation evidence.
- [ ] Record exact validation and any deliberately non-standard coverage in
  this plan, complete all passed checklists, and prepare the required plan/stage
  commit provenance.

#### Acceptance Gate

- All focused, aggregate, build, Vulkan, renderer parity, documentation, and
  budget gates selected by this plan pass with recorded evidence.
- Lasting contracts, code, tests, captures, and the active roadmap agree on one
  parameter foundation behavior.
- Milestone 2 is either ready for a newly created bounded plan or remains
  proposed with its missing entry evidence stated explicitly.

## Validation Matrix

| Concern | Required evidence |
| --- | --- |
| Metadata layout | Static and malformed fixtures for offset, alignment, nesting, arrays, optionality, duplicate members, unsupported types, range, access, discard, attachment, and managed result access |
| Parameter lifetime | Construction/destruction counters across builder destruction, compile failure, backing failure, execution failure, success, compiled-graph transfer, culling, and graph destruction |
| Lowering equivalence | Manual-versus-parameterized complete dump/capture comparison for texture, buffer, token, attachment, managed access, final state, RAW/WAR/WAW, discard, culling, and roots |
| Immutability and authority | Post-`AddPass` mutation and mixed manual/parameter declarations are rejected; one pass cannot attach a second parameter object |
| Pass-scoped resolution | Required and optional success plus foreign, wrong-pass, wrong-kind, undeclared, unavailable, and culled access failures |
| Diagnostics | Stable pass and field paths in compile errors, uses, dumps, captures, conflicts, and unavailable-resource reports without addresses |
| GBuffer structure | Disabled/required/isolated-deferred captures preserve names, uses, dependencies, lifetimes, culling, managed color/depth behavior, transitions, and budgets |
| GBuffer behavior | Result status, topology, telemetry, draw identity, attachment layouts, present/offscreen output, resize, target failure, and rendered/readback output match baseline |
| Compatibility | Unmigrated scene and synthetic manual passes compile/execute unchanged; shader parameter metadata and typed binding tests remain exact |
| Performance | Parameter allocation/traversal and full scene graph build/compile/execute remain within existing Debug structural and CPU regression ceilings |
| Platform | Focused and aggregate native tests, required build, inline/threaded recording where applicable, Vulkan validation, representative rendering, and Editor smoke follow repository guidance |
| Documentation | Changed/all documentation plus all-plan and all-roadmap lifecycle validators pass after final edits |

## Definition of Done

- RenderCore owns a documented graph-lifetime typed parameter allocation and
  static metadata contract.
- Parameterized `AddPass` derives every supported graph use through the current
  canonical compiler and freezes its parameter object before compilation.
- Typed pass-scoped resolution grants callbacks exactly the resources declared
  by their submitted parameters.
- Field-aware deterministic errors, dumps, and owning captures cover all
  parameter-derived uses.
- Synthetic equivalence tests prove no change to dependency, culling, lifetime,
  transition, final-state, failure atomicity, or stable scheduling semantics.
- GBuffer is fully parameter-driven, contains no copied persistent-input helper
  or manual use block, and passes structural, behavior, rendering, failure,
  Vulkan, and performance parity gates.
- Legacy APIs remain functional only as the explicit migration surface for
  unmigrated passes; this plan introduces no second compiler or permanent
  facade-only abstraction.
- Lasting Render Graph documentation and the active roadmap reflect the landed
  contract and actual validation evidence.
- Changes are staged and committed with repository-required plan provenance
  after successful validation.

## Deferred Follow-ups

- Builder-owned canonical external texture/buffer registration and conflicting
  initial/final access validation.
- Generic typed graph values with graph-owned storage, single writer, typed
  readers, and contributor-returned value references.
- Pre-compile exact fallback handle resolution and removal of scene-local
  persistent import/channel infrastructure.
- Composition of graph-resource and shader-resource metadata on the same pass
  parameter object, including direct typed graphics/compute binding.
- Contributor-by-contributor scene migration and removal of the broad
  `FSceneFrameGraphContributorContext`, frame-wide Channel aggregate, and all
  repeated persistent-input helpers.
- Parameter-aware scene inspection and enforcement sufficient to retire the
  legacy production authoring surface.
- Physical transient aliasing, queue-aware scheduling, split barriers,
  parallel recording, pass merging, scheduling reordering, and persistent graph
  reuse, each gated by the roadmap's independent evidence requirements.

## Related Documentation

- [Render Graph Parameter-Driven Authoring Roadmap](../Roadmaps/RenderGraphParameterDrivenAuthoring.md)
- [Render Graph](../Runtime/Rendering/RenderGraph.md)
- [Shader Parameters](../Runtime/Rendering/ShaderParameters.md)
- [Renderer Frame Preparation and Render Graph Execution](../Runtime/Rendering/RendererFramePreparation.md)
- [RHI Resource Transitions](../Runtime/Rendering/RHIResourceTransitions.md)
- [RHI Command Execution](../Runtime/Rendering/RHICommandExecution.md)
- [Renderer Resource Recovery](../Runtime/Rendering/RendererResourceRecovery.md)
- [Testing](../Agents/Testing.md)
- [Build and Run](../Agents/BuildAndRun.md)

## Related Code

- `Engine/Source/Runtime/RenderCore/Public/RenderGraph.h`
- `Engine/Source/Runtime/RenderCore/Private/RenderGraph.cpp`
- `Engine/Source/Runtime/RenderCore/Public/Shader/Shader.h`
- `Engine/Source/Runtime/RenderCore/Private/Shader/Shader.cpp`
- `Engine/Source/Runtime/RHI/Public/RHIShaderParameters.h`
- `Engine/Source/Runtime/Renderer/Private/Renderers/SceneFrameGraphContributors.h`
- `Engine/Source/Runtime/Renderer/Private/Renderers/SceneFrameGraphTypes.h`
- `Engine/Source/Runtime/Renderer/Private/Renderers/SceneFrameGBuffer.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/SceneFrameGraphComposer.cpp`
- `Engine/Tests/Native/RenderCoreTests/Private/RenderGraphTests.cpp`
- `Engine/Tests/Native/RenderCoreTests/Private/ShaderFoundationTests.cpp`
