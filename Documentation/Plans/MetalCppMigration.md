# Metal-cpp Migration Plan

Summary: Migrate the existing MetalRHI backend from Objective-C Metal calls and ARC ownership to Apple Metal-cpp and explicit C++ RAII while preserving observable RHI behavior.

Last reviewed: 2026-10-05

Status: Completed
Completed: 2026-10-04

## Current Status

Stages 0–5 are accepted and the migration is complete. Native resources,
shaders, pipelines, command encoding, completion and viewport operations compile
as C++ with explicit RAII. Stage 5 originally retained a small Objective-C++
adapter for untyped Cocoa layer handles. The 2026-10-05 follow-up replaces those
handles with a typed native layer boundary and removes the adapter. The later
platform-boundary follow-up moves that typed layer into the macOS-specific
presentation target; common interfaces now carry `FRHIPresentationTarget`.
MetalRHI compiles entirely as C++; stage receipts below retain
the original migration evidence. Capabilities, shader routes/identity and expected
GPU output remain unchanged.

Original migration qualification passed 37 headless cases (Metal API validation enabled),
17 production shader/material cases in their baseline environment, and the
workspace `all` build across Engine, Sandbox and RoadWeaver. Lasting rules are in
[RHI implementation documentation](../Runtime/Rendering/RHICapabilitiesAndVulkanStartup.md#metal-native-ownership-and-cocoa-boundary).
Stage receipts and diagnostic limits are recorded below. Native window/application
presentation remains the separate open gate identified in Stage 0 and owned by
[Metal RHI](MetalRHI.md); it is not claimed as passing migration evidence.

## Goal and Scope

Complete the API and ownership migration for existing MetalRHI functionality.
Keep backend-neutral RHI interfaces, supported capabilities, shader routes,
resource descriptors, synchronization, failure behavior, and rendered/readback
output unchanged. Keep native types private to MetalRHI.

The [Metal RHI plan](MetalRHI.md) owns feature delivery, platform qualification,
shader-route decisions, and production adoption. This independent plan owns
only the refactor; its completion does not close that plan's acceptance gates.
New features, wider hardware support, performance optimization, and changing
the default backend are outside scope.

## Selected Approach

- Migrate complete ownership boundaries in small, independently validated
  commits. Keep `.mm` during intermediate steps; rename or extract to `.cpp`
  only after the relevant code no longer requires Objective-C syntax.
- Use `NS::SharedPtr<T>` for owned native objects and raw `T*` for documented
  borrows. Use `NS::TransferPtr` for an existing owned reference and
  `NS::RetainPtr` when acquiring ownership of a borrowed/autoreleased object.
  Do not use default-deleting `std::shared_ptr` for native Metal objects.
- Temporary ARC/C++ interoperability may retain separate references, but every
  owning reference must have exactly one release responsibility. Centralize
  bridges, name whether they borrow or transfer, and remove temporary bridges
  as their consumers migrate. Never adopt an ordinary borrowed bridge with
  `TransferPtr`.
- Preserve autorelease pools on relevant worker, pipeline compilation, replay,
  and callback scopes. Pool drainage is not a GPU completion signal.
- Convert descriptor construction explicitly against the pinned headers:
  setters, descriptor-array access, enums, defaults, error outputs, and null
  handling. Avoid whole-file mechanical substitutions.
- Keep `NS_PRIVATE_IMPLEMENTATION`, `CA_PRIVATE_IMPLEMENTATION`, and
  `MTL_PRIVATE_IMPLEMENTATION` defined in exactly one translation unit.
- Preserve submission owners through actual GPU use. CPU replay completion,
  native object reference counting, and backing-memory retirement are distinct
  responsibilities, including for buffers backed by externally owned memory.

## Implementation Stages

### Stage 0: Record ownership boundaries and the validation baseline

Dependencies: current MetalRHI source and the prepared pinned dependency.

- [x] Inventory native fields and creation paths in device/queue, resource,
  pipeline, command, submission, deferred backing, and viewport objects. Record
  each as owned, borrowed, or autoreleased, with its required lifetime.
- [x] Inspect the pinned headers for smart pointers, completion-handler
  overloads, descriptor arrays, and QuartzCore support. Record necessary
  compiler flags and remaining Objective-C interoperability requirements.
- [x] Identify existing qualification coverage and run the relevant baseline
  in inline and threaded modes. Record coverage gaps separately from failures.
- [x] Map the tests needed for pool drainage, delayed completion, readback,
  creation failure, and shutdown with work outstanding to later stages.

Acceptance: an ownership inventory and a reproducible baseline receipt exist;
the first migration has a bounded set of fields, consumers, and tests.

### Stage 1: Move device, queue, and buffer ownership to C++

Dependencies: Stage 0 accepted.

- [x] Return RAII-owned device/queue values from the creation helper; remove
  the detach-to-ARC handoff and migrate long-lived RHI/submission-state owners.
- [x] Migrate buffer creation and `FMetalBuffer` storage to Metal-cpp and RAII;
  expose only a borrowed native handle from resource accessors.
- [x] Adapt existing command consumers with localized, temporary borrowed
  bridges. Audit deferred buffer backing and external storage owners without
  changing submission retirement semantics.
- [x] Verify initialization failure cleanup, buffer upload/copy/readback,
  release of caller references before GPU completion, and queue shutdown.

Acceptance: devices, queues, and buffers have explicit C++ owners; buffer
outputs match the baseline in both execution modes, with no early release or
new retained-object growth under repeated creation and teardown.

### Stage 2: Migrate samplers, textures, and texture views

Dependencies: Stage 1 accepted.

- [x] Migrate sampler creation and ownership, then textures and texture views,
  including viewport backing textures and temporary transfer resources.
- [x] Isolate descriptor translation in focused helpers and preserve support
  checks, allocation reporting, null results, and recoverable creation errors.
- [x] Verify currently supported dimensions, mip/layer/subresource operations,
  uploads, copies, views, and readbacks against existing coverage.
- [x] Exercise view/source lifetime and pool drainage while GPU work remains
  outstanding; retain any owner required by the existing RHI contract.

Acceptance: resource wrappers and creation paths use Metal-cpp; supported
transfer outputs and unsupported-descriptor behavior match the baseline.

### Stage 3: Migrate shaders and pipeline construction

Dependencies: Stage 2 accepted.

- [x] Migrate library/function and compute pipeline ownership and creation.
- [x] Migrate graphics pipeline, vertex layout, and depth/stencil descriptors
  field by field, preserving binding maps and error diagnostics.
- [x] Preserve autorelease pools for asynchronous pipeline work and ensure
  diagnostic text outlives any borrowed native error object when necessary.
- [x] Verify successful and failed creation, asynchronous compilation/cache
  teardown, pipeline replacement, and existing representative draw/dispatch
  outputs without changing shader generation or artifact identity.

Acceptance: pipeline creation and stored native state use C++ RAII; existing
draw/compute results and complete-or-failure publication behavior are preserved.

### Stage 4: Migrate command encoding and submission lifetime

Dependencies: Stage 3 accepted.

- [x] Migrate command buffers, render/compute/blit encoders, render-pass
  descriptors, and draw/dispatch/copy operations through bounded substeps.
- [x] Replace native resource arrays with owning C++ containers while retaining
  replay storage, RHI resource owners, deferred backing, and readback buffers.
- [x] Move completion handling to the pinned C++ API with an independently
  owned completion payload. Avoid capturing a destructible raw backend pointer
  or forming a command-buffer/callback ownership cycle.
- [x] Preserve timeline publication order, completion notification, failure,
  cancellation, timeout, and shutdown behavior. Audit all completion handlers,
  including transfer and presentation paths, rather than only normal submit.
- [x] Exercise empty submissions, delayed completion, pool drainage, upload
  reuse, readback success/failure, and teardown with outstanding callbacks in
  inline and threaded modes. Label simulated failure evidence explicitly.

Acceptance: CPU replay cannot release storage still needed by GPU work;
readbacks complete only from valid GPU completion, terminal paths release their
owners, and the existing submission contract remains intact.

### Stage 5: Isolate Cocoa interoperability and finish C++ extraction

Dependencies: Stage 4 accepted.

- [x] Migrate supported layer/drawable operations through Metal-cpp QuartzCore
  wrappers; retain a small `.mm` adapter for necessary Cocoa window integration,
  native object validation, and explicit ownership bridges.
- [x] Verify drawable acquisition failure, present completion, resizing,
  repeated viewport creation/destruction, and shutdown. Keep window ownership
  with its existing platform owner.
- [x] Extract the native C++ implementation into cohesive private files and
  update module/CMake source registration. Restrict ARC flags to remaining
  Objective-C++ files and remove obsolete temporary bridges and imports.
- [x] Run the full affected Metal qualification set and relevant project
  builds; inspect the remaining Objective-C usage and document its purpose.
- [x] Move lasting ownership/interop rules into the owning implementation
  documentation and record final evidence and remaining qualification gaps.

Acceptance: core MetalRHI resources, pipelines, commands, and submission compile
as C++; Objective-C++ is limited to justified Cocoa interoperability. Existing
headless and available presentation workloads retain their baseline behavior.
Unavailable presentation evidence remains an open gate, not a passing result.

## Validation and Handoff

Follow [build and run guidance](../Agents/BuildAndRun.md),
[native-test guidance](../Agents/Testing.md), and the
[documentation workflow](../Agents/Documentation.md). Use the host DevTool
launcher and existing qualification targets; do not copy stale command lines
into this plan. Do not overlap build process trees.

For each stage, record revision, pinned dependency version, SDK/compiler,
OS/GPU, execution modes, workloads, report paths, output comparisons, and
coverage gaps. Compilation alone does not establish ownership correctness.
Use available Metal validation and lifetime diagnostics for repeated teardown
and outstanding-work cases. Keep unimplemented or unverified checks open.

Each implementation commit updates this plan's status/checklists and uses this
file and the exact implemented stage title for `Plan` and `Stage` trailers.
Prefer backend-private changes. If a shared API must change, migrate all
consumers in `Durin.dworkspace` and complete the required `all` build before
handoff, as required by the repository rules.

## Stage 0 Evidence

### Ownership Inventory

The baseline source is `MetalDynamicRHI.mm` and the private `MetalBuffer.h`,
`MetalTexture.h`, `MetalSampler.h`, and `MetalCppDevice.*` wrappers. Objective-C
object members and local `id` variables below are strong ARC owners unless
explicitly described as borrowed. Accessors return borrowed native handles;
assigning those handles to a strong ARC local or array acquires another owner.

| Boundary | Creation and ownership at baseline | Required lifetime / migration owner |
| --- | --- | --- |
| Device / queue | `CreateSystemDefaultDevice` and `newCommandQueue` produce owned references. The helper adopts them with `TransferPtr`, detaches them, and `Init` transfers them to ARC. `FMetalDynamicRHI` owns both; `FMetalSubmissionState` separately owns the queue. | Backend initialization through pipeline shutdown and final callback drain. Stage 1 returns and stores `SharedPtr` values; submission state retains its own queue owner. |
| Buffer | `newBufferWithLength` creates an owned shared-storage buffer; `FMetalBuffer` owns it. `GetHandle` borrows it. | Wrapper lifetime plus native submission ownership through GPU completion. Stage 1 adopts creation with `TransferPtr` and stores `SharedPtr`. |
| Deferred buffer | `FMetalDeferredBacking` owns a copied `newBufferWithBytes` buffer and its immutable snapshot. Snapshot backend cache shares the backing; active submission `StorageOwners` retains it. | Exact immutable version through GPU completion. No no-copy/external-memory Metal buffer creation exists at baseline. Stage 1 preserves this retirement contract; Stage 4 migrates native storage. |
| Sampler | `newSamplerStateWithDescriptor` returns an owned state stored by `FMetalSampler`; shader parameter owners retain the RHI wrapper. | Wrapper and bound submission lifetime. Stage 2 uses `TransferPtr`. |
| Texture / view | `newTextureWithDescriptor` and `newTextureViewWithPixelFormat` return owned textures stored in wrappers. `FRHITextureView` also owns its parent RHI texture. Allocation bytes come from the native texture. | Source/view wrapper and GPU submission lifetime, including layer/mip views. Stage 2 uses `TransferPtr` and preserves the parent owner. |
| Shader / pipelines | `newLibraryWithSource`, `newFunctionWithName`, render/compute pipeline and depth/stencil creation return owned objects. Shader/pipeline wrappers own native state and the dependent RHI shaders/declaration. | Complete factory result through pipeline replacement and submission completion; async cache work finishes before backend device release. Stage 3 uses RAII candidates and copies error text while its error is valid. |
| Descriptors / errors | Convenience texture/render-pass descriptors and strings are autoreleased; `new` pipeline/vertex/depth/sampler/options descriptors are ARC-owned. Descriptor arrays and entries are borrowed from their parent. `NSError` output is borrowed/autoreleased and retained by ARC locals. | Factory/encoding scope; retain only when crossing a pool or async boundary. Stages 2–4 translate every setter/array access and preserve defaults. |
| Recording / submission | Queue `commandBuffer`, render/compute/blit encoders are autoreleased; ARC fields/locals extend their lifetime. `FMetalPendingSubmission` owns command, RHI resources, replay storage, native-resource array, and readback buffers. Completion blocks capture independent shared state and moved storage/resource/readback owners plus a copied native array. | CPU replay may finish before GPU use. Stage 4 retains autoreleased objects with `RetainPtr`, keeps completion owners independent of the command, and preserves timeline publication after readback processing. |
| Transfer temporaries | Upload, readback, deferred uniform, and push-constant `newBuffer` paths produce owned buffers. Active submissions retain them in native arrays/readbacks; standalone transfers capture owners in completion blocks or wait synchronously. | Until the exact consuming GPU operation completes, including standalone transfer paths. Stages 2/4 migrate these paths and all five completion-handler sites. |
| Viewport / presentation | `FMetalViewport` strongly owns device, layer, and backing RHI texture. Startup layer is a strong ARC owner acquired from a borrowed platform handle. `nextDrawable` is autoreleased; strong locals and the presentation completion block retain drawable/backbuffer/storage. Drawable texture is borrowed. Native window is a raw borrow. | Platform owner keeps the window alive; native presentation owners survive GPU completion and resize. Stage 5 retains layer/drawable with `RetainPtr` and confines validation/bridging to Cocoa adapter. |

### Pinned API and Compiler Inspection

- Prepared dependency and manifest both identify
  `27c4382b7151d55a51692cdcb27aaa98752240de` at
  `Engine/External/Source/metal-cpp`. No dependency update is needed.
- `Foundation/NSSharedPtr.hpp` copies by retaining, moves ownership, releases
  on destruction/reset, and exposes raw `get()` borrows. `TransferPtr` adopts
  an owned reference; `RetainPtr` extends a borrowed/autoreleased reference.
- `MTLCommandBuffer.hpp` provides block and `std::function` completion-handler
  overloads; the function overload copies its callable into a native block.
  C++ files including Metal headers therefore require Clang blocks support
  (`-fblocks`). C++23 is already selected. Keep implementation macros in
  `MetalCppDevice.cpp` alone and link Metal, Foundation, and QuartzCore.
- Render-pass, vertex-layout/attribute, and pipeline color-attachment arrays
  expose `object(index)` and setter methods. Their entries borrow the parent
  descriptor; do not adopt them with `TransferPtr`.
- QuartzCore supplies `CA::MetalLayer` device, format, drawable size, timeout,
  and `nextDrawable` operations, and `CA::MetalDrawable::texture`. Cocoa class
  validation of opaque platform handles still requires an Objective-C++
  adapter. ARC is currently scoped to `MetalDynamicRHI.mm` and native tests.
- Async pipeline jobs already have autorelease pools. Replay/factory and
  callback scopes need explicit pool coverage during C++ extraction; pool
  drainage never retires a submission.

### Baseline Receipt and Coverage Map

Baseline revision: `2d0b31e5499a6eb39a9bdc93d00d68986b947ad7`, clean checkout.
Host: macOS 27.0.1, Apple M4, Apple Clang 21.0.0
(`clang-2100.1.1.101`), Xcode macOS SDK 26.5, arm64 Debug DurinEditor,
profile `macos-xcode-arm64`, preset `MacOS-arm64-Debug-DurinEditor`.
Qualification ran outside the sandbox through the host DevTool launcher.

| Selection | Result / receipt |
| --- | --- |
| `./DevTool test MetalRHIHeadlessTests --mode qualification --report` | Passed 2026-10-04 22:00 CST; CTest receipt `Build/NativeTestResults/MacOS-arm64-Debug-DurinEditor/MetalRHIHeadlessTests.xml`; build/CTest logs `Build/.agent-state/logs/20261004-220027-339239-13263-cmake.log` and `20261004-220027-468566-13263-ctest.log`. Existing paired inline/threaded device, buffers, transfers, compute, draws, viewports, and byte/pixel assertions pass. |
| `./DevTool test MetalShaderQualificationTests --mode qualification --report` | Passed 2026-10-04 22:01 CST, 17 cases; CTest receipt `Build/NativeTestResults/MacOS-arm64-Debug-DurinEditor/MetalShaderQualificationTests.xml`; logs `Build/.agent-state/logs/20261004-220040-259611-13282-cmake.log` and `20261004-220126-307448-13282-ctest.log`. Production shader/material compile and checked-pixel workloads pass. |

The first headless invocation omitted required qualification mode: its build
passed but the launcher rejected test execution. It is not a GPU failure and
is superseded by the successful exact qualification invocation above.

| Later stage | Existing coverage and additional ownership evidence needed |
| --- | --- |
| 1 | `DeviceAndSingleQueueInitializeInBothExecutionModes`, `UploadThenCopyRetainsBuffersUntilGPUCompletion`, `DeferredUniformVersionsReachSeparateDispatches`, invalid descriptors, buffer views, and dispatch output cover initialization, buffer bytes, caller release and shutdown. Add repeated creation/teardown, candidate cleanup, and pool-drain lifetime evidence. |
| 2 | Texture dimension/mip/layer/volume/block-row, pitched upload, offset/pitch copies, view binding, and round-trip/readback cases cover supported outputs. Add source/view release across pool drainage with outstanding GPU work. |
| 3 | Shader validation, graphics/depth/compute draw cases and production material qualification cover successful creation and checked pixels. Add failed native compilation, async cache teardown and pipeline replacement lifetime evidence. |
| 4 | Frame-end/empty submission, pending-work and readback cancellation, deferred versions, transfers and readback completion cover normal publication. Add deterministic delayed completion, pool drainage, upload reuse, submitted shutdown, terminal readback failure and callback-owner release; label injected failures as simulated. |
| 5 | Layer viewport test acquires a real drawable, clears/presents, reads backing pixels, resizes, recreates, and shuts down in both modes. Add unavailable drawable and repeated viewport/presentation teardown. Native window/application-host presentation remains a separate qualification gate in the Metal RHI plan. |

Baseline uses existing deterministic byte/pixel assertions, including Metal /
Vulkan authored ImGui and hit-proxy parity, rather than creating new output
goldens. Baseline has no retained-object growth measurement, forced GPU delay,
native allocation-failure injection, async cache stress, or terminal GPU-error
injection. These are coverage gaps, not passing lifetime results. Later stage
receipts must record the evidence used to close them.

## Stage 1 Evidence

Implementation base: `86c3d1f28`; dependency, SDK/compiler, OS/GPU and preset
match Stage 0. `MetalCppDevice.*` returns RAII device/queue owners without
detaching; backend, submission state, and viewport store C++ owners.
`FMetalBuffer` exposes a raw C++ borrow and stores `SharedPtr`. All native buffer
creation paths, including deferred versions, upload/readback and push constants,
adopt `newBuffer` results with `TransferPtr`. No external/no-copy Metal buffer
path exists. Temporary ARC locals and submission arrays acquire independent
references through `MetalObjCBridge.h`; GPU retirement semantics are preserved.

The new initialization-unwind test releases 16 real candidate queues after a
simulated post-creation exception. The repeated ownership test runs 16
create/upload/copy/shutdown rounds per execution mode, drains the recording
pool after caller source release, checks pending ownership and output bytes,
and confirms both native buffer weak references are nil after shutdown.
Native queue-allocation failure is not injected; candidate exception evidence
is explicitly simulated. System-device singleton deallocation is not asserted.

Validation on 2026-10-04 CST:

- Full `MetalRHIHeadlessTests` qualification passes all 31 cases with original
  byte/pixel expectations (receipt
  `Build/NativeTestResults/MacOS-arm64-Debug-DurinEditor/MetalCppStage1.xml`;
  build/CTest logs `20261004-221849-365785-18312-cmake.log` and
  `20261004-221852-193743-18312-ctest.log` under `Build/.agent-state/logs`).
- Production shader/material qualification passes all 17 cases (receipt
  `Build/NativeTestResults/MacOS-arm64-Debug-DurinEditor/MetalCppStage1Shaders.xml`;
  logs `20261004-221718-253044-18145-cmake.log` and
  `20261004-221720-612998-18145-ctest.log`).
- The two new lifetime cases pass with `MTL_DEBUG_LAYER=1` and
  `OBJC_DEBUG_MISSING_POOLS=YES` (receipt
  `Build/NativeTestResults/MacOS-arm64-Debug-DurinEditor/MetalCppStage1Lifetime.xml`;
  logs `20261004-221509-784143-17920-cmake.log` and
  `20261004-221515-103232-17920-ctest.log`). Buffer/queue weak references show no
  surviving owners across these rounds. This is native-object release evidence,
  not a whole-process memory benchmark.

### Diagnosed Changes and Remaining Diagnostics

Pool coverage is advanced from later stages because validation exposed missing
pools on RHI workers and retained debug command/encoder objects.
`FMetalAutoreleasePool` now scopes native factory/encoding/callback entrypoints
and C++ buffer/queue release. A temporary completion-array owner scopes its
release even when the driver destroys the copied callback after its body
returns; Stage 4 replaces that ARC container with owning C++ resources.

Residual missing-pool messages concern the device singleton on Metal's
`com.Metal.CompletionQueueDispatch` thread. LLDB stopped at
`objc_autoreleaseNoPool`: the stack is `IOGPUMetalBuffer dealloc` ->
`AGXBuffer dealloc` -> `MTLResourceListChunkFreeEntries` ->
`IOGPUMetalCommandBufferStorageDealloc` -> driver completion dispatch, after
the application's completion scope. These driver-internal messages remain a
platform diagnostic limitation; the focused checks pass and buffers/queues
release. They must not be reported as a clean missing-pool diagnostic run.

Full API validation also rejects the baseline graphics path's nil disabled
depth/stencil state (`setDepthStencilState: nil`). Stage 3 must provide an
equivalent explicit disabled state and rerun full API validation. The full
validation-layer attempt is a failed run, not substituted with a passing result.

Pool drainage exposed a pre-existing indexed-indirect fixture overflow:
one-index binding offset plus `FirstIndex = 1` read three indices from a
four-index array. The fixture now has two padding indices and three valid
triangle indices; direct draw skips both padding indices, while indirect draw
combines binding offset with `FirstIndex`. Expected pixels and shader source
remain unchanged, and the complete target passes.

## Stage 2 Evidence

Implementation base: `f669bc5c6`; dependency and host/toolchain match Stage 0.
Sampler, texture, and native texture-view wrappers store C++ owners and expose
raw borrows; their release is pool-scoped. View wrappers continue to retain the
source RHI texture. `MetalResourceDescriptors.cpp` translates every prior texture
and sampler field explicitly, uses one pixel-format map, retains autoreleased
texture descriptors, and adopts allocated sampler descriptors and created
textures/views. Viewport backing creation uses the same descriptor helper.
Stage 1 already migrated temporary transfer buffer creation. Support checks,
allocation reporting, recoverable errors and null publication remain intact.

The new `CppViewAndSourceSurvivePoolDrainWithDelayedNativeWork` test runs eight
rounds per execution mode. It completes a public upload, obtains a counted view,
releases the caller's source reference and drains its creation pool, then submits
a real native copy behind an unsignaled shared event on a separate qualification
queue. It drops the view and drains deferred RHI deletion while GPU work remains
blocked, verifies native source/view liveness, signals the event, compares all
64 readback bytes, and checks weak references after teardown. This tests resource
ownership with actual delayed GPU execution; Stage 4 still owns delayed
completion testing of the backend submission payload.

Validation on 2026-10-04 CST:

- Full `MetalRHIHeadlessTests` qualification passes all 32 cases in the existing
  mode matrix, including dimension/mip/layer/volume/block-row transfers, native
  view types, sampler binding, unsupported descriptors, viewport backing and
  checked draw/compute bytes/pixels. Receipt
  `Build/NativeTestResults/MacOS-arm64-Debug-DurinEditor/MetalCppStage2.xml`; logs
  `20261004-222812-285266-20121-cmake.log` and
  `20261004-222817-073142-20121-ctest.log` under `Build/.agent-state/logs`.
- Four focused sampler/transfer/lifetime cases pass with `MTL_DEBUG_LAYER=1` and
  missing-pool diagnostics. Receipt
  `Build/NativeTestResults/MacOS-arm64-Debug-DurinEditor/MetalCppStage2Lifetime.xml`;
  logs `20261004-222852-202721-20898-cmake.log` and
  `20261004-222852-448565-20898-ctest.log`. Stage 1's driver-origin missing-pool
  diagnostic limitation still applies; full graphics API validation awaits
  the Stage 3 disabled depth/stencil state.
- Production shader/material qualification passes all 17 cases. Receipt
  `Build/NativeTestResults/MacOS-arm64-Debug-DurinEditor/MetalCppStage2Shaders.xml`;
  logs `20261004-222907-279632-21150-cmake.log` and
  `20261004-222911-316649-21150-ctest.log`. Expected bytes/pixels match the baseline.

Native memory exhaustion is not forced; descriptor rejection and nullable
creation semantics are covered by existing cases and RAII candidate cleanup.
No new dimensions, shader routes, native memory policy or RHI interfaces are added.

## Required References

- [RHI command execution](../Runtime/Rendering/RHICommandExecution.md)
- [RHI resource transitions](../Runtime/Rendering/RHIResourceTransitions.md)
- [RHI resource views and transfers](../Runtime/Rendering/RHIResourceViewsAndTransfers.md)
- [RHI diagnostics and conformance](../Runtime/Rendering/RHIDiagnosticsAndConformance.md)
- [Graphics state and bindings](../Runtime/Rendering/GraphicsStateAndBindings.md)
- [Viewport rendering](../Runtime/Rendering/ViewportRendering.md)
- [Apple Metal-cpp ownership and interoperability guidance](https://developer.apple.com/videos/play/wwdc2022/10160/)

The pinned local headers remain authoritative for API availability in this
refactor; upstream documentation does not justify a dependency upgrade.

## Stage 3 Evidence

Accepted on 2026-10-04 against the Stage 2 parent `6f60cacf4`, with the same
pinned dependency, host, SDK and compiler as Stage 0.

- `MetalPipeline.cpp` owns shader/library/function creation and graphics/compute
  pipeline construction. Every created native candidate is adopted exactly once;
  wrapper getters return borrows. Vertex declarations and pipeline RHI dependency
  references retain their prior semantics. Descriptor fields, remaps, keys,
  language version, source/hash validation and native-stage checks are preserved.
- Native error descriptions are copied into `std::string` inside the compilation
  pool. Asynchronous cache callbacks and native-owner destructors use C++ pool
  scopes. Temporary command consumers borrow native pipeline/depth state.
- Color-only passes now create an explicit Always/no-write depth state. This
  preserves disabled depth behavior and resolves the Stage 1 Metal validation
  assertion caused by `setDepthStencilState:nil`.
- The shader validation case now also exercises native MSL compilation failure
  and native function frequency mismatch. The new pipeline lifetime case performs
  eight create/cache/replace/shutdown rounds in each execution mode, drains pools,
  checks native function/pipeline weak references, preserves ready results through
  cache stop, and verifies outstanding requests reach Ready or Canceled before
  resource release. Stop may cancel work before native creation; the initial test
  incorrectly required every outstanding request to succeed and was corrected.
- Full headless qualification passes all 33 cases with `MTL_DEBUG_LAYER=1`:
  `Build/NativeTestResults/MacOS-arm64-Debug-DurinEditor/MetalCppStage3.xml`;
  logs `Build/.agent-state/logs/20261004-224057-617288-23277-cmake.log` and
  `Build/.agent-state/logs/20261004-224104-226923-23277-ctest.log`.
- Production shader qualification with `MTL_DEBUG_LAYER=1` aborts on an existing
  direct-native fixture: `main0` expects 96 uniform bytes but the fixture allocates
  80. Receipt log: `Build/.agent-state/logs/20261004-224125-756735-23350-ctest.log`.
  This target does not use the backend's shader/pipeline factories for that case;
  the additional validation failure is recorded separately from migration output.
- Baseline-environment production shader qualification passes all 17 cases:
  `Build/NativeTestResults/MacOS-arm64-Debug-DurinEditor/MetalCppStage3Shaders.xml`;
  logs `Build/.agent-state/logs/20261004-224224-286999-23418-cmake.log` and
  `Build/.agent-state/logs/20261004-224224-438362-23418-ctest.log`. No production
  source/artifact identity or expected pixel/byte changes were made.
- Final formatting/comment-only MetalRHI rebuild passes; log
  `Build/.agent-state/logs/20261004-224235-138665-23448-cmake.log`.

## Stage 4 Evidence

Accepted on 2026-10-04 against the Stage 3 parent `c6ae03c19`, using the same
pinned headers, host, SDK and compiler as Stage 0.

- Render-pass/color/depth descriptors, render/compute/blit encoders, command
  buffers, bindings, direct/indirect draw and dispatch, copies, uploads, readback,
  and presentation command operations use the pinned C++ API. Autoreleased
  command/encoder objects are retained; native resources use owning C++ vectors.
- `MetalSubmission.h/.cpp` defines the independently owned completion payload and
  production completion routine. All five native handlers use `MTL::HandlerFunction`
  with owned state/payload captures, no raw backend capture and no command-buffer
  cycle. Replay storage, deferred backing snapshots, RHI dependencies, native
  buffers/textures and readback allocations retain their existing responsibilities.
- Terminal callbacks release payload dependencies before timeline/callback-counter
  publication. Shutdown flushes RHI deferred deletions after its callback drain.
  The actual delayed-work test exposed references being enqueued for deletion
  after the earlier outer shutdown flush; these changes resolve that release gap.
- The new delayed-work case gates the backend queue with a native shared event,
  releases caller shader/pipeline/buffer/view/texture references and drains pools,
  checks Pending readback and timeout, then validates readback bytes and buffer
  output. Both execution modes cover ordinary completion and shutdown with work
  outstanding. Native weak references become nil after teardown. Its private
  borrowed-queue seam is confined to MetalRHI qualification/diagnostics.
- A first attempted fixture used CPU writes to an atomic shared buffer to wake
  GPU polling. That memory-visibility assumption proved unreliable and the fixture
  was replaced with the native shared event; it is not passing evidence.
- Empty submissions complete across 16 rounds per execution mode. A separate test
  injects `CommandBufferStatusError` into the production completion routine and
  verifies failed readback/timeline, drained callback count and payload/storage
  release. This is explicitly simulated status evidence, not a physical GPU fault.
- Full headless qualification passes all 36 cases with `MTL_DEBUG_LAYER=1`:
  `Build/NativeTestResults/MacOS-arm64-Debug-DurinEditor/MetalCppStage4.xml`;
  logs `Build/.agent-state/logs/20261004-225733-236499-24218-cmake.log` and
  `Build/.agent-state/logs/20261004-225737-276472-24218-ctest.log`.
- Focused lifetime/empty/simulated-error qualification passes with API validation:
  `Build/NativeTestResults/MacOS-arm64-Debug-DurinEditor/MetalCppStage4Lifetime.xml`;
  logs `Build/.agent-state/logs/20261004-225644-051084-24148-cmake.log` and
  `Build/.agent-state/logs/20261004-225649-541606-24148-ctest.log`.
- Baseline-environment production shader qualification passes all 17 cases:
  `Build/NativeTestResults/MacOS-arm64-Debug-DurinEditor/MetalCppStage4Shaders.xml`;
  logs `Build/.agent-state/logs/20261004-225800-621918-24277-cmake.log` and
  `Build/.agent-state/logs/20261004-225801-571417-24277-ctest.log`. Stage 3's separate
  direct-native uniform fixture validation failure remains recorded.

## Stage 5 Evidence and Final Handoff

Accepted on 2026-10-04 against the Stage 4 parent `2ac9caf85`, with the same
pinned dependency, macOS/M4 host, SDK/compiler, profile and preset as Stage 0.

### Implementation and ownership audit

- `MetalDynamicRHI.cpp`, `MetalCommandContext.cpp`, resource-descriptor,
  pipeline and submission implementations compile as C++. Viewport ownership
  is a cohesive private wrapper. Core sources compile independently of the PCH;
  `-fblocks` remains required by the pinned headers' handler overloads.
- Platform-specific implementations/headers live in `Private/MacOS` so ordinary
  module source discovery excludes native C++ code on other platforms. The
  pre-existing `MetalCppDevice.cpp` implementation entry remains at its original
  path, guarded by `__APPLE__`; the unsupported-platform module stays intact.
  The Metal native-test private include path follows this layout. No shared RHI
  API or other project's source consumer requires migration. Non-macOS execution
  was not performed on this host.
- `MetalCppDevice.cpp` remains the sole definition site of all three private
  implementation macros. Source audit finds Objective-C imports, class messages
  and ownership bridges only in the 15-line `MetalCocoaInterop.mm`. ARC flags
  apply only to that adapter in the backend. Temporary command/resource bridges
  and the old `MetalDynamicRHI.mm` are removed.
- The adapter checks the borrowed opaque handle with Cocoa `isKindOfClass`, then
  `RetainPtr` acquires a separate layer reference. Startup/viewport storage uses
  `SharedPtr<CA::MetalLayer>`; drawable acquisition retains its autoreleased
  result. Device/format/size/vsync configuration and presentation use C++ wrappers.
  The window handle remains borrowed. Destructor pools cover layer/device owners,
  including partially initialized backend instances.
- Native color-format support translation is shared by backend support queries,
  render-pass checks and pipeline factories. Its admitted formats are unchanged.
- Lasting ownership, pool, payload-retirement and Cocoa-boundary rules moved to
  the owning [RHI implementation contract](../Runtime/Rendering/RHICapabilitiesAndVulkanStartup.md#metal-native-ownership-and-cocoa-boundary).

### Final validation receipts

- Final `MTL_DEBUG_LAYER=1` headless qualification passes all 37 cases, including
  inline/threaded byte/pixel comparisons, caller release, delayed GPU completion,
  shutdown, failure/cancellation, native sampler/view binding, graphics/depth and
  indirect operations:
  `Build/NativeTestResults/MacOS-arm64-Debug-DurinEditor/MetalCppStage5Final.xml`;
  logs `Build/.agent-state/logs/20261004-231153-015518-25939-cmake.log` and
  `Build/.agent-state/logs/20261004-231203-224685-25939-ctest.log`.
- Final baseline-environment production shader/material qualification passes
  all 17 cases with existing sources, artifact identities and expected pixels:
  `Build/NativeTestResults/MacOS-arm64-Debug-DurinEditor/MetalCppStage5FinalShaders.xml`;
  logs `Build/.agent-state/logs/20261004-231245-251985-26121-cmake.log` and
  `Build/.agent-state/logs/20261004-231246-970926-26121-ctest.log`.
- The new viewport case validates null/wrong Cocoa handles and runs four
  create/present/teardown rounds per execution mode. It injects one nil drawable,
  then requires an actual drawable on retry, drains backend GPU callbacks, and
  checks layer weak references after Cocoa transaction/run-loop drainage. The
  initial immediate weak check failed because Core Animation had not drained;
  this fixture now performs the platform drainage rather than treating GPU
  completion as a display/transaction completion signal. Existing clear/readback,
  resize and recreation output checks still pass. Focused receipt:
  `Build/NativeTestResults/MacOS-arm64-Debug-DurinEditor/MetalCppStage5Viewport.xml`;
  logs `Build/.agent-state/logs/20261004-230453-881783-24786-cmake.log` and
  `Build/.agent-state/logs/20261004-230458-118527-24786-ctest.log`.
- `./DevTool build --target all --jobs 8` passes the registered Engine, Sandbox
  and RoadWeaver project targets, including editor links. Full build log:
  `Build/.agent-state/logs/20261004-230759-138925-25120-cmake.log` (58.63 seconds).
  The final source-layout revision also passes `all`:
  `Build/.agent-state/logs/20261004-231309-262215-26169-cmake.log`.
- `test affected --explain` expands the CMake/source-layout change to an unbounded
  routine selection. The explicit plan gates use the full Metal qualification
  targets and the complete workspace build; unrelated routine suites are not
  claimed as executed. Diff checks, changed-document validation and the required
  all-plan lifecycle validator pass before commit.

### Qualification limits retained

- Stage 0's native-window/application-host visible presentation, minimize/close
  and production adoption gate remains open in the Metal RHI feature plan.
  Real headless layer drawable acquisition/present/completion is passing here;
  it does not establish visible window behavior.
- Stage 1's driver-internal `OBJC_DEBUG_MISSING_POOLS` diagnostic is retained as
  a platform limitation. Weak native lifetime checks pass; a clean missing-pool
  diagnostic run is not claimed.
- The separate direct-native production shader fixture rejects API validation
  because it allocates 80 uniform bytes for a native argument requiring 96.
  Stage 3 recorded the exact failure. Production output qualification passes in
  the baseline environment, while the full production backend headless suite
  passes with API validation. This refactor does not change shader generation
  or mask that independent fixture issue.
- Native allocation exhaustion and a physical GPU fault are not forced. Failure
  evidence comprises preserved descriptor/null handling, actual compiler/function
  failure, unsubmitted readback cancellation and explicitly simulated native
  completion status. No additional hardware/OS profile is qualified.


### 2026-10-05 typed presentation handle follow-up

- Replaced `void* NativeMetalLayer` and the window getter with
  `FNativeMetalLayerHandle`, whose explicit constructor accepts only
  `CA::MetalLayer*`. ApplicationCore creates the typed pointer from the known
  `CAMetalLayer` class; native fixtures bridge their actual layer objects at the
  producer boundary. The handle remains borrowed and requires no SDK headers.
- MetalRHI retains the typed borrow directly with `NS::RetainPtr`. Removed
  `MetalCocoaInterop.h/.mm`, backend Objective-C++ language setup and ARC source
  flags. The backend now compiles entirely as C++. This supersedes the original
  Stage 5 adapter decision; its historical receipts above remain unchanged.
- Compile-time assertions reject `void*` and unrelated pointer construction.
  `RHIInitializationTests` passes all 8 cases, including propagation of the typed
  presentation handle:
  `Build/NativeTestResults/MacOS-arm64-Debug-DurinEditor/MetalLayerHandleInitialization.xml`;
  command log `Build/.agent-state/logs/20261005-010200-292516-28053-RHIInitializationTests.log`.
- The 2 `FMetalRHIViewportTests.*` cases pass outside the sandbox with
  `MTL_DEBUG_LAYER=1` on the same Apple M4 qualification host. They cover both
  execution modes, null layer rejection, actual drawable presentation and native
  lifetime/teardown:
  `Build/NativeTestResults/MacOS-arm64-Debug-DurinEditor/MetalLayerHandleViewport.xml`;
  command log `Build/.agent-state/logs/20261005-010230-035936-28154-ctest.log`.
- Final workspace `all` build passes across Engine, Sandbox and RoadWeaver:
  `Build/.agent-state/logs/20261005-010239-061548-28278-cmake.log`. Changed-document
  validation and diff checks pass. Existing qualification limits above remain.


### 2026-10-05 platform-neutral presentation follow-up

- Replaced the shared Metal layer field/getter with `FRHIPresentationTarget` and
  `FGenericWindow::GetPresentationTarget`. Common Core/HAL and RHI interfaces
  expose only immutable platform target metadata and native window identity.
  Typed layer access lives in `Core/Public/MacOS/MacOSPresentationTarget.h` and
  is consumed only by platform producers, MetalRHI and native fixtures. Core owns
  exported target construction, destruction and RTTI; no Apple SDK dependency
  is introduced into common headers. Metadata ownership does not retain the
  native window; native backend references retain their previous ownership.
- Launch, Mona, MetalRHI, VulkanRHI and native fixtures use the same target across
  initialization and viewport creation. Vulkan still creates/adopts the surface
  by native window identity; Metal validates the macOS target extension before
  retaining its typed layer. The former `FNativeMetalLayerHandle` is removed.
- `RHIInitializationTests` passes 8 cases:
  `Build/NativeTestResults/MacOS-arm64-Debug-DurinEditor/NeutralPresentationInitialization.xml`;
  command log `Build/.agent-state/logs/20261005-015906-170538-31124-RHIInitializationTests.log`.
- `MetalRHIHeadlessTests` passes 38 cases with `MTL_DEBUG_LAYER=1` on the same
  Apple M4 host, including missing-layer and incompatible-platform viewport
  rejection and retained native lifetime:
  `Build/NativeTestResults/MacOS-arm64-Debug-DurinEditor/NeutralPresentationMetal.xml`;
  command log `Build/.agent-state/logs/20261005-015957-382917-31305-ctest.log`.
- Application-hosted Vulkan/window fixtures are migrated but remain disabled
  under the default macOS profile. Native window smoke and Windows execution
  are not claimed as tested by this follow-up.
- Final workspace `all` build passes for Engine, Sandbox and RoadWeaver:
  `Build/.agent-state/logs/20261005-020034-890755-31429-cmake.log`. Changed contract
  documentation, plan validation and diff checks pass.
