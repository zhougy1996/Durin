# Renderer Resource Recovery

Summary: Define complete-or-null Renderer resource publication, explicit readiness, generation-scoped retries, and device invalidation.

Modules: RenderCore, Renderer, RHI, VulkanRHI, TextureEditor

Last reviewed: 2026-10-03

## Complete-Or-Null Construction

Nullable RHI creation is a complete-or-null boundary. Vulkan buffer, texture,
shader, graphics-pipeline, sampler, and vertex-declaration factories publish a
reference only after every required native handle and allocation exists.
Expected creation failure returns null without failing the RHI executor; device
loss, command replay, submission, presentation, and invariant failures remain
terminal.

`RHICreateGraphicsPipelineState` returns a complete pipeline or null; it does
not promise a distinct object on every call. Vulkan reuses a strongly retained
pipeline for an equal normalized graphics-pipeline key. `DebugName` labels
diagnostics and captures and is excluded from that key. Changing the name does
not require a new pipeline, while changing semantic pipeline state can select
a different pipeline even with the same name. Compute pipelines follow the
same Vulkan cache policy. Renderer slots and explicit Renderer-owned payloads
hold logical ownership; the backend cache and recorded commands can also retain
references. Cache eviction selects only entries with no external references.

Vertex declarations copy immutable CPU descriptions directly on the caller.
Vulkan error translation and the RHI fallible creation boundary execute their
callback without scheduling or waiting. Context-required public factory entries
choose their compatibility scheduling explicitly.

Native graphics/compute PSO candidates receive owned input copies and retained
shader/declaration references, with render-pass and structural-layout dependencies
acquired separately. The Vulkan manager serializes cache misses in a creator
domain. Ready lookup, strong-reference acquisition, LRU, capacity reservation,
and publication use one short cache/statistics transaction. A full cache reserves
a cache-only victim before native compilation and restores its retained node on
failure. Driver compilation holds a separate exclusive driver-cache mutex without
the table transaction; cache hits do not wait for the creator. Public synchronous
PSO entries return Ready hits directly with zero replay submissions. With Core
running, misses join the bounded request channel; a permitted caller waits for
its result. Startup compatibility without Core runs native creation directly
on the caller, while asynchronous requests return Unsupported.

Native PSO construction can run away from replay. Published RHI objects still
retire through the RHI deletion owner: final reference release appends an
intrusive node already reserved in the resource, without allocation or scheduler
admission. Gathering reserves deletion-batch storage before removing queue
ownership. PSO destructors notify owner-thread context caches and enqueue native
handles for GPU-completion-aware deletion; a background final release never
destroys those handles directly. Unpublished native candidates roll back their
handles immediately. Input descriptions with raw resource fields require a live
caller-owned reference until the factory has copied and retained them.

Texture assets use the owned update protocol in [Texture System](TextureSystem.md).
An explicit `UpdateResource()` retries installed immutable input. Availability
remains true after failed replacement when a prior successful allocation exists;
retiring an old resource cannot unbind its replacement. Failure
details are logged, while the update retains only its completion state. These
assets have no render-request generation, and thumbnail readiness is checked separately
from last-successful fallback availability.

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
| Buffer / local texture view | Complete view retaining its source resource | Backing must remain stable during creation; automatic view-cache lookup/publication remains mutex protected |

These categories use ordered ordinary calls rather than a second asynchronous
service. A caller batch can contain mixed success/null results; Renderer slots
and RDG retain their all-or-nothing publication and candidate-release policy.
Buffer/texture initial-data commands may already own an unpublished candidate
after a later batch failure; their references retire in command order. No batch
failure rewinds submitted GPU work or publishes a partially initialized aggregate.

VMA runs with internal synchronization enabled. Allocation handles and initial
state trackers are object-local until publication; allocation statistics and
heap snapshots use their existing mutex. Naming counters are atomic. Persistent
mapping does not grant concurrent access to the same byte range: mapped writes,
normalization/flush/invalidation, transfer arenas, state trackers and reclaim
remain governed by the existing recording/replay owner protocols. Native handles
and VMA allocations roll back immediately if post-create publication/naming
throws; ordinary final release continues through GPU-aware owner retirement.

Retained synchronous entries have explicit reasons: external/swapchain texture
views follow mutable replay-owned backing; viewport creation/resize follows
game-thread presentation ownership and replay ordering; GPU timing queries use
owner-managed pools; completed-resource collection polls retirement on its owner;
frame preparation and dynamic uniform/storage overflow reserve producer pages
through the owner; native command-buffer integration, reads and idle waits need
ordered context access. These entries are not arbitrary-thread factories. The
universal create-and-wait wrapper remains removed.

