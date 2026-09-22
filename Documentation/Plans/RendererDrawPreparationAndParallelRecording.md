# Renderer Draw Preparation and Parallel Recording Plan

Summary: Replace repeated per-view mesh interpretation with persistent draw records, prepared binding batches, and task-based CPU recording while preserving RDG resource and submission contracts.

Last reviewed: 2026-09-23

Status: Completed
Completed: 2026-09-23

## Current Status

Completed at the user's explicit request on 2026-09-23 after implementation,
functional qualification and the Sandbox default-level before/after comparison.
Stages 1–5 implementation checklists, Stage 6 correctness/consumer handoff and
Stage 0 ownership contracts are complete. The user accepted this handoff and
requested closing the plan; the broader baseline coverage, numeric budgets and
performance portions of the original gates are deferred follow-up, not completion
blockers. They have not been marked as measured or passed. The default Sandbox
diagnostic comparison below does not establish full performance acceptance.
The source audit follows `f3c835e51`, which introduced shared preparation facts,
dense uniform groups and separate view/material descriptor sets. The user
reports no obvious reduction in render-thread time. This is qualitative feedback,
not a controlled benchmark. That commit's correctness results do not establish
the performance baseline or acceptance of this plan.

On 2026-09-22 the user explicitly deferred performance testing to their own
follow-up and requested refactoring first. This overrides the requirement to
freeze measured budgets before implementation, but does not close any performance
gate. Baseline captures, numeric budgets and performance acceptance remain pending.
Correctness and ownership validation remain required during implementation.

On 2026-09-23 the user requested a before/after measurement on Sandbox's default
level, superseding that deferral for this workload. Both `904d42602` (the parent
of the first implementation commit `49c5273f0`) and `df7b7c9d0` built successfully
with `Win64-Release-DurinEditor`. Engine/Sandbox content hashes matched. The
`/Game/Levels/GrayboxStage15` default camera and settings were used, with matching
3840x2064 FIFO swapchains and 2916x1908 SceneViewport panels. Three alternating
process runs per revision warmed for 40 seconds and captured 20 seconds of Tracy;
analysis discarded two seconds from each end and retained complete CPU zones.

The median of the three per-run `Renderer.RenderViewSubmission` medians was
1.991 ms before and 1.271 ms after (-36.2%, 0.720 ms saved in this sample).
Per-run before medians were 2.021/1.368/1.991 ms, versus 1.521/0.893/1.271 ms
after; corresponding p95 values were 2.724/2.966/3.627 ms versus
2.381/2.004/2.257 ms. Frame intervals remained approximately 16.7 ms / 60 FPS.
Shadow preparation median-of-medians changed from 0.309 to 0.150 ms,
`RDG.Compile` from 0.351 to 0.215 ms, and `RDG.Record` from 0.569 to 0.315 ms.
These are inclusive elapsed CPU zones, not independently measured CPU busy time.

This fixture has three opaque receiver draws and 5/4/0 cascade draws, below the
automatic worker thresholds; it does not qualify parallel speedup. Graph
declarations changed from 11 passes / 32 resources to 14 / 35. Run variation is
large, CPU affinity/frequency were not controlled, and external desktop GPU
activity was not excluded. Timings are diagnostic only; no frozen budget or
performance checklist is closed. All runs were serial with no competing build.
The first after capture completed before a window-close timeout required stopping
its process; subsequent fixed-tick runs exited normally and confirmed the same
direction. GPU time, allocation counts and retained-memory budgets remain
unqualified. The revision range includes an independent collision refactor, so
these measurements do not isolate individual commits' contributions.

Detailed methods, per-run statistics, host counters, scripts, CSVs and six native
Tracy captures remain under `Build/Profiling/RendererPlanComparison/`, with
`report.md` and `summary.json` as the entrypoints. This records the user's actual
default-level comparison while preserving the broader Stage 0/6 gates.

The first change establishes immutable material publication records. Each accepted
representation owns a counted immutable payload and a process-unique nonzero
revision ID; copies and prepared binding views retain that payload. Successful
publication assigns a new ID, while copies preserve it. The terminal error record
is shared. Failed publication returns the error record and its existing diagnostic.
Record lifetime ends after the last asset/proxy, prepared view or binding releases
it; there is no global record registry or historical cache. Retained storage is
bounded by live published revisions and their consumers. Texture-reference contents
are still resolved on the render thread under existing device/resource-generation
rules; a record ID does not certify backend texture readiness.

Stage 1 now uses immutable material publications and validated generic geometry
publications. `FMeshGeometryRecord::Publish` consumes exclusive input-binding
ownership and validates draw/stream ranges, resource usage, unique element IDs,
instance inputs and declaration compatibility. Static-mesh resource initialization
publishes one record per LOD; unchanged ready initialization preserves its ID.
Collection reads geometry spans without per-section geometry copies or repeated
structural/input validation. Dynamic providers retain their checked path;
independent factories can explicitly opt into the same publication API.

Spline proxies retain at most one publication per LOD and accepted deformation
state. A changed binding gets a new record ID, shares the immutable geometry span
and geometry ID, and validates the new inputs once. Material changes do not rebuild
geometry. Geometry replacement/release withdraws current publications; retained
prepared frames keep old resources alive until their consumers release them.
Receiver/cascade LOD, transform, material selection, pass eligibility and sorting
remain per-view decisions. Storage is bounded by live asset LODs, proxy bindings
and consumers, without a global history/cache.

Material bindings, resolved uniforms and sort keys retain the material publication
rather than copying payload arrays. Sort keys preserve byte-wise uniform ordering
across independently published records and use ID equality for repeated records.
Shader/pipeline generations remain resolved through the existing resource
coordinator; geometry records contain no shader/pipeline payload to invalidate on
shader reload. Buffer replacement requires a new geometry publication. The tested
device-generation seam preserves the existing resource-recovery contract and is
not a claim of actual Vulkan device-loss recovery.

