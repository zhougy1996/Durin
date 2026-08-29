# Render Graph External Registration and Typed Values Plan

Summary: Canonicalize external resource imports and replace untyped scene completion channels with graph-owned typed values and pre-compile fallback selection.

Last reviewed: 2026-08-29

Status: Archived
Completed: 2026-08-29

## Current Status

Milestone 2 is complete. RenderCore canonicalizes equivalent texture and buffer
imports by physical identity, preserves the first name and declaration order,
and records deterministic contract conflicts before compilation. The scene
composer delegates persistent texture identity directly to the builder; its
local `PersistentTextureImports` table has been removed.

Graph-owned typed values now construct aligned payloads in builder storage,
transfer them only on successful compile, destroy them exactly once, enforce
one writer and declared readers, and reuse token value versions, culling, and
lifetime compilation. Manual and parameterized declarations expose exact
pass-scoped const-read or mutable-write capabilities. Stable value type names,
directions, and parameter field paths appear in pointer-free captures.

All eleven scene result channels now contain typed handles only. Callbacks read
and write graph payloads, while only final Scene Color and post-process
transactional publications leave the graph. Environment candidate/fallback
selection completes before registration and registers only the selected
irradiance, prefiltered, and BRDF physical set. The newly explicit post-process
read of the isolated-deferred result adds one correct value dependency that the
former side payload could not represent.

Validation passed `RenderContractTests` (88 tests),
`RendererSceneContractTests` (38 tests), `VolumetricCloudSceneContractTests`
(7 tests), `RendererResourceReloadVulkanTests` (1 test),
`VolumetricCloudSceneVulkanTests` (1 test), GBuffer qualification after one
documented unstable-sample retry, and Directional Shadow Vulkan qualification.
The standalone `VolumetricCloudQualificationTests` target remains excluded
because its existing direct renderer calls do not compile against the current
four-argument API; the production cloud scene Vulkan route passes.

## Goal

Make one graph builder the canonical authority for external physical-resource
identity and typed non-RHI values, then prove in the production scene graph
that fallback selection is complete before compilation and the declared handle
is exactly the one consumed.

## Scope

- Builder-owned texture and buffer external registration keyed by physical RHI
  identity, with deterministic canonical handles and access-contract checks.
- Graph-owned typed value storage and typed handles/references for non-RHI pass
  results, with exactly one declared writer and explicit declared readers.
- Typed-value parameter metadata, lowering, pass-scoped read/write resolution,
  capture provenance, lifetime, culling, and failure behavior.
- A bounded manual compatibility surface for unmigrated passes while typed
  parameter migration remains owned by later roadmap milestones.
- Removal of the scene composer's `PersistentTextureImports` table and
  `FSceneFrameGraphExecutionChannels` payload storage after production wiring
  moves to canonical imports and graph-owned values.
- Pre-compile selection of the exact external texture used for representative
  nullable/fallback scene inputs, with capture and rendered-output parity.
- Lasting Render Graph and renderer-preparation contracts plus focused
  RenderCore, Renderer, RHI/Vulkan, recovery, capture, and qualification
  evidence.

## Non-Goals

- Composing graph-resource metadata with shader reflection or changing shader
  descriptor/binding architecture; that is Milestone 3.
- Converting every scene contributor to narrow signatures or parameterized
  passes, or removing the broad contributor context; that is Milestone 4.
- Moving feature route choice, resource lifetime, cache invalidation,
  recovery, temporal publication, or fallback policy into RenderCore.
- Treating a graph registration as ownership transfer of an RHI resource or as
  permission for the graph to replace, release, or recover it.
- A mutable string blackboard, runtime-polymorphic value registry, multi-writer
  merge semantics, cross-graph values, or callback-time resource discovery.
- Changing pass topology, draw/dispatch algorithms, telemetry meaning,
  rendered output, or synchronized qualification budgets.
- Physical aliasing, queue-aware scheduling, split barriers, parallel command
  recording, pass merging, compiler reordering, or persistent graph reuse.

## Design Decisions and Invariants

- **Physical identity canonicalizes external registration.** Re-registering the
  same non-null `FRHITexture*` or `FRHIBuffer*` with the same resource kind,
  description, initial access, and final access returns the first graph handle.
  Registration order and the first name own the stable capture identity.