## Internal Pipeline Compilation

`FDynamicRHI` owns one `FRHIPipelineStateCache`. Cache acquisition and precaching
copy immutable descriptions, names, and strong shader/declaration references
before returning. Public drawing and preparation do not submit raw creation
requests. Backend hooks supply native lookup and creation callbacks through
`FRHIPipelineCompileBackend`; creation callbacks call the native manager directly,
never the public cache or a synchronous compatibility factory.

The cache owns a private `FPipelineCompileQueue`, with one worker-only Core scope,
a bounded queue, and one active native creator. Limits across graphics and compute
are 256 unfinished unique requests, 4,096 live internal observations, 16 MiB of
conservatively charged unfinished descriptions, and 1 MiB per description.
The queue retains shared in-flight work and independently cancelable observations
as an implementation detail. No public request/batch admission bypasses the cache,
and no per-item semaphore-waiting worker is launched.

Core scope rules forbid constructing completion sources into a foreign scope
from an executing task. Cache acquisition handles ordinary scoped callers through
its admission handoff below; the private queue itself rejects foreign-scope
admission before Core construction. Pending native waits reject Core workers,
RHI replay, and the creator itself. Core is never started implicitly.

Internal completion edges observe terminal notification and carry no native
resource references. Candidate Failed is an ordinary completed notification;
consumers inspect the cached pipeline state before drawing or dispatching.
Native access returns complete RHI references only for Ready. Device/invariant
exceptions publish executor failure and wake serial waiters before failing the
Core creator; native/error access preserves the original terminal exception.

The Vulkan CPU metadata budget is 64 MiB shared by PSO keys/results, structural
layouts, descriptor-set layouts, and render passes. Reservations charge dynamic
container capacities and conservative object/node overhead before native
creation. Their ownership follows the retained result until deletion, including
evicted objects awaiting owner retirement. This is logical cache accounting,
not an allocator/driver memory measurement. Resident count limits remain 2,048
PSOs of each type, 256 structural layouts, 1,024 render passes, and 4,096
descriptor-set layouts. A full budget rejects recoverably.

`RHIExit` closes request admission and joins the device-owned Core scope before
release delegates, replay shutdown, or native cache persistence/destruction.
Queued observations cancel; in-flight native creation is joined and cannot
publish into a closed device generation. Old request/completion handles may
survive shutdown. Close/join first preserves Ready publications for already
accepted command consumers; after replay drains, result retirement releases
those RHI references and a second retirement flush precedes native teardown.
Immutable request layout aliases remain valid after backend module unload;
their metadata budget and deleter are owned by RHI, not the Vulkan module.
Callers must still release explicit RHI references copied out of a Ready result
before device destruction. The owning shutdown thread performs close/join;
creation callbacks must never attempt to close their own compile queue.

## Pipeline Cache And Binding

`PipelineStateCache.h` separates typed `FGraphicsPipelineState` and
`FComputePipelineState` cache identities from complete native RHI PSOs.
`PipelineStateCache::GetAndOrCreate*PipelineState` and `Precache*PipelineState`
return the same shared identity for an equal normalized key while an owner or
recorded command retains it. Names are diagnostic only. Admission returns an
`expected` identity or an explicit request rejection; accepted identities expose
Pending/Ready/Failed/Canceled, immutable layout metadata, terminal readiness,
permitted waits, and native failure details. Raw Core completion handles remain
internal because they carry cancellation authority. A failed identity can be
retried by a later cache acquisition without changing an existing owner's result.

The device-owned PipelineStateCache has a weak table capped at 4,096 entries. Keys and
identity metadata are charged against the existing shared 64 MiB metadata budget;
individual cache metadata payloads are capped at 1 MiB. Expired identities are
reclaimed at entry capacity or metadata pressure. The Vulkan cache continues to own complete native reuse.
Cache acquisition rejects independent CPU leaves. Ordinary foreign-scope tasks
can acquire cold identities: one lazily started admission thread per device
performs a serialized metadata handoff outside the caller's Core scope. The
caller waits only for admission, never native compilation. The admission thread
queues existing Core creator work and never waits for replay, GPU work, or PSO
completion. Closing cache admission joins this thread before closing the creation
scope. Cache closure then joins its private queue; native results retire through
the existing two-phase shutdown boundary.