Validation: the final runtime source passed `build --target all` on
`Win64-Debug-DurinEditor`; `test affected --report` passed all 96 selected targets.
Four geometry contract cases cover invalid ranges/inputs, duplicate IDs, mixed or
mismatched batch rejection, immutable snapshots, and shared storage after rebinding.
Vulkan preparation tests cover unchanged publication reuse, material changes,
spline revisions, independent factory dynamic/published equivalence, and zero
per-view input validation for published geometry. The affected selection includes
resource reload, editor consumers, material/cook coverage and Sandbox gameplay.
The final lifecycle-test extension also passed its isolated named-case rerun:
a complete prepared frame retains the old record and valid buffers across asset
resource release/reinitialization, and the record expires only after that frame
is released. Changed-document validation and `git diff --check` passed.
Local receipts are `Build/NativeTestResults/Win64-Debug-DurinEditor/affected.xml`
and the named `RendererSceneContractTests.xml` and
`StaticMeshRenderPreparationVulkanTests.xml` in that directory.

Stage 2 now compiles surface fragment binding layouts when shader instances are
created and prepares immutable concrete batches before recording. Receiver and
cascade draws reuse batches by material uniform group, pass and selected shader;
forward, retained-forward, GBuffer, masked shadow and hit proxy consume prepared
bindings. GBuffer draws retain their pipeline payload and resolved fragment RHI
shader instead of discovering pipelines during recording. Independent factories
and editor previews use these same surface paths.

The shared RHI parameter-batch API canonicalizes views during preparation and
retains the shader and canonical resources. Recorded commands share immutable
arrays rather than canonicalizing resources and copying arrays on every bind.
Its command payload accounting conservatively charges shared retained array
storage per command reference. Unsupported layouts and invalid required bindings
fail before pass recording; binding retries clear previously prepared payloads.

Stage 2 checkpoint validation: `build --target all` passed after the shared RHI
readonly-span migration. `test affected --report` passed all 33 selected targets,
including Vulkan preparation/hit proxy, GBuffer/editor grid, thumbnails, reload,
RHI command/thread/resource contracts and material consumers. Two added RHI
tests also passed independently; they cover repeated batch replay, retained
resource lifetime, caller-input mutation, missing resources and invalid ranges.
Two surface contract cases cover unsupported layouts and clearing failed output.
The affected receipt remains `Build/NativeTestResults/Win64-Debug-DurinEditor/affected.xml`.

The next Stage 2 checkpoint moves vertex-factory parameter preparation out of
recording. Built-in local/spline and independent dynamic factories now return
immutable RHI parameter batches. Receiver and cascade resolution shares a batch
per primitive and selected vertex shader; hit proxy prepares before its render
pass. Typed metadata mapping uses the existing shader-instance binding layout;
ordinary immediate binding and prepared binding share parameter extraction.
Spline/deformation uploads and typed parameter-vector construction no longer
occur in the mesh draw loop. A new view still prepares its own transform ranges.

Validation for this vertex checkpoint: `build --target all` passed, and
`test affected --report` passed all 95 selected targets. The independent factory
fixture asserts preparation occurs outside render passes across its existing
consumer tests. A new typed-parameter test verifies dynamic uniform arrays,
descriptor coordinates, preserved offsets, canonical ownership and independence
from caller data; it also passed an isolated named-case rerun. These results
close the binding-layout/payload checklist, not persistent-command or performance
acceptance. Receipts are the affected report and `RenderShaderContractTests.xml`
under `Build/NativeTestResults/Win64-Debug-DurinEditor`.

Stage 2 now retains immutable command templates for published geometry elements.
Templates own geometry/material publications, logical pipeline identity and
stable sort facts; per-view transforms, depths and uniform ranges stay external.
The renderer cache is shared across receiver and cascade preparation. Its key
tracks geometry/material record IDs, element and pass-policy variants; lookup
also checks material planning identity and compiled-program ownership. Entries
unused by the latest submission are removed, including on preparation failure.
Old prepared frames retain their selected templates; resource release clears the
owner cache. Providers without immutable geometry publications build fresh
templates on their dynamic path. Shader/device readiness remains independently
resolved through existing generation-aware resource preparation.

Template validation: `build --target all` and all 23 targets selected by
`test affected --report` passed. The Vulkan collection case verifies zero
template rebuilds across successive preparations of unchanged publications with
fresh submission-local caches, while a moved camera recomputes translucent depth.
A cache contract case covers changed key fields, changed material pipeline
policy, eviction and retained-consumer lifetime. Both cases also passed alone;
receipts are the affected report and named `RendererSceneContractTests.xml` /
`StaticMeshRenderPreparationVulkanTests.xml` in the configured results directory.

Visible draw records now contain compact references with a compile-time limit of
128 bytes. Geometry, material, pipeline and layout-sort arrays live only in their
immutable templates. Visible and combined-translucent keys retain aliases to that
storage and preserve the original primitive/batch/LOD/section tie order. Cascade
raster-bias magnitudes are separate per-view values, applied as dynamic draw state
without entering pipeline-cache identities or changing shared templates.

Compact-reference validation: the final `all` build and all 23 affected targets
passed. The new ordering contract test compares compact and original full-key
ordering across state and tie-breaker changes, verifies shared storage lifetime,
and passed alone. Existing transparent ordering, independent dynamic/published
equivalence, reload, hit proxy and GBuffer consumers passed the affected selection.
Receipts are `affected.xml` and `RendererSceneContractTests.xml` under the configured
native-test results directory. This structural storage reduction is not a measured
frame-time improvement.

Adjacent recording now reuses a complete pipeline/vertex/fragment binding group.
Changing any member rebinds the complete group; failures reset it. Each uninterrupted
mesh span starts with empty state, including new passes and the transition from
non-mesh work. Forward, retained-forward, sorted translucency, GBuffer and shadow
cascades use this path without changing draw order. Hit-proxy IDs remain distinct
prepared bindings. Geometry commands and dynamic raster bias remain per draw.

Final Stage 2 functional validation: `build --target all` passed and
`test affected --report` passed all 23 selected targets. The binding-group contract
test covers complete-group reuse, changing any component, failure after partial
binding, new-pass reset and opaque-shadow null fragment state. It also passed alone.
Source audit confirms material resolution, shader reflection interpretation and
GBuffer pipeline discovery occur in preparation, with draw execution consuming
retained references. The cumulative tests cover forward, retained-forward, GBuffer,
shadow, hit proxy, preview and independent geometry paths. Native receipts remain
the affected and named renderer contract reports in the configured results directory.

Stage 3 has an initial graphics descriptor-state reuse checkpoint. Descriptor
ordering becomes dirty only on binding insertion. A selected bounded snapshot
index bypasses hashing and lookup while descriptor identity is unchanged;
resource/type/size/static-offset changes invalidate it. Cache clear and eviction
compaction invalidate the index. Resource-owner arrays rebuild only when resources
change, so offset-only updates retain the existing ownership. Draw-time ordered
binding, dynamic-offset and texture-access validation remain enabled.

