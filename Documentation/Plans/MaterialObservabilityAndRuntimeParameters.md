# Material Observability and Runtime Parameters Plan

Summary: Measure material compilation and runtime update costs, add atomic dynamic-instance batches, and deliver world-scoped numeric parameter collections without runtime shader compilation.

Last reviewed: 2026-09-29

Status: Completed
Completed: 2026-09-29

## Current Status

Stages 0-2 provide detached accepted-program and loaded-family statistics, bounded
compile/proxy/RHI owner diagnostics, MaterialEditor display, and the dynamic-only
atomic Set/Clear API with one validated commit/publication path. Deterministic CPU
coverage proves rollback, no-op, failure-location, Cook and structural counter
conservation behavior.

Stage 3 adds the versioned numeric collection asset and editor, collection graph
expressions and detached compiler/function capture, world-owned atomic snapshots,
DMAT v8 source-free loading, and indexed collection buffers shared by Forward,
GBuffer and reachable masked shadow. Defaults and world values do not change shader
identity; schema changes do. Opaque shadow remains resource-free.

Qualification disposition (2026-09-29): the user confirmed that the two prerequisite
plans are considered wrapped up and that no Windows RTX 3090 environment is
available. The exclusive Windows/RTX 3090 timing, allocation and retained-memory
comparison is therefore explicitly skipped for this plan. This is a disposition,
not a passing receipt: the diagnostic macOS Debug measurements below remain
non-authoritative, no numeric performance budget is claimed, and no performance
improvement or regression statement may be made from them. Correctness, structural
counter conservation, available GPU coverage, builds and documentation remain
required.

Final validation receipts are recorded in Stage 4. The implementation is complete;
only the named unavailable Windows performance comparison is dispositioned, without
a pass or performance claim.

The completed [Runtime Dynamic Material Instances plan](Archive/2026-09/RuntimeDynamicMaterialInstances.md)
provides transient instances with typed scalar, vector and Texture2D overrides,
accepted-program reuse, stable render proxies, and no package dirtiness or
runtime compilation. Each successful setter currently rebuilds and publishes
the owner's complete local layer immediately. Proxy-side command coalescing can
replace a pending publication, but it does not provide caller-visible atomicity,
one validation result for a related set of edits, or measured update bounds.

Compilation already exposes per-request duration/cache outcome and fixed aggregate
queue/cache diagnostics. Accepted compiler results retain normalized IR, generated
source, compiled shaders, layout, active declarations, and phase timings. Material
render proxies expose publication, coalescing, resolution-cache, stale-publication,
binding-update and validation-failure counters. MaterialEditor displays basic
compile state, cache outcome, target, generation and duration. This plan extends
those existing authorities; it does not create a second profiler or retain an
unbounded event history.

There is no material parameter collection asset, collection expression, world-
scoped runtime value owner, or renderer binding source. Global numeric changes
therefore require per-instance mutation today. The material roadmap also contains
stale text claiming that transient instances are absent; activating this plan
reconciles that status.

### Stage 0 Measurement Contract (2026-09-29)

Counter/statistic ownership is frozen as follows:

| Facts | Authority and thread | Semantics |
| --- | --- | --- |
| Accepted program IR, declarations, layout/resources, source/code bytes, dependencies and phase timings | Engine `FMaterialProgramStatistics`, queried on GameThread | Current detached snapshot; compiled code uses checked `uint64` accumulation; absent/current/last-known-good is explicit |
| Request/cache outcome and aggregate queue/cache work | Engine compile status and `FMaterialCompilationDiagnostics` | Per-owner current status plus mutex-protected cumulative process counts and retained/in-flight occupancy |
| Loaded-family programs/static configurations | Engine loaded material query | Current GameThread snapshot of loaded authored root/instances; dynamic owners excluded; no package load |
| Dynamic records and commits | Engine `FMaterialDynamicParameterCounters` | Cumulative submitted records and changed/no-op/rejected commits; owner publications equal changed commits |
| Proxy queue/application/resolution and copied payload bytes | Engine render proxy counters | Atomic cumulative process work plus current pending gauge; reset only on GameThread after draining relevant render work |
| Surface sampler lookup/create/reuse/failure and retained slots | Renderer `FSurfaceMaterialResources` | RenderThread cumulative work and retained occupancy; `lookups = creations + reuses + failures` |
| Pipeline creation/cache and upload/storage | RHI pipeline-creation/cache and memory/upload diagnostics | Backend-owned bounded snapshots; this plan does not duplicate them |

