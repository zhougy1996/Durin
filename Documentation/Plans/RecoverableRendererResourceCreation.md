# Recoverable Renderer Resource Creation Plan

Summary: Replace permanent one-shot renderer resource failures with transactional creation, generation-scoped retry, retained diagnostics, and safe last-known-good refresh.

Last reviewed: 2026-07-30

Status: Active
Completed: Stages 0-3

## Current Status

Implementation baseline: `2081dd81` (`feat(material): bind scene proxies to
material proxies`).

Stage 0 locked the common state contract and test boundary. The public,
RHI-independent primitive lives in RenderCore because Renderer, Renderer editor
assistance, and Texture Editor require the same payload/attempt/failure
semantics. The slot owns a complete payload candidate, full and selected
generations, structured failure, latest attempt, and diagnostic transition
state. Availability is derived as `Uninitialized`, `Creating`, `Ready`,
`Refreshing`, `Failed`, or `StaleReady`; reentrant resolution returns the
already valid fallback, if any, without invoking the factory again.

All previously inventoried one-shot flags are now migrated. Static Mesh, Sky
Box, Texture Cube thumbnail, Post Process, shared fullscreen geometry, Texture
Editor preview, and the four demand-driven editor-assistance features use the
common slot. Assistance pipelines remain independently keyed while their
module-private tri-state and string-only diagnostics have been removed.

The post-planning relevant diff is the demand-driven assistance extraction and
the Material render-proxy identity work through the implementation baseline.
Those changes preserve the plan's ownership boundary: the assistance plan owns
feature demand and pipeline keys, Material System owns shader-map/pipeline
identity, and this plan owns attempt, failure, retry, refresh, and invalidation
semantics.

The initial implementation working set is:

- `Engine/Source/Runtime/RenderCore/Public/RenderResourceCreation.h`
- `Engine/Tests/Native/RenderCoreTests/Private/RenderResourceCreationTests.cpp`
- `Engine/Tests/Native/RenderCoreTests/CMakeLists.txt`
- `Engine/Source/Runtime/Renderer/Private/RendererModule.cpp`
- `Engine/Source/Runtime/Renderer/Private/RendererEditorAssistance.h`
- `Engine/Source/Runtime/Renderer/Private/RendererEditorAssistanceRenderer.cpp`
- `Engine/Source/Runtime/Renderer/Private/RendererFullscreenGeometry.cpp`
- `Engine/Source/Editor/TextureEditor/Private/Widgets/TexturePreview.cpp`
- the narrow invalidation interface and focused owner tests selected in later
  stages

The current release path is render-thread ordered: Renderer shutdown enqueues
one `ReleaseRendererResources` command, resets fixed and keyed state, releases
editor assistance and fullscreen geometry, then flushes rendering commands.
Texture Editor enqueues its own shared-preview reset and flushes before module
shutdown. No supported device-recovery event exists: `RHIInit()`/`RHIExit()`
cover whole-process RHI lifetime, while Vulkan `Init()`/`Shutdown()` do not
publish device-loss recovery. This plan therefore adds and tests an internal
Renderer device-invalidation request without connecting it to a fictitious
event.

The exact development commands are `renderer.reload-shaders changed`,
`renderer.reload-shaders all`, and `renderer.retry-resources`. Renderer module
startup owns registration. Shutdown first stops admission and unregisters the
handles, then enqueues the existing ordered release; callbacks enqueue one
render command and never mutate slots from the console thread. The narrow
Renderer invalidation request accepts shader-changed, shader-all, device, or
manual-retry causes; the device cause remains an internal API until an RHI
recovery lifecycle exists.

Keyed Static Mesh and assistance state retains vectors. Lookup and construction
are synchronous on the render thread; callers receive payload ownership or RHI
references, never slot pointers that survive another insertion. Factory
injection occurs at the common result boundary and can deterministically fail
shader, declaration, sampler, buffer, texture, or individual PSO steps without
a driver failure. The Stage 0 red test run built successfully and ran six
focused cases. Two passed to characterize transaction rollback and reentrancy
suppression; four failed on relevant-generation retry, last-known-good refresh,
device invalidation, and recovery diagnostics, matching the one-shot
scaffold's known gaps.

