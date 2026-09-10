# Renderer Resource Recovery

Summary: Define complete-or-null Renderer resource publication, generation-scoped retries, fallback retention, and device invalidation.

Modules: RenderCore, Renderer, RHI, VulkanRHI, TextureEditor

Last reviewed: 2026-09-09

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

## Asynchronous Pipeline Requests

`RHIRequestGraphicsPipelineState` and `RHIRequestComputePipelineState` copy
immutable descriptions, names, and strong shader/declaration references before
returning. Requests distinguish rejection from accepted Pending, Ready, Failed,
and Canceled states. Ready/Failed publications are shared by observers of one
normalized key. An ordinary candidate failure is retryable by a later request;
no Failed cache entry permanently suppresses that key. Batch calls admit at most
256 items and preserve per-item results in input order, including mixed outcomes.
The caller retains raw-pointer inputs until the call has copied them.

Each device owns one worker-only Core scope, bounded queue, and active native
creator. Limits across graphics and compute are 256 unfinished unique requests,
4,096 live observers, 16 MiB of conservatively charged unfinished descriptions,
and 1 MiB per description. A same-key observer has independent cancellation;
canceling it does not cancel creation needed by other observers or release a
still-live observer's budget. No per-item semaphore-waiting worker is launched.

Current Core scope rules forbid creating roots/completion sources into a foreign
scope from an executing task. Such request calls return Unsupported before Core
construction; this also applies to deferred tasks running on the game thread.
Issue requests from the resource owner's ordinary thread before scoped work.
Core workers can consume already-issued handles. A pending synchronous wait
rejects Core workers, RHI replay, and the creator itself; it does not occupy the
only worker needed to complete creation. Other caller threads use the service's
completion notification because this device scope admits no deferred/game-thread
or replay dependencies. Core is never started implicitly by a request.

`GetCompletion()` observes terminal notification only, and carries no native
resource references. Candidate Failed is an ordinary completed notification;
consumers must inspect the request's domain state before drawing or dispatching.
`GetResult()` returns complete RHI references only for Ready. Device/invariant
exceptions first publish executor failure and wake serial waiters, then fail the
Core creator; `GetResult()` rethrows the original terminal exception instead of
turning it into a recoverable null result.

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
creation callbacks must never attempt to close their own service.

## Transactional Resource Slots

With Core running, production fixed graphics/compute and static-mesh PSO
factories use `FRenderPipelineRequestScope` inside their owning slot. Logical
resource preparation before consuming passes is the prewarm trigger. A slot
retains normalized request identities across attempts (at most 256 keys and
1 MiB of key storage), instead of creating a new observer every frame. Pending
and global admission pressure retry on later preparation without a failure
diagnostic or a synchronous wait in the factory. Bare preparation callers see
unavailable on a first-use miss; a same-device refresh retains the prior complete
payload. Scene submission provides an explicit first-consumer boundary: it
collects required Pending observations from a preparation attempt, joins that
batch on the render owner, then prepares again before authoring any Render
Graph. There are at most 4,096 collected observations and 64 preparation rounds;
admission pressure without progress returns RendererResourcesUnavailable.
Compatible refreshes are excluded from that join. This preserves single-shot
capture and first-frame correctness while native creation stays off replay;
it does not promise zero first-consumer latency. GBuffer, deferred lighting,
GTAO, debug, and cloud temporal/composite PSOs prepare before graph execution.
Multi-PSO groups publish only when all members are
Ready. Generation changes cancel obsolete observations, and device changes
discard the old payload. Late results may populate the backend cache but cannot
replace the current slot. Startup without Core keeps explicit synchronous
compatibility. Shader preparation itself is not made asynchronous by this helper.

Recorded command lists can also retain pipeline requests directly. Pending
requests expose immutable layout metadata, never unfinished native handles.
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
succeeds. Callers observe the prior complete payload, a newly committed payload,
or no payload; partially initialized aggregates are never visible.

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
attempt records its generation, error category, context, identity, diagnostic,
retry dependencies, and fallback state. Repeated lookup in the same relevant
generation neither calls the factory nor logs the same failure again. A later
relevant generation permits one new lazy attempt.

Same-device shader or manual refresh may retain a complete last-known-good
payload as stale-ready. Device-generation changes always discard dependent RHI
payloads before replacement, so fallback never crosses a device generation.
This seam coordinates reconstruction; it does not recover a lost Vulkan device
or a failed RHI executor.

Compiled materials retain one immutable accepted `FMaterialCompilerResult` in
Engine render data. The Renderer adapter verifies its identity, target, pass
contract, and generated entry point before combining accepted fragments with
fixed mesh stages in one exact typed candidate. RenderCore validates reflection,
bindings, set identity, and merged layout. Shader reload may refresh a fixed
mesh stage; device invalidation discards combined RHI shaders and PSOs. Both
reconstruct lazily without rereading graph state or recompiling generated
Material IR. Failed same-device candidates retain the compatible complete map
and pipeline; device-generation changes permit no RHI fallback.

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
failures through `RHITryCreateTexture` and `RHITryCreateBuffer`. Vulkan preserves
memory exhaustion, resource exhaustion, and unsupported-description categories
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
New failures and changed fingerprints produce one diagnostic, retained fallback
is identified explicitly, and successful retry reports one recovery transition.

`FRendererResourceCoordinator` owns command admission and the shader, device,
and manual generation counters. It explicitly supplies accepted generations
to the RenderCore global map while `FSceneRenderer` fans requests out to its
remaining concrete owners. Shader and manual invalidation leave
reconstruction lazy. Device invalidation releases every dependent payload
before advancing the device generation, recreates only startup defaults, and
leaves feature resources to rebuild on demand.

`FRendererModule` is the explicit cross-module request and focused-test entry
point. It forwards only while its composed `FSceneRenderer` exists; shutdown
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
is not the publication timestamp. Creator duration excludes the service queue;
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