The final owner review did not add a duplicate Engine material shader-map/pipeline
snapshot: bounded Renderer caches retain their explicit entry budgets, while RHI's
pipeline creation/cache diagnostics remain the public work/occupancy authority and
surface material resources retain exact sampler counters. The macOS default-zone
sampler is not an exact allocation-count authority. Exact allocation-count and
authoritative retained-memory comparison therefore remain part of the explicitly
skipped Windows performance qualification, not a claimed observability result.

Conservation rules are `changed commits = owner publications`, no-op and rejected
commits publish zero work, `queued publications = applied + coalesced + stale +
pending` after shutdown/cancellation accounting is added, and surface sampler
lookups obey the equation above. Current counters have fixed-size storage and
retain no owner or event history.

Representative compile fixtures are the qualification target's default small
opaque program, PBR masked/texture graph, transitive function/variant fixtures,
maximum-node layout fixture, and eight-owner/four-identity static-variant family.
The compiler fixture currently records 167 normalized nodes, 8,185 canonical
bytes, 14,075 generated-source bytes, six texture samples, four shaders, 138,244
compiled-code bytes, one dependency, and 142,252 cooked bytes. Exact values are
schema fixtures; timings are sampled separately.

`DynamicInstanceUpdateWorkloads` creates 1,000 and 10,000 transient instances and
runs 1, 4 and 16 active numeric parameters. Each case uses one warm-up and three
alternating measured samples for individual setters, one atomic batch, a no-op
batch and a rejected duplicate-GUID batch. It records median/p95, changed commits,
queued waves, copied logical payload bytes, and sampled default-zone peak-byte
increase. The fixed batch gates are one changed commit/publication per instance per
sample, no work for no-op/rejected batches, and payload independent of record count.

The future collection workload is also frozen: 1/16/128 declarations, 1/4
collections per expanded closure, 1/1,000/10,000 referencing draws, one update and
60 consecutive update frames, plus two simultaneous worlds with different values.
It must record world publications/versions, coalescing, uploads/payload bytes,
material compile/proxy deltas, retained bytes, and Forward/GBuffer/masked-shadow
results.

Diagnostic run receipt: `./DevTool test MaterialQualificationTests --mode
qualification --report` passed on 2026-09-29 using
`MacOS-arm64-Debug-DurinEditor`; report:
`Build/NativeTestResults/MacOS-arm64-Debug-DurinEditor/MaterialQualificationTests.xml`.
A direct filtered diagnostic run reported batch median/p95 microseconds of
46,641/46,774, 54,343/54,478 and 84,875/85,057 for 1K × 1/4/16, and
482,656/484,148, 566,314/579,400 and 903,627/925,505 for 10K × 1/4/16.
The corresponding individual-setter p95 values were 47,550, 196,823, 900,662,
484,964, 2,005,230 and 9,267,320 microseconds. Batch copied payload was 2,544,000
bytes for 1K and 25,440,000 bytes for 10K in every record-count case (four commits
per instance); individual payload scaled linearly with record count. Sampled batch
peak default-zone increases were 9,296/9,696/11,296 bytes for 1K and
9,296/9,696/39,952 bytes for 10K; individual peaks were
1,673,232/89,312/440,976 and 16,649,248/809,312/4,329,216 bytes respectively.
These Debug
measurements are diagnostic only because exclusive-host quietness was not proven;
they do not freeze or satisfy numeric regression budgets.

## Goal

Make material cost and variant growth visible before adding more permutation
features. Let gameplay update several parameters on one dynamic material instance
atomically with one owner publication. Let one world-scoped numeric collection
update every referencing material without editing assets, recompiling programs,
or publishing each material proxy.

The completed system must provide deterministic structural counters and measured
representative workloads. Timing claims require the repository's performance-
qualification workflow; correctness does not depend on an unavailable timing
lane, but this plan cannot close its scalability gate without recorded limits.

## Scope and Non-Goals

In scope:

- Per-material accepted-program statistics derived from existing compiler results:
  normalized IR node count, active parameter count, compiled layout uniform bytes,
  texture/sampler counts, generated-source bytes, compiled shader count/code bytes,
  dependency count, phase timings, total request duration and cache outcome.
- Loaded-family variant visibility: distinct accepted program identities and static
  configurations among a root material and its loaded authored instances. This is
  an editor/game-thread query, not a permanent global history.