Checkpoint validation: `build --target all`, the isolated public-RHI Vulkan
conformance case, and all 8 affected targets passed. The conformance workload
checks pixel equality across its execution modes, repeated unchanged parameter
submission and 512 changed snapshots with eviction. Test-only work counters show
1 sort, 513 hashes and 2 resource-owner rebuilds for 514 draws; all 1542 binding
validation visits remain. These are structural work counts, not timing acceptance.
Receipts are `VulkanRHIIntegrationTests.xml` and `affected.xml` in the configured
native-test results directory. Per-set state and separating structural from
draw-dependent validation remain open.

Dynamic-offset qualification now passes in both inline and threaded RHI modes
with Vulkan validation enabled. The shader reads a two-element dynamic-uniform
array in sparse set 2, binding 5; parameters arrive in reverse array order.
Changing only the first element's offset changes the right-side pixels from
red/green to blue/green while preserving the left-side output. Three draws,
including a repeated unchanged batch, perform exactly one descriptor sort, one
hash and one owner-array build, while all six binding validation visits remain.
This verifies offset ordering and selected-snapshot reuse against actual shader
output. The isolated case and all 95 affected targets passed; the broader
selection is caused by the shared native-test data change. No runtime code changed
in this qualification checkpoint. Receipts are the named Vulkan integration
report and `affected.xml` under the configured native-test results directory.

Graphics descriptor validation is now split by lifetime. Binding insertion or
changed descriptor identity invalidates structural validation; the next draw
checks the complete layout and canonical view kinds/usages and builds a compact
ordered list of dynamic-uniform and image indices. Unchanged state reuses that
list. Each draw still checks uniform alignment and buffer bounds and the current
texture-view access state, including before a selected-snapshot hit. The
`DURIN_VULKAN_FULL_DESCRIPTOR_VALIDATION=on` startup diagnostic repeats the full
structural walk per draw. PSO-local ownership and reset boundaries remain intact.

Validation: `build --target all` and all 8 affected targets passed. The sparse
array pixel case runs both diagnostic settings in both RHI execution modes:
three draws visit 2 structural elements normally or 6 in full-validation mode,
and always perform 6 draw-dependent checks. The changed-snapshot conformance
case performs 1539 structural visits and 514 texture-access visits across 514
draws, preserving cache capacity/eviction and pixels. Receipts are the Vulkan
integration and affected reports in the configured native-test results directory.
Per-set state and broader lifecycle qualification remain open; no timing
improvement is claimed.

Graphics descriptor snapshots now cache individual sets in the command context.
Each PSO retains independent pending parameters and weak per-set selections.
Changing material resources rebuilds only their set's ownership and descriptor;
common sets survive unchanged. Reuse across pipelines requires the same interned
structural layout handle and complete binding/array/resource equality, with
explicit full-sequence binding on every draw. Set indices and dynamic offsets
remain external to identity. Empty sparse sets participate in the same cache.
The context retains at most 512 set snapshots / 8192 values; eviction and frame
clear expire weak selections. Pipeline deletion drops pending state while bounded
compatible cache entries remain available to other pipelines.

The new public-RHI pixel case passes alone in inline and threaded modes with
Vulkan validation: material change preserves common state, compatible pipeline
change reuses all sets, and an incompatible common layout creates only that set.
Four draws create exactly five sets (common, empty, red, green, incompatible
common), retain four resource values, and record seven hits. The sparse dynamic
array case now hashes three set identities once rather than one aggregate
snapshot; offset updates still perform no additional hashes. All 95 affected
targets passed, including both new and existing conformance, cache eviction,
resource reload and renderer consumers. The wider selection follows the shared
shader-fixture change. The final `all` build and documentation validation are
recorded with this checkpoint. Receipts are `VulkanRHIIntegrationTests.xml` and
`affected.xml` under the configured native-test results directory. Broader chunk
boundary/lifecycle qualification and measured performance remain open.

Stage 4 now separates render-thread collection from logical draw preparation.
An immutable collected-view owner retains primitive identity/classification,
projection fallback and submission outcomes, admitted geometry/material batches,
and preparation-specific view matrices/policy. Replay does not dereference scene
info or proxies. The ordinary receiver, cascade and hit-proxy entrypoint composes
capture and replay; profiling retains the original overall preparation boundary.
Factory resolution and shared transform/material/command-cache access still run
on the render thread. This is an ownership prerequisite, not completion of the
worker-safe snapshot or task scheduling checklist.

The extended Vulkan collection case passes alone after destroying source
scene-info/proxy/view values and releasing/reinitializing asset geometry. Replay
preserves buckets, translucent depth, pipeline state and old geometry identity;
the retired publication expires after the final collected/prepared owner drops.
All 23 affected targets passed; the final `all` build includes the preserved
profiling boundary. Engine, Sandbox and RoadWeaver symbol searches found no
additional consumers requiring migration. Receipts are
`StaticMeshRenderPreparationVulkanTests.xml` and `affected.xml` in the configured
native-test results directory. Factory/cache freezing, visibility/LOD tasks,
bounded scheduling, deterministic merge and task-failure handling remain open.

Collection snapshots now also freeze canonical input bindings, factory/layout
compatibility, per-element preparation/GBuffer capability and dynamic input
validation results. Logical replay performs no registry lookup, factory virtual
capability query or repeated dynamic-geometry validation. Existing published
geometry bypasses dynamic checks. The independent multi-batch factory case passes
alone and asserts that capability-query counts stay unchanged during replay.
All 95 affected targets passed; the shared fixture header causes broad Engine
selection. The final `all` build and changed-document validation also passed.
Receipts are the named static-mesh Vulkan report and `affected.xml` in the
configured native-test results directory. Shared transform/material/command
caches are still resolved on the render thread.

The task-system audit found that ordinary task waits explicitly reject the
rendering thread (`TaskSystem.md`, dependencies/waiting policy). Scheduling must
therefore establish a documented, qualified render-preparation join or asynchronous
consumption contract; bypassing task waits with a private blocking primitive is
not the intended implementation. Worker-safe cache inputs, scheduling, merge and
failure handling remain open. This checkpoint does not close a Stage 4 checklist.

