# Renderer Resource Recovery

Summary: Define asynchronous Renderer readiness, asset/shader preparation errors, generation-scoped invalidation, and required-success native creation.

Modules: RenderCore, Renderer, RHI, VulkanRHI, TextureEditor

Last reviewed: 2026-11-19

## Required-Success Native Construction

Ordinary texture/buffer/shader/view factories require complete non-null resources.
Known PSO compilation failure may return null and produces an accepted Failed
pipeline result. OOM, device loss and unexpected failures remain terminal through
the [RHI creation boundary](../RHI/RHICommandExecution.md#required-success-resource-creation).
Renderer owns preparation availability and generation-scoped failure suppression,
not native fault classification. It does not retry a failed compilation each frame.
Source/content errors and asynchronous admission remain distinct outcomes.

A successful `RHICreateGraphicsPipelineState` need not produce a distinct object.
Vulkan reuses an equal normalized pipeline key; DebugName affects diagnostics,
not identity. Cache eviction releases only the cache reference. Explicit owners
and recorded commands keep their pipeline/dependency references valid.

Vertex declarations copy immutable CPU descriptions on the caller. Vulkan native
result codes feed the terminal RHI diagnostic; they are not transported as asset
errors. Texture views of external images retain ordered synchronization because
swapchain backing can change during replay.

Native graphics/compute PSO candidates receive owned input copies and retained
shader/declaration references, with render-pass and structural-layout dependencies
acquired separately. The Vulkan manager serializes cache misses in a creator
domain. Ready lookup, strong-reference acquisition, LRU, capacity reservation,
and publication use one short cache/statistics transaction. A full cache reserves
a victim before native compilation and restores its retained node on compilation
failure. Prefer unused entries; externally owned pipelines remain valid if their
cache reference is evicted. Driver compilation holds a separate exclusive cache
mutex. Synchronous misses create directly through the native manager rather than
joining optional precache admission; ready hits require no replay submission.

Native PSO construction can run away from replay. Published RHI objects still
retire through the RHI deletion owner: final reference release appends an
intrusive node already reserved in the resource, without allocation or scheduler
admission. Gathering reserves deletion-batch storage before removing queue
ownership. PSO destructors notify owner-thread context caches and enqueue native
handles for GPU-completion-aware deletion; a background final release never
destroys those handles directly. Native graphics/compute creation uses the basic
Vulkan result-code interface. Its failure branch releases any returned pipeline
and the earlier pipeline layout before the manager handles its native result.
Native failure terminates instead of publishing a failed candidate. Successful
cache publication and GPU-aware retirement retain their ownership ordering.
Input descriptions with raw resource fields require a live
caller-owned reference until the factory has copied and retained them.

Texture assets use the owned update protocol in [Texture System](../../Assets/Families/TextureSystem.md).
`UpdateResource()` uploads installed immutable input in render-command order.
Explicit release/update invalidates the old allocation; source/build failure
before that boundary can preserve accepted content. Native allocation failure
terminates, so there is no per-texture GPU-failure fallback or retry state.
Thumbnail readiness remains separate from CPU compilation completion.

## Non-Pipeline Creation Domains

The following Vulkan native factories execute on the calling thread, with no
replay submission: shader, sampler, buffer allocation, local texture allocation,
buffer view, and stable local texture view. Vertex declarations remain CPU-only.
The caller must hold the device lifetime and input references until return;
shutdown must join such callers before destroying the device. This is not
permission to concurrently record into one command list or mutate image backing.

| Resource | Native readiness at return | Data and batch contract |
| --- | --- | --- |
| Shader | Complete module, owned entry-point string, immutable reflection | Code/name inputs are borrowed only during the call; an ordered caller batch retains successful modules until its consumer transaction commits |
| Sampler / declaration | Complete immutable state | Independent calls; no command-context work or replay waits |
| Buffer | Complete VMA allocation and handle | Initial bytes are copied into the caller-owned command list; upload, host flush, barriers and visibility occur in recorded order |
| Local texture | Complete VMA allocation and image | Storage initialization is a recorded transition; texture uploads remain separate owned commands, and allocation success does not imply defined contents |
| Buffer / local texture view | Complete view retaining its source resource | The caller retains stable backing during creation. RDG owners retain reusable views for their resource lifetime |

These categories use ordered ordinary calls rather than a second asynchronous
service. Normal-return native results are complete. Renderer slots and RDG retain
transactional publication for logical preparation and validation.
Buffer/texture initial-data commands may already own an unpublished candidate
after a later batch failure; their references retire in command order. No batch
failure rewinds submitted GPU work or publishes a partially initialized aggregate.

VMA runs with internal synchronization enabled. Allocation handles and initial
state trackers are object-local until publication; allocation statistics and
heap snapshots use their existing mutex. Naming counters are atomic. Persistent
mapping does not grant concurrent access to the same byte range: mapped writes,
normalization/flush/invalidation, transfer arenas, state trackers and reclaim
remain governed by the existing recording/replay owner protocols. Construction
ownership remains ordered through successful creation and publication. Native
creation failures terminate; no per-resource native rollback/retry result is
exposed to Renderer.

Retained synchronous entries have explicit reasons: external/swapchain texture
views follow mutable replay-owned backing; viewport creation/resize follows
game-thread presentation ownership and replay ordering; GPU timing queries use
owner-managed pools; completed-resource collection polls retirement on its owner;
frame preparation and dynamic uniform/storage overflow reserve producer pages
through the owner; native command-buffer integration, reads and idle waits need
ordered context access. These entries are not arbitrary-thread factories. The
universal create-and-wait wrapper remains removed.

## Internal Pipeline Compilation

During device initialization, a backend publishes its capabilities and supplies
native creation callbacks. Hosts running the Core task scheduler at this point
create one compilation cache and queue for the device. Core owns the worker
threads and stays running until RHI exit completes. Vulkan owns the native pipeline manager
and driver cache. Cache acquisition and precaching
copy immutable descriptions, names, and strong shader/declaration references
before returning. Public drawing and preparation do not submit raw creation
requests. Backend hooks supply native lookup and creation callbacks through
`FRHIPipelineCompileBackend`; creation callbacks call the native manager directly,
never the public cache or a synchronous compatibility factory.

After pipeline producers stop, RHI exit closes the cache. Cache shutdown
cancels queued compilation and joins active creators. Owners release their PSO
references and accepted command lists drain before the backend destroys the
device. CPU outcome observations and immutable layout metadata can outlive RHI
exit. Completed compilation outcomes remain stable. A replacement device starts
with a fresh cache and capability snapshot.

The cache constructs its private `FPipelineCompileQueue` during initialization.
When compilation is needed, the queue submits a drain task to the existing Core
worker pool. Only one native creator runs at a time. Valid requests are not rejected by
artificial request-count, description-size, or metadata-budget limits.
Each live key shares one compilation state, priority, result, and completion
event. The queue retains unfinished work independently of caller handles; dropping
a caller does not cancel compilation. There is no per-caller cancellation,
observer registry, or observation-count admission limit. No public request/batch admission bypasses the cache,
and no per-item semaphore-waiting worker is launched.

Ordinary tasks admit requests directly on the calling thread without waiting for
compilation or scheduler capacity. Launching the device-owned Worker drain does
not add a completion dependency to the calling task. Cache and private-queue
admission reject independent CPU tasks, including inherited independent children.
Pending native waits reject Core workers, RHI replay, and the creator itself.
Core is never started implicitly.

Internal completion edges observe terminal notification and carry no native
resource references. Accepted compilation completes Ready or Failed, or is canceled
by its owner/lifetime. Native allocation/device errors terminate from the creator.
Only Ready exposes complete native resources. Unavailable precache acquisition
returns empty; required acquisition or required compilation failure is fatal.

RHI interns canonical pipeline-layout, vertex-input, and blend descriptions in
weak CPU-only indexes without a live-description cap. Shared metadata accounting
charges dynamic container capacities once, plus conservative object/node overhead.
Keys retain immutable shared descriptions; the last strong
description owner releases its accounting lease. Descriptions contain no device or
shader resources and may survive backend shutdown.

Vulkan PSO request metadata and native pipeline/dependency metadata have separate
lifetime accounting; neither counter rejects creation.
Retained PSO tables remain capped at 2,048 entries per type and structural
layouts at 256; evicted objects can remain alive under external ownership.
Descriptor-set layouts and render passes are interned until device teardown,
without artificial live-object count limits. Accounting is logical CPU metadata,
not an allocator/driver memory measurement.

`RHIExit` closes request admission and joins the device-owned drain task before
release delegates, replay shutdown, or native cache persistence/destruction.
Queued compilations settle Canceled; active compilation completes its event only
after its native call returns, and cannot publish a usable result into a closed
device generation. In-flight creator captures are joined separately before
teardown. Commands retain ready PSOs through replay.
Immutable request layout aliases remain valid after backend module unload;
their metadata accounting and deleter are owned by RHI, not the Vulkan module.
Callers must still release explicit RHI references copied out of a Ready result
before device destruction. The RHI lifecycle owner joins compilation outside the
cache ownership mutex so creator callbacks can finish.

## Pipeline Cache And Binding

`PipelineStateCache.h` separates typed `FGraphicsPipelineState` and
`FComputePipelineState` cache identities from complete native RHI PSOs.
`PipelineStateCache::GetAndOrCreate*PipelineState` and `Precache*PipelineState`
return the same shared identity for an equal normalized key while an owner or
recorded command retains it. Names are diagnostic only. Ordinary acquisition
returns a typed reference and must succeed; unavailable acquisition is fatal.
Like UE, ordinary acquisition receives the recording command list and attaches
the internal compilation dependency there. Native compilation may still be Pending;
command dispatch joins its shared completion event before replay. A failed PSO required by draw/dispatch is fatal,
including a failure first discovered during precaching. `Precache*` may return
an empty reference when acquisition is unavailable or the description is invalid;
an acquired precache identity may settle as Failed without terminating rendering.
There is no public rejection enum or `expected` acquisition protocol.

Identities expose Pending/Ready/Failed/Canceled, immutable layout metadata, and
type-specific nullable `GetRHIPipeline()` results. `Wait()` returns the observed
domain state: Pending means the caller cannot safely wait; the other states are
terminal. Completion tasks, typed native accessors, executor wait preflight, and
`FPipelineCompilation` belong to the Backend implementation seam. Public state
objects hide compilation storage behind an incomplete type.
`GetStateObservation()` returns read-only outcome metadata without atomic storage,
native ownership, or a completion task. It is not proof of GPU completion.
Dropping a state reference never cancels shared compilation. Shutdown cancels work that has yet to publish a result.

The device-owned PipelineStateCache has a weak table without a live-identity cap. Keys and
identity metadata participate in device metadata accounting; shared
descriptions are charged only by their RHI owner. Unfinished initializer/name
reservations are released when compilation finishes; retained shared states pay
for one key and compilation identity regardless of handle count. The private
compilation cache stores hashes and weak state references, compares full keys on
hash matches, and prunes expired entries on cold acquisition. It retains no
descriptors or native results after the last shared state is released. Acquisition prunes expired public cache
identities every 60 seconds; no metadata-pressure retry path is required.

At frame begin, Vulkan retains at most 64 cache-only graphics and 64 cache-only
compute PSOs, evicting older entries first. This periodic trim preserves externally
retained entries; capacity eviction can release their cache reference. External
owners and GPU-aware retirement protect native lifetime.
Maintenance skips an active creator rather than waiting for native compilation.
Evicted PSOs can be recreated using the persistent driver pipeline cache; this
retention policy trades CPU/driver memory for possible future creation latency.
Cache acquisition registers cold identities directly under the cache and queue
mutexes; it creates no admission thread, future, or cross-thread handoff. Native
creation remains on the existing Worker drain. Cache closure closes acquisition
under the cache mutex, then delegates the idempotent compilation join to its
private queue. The queue owns shutdown serialization and joins its last drain,
including earlier compiler capture retirement.

Drawing binds an explicit typed pipeline state through the command-list overload,
which records its internal compilation dependency. Command-list overloads accept
typed cache references or complete native RHI objects. Recording never waits for
native compilation. There is no scheduler-dependent synchronous fallback. Without
Core, precache returns empty and required acquisition is fatal; startup tools use
the explicit native creation factories.

## Transactional Resource Slots

Resource factories return exactly one owned result: Pending, Canceled, payload or
error. A tagged union prevents empty or contradictory result combinations.

With Core running, production fixed graphics/compute and static-mesh PSO
factories use `FRenderPipelinePreparationScope` inside their owning slot. Logical
resource preparation before consuming passes is the prewarm trigger. A slot
retains normalized shared cache identities across attempts without a key-count
or metadata-size admission cap. Precache and drawing share the same compilation;
resetting a slot releases its handle without canceling queued or active creation.
CPU preparation retries on later preparation without a failure diagnostic or a
synchronous wait in the factory. Accepted native PSOs
publish cached handles before compilation completes. Scene preparation collects
these handles explicitly at each resource phase, including ready cache hits,
and the caller-owned view request observes native completion without revisiting
resolved phases. Normal drawing records the handles and their completion
dependencies; the RHI executor gates replay at closed command scopes.
Resource-phase batches collect shared dependencies without joining compilation.
CPU preparation and admission errors remain owned by their slots. View
observations have no artificial handle-count admission limit.
Multi-PSO metadata publishes after all handles are accepted, while explicit
prewarm readiness requires native completion. Request ownership, cancellation
and invalidation follow [load preparation](LoadPreparation.md#scene-resource-readiness).
A slot invalidates its previous payload before attempting the requested generation;
neither Pending nor Failed returns an earlier shader/PSO/binding version.
Generation changes release obsolete cache identities. Late results may populate
the backend cache but cannot replace the current slot. Startup without Core keeps explicit synchronous
compatibility. Shader preparation itself is not made asynchronous by this helper.

Recorded command lists retain typed cache identities and their internal creation
dependencies. Pending identities expose immutable layout metadata, never
unfinished native handles.
Submission deduplicates shared compilation dependencies without a PSO-count cap.
The threaded queue preserves FIFO serial order and wakes from Core completion; its
replay thread does not wait inside a command. A failed required compilation is fatal. A canceled dependency
skips the whole batch and terminates its CPU fence as Canceled,
while later independent batches can proceed. Inline submission waits before
replay only on permitted callers. Ordinary Submit and QueueCommandList are
required-success operations; invalid input and submission after queue closure are
internal errors, not a public retry protocol.
Neither creator work nor its dependencies may depend on replay. Compute stage
and binding validation remains active; a pipeline becoming Ready does not make
missing bindings valid.

`FRHICommandListFence` reports Pending, Succeeded, Canceled, or Expired.
Completed serials describe terminal CPU queue progress, not successful native
execution or GPU completion. Captured fences retain their batch outcome; render
fences capture that receipt on the rendering thread. Late serial lookup uses a
fixed 4,096-entry history and returns Expired when a needed receipt has been
overwritten, never an inferred success. `Wait` blocks and returns the final batch
state, including Canceled or Expired. GetState/IsComplete are nonblocking queries;
there is no TryWait wrapper. Required-success owners such as render fences check
the returned state before releasing their completion receipt. Queue payload
accounting reserves dependency wake storage; retired groups release subscriptions.

Fixed Renderer resources, static-mesh shader and pipeline identities, editor
assistance, shared fullscreen geometry, and Texture Editor previews use
`TRenderResourceCreationSlot`. A slot constructs a complete candidate in local
ownership and publishes only after every binding, RHI resource, and pipeline
succeeds. Callers observe a complete payload only in Ready; Pending and Failed return null.
Uninitialized denotes a slot that has not been requested. The availability query
distinguishes deferred creation from terminal failure; partially initialized
aggregates are never visible.

Fixed non-Material shader compilation and typed lookup are centralized in
RenderCore's [Global Shader](../Shaders/GlobalShaders.md) map. Each bounded exact shader
set has its own transactional slot and strong `FGlobalShaderSetRef` lifetime.
Renderer feature payloads retain typed `TShaderMapRef` values and the exact set
used by their PSO; they do not allocate, cast, or own private global
`FShaderMapBase` instances. Material and vertex-factory/mesh combinations use
`TMaterialShaderRef` over an exact `FMaterialShaderMap`; consumers likewise do
not initialize an ordinary map, perform a raw lookup, or cast a shader.

`FSimpleElementRenderer` follows the same contract. Its line and sprite Global
Shader sets are independently demandable, so one unavailable class skips only
dependent batches. Output/depth/blend/shader-class pipeline keys retain the
exact typed shader refs used to create them. Persistent vertex and index upload
buffers grow to bounded power-of-two capacities through required-success
native factories. Device invalidation releases pipelines,
declarations, atlas resources, and upload buffers before lazy reconstruction;
shutdown performs the same ordered release.

Each owner tracks independent shader, device, and manual generations. Successful
payloads remain cached until a relevant generation changes. Shader preparation
failures retain their owned cause and retry dependencies, suppressing repeated
attempts until a relevant generation changes. This includes recognized PSO
compilation failures. Native allocation/device faults terminate; no native
allocation retry exists.

`FRenderResourceCreateError` owns an optional Shader cause plus resource context,
identity, category and reason. Native error details remain at the backend logging
boundary. Accepted async compilation may publish Failed with no pipeline, while
allocation/device faults terminate. Preparation scopes distinguish an empty
acquisition result from an acquired identity whose compilation failed. `FormatRenderResourceCreateError`
formats owner diagnostics when they are reported. Material validation formats its
own typed error at its logging boundary.

Failure fingerprints include category, reason, owner/identity, Shader semantic
context and retry dependencies. They exclude attempt generations and opaque
compiler diagnostics. Repeated failures with the same fingerprint do not emit
duplicate owner diagnostics; successful creation reports recovery once.

Fallback belongs to the consuming owner, not the creation slot. Material-data
validation selects the complete ErrorMaterial representation before draw planning;
Renderer resource failure does not combine an earlier PSO with current material
bindings. Unavailable draw resources prevent that draw from becoming Ready;
optional effects follow their explicit skip or neutral-resource policy, and required
frame resources keep their failure boundary. Pending replacements participate in
preparation waits without turning deferred creation into a failure diagnostic.
Editor installation may retain an accepted version only through its own explicit
transaction, such as the texture update protocol above. The generic slot supplies
no installed-version fallback. Device-generation changes always discard dependent
RHI payloads. This seam does not recover a lost Vulkan device or a failed RHI executor.

Compiled materials retain one immutable accepted `FMaterialCompilerResult` in
Engine render data. The Renderer adapter verifies its identity, target, pass
contract, and generated entry point before combining accepted fragments with
fixed mesh stages in one exact typed candidate. RenderCore validates reflection,
bindings, set identity, and merged layout. Shader reload may refresh a fixed
mesh stage; device invalidation discards combined RHI shaders and PSOs. Both
reconstruct lazily without rereading graph state or recompiling generated
Material IR. Failed candidates expose no map or pipeline for the requested
generation; previously recorded consumers retain their own resource references.

Frame-transient targets use the Renderer-private RDG allocator described by
[Renderer Frame Preparation and Render Graph Execution](RendererFramePreparation.md).
Allocation publishes only after every retained texture or buffer resolves.
Each graph execution attempts missing allocations once, without moving shaders,
PSOs, samplers, or committed view history into transient ownership.

The graph frame executor derives one immutable requirements value after logical
preparation and persistent-resource resolution, then the single-use graph builder sends
one retained descriptor batch to `FRDGAllocator` before the first consuming
pass. The pool key contains the complete allocation-compatible descriptor and
excludes diagnostic names, observation tags, and feature identity. Texture and
buffer requests share whole-batch reservation, pre-allocation
eviction, rollback, publication, and error reporting; only typed RHI creation
differs. `RHICreateTexture` and `RHICreateBuffer` return complete resources.
Native allocation failure terminates; RDG does not report a retryable physical
allocation failure or inject one into production allocation logic.

A rejected logical batch rolls back newly retained candidates and publishes
no graph allocation or extraction destination. The
graph returns a preparation failure before recording any pass.

The allocator retains no failed-descriptor cache, retry counter, or cooldown.
A later graph execution may retry a rejected logical reservation. Native
allocation failures have no later graph execution in the same process.
Before a later creation attempt, RHI collects completed retirement without
prematurely deleting in-flight resources. A new attempt always needs a newly
authored builder. Compile or preparation failure consumes the current builder; retrying the same builder returns
InvalidState without consulting the allocator. Pass execution receives counted resources and never
performs target lookup, creation, or recovery policy itself.

Qualification policy does not participate in generation state or mutate a
prepared plan. A Renderer-private scoped policy may add feature-bounded target
requirements for one test/tool submission, but those targets follow the same
pool transaction and invalidation rules as production and supported debug
views.

## Invalidation And Commands

Renderer route-policy changes use the separate configuration version described
in [Load preparation](LoadPreparation.md#renderer-preparation-configuration).
They retire obsolete readiness observations without advancing shader/device
generations or clearing exact native PSO cache entries.

Renderer owns these development commands:

- `renderer.reload-shaders changed` advances shader generation and lets normal
  dependency fingerprints select changed output on next demand.
- `renderer.reload-shaders all` advances shader generation and forces
  compilation for each next-demanded shader candidate.
- `renderer.retry-resources` advances manual generation for eligible failed
  shader/content preparation; it does not recover native resource failures.

Console callbacks enqueue one render command. Views submitted before that
command retain the old generation; later views observe the new one. Resource
preparation remains demand-driven on the rendering thread; pipeline requests
finish asynchronously and are consumed on a later preparation attempt.
New failures and changed fingerprints produce one diagnostic, and successful
retry reports one recovery transition.

`FRendererResourceCoordinator` owns command admission and the shader, device,
and manual generation counters. It explicitly supplies accepted generations
to the RenderCore global map while `FSceneRenderingService` fans requests out to its
remaining concrete owners. Shader and manual invalidation leave
reconstruction lazy. Device invalidation releases every dependent payload
before advancing the device generation, recreates only startup defaults, and
leaves feature resources to rebuild on demand.

`FRendererModule` is the explicit cross-module request and focused-test entry
point. It forwards only while its composed `FSceneRenderingService` exists; shutdown
stops the scene renderer before destroying it. Consumers composed below the
scene renderer receive the coordinator by reference. No active coordinator
pointer or process service-locator path exists.

The device-invalidation request is a tested internal seam, not a claim of
Vulkan device-loss recovery. Renderer shutdown closes command admission,
unregisters development commands, enqueues release, and flushes rendering work.
Texture Editor retains module ownership of its preview slot and releases it
through its own ordered shutdown.

Global-map device invalidation is ordered after Renderer consumers release
their pipelines and typed refs and before the new device generation is
published. Shutdown follows the same consumer-before-map order.

## Creation Measurement Boundaries

The opt-in Vulkan creation qualifiers measure public calls and frame workloads
from the test side. Shared compilation admission, first-consumer waits, and an
independent replay marker have separate measurements. Sequential caller
observation of Ready is not the publication timestamp. These measurements do
not isolate native driver time or creator queue latency. Resident pipeline
caches start empty for each device lifetime; the persisted driver cache uses
the normal runtime path and is not reset by a test-only override.

The selected background configuration has one native creator. Concurrent
producers can share one normalized key while replay progresses, but background
execution does not promise lower native driver latency or eliminate first use
waits. Compare median and p95 using identical input bytes, cache policy, host
configuration, and process lifecycle order. Process private peaks include driver
and allocator retention across device lifetimes; RHI metadata accounting
does not bound them. Preserve absolute timing differences and measurement
uncertainty instead of treating microsecond percentage drift as a functional gate.

## Related Documentation

- [Viewport Rendering](ViewportRendering.md)
- [HDR Scene Color and Display Mapping](HDRSceneColorAndDisplayMapping.md)
- [Renderer Frame Preparation and Render Graph Execution](RendererFramePreparation.md)
- [Runtime Lifecycle](../../Application/RuntimeLifecycle.md)

Independent procedural capture and transient environment generations follow
[Sky Lighting](../Features/SkyLighting.md).