- Fixed aggregate runtime diagnostics for logical parameter mutations, atomic
  commits, no-op/rejected commits, owner/dependent publications, queued/coalesced
  proxy waves, render-thread applications/resolutions and copied material payload
  bytes. Counters must state their owning thread and conservation relationships.
- A single-call, all-or-nothing multi-parameter update API for dynamic material
  instances, including Set and Clear operations by persistent parameter GUID.
- A persistent `DMaterialParameterCollection` asset with stable declarations and
  defaults for Scalar, Vector2, Vector3 and Vector4 values.
- A collection-parameter material expression usable by root materials and material
  functions; deterministic compiler/cook dependencies; world-scoped overrides;
  and Forward, GBuffer and applicable masked-shadow binding.
- Content Browser creation, bounded collection editing, material-graph collection
  parameter authoring, MaterialEditor statistics, runtime APIs, Cook, reload,
  PIE/multiple-world isolation, shutdown, and focused diagnostics.

Out of scope:

- Static Bool/Switch parameters, quality/feature-level switches, new shader stages,
  new surface outputs, shading models or material domains. They consume the metrics
  established here but belong to later plans.
- Texture, sampler, buffer, cube or virtual-texture values in a collection. The
  first collection slice is numeric so it adds one bounded uniform-buffer contract
  without introducing descriptor-array or resource-residency policy.
- Dynamic-to-dynamic instance parenting, persistent batching for authored assets,
  background mutation, arbitrary-thread setters or runtime shader compilation.
- Backend-independent GPU instruction counts. This plan reports exact source,
  bytecode, layout and resource facts; it must not label bytecode size as executed
  instruction cost.
- A general telemetry database, per-frame event log, remote profiler, material
  layers, automatic shader optimization, descriptor virtualization or bindless
  resources.

## Selected Decisions

### Statistics and ownership

- Engine owns `FMaterialProgramStatistics`, a value snapshot derived from one
  accepted `FMaterialCompilerResult`. It contains no object pointers and no new
  cache. Byte totals use checked `uint64` accumulation. Missing or last-known-good
  programs are represented explicitly; failed compilation never fabricates zero-
  cost success statistics.
- Existing `FMaterialCompileStatus`, `FMaterialCompilationDiagnostics` and
  `FMaterialRenderProxyCounters` remain their current authorities. Extend them
  only for facts their owners can count exactly. Renderer-private shader-map,
  pipeline and uniform-upload owners expose bounded occupancy/work counters through
  an existing diagnostics snapshot boundary; Engine does not inspect renderer
  cache internals.
- MaterialEditor adds a Statistics section to its existing Diagnostics panel.
  It shows the selected owner, accepted root program, loaded-family distinct
  variants, exact layout/resource/code sizes and current process counters. Values
  are labeled as current snapshots or process aggregates. Refreshing the panel
  must not compile, load packages, create renderer resources or retain material
  owners.
- Stage 0 records current representative assets and exact workload definitions.
  Numeric timing/allocation regression budgets are written into this plan before
  Stage 2 implementation begins. Structural conservation gates below are fixed
  now and cannot be relaxed to accommodate an implementation.

### Atomic dynamic-instance updates

- Add one value-owned update record with operation `Set` or `Clear`, parameter GUID
  and a value for Set. Add one `DMaterialInstance::ApplyDynamicParameterUpdates`
  operation accepting a borrowed span for the duration of the call. Do not store
  a mutable open transaction on the instance and do not rely on a destructor to
  commit or roll back.
- The operation is GameThread-only and accepts only a dynamic instance. It captures
  the current parent accepted-program identity and declaration contract, rejects
  duplicate GUIDs, validates every record and builds candidate typed storage before
  changing the owner. A missing, unreachable, mismatched, nonfinite or invalid-
  sampling Set, or an invalid Clear, rejects the whole batch with the failing
  record index and parameter identity. No owner state, package state, notification,
  version or render publication changes on rejection.
- Clear of a reachable parameter with no local override is a valid no-op. A fully
  no-op batch succeeds without incrementing render state, notifying dependents or
  publishing. Existing single-value setters preserve source compatibility and
  route dynamic owners through the same validation/commit primitive.
- One changed commit swaps the candidate storage, increments the owner render-state
  version once, builds one complete local layer, notifies the existing dependent
  query once and queues at most one proxy publication wave per affected owner.
  It never marks a package dirty and never requests compilation. Listener edits
  begin a fresh operation after the committed notification, preserving the current
  synchronous reentrancy boundary.
