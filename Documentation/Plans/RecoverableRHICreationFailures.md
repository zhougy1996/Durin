# Recoverable RHI Creation Failures Plan

Summary: Make fallible Vulkan runtime creation return complete-or-null results without poisoning the RHI executor, while preserving terminal handling for startup and execution failures.

Last reviewed: 2026-08-06

Status: Active
Completed:

## Current Status

Stages 0 through 5 are complete. The failure-domain, retry-trigger, and cleanup-owner inventory is recorded below. The backend-neutral fallible synchronous-operation result and matching inline/threaded executor paths are implemented and validated. Deterministic, test-only one-shot injection covers Vulkan startup and runtime factories. Vulkan initialization is atomic, ordinary resource factories return a complete resource or null without terminating the RHI thread, and Vulkan viewport output creation/recreation now commits a complete swapchain candidate or exposes no backbuffer while retaining a legal old output when possible. Renderer fixed payloads now validate nullable RHI shaders and resources before commit, default textures publish as one candidate, and size-keyed scene targets use generation-aware slots without null-valued cache hits. Stage 6 is next: validate end to end and publish the lasting contract.

Completed stages: 0-5.

The Renderer already constructs shader, pipeline, buffer, texture, and fixed-feature payloads as local candidates and commits them through generation-aware resource slots. Those slots can retain a last-known-good payload, suppress a failed generation, and retry after shader, device, or manual invalidation.

That policy is not currently enforceable at the RHI boundary. Vulkan instance, device, and swapchain creation catch errors and continue with null native handles. VMA buffer and image allocation reports failure, but several constructors ignore the result and publish RHI objects with invalid allocations. Conversely, an exception escaping shader, pipeline, or another synchronous creation callback is treated as an RHI worker failure; the executor drains, rejects later work, and waiting code terminates the process. Some Renderer caches can also publish null resource pairs as if creation succeeded.

The selected fix is to distinguish expected creation failure from executor failure. Backend initialization remains fail-fast and rollback-safe. Explicitly fallible runtime factories complete on the RHI thread but return a backend-neutral failed result to their caller without failing the worker. The existing nullable RHI resource references remain the public creation contract; Renderer candidates translate null into their existing owned failure records. Command replay failure, device loss, queue failure, and invariant violations remain terminal.

Stage 1 validation passed in both executor modes while retaining the existing terminal worker-failure behavior.

## Goal

- Make every in-scope Vulkan creation path publish either a fully usable object or no object.
- Let Renderer candidate creation observe an ordinary resource-creation failure, retain a valid stale payload where allowed, skip only unavailable work, and retry on an existing relevant generation.
- Keep the dedicated RHI thread running after an expected shader, pipeline, buffer, image, sampler, vertex-declaration, or viewport-output creation failure.
- Fail Vulkan startup once, with owned diagnostics and complete rollback, instead of continuing from a null instance, device, or allocator.
- Preserve terminal executor behavior for failures that invalidate ordering, the device, or the execution context.

## Scope

- The backend-neutral synchronous RHI-operation boundary used by nullable resource factories.
- Vulkan instance, logical-device, and VMA allocator initialization.
- Vulkan runtime creation of buffers, textures and image views, shaders, graphics pipelines, samplers, vertex declarations, and viewports.
- Vulkan swapchain creation and recreation, including dependent images, views, and per-frame resources.
- Renderer resource candidates and size/key caches that consume nullable RHI creation results.
- Failure injection, focused RHI/Vulkan/Renderer tests, diagnostics, and lasting rendering documentation.

## Non-Goals

- Recovering from `VK_ERROR_DEVICE_LOST`, recreating a Vulkan device, or replaying live Renderer state onto a replacement device.
- Making arbitrary recorded command replay, submission, presentation, or GPU synchronization failures non-terminal.
- Retrying allocation every frame, adding automatic exponential backoff to all resources, or changing the existing shader/manual/device generation policy.
- Replacing every nullable RHI factory with a repository-wide `Expected`-style public API.
- Providing a default visual substitute for every unavailable feature.
- Treating programming errors, contract assertions, malformed descriptors, or unknown exceptions outside an explicitly fallible creation boundary as recoverable.

## Design Decisions and Invariants

- RHI failure has three distinct domains:
  - startup-critical creation failure aborts `Init()`, propagates an owned diagnostic through the existing RHI-initialization rollback path, and never publishes `GDynamicRHI` as initialized;
  - expected runtime creation failure returns no resource while completing its synchronous queue entry successfully, so serial ordering and later RHI admission remain valid;
  - executor or device failure marks the RHI thread failed and retains the existing terminal waiter behavior.
