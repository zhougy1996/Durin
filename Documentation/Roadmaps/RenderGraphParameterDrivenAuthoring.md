# Render Graph Parameter-Driven Authoring Roadmap

Summary: Evolve Durin RDG toward parameter-driven pass declaration, binding, contributor composition, and evidence-gated execution optimizations.

Last reviewed: 2026-08-29

Status: Active
Completed:

## Current Status

The completed Render Graph architecture provides one parent scene graph,
deterministic dependency compilation, exact range and subresource tracking,
logical transient lifetimes, culling, retained-backing publication, automatic
transition batches, pass-scoped physical resource lookup, captures, and
production scene contributors. Milestone 1 has now implemented graph-owned
typed parameter storage, metadata lowering into that compiler, immutable
parameterized `AddPass`, field-path diagnostics, and typed pass-scoped
resolution. `Scene.GBuffer` is the production pilot and has no separate manual
use block.

Unmigrated scene contributors still expose the remaining split. They receive a broad mutable
`FSceneFrameGraphContributorContext`, unpack resources and execution channels
they do not own, and repeat persistent-input declarations. Thirteen local
`DeclarePersistentGraphicsInputs` definitions currently exist while only the
Base Scene and Deferred Directional Lighting copies are invoked. This is both
mechanical duplication and evidence that the declaration model does not expose
the actual inputs of each pass.

The active [Render Graph Pass Parameters Foundation](../Plans/RenderGraphPassParametersFoundation.md)
is in its final hardening stage. Focused and aggregate RenderCore tests, scene
contracts, Vulkan RHI validation, cloud rendering, Editor grid rendering,
recovery, resize, and parameter budget evidence pass. Its exit gate remains
open because the retained monolithic GBuffer qualification exposes stale
asset/reflection setup and downstream zero-draw/query failures, while two
pre-existing directional-shadow rendered baselines still fail outside the
parameter foundation. Milestone 2 is therefore not ready for plan creation;
the next child plan remains proposed until Milestone 1 records a complete
qualification disposition.

## Outcome

Provide a Render Dependency Graph authoring and execution model in which:

1. One immutable pass parameter structure is the authoritative declaration of
   graph resources, access intent, attachments, logical values, and the
   execution callback's resource capability.
2. Graph and shader parameter metadata compose around the same typed parameter
   object, so dependency declaration and shader binding cannot silently drift.
3. External and persistent resources are registered once per graph, deduplicated
   by physical identity, and resolved to the exact selected fallback before
   compilation.
4. Feature contribution functions receive narrow typed inputs and return typed
   graph outputs rather than discovering state through a frame-wide mutable
   context or channel bag.
5. The existing deterministic compiler, RHI access authority, Vulkan state
   validation, recovery, and frame commit or abort contracts remain intact
   while authoring migrates.
6. Physical aliasing, split barriers, queue-aware scheduling, parallel command
   recording, pass merging, and persistent graph reuse are implemented only by
   separate evidence-backed plans after the declarative model is complete.

The target follows the architectural lessons of Unreal Engine RDG without
copying Unreal APIs, macros, allocator assumptions, or scheduling policy where
Durin's RHI and workloads require a different contract.

## Scope

- RenderCore pass parameter types, metadata, graph-owned allocation, immutable
  `AddPass` integration, validation, lowering, capture, and pass-scoped
  resolution.
- Composition of graph-resource and shader-resource metadata on one C++ pass
  parameter structure, including graphics, compute, copy, SRV/UAV, attachment,
  and optional-resource forms.
- Builder-owned registration of imported textures and buffers with consistent
  initial/final access and duplicate physical identity handling.
- Typed graph values for non-RHI outcomes with single-writer and declared-reader
  enforcement.
- Renderer fallback resolution that publishes the exact graph handle actually
  bound by a pass.
- Incremental migration of all production scene contributors to narrow typed
  inputs and returned outputs while retaining one explicit composer.
- Deterministic diagnostics that name parameter fields and explain the resource
  uses derived from them.
- Lasting Runtime Rendering contracts and focused RenderCore, Renderer, RHI,
  Vulkan, capture, recovery, and rendering validation.
- Conditional advanced compiler, allocator, queue, and recording work selected
  only from measured entry evidence.

## Non-Goals

- Importing Unreal Engine source, matching its public API spelling, reproducing
  its macro implementation, or making binary/source compatibility a goal.