- Atomicity is CPU/publication atomicity. Already-recorded frames may use the prior
  immutable representation; later frames see the complete committed layer. No
  partial parameter set may be observable through the owner or its proxy.

### Collection asset and runtime values

- `DMaterialParameterCollection` is an ordinary Engine asset. Each declaration has
  a persistent GUID, case-insensitive unique name, one supported numeric type,
  display metadata and a finite default. Authored order is presentation only;
  canonical layout order is GUID-sorted. Rename/reorder preserves identity. Retype
  or deletion of a referenced declaration is a diagnosed schema change, never a
  silent reinterpretation.
- The first version allows at most 128 declarations per collection and at most four
  distinct collections in one expanded material/function closure. These are schema
  validation limits, not best-effort renderer failures. Layout construction reuses
  the material compiler's checked uniform alignment/packing rules.
- `DMaterialExpressionCollectionParameter` references a collection asset and
  declaration GUID. Its output width follows the declaration. Material functions
  may contain the expression; caller expansion preserves the collection dependency.
  Missing assets/declarations, type changes, duplicate collection-layout identities
  and limit overflow produce source-located material diagnostics.
- Compiler input captures detached collection schema records. Program identity and
  shader identity include collection asset identity plus canonical layout/schema,
  but exclude defaults and world runtime values. Generated shaders declare only
  reachable collection buffers in deterministic identity order. Collection default
  or runtime-value changes never compile material programs; schema changes
  invalidate dependent materials through the existing loaded dependency boundary.
- Cook records collection asset dependencies and embeds the exact required
  collection layout identities in DMAT. Cooked Game loads no authored expression
  graph, but loads the referenced collection assets for defaults. Missing or
  incompatible cooked collection layouts select the existing whole-material error
  path with a collection-specific diagnostic; they do not bind mismatched bytes.

### World and renderer boundary

- A registered Engine `DMaterialParameterCollectionSubsystem` owns runtime values
  per `DWorld`. It lazily creates one instance state per referenced collection from
  asset defaults, retains collection assets through reflected ownership, and offers
  single and atomic-batch numeric mutation by GUID/name. Editor and PIE worlds are
  isolated. There is no process-global mutable value singleton.
- A changed collection commit publishes one immutable identity/layout/payload
  snapshot from the world to its scene. Repeated commits before render-thread
  consumption coalesce by collection identity and monotonic world-local version.
  Destruction detaches the world/scene producer and drains existing render-command
  ownership before releasing snapshots.
- Renderer scene state owns render-thread collection snapshots keyed by collection
  identity. A material proxy does not copy collection values and a collection update
  does not dirty or publish material proxies. Draw preparation resolves the exact
  collection layouts requested by the accepted material program against the
  current scene snapshot, falling back to asset defaults only when the world has no
  override. An incompatible or absent required layout rejects the affected draw
  through the existing material fallback/diagnostic path.
- Extend the reflected surface binding vocabulary with an indexed collection-
  uniform source. Forward and GBuffer bind reachable collections. Masked shadow
  binds them only when reflected opacity-mask code uses them; opaque shadow remains
  resource-free. Geometry families continue sharing one material execution path.
- Collection payload allocation/upload follows the active RHI asynchronous upload
  contract. This plan must coordinate with the active RHI upload and multi-queue
  plans rather than adding a private persistent mapped-buffer allocator or assuming
  frame-start completion.

## Required References and Coordination

- [Material system roadmap](../Roadmaps/MaterialSystem.md) owns milestone ordering
  and the boundary with later expression/stage/shading work.
- [Material system](../Runtime/Rendering/MaterialSystem.md),
  [material expression building](../Runtime/Rendering/MaterialExpressionBuilding.md),
  and [Material Editor lifecycle](../Editor/Architecture/MaterialEditorLifecycle.md)
  own the implemented contracts after completion.
- Follow [Build and run](../Agents/BuildAndRun.md), [Testing](../Agents/Testing.md)
  and [Documentation](../Agents/Documentation.md). Timing qualification additionally
  follows the linked performance-qualification guidance from Testing.
- The active [Geometry Submission Refactor](GeometrySubmissionRefactor.md) owns
  generic mesh/factory execution and final performance qualification. Preserve its
  shared material execution boundary and do not add geometry-family collection paths.