- **Conflicting registration is an atomic declaration error.** A repeated
  physical identity with a different resource kind or initial/final access is
  diagnosed using the canonical and conflicting names/contracts. Compilation
  publishes no graph and records no callback or transition. Null imports remain
  invalid according to the existing resource contract rather than becoming an
  identity key.
- **Registration is ownership-neutral.** The compiled graph retains only the
  references already required for execution; the external producer remains
  responsible for lifetime, replacement, invalidation, recovery, and final
  state beyond the declared graph boundary.
- **Typed values reuse compiler value versions.** A
  `TRenderGraphValueHandle<T>` remains graph-local and lowers to the existing
  dependency, hazard-frontier, culling, and lifetime machinery. Its C++ type
  grants payload access but does not create a second scheduler.
- **The graph owns value payload storage.** `CreateValue<T>` constructs one
  supported payload in graph-lifetime storage, transfers it on successful
  compile, and destroys it exactly once on every builder/compile/execute exit
  path. A value cannot escape or outlive its graph.
- **One writer, explicit readers.** Every retained typed value has exactly one
  declared write and every read is declared. Duplicate writers, missing
  producers, foreign handles, type mismatches, and undeclared access fail
  deterministically before the affected callback can observe a payload.
- **Resolution is capability-scoped.** A writer receives mutable access only
  through its exact declared write member; readers receive const access only
  through exact declared read members. Copied, raw, wrong-pass, wrong-type, and
  undeclared handles are rejected. A default-constructed payload is storage,
  not evidence that its producer executed.
- **Legacy tokens remain compatibility-only.** Existing tokens and manual
  `UseToken` continue to work for unmigrated/tests-only ordering edges. New
  production result payloads use typed values; this plan does not leave a
  permanent token-plus-side-payload pair for migrated results.
- **Fallback is renderer policy completed before registration.** The renderer
  resolves candidate versus fallback before it fills a pass parameter or
  declares a use. Only the selected physical resource is registered and only
  its canonical handle appears in uses and captures. RenderCore never chooses
  a fallback.
- **Failure stays transactional.** Invalid registration/value metadata,
  unavailable required selection, incomplete backing, compile failure, or
  execution preparation failure invokes no dependent result consumer and
  publishes no partial frame/temporal result.
- **Diagnostics remain deterministic.** Errors and captures use stable graph
  resource/value names, pass names, parameter field paths, and declared
  contracts; they contain no physical addresses, allocation-order artifacts,
  or implementation-specific C++ type spellings.

## Current Foundations and Gaps

| Area | Reusable foundation | Gap owned by this plan |
| --- | --- | --- |
| External resources | `ImportTexture`/`ImportBuffer` preserve exact initial/final access and compilation rejects duplicate physical imports | Imports always allocate a new handle; the scene composer owns a separate texture deduplication table that does not compare access contracts |
| Logical dependencies | Tokens already use resource versions, hazard frontiers, culling, lifetimes, field-path capture, and typed parameter lowering | Token payloads live in mutable Renderer channels with no graph-owned type, writer, reader, or lifetime authority |
| Parameter storage | Graph-owned aligned parameter allocations transfer atomically into compiled graphs and have exact resolver capability | No typed-value member kind or payload resolver exists |
| Scene composition | One explicit composer creates stable resources/tokens and contributors declare deterministic pass order | `FSceneFrameGraphExecutionChannels` combines graph tokens with callback-mutated side payloads, and persistent import deduplication is scene-local |
| Fallbacks | Renderer preparation owns route and default-resource policy; pass-scoped graph lookup prevents undeclared physical access | Some call sites carry candidate and fallback alternatives beyond graph declaration, so capture does not always prove the exact bound choice |
| Failure and observation | Compile/execute are atomic, capture is pointer-free, RHI/Vulkan validate physical state, and production baselines cover output/recovery | No oracle covers equivalent duplicate registration, conflicting contracts, typed payload lifetime/access, or selected-only fallback capture |

## Implementation Stages

### Stage 0: Freeze registration, value, and fallback contracts

- [x] Inventory every production `ImportTexture`/`ImportBuffer` call, physical
  duplicate, initial/final access pair, name, owner, and retained lifetime; freeze
  the current scene resource/use/transition captures as the parity oracle.