Shared-cache resolution now publishes immutable `FStaticMeshPreparationInputs`.
It owns the collected snapshot, copied transform facts, resolved material indices,
selected command-template references, material-table extent and template-work
attribution. `PrepareStaticMeshInputs` accepts no command list or shared cache;
it builds and sorts pass lists, assigns resolved/group indices and accumulates
only result-local telemetry. The production wrapper still calls it inline.

The independent-factory Vulkan case passes alone with two simultaneous worker
threads consuming the same input after source scene-info and both preparation
caches have been destroyed. Both produce the same ordered keys, command owners,
material groups, outcomes and triangle/template counts as inline consumption.
The geometry/publication lifetime case also passes alone. All 23 affected targets,
the final `all` build, changed-document validation and diff checks passed.
Consumer searches covered Engine, Sandbox and RoadWeaver. Receipts remain the
named static-mesh Vulkan and affected reports in the configured results directory.
This establishes the logical preparation boundary; production task dispatch,
visibility/LOD snapshots, chunk merge, render-thread join and failure policy are
still required before Stage 4 is complete.

Logical mesh preparation now exposes owned contiguous primitive chunks. Each
chunk retains the immutable inputs and has independent vectors, indices and
telemetry. Merge checks common input identity and complete non-overlapping range
coverage before consuming outputs, restores original primitive order, offsets
primitive indices, combines histograms/counters and then performs global sort,
dense index assignment and material grouping once. Inline preparation shares the
same range builder/finalizer. Empty scenes use one empty chunk; missing, duplicate,
invalid and mixed-input chunks return explicit errors without partial publication.

The independent-factory case now compares two concurrent full replays and a
reverse-arrival two-chunk merge against the inline result, including keys,
primitive/resolved/material indices, histogram and transition counts. Missing,
duplicate and mixed-source rejection plus empty input are covered. The named
case passed alone, all 23 affected targets passed, and the final `all` build,
changed-document validation and diff checks passed. Engine, Sandbox and
RoadWeaver consumer searches found no further API migrations. Receipts are the
named static-mesh Vulkan report and `affected.xml` in the configured results
directory. This closes
the chunk data/merge mechanism, not bounded production dispatch, visibility/LOD
partitioning, render-thread join or task failure/cancellation qualification.

The task system now has an explicit independent-CPU leaf contract through
`Tasks::LaunchIndependentTask`. Only these Worker leaves can be joined by the
rendering thread; ordinary task/scope waits retain their previous restrictions.
Admission rejects dependencies, external completion, other executors and child
submissions from a leaf. Dynamic dependency binding and blocking task/scope waits
are also rejected. Callables must not rely on owner-thread, RHI, I/O or external
progress; ordinary C++ effects remain their declared caller contract. Normal
cancellation, failure and success publication semantics are unchanged.

Two new Core cases pass in separate processes, covering queued render-thread
joins through success/failure/cancellation and rejection of dependencies,
external sources, wrong executors, children, dynamic edges and blocking waits.
The ordinary rendering-thread rejection case also passed alone, and all four
Core affected targets passed. This qualifies the join prerequisite; renderer
dispatch has not yet switched from inline execution. Shared-API searches covered
Engine, Sandbox and RoadWeaver, with no required consumer migrations. The final
`all` build passed across all workspace targets; changed-document validation and
diff checks passed. Receipts are `CoreConcurrencyTests.xml` and `affected.xml`
in the configured native-test results directory.

Production mesh preparation now dispatches immutable logical inputs in chunks of
128 primitives, with at most eight active tasks per view and a 256-primitive
automatic threshold. Small views and unavailable schedulers use the inline path.
The owning work object drains tasks on completion or abandonment; ordered failure
and cancellation discard partial results. Task count and join duration accompany
the merged view. `DURIN_MESH_PREPARATION=inline|tasks` supports path comparison.
The independent-factory Vulkan case exercises 1,536 primitives across twelve
tasks, including rolling-window refill, ordered command/index and counter
equivalence, pre-cancellation in both policies and an injected task failure.
The isolated case and all 23 affected targets passed.

Receiver tasks now start immediately after visibility collection and join at the
combined-translucent consumer, overlapping environment, light, shadow and cloud
preparation. All shadow cascades are dispatched before ordered consumption. The
frame holds at most 32 active mesh preparation tasks with three cascades. Cache
resolution stays serial and each dispatch retains a frozen input; later cache
growth cannot mutate it. Receiver failure keeps the required-output rejection,
while shadow failure uses the existing all-cascade readiness fallback.
LOD partitioning and additional lifecycle qualification remain open.
This checkpoint passed all 23 affected targets and the independent-factory Vulkan
case separately with forced task and inline policies. The latter also verifies
that abandoned work has drained every launched task before destruction returns.
The final `all` build, changed-document validation and diff checks passed.

Visibility now freezes bounds and visible flags before independent CPU
classification. The production path partitions at 512 candidates per task, with
eight active tasks and a 1,024-candidate threshold. Only the caller maps ordered
classifications back to scene pointers and accumulates counters. Pure computation
failure retries inline after discarding partial results and drains outstanding
tasks before return. A 6,144-candidate test exercises twelve chunks, mixed hidden,
invalid-bound and ordinary inputs, and invalid-view fallback; serial and task
classifications match. The isolated Vulkan case and all 23 affected targets
passed, including existing scene visibility conservation and hit-proxy coverage.
The final `all` build and changed-document validation passed. Provider-owned LOD
selection and collection still run on the rendering thread.

LOD selection now has a value-only snapshot of thresholds and readiness. The
resource and snapshot entrypoints share the original selection and availability
algorithms, preserving equality, invalid-input and lower-detail-first fallback
rules. This is the input boundary for worker LOD preparation; provider collection
has not yet switched to it. The snapshot does not hold geometry resources or
authorize concurrent mutation of the source during capture.
The LOD golden case passed alone in `EditorRenderingTests`, covering snapshot
equivalence and stability after readiness changes. All 49 affected targets and
the final `all` build passed. Engine, Sandbox and RoadWeaver source/test searches
found no additional consumer migrations. Changed-document validation and diff
checks passed; provider task integration remains outstanding.

