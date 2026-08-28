# Render Graph Pass Parameters Foundation Plan

Summary: Make typed pass parameters the single declaration source for graph dependencies, access, attachments, and pass-scoped resource resolution.

Last reviewed: 2026-08-29

Status: Completed
Completed: 2026-08-29

## Current Status

Stages 0 through 5 are complete. Typed graph-local parameter storage now feeds
an immutable parameterized `AddPass` path that traverses texture, buffer,
token, attachment, managed-resource, optional, fixed-array, and nested fields
into the existing canonical use model. Parameter declaration validation and
legacy manual declarations share one pass-use validation path.

Submission consumes and freezes the mutable parameter reference. Invalid or
foreign allocations, handles, ranges, access/domain combinations, overlapping
fields, resubmission, and mixed manual/parameter authority fail atomically
before a pass callback or canonical use is published. Compiled use captures and
dumps own deterministic field paths while manual dump text remains unchanged.
The complete synthetic parameter graph matches its manual oracle after field
annotations are removed, including dependencies, versions, lifetimes, culling,
and transitions. Parameterized callbacks now receive the immutable submitted
object plus a non-copyable pass-scoped resolver. Resolution accepts only exact
declared wrapper or optional-member addresses, returns typed texture, buffer,
and attachment views, and rejects raw, copied, wrong-kind, foreign-optional,
and cross-pass access before exposing a hidden resource. Culled and incomplete
backing routes invoke no callback.

The GBuffer production pilot now owns `FGBufferPassParameters`: one completion
token write, four optional managed clear/store color attachments, and one
optional managed clear/store depth attachment. Its callback resolves only
those exact parameter members, while the copied persistent-input helper and
manual `UseToken`/managed-attachment block are gone. Unmigrated contributors
continue through the compatibility overload.

Stage 5 hardening has passed the 17-test parameter filter, all 81
`RenderContractTests`, all 38 `RendererSceneContractTests`, all 7
`VolumetricCloudSceneContractTests`, 64 `VulkanRHIIntegrationTests`, the
one-case `VolumetricCloudSceneVulkanTests`, and all 7 `EditorGridVulkanTests`
on the Win64 Debug DurinEditor profile. Six production scene routes retained
the 11-pass, 22--25-dependency, 1-or-17 physical-transition budgets; capture
dumps were 19,599--24,991 bytes and compile/execute observations remained
below 5/250 milliseconds. The lasting Render Graph contract and roadmap now
reflect the implemented foundation.

`GBufferQualificationTests` now uses
the standard DObject, asset-compilation, mount, and catalog environment and
publishes its four material variants through normally compiled `DMaterial`
programs. This restored all four GBuffer draw families, Specular AA, GTAO, and
compute/fragment contact-shadow samples, removed the startup and teardown
failures, and produced consecutive complete passes of every functional, memory,
and synchronized production-route timing gate. Isolated GTAO and contact-route
batches retain absolute measurements as characterization rather than hard
cross-batch comparisons: live sampling showed the adapter switching between
P0/1755 MHz and P3/1335--1530 MHz inside one qualification run despite stable
functional work. Relative half-resolution GTAO benefit and the synchronized
production route remain hard gates; no synchronized GPU timing, memory, graph,
or output budget was raised. `DirectionalShadowBaselineVulkanTests` also passes
all three cases with unchanged frozen image, motion, filtering, graph, and
budget gates. Milestone 1 is complete and the Milestone 2 entry gate is met.

## Stage 0 Frozen Contract

### Declaration vocabulary and lowering

Graph parameter structures use ordinary public wrapper fields plus static
member metadata. The selected wrapper vocabulary is
`FRenderGraphTextureParameter`, `FRenderGraphBufferParameter`,
`FRenderGraphTokenParameter`, `FRenderGraphColorAttachmentParameter`,
`FRenderGraphDepthStencilAttachmentParameter`, and
`FRenderGraphManagedTextureParameter`. Managed color and depth attachments use
their corresponding attachment wrapper with a managed metadata kind. A wrapper
stores only its graph-local handle and exact runtime range; declaration intent
that is invariant for the member is stored in metadata.

