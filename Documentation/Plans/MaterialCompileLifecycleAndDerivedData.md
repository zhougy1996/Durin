# Material Compile Lifecycle and Derived Data Plan

Summary: Add cancelable generation-safe material compilation, last-known-good publication, derived artifacts, cooking, diagnostics, reload, and shutdown behavior.

Last reviewed: 2026-08-24

Status: Active
Completed:

## Current Status

This plan is selected and prepared, but production implementation is blocked
until Material System roadmap milestone 5, Material Program and Compiler
Foundation, passes its exit gate. M5 must provide the immutable authored-program
snapshot, normalized IR, deterministic program identity, synchronous compiler
entry point and result, renderer binding seam, fixed-schema transition policy,
and measured compile/artifact baseline consumed here.

The existing RenderCore shader compile service already owns synchronous Slang
compilation, reflection, dependency manifests, content-addressed shader
artifacts, in-process single-flight requests, bounded output retention, reload,
and transactional renderer resource refresh. The Core task system already owns
bounded background execution, typed results, cancellation, owner scopes,
GameThread-deferred continuations, task attribution, and shutdown. This plan
adds a material-asset lifecycle over those foundations; it does not replace
either subsystem.

No implementation checklist is complete. Stage 0 may collect the eventual M5
handoff, but its acceptance gate cannot pass before M5 completion evidence is
available.

## Goal

Compile material programs without blocking interactive editor work, publish
only the latest valid generation, preserve a last-known-good renderable result
through pending or failed edits, store and cook deterministic target artifacts,
and make cancellation, diagnostics, reload, recovery, and shutdown bounded and
observable.

## Scope

- Per-material authored revision, request generation, compile status, compiled
  identity, last-known-good result, staleness, and diagnostic state.
- Immutable value-owned compile snapshots produced on GameThread from the M5
  material-program contract.
- Bounded, cancelable Worker execution through the Core task system, including
  same-identity single-flight reuse and latest-generation publication rules.
- GameThread result admission followed by existing render-command and material-
  proxy publication; no Worker or GameThread code creates RHI resources.
- Deterministic material-level derived identity and any material-specific
  artifact not already owned by RenderCore's shader cache.
- Target-platform Cook publication and cooked-runtime admission for compiled
  materials and material instances.
- Last-known-good, ErrorMaterial, cache miss/corruption, compile failure,
  cancellation, supersession, reload, device recovery, module unload, and
  engine shutdown behavior.
- Bounded persistent and aggregate diagnostics, task attribution, compile
  timing, cache outcome, artifact size, and queue/residency observations.
- Minimal MaterialEditor compile-status and diagnostic presentation needed to
  expose this lifecycle before the graph canvas lands.
- Focused unit, integration, rendered-output, Cook/load, stress, reload,
  recovery, and shutdown qualification.

## Non-Goals

- Defining or extending the material graph schema, node library, typed IR,
  generated source contract, or synchronous compiler; M5 owns them.
- Implementing the graph canvas, node and pin interaction, copy/paste,
  diagnostic navigation on a canvas, or graph-specific Undo/Redo; M7 owns them.
- Adding a dedicated material worker pool or a second generic shader compiler,
  shader cache, task scheduler, or render-resource reload framework.
- Persisting transient compile status, raw diagnostics, task identifiers,
  request generations, or last-known-good runtime owners as authored package
  state.
- Duplicating SPIR-V and reflection artifacts already owned by RenderCore's
  shader cache merely to give them a material-specific path.
- Runtime dynamic material instances, high-frequency parameter upload batching,
  uniform deduplication, descriptor virtualization, or bindless resources; M8
  remains profiling-gated.
- Texture streaming, general asset residency, custom render passes, arbitrary
  user shader source, compute materials, decals, post-process materials, or ray
  tracing.
- Silently cooking ErrorMaterial in place of a referenced material whose target
  compilation failed.

## Design Decisions and Invariants

### State and identity

- Authored revision, compile request generation, latest diagnostic result, and
  currently renderable compiled result are separate state. A single `Ready`
  flag is insufficient and must not be introduced as the authority.