- The active [RHI Asynchronous Buffer Upload Refactor](RhiAsyncBufferUploadRefactor.md)
  and [RDG/RHI Multi-Queue Execution](RdgRhiMultiQueueExecution.md) own upload storage,
  GPU readiness and retirement. Reconcile their current stages before Stage 3
  changes uniform preparation or retained resource lifetime.
- Start source inspection in `Engine/Source/Runtime/Engine/Public/Materials`,
  `Engine/Source/Runtime/Engine/Private/Materials`,
  `Engine/Source/Runtime/Renderer/Private/Renderers/SurfaceMaterial.*`, material
  shader-map/pipeline cache owners, `Engine/Source/Editor/MaterialEditor`, and
  `Engine/Tests/Native/EngineTests/Private/Materials`. Search every source/test
  root declared in `Durin.dworkspace` before changing a shared API.

## Implementation Stages

### Stage 0: Freeze Workloads, Counters and Budgets

Dependency: none. Outcome: the phase-0 measurement contract is executable before
runtime behavior changes.

- [x] Inventory every existing material compilation, proxy publication, renderer
  shader-map/pipeline/resource and uniform-upload counter. Assign each requested
  statistic one owner and identify exact missing instrumentation.
- [x] Freeze per-material statistics semantics, loaded-family variant query rules,
  counter reset/snapshot thread contracts and conservation equations. Specify which
  values are current, cumulative, retained occupancy or per-frame work.
- [x] Create deterministic representative fixtures: small opaque, masked texture,
  function-heavy, maximum practical parameter-layout and multiple static-variant
  materials. Record current IR/layout/resource/source/code facts and compile/cache
  outcomes without changing production behavior.
- [x] Define runtime workloads for 1,000 and 10,000 dynamic instances with 1, 4 and
  16 changed numeric parameters per instance, plus no-op and rejected edits. Record
  current individual-setter GameThread cost, allocations, notifications, owner and
  dependent publications, render commands, payload bytes and render-thread applies.
- [x] Define collection workloads before implementation: 1/16/128 declarations,
  1/4 collections per material, 1/1,000/10,000 referencing draws, one update and
  60 consecutive update frames, with two simultaneous worlds holding different
  values.
- [x] On an exclusive quiet timing lane, use identical build/profile, fixtures,
  warm-up and sampling for baselines and later comparisons. Record median/p95,
  allocation and retained-byte results, then amend this plan with numeric timing
  and memory regression budgets before Stage 2. If the lane is unavailable, leave
  this gate open; do not substitute structural counters for timing evidence.
  Disposition: the required Windows RTX 3090 lane is unavailable and the user
  explicitly skipped this qualification on 2026-09-29. No numeric budget or pass is
  recorded; the macOS Debug receipt remains diagnostic only.

Fixed structural gates:

- A changed dynamic batch produces one logical commit, one owner render-state
  increment and at most one queued publication wave per affected owner, independent
  of record count. A no-op or rejected batch produces none.
- A collection value commit produces one world collection version/publication and
  no material compilation, material package dirtiness or per-material proxy
  publication, independent of referencing material/draw count.
- All aggregate counters conserve accepted work into applied, coalesced, rejected,
  canceled or still-pending categories. Reset cannot race an unsupported thread.
- Diagnostics and retained statistics have fixed-size aggregate storage; no work
  item, frame or material identity history grows without an explicit bound.

Completion: every retained metric has one authority and fixtures/commands are
recorded. The unavailable Windows timing/memory budget is named and dispositioned
without substitution; later stages retain no ambiguous measurement semantics.

### Stage 1: Publish Material Cost and Variant Observability

Dependency: Stage 0 metric definitions. Outcome: phase 0 is usable from tests and
MaterialEditor without changing material rendering.

- [x] Add the detached accepted-program statistics snapshot and checked size/count
  derivation. Cover absent, ready, last-known-good, failed and cooked programs.
- [x] Extend existing compilation/proxy/renderer diagnostic snapshots with only the
  missing Stage 0 counters. Add reset, thread and conservation tests; preserve
  bounded cache ownership and avoid per-request history.
- [x] Add the loaded-family distinct variant query using loaded material dependency
  data without package loading. Count exact accepted program/static identities and
  label pending/failed owners separately.
- [x] Show per-material statistics, loaded variants and relevant aggregate counters
  in the existing MaterialEditor Diagnostics panel. Keep compiler diagnostics and
  node navigation unchanged; add focused model/interaction coverage rather than
  screenshot-only verification.