- Add an explicitly named fallible synchronous-operation surface alongside the existing terminal `ExecuteSynchronousOperation`; do not silently change every synchronous operation to swallow exceptions.
- The fallible operation result owns its diagnostic and distinguishes operation failure from executor admission/wait failure. Only the former can become a nullable factory result; executor failure remains terminal.
- In threaded mode the fallible wrapper catches the backend creation exception inside the queued callable, stores the operation result, and returns `FRHIThreadWorkResult::Success()` so the serial completes normally. Inline mode must produce the same observable result and diagnostic.
- Public `RHICreate*` methods continue to return nullable reference-counted resources. A null return is the only published resource-creation failure state; invalid non-null wrappers are forbidden.
- Vulkan creation helpers must initialize output handles to null and commit output handles/allocations only after all required native steps succeed. Cleanup of a partially built local candidate is deterministic and immediate.
- Known Vulkan/VMA allocation and creation failures are logged once at the backend boundary with result code, resource identity, and relevant descriptor context. Renderer slots own suppression and recovery diagnostics at the feature/key generation level; they do not depend on parsing backend log text.
- A Renderer creation factory must return `TRenderResourceCreateResult::Failure` for every null member before slot commit. Existing live payloads remain available only when their declared dependency generations remain valid.
- Device-generation invalidation clears old RHI payloads; a device-dependent resource is never retained across device generation changes.
- Swapchain replacement is transactional across the swapchain handle and all dependent image views/frame resources. The live viewport is changed only after a complete candidate exists.
- If swapchain replacement fails, an old swapchain may remain active only when Vulkan still permits its use and its dependent resources are intact. Otherwise the viewport exposes no backbuffer for that frame, keeps a recreate-needed state, and retries only on a defined surface/configuration/retry event; it never dereferences a null swapchain.
- A successful retry produces one recovery diagnostic and atomically replaces the old resource or output candidate. A failed identical Renderer generation is not retried or re-logged every frame.
- `VK_ERROR_DEVICE_LOST`, queue/executor failures, failures while replaying already-recorded commands, and invariant violations never travel through the recoverable creation channel.

## Current Foundations and Gaps

- `TRenderResourceCreationSlot` already provides transactional candidate publication, nullable availability, last-known-good retention, generation-scoped suppression, and recovery diagnostics.
- `FRendererResourceCoordinator` already advances shader, device, and manual generations and exposes the manual RHI-resource retry command.
- Renderer fixed-resource factories generally check nullable RHI references before committing candidates.
- `FDynamicRHI` creation methods already return nullable reference-counted resources, so ordinary creation failure does not require a public return-type migration.
- `FRHICommandListExecutor::ExecuteSynchronousOperation` currently has only terminal semantics. Exceptions escape its queued callable into `FRHIThread`, which marks the worker failed; `WaitForSerial` then terminates the waiter.
- `FVulkanDynamicRHI::CreateInstance` and `FVulkanDevice::CreateDevice` log and continue after failure. Later initialization therefore consumes null native handles instead of using the existing initialization rollback path.
- `FVulkanMemoryManager::Init` ignores `vmaCreateAllocator`, while buffer and image callers do not consistently honor `CreateBuffer`/`CreateImage` failure.
- Vulkan shader, pipeline, sampler, vertex-declaration, texture-view, and viewport constructors may throw across the synchronous-operation boundary.
- Swapchain creation catches and continues, then queries images from a null handle. Recreation destroys or detaches live dependent state before a replacement candidate is known to be complete.
- Size-keyed Renderer intermediates such as post-process scene targets can insert a cache entry containing null resources, suppressing meaningful retry without using a resource slot.

## Stage 0 Inventory

### Backend entry points and native steps