- [x] Inventory every `TSceneFrameGraphValue<TResult>`, its token writer/readers,
  callback mutation/read sites, default/failure state, culling behavior, and
  final publication or telemetry consumer.
- [x] Select the public typed-value vocabulary, supported C++ type traits,
  storage/alignment/destruction rules, read/write parameter wrappers, manual
  compatibility declarations, and pass-scoped resolver signatures.
- [x] Specify canonical import naming, descriptor comparison, repeated
  same-contract behavior, conflicting-contract errors, null handling, compiled
  resource retention, and final-state responsibility for textures and buffers.
- [x] Identify representative candidate/fallback edges and freeze which route
  selects each physical texture before compile, including unavailable required
  input and recovery behavior.
- [x] Add focused failing fixtures for canonical import reuse, conflicting
  imports, typed-value storage/access, one-writer enforcement, parameter
  lowering, selected-only fallback capture, and scene parity.

#### Acceptance Gate

- Every existing scene import and execution channel has a named disposition,
  and every migrated fallback edge has one pre-compile selection owner.
- The selected API has unambiguous ownership, type, writer/reader, lifetime,
  compatibility, capture, failure, and destruction semantics.
- Frozen structural and rendered-output oracles detect duplicated imports,
  undeclared alternatives, missing result edges, transition drift, and partial
  publication.

### Stage 1: Canonicalize external registration in RenderCore

- [x] Add builder-owned texture and buffer identity maps so repeated equivalent
  external registrations return one canonical graph-local handle.
- [x] Compare kind, physical description, initial access, and final access on
  every repeat and record one deterministic conflict diagnostic containing both
  stable names and contracts.
- [x] Preserve first-registration ordering/name in dumps, captures, lifetimes,
  final transitions, budgets, and pass-scoped resolution without exposing the
  physical pointer.
- [x] Preserve external ownership and the current complete-or-null compile and
  execution-preparation behavior across builder destruction, compile failure,
  backing failure, execution failure, and success.
- [x] Cover texture/buffer equivalence, conflicts, nulls, cross-builder identity,
  disjoint use ranges, culled uses, final states, and deterministic output.

#### Acceptance Gate

- Equivalent repeated registrations produce exactly one graph resource and one
  handle, while conflicting registrations fail before recording.
- Canonicalization changes no dependency, culling, lifetime, transition, or RHI
  state result relative to a single-import manual oracle.
- No Renderer-owned identity table is needed to obtain correct behavior.

### Stage 2: Implement graph-owned typed values

- [x] Add `TRenderGraphValueHandle<T>` and graph-owned `CreateValue<T>` storage
  with validated type traits, alignment, exactly-once construction/destruction,
  and successful-compile transfer.
- [x] Add canonical typed-value read/write declarations that lower into the
  existing value-version compiler model without implementing separate
  dependency, culling, or scheduling semantics.
- [x] Enforce one writer, explicit readers, graph ownership, type identity,
  producer-before-read, and immutable declaration after pass submission.
- [x] Add stable value type metadata suitable for diagnostics without relying
  on compiler-specific RTTI/type-name text or addresses.
- [x] Preserve legacy token behavior as the bounded compatibility surface and
  prove token and typed-value dependency/culling equivalence.
- [x] Cover trivial and non-trivial payload lifetime across builder destruction,
  compile failure, culling, backing failure, execution failure, success, and
  compiled-graph destruction.

#### Acceptance Gate

- Typed values use the existing compiler's exact dependency versions,
  hazard frontier, culling closure, scheduling, and logical lifetimes.
- Missing/duplicate writers, wrong types, foreign handles, and invalid reads
  fail deterministically with no consumer callback or partial payload exposure.
- Supported payloads are aligned, constructed once, and destroyed once on every
  exit path; unsupported types fail at the documented boundary.

### Stage 3: Integrate typed values with pass parameters and resolution

- [x] Extend graph parameter metadata with typed-value read/write member kinds,
  stable field paths, ordered traversal, optionality disposition, and type
  identity validation.
- [x] Lower typed-value parameter members atomically through parameterized
  `AddPass` and reject mixed/overlapping manual and parameter declaration
  authority.
- [x] Extend the typed parameter resolver so exact write members resolve mutable
  payloads and exact read members resolve const payloads only for the current
  pass.