- [x] Record the final phase-0 fixture results in this plan and document the lasting
  statistics contract in the owning Runtime/Editor documents.

Completion: an artist or test can explain current compiled size/resources,
compile/cache outcome, loaded variants and update/publication work from bounded
snapshots, with no compile/load/resource-creation side effect.

### Stage 2: Add Atomic Dynamic-Instance Update Batches

Dependency: Stage 1 counters and the Stage 0 measurement disposition. Outcome: related runtime
overrides commit as one validated publication.

- [x] Add update records, structured error/index reporting and the dynamic-only
  batch API. Centralize single and batch validation so existing setters retain
  behavior and cannot drift from batch admission rules.
- [x] Build complete candidate typed storage before commit; reject duplicates and
  all invalid/unreachable/type/sampling cases without observable mutation. Preserve
  vector canonicalization and texture reference traversal.
- [x] Commit changed candidate storage once, publish/notify once through existing
  dependency and proxy seams, and prove no package dirtiness or compilation. Cover
  listener reentrancy, parent accepted-generation changes, failure and shutdown.
- [x] Add deterministic tests for mixed Set/Clear, no-op, rollback, error location,
  independent instances, dependent instances, cooked Game contracts and exact
  Stage 0 publication/counter conservation.
- [x] Run the frozen 1K/10K workloads and record timing/allocation results against
  the Stage 0 budgets. Optimize only measured owners; do not add pooling, descriptor
  virtualization or alternate renderer paths without evidence.
  Disposition: the user explicitly skipped the unavailable authoritative Windows
  RTX 3090 run. The existing macOS Debug workload remains diagnostic only; its
  values are not compared to a budget and do not establish a performance pass.

Completion: batch size does not change logical publication count, no partial state
is observable, and all structural gates pass. The named performance gate is
dispositioned without a budget or pass.

### Stage 3: Add World-Scoped Numeric Parameter Collections

Dependency: Stage 1 observability, Stage 2 atomic update semantics, and reconciled
RHI upload/multi-queue ownership. Outcome: one world value update reaches every
referencing material through a shared bounded binding.

- [x] Add versioned collection asset schema, validation, defaults, stable identity,
  reference discovery, package round trip, move/reload/delete behavior and Content
  Browser creation. Add a minimal list editor with Undo/Redo, save and diagnostics.
- [x] Add collection expressions and graph authoring, function-closure propagation,
  detached compiler capture, normalized IR/lowering, exact identities and source-
  located diagnostics. Enforce the 128-declaration/four-collection limits.
- [x] Add the world subsystem and atomic numeric update API. Preserve independent
  editor/PIE/game worlds, default fallback, collection/texture-free GC correctness,
  level changes, scene detach, module unload and engine shutdown.
- [x] Extend material program/layout/cooked versions and surface shader reflection
  for indexed collection buffers. Bind exact snapshots in Forward, GBuffer and
  reachable masked shadow without changing opaque-shadow resource freedom or
  adding geometry-family branches.
- [x] Integrate Cook and source-free runtime loading. Prove defaults and overrides
  do not change shader identity or compile count, while schema changes invalidate
  all loaded dependents and incompatible cooked layouts fail deterministically.
- [x] Add CPU fixtures for asset/compiler/function/world/PIE/Cook/reload/shutdown
  behavior and GPU qualification for Forward, GBuffer and masked-shadow values,
  including two worlds with different results and update/failure recovery.

Completion: a collection value commit changes all referencing draws in its world
without asset mutation, compilation or material-proxy publication; another world
remains unchanged; every supported pass consumes the same accepted layout.

### Stage 4: Qualify Scalability and Publish Contracts

Dependency: Stages 1-3. Outcome: both evolution phases have evidence-backed limits
and durable documentation.

- [x] Run `test affected --explain`, then the smallest sufficient affected/material
  selection. At minimum cover MaterialCompilerTests, MaterialCompileLifecycleTests,
  MaterialRuntimeTests, MaterialFunctionTests, MaterialGraphEditingTests,
  MaterialEditingPersistenceTests, MaterialEditorInteractionTests,
  MaterialCookTests and StaticMeshMaterialTests where selected by changed ownership.
- [x] Run required GPU material qualification because collection bindings change
  shader reflection and rendered values. Record backend/device, exact Forward,
  GBuffer, masked-shadow, reload/recovery coverage and unavailable explicit gates.