| Entry point or lifecycle | Failure domain | Native/VMA steps | Retry trigger | Partial-construction cleanup owner |
| --- | --- | --- | --- | --- |
| Vulkan `Init`: instance selection, logical device, queues, managers, frames, VMA allocator | Startup-critical | `vk::createInstance`, physical-device enumeration, `createDevice`, queue lookup, manager/frame construction, `vmaCreateAllocator` | A later whole-backend initialization attempt only | `FVulkanDynamicRHI::Init` owns instance/device rollback; `FVulkanDevice::InitGpu` owns device children; `FVulkanMemoryManager` owns allocator rollback |
| `RHICreateBuffer` | Recoverable runtime creation | `vmaCreateBuffer`; mapping is a separate caller contract | Device or manual generation for Renderer slots; a direct caller-defined retry otherwise | `FVulkanBuffer` candidate immediately destroys a committed VMA allocation; no deferred deletion before construction commits |
| `RHICreateTexture` | Recoverable runtime creation | `vmaCreateImage`, `createImageView`; staging `vmaCreateBuffer` and upload are terminal until a nullable upload contract exists | Device/manual generation for slots; source/size/request event for editor caches | `FVulkanTexture` candidate destroys a created view and image allocation in reverse order; staging allocation stays owned by the upload path |
| `RHICreateShader` | Recoverable runtime creation | `createShaderModule` | Shader or manual generation; device generation when part of a device-owned aggregate | `FVulkanShader` candidate destroys its module immediately unless the wrapper commits |
| `RHICreateGraphicsPipelineState` | Recoverable runtime creation | descriptor-set layouts, pipeline layout, `createGraphicsPipeline`, cached render-pass/layout references | Shader, device, or manual generation according to the consuming slot identity | Pipeline/layout candidate aggregate owns reverse-order destruction and must not enter caches before complete |
| `RHICreateSampler` | Recoverable runtime creation | `createSampler` | Device/manual generation for Renderer slots; caller event otherwise | `FVulkanSampler` candidate destroys its handle immediately unless committed |
| `RHICreateVertexDeclaration` | Recoverable runtime creation | Backend wrapper construction; no Vulkan native allocation today | Device/manual generation for Renderer slots; caller event otherwise | Factory-local reference; no native cleanup today |
| `RHICreateViewport` and swapchain create/recreate | Swapchain lifecycle | surface creation, `createSwapchainKHR`, swapchain image query, image views, acquire/render semaphores, present fences, per-frame resources | Initial window availability; out-of-date/suboptimal, extent/fullscreen/present-mode change, or explicit manual retry | A complete `FVulkanViewport` candidate owns the initial graph; recreation candidate owns replacement graph until atomic commit; old live graph remains owner until retirement is legal |
| Dynamic-uniform overflow, staging, upload, readback | Terminal execution until a safe skip/nullable contract is explicit | overflow-page buffer allocation, staging buffers, mapping, copy commands, synchronization/readback | None through the recoverable factory channel | Existing frame/upload/readback owner; failure must not be downgraded by the fallible creation wrapper |
| Command replay, submission, presentation, queue/device loss | Terminal executor/device work | command-buffer replay, queue submit/present, fences/semaphores and device synchronization | No runtime resource retry; requires process/backend recovery outside this plan | RHI executor and Vulkan device lifecycle owners |

### Renderer and editor consumers

| Consumer identity | Publication policy | Eligible retry |
| --- | --- | --- |
| Fullscreen geometry and default textures | Device-owned fixed-resource slots; all required buffers/textures must be non-null | Device or manual generation |
| Environment lighting | One transactional texture/LUT/sampler payload | Device or manual generation |
| Skybox and post-process fixed payloads | Shader/device slot; typed shaders, declarations, samplers, buffers, and all pipelines are required | Shader, device, or manual generation |
| Static-mesh base resources | Device slot; base declaration/buffers must be complete | Device or manual generation |
| Static-mesh shader-map and pipeline caches | Keyed generation-aware slots; no failed or partial payload publication | Shader/manual for shader maps; shader/device/manual for pipelines |
| Static-mesh material samplers | Per-sampler keyed slots; a null sampler is failure, not a cache hit | Device or manual generation |
| Editor grid, gizmo, overlay line, and overlay icon renderers | Base shader/device slot plus keyed pipeline slots; every base/pipeline member is required | Shader/device/manual according to the failure record |
| Texture Editor preview fixed payload | Shader/device slot; every declaration, buffer, sampler, shader, and pipeline is required | Shader, device, or preview/manual refresh |
| Texture Editor input/output textures | Request-owned nullable textures outside the fixed slot | New input, output-size change, or explicit preview/manual refresh |
| Post-process scene targets by size | Transactional color/depth pair; failed pair must not create a size-key tombstone | Size identity after device/manual invalidation, without per-frame retry; existing bounded eviction remains |
| Material and source-image thumbnail textures | Request/key-owned cache entries publish a texture only after successful create/upload | New source/request serial, asset invalidation, or explicit cache refresh |
| Rendered-asset thumbnail preview target | Preview-scene-owned complete-or-null output | Preview reconstruction or output-contract change |

The executor seam is `ExecuteFallibleSynchronousOperation`: tests inject standard and unknown exceptions directly into its callable. Vulkan seams sit immediately before instance/device/swapchain native creation and VMA allocator/buffer/image calls, consume a named failure once, and compile only when `BUILD_TESTING` is enabled; this prevents exception-type inference and real resource exhaustion from defining recoverability.