Static and spline mesh providers now opt into LOD snapshot capture. Renderer
captures bounds and optional LOD inputs before workers compute projected size
and requested/available LODs. Work is bounded to eight 128-primitive chunks per
wave and uses an inline path below 256 primitives. Ordered facts feed collection;
material resolution, spline binding updates and current geometry readiness checks
remain render-thread operations. Independent providers may retain their existing
selection without implementing the optional snapshot interface. A 1,536-object
test compares twelve task chunks with inline facts, including forced LOD0,
availability fallback, invalid bounds and providers without LOD snapshots. The
isolated Vulkan case and all 50 affected targets passed. Shared API searches
covered Engine, Sandbox and RoadWeaver; existing providers retain source-compatible
defaults. Stage 4 implementation checklists are now closed; its performance gate
remains pending under the user's deferred-measurement instruction.
The final `all` build, changed-document validation and diff checks passed.

Stages 3–6 remain open; hit proxy builds templates through its dynamic
submission-local path. Further worker preparation integration and owned RDG
recording chunks still require implementation.
The binding checkpoints do not close the broader Stage 2
gate. Functional results do not close Stage 0 or any performance
gate; no performance improvement is claimed.

RDG now exposes an owned-list `AddRecordingPass` callback alongside existing
immediate callbacks. The graph seals and queues a successful complete pass list
in compiled order; barriers and submission assembly remain on the immediate
timeline. Two isolated cases verify mixed-callback typed dependency/replay order
after builder destruction and release of a failed callback's unpublished commands.
This is the recording-interface prerequisite, not parallel dispatch or whole-graph
transactional failure handling. Production callback migration, CPU dependency
scheduling and parallel qualification remain open in Stage 5.
All seven affected targets passed with serial test scheduling. The first parallel
affected run passed six targets but Vulkan integration crashed during existing
creation-failure injection; its standalone rerun passed all 103 cases, and the
serial affected rerun also passed. The intermittent crash remains unexplained;
the passing reruns do not establish its root cause. The original failure receipt
is `Build/.agent-state/logs/20260922-232631-990468-57428-ctest.log`.
The final `all` build and changed-document validation passed. Shared callback API
searches covered Engine, Sandbox and RoadWeaver; no existing caller migration was
required by this additive entrypoint.

RDG recording now accepts an explicit independent-CPU policy. Consecutive eligible
passes without a compiled dependency between them record in waves of at most
eight; all dependency kinds conservatively delimit waves. The single-pass and
scheduler-unavailable cases run inline. Ordered join requires whole-wave success
before queuing any list; failure/cancellation discards that wave and drains its
tasks. Serial callbacks preserve their existing order. Two isolated tests pass:
workers produce a typed value before its dependent consumer while replay remains
ordered, and an injected failure prevents any command in its wave from publishing.
Production migration, wider failure transactions and parallel GPU qualification
remain open; this does not close Stage 5.
All seven affected targets passed with serial test scheduling, including Vulkan
integration. Shared API searches covered Engine, Sandbox and RoadWeaver; existing
recording calls keep the default serial policy. The earlier intermittent Vulkan
failure did not recur in this validation run.
The final `all` build, changed-document validation and diff checks passed.

Production shadow drawing now separates pure recording from observation writes.
Regular command lists consume const prepared/resolved geometry and return local
counts. Complete cascade recording retains exact target/attachment views and
opens/closes its own render pass without consulting renderer state. The existing
directional-shadow path consumes these functions and merges observations on the
rendering thread. The independent-geometry Vulkan case passed with this path.
Splitting the RDG shadow callback into cascade tasks and an ordered observation
consumer remains the next integration step.
All 23 affected Renderer test targets and the final `all` build passed for this
checkpoint. Changed-document validation and diff checks also passed. Shared API
consumer searches covered Engine, Sandbox and RoadWeaver. Worker execution of
the new cascade entrypoint remains unqualified until the RDG integration lands.

RDG now records each production shadow cascade on an owned list with an exact
single-layer attachment declaration. Typed observations retain all producers and
order the serial telemetry/capture consumer after them. The automatic policy
uses workers for at least 256 total cascade draws; smaller work records serially.
The immediate path remains available for spanning GPU timing queries and the
diagnostic override. Each list selects its graphics pipeline and closes its own
render pass, with no shared-cache or global telemetry writes on workers.
The Vulkan shadow baseline passes, including added immediate/owned-serial/worker
three-cascade fixtures with byte-identical final images and three observed worker
recordings in the parallel case. Its graph capture verifies exact, non-overlapping
layer coverage. The independent-factory Vulkan image case also passed after the
production path migration. Full native depth-layer comparison and delayed replay
qualification remain outstanding; no performance acceptance is claimed.
The cloud integration exposed its old single-shadow-pass structural baseline.
Three cascade passes plus their typed-result consumer add three passes, eight
dependency edges and two physical entry-barrier calls; transitioned subresource
counts are unchanged. Update the structural regression sentinels by those exact
deltas, while retaining all compile/execute timing and performance acceptance
budgets. This reflects the selected pass partition rather than a timing allowance.
Validation: 22 of 23 affected targets passed on the first run; the cloud target
failed only its old structural counts. Its failing case passed after updating
those expectations and the documented structural sentinels. The shadow baseline
qualification target, final `all` build and changed-document validation passed.
Shared API consumer searches covered all Engine, Sandbox and RoadWeaver source
and test roots. Timing samples from these correctness runs are not acceptance
evidence. Remaining work includes descriptor lifecycle qualification, recording
failure/delayed-replay transactions and the Stage 6 handoff audit.

Descriptor lifecycle qualification reproduced a stale-cache failure: an owned
command chunk recorded before pool retirement reused the earlier descriptor set
and failed the active-allocation-owner assertion on replay. Pool retirement now
increments a generation; graphics and compute contexts clear stale snapshots
before materializing their next bindings. Pending values/offsets remain intact.
The expanded sparse-set graphics test records three complete chunks before two
retirements, verifies red/green/red readback and exactly three fresh descriptors
per new generation in inline and threaded modes. Compute readback also passes
after retirement without a `BeginFrame` cache clear. The original failing receipt
is `Build/.agent-state/logs/20260923-001145-958793-42388-VulkanRHIIntegrationTests.log`.
All eight affected targets passed with serial test scheduling, including the
complete Vulkan integration target (sparse dynamic-offset arrays in both
validation modes, pool ownership/completion and repeated device lifetimes).
The missing-optional-reflection binding contract passed independently in
`RenderShaderContractTests`; optimized-out fields produce no backend binding.
Together with the new frozen-chunk retirement tests, this closes Stage 3's
functional lifecycle checklist. The final `all` build, changed-document
validation and diff checks passed. CPU/performance acceptance remains deferred.