- Every compile-affecting authored or dependency change advances a nonzero
  generation. Only the latest generation for the same live material owner may
  publish, even if an older request finishes successfully afterward.
- The material program identity is supplied by M5 and includes normalized IR,
  reachable dependency fingerprints, static material properties, compiler
  environment, target, and exact shader/pass contract. Dynamic parameter and
  resource values never enter that identity.
- Equal program identities may share one in-flight or retained immutable result.
  Asset-local generation and diagnostic state remain distinct, so sharing work
  never merges asset ownership or publication order.
- Process-local task IDs and generations are diagnostics and ordering tokens;
  they are never serialized or used as derived-data identity.

### Thread and ownership boundary

- GameThread snapshots all `DObject` and asset state into a bounded immutable,
  value-owned request before Worker submission.
- Worker code may validate, normalize already-snapshotted inputs, generate
  compiler inputs, invoke thread-safe compile services, and return value-owned
  artifacts and diagnostics. It never resolves, retains, or mutates `DObject`,
  editor model, render proxy, Renderer, RHI, or live asset-registry state.
- GameThread owns generation admission and asset-visible compile state. It
  rechecks owner lifetime, request generation, dependency revision, and target
  before accepting a result.
- Accepted immutable render data crosses through the existing material proxy
  and render-command boundary. RHI shaders and pipelines are created and
  replaced only by their existing rendering-thread owners.
- The material compile service uses a module-owned task operation group or an
  equivalently audited owner scope. Shutdown closes request admission, cancels
  or drains accepted work according to the selected lifecycle mode, empties
  result publication, and releases retained compiler/module storage before
  unload.

### Cancellation, coalescing, and ordering

- Requests are latest-wins per material owner. A new generation requests
  cancellation of obsolete queued or running work and makes stale publication
  impossible independently of whether cancellation completes promptly.
- Cancellation is cooperative. Long material-owned CPU phases check their token
  at bounded intervals and immediately before producing externally publishable
  output. A noninterruptible compiler call is bracketed by cancellation checks.
- Identical in-flight program identities use one single-flight record. The
  record retains value-owned inputs/results and weak or generation-qualified
  consumers, never material object pointers.
- A superseded successful result may populate content-addressed generic caches
  when publication is independently safe, but it must not change the obsolete
  material owner's state or render proxy.
- Debounce is optional UI policy selected only from measured M5/M6 workloads;
  it never replaces generation validation or bounded task admission.
- Scheduler admission or continuation rejection becomes an explicit material
  compile outcome and cannot strand a material indefinitely in `Pending`.

### Publication and failure

- A material keeps its last-known-good immutable compiled result while a newer
  request is pending, compiling, canceled, superseded, or failed.
- Successful latest-generation admission atomically replaces compiled identity,
  diagnostics, and the publishable material program. No partial shader set,
  pass set, reflection result, binding layout, or render representation becomes
  visible.
- A material without a last-known-good result uses the existing asset-
  independent ErrorMaterial terminal. Compile failure never recursively depends
  on compiling another asset.
- Authored dirty state and compiled readiness remain distinct. Saving an
  authored failure does not falsely mark it compiled, and a valid prior render
  result does not hide that the current authored revision failed.
- Diagnostics distinguish validation, IR/code generation, compiler,
  reflection, binding, cache, Cook, renderer resource, cancellation,
  supersession, admission, and shutdown outcomes. Each diagnostic identifies
  the asset, generation, stable program identity when available, dependency or
  M5 source location when available, severity, category, and whether a last-
  known-good result remains displayed.
- Per-request strings and records are bounded. Aggregate counters retain no
  unbounded task, asset, or terminal-request history.

### Derived data and cooking

- Authored packages store the M5 program, parameter declarations, static
  properties, and asset references. They do not store transient orchestration
  state or rebuildable editor diagnostics.
- RenderCore remains the sole owner of compiled Slang SPIR-V, reflection,
  dependency manifests, and their existing retention policy.