Stage 1 implements the public slot contract in the same header. Resolution
first applies destructive device-generation invalidation, suppresses an
unchanged failed generation using the failure's retry mask, exposes
`Creating`/`Refreshing` only during the synchronous factory call, and swaps a
complete successful candidate into the live optional payload. Failed shader or
manual refresh retains the prior payload as `StaleReady`; failed initial or
post-device construction is `Failed`. The slot retains the full error and its
fingerprint, reports one failure per attempted transition, reports recovery
only after a reported failure, and resets payload, attempt, failure,
fingerprint, and diagnostic state together. Generation advancement asserts
before `uint64` wrap.

Stage 1 handoff: baseline commit `2a07896b`; working set is the public
`RenderResourceCreation.h` primitive, its `RenderContractTests` cases, test
target metadata, and this plan. The key symbols are
`FRenderResourceGeneration`, `FRenderResourceCreateError`,
`TRenderResourceCreateResult`, and `TRenderResourceCreationSlot`. There are no
open primitive-design questions; owner-specific factory error mapping and
invalidation wiring remain in Stages 2-4. The focused run passed 9/9 cases and
the complete `RenderContractTests` target passed 32/32 cases through
DurinDevTool.

Stage 2 handoff: baseline commit `bc493527`; the working set adds
`RendererModule.cpp`, `RendererResourceSlotCache.h`, the focused keyed-cache
tests, and EngineTests target metadata. Static Mesh base resources now commit
one device-dependent declaration/sampler aggregate. Shader-map identities own
shader-dependent slots whose candidates bind both typed shaders before commit.
Pipeline identities own shader-and-device-dependent slots whose candidates
create both solid and wire PSOs before commit and retain their owning shader
map. Pipeline shader generation is taken from the committed shader-map payload,
so a failed shader refresh continues using the complete old shader-map/pipeline
pair rather than rebuilding a new-generation pipeline from stale shaders.

The keyed containers remain insertion-ordered vectors behind
`TRendererResourceSlotCache`; empty entries are explicit failed/uninitialized
slots, never incomplete payloads. The common failure callback now carries the
owned prior error on recovery so recovery diagnostics retain context and
identity. There are no open Stage 2 questions. Validation passed 4/4 focused
keyed-cache cases, 9/9 common slot cases, 26/26 `EditorRenderingTests`, and
35/35 `FMaterialTests.*` cases, including the Vulkan-backed rendered thumbnail
path.

Stage 3 handoff: baseline commit `53c870d6`; the working set adds
`RendererEditorAssistance.h`, `RendererEditorAssistanceRenderer.cpp`,
`RendererFullscreenGeometry.h/.cpp`, Texture Editor's `TexturePreview.cpp`,
and the assistance owner tests, while continuing the fixed-resource work in
`RendererModule.cpp`. Sky Box, Texture Cube thumbnail, and Post Process now
build complete shader/RHI/pipeline aggregates before slot commit. Shared
fullscreen geometry owns one device-dependent buffer aggregate. Texture Editor
preview owns its slot and generation in Texture Editor and advances manual
retry only when a later preview demand observes a retained failure.

Gizmo, Overlay Line, Overlay Icon, and Editor Grid keep the demand and keyed
pipeline decomposition established by the assistance plan, but their base
payloads and pipeline entries now use `TRenderResourceCreationSlot`. Dynamic
per-view line/icon buffers remain outside the fixed base payloads. Shader
refresh can retain a complete last-known-good payload; device invalidation
remains destructive. Renderer and Texture Editor shutdown explicitly reset
slots inside their existing render-thread-ordered release paths before clearing
owner mirrors and dynamic caches.

The private `FGenerationScopedAttempt`, its three-state availability enum, and
its public test adapter functions are removed. A grouped failure-injection test
covers every migrated fixed feature and Texture Editor preview identity,
proving one injected failure leaves all other identities ready and that a
manual retry recovers only the failed slot. There are no open Stage 3 design
questions; command registration and generation invalidation wiring remain in
Stage 4. Validation passed 13/13 focused assistance cases, all 26/26
`EditorRenderingTests`, all 32/32 `RenderContractTests`, and 35/35
`FMaterialTests.*` cases including the Vulkan-backed rendered-thumbnail path;
the TextureEditor module rebuilt and linked its modified preview source.

The lower shader compile service does not cache failed compiler output.
Dependency fingerprints already make a changed shader source produce a
different variant key, and `bForceRecompile` can bypass successful memory and
disk outputs. The recovery gap is therefore in the renderer-level ownership,
attempt state, and invalidation path rather than the completed shader-cache
integrity work.