| Existing declaration | Parameter member metadata | Canonical lowering |
| --- | --- | --- |
| `UseTexture` | texture, use, access, discard | exact texture `FGraphUse` |
| `UseBuffer` | buffer, use, access, discard | exact buffer `FGraphUse` |
| `UseToken` | token, use | token `FGraphUse`; writes discard the prior value |
| `UseColorAttachment` | color attachment, load, store | read/write color use; non-load discards |
| `UseDepthStencilAttachment` | depth/stencil attachment, load, store | read/write depth/stencil use; non-load discards |
| `UseManagedColorAttachment` | managed color attachment, load, store, result access | color attachment use with pass-managed transition and result access |
| `UseManagedDepthStencilAttachment` | managed depth/stencil attachment, load, store, result access | depth/stencil attachment use with pass-managed transition and result access |
| `UseManagedTexture` | managed texture, use, entry access, result access, discard | texture use with pass-managed transition and result access |

`std::optional<Wrapper>` is the only optional-resource form: disengaged means
no use. `std::array<Wrapper, N>` and `std::array<std::optional<Wrapper>, N>`
are traversed in increasing index order. A nested parameter structure is a
metadata member with its own static layout and is traversed in declaration
order. Raw graph handles, pointers to wrappers, dynamic containers, variants,
unions, and runtime-polymorphic parameter members are unsupported in this
foundation. No currently supported manual declaration is deferred.

Static metadata records the owning structure name and size/alignment, then one
entry per declaration with name, offset, element size/count, optionality,
resource kind, use, access, discard, load/store, pass-managed disposition,
result access, and nested metadata when applicable. Metadata uses the existing
shader-parameter structural pattern but remains an independent RenderCore view;
shader binding ABI and metadata do not change in this plan.

### Ownership, freeze, and execution

`AllocParameters<T>()` returns a move-only mutable graph-parameter reference
backed by builder-owned aligned storage. `T` must be a complete, destructible,
standard-layout type with registered graph metadata. Default construction and
non-trivial destruction are supported; copying, moving, assignment, and byte
relocation of `T` are not required. Each allocation is constructed exactly
once and registered for reverse-order destruction exactly once.

Parameterized `AddPass` consumes that mutable reference. Consumption freezes
the allocation, associates exactly one metadata/object pair with the pass, and
leaves the caller no mutable parameter capability. The callback receives a
const parameter reference and a pass-scoped typed resolver. Builder destruction
owns unsubmitted allocations; successful compile transfers all allocations and
destructor records to the compiled graph. Declaration failure, compile failure,
backing failure, callback failure, normal execution, culling, and destruction
without execution all destroy the object only when the owning builder or
compiled graph dies. Parameter storage is never released merely because a pass
is culled or execution returns early.

Nested structures and fixed arrays obey the outer allocation's lifetime and
freeze. A mutable reference retained outside the consumed allocation is an
invalid API use; the reference generation/frozen state must reject editing,
resubmission, or attaching the same object to another pass before any canonical
use is recorded.

### Compatibility and diagnostics

Legacy `AddPass` plus manual `Use*` remains unchanged for unmigrated callers.
A parameterized pass accepts no manual `Use*` declaration and no second
parameter object. Manual calls targeting it, parameter attachment to a legacy
pass that already has uses, duplicate or overlapping parameter fields, foreign
handles, unsupported layouts, and edits or resubmission after freeze are
declaration errors. Failure is atomic and precedes callbacks and RHI transition
recording.

Canonical uses gain an optional owning parameter field path. Manual uses keep
an empty path and retain byte-for-byte dump/capture output. Parameter paths are
`<Struct>.<Member>`; nesting appends `.Member` and arrays append `[index]`, for
example `FGBufferPassParameters.Attachments.Material[0]`. Errors use the exact
text `pass '<Pass>' parameter '<Path>'` before the existing normalized-use
reason. Parameterized dump lines append `field=<Path>`; capture use records own
the same string. Paths contain no address, builder identity, allocator index,
timestamp, or measured duration.

### GBuffer parity oracle