RDG failure/lifetime qualification now covers 19 independent recorders spanning
three waves: their payload owners outlive graph destruction, replay in declaration
order and release afterward. Failures at both ends of each of two eight-pass
waves drain unpublished owners, withhold extraction and later callbacks, retain
only already accepted work, and reject re-execution without duplicate commands.
Task-handle storage is reserved before launch to avoid orphaning a callback if
vector allocation fails. Both new tests pass individually. This closes Stage 5's
functional publication/failure checklist; full native depth-layer comparison and
the broader Stage 6 correctness/handoff audit still remain, as do deferred
performance gates.
All seven affected targets passed with serial scheduling, including Vulkan
multi-queue integration. The final `all` build, changed-document validation and
diff checks passed for this checkpoint.

Stage 6 now compares every native shadow depth layer between immediate,
owned-serial and worker recording. The shared depth readback fixture selects an
exact array-layer view; all three 2048-square layers must contain finite in-range
depth and non-clear coverage. Added far casters include negative scale and masked
material. Depth bytes and final color bytes match across all three policies, with
three actual worker chunks observed for parallel recording. The complete shadow
qualification target passes. The obsolete `ExecuteShadow_RenderThread` wrapper
and unused mesh-renderer arguments have been removed after consumer searches
across Engine, Sandbox and RoadWeaver; the immediate diagnostic path shares the
same pure cascade recorder. Performance output from this correctness run is not
used as acceptance evidence.

All 95 affected routine targets passed with serial scheduling, followed by the
shared-API `all` build. The initial broad build encountered unrelated GoogleTest
discovery timeouts; the incremental retry completed successfully before the full
test selection ran. These results also cover existing consumers of the extracted
depth readback helper. Changed-document validation and diff checks passed.

The final correctness audit adds
`FMaterialAnimationVulkanTests.MaterialTimeChangesPixelsWithoutReplacingCachedCommands`:
three native renders at explicit times 0.125, 0.75 and 0.125 retain command-template
identity, change more than 1,000 pixels and reproduce the original image exactly.
The test passes alone and in the complete static-mesh preparation target. Its
asset-compilation lifecycle is isolated from the older synchronous material
fixtures; initializing that service for the entire target changed their readiness
assumptions and was removed before final validation.

Stage 6 correctness evidence is covered by the following maintained tests:

| Concern | Passing coverage |
| --- | --- |
| Camera/LOD, transparent order, material mutations and retained revisions | `StaticMeshRenderPreparationVulkanTests`: `ClassifiesResolvedSectionsAndRecomputesPerViewFacts`, `SharedFactsRejectChangedTransformsAndMaterialResources` |
| Negative transforms, opaque/masked/translucent and independent factories | `QualifiesIndependentMultiBatchGeometryAndFactory`, shadow color/all-layer depth parity |
| Spline replacement and deformation | Static-mesh preparation spline publication checks and `SplineTests` |
| Material animation | `MaterialTimeChangesPixelsWithoutReplacingCachedCommands` |
| Reload, failed resources and delayed retirement | `RendererResourceReloadVulkanTests`, `ReplacementRetiresOldResourcesAndGpuFailureCanRetry`, RDG wave failure/lifetime tests |
| Editor preview and picking | `ThumbnailVulkanTests`, `HitProxyIdsRespectDepthBackgroundAndCancellation` |
| Project consumers | `SandboxGameplayTests` (13 cases), `RoadGraphContractTests` (14), `RoadSceneIntegrationTests` (5) |

RoadWeaver's initial-publication fixture omitted its authored material-slot table,
so its asynchronous build failed before preview construction. Supplying
`PreparedMaterialSlots` restores the documented build contract; an explicit
compilation-result assertion now diagnoses this failure directly. The corrected
case passes alone and in its full target. No RoadWeaver runtime behavior changed.

Ownership, invalidation, dynamic-provider eligibility and recording modes now
live in [frame preparation](../Runtime/Rendering/RendererFramePreparation.md),
[render graph execution](../Runtime/Rendering/RenderGraph.md),
[RHI command execution](../Runtime/Rendering/RHICommandExecution.md) and
[Vulkan retirement](../Runtime/Rendering/VulkanMemoryAndGPUCompletion.md).
The obsolete shadow wrapper and arguments are retired. The documented preparation,
shadow-recording and full descriptor-validation switches remain useful for
diagnosis, fallback comparisons and the user's deferred performance evaluation.
They are intentionally retained. This closes the non-performance checklists;
at that checkpoint the plan remained Active pending the deferred measurements.
The later user-directed completion decision above supersedes that lifecycle gate.
The final `all` build, validation of both changed documents and diff checks pass.
Final native receipts are the isolated animation and RoadWeaver preview reports,
the complete affected static-mesh target, and the three project-consumer reports
under `Build/NativeTestResults/Win64-Debug-DurinEditor/`; the earlier 95-target
receipt is preserved in `Build/.agent-state/logs/20260923-003614-184944-49952-ctest.log`.

## Goal

Move stable mesh, material and shader interpretation out of the per-frame path.
Make frame preparation produce compact visible-command lists and binding batches;
make recording consume those records without rediscovering draw state. Reduce
both total CPU work and the rendering critical path, then parallelize remaining
work where the workload justifies task overhead.

## Evidence and Scope

The audited implementation has the following concrete remaining costs:

| Area | Current evidence | Intended change |
| --- | --- | --- |
| Mesh collection | `PrepareStaticMeshView_RenderThread` collects batches, validates geometry and constructs pipeline keys for each view | Retain view-independent records; perform visibility and LOD selection per view |
| Material preparation | `ResolvePreparedMaterialBinding` calls `TryGetMaterialRenderBinding`, which validates layout and copies payload/resource arrays for each section | Resolve immutable material bindings once per accepted revision |
| Shared facts | `FStaticMeshPreparationCache` hashes material contents and compares representations during collection | Propagate stable revisioned identities; use content comparison at publication/deduplication boundaries |
| Shader binding | `BindCompiledSurfaceMaterial` walks reflection and creates a resource vector on each draw | Compile binding layouts once; prepare concrete bindings before recording |
| Vulkan descriptors | `GetOrCreateDescriptorSetsForDraw` sorts, validates and hashes pending resources per draw | Reuse prepared per-set state; update only changed state and dynamic offsets |
| RDG recording | `FRDGBuilder::Record` invokes pass callbacks sequentially on one immediate command list | Introduce owned recording chunks and deterministic assembly |