- A material-level artifact is introduced only for material-specific normalized
  or linked output needed to reconstruct the runtime program without authored-
  only state. Stage 0 locks its schema, path owner, size bound, checksum,
  compiler/target identity, and whether it is useful enough to persist.
- Derived files are content addressed, validated before publication, written
  through the repository's atomic byte-publication contract, and treated as
  disposable. Corrupt or incompatible editor artifacts are cache misses, not
  partially accepted results.
- Cook requires a complete latest authored revision compiled successfully for
  the requested target. A referenced failed, missing, stale, or incompatible
  program fails Cook with an asset-qualified diagnostic; Cook never silently
  substitutes ErrorMaterial.
- Cooked runtime loads only the versioned, bounded target artifact described by
  the cooked package/payload contract. Shipping does not depend on authored
  graph state, source Slang files, editor DDC, or live compilation.
- Material instances share their parent's compiled program unless an M5-defined
  static override selects another program identity. Dynamic overrides remain
  ordinary runtime uniform/resource data.

### Reload and recovery

- Shader reload invalidates demanded material-backed Renderer resource slots
  through the existing Renderer generation contract. Changed reload reuses
  valid identities or recompiles changed dependencies; force reload bypasses
  successful artifact reuse where the existing Shader Cache contract requires.
- Compile, reflection, binding, RHI shader, or PSO refresh failure preserves a
  complete last-known-good resource set when one exists.
- Device invalidation rebuilds RHI resources from retained validated program
  artifacts. It does not regenerate material IR or advance authored/request
  revision without a changed compile dependency.
- All geometry families and passes consuming one published material identity
  observe one complete generation; no family-local partial publication is
  permitted.

## Current Foundations and Gaps

### Foundations

- M5 is expected to provide a deterministic synchronous material compile
  vertical slice and immutable compiler input/result contracts.
- RenderCore `GetOrCompileShader()` already owns normalized macros, dependency
  validation, in-process single flight, Slang compilation/reflection, atomic
  artifact publication, bounded memory/disk retention, and corruption recovery.
- Renderer shader-backed resource slots already support generation-based reload,
  transactional last-known-good refresh, retry, device invalidation, and
  explicit diagnostics.
- Stable material render proxies already coalesce publications, reject stale
  local versions, lazily resolve inheritance, replay after render-command
  admission restarts, and preserve counted resource lifetime.
- The CPU task system provides typed shared and unique results, cooperative
  cancellation, continuations, module-owned operation groups, bounded queues,
  GameThread deferred work, attribution, and shutdown diagnostics.
- Asset packages, bulk payloads, Cook, atomic file publication, and strict
  compatibility validation have established repository contracts.
- MaterialEditor already has multi-document editing, Undo/Redo, live preview,
  source provenance labels, and an error/status presentation seam.

### Gaps this plan closes

- There is no material compile request owner, generation state machine, or
  immutable Worker request/result envelope.
- Shader compilation is demanded synchronously; material editing has no
  asynchronous orchestration, cancellation, supersession, or task attribution.
- Material assets do not retain separate authored revision, latest diagnostics,
  compiled readiness, or last-known-good program state.
- There is no material-specific derived/cooked artifact contract or target Cook
  admission gate.
- Shader reload and device recovery are not qualified for generated material
  programs across every consuming geometry family and pass.
- Editor UI cannot report pending, stale-preview, failed, canceled, cache, or
  Cook status for a compiled material.
- Shutdown and module unload do not yet audit material compile tasks, retained
  results, result mailboxes, or pending owner-thread publication.

## Implementation Stages

### Stage 0: Lock the M5 handoff and lifecycle contract

Dependencies: Material System roadmap M5 completion evidence.

- [ ] Record the exact immutable compile input, normalized program identity,
  synchronous compiler API/result, diagnostic source location, Renderer binding
  seam, and fixed-schema transition delivered by M5.
- [ ] Characterize cold/warm compile latency, generated input and artifact
  sizes, dependency counts, simultaneous material demand, render-resource
  creation cost, and synchronous failure categories on the selected build
  profile and representative hardware.