Drawing uses `SetGraphicsPipelineState(Commands, Initializer, Name)` or its compute
counterpart. These helpers acquire the cache identity and record its binding and
completion dependency automatically. Admission rejection is a contract error;
resource preparation should use the `expected` cache/precache API to apply
retry or fallback policy. Without Core, binding helpers use the complete-or-null
synchronous native factories, while cache/precache acquisition returns
Unsupported. Command-list overloads accept typed cache references or complete
native RHI objects, never creation requests. A Pending identity is not an
unfinished native resource. Recording never waits for native compilation.

## Transactional Resource Slots

With Core running, production fixed graphics/compute and static-mesh PSO
factories use `FRenderPipelineRequestScope` inside their owning slot. Logical
resource preparation before consuming passes is the prewarm trigger. A slot
retains normalized shared cache identities across attempts (at most 256 keys and
1 MiB of key storage), instead of creating a new observer every frame. Precache
and drawing share the same cache; resetting a slot releases its observation
without canceling creation retained by another slot or command. Pending and
global admission pressure retry on later preparation without a failure
diagnostic or a synchronous wait in the factory. Bare preparation callers see
unavailable on every Pending request, including same-device replacement. Scene submission provides an explicit first-consumer boundary: it
collects required Pending observations within each resource phase and joins them
at `ResolvePipelineStage_RenderThread`. Only that phase is revisited to publish
completed slot candidates; scene collection and logical preparation are retained.
The batch returns Empty, Ready, Failed, WaitUnavailable or CapacityExceeded, rather
than a boolean that conflates an empty batch with a failed wait. All admitted
requests are joined even if one fails. Terminal failures are resolved through the
owning slots so their diagnostics and retry policy remain authoritative.
Each phase admits at most 4,096 joined observations, including dependent request
waves; overflow is explicit. A consumed graph is never replayed. Replacements join the same consumption batch
as initial creation. Multi-PSO groups publish only when all members are Ready.
A slot invalidates its previous payload before attempting the requested generation;
neither Pending nor Failed returns an earlier shader/PSO/binding version.
Generation changes release obsolete cache identities. Late results may populate
the backend cache but cannot replace the current slot. Startup without Core keeps explicit synchronous
compatibility. Shader preparation itself is not made asynchronous by this helper.

Recorded command lists retain typed cache identities and their internal creation
dependencies. Pending identities expose immutable layout metadata, never
unfinished native handles.
Submission retains up to 256 distinct observer dependencies per group. The
threaded queue preserves FIFO serial order and wakes from Core completion; its
replay thread does not wait inside a command. A failed or canceled dependency
skips the whole batch and terminates its CPU fence with the corresponding state,
while later independent batches can proceed. Inline submission waits before
replay only on permitted callers; rejection preserves commands for retry.
Neither creator work nor its dependencies may depend on replay. Compute stage
and binding validation remains active; a pipeline becoming Ready does not make
missing bindings valid.

`FRHICommandListFence` reports Pending, Succeeded, Failed, Canceled, or Expired.
Completed serials describe terminal CPU queue progress, not successful native
execution or GPU completion. Captured fences retain their batch outcome; render
fences capture that receipt on the rendering thread. Late serial lookup uses a
fixed 4,096-entry history and returns Expired when a needed receipt has been
overwritten, never an inferred success. `TryWait` returns false for unsuccessful
outcomes; `Wait` requires success. Queue payload accounting reserves dependency
wake storage, and rejected/retired groups cancel their unused wake tasks.

Fixed Renderer resources, static-mesh shader and pipeline identities, editor
assistance, shared fullscreen geometry, and Texture Editor previews use
`TRenderResourceCreationSlot`. A slot constructs a complete candidate in local
ownership and publishes only after every binding, RHI resource, and pipeline
succeeds. Callers observe a complete payload only in Ready; Pending and Failed return null.
Uninitialized denotes a slot that has not been requested. The availability query
distinguishes deferred creation from terminal failure; partially initialized
aggregates are never visible.

Fixed non-Material shader compilation and typed lookup are centralized in
RenderCore's [Global Shader](GlobalShaders.md) map. Each bounded exact shader
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
buffers grow to bounded power-of-two capacities; a failed allocation publishes
no partial batch, reports once for the current device generation, and becomes
eligible again on the next frame. Device invalidation releases pipelines,
declarations, atlas resources, and upload buffers before lazy reconstruction;
shutdown performs the same ordered release.