Moving uniforms into descriptor set 0 does not itself reduce binding frequency.
Uniform grouping is also not draw-call merging. The existing Vulkan descriptor
key already excludes dynamic-uniform offsets; fewer uniform allocations alone
need not reduce descriptor allocation or lookup work.

Preserve the implemented [frame preparation contract](../Runtime/Rendering/RendererFramePreparation.md),
[render graph contract](../Runtime/Rendering/RenderGraph.md) and
[resource recovery contract](../Runtime/Rendering/RendererResourceRecovery.md).
RDG already provides dependency analysis, culling, lifetime planning and barriers;
this plan extends its execution boundary rather than replacing the graph.

The [Geometry Submission Refactor](GeometrySubmissionRefactor.md) owns generic
geometry and vertex-factory extensibility. Preserve independent geometry providers,
multiple batches and dynamic factories. The [RDG and RHI Multi-Queue Execution
Plan](RdgRhiMultiQueueExecution.md) owns GPU queue scheduling, split barriers and
aliasing. CPU parallel recording here must preserve its submission and retirement
contracts and must not silently enable production async compute.

Non-goals: a full UE renderer port, Nanite, GPU-driven visibility, a mandatory
bindless backend, or changing rendering features and image quality. A scene-wide
primitive buffer and instancing are later work, not prerequisites for eliminating
repeated CPU interpretation. Do not reorder transparent draws to increase batching.

## Selected Architecture

Names below describe responsibilities; Stage 0 freezes concrete API names.

| Lifetime | Owned data | Update trigger |
| --- | --- | --- |
| Shader/pipeline generation | Binding layout, shader selection metadata, pipeline identity | Shader/layout generation or device/resource generation changes |
| Scene/asset revision | Geometry records per supported LOD, material binding records, eligible mesh-pass command templates | Accepted geometry/material updates or relevant pass-policy changes |
| Frame/view | Visible command indices, selected LODs, sort keys, view/pass constants, instance data | Each frame and view |
| Recording chunk | Immutable command span, binding state, owned command memory and resource references | Each recording task |
| GPU submission | Submitted command/resource ownership and retirement proof | Completion or defined failure handling |

The intended flow is:

```text
Accepted scene/material/geometry changes
    -> publish revisioned records and invalidate affected commands
Visibility + per-view LOD selection
    -> visible command references + dynamic-provider commands
Pass preparation
    -> ordered command spans + material batches + view/primitive data
RDG setup and compile
    -> resource dependencies + recording plan
Recording tasks
    -> independent command chunks
Ordered assembly and RHI translation
    -> existing GPU submission and retirement contracts
```

Persistent commands contain view-independent draw state, not a saved view matrix
or a transient RDG resource pointer. View/pass resources are supplied at execution
through declared pass parameters. Stable resources remain alive through their
consumers; frame-ring updates must not overwrite data still used by an older frame.

Use explicit revision/generation dependencies rather than raw pointer identity.
Material scalar updates, texture replacement, geometry streaming, spline changes,
shader reload, pass-policy changes and device reset must either update referenced
data safely or invalidate exactly the dependent commands. Retain a bounded dynamic
path for providers whose bindings depend on the view. Cache eligibility is an
explicit capability, not an assumption about every geometry provider.

Cache construction/deduplication may use hashing. The hot recording path uses
prepared indices and spans, not content hashing or a general cache lookup per draw.
Pass constants, material bindings and primitive data have separate update rates;
descriptor-set numbering alone is not the frequency contract. Backend layout
compatibility and dynamic-offset ordering remain explicit.

CPU recording may run ahead without changing GPU dependency order. GPU async
compute, CPU recording parallelism and RHI translation parallelism are distinct
capabilities. The first implementation retains ordered backend translation;
parallel backend contexts require separate ownership and capability evidence.

## Implementation Stages

### Stage 0: Baseline and ownership contracts

Dependencies: none. Outcome: reproducible cost attribution and reviewed data boundaries.

- Deferred follow-up: capture the user workload and bounded static-heavy, material-diverse,
  shadow-heavy and dynamic-update scenes. Record scene revisions, camera, viewport,
  features, cascade count, build configuration, hardware, warm-up and instrumentation.
- Deferred follow-up: measure render-thread busy time separately from waits, worker CPU time,
  RHI translation, GPU time and end-to-end frame time. Separate graph compilation
  from callback recording; inclusive `RDG.Record` time is not graph overhead alone.
- Deferred follow-up: count material/layout validation, bytes copied, allocations, command builds,
  binding updates, descriptor lookup/update, draw calls and retained memory.
- Deferred follow-up: use repeated warmed runs on an exclusive timing lane; record median, p95
  and variability. Keep startup, shader compilation and resource streaming separate.
- Deferred follow-up: freeze per-scenario improvement and regression budgets from those measurements
  before evaluating implementations. Specify a minimum improvement exceeding noise;
  moving work from the render thread to another thread alone is not acceptance.
- [x] Define persistent-record ownership, invalidation keys, dynamic-provider
  eligibility, command-list state boundaries and supported execution modes.

Gate: baseline artifacts and numeric budgets are recorded; every proposed cache
has a documented owner, revision dependency, retirement rule and bounded size.
The screenshot and previous test passes cannot close this gate.

### Stage 1: Persistent geometry and material records

Dependencies: Stage 0. Outcome: unchanged scenes stop repeating stable preparation.

- [x] Publish immutable accepted material records and retained binding views;
  preserve publication validation, error fallback and old-revision lifetime.
- [x] Publish immutable geometry records per eligible batch/LOD at scene-update
  boundaries; preserve fallback diagnostics.
- [x] Carry material record IDs through collection; eliminate repeated layout
  validation, material-array copying and content hashing for unchanged publications.
- [x] Carry geometry record IDs through collection and eliminate repeated stable
  geometry interpretation.
- [x] Share stable records across receiver and cascade views while retaining
  independent visibility, LOD, pass eligibility and transparent ordering.
- [x] Add targeted invalidation for material parameters/resources, geometry/LOD
  replacement, spline deformation and shader/device generations.
- [x] Exercise update/removal while older frames still retain records; bound memory.

Gate: after warm-up, unchanged eligible records cause zero rebuilds and zero
repeated material-layout validation. Mutations update only dependent records;
dynamic providers and all existing mesh consumers preserve their behavior.

