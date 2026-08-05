# Recoverable RHI Creation Failures Plan

Summary: Make fallible Vulkan runtime creation return complete-or-null results without poisoning the RHI executor, while preserving terminal handling for startup and execution failures.

Last reviewed: 2026-08-06

Status: Active
Completed:

## Current Status

The Renderer already constructs shader, pipeline, buffer, texture, and fixed-feature payloads as local candidates and commits them through generation-aware resource slots. Those slots can retain a last-known-good payload, suppress a failed generation, and retry after shader, device, or manual invalidation.

That policy is not currently enforceable at the RHI boundary. Vulkan instance, device, and swapchain creation catch errors and continue with null native handles. VMA buffer and image allocation reports failure, but several constructors ignore the result and publish RHI objects with invalid allocations. Conversely, an exception escaping shader, pipeline, or another synchronous creation callback is treated as an RHI worker failure; the executor drains, rejects later work, and waiting code terminates the process. Some Renderer caches can also publish null resource pairs as if creation succeeded.

The selected fix is to distinguish expected creation failure from executor failure. Backend initialization remains fail-fast and rollback-safe. Explicitly fallible runtime factories complete on the RHI thread but return a backend-neutral failed result to their caller without failing the worker. The existing nullable RHI resource references remain the public creation contract; Renderer candidates translate null into their existing owned failure records. Command replay failure, device loss, queue failure, and invariant violations remain terminal.

No implementation work has started.

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

## Implementation Stages

### Stage 0: Lock the failure taxonomy and inventory creation call sites

- [ ] Enumerate every `FDynamicRHI::RHICreate*` implementation and every Vulkan native/VMA creation step reachable from it.
- [ ] Classify each operation as startup-critical, recoverable runtime creation, swapchain lifecycle, or terminal execution/device work.
- [ ] Inventory Renderer slot factories and keyed/size caches that consume nullable resources; record which generation or explicit event makes each failed identity retryable.
- [ ] Identify partial-construction cleanup requirements for buffer allocations, images and views, pipeline aggregates, swapchain images/views, and per-frame resources.
- [ ] Add a short code comment at the new fallible executor API stating which category may use it and that device/executor failure is excluded.
- [ ] Add test seams at the RHI executor and Vulkan allocation/native-create boundaries; failure injection must be test-only and deterministic rather than dependent on real memory exhaustion.

#### Acceptance Gate

- Every in-scope creation entry point and Renderer consumer has one documented failure domain, retry trigger, and cleanup owner; no call site is left to infer recoverability from exception type alone.

### Stage 1: Add a non-terminal fallible synchronous RHI operation

- [ ] Add an owned backend-neutral result for explicitly fallible synchronous operations, including success/operation-failure state and diagnostic text.
- [ ] Implement matching inline and threaded paths. Catch standard and unknown exceptions inside the fallible operation wrapper, publish an operation failure, and complete the RHI serial successfully.
- [ ] Preserve queue ordering, payload lifetime, backpressure accounting, and exact-serial waits for both success and operation failure.
- [ ] Keep admission rejection, a previously failed worker, wait failure, and replay-context unavailability on the terminal path; do not translate them into an ordinary factory null.
- [ ] Leave the existing terminal synchronous API in place for frame lifecycle, uploads, readback, resize coordination, GPU idle, and other operations whose failure invalidates subsequent assumptions.
- [ ] Add RHI tests proving that expected operation failure returns an owned diagnostic, later work executes, statistics remain consistent, and threaded/inline behavior matches.
- [ ] Retain existing tests proving that an uncaught ordinary work exception or explicit `FRHIThreadWorkResult::Failure` drains the worker and wakes/rejects waiters.

#### Acceptance Gate

- An injected fallible-operation exception completes its serial, returns failure to the caller, and permits a following command; executor, replay, or admission failure still produces the existing terminal state.

### Stage 2: Make Vulkan initialization fail atomically

- [ ] Remove catch-and-continue behavior from Vulkan instance and logical-device creation. Convert known Vulkan errors to owned initialization diagnostics and let `Init()` fail through the existing RHI initialization boundary.
- [ ] Check `vmaCreateAllocator` and treat allocator creation as part of logical-device initialization.
- [ ] Construct instance/device-owned managers, queues, contexts, frame objects, and allocator state as local or rollback-owned candidates; publish each owner only after its prerequisites are valid.
- [ ] Make shutdown safe for every partially initialized state and ensure no destroy call receives an invalid instance, device, allocator, queue, or manager handle.
- [ ] Extend RHI initialization tests for instance, device, and allocator failure, including RHI-thread startup, rollback, module-owner release, and one retained diagnostic.