## Implementation Stages

### Stage 0: Lock the failure taxonomy and inventory creation call sites

- [x] Enumerate every `FDynamicRHI::RHICreate*` implementation and every Vulkan native/VMA creation step reachable from it.
- [x] Classify each operation as startup-critical, recoverable runtime creation, swapchain lifecycle, or terminal execution/device work.
- [x] Inventory Renderer slot factories and keyed/size caches that consume nullable resources; record which generation or explicit event makes each failed identity retryable.
- [x] Identify partial-construction cleanup requirements for buffer allocations, images and views, pipeline aggregates, swapchain images/views, and per-frame resources.
- [x] Add a short code comment at the new fallible executor API stating which category may use it and that device/executor failure is excluded.
- [x] Add test seams at the RHI executor and Vulkan allocation/native-create boundaries; failure injection must be test-only and deterministic rather than dependent on real memory exhaustion.

#### Acceptance Gate

- Every in-scope creation entry point and Renderer consumer has one documented failure domain, retry trigger, and cleanup owner; no call site is left to infer recoverability from exception type alone.

#### Stage 0 Handoff

- Baseline commit: `41b3c098` (`feat(rhi): recover expected creation failures`).
- Working set: `VulkanRHIPrivate.h/.cpp`, Vulkan instance/device/swapchain/memory creation call sites, VulkanRHI/test CMake files, `VulkanFailureInjectionTests.cpp`, and this plan.
- Key symbols: `EVulkanCreateFailurePoint`, `ArmVulkanCreateFailure`, `ConsumeVulkanCreateFailure`, and `ThrowIfVulkanNativeCreateFailureIsArmed`.
- Decision: each named boundary uses an independent atomic one-shot flag; native boundaries throw an injected `eErrorOutOfDeviceMemory`, while VMA boundaries return `VK_ERROR_OUT_OF_DEVICE_MEMORY`. The hook surface and all call-site branches compile only for `BUILD_TESTING`.
- Open question: none for failure injection. Stage 2 must turn the existing instance/device catch-and-continue and unchecked allocator result into one rollback-safe initialization result.
- Validation: `FVulkanCreateFailureInjectionTests` passed 3/3; the complete `VulkanRHIIntegrationTests` target passed 5/5, including the pre-existing inline/threaded GPU integration cases.

### Stage 1: Add a non-terminal fallible synchronous RHI operation

- [x] Add an owned backend-neutral result for explicitly fallible synchronous operations, including success/operation-failure state and diagnostic text.
- [x] Implement matching inline and threaded paths. Catch standard and unknown exceptions inside the fallible operation wrapper, publish an operation failure, and complete the RHI serial successfully.
- [x] Preserve queue ordering, payload lifetime, backpressure accounting, and exact-serial waits for both success and operation failure.
- [x] Keep admission rejection, a previously failed worker, wait failure, and replay-context unavailability on the terminal path; do not translate them into an ordinary factory null.
- [x] Leave the existing terminal synchronous API in place for frame lifecycle, uploads, readback, resize coordination, GPU idle, and other operations whose failure invalidates subsequent assumptions.
- [x] Add RHI tests proving that expected operation failure returns an owned diagnostic, later work executes, statistics remain consistent, and threaded/inline behavior matches.
- [x] Retain existing tests proving that an uncaught ordinary work exception or explicit `FRHIThreadWorkResult::Failure` drains the worker and wakes/rejects waiters.

#### Acceptance Gate

- An injected fallible-operation exception completes its serial, returns failure to the caller, and permits a following command; executor, replay, or admission failure still produces the existing terminal state.

#### Stage 1 Handoff

- Baseline commit: `e025b463` (`docs(rhi): plan recoverable creation failures`).
- Working set: `RHICommandList.h`, `RHICommandList.cpp`, `RHICommandListTests.cpp`, and this plan.
- Key symbols: `FRHIFallibleOperationResult` and `FRHICommandListExecutor::ExecuteFallibleSynchronousOperation`.
- Decision: only the resource-creation callable is caught; replay-context lookup, queue admission, exact-serial wait, and prior worker failure remain outside the recoverable result channel.
- Open question: none for the executor boundary. Stage 0 still needs deterministic Vulkan native/VMA failure injection seams before its gate can close.
- Validation: `RHICommandListTests` passed 39/39, including standard/unknown fallible exceptions in inline/threaded modes; `RHIThreadTests` passed 10/10 and retained terminal worker-failure coverage.

### Stage 2: Make Vulkan initialization fail atomically