- [ ] Freeze the authored-revision/request-generation/compiled-result state
  machine, including every transition for success, failure, cancellation,
  supersession, admission rejection, deletion, reload, Cook, and shutdown.
- [ ] Freeze the immutable Worker request/result envelopes, byte/count/depth
  bounds, task attribution, cancellation checkpoints, and owner lifetime token.
- [ ] Decide whether a separate material-level DDC artifact is justified by M5
  output; if it is, lock its ownership, schema, identity, checksum, size limit,
  atomic publication, retention, corruption, and compatibility behavior without
  duplicating RenderCore shader artifacts.
- [ ] Lock the target Cook descriptor/payload, cooked-runtime admission policy,
  MaterialInstance sharing rules, and explicit failure behavior.
- [ ] Add characterization tests for existing synchronous material compile,
  shader reload, renderer fallback, proxy publication, and shutdown paths before
  changing orchestration.


#### Acceptance Gate

- M5 has passed its exit gate and exposes all inputs required by this plan.
- One non-conflicting state, ownership, ordering, failure, DDC, Cook, reload,
  and shutdown contract is recorded with measured baseline evidence.
- Tests can detect stale publication, loss of last-known-good state, partial
  Renderer publication, object access from Worker, and retained work at unload.

### Stage 1: Introduce material compile requests and synchronous publication

Dependencies: Stage 0.

- [ ] Add the bounded compile request/result types, stable program identity,
  request generation, result category, diagnostic records, and aggregate
  counters without adding asynchronous execution yet.
- [ ] Add a material compile owner/service with explicit startup, admission,
  result publication, asset deletion, and shutdown boundaries.
- [ ] Snapshot M5-authored state on GameThread and run the existing synchronous
  compiler through the new request path.
- [ ] Admit results only after rechecking owner lifetime, generation, target,
  authored revision, and dependency revision.
- [ ] Separate current authored state, latest request status, latest diagnostics,
  and last-known-good compiled result on the material asset/runtime facade.
- [ ] Atomically publish an accepted result through the existing material proxy
  and Renderer resource slot boundaries.
- [ ] Add focused unit tests for every state transition, generation wrap/invalid
  values, owner destruction, diagnostic bounds, and complete-or-no-op
  publication.

#### Acceptance Gate

- All production material compilation flows through one generation-checked
  request and publication contract while retaining synchronous behavior.
- Failed, canceled, rejected, deleted-owner, and stale-generation results cannot
  replace a valid renderable result.
- A material with no successful result selects ErrorMaterial deterministically;
  a material with a prior result preserves it and reports that preview/output is
  stale relative to authored state.

### Stage 2: Move compilation to bounded cancelable Worker execution

Dependencies: Stage 1.

- [ ] Bind the compile service to a module-owned task operation group and stable
  `MaterialCompile` task attribution categories.
- [ ] Submit immutable request snapshots through typed task results without
  capturing material objects, editor models, live registries, or Renderer/RHI
  owners.
- [ ] Add cooperative cancellation before and after each material-owned phase
  and around noninterruptible compiler calls.
- [ ] Implement latest-wins supersession per material owner and same-identity
  single-flight work sharing without merging asset-local state.
- [ ] Add a reliable GameThread publication path that cannot strand terminal
  results when the bounded deferred queue rejects admission or shuts down.
- [ ] Bound concurrent and queued compiles, retained request/result bytes,
  consumer lists, diagnostic records, and completed-result residency using the
  Stage 0 measurements.
- [ ] Expose pending, queued, running, completed, failed, canceled, superseded,
  rejected, cache, byte, latency, and task-attribution diagnostics.
- [ ] Test rapid edit waves, shared identities, cancellation races, scheduler
  rejection, stale completion, asset deletion, module close, and engine task-
  system shutdown.

#### Acceptance Gate

- Material compilation no longer performs its measured expensive phases on
  GameThread or RenderThread.
- Only the latest live generation can publish under adversarial completion
  ordering, cancellation, deletion, and shutdown.
- Queue, callable, request, result, diagnostic, and retained-storage bounds are
  enforced and reconcile to zero after a drained or canceled owner shutdown.