- [x] Complete the shared Engine/Renderer API `all` build and validate affected
  Sandbox/RoadWeaver targets and consumers declared in `Durin.dworkspace`.
- [x] Rerun every frozen timing/allocation workload on the implementation revision.
  Record median/p95, allocations, retained bytes, payload/upload bytes and counter
  conservation; close only the budgets actually measured on a valid quiet lane.
  Disposition: no valid Windows RTX 3090 lane exists, so this item closes only by
  the user's explicit skip. No timing, allocation or retained-memory budget closes.
- [x] Move lasting asset, compiler, world, renderer, editor and Cook contracts to
  their owning documents. Reconcile the material roadmap, remove stale claims and
  run changed-document, all-plan and all-roadmap validation.
- [x] Record exact receipts and limitations here. Mark the plan complete only when
  correctness, GPU, timing/memory, build and documentation gates all pass or the
  user explicitly dispositions a named qualification without representing it as
  passed.

Final receipts (2026-09-29):

- `./DevTool test affected --explain` selected 88 routine native-test targets from
  the Engine, Renderer and MaterialEditor ownership changes. `./DevTool test
  affected --test-jobs 4 --report` passed all 88 CTest targets with zero failures;
  report: `Build/NativeTestResults/MacOS-arm64-Debug-DurinEditor/affected.xml`, log:
  `Build/.agent-state/logs/20260929-205624-333167-37214-ctest.log`.
- The latest direct `MaterialRuntimeTests` run passed 88/88 GoogleTest cases; report:
  `Build/NativeTestResults/MacOS-arm64-Debug-DurinEditor/MaterialRuntimeTests.xml`.
- `./DevTool test StaticMeshRenderPreparationVulkanTests --mode qualification
  --report` passed the full ten-case target on Vulkan 1.3.334 / Apple M4, including
  two-world Forward and GBuffer values, masked-shadow update/failure recovery,
  opaque-shadow resource freedom and the existing resource-replacement recovery
  suite. Report: `Build/NativeTestResults/MacOS-arm64-Debug-DurinEditor/
  StaticMeshRenderPreparationVulkanTests.xml`; log:
  `Build/.agent-state/logs/20260929-205450-996619-37078-ctest.log`.
- `./DevTool build` passed the shared `all` target for
  `MacOS-arm64-Debug-DurinEditor`; log:
  `Build/.agent-state/logs/20260929-205646-990944-38544-cmake.log`.
- `./DevTool doc validate --scope changed`, `./DevTool doc plan validate --scope
  all`, and `./DevTool doc roadmap validate --scope all` passed.
- The exclusive Windows RTX 3090 timing/allocation/retained-memory gate was not run
  because the environment is unavailable. This is the user's explicit
  qualification disposition and is not recorded as a pass or performance result.

Completion: phase 0 exposes reliable costs and variants; phase 1 provides bounded
atomic instance and world-global numeric updates; structural update/lifetime limits
are qualified. The unavailable Windows performance gate is explicitly dispositioned
and unclaimed rather than silently waived.

## Validation Matrix

| Area | Required evidence |
| --- | --- |
| Statistics | Exact derivation, overflow checks, absent/failed/last-known-good/cooked state, bounded storage, thread-safe snapshots/resets, conservation |
| Dynamic batch | Atomic Set/Clear, duplicate/error location, no-op, rollback, one publication, dependency notifications, texture retention, cooked runtime, shutdown |
| Collection asset | Stable GUID/name rules, deterministic layout, limits, round trip, duplicate/malformed rejection, move/reload/delete, Cook dependencies |
| Compiler | Root/function expressions, deterministic normalized identity/source, defaults/value exclusion from shader key, schema invalidation, diagnostics, source-free load |
| World/runtime | Defaults, atomic overrides, two-world/PIE isolation, level/scene lifecycle, coalescing, no material-proxy fan-out, shutdown |
| Renderer | Exact reflection/layout matching, Forward/GBuffer/masked-shadow values, opaque-shadow resource freedom, StaticMesh/SplineMesh/common-factory parity, reload/recovery |
| Editor | Creation, list editing, Undo/Redo, save/reload, node picker, diagnostics navigation, statistics side-effect freedom |
| Scalability | Frozen 1K/10K instance and 1/10K draw collection workloads, median/p95, allocations, retained/payload bytes and structural counters |