The pilot identity is `Scene.GBuffer`. Its optional created resources remain
`Scene.GBuffer.Material`, `Scene.GBuffer.Normals`, `Scene.GBuffer.Surface`, and
`Scene.GBuffer.Emissive`, with backing class `renderer.gbuffer` and final
graphics-shader-read access. When enabled, the pass declares those four whole
color subresources as managed clear/store color attachments, `Scene.Depth` as
a managed clear/store depth attachment, and `Scene.GBuffer.Result` as one token
write. All managed attachment result accesses are graphics-shader-read. When
disabled, every optional attachment is absent while the pass and token write
remain.

The focused manual oracle in `RenderGraphTests` freezes the enabled pass's six
uses in declaration order, zero local dependencies, zero emitted graph-owned
transition batches, ten managed entry/exit capture records, six `[0, 0]`
logical lifetimes, rooted culling disposition, one successful callback, and no
callback on incomplete retained backing. Production scene capture parity
additionally remains covered by
`RendererSceneContractTests`, `VolumetricCloudSceneVulkanTests`, and
`DirectionalShadowBaselineVulkanTests`; the last fixture freezes representative
rendered GBuffer values, forward/deferred output parity, telemetry, resize, and
failure behavior used again at Stage 4.

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

- [x] Inventory every current `FRenderGraphBuilder::Use*` declaration form,
  normalized internal use record, compile diagnostic, dump/capture field, and
  `FRenderGraphPassResources` lookup rule that parameter lowering must preserve.
- [x] Freeze the GBuffer pilot's pass/resource/token identities, attachment
  load/store and result access, dependencies, logical lifetimes, transition
  batches, culling disposition, budget statistics, callback result, failure
  behavior, and representative rendered output.
- [x] Select the public wrapper vocabulary and static metadata schema for
  texture, buffer, token, color attachment, depth/stencil attachment, managed
  texture, optional member, and nested-parameter forms.
- [x] Specify builder-owned parameter construction, alignment, destruction,
  freeze, compile-transfer, callback capture, and graph-lifetime rules,
  including the initially supported C++ type traits.
- [x] Specify how parameter field paths enter canonical uses, compile errors,
  dump/capture output, and equality fixtures without destabilizing existing
  pointer-free diagnostics.
- [x] Specify compatibility and rejection rules for legacy `AddPass`, manual
  `Use*`, parameterized `AddPass`, overlapping declarations, post-submission
  mutation, and unsupported parameter layouts.
- [x] Add or update focused contract fixtures that fail before implementation
  when parameter lowering, lifetime, capability, or GBuffer parity is absent.

#### Acceptance Gate

- Every currently supported graph declaration maps to one selected parameter
  member form or is explicitly deferred with no GBuffer dependency.
- Parameter ownership, metadata, immutability, compatibility, diagnostics, and
  pilot parity have one non-conflicting selected contract.
- The baseline capture and test oracle can detect missing/extra uses,
  dependencies, transitions, callbacks, or output changes.

### Stage 1: Implement graph parameter metadata and storage

- [x] Add RenderCore graph parameter member metadata with stable declaration
  order, byte offset, field name, optionality, resource kind, use/access,
  range, discard, attachment load/store, and managed result access.
- [x] Add typed public parameter wrappers and declaration helpers that preserve
  graph-local handle ownership and cannot be confused with resolved RHI
  resources.
- [x] Add compile-time/static validation for supported layouts and runtime
  validation for malformed dynamic conditions that cannot be rejected by the
  type system.
- [x] Add `FRenderGraphBuilder::AllocParameters<T>()` backed by aligned
  graph-owned storage with exactly-once construction and destruction.
- [x] Transfer parameter storage safely into a successful compiled graph and
  release it correctly on builder destruction, compile failure, execution
  failure, and normal completion.
- [x] Cover trivial and non-trivial supported parameter lifetime, alignment,
  nesting, arrays, optionals, graph destruction, and compile-transfer cases.

#### Acceptance Gate

- Supported parameter types have deterministic complete metadata and correctly
  aligned graph-lifetime storage.