The current RHI creation API returns nullable references and relies on
backend-specific logging for native failure detail. There is no current
shader-reload command, source-file watcher, or supported Vulkan device-loss
recovery path. This plan adds a narrow renderer-facing failure record and
invalidation boundary without turning RHI creation into a new cross-backend
error API or claiming device recovery before it exists.

The initial working set is:

- `Engine/Source/Runtime/Renderer/Private/RendererModule.cpp`
- the Renderer-private resource-state files selected in Stage 0
- `Engine/Source/Runtime/RenderCore/Public/Shader/Shader.h`
- `Engine/Source/Runtime/RenderCore/Private/Shader/Shader.cpp`
- `Engine/Source/Runtime/RenderCore/Private/Shader/ShaderCompileService.cpp`
- `Engine/Source/Editor/TextureEditor/Private/Widgets/TexturePreview.cpp`
- focused tests under `Engine/Tests/Native/RenderCoreTests/` and
  `Engine/Tests/Native/EngineTests/`
- `Documentation/Runtime/Rendering/ViewportRendering.md`

Expand this working set only when Stage 0 identifies a direct console-command,
module-lifecycle, RHI invalidation, editor-assistance extraction, or test-build
dependency. The active Demand-Driven Editor Assistance Renderer plan owns
feature-demand and pipeline-key decomposition; this plan owns the common
attempt, failure, retry, refresh, and invalidation semantics used by those
feature states.

## Goal

Make renderer resource creation failures recoverable without introducing
per-frame retry storms, partial global state, or loss of a previously working
resource:

- A failed first attempt records an actionable error and remains suppressed
  only for the same relevant resource generation.
- A shader-source reload, supported RHI reinitialization, or explicit manual
  retry advances the relevant generation and permits another attempt.
- Shader-backed resources refresh transactionally and keep their last-known-good
  payload if the replacement fails.
- Device-invalidated RHI resources are cleared and never treated as a valid
  fallback after the device generation changes.
- Keyed shader-map and pipeline caches distinguish a failed slot from a ready
  payload and never interpret an empty cache entry as a permanent result.
- Diagnostics identify the resource, cache identity or pipeline key, failure
  category, attempt generation, and retained-fallback state without repeating
  every frame.

## Scope

- Define one small lazy-attempt abstraction for synchronous render-thread
  resource construction.
- Represent availability and the latest construction attempt independently so
  a ready last-known-good payload can survive a failed refresh.
- Construct shader maps, shader references, vertex declarations, samplers,
  buffers, textures, and PSOs into local candidate aggregates before commit.
- Replace the Renderer `bCreateAttempted` flags and the Texture Editor preview
  flag with explicit attempt state.
- Convert static-mesh base resources, shader-map entries, and pipeline entries
  to stateful slots with generation-aware lookup.
- Add shader, device, and manual invalidation generations with dependency masks
  per slot.
- Add an explicit development command for changed/all shader reload and a
  command or API for retrying failed renderer resources.
- Preserve old shader-backed resources during a failed refresh when their RHI
  device remains valid.
- Add focused failure-injection tests, integration coverage, full-build
  validation, and a real editor shader-repair smoke.
- Reconcile the failure policy in the Demand-Driven Editor Assistance Renderer
  plan as its feature resources move to the common state model.
- Document the lasting renderer resource creation and recovery contract after
  implementation.

## Non-Goals

- Retrying shader compilation, buffer creation, or PSO creation every frame.
- Adding timer-based or exponential-backoff polling.
- Adding a shader-source filesystem watcher in this plan.
- Implementing asynchronous shader compilation, asynchronous PSO creation,
  cancellation, or background resource publication.
- Implementing Vulkan device-loss detection or recovery where no supported
  engine lifecycle currently exists.
- Replacing the existing shader derived-data cache, dependency fingerprints,
  weak shader-map resource cache, or compiler request coalescing.
- Introducing an engine-wide serialized PSO cache, PSO prewarming system,
  render graph, or fallback-material framework.
- Redesigning every nullable RHI factory to return a typed backend error.
- Changing render output, material appearance, editor-assistance ordering,
  viewport composition, or thumbnail appearance.
- Moving ordinary asset-owned `FRenderResource` lifetime into the new lazy
  slot; this plan addresses module/static renderer caches and preview state.

## Design Decisions and Invariants

### Availability and attempt state are independent

- The implementation must not encode all behavior in one `bAttempted` or
  `bReady` flag.
- A slot stores an optional current payload, the latest attempt state, the
  generation in which the payload was built, the latest attempted generation,
  and an optional failure record.