Each owner tracks independent shader, device, and manual generations. A failed
attempt records its generation, error category/reason, owned context and identity, typed cause,
and retry dependencies. Repeated lookup in the same relevant
generation neither calls the factory nor logs the same failure again. A later
relevant generation permits one new lazy attempt.

`FRenderResourceCreateError` owns a mutually exclusive Shader or RHI cause.
`FormatRenderResourceCreateError` is the presentation boundary; factories and
slots do not store formatted diagnostics. Nullable backend interfaces retain an
explicit `BackendReturnedNull` cause when no richer error is available. Pipeline
request scopes retain the original asynchronous `FRHICreationError` and attach
it to the failed pipeline slot before publication. Material binding validation
formats its own typed Material error directly at its immediate logging boundary.

Failure fingerprints include category, reason, owner/identity, selected nested
semantic context and retry dependencies. They exclude
attempt generations and opaque external diagnostics, so changing compiler prose
does not change failure identity. Shader paths, native codes, limits, binding
locations, and numeric conflict ranges remain owned through producer teardown.

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
Device or manual invalidation and bounded transient-failure retries make later
construction eligible without moving shaders, PSOs, samplers, or committed
view history into transient ownership.

The graph frame executor derives one immutable requirements value after logical
preparation and persistent-resource resolution, then the single-use graph builder sends
one retained descriptor batch to `FRDGAllocator` before the first consuming
pass. The pool key contains the complete allocation-compatible descriptor and
excludes diagnostic names, observation tags, and feature identity. Texture and
buffer requests share whole-batch reservation, retry admission, pre-allocation
eviction, rollback, publication, and error reporting; only typed RHI creation
differs. Nullable texture and buffer factories also expose typed candidate
failures through the optional error output of `RHICreateTexture` and
`RHICreateBuffer`. Ordinary callers only inspect the nullable resource. Vulkan
preserves memory exhaustion, resource exhaustion, and unsupported-description categories
across both inline and threaded creation boundaries. Device loss and invariant
failures remain terminal; no retry policy catches them.

A failed batch rolls back every newly materialized candidate, including a retry
of an existing empty entry, drops unreserved idle cache, and publishes no graph
allocation or extraction destination. Before creating any candidate, the whole
batch is checked for suppressed descriptors, so a late unavailable resource
cannot repeatedly allocate and roll back an earlier prefix.

Unsupported descriptors remain suppressed until device or manual generation
changes. Memory/resource exhaustion and unclassified nullable failures retry
on later demand after 100 ms exponential backoff capped at 2 seconds, without
a maximum attempt count. A pool-wide cooldown also throttles new descriptors
and other views under pressure; reuse-only batches remain eligible. Device or
manual generation changes bypass cooldown, while shader changes do not.
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

Renderer owns these development commands:

- `renderer.reload-shaders changed` advances shader generation and lets normal
  dependency fingerprints select changed output on next demand.
- `renderer.reload-shaders all` advances shader generation and forces
  compilation for each next-demanded shader candidate.
- `renderer.retry-resources` advances manual generation for eligible failed
  resources.

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

The opt-in Vulkan creation qualifier records synchronous facade calls and
background native creation separately. A creator owns its timing record on its
own thread; it never borrows a caller's TLS record across asynchronous work.
Ready synchronous cache hits have a caller body and no scheduling interval.
Async observer admission, creator work, first-consumer waits, and an independent
replay marker have separate measurements. Sequential caller observation of Ready
is not the publication timestamp. Creator duration excludes the compile queue;
zero creator queue fields do not establish zero observer queue latency.

The selected background configuration has one native creator. Concurrent
producers can share one normalized key while replay progresses, but background
execution does not promise lower native driver latency or eliminate first use
waits. Compare median and p95 using identical input bytes, cache policy, host
configuration, and process lifecycle order. Process private peaks include driver
and allocator retention across device lifetimes; the bounded RHI metadata budget
does not bound them. Preserve absolute timing differences and measurement
uncertainty instead of treating microsecond percentage drift as a functional gate.

## Related Documentation

- [Viewport Rendering](ViewportRendering.md)
- [HDR Scene Color and Display Mapping](HDRSceneColorAndDisplayMapping.md)
- [Renderer Frame Preparation and Render Graph Execution](RendererFramePreparation.md)
- [Runtime Lifecycle](../Core/RuntimeLifecycle.md)

Independent procedural capture and transient environment generations follow
[Sky Lighting](SkyLighting.md).
