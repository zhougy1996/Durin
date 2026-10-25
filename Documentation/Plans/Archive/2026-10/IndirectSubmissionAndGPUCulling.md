# Indirect Submission and GPU Culling Plan

Summary: Add validated indirect draw and dispatch commands, then prove them through an opt-in GPU-frustum-culling path for compatible static-mesh instances with deterministic CPU fallback.

Last reviewed: 2026-10-18

Status: Archived
Completed: 2026-10-18

## Current Status

Completed on 2026-10-18 on Apple M4. The default remains direct rendering;
compatible opaque and masked GBuffer instance groups have an explicit opt-in.
The operator waived complex reference/candidate timing, quiet-lane noise checks
and automatic-threshold measurements. No speedup or measured crossover is
claimed. The 64-candidate floor and 4 MiB storage ceiling remain structural
admission rules. Multi-draw, bindless grouping, GPU Scene and Hi-Z have no
measured trigger and are outside this completed scope. This waiver does not
close the native-event gate in the separate multi-queue plan.

Closeout repaired three native correctness defects: the Metal constant-buffer
layout is now the same 80-byte matrix-plus-uint4 ABI as the CPU; GPU graphics
permutations own their own per-pass fragment bindings; generated and fixed
vertex entries add `SV_StartInstanceLocation` to relative `SV_InstanceID` before
addressing each group's visible-index range. Generator revision 15 invalidates
cached material translation products containing the old entry. The existing
builder already admitted masked GBuffer groups; the opaque-only Stage 0 wording
is superseded by native qualification of both opaque and masked groups here.
Masked shadow, spline, translucent, hit-proxy and editor-assistance passes stay
direct.

Per-view telemetry now publishes asynchronous validated visible/culled counts,
admission/fallback attribution, exact successful direct/indirect draw totals,
and both logical RDG assignment and physical queue mapping. Registering a view
telemetry observer explicitly authors two rooted diagnostic copy passes;
unobserved rendering adds neither copy nor readback. Queries distinguish
Pending/Ready/Failed/Canceled from a real zero-visible result. Rendering never
waits for or uses CPU-visible counts. This replaces the original absolute
no-readback wording with a no-readback-rendering policy. Graph-wide allocation
rejection preserves transactional view failure; shader/pipeline/budget and
pre-record command admission rejection preserve direct fallback.

### Final acceptance receipts

Tested input is base revision `00644cbe20eb83dd6141c45badf1825f897b26be` plus
this closeout's source/fixture changes. Exact SHA-256 inputs are retained in
`Build/Qualification/IndirectCullingFinalInputs.json`. No relevant source,
fixture, shader or runtime-setting change follows the recorded runs. The
configuration is `MacOS-arm64-Debug-DurinEditor`, Tracy enabled, application
tests disabled, Apple M4; Vulkan API 1.3.334, driver `0x28a1`, MoltenVK from
SDK 1.4.357.0. Native GPU runs used the authorized unsandboxed environment.

- `MetalSceneQualificationTests --mode qualification`: 8/8, including four
  Metal/Vulkan × inline/threaded culling cases with `MTL_DEBUG_LAYER=1` and
  `DURIN_VULKAN_VALIDATION=on`; native validation is clean. Each culling case
  executes 60 submissions: 51 successful outputs and nine deliberate failures.
  The 12 direct/indirect pairs cover zero/partial/full visibility, conservative
  camera-boundary bounds, distinct transforms, two opaque/masked groups,
  nonzero first index/argument offset/first instance, resize/reuse, repeated
  visibility changes and a 1e12 world origin. All final colors, four GBuffer
  attachments and sampled D32 bits agree exactly: 48 pairs / 288 outputs.
- `MetalSceneQualificationTests '*GPUCulling*' --mode qualification`: 4/4
  with `MTL_DEBUG_LAYER=0` and `DURIN_VULKAN_VALIDATION=off`, the same 48-pair
  correctness matrix. Each validation setting retains 240 attachment pairs.
  The validation-on run also has 171/171 identical image/attachment artifacts
  between replay modes per backend and across the two backends.