- Observable states are derived as follows:
  - `Uninitialized`: no payload and no attempt for the current generation.
  - `Creating`: initial construction is in progress.
  - `Ready`: a payload exists for the current generation.
  - `Refreshing`: an older valid payload exists while a replacement is built.
  - `Failed`: no payload exists and the current generation failed.
  - `StaleReady`: an older valid payload exists and refresh for the current
    shader or manual generation failed.
- The attempt transition is synchronous and render-thread-owned in this plan.
  `Creating` and `Refreshing` reject reentrant lookup; they do not introduce a
  mutex, condition variable, or waiting render command.
- A shader refresh failure records `StaleReady` semantics and continues using
  the last-known-good payload. A device-generation change clears all dependent
  payloads before retry because old RHI objects cannot be assumed valid.

### Transactional construction

- A factory builds one complete candidate aggregate without mutating the live
  slot.
- Shader-map ownership must already be present in the candidate before typed
  shader references are formed, and the aggregate commit must preserve that
  ownership relationship.
- RHI references created before a later candidate step fails are released by
  ordinary reference ownership on the render thread; the live slot remains
  unchanged.
- Readiness is validated once at the candidate boundary. Call sites do not
  infer partial readiness from a subset of non-null members.
- Successful construction moves the candidate into the live slot in one
  commit, clears the relevant failure, records the built generation, and only
  then exposes a payload pointer.
- Keyed caches may insert a state slot before attempting construction, but they
  must never insert or expose an incomplete payload entry.

### Failure representation and retry policy

- A narrow `FRenderResourceCreateError`-style value records:
  - category: shader options, shader compile, shader binding, RHI resource,
    graphics pipeline, or invalid configuration;
  - contextual resource or pipeline name and keyed identity;
  - human-readable message;
  - retry dependency mask;
  - relevant attempted generation;
  - whether a last-known-good payload remains available.
- Shader compiler diagnostics are retained verbatim as owned text. RHI failures
  use contextual engine diagnostics plus the backend log; broad Vulkan/RHI
  typed-error propagation is outside this plan.
- A failed slot is sticky only while every generation selected by its retry
  dependency mask remains unchanged.
- Logging occurs on a new failed transition or a changed failure fingerprint,
  not on every lookup. A successful recovery logs one concise recovery message
  when the earlier failure was user-visible.
- Unrecoverable configuration errors use a manual/configuration generation and
  are not automatically retried by an unrelated shader or device event.

### Generation ownership and invalidation

- Renderer resource generation contains independent monotonically increasing
  shader, device, and manual counters. Zero is a valid initial generation; wrap
  behavior must be asserted or handled rather than silently aliasing a live
  generation.
- Each slot declares which counters affect its payload:
  - shader maps depend on shader generation;
  - shader-backed PSOs depend on shader and device generations;
  - vertex declarations, samplers, buffers, textures, and scene targets depend
    on device generation;
  - explicit retry may additionally advance the manual generation selected by
    failed slots.
- Invalidation is ordered with rendering work on the render thread. A
  game-thread or console request enqueues one command; views before the command
  use the old generation, and views after it observe the new generation.
- Shader invalidation marks shader-backed payloads stale and rebuilds them
  lazily on next demand. It does not eagerly compile every registered material
  identity or assistance permutation.
- Device invalidation clears all dependent payloads, keyed entries, dynamic
  capacities, and failure-log state before the new generation can render.
- The plan adds a device-invalidation hook and tests its semantics. Connecting
  that hook to real Vulkan device recovery is deferred until the engine owns a
  supported recovery lifecycle.
- Module shutdown unregisters development commands, stops new invalidation
  requests, and uses the existing ordered render-thread release path. Shutdown
  never schedules a retry.

### Development reload behavior

- Stage 0 selects final command spelling using the existing console registry;
  the intended surface is equivalent to:
  - `renderer.reload-shaders changed`
  - `renderer.reload-shaders all`
  - `renderer.retry-resources`
- `changed` advances shader generation and relies on existing dependency
  fingerprints and variant keys when a stale slot is next demanded.
- `all` advances shader generation and requests forced compilation for the next
  demanded shader-backed candidate; it does not destroy the last-known-good
  payload before compilation succeeds.
- `retry-resources` advances manual generation for explicitly retryable failed
  slots. It does not bypass device validity or reclassify a configuration error
  as safe.
- Command results report that invalidation was queued and that reconstruction
  remains demand-driven. A separate status surface is added only if Stage 0
  shows stored failure diagnostics cannot otherwise be inspected effectively.