- [x] Reject raw/copied members, wrong-pass or wrong-direction resolution,
  foreign values, wrong types, culled callbacks, and unavailable execution
  preparation before exposing storage.
- [x] Extend deterministic dumps/captures and table-driven manual-versus-
  parameterized equivalence fixtures with value name, type key, direction, and
  parameter field path.
- [x] Measure value allocation, metadata traversal, and capture growth against
  the existing RenderGraph Debug structural/CPU budgets.

#### Acceptance Gate

- A parameterized callback can mutate only its declared value writes and read
  only its declared value reads; no other typed payload is observable.
- Equivalent manual and parameterized graphs have identical dependencies,
  culling, lifetimes, ordering, callback results, and normalized captures after
  field provenance is accounted for.
- Invalid metadata or resolution fails atomically and equal declarations
  produce byte-stable pointer-free diagnostics.

### Stage 4: Replace scene-local imports/channels and prove exact fallback

- [x] Replace `PersistentTextureImports` with direct canonical builder
  registration and verify every scene external texture retains its selected
  initial/final access and stable capture identity.
- [x] Replace `FSceneFrameGraphExecutionChannels` payload storage with
  graph-owned typed scene values while retaining only the smallest temporary
  aggregate of typed handles needed by unmigrated contributor signatures.
- [x] Convert each scene result writer/reader declaration and callback access to
  the typed value API without changing pass topology, recorder inputs, result
  status, telemetry, temporal commit, or output publication.
- [x] Resolve the Stage 0 candidate/fallback pilots before registration and pass
  declaration; remove callback captures/lookups of the unselected alternatives.
- [x] Verify disabled, compute/fragment, isolated deferred, present/offscreen,
  resize, multi-view, missing optional input, target-preparation failure, and
  device/resource recovery routes.
- [x] Compare owning captures, transitions, images/readbacks, draw/dispatch
  identities, telemetry, memory, CPU, and synchronized GPU qualification with
  the frozen oracle.

#### Acceptance Gate

- The production scene composer contains no local physical-import deduplication
  table and no token-plus-mutable-payload execution channel.
- Captures contain one canonical resource for each physical import and only the
  exact preselected candidate or fallback used by each migrated pilot.
- All typed results have one declared writer and explicit readers, and scene
  structure, failure atomicity, output, recovery, and accepted budgets remain
  equivalent.

### Stage 5: Harden, document, and hand off Milestone 2

- [x] Run focused and aggregate RenderCore tests, Renderer scene contracts,
  representative RHI/Vulkan validation and rendered-output fixtures, recovery
  routes, qualification coverage, and the required build tier under repository
  guidance.
- [x] Record canonical resource/value counts, capture size, dependency and
  transition structure, compile/execute CPU, memory, output, and synchronized
  GPU evidence against frozen gates; diagnose regressions instead of silently
  raising budgets.
- [x] Update the lasting Render Graph contract with external registration,
  typed-value ownership/access, compatibility, diagnostics, and failure rules.
- [x] Update renderer preparation documentation after scene-local import/channel
  infrastructure is removed and fallback selection boundaries are proven.
- [x] Update the roadmap Milestone 2 state and Milestone 3 entry-gate
  disposition from actual validation evidence.
- [x] Record exact validation, close only passed checklists, and prepare the
  repository-required plan/stage commit provenance.

#### Acceptance Gate

- Selected focused, aggregate, build, Vulkan, rendered-output, recovery,
  documentation, structural, memory, CPU, and synchronized GPU gates pass with
  recorded evidence.
- Code, tests, captures, lasting contracts, and the roadmap agree on one
  canonical import and typed-value model with renderer-owned fallback policy.
- Milestone 3 is either ready for a newly created bounded plan or remains
  proposed with its missing entry evidence stated explicitly.

## Validation Matrix