### Stage 3: Complete last-known-good Renderer publication and recovery

Dependencies: Stage 2.

- [ ] Make the accepted material program and complete pass/shader resource set
  one transactional Renderer publication.
- [ ] Preserve the prior complete set across material compile, reflection,
  binding, RHI shader, and PSO refresh failure; use ErrorMaterial only when no
  prior valid result exists.
- [ ] Integrate material program identities with changed and force shader reload
  without eager recompilation of unused materials.
- [ ] Rebuild RHI resources after device invalidation from retained validated
  artifacts without mutating authored or compile generation state.
- [ ] Qualify identical program generation visibility across StaticMesh,
  SkeletalMesh, Terrain, preview, thumbnail, forward, GBuffer, opaque shadow,
  and masked shadow consumers applicable to the M5 program.
- [ ] Add Renderer diagnostics that distinguish unavailable program, stale
  last-known-good, compile failure, binding rejection, resource creation
  failure, and device recovery.

#### Acceptance Gate

- No production consumer observes a partially refreshed program or a generation
  inconsistent with another geometry family or pass.
- Reload and device recovery preserve valid output on failure and replace it
  atomically on success.
- Existing fixed-schema/default/error behavior remains qualified through the M5
  transition policy.

### Stage 4: Add material derived-data storage and retention

Dependencies: Stages 2 and 3; Stage 0 decision that material-specific persisted
output is required. If Stage 0 proves no separate artifact is useful, this
stage records that disposition, adds identity/reuse tests, and does not create
an empty cache layer.

- [ ] Implement the selected content-addressed material artifact schema and
  target/compiler/program identity without copying RenderCore-owned shader
  binary or reflection storage.
- [ ] Validate header/version, bounds, checksums, target, compiler environment,
  program identity, dependencies, and complete payload before publication.
- [ ] Publish through Core atomic file APIs and keep cache paths beneath the
  owning DerivedDataCache domain.
- [ ] Treat missing, corrupt, truncated, stale, or incompatible entries as
  bounded cache misses in editor/development environments and repair them from
  authored inputs.
- [ ] Add bounded memory and disk retention with protected-current publication,
  deterministic eviction, symlink/path confinement, and failure diagnostics.
- [ ] Measure and expose warm/miss latency, bytes read/written/retained,
  validation failures, repairs, and eviction.

#### Acceptance Gate

- Equivalent program identities reuse one validated artifact; any identity or
  reachable dependency change selects a different result.
- Corruption and concurrent publication cannot expose partial output or escape
  the selected cache root.
- Retention remains within the Stage 0 budget, and a successful current artifact
  is not immediately evicted.

### Stage 5: Integrate Cook and cooked-runtime loading

Dependencies: Stage 4 or its explicit no-separate-DDC disposition.

- [ ] Add material compile roots and target requests to the canonical Cook
  dependency walk without introducing DevTool-owned material policy.
- [ ] Require the latest authored material revision and every reachable material
  program dependency to have a complete successful target result.
- [ ] Publish the versioned cooked descriptor/payload selected in Stage 0 and
  strip graph/editor-only state according to the M5 asset contract.
- [ ] Keep instances sharing the parent program unless an accepted static
  override selects a distinct compiled identity; cook only dynamic overrides as
  ordinary runtime values.
- [ ] Reject missing, stale, failed, corrupt, wrong-target, wrong-compiler, and
  incompatible artifacts with asset-qualified diagnostics; never silently cook
  ErrorMaterial as the authored result.
- [ ] Load cooked materials without editor DDC, authored-only graph state,
  source shader availability, or live compilation in Shipping.
- [ ] Add deterministic round-trip, multi-target, dependency invalidation,
  package-only/DBLK ownership, missing payload, corruption, and Shipping
  admission tests.

#### Acceptance Gate

- Cook succeeds only from a complete validated latest target result and fails
  atomically with actionable asset/dependency evidence otherwise.
- Cooked runtime renders the same program identity and material output as the
  editor-qualified source without consulting editor-only inputs.
- Material and instance package references, GC reachability, and strict
  compatibility behavior remain correct.