- Shipping builds may omit the command registration while retaining the
  internal generation and reset contract.

### Static-mesh cache semantics

- Shader-map identity and pipeline identity remain the cache keys selected by
  the Material System plan.
- A shader-map slot owns either no map or one complete map with its typed shader
  references. Failure of one identity cannot poison a different identity.
- A pipeline slot owns either no pipeline aggregate or the complete required
  solid/wire aggregate until the Demand-Driven Editor Assistance or Material
  System work intentionally decomposes that key further.
- If either PSO in the current aggregate contract fails, the candidate is not
  committed. A later relevant generation may retry the same identity.
- When a shader map refreshes, dependent pipeline slots observe the shader
  generation change and rebuild against the refreshed map. An old pipeline
  retains its owning old shader map until its replacement commits.
- Container changes are driven by key stability and testability, not by a goal
  to introduce a general PSO cache. Stage 0 must record whether the existing
  vectors remain safe for synchronous immediate use or node-stable storage is
  required by the selected test seam.

### Coordination with editor assistance and previews

- The Demand-Driven Editor Assistance Renderer plan continues to own when Grid,
  Gizmo, Line, and Icon resources are requested and how their pipeline keys are
  decomposed.
- This plan owns how each requested base-resource or pipeline slot attempts,
  fails, retries, refreshes, logs, and resets.
- The assistance refactor must use the common semantics rather than reintroduce
  a separate tri-state that treats failure as permanent for the session.
- Texture Editor preview uses the same semantics for initial construction and
  shader/manual invalidation, but remains owned and released by Texture Editor.
- MonaImGui has no current `bCreateAttempted` cache and is excluded unless Stage
  0 proves an equivalent persistent failed-entry path.

### Unreal Engine comparison boundary

- UE's `FRenderResource` and `BeginInitResource`/`BeginReleaseResource`
  reinforce render-thread ownership and ordered producer-to-render-thread
  lifecycle. Durin retains its existing `FRenderResource` contract rather than
  importing UE class hierarchy.
- UE's shader development workflow uses an explicit
  `recompileshaders changed` operation and development-mode retry rather than
  treating a failed live-session compile as an eternal cache result. Durin
  adopts an explicit changed/all reload boundary without importing UE's
  material compiler or editor UI.
- UE's PSO precaching behavior distinguishes compiling, ready, missed, and
  fallback/skip behavior. Durin uses the same availability principle for
  diagnostics and safe draw skipping, but async precaching, bundled PSO caches,
  and driver-cache policy remain out of scope.
- Durin's existing shader dependency keys, synchronous compiler, render
  command model, RHI references, and module shutdown contract remain
  authoritative whenever UE behavior differs.

## Current Foundations and Gaps

### Existing foundations

- `FShaderCompileService` caches only successful compiler outputs.
- Shader dependency metadata and variant keys already detect changed root and
  transitive source content.
- `FShaderCompileOptions::bForceRecompile` already bypasses successful output
  reuse.
- `FShaderMapBase::InitializeFromShaderTypes()` returns an owned compiler or
  binding diagnostic and resets its incomplete internal state.
- RHI resources use counted references, allowing local candidate rollback
  without manual deletion.
- Renderer resource mutation and release already occur through render-thread
  work.
- Static-mesh shader and pipeline identities already separate material static
  state from dynamic parameters.
- The console command registry provides a bounded development-facing trigger.
- Existing RenderCore lifecycle, shader compile, material rendering,
  assistance, thumbnail, and Vulkan tests provide nearby test fixtures.

### Gaps to close

- Remaining fixed-resource `bCreateAttempted` flags conflate attempted, ready,
  and failed states.
- Remaining fixed Renderer feature factories mutate global state before
  complete success.
- Editor assistance still uses its module-private string-only attempt adapter
  rather than the common structured slot.
- Error messages are logged and discarded rather than retained with the failed
  slot.
- There is no generation that makes one failed slot eligible after relevant
  external state changes.
- There is no explicit shader reload or resource retry command.
- Shader refresh has no last-known-good replacement transaction.
- Device reset semantics are stated only as whole-state release; no reusable
  invalidation hook exists for future recovery.
- Existing tests do not inject a first failure followed by recovery for the
  same renderer cache key.
- The active editor-assistance plan currently describes permanent sticky
  failures and must be coordinated with this generation-scoped policy.

## Implementation Stages

### Stage 0: Lock the state owner, invalidation surface, and test seams