- Replacing Durin's exact range/subresource compiler, access vocabulary,
  deterministic stable scheduling, complete-or-null backing publication, or
  RHI/Vulkan execution-state authority merely for structural similarity.
- Building a public plugin pass registry, nested feature graphs, runtime
  polymorphic contributor registry, mutable string blackboard, or generic
  service locator.
- Moving feature algorithms, topology choice, fallback policy, shader
  compilation, PSO ownership, temporal commit, or renderer recovery into
  RenderCore.
- Migrating all scene contributors in one change or keeping two permanent
  authoring authorities for a migrated pass.
- Treating async compute, transient aliasing, pass merging, scheduling
  reordering, parallel recording, or persistent graph reuse as required for
  completion of the parameter-driven authoring milestones.
- Changing rendered output, draw/dispatch algorithms, telemetry meaning, or
  qualification policy as incidental refactor work.

## Program Decisions and Invariants

- **Pass parameters are the use authority.** A parameterized pass does not
  append independent manual uses after `AddPass`. Its graph uses are derived
  from immutable typed fields and lowered to the existing compiler model.
- **One pass may have one parameter object.** The object is graph-owned,
  allocated with builder lifetime, frozen at `AddPass`, retained by the
  compiled graph, and inaccessible after graph destruction.
- **Metadata is structural, not policy.** It describes field offset, graph
  resource kind, access, range, attachment load/store, optionality, and logical
  value direction. It does not choose a renderer route or fallback.
- **Graph and shader reflection compose.** The implementation may expose
  separate metadata views for graph compilation and shader binding, but both
  views describe fields of the same parameter structure. Call sites do not
  construct a second RHI-only texture table for the same declared inputs.
- **The existing compiler remains canonical.** Parameter traversal produces
  the same normalized internal uses accepted by `UseTexture`, `UseBuffer`,
  `UseToken`, and attachment declarations. Dependency, culling, lifetime, and
  transition semantics are not reimplemented in the metadata layer.
- **Manual APIs are compatibility-only during migration.** Existing
  non-parameterized `AddPass` and `Use*` calls remain available until all
  production callers migrate. A pass cannot mix parameter-derived and manual
  declarations for the same resource range, and new production passes use the
  parameter path.
- **Execution capability is pass-scoped.** A callback can resolve only handles
  declared by its parameters. Foreign, undeclared, incorrectly typed, or
  unavailable access is an authoring failure, not a null fallback.
- **External registration is ownership-neutral.** The graph retains what
  execution needs but never assumes lifetime, eviction, recovery, history, or
  publication ownership from the external producer.
- **Fallback is selected before compile.** Parameter fields contain the final
  chosen graph handle. Alternative and fallback resources are not both
  declared unless the pass actually accesses both.
- **Typed values are not a blackboard.** Every non-RHI value has an explicit
  type, graph lifetime, one writer, and declared readers. Contributor outputs
  carry these references directly to downstream inputs.
- **Contributors stay independent.** The scene composer remains explicit and
  stable-order. Contributors own coherent pass chains and return typed outputs;
  they do not compile, execute, create child graphs, or receive the entire
  scene plan.
- **Failure remains pre-recording where possible.** Invalid parameter metadata,
  illegal uses, duplicate/conflicting imports, missing producers, and backing
  publication failures reject the graph before the first pass records.
- **RHI remains authoritative.** RDG derives logical access and transitions;
  RHI validates expected state and Vulkan owns native stages, access masks,
  layouts, descriptors, submission, and completion.
- **Determinism and observability are acceptance gates.** Equal declarations
  produce equal parameter paths, uses, dependencies, transitions, captures,
  and errors without addresses or allocation-order noise.
- **Advanced optimization remains evidence-gated.** Authoring completeness
  does not manufacture a justification for aliasing, async compute, parallel
  recording, or compiler reordering.

## Current Foundations and Gaps