### Stage 6: Expose editor status and close lifecycle boundaries

Dependencies: Stages 2 through 5.

- [ ] Show bounded compile state, authored/compiled freshness, cache outcome,
  last-known-good usage, target, duration, and diagnostics in MaterialEditor
  without implementing graph-canvas interaction.
- [ ] Provide explicit recompile/cancel/retry actions through the compile owner;
  prevent UI code from owning task handles or Renderer resources directly.
- [ ] Synchronize save, discard, Undo/Redo, document close, asset deletion, and
  material/instance dependency changes with request generation and publication.
- [ ] Close compile admission before MaterialEditor/Engine/compiler provider
  teardown, cancel or drain by the selected mode, empty the reliable result
  path, release terminal handles and single-flight records, then unload modules.
- [ ] Add editor multi-document, shared-identity, close/delete, failed-edit/
  repair, save/reload, Cook feedback, shutdown, and restart coverage.

#### Acceptance Gate

- Users can distinguish compiling, ready, stale-last-known-good, failed,
  canceled, superseded, rejected, and cooked-target states without consulting
  logs.
- Editor close, asset deletion, module unload, and engine shutdown leave no
  accepted work, retained results, owner callbacks, proxy updates, or compiler
  storage beyond their declared lifetime.

### Stage 7: Qualify the complete compiled-material lifecycle

Dependencies: Stages 1 through 6.

- [ ] Run focused state/identity/compiler/cache/Cook/editor tests in the selected
  Agent Build Profile.
- [ ] Run StaticMesh, SkeletalMesh, Terrain, preview, thumbnail, forward,
  GBuffer, shadow, reload, device-recovery, and Vulkan rendered-output tests
  selected by the testing guide.
- [ ] Run cold/warm, rapid-edit, same-identity, cancellation, capacity,
  corruption, multi-target Cook, dependency fan-out, module unload, and engine
  shutdown stress workloads against the plan-owned budgets.
- [ ] Run the owning module targets, required aggregate tests, complete build,
  and editor runtime smoke selected by repository guidance.
- [ ] Move lasting state, threading, artifact, Cook, reload, fallback, and
  diagnostics contracts into their authoritative Runtime and Editor documents.
- [ ] Record exact validation evidence, environment-dependent measurements,
  limitations, final handoff, and follow-up disposition before completing this
  plan and updating the Material System roadmap.

#### Acceptance Gate

- Every required validation-matrix row passes with recorded evidence.
- The complete lifecycle remains bounded and generation-correct from authored
  edit through Worker compile, GameThread admission, RenderThread publication,
  DDC, Cook, cooked load, reload, recovery, and shutdown.
- Lasting contracts no longer depend on this active plan as their sole authority.

## Validation Matrix

| Area | Required evidence |
| --- | --- |
| State and ordering | Exhaustive state-transition tests; older, canceled, rejected, deleted-owner, wrong-target, and wrong-dependency generations cannot publish |
| Thread ownership | Worker inputs/results are value-owned; no Worker `DObject` resolution or Renderer/RHI access; GameThread and RenderThread assertions cover publication |
| Cancellation and bounds | Rapid supersession, queued/running cancellation, scheduler/deferred rejection, capacity, retained bytes, single-flight consumer bounds, and shutdown reconciliation |
| Compiler and diagnostics | Validation/codegen/compiler/reflection/binding categories, bounded source locations and strings, cache outcome, timing, size, and last-known-good indication |
| Renderer | Atomic program/pass-set publication; StaticMesh/SkeletalMesh/Terrain and forward/GBuffer/shadow parity; preview/thumbnail; ErrorMaterial and last-known-good output |
| Derived data | Deterministic identity, warm/miss, dependency invalidation, atomic concurrent publication, corruption repair, retention, path confinement, and compatibility rejection |
| Cook and runtime | Latest-revision gate, target isolation, dependency closure, material-instance sharing, deterministic payload round trip, missing/corrupt artifact rejection, and Shipping without live compile |
| Reload and recovery | Changed/force reload, compile/reflection/binding/RHI/PSO failure, last-known-good retention, device invalidation, retry, and complete replacement |
| Editor workflow | Pending/status/diagnostics, continuous edits, Undo/Redo, save/reload, multi-document sharing, retry/cancel, close/delete, Cook feedback, and failure recovery |
| Lifecycle | Provider close, module unload, editor shutdown, engine drain/cancel, render-command admission restart, and zero retained work/storage audits |
| Qualification | Plan-owned latency, queue, memory, artifact, retention, and stress budgets derived from Stage 0; focused, aggregate, full build, and editor smoke evidence |