- [x] Remove catch-and-continue behavior from Vulkan instance and logical-device creation. Convert known Vulkan errors to owned initialization diagnostics and let `Init()` fail through the existing RHI initialization boundary.
- [x] Check `vmaCreateAllocator` and treat allocator creation as part of logical-device initialization.
- [x] Construct instance/device-owned managers, queues, contexts, frame objects, and allocator state as local or rollback-owned candidates; publish each owner only after its prerequisites are valid.
- [x] Make shutdown safe for every partially initialized state and ensure no destroy call receives an invalid instance, device, allocator, queue, or manager handle.
- [x] Extend RHI initialization tests for instance, device, and allocator failure, including RHI-thread startup, rollback, module-owner release, and one retained diagnostic.

#### Acceptance Gate

- Instance, device, or allocator failure leaves no published backend and no leaked/invalid native owner; startup reports the cause once and the initialization rollback tests pass.

#### Stage 2 Handoff

- Baseline commit: `b8b72da6` (`test(vulkan): add deterministic creation failures`).
- Working set: `RHIGlobals.h/.cpp`, `VulkanDynamicRHI.h/.cpp`, `VulkanDevice.cpp`, `VulkanMemory.cpp`, RHI/Vulkan initialization tests, and this plan.
- Key symbols: `GetLastRHIInitializationDiagnostic`, `InitializeBackendWithRollback`, `FVulkanDynamicRHI::SelectDevice`, `FVulkanDevice::Destroy`, and `FVulkanMemoryManager::Init`.
- Decision: the RHI boundary remains the sole startup diagnostic owner. Vulkan throws contextual owned diagnostics, publishes the device only after complete initialization, and uses the candidate device destructor for partial rollback. A failed real `RHIInit()` also releases the loaded Vulkan module; isolated test backends retain their existing ownership contract.
- Open question: none for startup. Stage 3 must keep runtime allocation failure inside the explicitly fallible operation boundary rather than reusing the startup exception path.
- Validation: `RHIInitializationTests` passed 4/4; `FVulkanCreateFailureInjectionTests` passed 4/4; the complete `VulkanRHIIntegrationTests` target passed 6/6 with no Vulkan validation error or rollback-failure diagnostic.

### Stage 3: Enforce complete-or-null Vulkan resource factories

- [x] Route buffer, texture, shader, graphics-pipeline, sampler, and vertex-declaration creation through the fallible synchronous operation when crossing to the RHI thread.
- [x] Make VMA buffer/image helpers preserve null outputs on failure and return enough result context for the outer factory diagnostic.
- [x] Require `FVulkanBuffer` and `FVulkanTexture` construction to stop immediately when allocation fails; create image views only after image allocation succeeds and destroy the image candidate if view creation fails.
- [x] Audit staging, dynamic-uniform overflow, upload, and readback allocations separately. Return ordinary failure only where the caller already has a nullable/boolean contract; keep frame-critical exhaustion terminal until a safe skip contract exists.
- [x] Make shader, pipeline, sampler, and vertex-declaration factories catch known backend creation failures at the explicit fallible boundary and return null without publishing cache entries or partial aggregates.
- [x] Ensure destructors and deferred-deletion enqueue paths accept only valid committed handles; failed local candidates clean themselves immediately on the RHI thread.
- [x] Add deterministic Vulkan tests for each factory category, including a failed first attempt followed by successful creation while the same RHI thread remains alive.

#### Acceptance Gate

- Every nullable Vulkan factory returns either a fully valid resource or null; no invalid handle reaches a destructor, deferred deletion, descriptor binding, or Renderer candidate, and a later factory call succeeds after an injected failure.

#### Stage 3 Handoff

- Baseline commit: `57245359` (`fix(vulkan): make initialization atomic`).
- Working set: `RHIResources.h/.cpp`, Vulkan memory/buffer/texture/shader/pipeline/resource factories and headers, `VulkanRHIPrivate.h/.cpp`, Vulkan failure-injection tests/data, and this plan.
- Key symbols: `ExecuteFallibleVulkanCreationOperation`, `FVulkanMemoryManager::CreateBuffer`, `FVulkanMemoryManager::CreateImage`, `FVulkanPipelineStateCacheManager::CreateGraphicsPipelineState`, and `FRHIResource::FAtomicFlags::IsUnpublished`.
- Decision: only calls crossing to the RHI thread use the executor boundary; a factory already on that owner catches locally to avoid self-wait. VMA helpers return `vk::Result` with cleared outputs. Staging and dynamic-uniform allocation remain terminal, while texture readback retains its existing boolean failure contract. Constructor unwinding is permitted only for an untouched, never-published RHI lifetime state.
- Open question: none for ordinary resource factories. Stage 4 must keep swapchain candidate ownership separate because old-output retirement and viewport publication require a wider transaction than a single resource constructor.
- Validation: the complete `VulkanRHIIntegrationTests` target passed 7/7, including deterministic failure/retry for every factory category and RHI-thread-local creation; `RHICommandListTests` passed 39/39 and `RHIThreadTests` passed 10/10. No Vulkan validation error, invalid-handle destruction, terminal worker failure, or pipeline-cache tombstone was observed.