| Area | Reusable foundation | Gap owned by this roadmap |
| --- | --- | --- |
| Graph compiler | Exact texture subresources and buffer ranges, value versions, hazard frontiers, culling, lifetimes, transitions, budgets, and deterministic capture | Pass declarations are assembled by independent imperative `Use*` calls |
| Pass execution | Compiled callbacks receive `FRenderGraphPassResources`; undeclared lookup is rejected | The callback captures frame-wide handles and manually resolves each resource instead of receiving one typed capability object |
| Shader parameters | Compile-time `FShaderParametersMetadata`, reflected binding validation, typed `SetShaderParameters`, counted views, graphics/compute support | Shader resource fields and graph resource declarations are separate structures and call-site operations |
| Resource creation | Graph-local handles, logical descriptions, retained-backing resolver, complete atomic publication | External registration and physical-identity deduplication are implemented in the scene composer rather than the builder |
| Logical results | Graph tokens share value-version dependency semantics; Renderer owns typed result storage | All contributors can reach the frame-wide channel aggregate; type and writer ownership are not carried in contributor signatures |
| Scene authoring | One graph, one composer, twelve named contributors, topology fixed before compile, feature recorders isolated from graph lifecycle | One broad contributor context exposes all services, topology flags, resources, and channels; declarations repeat and actual inputs are hard to audit |
| Fallback | Renderer prepares routes and resources before compile; complete failure policies already exist | Some callbacks still choose physical alternatives after declarations, allowing declared and bound resources to diverge |
| Diagnostics | Pointer-free graph capture, dependency causes, transitions, lifetimes, culling, budgets, scene inspection | Captures cannot name the pass parameter field that produced a use or compare graph and shader declarations |
| Optimization | Logical lifetimes and timings exist; RHI/Vulkan state and completion contracts are established | Current evidence does not independently justify aliasing, queue expansion, split barriers, parallel recording, or graph reuse |

## Milestone Map

| Milestone | Dependencies | Deliverable | Entry gate | Exit gate | State |
| --- | --- | --- | --- | --- | --- |
| 1. Pass parameters foundation | Completed Render Graph consolidation and shader parameter metadata | Graph-owned typed parameter storage, metadata traversal, automatic lowering to current uses, immutable parameterized `AddPass`, pass-scoped resolution, capture field paths, and one production pilot | Current RenderCore graph tests and representative scene capture establish exact manual declarations and behavior | Synthetic coverage proves all supported resource/use forms and invalid metadata; the pilot has no manual `Use*` calls and preserves capture, transition, failure, and output behavior | Active; implementation landed, qualification disposition open |
| 2. External registration and typed graph values | Milestone 1 | Builder-owned deduplicated external texture/buffer registration, typed logical value references, exact fallback selection, and removal of scene-local import/channel infrastructure | Parameterized passes can express imported resources and tokens without semantic loss | Duplicate/conflicting imports fail deterministically; fallback declares only the bound handle; every migrated logical value has one writer and explicit readers | Proposed |
| 3. Graph and shader parameter composition | Milestones 1 and 2; stable shader parameter metadata | One pass parameter object supplies graph dependency metadata and shader binding fields for graphics and compute, including optional SRV/UAV and arrays | A migrated production pass demonstrates the current declaration/binding duplication with stable reflection fixtures | Graph declarations and shader binding share fields; validation rejects missing or inconsistent graph/shader capabilities; no duplicate RHI texture table remains in migrated passes | Proposed |
| 4. Scene contributor migration | Milestones 2 and 3 | All scene contributors accept narrow typed inputs, return typed outputs, use parameterized passes, and remove the broad contributor context, channel bag, and persistent-input helper copies | Foundation APIs cover GBuffer, attachments, compute/graphics routes, clouds, post process, output roots, and failure results | Directional shadow through output uses one parameter-driven graph; captures, images/readbacks, telemetry, fallback, recovery, resize, multi-view, and CPU/GPU budgets pass; legacy scene declarations are removed | Proposed |
| 5. Authoring contract and diagnostics completion | Milestone 4 plus at least one new or materially changed rendering feature | Lasting public guidance, parameter-aware capture/inspection, migration enforcement, compatibility API disposition, and qualified regression budgets | Full scene migration exposes real parameter layouts and diagnostic needs | New inter-pass work uses the parameter path; actionable errors name pass/field/resource; obsolete compatibility paths are removed or explicitly bounded; lasting contracts are authoritative | Proposed |
| 6. Physical transient aliasing | Milestone 5 plus measured peak-memory pressure and Vulkan placement/completion prerequisites | Compatible placement classes, alias barriers, completion-safe reuse, capture identities, budgets, and non-alias fallback | Target workloads demonstrate material memory benefit beyond complexity and validation cost | Vulkan validation, stress, recovery, and target hardware measurements prove safe material memory savings | Evidence-gated |
| 7. Queue-aware scheduling and split barriers | Milestone 5 plus independent queue/timeline RHI contracts and measured overlap opportunity | Queue-qualified passes, cross-queue ownership/synchronization, split barriers, deterministic scheduling, and synchronous fallback | Target hardware and workloads show independent compute/graphics work with expected net benefit | Supported and fallback devices prove correctness, no starvation/deadlock, bounded CPU/submission cost, and material frame-time improvement | Evidence-gated |
| 8. Parallel recording and advanced compilation | Milestone 5 plus one measured CPU or render-pass bottleneck | One selected parallel recording, pass merge, scheduling reorder, persistent graph reuse, or related optimization per child plan | Profiling identifies one dominant bounded cost and a stable semantic oracle | The selected technique preserves diagnostics and graph semantics while meeting a frozen target improvement | Optional and evidence-gated |