- [x] Record the implementation baseline, relevant diff, exact
  `bCreateAttempted` and incomplete-entry inventory, working set, and current
  render-thread release path.
- [x] Decide whether the reusable slot lives in a narrow public RenderCore
  header or remains module-private with a second small Texture Editor adapter.
  Select the public helper only if two owners require identical semantics and
  the type can remain independent of Renderer identities and RHI backend types.
- [x] Define the exact payload/attempt/failure data model, legal transitions,
  generation comparison, wrap handling, and last-known-good rules.
- [x] Define the narrow renderer invalidation API, command registration owner,
  shutdown ordering, and final `changed`, `all`, and manual-retry command
  spelling.
- [x] Confirm which RHI-reinitialization hooks actually exist. Record the
  device-invalidation call contract without inventing a device-recovery event.
- [x] Select a deterministic factory-injection seam that can fail shader,
  declaration, sampler, buffer, texture, and individual pipeline steps without
  requiring a real driver failure.
- [x] Decide whether keyed static-mesh state retains vectors or needs
  node-stable storage; document pointer lifetime and synchronous lookup
  assumptions.
- [x] Add focused failing tests for initial failure suppression, retry after a
  relevant generation, no retry after an unrelated generation, transactional
  rollback, last-known-good refresh, device invalidation, reentrant creation,
  and one-time diagnostics.
- [x] Record the dependency boundary with the Demand-Driven Editor Assistance
  Renderer and Material System plans before editing overlapping state.

#### Acceptance Gate

- One ownership and transition design is recorded; command and shutdown
  ordering are explicit; focused tests fail against the current boolean and
  incomplete-entry behavior for the expected reasons; no unresolved device
  event is presented as implemented.

### Stage 1: Implement and validate the recoverable slot primitive

- [x] Implement the selected availability/attempt slot with owned failure
  diagnostics and generation dependency masks.
- [x] Implement a factory result boundary that cannot publish a partial
  candidate.
- [x] Implement same-generation suppression, relevant-generation retry,
  reentrancy rejection, recovery logging, and failure fingerprinting.
- [x] Implement last-known-good refresh for shader/manual invalidation and
  destructive payload clearing for device invalidation.
- [x] Keep the primitive synchronous and free of internal locks or background
  work.
- [x] Pass the focused transition, rollback, generation, fallback, and logging
  tests.

#### Acceptance Gate

- Every legal transition is covered by deterministic native tests; a failed
  attempt cannot expose a payload; the same generation performs at most one
  attempt; a relevant new generation can recover; a failed shader refresh
  preserves the earlier ready payload.

### Stage 2: Repair static-mesh base, shader-map, and pipeline caches

- [x] Convert static-mesh base declarations and samplers from
  `bBaseResourcesCreateAttempted` to one device-dependent transactional slot.
- [x] Convert shader-map entries to shader-generation-dependent state slots.
- [x] Compile and bind a complete local shader-map candidate before commit.
- [x] Convert pipeline entries to shader-and-device-dependent state slots.
- [x] Create the complete current PSO aggregate locally and commit only after
  every required pipeline succeeds.
- [x] Make lookup return a payload only from a ready or valid stale-ready slot;
  never infer permanent failure from an empty resource member.
- [x] Preserve old pipeline and owning shader-map references across a failed
  shader refresh.
- [x] Add focused tests for first compile failure then same-identity recovery,
  first PSO failure then recovery, unrelated identity isolation, same-generation
  suppression, and old-pipeline preservation.
- [x] Keep Material System identity and render-output behavior unchanged.

#### Acceptance Gate

- No failed static-mesh shader or PSO creation leaves an incomplete payload
  entry; the same key recovers after relevant invalidation; one identity's
  failure does not affect another; existing material rendering tests remain
  behaviorally unchanged.

### Stage 3: Migrate fixed Renderer and Texture Editor resources

- [x] Convert Sky Box, Post Process, Texture Cube thumbnail, Gizmo, Overlay
  Line, Overlay Icon, and Editor Grid base states to transactional slots.
- [x] Coordinate Grid, Gizmo, Line, and Icon migration with the active
  demand-driven assistance stages so feature demand and pipeline decomposition
  are not implemented twice.
- [x] Convert Texture Editor preview resource creation to the common attempt
  and failure semantics while preserving its module ownership.
- [x] Split monolithic candidate aggregates only where shader, device, dynamic
  per-view, or pipeline-key dependencies require different invalidation or
  failure domains.