| Concern | Required evidence |
| --- | --- |
| External identity | Same-pointer texture/buffer registration with equal contracts returns one handle/resource; cross-builder identities remain independent |
| Import conflicts | Initial/final access, kind, description, name provenance, and null conflicts produce deterministic pre-recording diagnostics |
| Typed storage | Type traits, alignment, construction/destruction, compile transfer, culling, and every failure/success lifetime path are covered |
| Value semantics | One writer, explicit readers, missing/duplicate producer, RAW ordering, culling closure, lifetime, and stable scheduling match token/compiler oracles |
| Parameter capability | Manual/parameter equivalence plus exact typed read/write, wrong-type, foreign, copied, wrong-pass, wrong-direction, optional, and unavailable cases |
| Fallback | Candidate/fallback selection precedes compile; owning capture contains only the selected handle and output/failure policy matches baseline |
| Scene migration | Every prior channel result retains writer/readers, pass topology, status, telemetry, temporal/output transaction, resize, and multi-view isolation |
| Diagnostics | Errors and captures name resource/value, stable type key, contract, pass, direction, and parameter field path without addresses or RTTI spelling |
| Structure and performance | Resource/value counts, dependencies, transitions, capture bytes, compile/execute median and p95, memory, and synchronized GPU timings remain inside frozen gates |
| Platform | Focused and aggregate native tests, required builds, inline/threaded recording where applicable, Vulkan validation, rendered output, recovery, and Editor smoke follow repository guidance |
| Documentation | Changed/all documentation plus all-plan and all-roadmap lifecycle validators pass after final edits |

## Definition of Done

- RenderCore returns one canonical graph handle for repeated equivalent external
  texture/buffer registration and rejects conflicting contracts atomically.
- Typed graph values have graph-owned storage, stable type metadata, one writer,
  explicit readers, existing compiler semantics, deterministic capture, and
  exact pass-scoped read/write capability.
- Legacy tokens remain only as a bounded compatibility surface; migrated
  production results have no side payload outside graph ownership.
- The scene composer owns neither a physical-import deduplication table nor
  mutable execution-channel payload storage.
- Representative fallback inputs are selected before compile, and graph uses,
  captures, shader inputs, and output prove that only the selected resource is
  accessible.
- Scene pass order, dependencies, lifetimes, culling, transitions, results,
  telemetry, temporal/output transactions, recovery, images/readbacks, and
  accepted structural/performance budgets remain equivalent.
- Lasting Render Graph and renderer-preparation documentation plus the roadmap
  reflect the landed contract and actual validation evidence.
- Changes are staged and committed with repository-required plan provenance
  after successful validation.

## Deferred Follow-ups

- Graph and shader parameter metadata composition on the same pass object,
  including typed graphics/compute binding, optional SRV/UAV, and arrays.
- Contributor-by-contributor narrow input/output signatures, complete
  parameterized-pass migration, broad-context removal, and compatibility API
  retirement.
- Parameter-aware scene inspection and enforcement for new production passes.
- Physical transient aliasing, queue-aware scheduling, split barriers,
  parallel recording, pass merging, compiler reordering, and persistent graph
  reuse, each gated independently by roadmap evidence.

## Related Documentation

- [Render Graph Parameter-Driven Authoring Roadmap](../../../Roadmaps/Archive/2026-08/RenderGraphParameterDrivenAuthoring.md)
- [Render Graph Pass Parameters Foundation](RenderGraphPassParametersFoundation.md)
- [Render Graph](../../../Runtime/Rendering/RenderGraph.md)
- [Renderer Frame Preparation and Render Graph Execution](../../../Runtime/Rendering/RendererFramePreparation.md)
- [RHI Resource Transitions](../../../Runtime/Rendering/RHIResourceTransitions.md)
- [RHI Command Execution](../../../Runtime/Rendering/RHICommandExecution.md)
- [Renderer Resource Recovery](../../../Runtime/Rendering/RendererResourceRecovery.md)
- [Testing](../../../Agents/Testing.md)
- [Build and Run](../../../Agents/BuildAndRun.md)

## Related Code

- `Engine/Source/Runtime/RenderCore/Public/RenderGraph.h`
- `Engine/Source/Runtime/RenderCore/Private/RenderGraph.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/SceneFrameGraphTypes.h`
- `Engine/Source/Runtime/Renderer/Private/Renderers/SceneFrameGraphComposer.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/SceneFrameGraphContributors.h`
- `Engine/Source/Runtime/Renderer/Private/Renderers/SceneFrameAmbientOcclusion.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/SceneFrameBaseScene.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/SceneFrameDeferredLighting.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/SceneFramePostProcess.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/SceneFrameSceneColor.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/SceneFrameVolumetricCloud.cpp`
- `Engine/Tests/Native/RenderCoreTests/Private/RenderGraphTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/RendererSceneContractTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/GBufferQualificationTests.cpp`