## Child Plan Boundaries

| Child plan | State | Owns | Must not own |
| --- | --- | --- | --- |
| [Render Graph Pass Parameters Foundation](../Plans/RenderGraphPassParametersFoundation.md) | Active; Stage 5 qualification disposition open | Milestone 1 metadata, allocation, lowering, immutable parameterized pass API, pass-scoped resolution, capture paths, and one bounded production pilot | External registry redesign, shader binding unification, all-scene migration, allocator aliasing, queue scheduling, or feature algorithm changes |
| Render Graph External Registration and Typed Values | Proposed; entry gate not yet satisfied | Milestone 2 import identity/access contract, typed value storage/references, fallback resolution boundary, and scene infrastructure replacement | Shader descriptor architecture or full contributor migration |
| Render Graph and Shader Parameter Composition | Proposed after Milestone 2 | Milestone 3 shared parameter-object declaration/binding, reflection validation, SRV/UAV/array coverage, and graphics/compute pilots | Whole-scene migration or backend descriptor redesign unrelated to the shared object |
| Render Graph Scene Parameter Migration | Proposed after Milestone 3 | Milestone 4 contributor-by-contributor migration, typed signatures/outputs, broad-context removal, parity and qualification | Compiler optimization, public plugin injection, or feature redesign |
| Render Graph Authoring Contract and Diagnostics | Proposed after Milestone 4 | Milestone 5 lasting docs, enforcement, inspection, compatibility retirement, and new-feature proof | Speculative execution optimization |
| Render Graph Transient Aliasing | Evidence-gated | Milestone 6 placement compatibility, alias transitions, completion, fallback, and memory evidence | Logical lifetime correctness or ordinary pool recovery |
| Render Graph Queue Scheduling | Evidence-gated | Milestone 7 queue capabilities, timeline/ownership contracts, split barriers, overlap policy, and fallback | Treating every compute pass as asynchronously profitable |
| Render Graph Compiler Optimization: `<Technique>` | Optional per measured bottleneck | One Milestone 8 technique with its own semantic oracle and performance gate | A bundle of unrelated optimizations |

Only one required migration plan is active at a time. Completing a plan updates
this roadmap before the next proposed plan is created.

## Program Validation Matrix

| Concern | Required program evidence |
| --- | --- |
| Parameter metadata | Field offsets, nesting, arrays, optional members, alignment, duplicate fields, unsupported types, invalid ranges, illegal access/domain combinations, and post-`AddPass` mutation are validated deterministically |
| Dependency equivalence | Parameter-derived RAW, WAR, WAW, discard, token, attachment, managed-resource, final-state, culling, and root behavior matches the existing compiler oracle |
| Execution capability | Declared resources resolve with the expected type/range; undeclared, foreign, unavailable, or wrong-kind access fails; graph-owned parameter lifetime covers compile and execute only |
| Shader composition | Reflected names, types, arrays, optionality, SRV/UAV intent, and graph access agree; graphics and compute bind exactly the resources declared to RDG |
| External resources | Duplicate physical imports canonicalize only when access contracts agree; ownership, counted lifetime, final state, invalidation, and recovery remain external |
| Fallback and topology | Route and fallback selection finish before compile; captures show only resources actually accessible to the pass; allocation failure cannot silently change renderer policy |
| Typed values | Each value has one writer, declared typed readers, graph lifetime, deterministic dependency/culling behavior, and no mutable string lookup |
| Scene parity | Pass order, dependencies, transitions, lifetimes, culling, captures, images/readbacks, draw/dispatch identity, telemetry, fallback, failure, temporal commit, resize, and multi-view isolation remain equivalent |
| Diagnostics | Errors and captures name pass, parameter field path, resource identity, declared capability, normalized use, and conflicting declaration without addresses or unstable allocation IDs |
| Performance | Parameter allocation/traversal, graph build/compile/execute median and p95, capture cost, command counts, barriers, retained/peak bytes, and full-frame CPU/GPU timings remain within frozen gates |
| Platform/runtime | Focused and aggregate native tests, required builds, inline/threaded command recording, Vulkan validation, offscreen/window output, recovery, rendered fixtures, and Editor smoke follow repository guidance |