- Sanitized/debug native tests observe no leak, double destruction,
  use-after-free, post-graph access, or unstable field ordering across success
  and every early-exit path.
- Shader parameter declaration and binding behavior remains unchanged.

### Stage 2: Lower parameterized passes into canonical graph uses

- [x] Add the immutable parameterized `AddPass` overload and attach one
  parameter metadata/object pair to its pass record.
- [x] Traverse texture, buffer, token, attachment, managed-resource, optional,
  array, and nested members into the existing canonical declaration functions
  without duplicating compiler semantics.
- [x] Reject foreign handles, invalid ranges, illegal use/access/domain pairs,
  conflicting or duplicate fields, unsupported layouts, and mixed manual/
  parameter declaration authority before recording.
- [x] Preserve exact dependency, value-version, hazard-frontier, culling,
  lifetime, final-state, and transition results for equivalent manual and
  parameterized synthetic graphs.
- [x] Extend use provenance, errors, dumps, and owning captures with stable
  parameter field paths while retaining pointer-free deterministic output.
- [x] Measure parameter allocation and traversal against the RenderGraph
  foundation compile budget and add a bounded regression fixture.

#### Acceptance Gate

- Table-driven equivalence fixtures cover every supported member form and
  compare complete compiled graph structure against the manual API oracle.
- Invalid parameterized graphs fail before execution with the pass and exact
  field path in the diagnostic, and no transition or callback records.
- Equal parameter declarations produce byte-for-byte stable dump/capture
  output and remain within the accepted structural and CPU budgets.

### Stage 3: Add typed pass-scoped parameter resolution

- [x] Add a typed resolution view over `FRenderGraphPassResources` that accepts
  only members from the current pass parameter object and produces the
  corresponding RHI texture/buffer or attachment view.
- [x] Preserve null behavior only for declared optional members; unavailable
  required retained backing remains an execution-preparation failure rather
  than callback-visible absence.
- [x] Reject attempts to resolve a raw foreign handle, a parameter member from
  another pass, a wrong resource kind, or a member not declared for execution.
- [x] Define callback signatures and captures so the immutable parameter object
  and typed resolver outlive recording but cannot escape the compiled graph.
- [x] Extend tests for graphics, compute, copy, attachment, optional, culled,
  unavailable, and wrong-pass resolution.

#### Acceptance Gate

- A parameterized callback can obtain every resource it declared and no other
  graph resource.
- Hidden or cross-pass access fails deterministically in focused tests without
  weakening the existing `FRenderGraphPassResources` contract.
- Compile/cull/backing failure invokes no parameterized callback and exposes no
  partially resolved resource table.

### Stage 4: Migrate the GBuffer production pilot

- [x] Define feature-owned GBuffer pass parameters beside the contributor with
  four optional managed color attachments, managed depth, and completion-token
  write capability.
- [x] Convert `FGBufferGraphContributor` to parameterized `AddPass` and typed
  pass-scoped resolution while preserving feature-specific record inputs and
  callback behavior.
- [x] Remove the contributor's unused `DeclarePersistentGraphicsInputs` copy
  and its manual `UseToken`/managed-attachment declaration block.
- [x] Reject any remaining mixed declaration authority for the GBuffer pass and
  verify its callback captures no undeclared graph resource handle.
- [x] Compare production captures for disabled, required, isolated-deferred,
  present, offscreen, resize, and injected target-preparation failure routes.
- [x] Run focused Renderer and Vulkan integration/rendered-output validation
  selected under repository testing guidance.

#### Acceptance Gate

- GBuffer has one parameter-driven declaration authority and no local manual
  graph-use or persistent-input helper code.
- Pass/resource/token names, scheduled topology, dependencies, lifetimes,
  managed attachment semantics, transitions, culling, result status,
  telemetry, failures, and rendered output match the Stage 0 oracle.
- Unmigrated contributors continue to use the compatibility path unchanged.

### Stage 5: Harden, document, and hand off the foundation

- [x] Run the focused RenderCore parameter/lowering/lifetime suite, complete
  registered `RenderContractTests` target, selected Renderer scene contracts, representative
  Vulkan validation and rendering fixtures, and the required build tier from
  repository guidance.