### Stage 4: Make swapchain creation and recreation transactional

- [x] Replace `FVulkanSwapchain` catch-and-continue construction with an explicit complete candidate or failed result.
- [x] Build the new swapchain images, image views, semaphores/fences, frame resources, and backbuffer linkage as candidate state before changing the live viewport.
- [x] Preserve the old swapchain and its dependent resources until candidate commit. Respect Vulkan old-swapchain retirement rules when deciding whether the old output can still be used after a failed replacement.
- [x] Represent unavailable output explicitly: backbuffer acquisition and drawing skip the affected viewport without dereferencing null, while unrelated offscreen/auxiliary viewports continue.
- [x] Define retry eligibility for surface out-of-date/suboptimal events, extent/fullscreen/present-mode changes, and explicit manual retry. Suppress duplicate identical creation diagnostics between eligible attempts.
- [x] Keep device loss and unrecoverable surface/device errors on the terminal path; do not present them as an ordinary resize retry.
- [x] Add Vulkan viewport tests for initial creation failure, resize/recreate failure with valid-old retention, failure with no usable output, later recovery, and resource destruction order.

#### Acceptance Gate

- A swapchain failure never leaves a non-null viewport backed by a null/partial swapchain; valid old output is retained only when legal, otherwise the viewport skips safely and a defined event can recover it.

#### Stage 4 Handoff

- Baseline commit: `87c8bbf1` (`fix(vulkan): make resource factories recoverable`).
- Working set: `RHICommandList.h/.cpp`, `VulkanSwapchain.h/.cpp`, `VulkanViewport.h/.cpp`, `VulkanGenericPlatform.cpp`, Vulkan failure injection/integration tests, and this plan.
- Key symbols: `FRHICommandListImmediate::AcquireBackBufferSynchronously`, `FVulkanSwapchain::InitializeSynchronizationResources`, `FVulkanViewport::TryCreateSwapchain`, `FVulkanViewport::SetOutputUnavailable`, and `FVulkanViewport::HasAvailableOutput`.
- Decision: a replacement candidate owns the new swapchain, acquire semaphores, image views, rendering-done semaphores, present fences, and frame state until one commit. Failure before native swapchain creation retains the legal old output; failure after native creation retires old output and publishes no backbuffer. Backbuffer acquisition completes synchronously before viewport drawing is recorded, so an unavailable output skips only that viewport. Identical failures remain suppressed until resize, native recreate, or explicit retry eligibility is raised.
- Open question: none for Vulkan viewport output. Stage 5 must make Renderer candidates and keyed/size caches consume the nullable output/resource contract without tombstones or per-frame retries.
- Validation: `VulkanRHIIntegrationTests` passed 8/8 with a real hidden Vulkan window, including initial failure, retry suppression, explicit recovery, resize failure with old-output retention, post-native image-view/semaphore failure with unavailable output, and orderly cleanup; `RHICommandListTests` passed 39/39 and `RHIThreadTests` passed 10/10. No Vulkan validation error, invalid-handle use, or RHI worker failure was observed.

### Stage 5: Close Renderer candidate and cache retry gaps

- [x] Audit all Renderer and Texture Editor RHI creation sites, including fixed feature payloads, keyed shader/pipeline caches, default/environment resources, scene intermediates, thumbnails, and preview resources.
- [x] Require every slot candidate to validate all required RHI members and translate null into the correct `RHIResource` or `GraphicsPipeline` failure before commit.
- [x] Replace null-valued keyed/size cache insertion with transactional insertion. A failed first creation must leave no tombstone that blocks a later eligible attempt.
- [x] Give dynamic scene-target/output caches an explicit retry identity and invalidation rule without forcing a per-frame retry or changing their bounded eviction policy.
- [x] Verify shader or pipeline refresh failure retains the last-known-good payload, while initial failure skips only the affected feature and device invalidation clears stale device resources.
- [x] Verify the existing manual retry advances the relevant Renderer identities and does not claim to recover a failed RHI executor or lost device.
- [x] Add focused tests for nullable buffer/texture/shader/pipeline results, no partial candidate publication, no null cache tombstone, same-generation suppression, unrelated-feature isolation, stale-ready retention, and successful retry.