- [x] Remove migrated `bCreateAttempted` flags and independent
  failure-log-suppression booleans.
- [x] Route renderer and preview shutdown through explicit slot reset/release
  operations.
- [x] Add per-feature and preview failure-injection tests proving independent
  availability and recovery.

#### Acceptance Gate

- No in-scope Renderer or Texture Editor preview resource uses
  `bCreateAttempted`; each fixed aggregate is transactionally committed;
  failure of one feature does not disable unrelated drawing; reset releases
  payloads and diagnostics in render-thread order.

### Stage 4: Add explicit shader reload and invalidation integration

- [ ] Register the selected development shader-reload and manual-resource-retry
  commands with bounded module ownership.
- [ ] Implement `changed` as lazy shader-generation invalidation backed by the
  existing dependency fingerprint path.
- [ ] Implement `all` as lazy shader-generation invalidation with forced
  recompilation on the next demanded candidate.
- [ ] Enqueue invalidation from non-render threads and verify ordering against
  already submitted views.
- [ ] Add and test the internal device-generation invalidation hook; keep the
  old payload unavailable after device invalidation.
- [ ] Report queued invalidation, recovered resources, current failures, and
  retained stale-ready resources through concise diagnostics.
- [ ] Ensure command unregistration and renderer shutdown cannot race a late
  invalidation request.
- [ ] Add integration tests for broken shader, corrected shader plus
  `changed`, forced `all`, manual RHI-factory retry, and shutdown with a queued
  invalidation.

#### Acceptance Gate

- A shader compile failure can be corrected and recovered in one editor
  session without module restart; unchanged failed generations do not retry;
  `all` bypasses successful shader output reuse on next demand; device
  invalidation never exposes old RHI objects; shutdown admits no late retry.

### Stage 5: End-to-end validation and lasting documentation

- [ ] Run focused RenderCore, Renderer, Material, thumbnail, Texture Editor,
  assistance, Sky Box, and Vulkan tests through the repository-native workflow.
- [ ] Run a successful full `all` build through the root DurinDevTool workflow.
- [ ] Run the verified `DurinEditor` with the same Agent Build Profile.
- [ ] In a development shader copy or controlled test shader, introduce a
  compile error, observe one retained diagnostic, fix it, run the changed
  reload command, and verify recovery without restarting the module.
- [ ] Verify a failed refresh continues drawing the last-known-good shader/PSO
  and a successful refresh swaps output without a partial-resource frame.
- [ ] Verify initial resource failure skips only the affected draw, does not
  spam logs, and recovers after explicit retry.
- [ ] Verify main, auxiliary, window-backed, and render-target-backed views
  retain existing output, scissor, post-process, assistance, and thumbnail
  behavior.
- [ ] Update the owning runtime rendering documentation with the landed state,
  transaction, generation, diagnostics, and shutdown contracts.
- [ ] Update the Demand-Driven Editor Assistance Renderer plan with landed
  common-state evidence and remove any superseded permanent-failure wording.
- [ ] Record validation evidence and complete this plan only after every
  required gate passes.

#### Acceptance Gate

- Focused and integration tests, full build, Vulkan editor smoke, shader
  repair/reload exercise, failure isolation, log-suppression inspection, and
  last-known-good visual behavior pass; lasting documentation owns the final
  contract.

## Validation Matrix

| Scenario | Required behavior | Evidence |
| --- | --- | --- |
| Initial shader failure | One failed state and diagnostic; affected draw skipped; no per-frame retry | Slot test plus injected Renderer test |
| Same generation lookup | Factory attempt count remains unchanged | Focused generation test |
| Unrelated generation change | Slot remains suppressed | Dependency-mask test |
| Relevant shader generation | Same identity becomes eligible and can recover | Static-mesh cache test |
| Candidate late-step failure | Live state contains no partial first payload | Transaction rollback test |
| Failed shader refresh | Old shader and PSO remain drawable; refresh failure retained | Last-known-good test plus visual smoke |
| Successful shader refresh | Candidate atomically replaces old payload and clears failure | Integration test |
| Static-mesh shader-map failure | No empty payload tombstone; another identity remains ready | Keyed cache test |
| Static-mesh PSO failure | No partial solid/wire aggregate; retry succeeds after invalidation | Keyed pipeline test |
| Fixed Renderer feature failure | Only the affected feature or pass is unavailable | Failure-injection test |
| Texture Editor preview failure | Preview reports failure and recovers without module restart | Editor test or smoke |
| `changed` reload | Dependency fingerprint selects changed output lazily | Compile-service/Renderer integration test |
| `all` reload | Next demanded candidate forces compilation | Compile counter test |
| Device invalidation | Old RHI payload is cleared and cannot serve as fallback | Device-generation test |
| Reentrant lookup | No second factory call and no partially visible payload | Slot test |
| Repeated identical error | One log per key and generation | Captured-log test |
| Recovery | One concise recovery diagnostic and normal drawing resumes | Captured-log plus render test |
| Queued invalidation then shutdown | Command ownership is ordered; no late retry or live resource | Lifecycle test and editor exit smoke |