Use the repository [build and run](../Agents/BuildAndRun.md) and
[testing](../Agents/Testing.md) workflows to select profiles and commands.
Stage handoffs record exact targets, filters, timings, hardware, cache state,
and any environment-dependent qualification values.

## Definition of Done

- M5's synchronous compiled-material path is fully mediated by the bounded
  generation-safe lifecycle without changing its authored/IR ownership.
- Expensive compilation work executes off GameThread and RenderThread, and only
  the latest live generation can become visible.
- Pending or failed edits retain one complete last-known-good program; absence
  of any valid program selects ErrorMaterial with an actionable diagnostic.
- Material-specific derived output, if justified, is deterministic, bounded,
  atomic, disposable, non-duplicative, and compatible with RenderCore shader
  cache ownership.
- Cook admits only complete latest target results, and cooked runtime requires
  neither editor DDC nor live compilation.
- Reload, RHI/PSO recovery, asset deletion, module unload, and engine shutdown
  preserve ownership and leave no partial resource set or retained task state.
- MaterialEditor exposes the lifecycle states and diagnostics needed before the
  M7 graph canvas.
- All validation-matrix evidence and plan-owned budgets pass, lasting contracts
  are updated, and the Material System roadmap records completion.

## Deferred Follow-ups

- Graph canvas, node/pin editing, compiler diagnostic navigation within the
  canvas, copy/paste, and graph-specific Undo/Redo belong to M7.
- Runtime-only dynamic instances and measured high-frequency update, allocation,
  uniform/resource reuse, and descriptor policy belong to M8.
- Additional material domains, custom passes, arbitrary source snippets,
  bindless resources, compute materials, decals, post-process materials, and ray
  tracing require separately gated roadmap work.
- Distributed or remote shader compilation requires measured local saturation,
  security and reproducibility boundaries, and its own plan.

## Related Documentation

- [Material System Roadmap](../Roadmaps/MaterialSystem.md)
- [Material System](../Runtime/Rendering/MaterialSystem.md)
- [Shader Cache](../Runtime/Rendering/ShaderCache.md)
- [Shader Parameters](../Runtime/Rendering/ShaderParameters.md)
- [Renderer Resource Recovery](../Runtime/Rendering/RendererResourceRecovery.md)
- [Render Resource Lifecycle](../Runtime/Rendering/RenderResourceLifecycle.md)
- [CPU Task System](../Runtime/Core/TaskSystem.md)
- [Runtime Lifecycle](../Runtime/Core/RuntimeLifecycle.md)
- [Asset Packages](../Runtime/Assets/AssetPackages.md)
- [Asset Data Lifecycle](../Runtime/Assets/AssetDataLifecycle.md)
- [File I/O](../Runtime/Core/FileIO.md)
- [Asset Thumbnails](../Editor/Architecture/AssetThumbnails.md)
- [Reflected Property Editing](../Editor/Architecture/ReflectedPropertyEditing.md)

## Related Code

- `Engine/Source/Runtime/Engine/Public/Materials`
- `Engine/Source/Runtime/Engine/Private/Materials`
- `Engine/Source/Runtime/RenderCore/Public/Shader`
- `Engine/Source/Runtime/RenderCore/Private/Shader`
- `Engine/Source/Runtime/Renderer/Private/Renderers`
- `Engine/Source/Runtime/Core/Public/Threading/Task.h`
- `Engine/Source/Editor/MaterialEditor`
- `Engine/Tests/Native/EngineTests/Private/Materials`