#### Acceptance Gate

- All Renderer consumers turn null RHI creation into owned slot/cache failure state; no null cache entry masquerades as success, independent rendering continues, and relevant invalidation makes a later attempt possible.

#### Stage 5 Handoff

- Baseline commit: `01b4c736` (`fix(vulkan): make swapchain replacement transactional`).
- Working set: `RenderCore` typed-shader access, Renderer default/fixed/keyed/scene-target resource owners, Texture Editor preview resources, Renderer slot-cache tests, and this plan.
- Key symbols: `TShaderRef::GetRHIShader(bool)`, `FDefaultTextureResources::FPayload`, `TRendererResourceSlotCache::EvictOldestExcept`, and `FPostProcessRenderer::EnsureSceneTargets_RenderThread`.
- Decision: candidate factories request RHI shaders with non-required semantics and translate null before pipeline creation, while draw-time access remains required. Default textures commit as one device-owned slot payload. Each scene-target size owns a device-dependent slot whose failure retries only after device/manual generation change or bounded identity eviction; the cache remains capped at eight entries.
- Open question: Stage 6 must inspect and resolve or explicitly baseline the existing `RendererResourceReloadVulkanTests` `DrawParameters` feature VUID before claiming a validation-clean editor smoke; it is not caused by the Stage 5 nullable candidate changes.
- Validation: `EditorRenderingTests` passed 33/33, `RenderContractTests` passed 37/37, `RendererResourceReloadVulkanTests` passed 1/1, and `VulkanRHIIntegrationTests` passed 8/8; the `TextureEditor` target built successfully. Renderer slot tests cover failed aggregate publication, same-generation suppression, unrelated-key isolation, manual recovery, stale-ready retention, device clearing, and bounded cache eviction. The Vulkan factory suite retains deterministic nullable buffer, texture/image-view, shader, pipeline, sampler, and declaration failure/retry coverage.

### Stage 6: Validate end to end and publish the lasting contract

- [ ] Run focused RHI executor, RHI initialization, RenderCore resource-creation, Vulkan RHI, and Renderer failure/retry tests through the repository-native workflow.
- [ ] Exercise both dedicated-thread and `DURIN_RHI_EXECUTION=inline` modes for expected factory failure and recovery.
- [ ] Run a successful full `all` build through the root DurinDevTool workflow.
- [ ] Run the verified `DurinEditor` from the same Agent Build Profile and smoke main window, render-target viewport, auxiliary viewport, resize, shader retry, and orderly shutdown.
- [ ] Inject one allocation failure and one shader/pipeline creation failure in a controlled Vulkan test run; verify the RHI thread stays live, unaffected rendering continues, diagnostics do not spam, and manual/relevant invalidation recovers.
- [ ] Inject startup instance/device/allocator failure and executor/device failure separately; verify they remain rollback/terminal rather than entering Renderer retry state.
- [ ] Update `RHICommandExecution.md` with the fallible-operation versus terminal-executor contract.
- [ ] Update `ViewportRendering.md` with complete-or-null RHI candidates, swapchain unavailable-output behavior, and the exact limits of last-known-good/device retry.
- [ ] Record validation evidence and complete the plan only after every required gate passes.

#### Acceptance Gate

- Focused tests, both executor modes, full build, Vulkan editor smoke, runtime creation recovery, startup rollback, and terminal executor/device behavior all pass; owning documentation describes the landed failure boundary without promising device-loss recovery.

## Validation Matrix