- [x] Record compile/execute CPU, capture size, graph statistics, transition,
  and output evidence against frozen budgets; resolve any regression rather
  than silently raising the scene budget.
- [x] Update the lasting Render Graph contract with parameter ownership,
  metadata, lowering, immutability, execution capability, compatibility, and
  diagnostic rules.
- [x] Update the roadmap current status, Milestone 1 state, and next child-plan
  entry-gate disposition from actual validation evidence.
- [x] Record exact validation and any deliberately non-standard coverage in
  this plan, complete all passed checklists, and prepare the required plan/stage
  commit provenance.

#### Acceptance Gate

- All focused, aggregate, build, Vulkan, renderer parity, documentation, and
  budget gates selected by this plan pass with recorded evidence.
- Lasting contracts, code, tests, captures, and the active roadmap agree on one
  parameter foundation behavior.
- Milestone 2 is either ready for a newly created bounded plan or remains
  proposed with its missing entry evidence stated explicitly.

## Stage 5 Validation Evidence

Validation ran on 2026-08-29 with the Win64 Debug DurinEditor profile. Each
native-test command built its selected target first, satisfying the repository's
smallest-sufficient build tier for this non-user-visible hardening/documentation
stage; no full `all` editor build was required by the build guidance.

- `test RenderContractTests 'FRenderGraphTests.*Parameter*'`: 17/17 passed,
  covering metadata, storage, lifetime, lowering, immutability, diagnostics,
  resolver capability, culling/backing atomicity, and the traversal budget.
- `test RenderContractTests`: 81/81 passed across all RenderCore/RHI contract
  suites, including manual compatibility and shader-adjacent foundations.
- `test RendererSceneContractTests`: 38/38 passed, including the exact
  `FGBufferPassParameters` metadata contract.
- `test VolumetricCloudSceneContractTests`: 7/7 passed.
- `test VulkanRHIIntegrationTests`: 64/64 passed with Vulkan validation.
- `test VolumetricCloudSceneVulkanTests`: 1/1 passed. Disabled, invalid-input,
  compute, fragment, offscreen/present, and resize routes all succeeded.
- `test EditorGridVulkanTests`: 7/7 passed, covering rendered output, required
  unavailable behavior, and injected GBuffer target recovery.
- `test DirectionalShadowBaselineVulkanTests --mode qualification`: 3/3 passed
  after the fixture began publishing material compiler output through the
  standard `DMaterial` path. The frozen image hashes, motion pixels, filter
  quality, graph statistics, transition budget, and preparation qualification
  remained unchanged; the contact-shadow case again recorded real GBuffer
  draws and nonzero changed output pixels. The complete target ran in 39.45
  seconds.
- `test GBufferQualificationTests --mode qualification`: the repaired fixture
  completed all routes and passed twice consecutively in 8.43 and 7.81 seconds.
  Its standard studio IBL final-output matrix requires Specular AA to reduce
  peak motion range by at least 70%; all six scale/FXAA combinations passed.
  Earlier diagnostic runs retained every functional, telemetry, query-count,
  and memory assertion but exposed cross-batch GPU frequency changes: live
  sampling recorded P0/1755 MHz and P3/1335--1530 MHz states during one run.
  Isolated GTAO absolute timings and the tight contact compute/fragment ratio
  are therefore characterization output, while relative half-resolution GTAO
  benefit and all synchronized production-route absolute and stability gates
  remain enforced. No synchronized timing or memory budget was raised.

The six owning scene captures retained 11 declared and scheduled passes,
22--25 dependencies, zero buffer transitions, and 1 or 17 physical texture
transitions. Their dumps measured 19,599--24,991 bytes with 26--31 resources,
71--91 uses, and 30--48 owning transition records. Observed compile time was
592--912 microseconds and execute time was 566--62,352 microseconds, below the
existing 5,000/250,000-microsecond observational ceilings with no budget flag.
No pass, dependency, transition, capture, or CPU budget was raised. Output and
failure parity are additionally covered by the passing cloud and Editor grid
fixtures above. All selected Stage 5 acceptance gates are closed.

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