## Definition of Done

- No in-scope `bCreateAttempted` flag remains.
- No static-mesh cache path interprets an incomplete payload entry as a
  permanent cached result.
- Every in-scope resource group constructs into a local candidate and commits
  only after complete validation.
- Failed attempts retain category, context, owned message, retry dependencies,
  generation, and fallback availability.
- Same-generation failures do not retry or repeat logs.
- Relevant shader/manual invalidation permits recovery without module restart.
- Shader refresh preserves last-known-good resources; device invalidation does
  not.
- Static-mesh shader and pipeline identities retain current Material System
  semantics and failure isolation.
- Explicit changed/all shader reload is ordered, demand-driven, test-covered,
  and safely unregistered at shutdown.
- Device-generation invalidation has a tested internal contract without
  claiming unsupported Vulkan recovery.
- Focused tests, full build, real Vulkan editor smoke, shader repair/reload,
  failure isolation, and visual behavior validation pass.
- Lasting renderer documentation describes the implemented contract and
  coordinated active plans contain no conflicting failure policy.

## Deferred Follow-ups

- Shader-source filesystem watching and automatic generation advancement.
- Async shader compilation, PSO creation, cancellation, and completion
  publication.
- Real Vulkan device-loss detection, teardown, recreation, and renderer
  resubmission.
- Engine-wide PSO precaching, serialization, prewarming, and driver-cache
  policy.
- Default-material or feature-specific visual fallbacks while no
  last-known-good payload exists.
- Persistent editor UI for resource failures beyond console/log diagnostics.
- Automatic retry backoff if future platforms expose transient failures
  without a reliable recovery event.
- General typed RHI creation errors across every backend.

## Related Documentation

- `Documentation/Runtime/Core/RuntimeLifecycle.md`
- `Documentation/Runtime/Rendering/ViewportRendering.md`
- `Documentation/Runtime/Rendering/ShaderCache.md`
- `Documentation/Plans/DemandDrivenEditorAssistanceRenderer.md`
- `Documentation/Plans/MaterialSystem.md`
- `Documentation/Plans/Archive/2026-07/ShaderCacheHardening.md`
- `Documentation/Development/Build/BuildAndRun.md`
- `Documentation/Development/Build/NativeTests.md`
- [UE FRenderResource API](https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/RenderCore/FRenderResource)
- [UE Threaded Rendering](https://dev.epicgames.com/documentation/en-us/unreal-engine/threaded-rendering-in-unreal-engine)
- [UE Shader Development](https://dev.epicgames.com/documentation/en-us/unreal-engine/shader-development-in-unreal-engine)
- [UE PSO Precaching](https://dev.epicgames.com/documentation/en-us/unreal-engine/pso-precaching-for-unreal-engine)

## Related Code

- `Engine/Source/Runtime/Renderer/Private/RendererModule.cpp`
- `Engine/Source/Runtime/Renderer/Private/RendererEditorAssistance.h`
- `Engine/Source/Runtime/Renderer/Private/RendererEditorAssistance.cpp`
- `Engine/Source/Runtime/RenderCore/Public/Shader/Shader.h`
- `Engine/Source/Runtime/RenderCore/Private/Shader/Shader.cpp`
- `Engine/Source/Runtime/RenderCore/Private/Shader/ShaderCompileService.cpp`
- `Engine/Source/Runtime/RHI/Public/DynamicRHI.h`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanPipeline.cpp`
- `Engine/Source/Editor/TextureEditor/Private/Widgets/TexturePreview.cpp`
- `Engine/Source/Runtime/Core/Public/Console/ConsoleCommand.h`
- `Engine/Tests/Native/RenderCoreTests/Private/ShaderCompileServiceTests.cpp`
- `Engine/Tests/Native/RenderCoreTests/Private/RenderResourceLifecycleTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/RendererEditorAssistanceTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/Materials/MaterialRenderingTests.cpp`
