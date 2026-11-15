# UE-Aligned GPU Scene Plan

Summary: Track persistent scene storage, shared mesh commands and UE-aligned pass submission; lasting contracts live in GPUScene.

Last reviewed: 2026-11-15

Status: Active
Completed:

## Current Status

Stages 0-4 and 6-30 are implemented. Stage 30 makes context finalization the
completion boundary, requires matching cached Pipeline ownership, and removes
dynamic storage's standalone publication path. Windows bounded owner/consumer
builds and 215 regressions pass. Stage 5's native Metal, independent GPU queue
and exclusive timing gates remain open.

## Goal and References

Use scene-owned incremental Primitive/Instance data and retained mesh commands
for direct, CPU-instanced and GPU-culled rendering. Main interfaces follow the
local UE 5.8.2 reference; Durin keeps its explicit RDG resolver and counted owners.

- [GPUScene contract](../Runtime/Rendering/Scene/GPUScene.md): ownership, packing,
  updates, submission and retirement. This is the implemented authority.
- [RHI transfers](../Runtime/Rendering/RHI/RHIResourceViewsAndTransfers.md#asynchronous-gpu-buffer-readback): asynchronous buffer readback.
- [Build workflow](../Agents/BuildAndRun.md) and [test workflow](../Agents/Testing.md): bounded validation and result reuse.

## Implementation Stages

### Stage 0: Specify interfaces and contract migration

- [x] Implemented and validated; superseded implementation detail is retained in Git history.

### Stage 1: Add scene storage and incremental updates

- [x] Implemented and validated; superseded implementation detail is retained in Git history.

### Stage 2: Unify scene-data shader and direct submission

- [x] Implemented and validated; superseded implementation detail is retained in Git history.

### Stage 3: Share commands and add CPU dynamic instancing

- [x] Implemented and validated; superseded implementation detail is retained in Git history.

### Stage 4: Generalize GPU instance culling

- [x] Implemented and validated; superseded implementation detail is retained in Git history.

### Stage 5: Qualify and retire replaced contracts

- [x] Retire obsolete supported-path uniforms/stages and document capability fallbacks.
- [x] Publish lasting ownership, update and submission contracts.
- [ ] Complete native Metal and independent-queue qualification, including output
  parity, growth, teardown/recovery and legacy-factory/cooked-product cases.
- [ ] Compare static/dynamic, small/large, multi-view and shadow-heavy workloads
  in an exclusive Release lane; record CPU preparation, upload bytes, native
  submissions, GPU time and retained memory. Earlier samples are historical and
  do not establish performance acceptance for the current architecture.

### Stage 6: Align mesh submission interfaces with UE

- [x] Implemented and validated; superseded implementation detail is retained in Git history.

### Stage 7: Unify dynamic allocation and primitive cache ownership

- [x] Implemented and validated; superseded implementation detail is retained in Git history.

### Stage 8: Unify command grouping and submission identity

- [x] Implemented and validated; superseded implementation detail is retained in Git history.

### Stage 9: Consolidate submission and collector contracts

- [x] Implemented and validated; superseded implementation detail is retained in Git history.

### Stage 10: Reuse instance uploads and streamline cached submission

- [x] Implemented and validated; superseded implementation detail is retained in Git history.

### Stage 11: Align cached grouping and sparse scene updates

- [x] Implemented and validated; superseded implementation detail is retained in Git history.

### Stage 12: Consolidate pass command ownership and preparation

- [x] Implemented and validated; superseded implementation detail is retained in Git history.

### Stage 13: Separate primitive collection and unify cache queries

- [x] Implemented and validated; superseded implementation detail is retained in Git history.

### Stage 14: Align persistent selection and mesh pass dispatch

- [x] Implemented and validated; superseded implementation detail is retained in Git history.

### Stage 15: Consolidate mesh pass ownership and bindings

- [x] Implemented and validated; superseded implementation detail is retained in Git history.

### Stage 16: Simplify command storage and optional statistics

- [x] Implemented and validated; superseded implementation detail is retained in Git history.

### Stage 17: Gate mesh statistics and seal prepared passes

- [x] Implemented and validated; superseded implementation detail is retained in Git history.

### Stage 18: Unify culling parameters and buffer statistics readback

- [x] Implemented and validated; superseded implementation detail is retained in Git history.

### Stage 19: Consolidate culling submission and statistics ownership

- [x] Implemented and validated; superseded implementation detail is retained in Git history.

### Stage 20: Prune obsolete rendering tests and documentation

Retain behavioral coverage for draws, fallback, source invalidation, readback,
allocation and lifetime. Remove old API/member-shape guards, source-text scanning
and duplicate wrapper tests. Consolidate surviving tests where a single lifecycle
scenario can cover binding stability and ownership. Keep test registration and
production behavior unchanged; remove unused test-only configuration.

- [x] Clean obsolete and duplicate tests while preserving key scenarios.
- [x] Keep current runtime contracts, phase summaries and open acceptance gates.
- [x] Run affected contract/allocator tests and documentation validation; record
  final inputs under `Build/Qualification/GPUSceneCleanup/`.

Baseline: `8e0e41c4f`. This test/documentation-only stage requires no new GPU run,
workspace all build or timing comparison; reuse Stage 19's native results while
production sources and GPU fixtures remain unchanged.

### Stage 21: Align reusable readback and processor build scopes

Reusable readbacks use one stable state and one in-flight copy without revision
tokens. Recorded/backend owners survive the facade and preserve an old lock across
re-enqueue after retirement. Stage 22 supersedes the original public publication
seam and bidirectional processor/context detach protocol.

- [x] Implement/readback lifecycle and scoped processor contracts.
- [x] Validate retirement-before-reuse, locks, cancellation, processor scope and drawing.
- [x] Update concise owning contracts and final evidence.

Baseline: `cfe44a67a` (consolidated history; task started at `f7f167e33`).
Evidence: `Build/Qualification/GPUSceneBatch11/`.
The public RHI lifecycle/backend-context migration requires a final Windows all
build in addition to owner and affected-consumer tests. Native Metal execution
remains a separate platform gate; no timing acceptance is claimed.

### Stage 22: Seal readback publication and draw-list build lifetimes

Backend command contexts alone publish readback completion/failure. Statistics
queries expose only state/count observations and own their readback directly.
Draw-list contexts retain build-scope destinations; FinalizeCommand receives
per-command input and a completed command. Commands are built locally and only
published on success. Caller scopes own contexts across processor use; no reverse
binding or destructor detach protocol remains. Migrate all workspace consumers.

- [x] Implement publication permissions, immutable observations and build contexts.
- [x] Validate readback cancellation/retirement, repeated finalization, rejection
  without publication, statistics parsing and native drawing in both RHI modes.
- [x] Update owning contracts and record final source hashes and validation.

Baseline: `bb41c487b`. Evidence: `Build/Qualification/GPUSceneBatch12/`.
Public RHI and Renderer interface changes require the final Windows all-build
integration gate plus bounded owner/consumer tests. Native Metal remains an open
platform gate; no performance claim is required for this lifecycle change.

### Stage 23: Separate borrowed buffer reads from texture consumption

Move TakeData behind the protected facade implementation and private shared-state
boundary. Buffer clients only borrow through Lock/Unlock; texture clients keep
TakePixels. Preserve completion-state semantics and reuse after consumption or
cancellation. No backend, object-layout, virtual-interface or GPU algorithm change.

- [x] Narrow access and validate all workspace consumers.
- [x] Verify buffer re-locking and texture consumption, cancellation and reuse.
- [x] Update owning contracts and retain final source/validation evidence.

Baseline: `16685a8e6`. Evidence: `Build/Qualification/GPUSceneBatch13/`.
Bounded RHI, Vulkan RHI, native mesh, Editor, Sandbox and RoadWeaver targets cover
the readback owner and all discovered client roots. This access-only change does
not require another all-build gate or timing comparison. Native Metal remains open.

### Stage 24: Freeze pipelines and simplify command storage

Separate mutable pipeline construction input from immutable published state.
Build value identity at publication without lazy atomic snapshots or assignment.
PreparePipeline returns a const shared pipeline only on success. Dynamic storage
publishes owning stable command pointers directly; remove transient handles and
storage IDs, and enforce frame validity at publication and pass submission.
Cached entries obtain their canonical command from StateBucket, retaining source
invalidation and resource retirement. Migrate all workspace consumers and fixtures.

- [x] Implement immutable pipelines and direct command publication.
- [x] Validate identity isolation, stable addresses, frame expiration, canonical
  pipeline ownership, source invalidation and real drawing in both RHI modes.
- [x] Update contracts and retain final source/validation evidence.

Baseline: `85ac8e461`. Evidence: `Build/Qualification/GPUSceneBatch14/`.
The Renderer pipeline layout and exported GBuffer/processor/storage interfaces
change across modules: require a final Windows all-build integration gate plus
bounded Renderer, native mesh/reload/cloud, Editor, Sandbox and RoadWeaver tests.
Native Metal and exclusive timing remain separate open gates.

### Stage 25: Scope visible-command preparation data and resource owners

Remove scene-domain ownership, CPU instance-ID spans and admission flags from
FVisibleMeshDrawCommand. Build generic source-indexed culling payloads only for
preparation; Culling Setup consumes them synchronously and materializes its own
candidates/submit information. SourceDrawIndex/NumSourceDraws identify compacted
sources. CPU compaction retains no per-command preparation spans. Passes retain
distinct scene-domain owners and merged ID buffers; contexts retain native culling
buffers and dynamic allocation leases. Preserve direct and failed-dispatch draws.

- [x] Migrate preparation/submission interfaces and all workspace consumers.
- [x] Verify preparation release, pass/context resource leases, CPU compaction,
  malformed/foreign domains, source invalidation and GPU failure fallback.
- [x] Run the Windows all-build gate and owner/consumer/native rendering tests;
  update contracts and retain final source/reference hashes and reports.

Baseline: `30204cf66`. Evidence: `Build/Qualification/GPUSceneBatch15/`.
Visible-command layout and exported pass/culling signatures change across modules;
require an all build plus Renderer/allocator, native mesh/reload/cloud in both RHI
modes, Editor, Sandbox and RoadWeaver coverage. Metal and timing remain open.

### Stage 26: Remove redundant binding and shadow fallbacks

The unified resource phase publishes the uploaded shared context into resolved
receiver/shadow views. Binding preparation consumes and validates it, with no
nullable context argument or local collection/upload fallback. Preserve empty
unsupported-factory entries and reject missing/unuploaded/foreign prepared data.
Shadow depth binding receives empty Lighting; remove its unused allocation,
readiness condition and retained state. Preserve optional feature/capability
routing and genuine execution-time culling recovery.

- [x] Migrate resource/binding preparation and remove the shadow placeholder.
- [x] Run Renderer contracts, native mesh/reload/cloud in both RHI modes, Editor,
  Sandbox and RoadWeaver regressions; update contracts and final evidence.

Baseline: `2b40648c3`. Evidence: `Build/Qualification/GPUSceneBatch16/`.
The changed renderer-private signatures have only same-module callers and no
shared layout/backend change. Bounded owner and consumer builds cover this scope;
no new all-build, platform or timing gate is introduced.

### Stage 27: Admit culling before setup and classify direct routing

Freeze Disabled/Unsupported/ResourcesUnavailable/Enabled in the prepared GBuffer
feature decision. Preserve the result of compute resource preparation; unavailable
resources skip culling context/group/buffer/readback creation. Keep direct output
parity and genuine execution-time recovery. Remove the aggregate fallback metric,
rename normal direct/unsupported/no-group metrics and record resource unavailability
separately from dispatch failure. Migrate all workspace telemetry consumers/tests.

- [x] Implement prepared admission and reason-specific statistics.
- [x] Verify shader/PSO unavailability produces no culling work/readback; preserve
  small-group routing, metadata budgets, real dispatch-failure recovery and retry.
- [x] Run Windows all build, owner/consumer/native regressions in both RHI modes,
  compile the affected GBuffer qualification target, and record final evidence.

Baseline: `fd1f0a41b`. Evidence: `Build/Qualification/GPUSceneBatch17/`.
Prepared feature and exported telemetry layouts change across module boundaries,
requiring the final all-build gate. Renderer/allocator, native mesh/reload/cloud,
Editor, Sandbox and RoadWeaver regressions cover behavior. Qualification-source
migration needs compilation only; Metal and exclusive timing remain open.

### Stage 28: Consolidate culling policy and required construction dependencies

Keep requested/capability/readiness policy in prepared frame admission. Remove
repeated policy flags from pass/context setup. Require the manager's resource
coordinator and the context's manager at construction; migrate CPU fixtures to
normal manager creation without adding test-only incomplete object modes.

- [x] Remove duplicate policy flags and nullable construction dependencies.
- [x] Migrate workspace callers and remove obsolete lower-layer policy assertions.
- [x] Run Renderer contracts and native mesh integration in both RHI modes;
  compile the GBuffer qualification consumer and validate documentation.

Baseline: `054af223a`. Evidence: `Build/Qualification/GPUSceneBatch18/`.
Changes are private Renderer APIs with no Sandbox/RoadWeaver source consumers.
Bounded owner and native integration targets cover construction, command setup,
frame admission, direct routing and genuine dispatch recovery. No shader or GPU
algorithm changes; qualification execution and timing are not required.

### Stage 29: Align draw-list allocation and publication semantics

Use UE 5.8's distinct AddCommand/FinalizeCommand responsibilities. Dynamic Add
allocates stable frame storage; Finalize publishes its existing address. Cached
Add uses the single-command initializer and Finalize performs state bucketing.
Remove the identity default implementation and TakeCommand forwarding wrapper.
Keep failed builds unpublished; incomplete dynamic entries expire with the frame.

- [x] Implement concrete context allocation/publication and migrate the observer.
- [x] Cover address stability, delayed publication and retained allocation lifetime.
- [x] Run Renderer contracts, native mesh integration in both RHI modes, compile
  GBuffer qualification consumers and validate documentation.

Baseline: `2a30ce9d1`. Evidence: `Build/Qualification/GPUSceneBatch19/`.
Private Renderer interfaces have no Sandbox/RoadWeaver source consumers. Bounded
owner/native tests cover cached reuse, dynamic binding, rejection and frame
retirement. GPU algorithms are unchanged; qualification execution is not required.

### Stage 30: Consolidate command completion and pipeline ownership

Only context finalization marks completed commands; geometry preparation leaves
commands unfinished. Require a matching Pipeline owner for cached publication.
Remove dynamic storage Publish and migrate its fixtures through normal contexts;
retire the obsolete bypass/frame-publication test and merge duplicate retention
coverage into the context stability test.

- [x] Implement final completion, required Pipeline ownership and one dynamic path.
- [x] Migrate all callers and cover completion, binding retention and invalidation.
- [x] Run Renderer contracts and native mesh integration in both RHI modes,
  compile qualification consumers and validate documentation.

Baseline: `f2324b078`. Evidence: `Build/Qualification/GPUSceneBatch20/`.
The new test helper is included by two Renderer contract translation units only;
its project-wide affected selection is broader than these private API changes.
Use bounded owner/native targets; GPU algorithms and external project APIs are unchanged.

## Validation Evidence

| Revision / stage | Evidence and scope |
| --- | --- |
| Stage 30, base `f2324b078` | `Build/Qualification/GPUSceneBatch20/receipt.json`: bounded owner/consumer builds and 215 checks; Renderer 173, native mesh 21 in each RHI mode. |
| Stage 29, base `2a30ce9d1` | `Build/Qualification/GPUSceneBatch19/receipt.json`: bounded owner/consumer builds and 217 checks; Renderer 175, native mesh 21 in each RHI mode. |
| Stage 28, base `054af223a` | `Build/Qualification/GPUSceneBatch18/receipt.json`: bounded owner/consumer builds and 216 checks; Renderer 174, native mesh 21 in each RHI mode. |
| Stage 27, base `fd1f0a41b` | `Build/Qualification/GPUSceneBatch17/receipt.json`: Windows all build, GBuffer qualification target compilation and 339 checks; Renderer 174, allocator 8, native mesh/reload/cloud 24 in each RHI mode, Editor 91, Sandbox 13, RoadWeaver 5. |
| Stage 26, base `2b40648c3` | `Build/Qualification/GPUSceneBatch16/receipt.json`: 331 checks and bounded owner/consumer builds; Renderer 174, native mesh/reload/cloud 24 in each RHI mode, Editor 91, Sandbox 13, RoadWeaver 5. |
| Stage 25, base `30204cf66` | `Build/Qualification/GPUSceneBatch15/receipt.json`: Windows all build and 339 checks; Renderer 174, allocator 8, native mesh/reload/cloud 24 in each RHI mode, Editor 91, Sandbox 13, RoadWeaver 5. |
| Stage 24, base `85ac8e461` | `Build/Qualification/GPUSceneBatch14/receipt.json`: Windows all build and 331 checks; Renderer 174, native mesh/reload/cloud 24 in each RHI mode, Editor 91, Sandbox 13, RoadWeaver 5. |
| Stage 23, base `16685a8e6` | `Build/Qualification/GPUSceneBatch13/receipt.json`: 392 checks; RHI 151, Vulkan RHI 111, native mesh 21, Editor 91, Sandbox 13, RoadWeaver 5. Owner/consumer targets compile the narrowed interface; no new all-build or timing claim. |
| Stage 22, base `bb41c487b` | `Build/Qualification/GPUSceneBatch12/receipt.json`: Windows all build and 847 checks. RHI 151, Renderer 174, RDG/allocator 254, Vulkan RHI 111, mesh/reload/cloud 24 in each RHI mode; Editor 91, Sandbox 13, RoadWeaver 5. |
| Stage 21, base `cfe44a67a` | `Build/Qualification/GPUSceneBatch11/receipt.json`: Windows all build and 847 checks. RHI 151, Renderer contracts 174, RDG 246 + allocator 8, Vulkan RHI 111, native mesh 21 + reload 2 + cloud scene 1 in both RHI modes; Editor 91, Sandbox 13, RoadWeaver 5. |
| Stage 20, base `8e0e41c4f` | `Build/Qualification/GPUSceneCleanup/receipt.json`: 174 Renderer contracts and 8 allocator checks pass. Records test removals/merges, final hashes and documentation size reduction. |
| Stage 19, `8e0e41c4f` | `Build/Qualification/GPUSceneBatch10/receipt.json`: 583 checks; Windows Debug, Vulkan validation on, graphics queue, threaded/inline RHI. Covers final-command ranges, source mapping, failed-dispatch direct fallback, optional statistics and pass lifetime. |
| Stage 18, `9a36de7b1` | `Build/Qualification/GPUSceneBatch9/receipt.json`: Windows all build and 840 checks for the stable buffer-readback/context migration. Windows does not compile native Metal implementation. |

Raw XML, logs, scripts and hashes belong under ignored `Build/Qualification/`.
Earlier stage receipts remain there and in Git history before this cleanup;
completed decisions are documented once in their owning runtime contracts.

## Completion Gates

- Camera-only movement and additional views upload no unchanged scene attributes;
  ID/visibility/argument uploads are counted separately. Dirty work scales with
  changed records and allocator growth.
- Compare direct/CPU/GPU output, zero/partial/full visibility, multiple views/scenes,
  source mutation, growth, cancellation, teardown and recovery on supported backends.
- Retain the final shared-ABI workspace all-build gate; reuse passing evidence while
  relevant inputs remain unchanged. New broad ABI migrations require a new gate.
- Stage 5's native platform and exclusive timing requirements remain open. Fewer
  draw submissions alone do not prove a frame-time improvement.

Stage 20 validation uses `windows-msvc-x64` / `Win64-Debug-DurinEditor` with
application-hosted tests off. CMake changes only remove the unused source-scan macro;
test registration and dependencies are unchanged. `affected --explain` conservatively
selects the Engine project because the target-definition file changed; only the
owning contract target and the allocator target that includes its source need rerun.
Production/GPU-fixture hashes match Stage 19, so no new native GPU or all-build run
is claimed. Changed-document validation and all-plan validation pass. Removal/merge
reasons and pre-cleanup document snapshots are retained with the local receipt.

Stage 21 records final source and local UE 5.8.2 hashes, XML and logs. RHI tests
cover stack ownership, re-locking, lock across re-enqueue, pending reuse rejection,
cancellation/abandonment and backend storage outliving its facade. Native mirror
checks reuse readbacks across updates, growth, recovery and cancellation. Processor
checks cover context destruction in either order and read-only prototype isolation;
mesh/PSO, shadow and picking regressions validate successful binding publication.
The first complete inline run hit 300s while other native tests were active.
Diagnostic groups passed with the same disjoint 21-case set, then a complete
single-process inline rerun passed in 11.09s; the timeout log is retained. These
are correctness checks, not exclusive timing evidence. Metal code is migrated and
source-reviewed but native macOS compilation/execution was unavailable. Subsequent
changes are documentation only. Reproduce with the receipt's Validate.ps1.

Stage 22 validates final source hashes with the receipt's Validate.ps1 and
Receipt.py. CPU fixtures publish through simulated RHI backends; observations
retain validated counts without exposing transport mutation. Native mesh coverage
reuses one context across successful, rejected and subsequent successful batches,
including geometry failure after construction starts and fresh source metadata.
The final native fixture passes in threaded and inline modes; other passing inputs
remain unchanged. Vulkan validation is on, the compute queue is disabled, and no
exclusive timing acceptance is claimed. Windows cannot compile/run native Metal.

Stage 23 records final header/test hashes, local UE buffer-readback references,
XML and logs. Existing buffer coverage verifies repeated Lock/Unlock and borrowed
bytes across cancellation/re-enqueue. The texture lifecycle case verifies one-time
consumption, unchanged output on unavailable reads, retained completion status,
canceled late publication and reuse. Reproduce with Batch13's Validate.ps1.

Stage 24 records final source/reference hashes, XML and logs. Initializer changes
cannot alter published pipeline identity; failed PreparePipeline leaves the prior
publication intact. Existing growth/retention tests cover direct owning command
pointers; frame tests cover expired publication and submission. Canonical pipeline
retention and independent source invalidation pass without duplicate entry command
owners. Both final native mesh reports cover the final failure-preservation fixture;
production sources remain unchanged from the successful all build. Reproduce with
Batch14's Validate.ps1. Vulkan validation is on, the compute queue is disabled,
and Metal/independent-queue/exclusive timing gates remain open.

Stage 25 records final source/reference hashes, XML and logs. Scoped payloads are
released after culling setup without being retained by contexts; pass-owned scene
domains survive prepared/resolved source release and expire with the pass. Source
ranges preserve CPU/GPU compaction, candidate IDs, diagnostics and direct fallback.
Both RHI modes pass native rendering/reload regressions with Vulkan validation on
and the compute queue disabled. Reproduce with Batch15's Validate.ps1. Production
and fixture inputs remain unchanged after validation; native Metal, independent
queues and exclusive timing remain open.

Stage 26 records final source hashes, XML and logs. Existing native opaque/masked/
WPO shadow, shared scene preparation, culling/direct drawing and resource-reload
regressions pass in threaded/inline RHI modes without the placeholder Lighting
allocation. Binding only consumes already uploaded resolved data. Vulkan validation
is on and the compute queue is disabled. Reproduce with Batch16's Validate.ps1.
No new all-build, Metal, independent-queue or exclusive timing result is claimed.

Stage 27 records final source/reference hashes, XML and logs. Shader/PSO-unavailable
frames retain direct output parity with zero culling candidates/buffers/query and
no culling/readback graph pass or resource. Small-group and metadata-budget routes
report no eligible group, while CommandAdmission records dispatch failure and
preserves direct recovery/retry. Both RHI modes pass; qualification consumers
compile with renamed telemetry fields. Reproduce with Batch17's Validate.ps1.
Vulkan validation is on and the compute queue is disabled. No native Metal,
independent-queue, qualification execution or exclusive timing result is claimed.


Stage 28 records source/reference hashes and XML/logs. Required dependencies use
borrowed references with enclosing service lifetimes; CPU fixtures create normal
managers and registered contexts. Frame admission retains disabled/unavailable and
direct-recovery coverage while lower-layer opt-in assertions are removed. Native
mesh integration passes in threaded/inline modes with Vulkan validation on and the
compute queue disabled; GBuffer qualification consumers compile. Reproduce with
Batch18's Validate.ps1. No all-build, native Metal, qualification execution or
performance result is claimed.

Stage 29 records source/reference hashes and XML/logs. Dynamic context allocation
is address-stable and publication retains the same command across facade teardown.
Native cached reuse, dynamic bindings and failure rejection pass in both RHI modes
with Vulkan validation on and the compute queue disabled. GBuffer qualification
consumers compile. Reproduce with Batch19's Validate.ps1. No all-build, native
Metal, qualification execution or performance result is claimed.

Stage 30 records final source/reference hashes and XML/logs. Context and native
non-indexed tests verify unfinished geometry preparation followed by completed
publication; cached state buckets retain the matching Pipeline until source
invalidation. Dynamic fixtures use AddCommand/FinalizeCommand and context tests
retain stable binding memory across facade teardown. Both RHI modes pass with
Vulkan validation on and the compute queue disabled; GBuffer qualification
consumers compile. Reproduce with Batch20's Validate.ps1. No all-build, native
Metal, qualification execution or timing result is claimed.