### Stage 2: Prepared mesh draw commands and binding batches

Dependencies: Stage 1. Outcome: recording consumes fully interpreted draw records.

- [x] Build eligible pass-specific command templates with geometry, pipeline
  identity and binding references; keep transient view/pass data external.
- [x] Produce compact visible-command arrays, sort opaque/masked state where legal,
  and group adjacent compatible bindings without changing transparent order.
- [x] Compile shader binding layouts at shader creation; prepare binding payloads
  before recording. Remove reflection traversal and material-vector construction
  from the steady-state draw loop.
- [x] Resolve readiness, fallback and pipeline availability before recording;
  preserve output failure/publication contracts rather than creating resources mid-pass.
- [x] Migrate forward, GBuffer, shadow, retained-forward, hit proxy, preview and
  independent geometry consumers, with an explicit dynamic path where needed.

Gate: warmed eligible draw recording performs no material resolution, shader
reflection parsing or pipeline discovery. Counters scale with changed records and
binding batches rather than visible sections; correctness and Stage 0 budgets pass.

### Stage 3: Descriptor frequency and backend state reuse

Dependencies: Stage 2. Outcome: low-frequency data has low-frequency binding work.

- [x] Prepare pass-common and material set state independently; use stable layout
  identities and explicit compatibility when pipelines change.
- [x] Avoid sorting and hashing unchanged complete descriptor state per draw.
  Update primitive offsets without rebuilding material or pass resource ownership.
- [x] Preserve sparse sets, optional bindings, dynamic-offset ordering, descriptor
  pool lifetime, device reset and chunk-boundary state initialization.
- [x] Validate structural bindings when constructed and changed; retain necessary
  draw-dependent range/access checks and a diagnostic full-validation mode.

Gate: identical consecutive binding batches cause no redundant descriptor update;
offset-only changes do not perform full descriptor reconstruction. Pipeline/set
changes and resource retirement pass backend tests. Allocation and retained-memory
budgets remain bounded, with measured CPU improvement over Stage 2.

### Stage 4: Parallel frame and mesh-pass preparation

Dependencies: Stage 2; Stage 3 may complete independently. Outcome: safe preparation tasks.

- [x] Freeze a frame snapshot before worker access; avoid mutable scene/proxy reads
  and shared cache writes inside preparation tasks.
- [x] Partition visibility/LOD and pass-list work using bounded task sizes and
  thread-local scratch; merge deterministically before consumption.
- [x] Schedule tasks early and join at their first real consumer; preserve an
  inline path and avoid parallelizing tiny workloads.
- [x] Define local telemetry accumulation and deterministic failure propagation.

Gate: inline and task paths produce equivalent ordered commands and outputs;
mutation/lifetime stress passes and render critical-path savings exceed scheduling
overhead without violating total CPU and memory budgets.

### Stage 5: Parallel RDG command recording

Dependencies: Stages 3 and 4. Outcome: independent command chunks with ordered assembly.

- [x] Replace the all-pass immediate-list assumption with an owned recording
  interface; classify callbacks by parallel safety and retain serial-only support.
- [x] Make graph metadata and resolved resources immutable during recording;
  callbacks cannot mutate shared renderer caches, graph state or global telemetry.
- [x] Record eligible passes or large mesh-pass chunks on independent lists.
  Define how graphics render-pass scope spans chunks before enabling intra-pass
  splitting; workers must not independently corrupt attachment load/store behavior.
- [x] Keep barrier/submission assembly in the deterministic execution plan, and
  preserve typed-value producer/consumer dependencies, including CPU dependencies.
- [x] Publish outputs only after successful required recording. Define partial
  failure, cancellation, resource retention and fallback without duplicate submission.

Gate: serial and parallel paths match images, resource transitions and externally
observable ordering. Failure and delayed-completion tests pass; current multi-queue
contracts remain valid. Large workloads improve under frozen budgets and small
workloads select an appropriate serial/task-granularity policy.

### Stage 6: Qualification and contract handoff

Dependencies: Stages 0–5. Outcome: functional qualification, diagnostic performance
evidence and maintained contracts under the user-directed completion decision.

- Deferred follow-up: compare against the frozen baseline on identical workloads and instrumentation;
  report median/p95 frame critical path, total CPU, GPU time, allocations and memory.
- [x] Validate camera/LOD changes, negative transforms, masked/translucent materials,
  spline and independent factories, material animation, reload and resource failure.
- [x] Verify editor preview/hit proxy and applicable Engine, Sandbox and RoadWeaver
  consumers; remove superseded interpretation paths only after equivalent coverage.
- [x] Update owning runtime documents, record evidence and retire temporary controls
  that are no longer required for diagnosis or supported fallback.

Gate: all frozen performance, correctness and lifecycle budgets pass. Follow the
[build workflow](../Agents/BuildAndRun.md) and [test workflow](../Agents/Testing.md);
shared Engine API migrations require an `all` build and affected consumer validation.
This original performance gate was deferred by the user's explicit completion
request; the functional, lifecycle and shared-consumer validation was completed.

## Follow-Up Boundary

The deferred performance work above remains unverified. It is retained here for
future evaluation rather than counted as a passing result of this completed plan.

After this plan, evaluate scene-wide primitive data and compatible instancing as
a separate measured change. Their purpose is to reduce per-object binding and
draw count; they are not substitutes for command lifetime and invalidation design.
Optimize RDG compiler data structures only when the isolated compile measurements
identify a material cost. Preserve graph correctness rather than removing dependency
or lifetime checks to make inclusive recording timings appear smaller.

## Reference Basis

- [Epic: Mesh Drawing Pipeline](https://dev.epicgames.com/documentation/unreal-engine/mesh-drawing-pipeline-in-unreal-engine)
  motivates persistent draw state, explicit invalidation and visible-command processing.
- [Epic: Render Dependency Graph](https://dev.epicgames.com/documentation/unreal-engine/render-dependency-graph-in-unreal-engine)
  motivates separation of setup from recording and pass-scoped resource declarations.
- [Epic: Parallel Rendering Overview](https://dev.epicgames.com/documentation/unreal-engine/parallel-rendering-overview-for-unreal-engine)
  motivates separating command recording from backend translation and GPU submission.

These are architectural references, not proof of a particular UE release's full
implementation. The selected Durin design and acceptance gates above are proposals
grounded in the audited local paths; do not import version-specific UE limitations
or assume matching class names provide matching performance.