## Risks and Control Gates

- **A facade hides rather than removes duplication.** Milestone 1 requires the
  compiler to consume parameter metadata; a helper that merely calls several
  `Use*` functions does not satisfy the exit gate.
- **Graph metadata becomes a second compiler.** Parameter traversal lowers to
  the existing canonical use structures and is tested for exact equivalence.
  It does not implement dependency, lifetime, culling, or barrier logic.
- **Graph and shader metadata diverge structurally.** Both views must describe
  one parameter object. Milestone 3 rejects a second call-site-owned resource
  table and adds consistency validation.
- **The parameter object becomes a new service locator.** Only resource/value
  capabilities and small immutable pass constants belong in it. Renderer
  services, complete topology, mutable composition state, and the full scene
  plan remain outside.
- **Macro machinery obscures errors.** Public wrappers remain ordinary typed
  C++ fields with inspectable metadata; compiler diagnostics name the exact
  field path and tests cover malformed declarations.
- **Optional fields hide fallback policy.** Null optional fields mean no use.
  Renderer fallback selection is explicit before assignment and is observable
  independently from metadata traversal.
- **Incremental migration creates two authorities.** A parameterized pass may
  not declare the same range manually. Each pilot removes its old `Use*` block
  in the same change after structural parity is proven.
- **Graph-owned storage retains non-trivial objects unsafely.** The first plan
  selects and tests construction, destruction, alignment, move/copy, capture,
  and compiled-graph ownership rules before accepting arbitrary types.
- **External canonicalization masks conflicting states.** Duplicate identity
  succeeds only for compatible initial/final access and description; conflicts
  fail before recording with both registration sites named.
- **Contributor migration recreates a giant composer.** Feature-owned input,
  output, and pass parameter types remain beside each contributor; the composer
  only connects returned values in stable order.
- **UE parity becomes speculative optimization scope.** Advanced milestones
  retain independent entry gates. Failure to justify them does not block the
  parameter-driven architecture from completing.
- **Hot-path reflection regresses frame cost.** Structural metadata is static,
  traversal uses graph-local allocation without per-frame name lookup, and CPU
  budgets are frozen before production expansion.

## Completion Criteria

- Every production scene pass declares graph resources, exact access,
  attachments, and logical values through an immutable graph-owned typed
  parameter object.
- Graph dependency compilation and shader binding consume compatible metadata
  views over the same object; migrated call sites maintain no duplicate
  resource declaration or RHI texture table.
- External resources are registered through one builder-owned canonical path,
  preserve external ownership and final-state contracts, and reject conflicting
  duplicate registration before recording.
- Fallback and topology are resolved before parameter assignment, and graph
  captures expose only the capabilities actually granted to each pass.
- Scene contributors receive narrow typed inputs, return typed outputs, retain
  independent feature ownership, and no longer use the broad contributor
  context, frame-wide channel bag, or repeated persistent-input helpers.
- Parameter-aware diagnostics, structural tests, renderer integration,
  Vulkan validation, rendered references, recovery, resize, multi-view, and
  performance gates pass under repository build and testing guidance.
- The legacy manual declaration surface is removed from production authoring or
  retained only behind a documented low-level compatibility boundary with no
  migrated scene callers.
- Lasting Render Graph and shader parameter contracts describe the implemented
  model, and all required child plans are completed and archived as historical
  provenance.
- Aliasing, queue scheduling, split barriers, parallel recording, and advanced
  compilation are completed only where their entry evidence is met; otherwise
  they are explicitly dispositioned without leaving two authoring models.

## Related Documentation

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
- `Engine/Source/Runtime/Renderer/Private/Renderers/SceneFrameGraphComposer.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/SceneFrameGraphContributors.h`
- `Engine/Source/Runtime/Renderer/Private/Renderers/SceneFrameGraphTypes.h`
- `Engine/Tests/Native/RenderCoreTests/Private/RenderGraphTests.cpp`
- `Engine/Tests/Native/RenderCoreTests/Private/ShaderFoundationTests.cpp`