- The scene matrix exercises unavailable compute shader, GPU PSO, metadata
  budget and command admission, plus candidate/visible/argument/transform
  allocation failure and next-frame recovery. No observer produces no counter
  carrier; shader/device/manual resource-generation invalidation reconstructs
  a working indirect path. Existing raster failures cannot publish later
  success. Submission/completion cancellation and failure are injected at the
  supported RHI/RDG contract seams; these are not claims of a destructive
  physical-device-loss test.
- `MetalRHIHeadlessTests --mode qualification`: 46/46 with Metal API
  validation, including native indirect commands, exact completion/ownership,
  cancellation, readback and shutdown behavior. Full output is retained in
  `Build/Qualification/IndirectCullingHeadless.log`.
- `RendererSceneContractTests`: 63/63, including immutable query-state/count
  validation, indexed argument preservation, 63/64 admission, exact/one-byte-
  over budget, empty inputs, maximum command groups and group-wise overflow,
  excluded deformation and translated-origin bounds/transforms.
- `RendererRDGAllocatorTests`: 11/11; `RenderContractTests`: 227/227;
  `RHICommandListTests`: 143/143. These retain exact indirect ranges, compute
  producers, parameter/pipeline/geometry owners, canceled recording, failed
  submission quarantine, unrelated-queue completion and pool retirement gates.
- `MaterialCompilerTests`: 93/93; `MaterialCookTests`: 7/7;
  `RenderShaderCookIntegrationTests`: 1/1. Generated GPU entry and cache identity
  changes preserve cooked and authored shader consumers.
- `./DevTool build all` passed for Engine, Sandbox and RoadWeaver. Its receipt
  is `Build/.agent-state/logs/20261018-075342-726429-60668-cmake.log`.
  `affected --explain` expanded to all Engine tests because of the shared
  qualification header. The named owning Renderer/RHI/RDG/material selections
  above replace unrelated asset/editor/platform tests; native scene cases
  explicitly cover the opt-in GPU gates excluded by ordinary affected runs.

Native XML receipts use the `IndirectCulling*` prefix under
`Build/NativeTestResults/MacOS-arm64-Debug-DurinEditor/`. Full GPU logs and
pixels/graph/attachment evidence are retained under
`Build/Qualification/IndirectCullingValidated.log`,
`Build/Qualification/IndirectCullingUnvalidated.log`, and
`Build/Qualification/IndirectCullingValidationOn/` / `IndirectCullingValidationOff/`.
The headless production scene/RHI fixtures provide current-host correctness
coverage without enabling unavailable application-hosted wrappers; those
wrapper target names are not reported as newly executed.