#### Acceptance Gate

- Instance, device, or allocator failure leaves no published backend and no leaked/invalid native owner; startup reports the cause once and the initialization rollback tests pass.

### Stage 3: Enforce complete-or-null Vulkan resource factories

- [ ] Route buffer, texture, shader, graphics-pipeline, sampler, and vertex-declaration creation through the fallible synchronous operation when crossing to the RHI thread.
- [ ] Make VMA buffer/image helpers preserve null outputs on failure and return enough result context for the outer factory diagnostic.
- [ ] Require `FVulkanBuffer` and `FVulkanTexture` construction to stop immediately when allocation fails; create image views only after image allocation succeeds and destroy the image candidate if view creation fails.
- [ ] Audit staging, dynamic-uniform overflow, upload, and readback allocations separately. Return ordinary failure only where the caller already has a nullable/boolean contract; keep frame-critical exhaustion terminal until a safe skip contract exists.
- [ ] Make shader, pipeline, sampler, and vertex-declaration factories catch known backend creation failures at the explicit fallible boundary and return null without publishing cache entries or partial aggregates.
- [ ] Ensure destructors and deferred-deletion enqueue paths accept only valid committed handles; failed local candidates clean themselves immediately on the RHI thread.
- [ ] Add deterministic Vulkan tests for each factory category, including a failed first attempt followed by successful creation while the same RHI thread remains alive.

#### Acceptance Gate

- Every nullable Vulkan factory returns either a fully valid resource or null; no invalid handle reaches a destructor, deferred deletion, descriptor binding, or Renderer candidate, and a later factory call succeeds after an injected failure.

### Stage 4: Make swapchain creation and recreation transactional

- [ ] Replace `FVulkanSwapchain` catch-and-continue construction with an explicit complete candidate or failed result.
- [ ] Build the new swapchain images, image views, semaphores/fences, frame resources, and backbuffer linkage as candidate state before changing the live viewport.
- [ ] Preserve the old swapchain and its dependent resources until candidate commit. Respect Vulkan old-swapchain retirement rules when deciding whether the old output can still be used after a failed replacement.
- [ ] Represent unavailable output explicitly: backbuffer acquisition and drawing skip the affected viewport without dereferencing null, while unrelated offscreen/auxiliary viewports continue.
- [ ] Define retry eligibility for surface out-of-date/suboptimal events, extent/fullscreen/present-mode changes, and explicit manual retry. Suppress duplicate identical creation diagnostics between eligible attempts.
- [ ] Keep device loss and unrecoverable surface/device errors on the terminal path; do not present them as an ordinary resize retry.
- [ ] Add Vulkan viewport tests for initial creation failure, resize/recreate failure with valid-old retention, failure with no usable output, later recovery, and resource destruction order.

#### Acceptance Gate

- A swapchain failure never leaves a non-null viewport backed by a null/partial swapchain; valid old output is retained only when legal, otherwise the viewport skips safely and a defined event can recover it.

### Stage 5: Close Renderer candidate and cache retry gaps

- [ ] Audit all Renderer and Texture Editor RHI creation sites, including fixed feature payloads, keyed shader/pipeline caches, default/environment resources, scene intermediates, thumbnails, and preview resources.
- [ ] Require every slot candidate to validate all required RHI members and translate null into the correct `RHIResource` or `GraphicsPipeline` failure before commit.
- [ ] Replace null-valued keyed/size cache insertion with transactional insertion. A failed first creation must leave no tombstone that blocks a later eligible attempt.
- [ ] Give dynamic scene-target/output caches an explicit retry identity and invalidation rule without forcing a per-frame retry or changing their bounded eviction policy.
- [ ] Verify shader or pipeline refresh failure retains the last-known-good payload, while initial failure skips only the affected feature and device invalidation clears stale device resources.
- [ ] Verify the existing manual retry advances the relevant Renderer identities and does not claim to recover a failed RHI executor or lost device.
- [ ] Add focused tests for nullable buffer/texture/shader/pipeline results, no partial candidate publication, no null cache tombstone, same-generation suppression, unrelated-feature isolation, stale-ready retention, and successful retry.

#### Acceptance Gate

- All Renderer consumers turn null RHI creation into owned slot/cache failure state; no null cache entry masquerades as success, independent rendering continues, and relevant invalidation makes a later attempt possible.

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