| Scenario | Required behavior | Evidence |
| --- | --- | --- |
| Fallible sync operation throws | Owned operation failure; serial completes; later work runs | RHI executor test in threaded and inline modes |
| Ordinary queued work throws | Worker fails, queued work rejects, waiter observes terminal failure | Existing and extended RHI thread tests |
| Vulkan instance/device/allocator failure | Initialization aborts and rolls back; no null-handle continuation | RHI initialization failure-injection tests |
| VMA buffer/image failure | Factory returns null; no invalid RHI wrapper or deferred delete | Vulkan allocation tests |
| Image view or late constructor failure | Earlier local native objects are destroyed; no partial publication | Vulkan resource candidate test |
| Shader/pipeline/sampler/declaration failure | Factory returns null and RHI thread remains available | Vulkan factory tests |
| Renderer first creation failure | Affected draw skips; independent features render; no log spam | RenderCore/Renderer injected tests |
| Renderer refresh failure | Last-known-good payload remains drawable for the same device generation | Renderer slot integration test |
| Same generation lookup | No second factory attempt and no repeated diagnostic | Slot/cache counter test |
| Relevant manual/shader invalidation | Failed identity becomes eligible and can recover | Renderer invalidation test |
| Device generation changes | Old RHI payload is cleared and never used as fallback | Renderer coordinator test |
| Dynamic target creation fails | No null cache tombstone; later eligible attempt can insert | Post-process cache test |
| Initial swapchain creation fails | Viewport exposes no backbuffer and skips safely | Vulkan viewport test |
| Swapchain recreation fails | Legal old candidate retained or output becomes explicitly unavailable | Vulkan viewport lifecycle test |
| Swapchain retry succeeds | Candidate commits atomically and emits one recovery diagnostic | Vulkan viewport retry test |
| Device lost or replay fails | No nullable downgrade; executor remains terminal | RHI/Vulkan terminal-path test |
| Shutdown after recoverable failures | Queue drains and all candidate/live resources are released once | RHI shutdown test and editor smoke |

## Definition of Done

- No Vulkan instance, device, allocator, or swapchain creation path logs and then consumes a null native handle.
- No VMA buffer/image failure can produce a non-null RHI resource with an invalid allocation.
- Every nullable Vulkan `RHICreate*` factory is complete-or-null and uses the explicit fallible executor boundary when required.
- Expected runtime creation failure does not mark the RHI worker failed; executor/device/replay failure still does.
- Renderer factories and dynamic caches publish no partial candidates or null tombstones.
- Same-generation Renderer failures are suppressed, relevant invalidation retries, and valid last-known-good payloads are retained only within their dependency contract.
- Swapchain replacement is transactional and unavailable output is handled without null dereference or false success.
- Failure diagnostics are owned, categorized at the Renderer level, bounded per identity/generation, and paired with one recovery diagnostic.
- Focused tests, inline/threaded parity, full build, Vulkan editor smoke, and failure-injection scenarios pass.
- `RHICommandExecution.md` and `ViewportRendering.md` own the final runtime contract.

## Deferred Follow-ups

- Vulkan device-loss recovery, device recreation, and Renderer resource resubmission.
- Cross-backend structured native error payloads beyond the nullable public factory contract.
- Automatic transient-allocation retry/backoff when a backend exposes a reliable recovery signal.
- Persistent editor UI for resource failures beyond logs and console diagnostics.
- A general transactional cache abstraction for non-Renderer systems.
- Visual fallback assets for every feature with no last-known-good payload.

## Related Documentation

- `Documentation/Runtime/Rendering/RHICommandExecution.md`
- `Documentation/Runtime/Rendering/ViewportRendering.md`
- `Documentation/Runtime/Core/RuntimeLifecycle.md`
- `Documentation/Plans/Archive/2026-07/RecoverableRendererResourceCreation.md`
- `Documentation/Plans/Archive/2026-08/DedicatedRHIThread.md`
- `Documentation/Development/Build/BuildAndRun.md`
- `Documentation/Development/Build/NativeTests.md`

## Related Code

- `Engine/Source/Runtime/RHI/Public/DynamicRHI.h`
- `Engine/Source/Runtime/RHI/Public/RHICommandList.h`
- `Engine/Source/Runtime/RHI/Private/RHICommandList.cpp`
- `Engine/Source/Runtime/RHI/Private/RHIThread.cpp`
- `Engine/Source/Runtime/RenderCore/Public/RenderResourceCreation.h`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanDynamicRHI.cpp`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanDevice.cpp`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanMemory.cpp`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanBuffer.cpp`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanTexture.cpp`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanShader.cpp`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanPipeline.cpp`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanResources.cpp`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanSwapchain.cpp`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanViewport.cpp`
- `Engine/Source/Runtime/Renderer/Private/Resources/RendererResourceCoordinator.cpp`
- `Engine/Source/Runtime/Renderer/Private/Resources/FullscreenGeometryResources.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/PostProcessRenderer.cpp`
- `Engine/Tests/Native/RHITests/Private/RHIThreadTests.cpp`
- `Engine/Tests/Native/RHITests/Private/RHICommandListTests.cpp`
- `Engine/Tests/Native/RHITests/Private/RHIInitializationTests.cpp`
- `Engine/Tests/Native/RenderCoreTests/Private/RenderResourceCreationTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/RendererResourceReloadVulkanTests.cpp`
- `Engine/Tests/Native/VulkanRHITests/Private/VulkanTextureSamplingTests.cpp`