Logical async batches map to the same physical graphics queue on this M4.
Independent physical compute and native Vulkan events are unavailable, not
reported as passes. Full-barrier/shared-queue fallbacks pass; the separate
multi-queue plan retains its hardware gate. Shadows remain an independent
direct consumer; this pilot does not introduce an indirect shadow-output gate.
Lasting grouping, ABI, telemetry, fallback and opt-in rules live in the
[GBuffer contract](../../../Runtime/Rendering/Pipeline/GBuffer.md#compatible-instance-culling)
and [performance baseline](../../../Development/Build/RenderingPerformanceBaseline.md).

## Goal

Deliver one complete GPU-driven geometry-submission path in which a compute pass
tests bounds for a compatible group of static-mesh instances, writes a compact
visible-instance list and indirect indexed-draw arguments, and a later graphics
pass consumes both without a CPU readback or GPU-idle wait.

The path must preserve the existing direct submission as a deterministic
fallback. It must produce equivalent color, depth, GBuffer, and applicable
shadow results; retain every input and generated resource through GPU
completion; operate on the graphics queue when independent compute is
unavailable or disabled; and remain opt-in until measured workloads establish
a useful activation threshold.

The low-level family also includes non-indexed indirect draw and indirect
dispatch so the argument-buffer access, validation, recording, replay, and
Vulkan lowering form one coherent contract rather than unrelated later APIs.
Only indexed indirect draw requires a production Renderer consumer in this
plan; the other operations require public-RHI conformance coverage.

## Scope and Non-Goals

### In scope

- Backend-neutral indirect argument records with fixed field order, width, and
  checked native-compatible layout.
- Conservative capability publication for indexed/non-indexed draw, dispatch,
  and optional multiple-command indirect execution.
- Exact indirect-argument buffer usage, offset, stride, count, bounds, access,
  command ownership, and replay validation.
- Vulkan lowering for `vkCmdDrawIndirect`, `vkCmdDrawIndexedIndirect`, and
  `vkCmdDispatchIndirect`, including feature/limit negotiation and debug labels.
- RDG declaration and barrier synthesis for compute/storage writes consumed as
  indirect arguments and graphics-readable visible-instance data.
- One bounded static-mesh instance-group consumer selected by equal geometry,
  material, vertex-factory, graphics-pipeline, render-pass, and binding
  compatibility.
- Compute frustum culling, compact visible-instance indices, one indexed
  indirect command per admitted compatible group, overflow handling, telemetry,
  output equivalence, fallback, and performance qualification.
- Inline/threaded replay, single/independent queue execution, cancellation,
  failure, resource-pool reuse, resize, and shutdown coverage.

### Out of scope

- Bindless descriptors, a process-wide GPU Scene, heterogeneous-material or
  heterogeneous-PSO multi-draw, mesh/amplification shaders, ray tracing, or
  sparse resources.
- Hi-Z occlusion, software occlusion, LOD selection on the GPU, GPU sorting,
  meshlet generation, or general visibility-system replacement.
- Repacking existing vertex or index resources into global geometry arenas.
- Making every primitive family indirect, removing direct draws, or changing
  translucent ordering.
- Production-wide automatic asynchronous compute, transient aliasing, or
  completion of later stages in the multi-queue plan.
- Enabling a path because the device advertises it without a qualified workload
  and an explicit Renderer policy.

## Selected Design

### Portable command and argument contract

Define value records equivalent in meaning and binary layout to one native
non-indexed draw, indexed draw, and dispatch command. Field semantics remain
backend-neutral: vertex/index or group counts, instance count, first locations,
and signed base vertex. Static assertions freeze sizes, alignments, field
offsets, and signedness used by GPU writers and backend readers.

The RHI command receives a counted argument buffer, byte offset, and command
count/stride where selected by Stage 0. Recording copies scalar descriptors and
retains the argument buffer. It does not copy GPU-authored argument bytes or
map the resource. Validation rejects null resources, missing indirect usage,
CPU-authored resources, unaligned or overflowing offsets, undersized strides,
zero or unsupported command counts, out-of-bounds ranges, wrong pipeline or
render-pass domain, and missing device capability before a backend command is
recorded.

`ERHIAccess::IndirectArgumentRead` is a read-only buffer access distinct from
shader, vertex, index, transfer, and host reads. It may combine only with
compatible read-only intents. Vulkan lowers it to draw-indirect pipeline access;
the compute-write-to-indirect-read barrier must not be approximated as a shader
read. Queue-family ownership continues through the existing explicit handoff
contract when compute and graphics use distinct families.

Multiple-command indirect execution is capability-gated. Stage 0 must decide
whether the first stable API exposes count/stride immediately or admits exactly
one command and leaves a later additive multi-draw operation. No implementation
may silently loop native single draws to claim GPU multi-draw support; an
explicit CPU replay loop, if retained as a compatibility behavior, must have a
separate observable capability and cost.

Indirect dispatch is legal only in the compute pipeline domain and outside a
render pass. Indirect draws are legal only in the graphics domain and inside a
compatible active render pass. They reuse the active PSO, descriptors, vertex
and index bindings, dynamic state, validation, diagnostics, and submission
ownership of direct commands.

### Capabilities and API stability

Add capabilities only for complete executable paths. Defaults are false/zero,
and unsupported devices retain the direct or direct-dispatch path. Capability
meaning distinguishes at least single indirect draw, indirect dispatch, native
multiple-command indirect draw, and the maximum admitted command count when
applicable.

Stage 0 selects the public-header boundary under the rules in
[RHI public API stability](../../../Runtime/Rendering/RHI/RHIPublicAPIStability.md). A
stable additive family must land atomically with validation, Vulkan execution,
fallback semantics, and owning Runtime documentation. If staged exposure is
required, use one coherent `Public/Experimental/` family and do not scatter
experimental types across otherwise stable headers without recording the
mixed-header boundary.

Existing reserved `DrawIndirect` buffer vocabulary is not treated as evidence
of support. Support begins only when the active backend publishes the matching
capability and the complete command path is available.

### RDG ownership and synchronization

RDG parameters declare the exact argument range as
`IndirectArgumentRead`. The graph compiler derives a dependency from the
compute storage writer to the graphics indirect consumer, emits the transition
or queue handoff before the draw batch, and retains both the argument buffer and
visible-instance buffer through every using submission.

The visible-instance list is compute-written storage consumed by the vertex
stage as graphics shader read. The argument buffer is compute-written storage
consumed by the fixed-function indirect command reader. These are separate uses
even when suballocated from one physical allocation; their exact ranges must be
declared and validated independently.

The graph may assign the culling pass to an independent compute queue only when
the view policy, backend topology, allocator, and existing multi-queue safety
gates all admit it. Otherwise the same graph executes on the graphics queue.
Neither path introduces a CPU wait or a readback of the visible count.

### Compatible instance groups

The pilot groups only draws that can share one graphics draw state and geometry
binding. Compatibility includes the existing mesh draw key plus exact geometry
identity, index type and range, vertex-factory implementation and layout,
material/pipeline generation, render-target/pass compatibility, and any
resource binding whose value cannot vary through per-instance data.

Each candidate instance supplies a stable primitive identity, world transform,
world-space bounds, and the per-instance values selected in Stage 0. A bounded
GPU candidate buffer is immutable for one graph execution. Compute appends or
otherwise compacts visible candidate indices into a bounded output and writes
one indirect indexed-draw record. The draw's instance count equals the admitted
visible count; `FirstInstance` and shader indexing must preserve the current
instance semantics.

The vertex-factory path resolves the visible-instance index and transform from
graphics-readable storage. It must not recover Engine objects, raw scene proxy
pointers, or mutable Renderer containers on the GPU. Cooked shader identity,
reflection, layout, and pipeline-cache keys distinguish the GPU-driven
permutation without multiplying keys by per-frame buffer identity.

The initial consumer covers opaque compatible groups in one selected production
mesh pass. Stage 0 chooses GBuffer or the retained forward base pass from the
actual production inventory and records why. Masked shadow, hit-proxy,
translucent, editor-assistance, and other passes stay direct until separately
qualified; the grouping API must not imply their support.

### Overflow, failure, and fallback

Candidate, visible-index, and argument storage have explicit per-view and
process budgets. Capacity arithmetic is checked before allocation and dispatch.
No shader write may exceed the admitted ranges. Overflow or unsupported group
shape produces deterministic telemetry and uses the existing direct path for
the affected group or view; it must not silently drop geometry.

The direct path also handles unsupported capabilities, disabled policy, a group
below the selected activation threshold, unavailable compute shader or PSO,
resource-allocation/admission failure, incompatible bindings, and unavailable
independent compute. A runtime failure after GPU work has been admitted follows
the established RHI/RDG terminal failure contract and must not attempt an
unordered CPU redraw of the same pass.

Fallback is selected before recording the affected graphics pass. A view cannot
partially publish indirect output and then replay the same instances directly.
Telemetry distinguishes policy-disabled, unsupported, below-threshold,
incompatible, capacity, preparation, and terminal execution outcomes.

### Activation and performance policy

The production path remains explicitly opt-in while this plan is active. Device
support alone never enables it. Stage 0 records a representative repeated-mesh
workload and the current CPU path using the shared
[rendering performance baseline](../../../Development/Build/RenderingPerformanceBaseline.md).

Qualification compares identical candidate counts, visibility ratios, camera
paths, output settings, RHI execution mode, validation state, and instrumentation
between the frozen reference and candidate revisions. Observe at least Renderer
preparation time, RHI recording/replay time, culling dispatch time, graphics GPU
time, direct and indirect command counts, barriers, submission batches,
descriptor/PSO cache behavior, upload bytes, retained GPU bytes, and retirement
backlog.

An activation threshold is accepted only where repeated quiet-lane measurements
show a material frame benefit without violating the selected memory and
submission budgets. If no representative workload benefits, retain the feature
as an explicit opt-in capability and record that automatic production selection
was not justified.

## Implementation Stages

### Stage 0: Freeze the ABI, consumer, fallback, and baseline

Dependencies: accepted RHI command/resource ownership contracts, Render Graph,
common geometry submission, and the rendering performance catalog.

Outcome: implementation decisions and comparison evidence are sufficient to
add one bounded public command family without inventing policy during coding.

- [x] Inventory every current user of `EBufferUsageFlags::DrawIndirect`, direct
  draw/dispatch overloads, graphics/compute access validation, Vulkan buffer
  usage mapping, mesh instance ranges, and compatible draw grouping.
- [x] Freeze argument record layouts, offset/stride/count rules, integer
  overflow rules, command-domain legality, capability names, and whether native
  multi-draw enters the first public contract.
- [x] Select and document the stable or experimental header boundary; enumerate
  every RHI, RenderCore, VulkanRHI, Renderer, shader, and test consumer affected
  by the shared API migration.
- [x] Select one production opaque mesh pass and one repeated-instance fixture
  whose geometry/material/pipeline compatibility is explicit and whose current
  direct output is deterministic.
- [x] Freeze candidate and visible-record schemas, bounds convention,
  compaction algorithm, counter reset/overflow behavior, per-view capacities,
  and shader indexing semantics.
- [x] Define the complete pre-record fallback matrix and telemetry outcomes,
  including policy, capability, compatibility, threshold, capacity, creation,
  and admission cases.
- [x] Capture the reference revision's correctness images/counters and available
  diagnostic performance measurements. Record unavailable quiet-lane or
  hardware coverage as an open acceptance gate rather than synthesizing a
  threshold.

Completion: reviewers can determine exact buffer bytes, access states,
commands, grouping, failure behavior, consumer, reference evidence, and public
API impact without consulting an implementation prototype.

#### Stage 0 contract decisions and inventory (2026-09-30)

- `FRHIDrawIndirectArguments`, `FRHIDrawIndexedIndirectArguments`, and
  `FRHIDispatchIndirectArguments` are standard-layout records of respectively
  four, five, and three 32-bit words. The indexed record's fourth word is signed.
  Their sizes are 16, 20, and 12 bytes, alignment is four bytes, and field
  offsets exactly match Vulkan's corresponding command structures.
- The stable surface lives with the existing direct records in
  `RHIResources.h`, `RHICommandList.h`, and `RHIContext.h`. Each operation takes
  one GPU-authored `DrawIndirect` buffer and a four-byte-aligned byte offset.
  The first contract has no count or stride: native multi-draw support and a
  maximum multi-draw count therefore remain false/zero and cannot be emulated
  by a hidden CPU loop.
- `bSupportsIndirectDraw` and `bSupportsIndirectDispatch` describe complete
  executable single-command paths. Draw requires the graphics domain and an
  active render pass; dispatch requires the compute domain outside a render
  pass. Range admission uses widened subtraction and rejects null,
  CPU-authored, wrong-usage, unaligned, or undersized buffers before recording.
- `ERHIAccess::IndirectArgumentRead` is a combinable read-only buffer access.
  Its Vulkan state is draw-indirect stage plus indirect-command-read access; it
  is never lowered as a shader read. The exact record-sized range is the unit
  declared by RHI and RDG.
- The shared migration set is RHI command/resource/capability and validation,
  RenderCore RDG parameter/use planning, VulkanRHI startup/synchronization/native
  lowering, Renderer GBuffer preparation/execution, the Local Vertex Factory
  GPU-driven permutation, Slang culling/base-pass shaders, and their RHI,
  VulkanRHI, RenderCore, Renderer contract and qualification tests. Searches of
  Engine, Sandbox, and RoadWeaver found no existing command consumer of
  `DrawIndirect`; only Vulkan buffer-usage mapping pre-existed.
- The pilot admits opaque GBuffer draws only. A group key includes pass and
  render-target compatibility, effective graphics pipeline and material
  generation, vertex-factory type/layout, vertex/index buffer generations,
  index format/range, and every non-instance binding. Masked, translucent,
  shadow, hit-proxy, retained-forward, and editor-assistance draws remain direct.
- One candidate is `{PrimitiveId, LocalToWorld, WorldBoundsMin,
  WorldBoundsMax}` with finite world-space AABB bounds. One visible record is a
  32-bit candidate index. A single workgroup-safe atomic append compacts into a
  group-sized visible array; the counter and indexed argument instance count
  begin at zero, writes are bounds-checked, and `FirstInstance` remains the
  group's existing first-instance value. Initial per-view capacity is 65,536
  candidates and 4 MiB of generated storage, checked before allocation.
- Selection occurs before either culling or graphics recording. Policy-disabled,
  unsupported, below-threshold, incompatible, capacity, creation, and admission
  outcomes select the unchanged direct group and increment distinct telemetry.
  A failure after admitted GPU work is terminal for the graph and never redraws
  the group. The opt-in threshold is 64 compatible candidates; this is a
  diagnostic default, not permission for automatic production activation.
- Reference revision `8651c7bbe` uses
  `FGBufferQualificationTests.StaticAndSplinePassMeetsFrozenRTX3090TimingAndMemoryGates`
  and its deterministic color, depth, four GBuffer attachments, draw counters,
  and existing RTX 3090 budgets. The same registered fixture is the candidate
  comparison workload. A current-host correctness receipt is recorded below
  when available; no quiet-lane timing threshold is claimed by Stage 0.
- Current-host receipt: `./DevTool test GBufferQualificationTests --mode
  qualification` and `./DevTool test VulkanRHIIntegrationTests --mode
  qualification` pass on Apple M4 through MoltenVK. The GBuffer fixture includes
  the 64-instance partial-visibility direct/indirect comparison described in
  Current Status. Existing frozen RTX 3090 evidence remains the performance
  baseline; no quiet-lane candidate timing threshold is claimed.

### Stage 1: Implement indirect RHI and Vulkan execution

Dependencies: Stage 0.

Outcome: public command lists can record, retain, validate, and execute indirect
draw and dispatch work through Vulkan with exact access and capability behavior.

- [x] Add the argument records, capability fields, access state, error values,
  validation helpers, command records, command-list surface, and context replay
  operations selected in Stage 0.
- [x] Extend access compatibility, buffer-usage validation, diagnostic strings,
  command admission, payload accounting, move/finish/admit behavior, and inline
  and threaded replay oracles.
- [x] Publish Vulkan capabilities from activated physical-device features and
  limits; unsupported or unactivated features remain false even when the API
  version exposes their symbols.
- [x] Map `IndirectArgumentRead` to the correct synchronization2 and legacy
  stage/access masks and preserve exact buffer-range tracking and queue-family
  ownership validation.
- [x] Lower non-indexed draw, indexed draw, and dispatch commands to their native
  Vulkan operations without CPU readback, hidden GPU-idle waits, or unbounded
  native command expansion.
- [x] Cover malformed offsets, strides, counts, usage, state, bounds, pipeline
  domains, render-pass placement, missing capability, canceled recordings,
  retained resources, submission failure, and device teardown.
- [x] Update the lasting RHI command, transition, capability, graphics-state,
  synchronous-compute, diagnostics, and API-stability contracts that own the
  accepted behavior.

Completion: focused RHI tests and Vulkan integration tests prove bytes,
validation, state transitions, direct native execution, output/readback,
ownership, failure, and inline/threaded equivalence for all three operations.

### Stage 2: Integrate indirect resources with RDG

Dependencies: Stage 1 and the accepted single-queue path from the multi-queue
plan. Native split-event support is optional.

Outcome: a graph can generate argument and visible-instance buffers in compute
and consume them in graphics or compute with deterministic plans and correct
submission lifetime.

- [x] Add parameter/use vocabulary for exact indirect-argument ranges without
  weakening ordinary storage, shader-read, vertex, or index semantics.
- [x] Extend graph access validation, write/read dependency generation, barrier
  planning, capture/dump formatting, execution budgets, and external/final
  access handling for `IndirectArgumentRead`.
- [x] Exercise storage-write to indirect-read and storage-write to graphics-read
  transitions on one queue, shared-family queues, and distinct-family queues;
  preserve the full-barrier and graphics-queue fallbacks.
- [x] Prove culling, extraction, preparation failure, callback failure,
  cancellation, cross-graph resource pooling, and retirement retain every
  argument and visible-list use until its actual completion prerequisites pass.
- [x] Add a synthetic compute writer followed by indirect draw and a synthetic
  indirect-dispatch chain with deterministic readback or rendered output.

Completion: logical plans are deterministic, invalid graphs fail before
recording, valid graphs require no CPU synchronization, single-queue output is
unchanged, and enabled multi-queue execution retires and reuses buffers safely.

### Stage 3: Build the compatible-instance GPU culling pilot

Dependencies: Stage 2 and the completed common geometry-batch path used by the
selected mesh pass.

Outcome: one production pass can select an opt-in GPU path for compatible
static-mesh instance groups and render from compute-generated visibility and
arguments.

- [x] Add immutable candidate/group preparation with stable identities, exact
  compatibility keys, checked counts, bounds, transforms, and resource owners.
- [x] Add the typed global or mesh compute shader, reflected parameters,
  pipeline creation/recovery, counter initialization, frustum test, bounded
  compaction, indirect record publication, and deterministic zero-visible case.
- [x] Add the required vertex-factory/material permutation or shared binding
  path for visible-instance index and transform lookup without per-frame data in
  shader or PSO identity.
- [x] Author the RDG compute and graphics passes with exact parameter-derived
  uses, queue eligibility, diagnostic regions, timestamps, and fallback-safe
  resource allocation.
- [x] Route only the selected compatible opaque groups through indexed indirect
  draw; retain existing direct execution for every excluded pass, group, and
  failure outcome.
- [x] Publish per-view candidate, visible, culled, group, command, byte,
  overflow, fallback-reason, queue-assignment, and timing telemetry without
  logging every frame.

Completion: the production fixture renders through the indirect path when
explicitly enabled, uses the direct path otherwise, and exposes enough evidence
to explain every selection or fallback decision.

### Stage 4: Qualify parity, failure, lifetime, and fallback

Dependencies: Stage 3.

Outcome: indirect submission is behaviorally interchangeable with the selected
direct path within its declared compatibility domain and cannot lose, duplicate,
or prematurely recycle work.

- [x] Compare final color, scene depth, every active GBuffer attachment, and any
  selected shadow output for zero, partial, and full visibility; include camera
  boundary and large-world-coordinate cases relevant to current bounds math.
- [x] Cover one/many groups, first instance, base vertex, first index, instance
  ranges, nonzero argument offsets, exact-capacity and overflow inputs, empty
  buffers, and maximum admitted command counts.
- [x] Inject shader, PSO, argument/visible buffer allocation, metadata budget,
  command admission, submission, completion, resize, and resource-generation
  failures at their supported seams; verify either pre-record direct fallback or
  the established terminal failure outcome.
- [x] Exercise inline/threaded RHI, async policy on/off, graphics-queue fallback,
  available independent queues, validation on/off, repeated pool reuse,
  renderer recovery, and shutdown with pending work.
- [x] Verify unrelated completion cannot release candidate, visible, argument,
  descriptor, pipeline, or geometry owners needed by an outstanding indirect
  submission.
- [x] Run the affected registered native selections, required Vulkan GPU
  integration/qualification targets, and shared Engine API `all` build following
  the repository build and test guidance.

Completion: all selected outputs and counters agree with the direct reference,
failure never silently drops geometry, validation is clean, lifetime tests pass,
and unavailable hardware coverage remains explicitly recorded.

### Stage 5: Measure, select policy, and publish lasting contracts

Dependencies: Stage 4 and an admitted quiet timing lane for any claimed
performance threshold.

Outcome: measured evidence selects a bounded production policy or retains an
honest explicit opt-in, and temporary plan rules move to their owning Runtime
documents.

- [x] Operator-waived on 2026-10-18: complex reference/candidate timing and
  quiet-lane qualification. Correctness remains independently qualified above.
- [x] Retain correctness counters, captures, adapter and instrumentation
  metadata above. Repeated performance/retirement-latency sampling is operator-
  waived; no performance measurements are synthesized.
- [x] Retain the default-disabled explicit override and deterministic fallback.
  Automatic threshold selection is operator-waived and remains disabled; the
  64-candidate floor is tested as a structural admission rule.
- [x] Decide whether native multi-draw, bindless grouping, GPU Scene/Hi-Z, or
  transient aliasing has a measured next-step trigger. Do not implement those
  follow-ups in this plan.
- [x] Move the accepted command, RDG, geometry grouping, culling, telemetry,
  fallback, and performance-policy contracts into their owning Runtime and
  Development documents; remove any experimental API classification that has
  satisfied its promotion gates.
- [x] Validate changed documents and run all-plan validation for completion.
  Retain valid affected-test, GPU-gate, and required workspace-build receipts
  covering the final relevant inputs under the
  [result reuse policy](../../AGENTS.md#writing-rules); run missing or invalidated
  coverage, then record exact tested revisions, applicability to the final state,
  and completion provenance.

Completion: the feature has an evidence-backed activation policy or documented
opt-in status, lasting contracts own the implemented behavior, no unsupported
follow-up is implied, and every required validation gate is reproducible.

## Validation and Handoff

Use [agent build guidance](../../../Agents/BuildAndRun.md), [agent testing
guidance](../../../Agents/Testing.md), and the registered test catalog rather than
inferring target names. RHI/RDG CPU contracts, Renderer preparation contracts,
Vulkan integration, selected rendered-output qualification, and the workspace
`all` build are independently required according to the stage that changes
them.
Preserve these stage-specific acceptance gates; documentation-only closeout
does not require rerunning valid runtime coverage. Reused GPU or timing evidence
must still satisfy the gate's backend, hardware, workload, and environment conditions.

GPU correctness may proceed on an available supported host. Timing acceptance
follows [native-test qualification](../../../Development/Build/NativeTestQualification.md)
and requires an exclusive quiet lane. A missing adapter, queue topology, native
split-event implementation, or timing lane remains an explicit coverage gap;
it does not become a pass and does not justify weakening the graphics-queue,
full-barrier, or direct-draw fallback.

Each implementation commit updates `Last reviewed`, `Current Status`, and the
completed checklist items with exact evidence. Active-plan commits use
`Documentation/Plans/Archive/2026-10/IndirectSubmissionAndGPUCulling.md` as the `Plan` trailer
and the exact stage title as the `Stage` trailer.

## Related Documentation

- [RHI public API stability](../../../Runtime/Rendering/RHI/RHIPublicAPIStability.md)
- [RHI command execution](../../../Runtime/Rendering/RHI/RHICommandExecution.md)
- [RHI resource transitions](../../../Runtime/Rendering/RHI/RHIResourceTransitions.md)
- [RHI resource views and transfers](../../../Runtime/Rendering/RHI/RHIResourceViewsAndTransfers.md)
- [RHI capabilities and Vulkan startup](../../../Runtime/Rendering/RHI/RHICapabilitiesAndVulkanStartup.md)
- [Graphics state and bindings](../../../Runtime/Rendering/RHI/GraphicsStateAndBindings.md)
- [Synchronous compute pipelines](../../../Runtime/Rendering/RHI/SynchronousComputePipelines.md)
- [Render Graph](../../../Runtime/Rendering/Pipeline/RenderGraph.md)
- [Static mesh rendering](../../../Runtime/Rendering/Scene/StaticMeshRendering.md)
- [Renderer frame preparation](../../../Runtime/Rendering/Pipeline/RendererFramePreparation.md)
- [RDG and RHI multi-queue execution plan](RdgRhiMultiQueueExecution.md)
- [Geometry submission refactor plan](GeometrySubmissionRefactor.md)
- [Rendering performance baseline](../../../Development/Build/RenderingPerformanceBaseline.md)
