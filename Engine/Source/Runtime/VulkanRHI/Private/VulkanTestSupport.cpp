#include "VulkanTestSupport.h"

#if DURIN_VULKAN_TEST_SUPPORT
#include "VulkanRHIPrivate.h"
#include "Backend/RHICompletionBackend.h"
#include "VulkanGPUTiming.h"

#include "RHICommandList.h"

#include "VulkanDevice.h"
#include "VulkanBuffer.h"
#include "VulkanUniformBufferPool.h"
#include "VulkanDescriptorSets.h"
#include "VulkanSubmission.h"
#include "VulkanCommandBuffer.h"
#include "VulkanCompletion.h"
#include "VulkanContext.h"
#include "VulkanDynamicRHI.h"
#include "VulkanQueue.h"
#include "VulkanQueueTransfer.h"
#include "VulkanTexture.h"
#include "VulkanTransferArena.h"

namespace Durin::VulkanRHI
{
	std::atomic<uint64> GVulkanRenderPassEntryCount = 0;
	std::atomic<uint64> GVulkanFramebufferEntryCount = 0;
	std::atomic<uint64> GVulkanDescriptorSetLayoutEntryCount = 0;
	std::atomic<uint64> GVulkanPipelineLayoutEntryCount = 0;
	std::atomic<uint64> GVulkanCreatedFramebufferViewCount = 0;
	std::atomic<uint64> GVulkanReleasedFramebufferViewCount = 0;
	std::atomic<uint64> GVulkanCreatedFramebufferCount = 0;
	std::atomic<uint64> GVulkanReleasedFramebufferCount = 0;
	std::atomic<uint64> GVulkanSync2BufferBarrierCount = 0;
	std::atomic<uint64> GVulkanLegacyBufferBarrierCount = 0;
	std::atomic<uint64> GVulkanSync2ImageBarrierCount = 0;
	std::atomic<uint64> GVulkanLegacyImageBarrierCount = 0;
	std::atomic<int32> GVulkanBarrierPathOverride = -1;
	std::atomic<uint64> GVulkanBindingValidationVisitCount = 0;
	std::atomic<uint64> GVulkanDescriptorDrawValidationVisitCount = 0;
	std::atomic<uint64> GVulkanDescriptorSortCount = 0;
	std::atomic<uint64> GVulkanDescriptorHashCount = 0;
	std::atomic<uint64> GVulkanViewportWriteCount = 0;
	std::atomic<uint64> GVulkanScissorWriteCount = 0;
	std::atomic<uint64> GVulkanDepthBiasWriteCount = 0;
	std::atomic<uint64> GVulkanPipelineBindCount = 0;
	std::atomic<uint64> GVulkanVertexBufferBindCount = 0;
	std::atomic<uint64> GVulkanIndexBufferBindCount = 0;
	std::atomic<uint64> GVulkanDescriptorBindCount = 0;
	std::atomic<uint64> GVulkanDescriptorOwnerRebuildCount = 0;
	std::atomic<uint64> GVulkanDescriptorOccupancyVerificationVisitCount = 0;
	std::atomic<uint64> GVulkanDescriptorOccupancyMutationCount = 0;
	auto GetVulkanStructuralCacheTestStats() -> FVulkanStructuralCacheTestStats
	{
		return {
			.RenderPassEntryCount = GVulkanRenderPassEntryCount.load(std::memory_order_acquire),
			.FramebufferEntryCount = GVulkanFramebufferEntryCount.load(std::memory_order_acquire),
			.DescriptorSetLayoutEntryCount = GVulkanDescriptorSetLayoutEntryCount.load(std::memory_order_acquire),
			.PipelineLayoutEntryCount = GVulkanPipelineLayoutEntryCount.load(std::memory_order_acquire),
			.CreatedFramebufferViewCount = GVulkanCreatedFramebufferViewCount.load(std::memory_order_acquire),
			.ReleasedFramebufferViewCount = GVulkanReleasedFramebufferViewCount.load(std::memory_order_acquire),
			.CreatedFramebufferCount = GVulkanCreatedFramebufferCount.load(std::memory_order_acquire),
			.ReleasedFramebufferCount = GVulkanReleasedFramebufferCount.load(std::memory_order_acquire),
		};
	}

	auto RunVulkanQueueTransferForTesting(bool bSynchronization2) -> FVulkanQueueTransferTestResult
	{
		CheckVulkanRHIThread();
		auto& RHI = FVulkanDynamicRHI::Get();
		auto& Device = *RHI.GetDeviceForTesting();
		auto& Graphics = *Device.GetImmediateContext();
		auto& SourceQueue = *Device.GetGraphicsQueue();
		auto& DestinationQueue = *Device.GetComputeQueue();
		require(&SourceQueue != &DestinationQueue);
		TRefCountPtr<FVulkanBuffer> Shared = new FVulkanBuffer(Device,
			FRHIBufferCreateDesc::Create("QueueHandoffBuffer", 64, 4,
				EBufferUsageFlags::Static | EBufferUsageFlags::SourceCopy | EBufferUsageFlags::DestinationCopy));
		TRefCountPtr<FVulkanBuffer> Readback = new FVulkanBuffer(Device,
			FRHIBufferCreateDesc::Create("QueueHandoffReadback", 64, 4,
				EBufferUsageFlags::Dynamic | EBufferUsageFlags::DestinationCopy | EBufferUsageFlags::KeepCPUAccessible));
		TRefCountPtr<FVulkanTexture> Texture = new FVulkanTexture(Device,
			FRHITextureCreateDesc::Create2D("QueueHandoffTexture", 4, 4, EPixelFormat::RGBA8_UNORM)
				.SetNumMips(2).SetFlags(ETextureCreateFlags::SourceCopy | ETextureCreateFlags::DestinationCopy));
		FVulkanCommandListContext Compute(&RHI, Device, &DestinationQueue);
		struct FDrainBeforePoolDestruction
		{
			FVulkanDevice& Device;
			~FDrainBeforePoolDestruction() { try { Device.WaitUtilIdle(); } catch (const vk::SystemError&) {} }
		} Drain{Device};
		const FRHITextureSubresourceRange Range{ERHITextureAspect::Color, 1, 1, 0, 1};
		const std::array InitializeBuffer{FRHIBufferTransition{Shared.GetReference(), 0, 64, ERHIAccess::None, ERHIAccess::TransferWrite}};
		const std::array InitializeTexture{FRHITextureTransition{Texture.GetReference(), Range, ERHIAccess::None, ERHIAccess::TransferWrite}};
		Graphics.RHITransitionBuffers(InitializeBuffer);
		Graphics.RHITransitionTextures(InitializeTexture);
		Graphics.GetCommandBuffer()->GetHandle().fillBuffer(Shared->GetHandle(), 0, 64, 0x12345678);
		Graphics.GetCommandBuffer()->GetHandle().clearColorImage(Texture->Image, vk::ImageLayout::eTransferDstOptimal,
			vk::ClearColorValue(std::array<float, 4>{1, 0, 0, 1}), vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 1, 1, 0, 1));
		const std::array BufferTransitions{FRHIBufferTransition{Shared.GetReference(), 16, 16, ERHIAccess::TransferWrite, ERHIAccess::TransferRead}};
		const std::array TextureTransitions{FRHITextureTransition{Texture.GetReference(), Range, ERHIAccess::TransferWrite, ERHIAccess::TransferRead}};
		auto Transfer = std::make_shared<FVulkanQueueTransfer>(Device, SourceQueue.GetId(), DestinationQueue.GetId(),
			BufferTransitions, TextureTransitions, bSynchronization2);
		// A canceled later recording must not replace the release's producer sync point.
		FVulkanCommandListContext OtherGraphics(&RHI, Device, &SourceQueue);
		auto LaterGraphics = OtherGraphics.Finalize();
		Graphics.ReleaseQueueOwnership(Transfer);
		LaterGraphics.reset();
		Device.GetSubmissionCoordinator().EnqueueContext(Graphics);
		Compute.AcquireQueueOwnership(Transfer);
		const std::array InitializeReadback{FRHIBufferTransition{Readback.GetReference(), 0, 64, ERHIAccess::None, ERHIAccess::TransferWrite}};
		Compute.RHITransitionBuffers(InitializeReadback);
		Compute.GetCommandBuffer()->GetHandle().copyBuffer(Shared->GetHandle(), Readback->GetHandle(), vk::BufferCopy(16, 0, 16));
		const vk::BufferImageCopy ImageCopy(16, 0, 0, {vk::ImageAspectFlagBits::eColor, 1, 0, 1}, {0, 0, 0}, {2, 2, 1});
		Compute.GetCommandBuffer()->GetHandle().copyImageToBuffer(Texture->Image, vk::ImageLayout::eTransferSrcOptimal, Readback->GetHandle(), ImageCopy);
		const std::array HostRead{FRHIBufferTransition{Readback.GetReference(), 0, 64, ERHIAccess::TransferWrite, ERHIAccess::HostRead}};
		Compute.RHITransitionBuffers(HostRead);
		// Seal consumer first; the coordinator orders its still-unsubmitted release.
		Device.GetSubmissionCoordinator().SubmitPendingContexts(&Compute);
		const auto Completion = DestinationQueue.GetCompletionTracker().GetLastReservedSyncPoint();
		std::weak_ptr<FVulkanQueueTransfer> Observer = Transfer;
		Transfer.reset();
		FVulkanQueueTransferTestResult Result;
		Result.bRetainedUntilCompletion = !Observer.expired();
		require(DestinationQueue.GetCompletionTracker().WaitForSyncPoint(Completion, 1'000'000'000) == ERHIGPUWaitResult::Complete);
		Device.PollQueues();
		Result.bReleasedAfterCompletion = Observer.expired();
		Readback->InvalidateMappedMemory(0, 32);
		const auto* Words = static_cast<const uint32*>(Readback->GetMappedPointer());
		Result.bBufferMatched = std::ranges::all_of(std::span{Words, 4}, [](uint32 Value) { return Value == 0x12345678; });
		Result.bTextureMatched = std::ranges::all_of(std::span{Words + 4, 4}, [](uint32 Value) { return Value == 0xff0000ff; });
		Result.bUnselectedRangesPreserved = Shared->GetStateTracker().GetOwnership().CanUse(0, 16, SourceQueue.GetFamilyIndex())
			&& Texture->GetStateTracker().Get(ERHITextureAspect::Color, 0, 0) == ERHIAccess::None;
		return Result;
	}

	auto RunVulkanComputeGateForTesting(
		const std::function<void(const std::function<void()>&)>& Test) -> void
	{
		auto& Device = *FVulkanDynamicRHI::Get().GetDeviceForTesting();
		auto& Compute = *Device.GetComputeQueue();
		require(&Compute != Device.GetGraphicsQueue() && Device.SupportsTimelineSemaphores());
		vk::Semaphore Gate;
		GCommandListExecutor.ExecuteSynchronousOperation(false, [&] {
			CheckVulkanRHIThread();
			vk::SemaphoreTypeCreateInfo Type(vk::SemaphoreType::eTimeline, 0);
			vk::SemaphoreCreateInfo Info;
			Info.setPNext(&Type);
			Gate = Device.GetHandle().createSemaphore(Info);
		});
		bool bReleased = false;
		const std::function<void()> Release = [&] {
			if (bReleased) return;
			GCommandListExecutor.ExecuteSynchronousOperation(false, [&] {
				const vk::SemaphoreSignalInfo Signal(Gate, 1);
				if (Device.GetGpuProperties().apiVersion >= VK_API_VERSION_1_2)
					Device.GetHandle().signalSemaphore(Signal);
				else Device.GetHandle().signalSemaphoreKHR(Signal);
			});
			bReleased = true;
		};
		auto Cleanup = [&] {
			Release();
			GCommandListExecutor.ExecuteSynchronousOperation(false, [&] {
				Compute.GetHandle().waitIdle();
				Device.GetHandle().destroySemaphore(Gate);
			});
		};
		try
		{
			GCommandListExecutor.ExecuteSynchronousOperation(false, [&] {
				const uint64 Value = 1;
				const vk::PipelineStageFlags Stage = vk::PipelineStageFlagBits::eAllCommands;
				vk::TimelineSemaphoreSubmitInfo Timeline;
				Timeline.setWaitSemaphoreValues(Value);
				vk::SubmitInfo Submit;
				Submit.setPNext(&Timeline).setWaitSemaphores(Gate).setWaitDstStageMask(Stage);
				Compute.GetHandle().submit(Submit);
			});
			Test(Release);
		}
		catch (...)
		{
			Cleanup();
			throw;
		}
		Cleanup();
	}

	auto RunVulkanCrossQueueWaitForTesting() -> FVulkanCrossQueueWaitTestResult
	{
		CheckVulkanRHIThread();
		auto& Device = *FVulkanDynamicRHI::Get().GetDeviceForTesting();
		auto& Compute = *Device.GetComputeQueue();
		auto& Graphics = *Device.GetGraphicsQueue();
		require(&Compute != &Graphics && Device.SupportsTimelineSemaphores());
		vk::SemaphoreTypeCreateInfo GateType(vk::SemaphoreType::eTimeline, 0);
		vk::SemaphoreCreateInfo GateInfo;
		GateInfo.setPNext(&GateType);
		const auto Gate = Device.GetHandle().createSemaphore(GateInfo);
		bool bGateReleased = false;
		auto ReleaseGate = [&] {
			if (bGateReleased) return;
			const vk::SemaphoreSignalInfo Signal(Gate, 1);
			if (Device.GetGpuProperties().apiVersion >= VK_API_VERSION_1_2)
				Device.GetHandle().signalSemaphore(Signal);
			else Device.GetHandle().signalSemaphoreKHR(Signal);
			bGateReleased = true;
		};
		FVulkanTransferArena TestTransfers(Device, {
			.AllocationClass = EVulkanAllocationClassCandidate::TransferUpload,
			.PageSize = 256, .MaxPageCount = 1, .DebugName = "DelayedComputeTransfer"});
		constexpr uint64 AdmissionPage = 4ull * 1024 * 1024;
		auto TestUniforms = std::make_shared<FVulkanUniformBufferAdmission>(256);
		auto UniformOwner = TestUniforms->Reserve(AdmissionPage);
		require(UniformOwner);
		const auto UniformSlot = UniformOwner->Slot;
		auto Submit = [&](FVulkanQueue& Queue, const FRHIGPUSyncPointRef& Wait, std::shared_ptr<void> Owner = {}, FVulkanGPUTimingQuery* Timing = nullptr) {
			auto& Tracker = Queue.GetCompletionTracker();
			std::unique_ptr<FVulkanPayload> Payload;
			if (Timing)
			{
				auto& Context = *Device.GetQueueContext(Queue.GetId());
				require(Timing->TryReserveRecording());
				Context.RHIBeginGPUTimingQuery(Timing);
				Context.RHIEndGPUTimingQuery(Timing);
				Payload = Context.Finalize();
			}
			else Payload = std::make_unique<FVulkanPayload>(Queue, Tracker.ReserveSyncPoint());
			if (&Queue == &Compute)
			{
				auto Allocation = TestTransfers.Acquire(256, 16, Payload->GetSyncPoint());
				require(Allocation.Range);
				Payload->RetainAllocation(Allocation.Range.GetAllocationOwner());
				// Destruction before submission must preserve the payload's interval.
			}
			if (Owner) Payload->RetainAllocation(std::move(Owner));
			if (UniformOwner) Payload->RetainAllocation(UniformOwner);
			if (Wait.GetState() != ERHIGPUSubmissionState::Invalid)
			{
				Payload->AddCompletionWait(Wait);
				Payload->AddCompletionWait(Wait); // Native fan-in must deduplicate semaphore handles.
			}
			return Device.GetSubmissionCoordinator().Submit(std::move(Payload));
		};
		FVulkanCrossQueueWaitTestResult Result;
		Result.bTimingSupported = Device.GetQueueFamilyProperties(Compute.GetFamilyIndex()).timestampValidBits != 0;
		DURIN_DEBUG("Compute timestamp validation: family={}, supported={}", Compute.GetFamilyIndex(), Result.bTimingSupported);
		auto Timing = Result.bTimingSupported ? Device.GetGPUTimingManager().CreateQuery() : nullptr;
		require(!Result.bTimingSupported || Timing);
		FVulkanGlobalDescriptorPool TestPools(Device);
		try
		{
			const vk::DescriptorSetLayoutBinding Binding(0, vk::DescriptorType::eSampler, 1, vk::ShaderStageFlagBits::eCompute);
			const auto Layout = Device.GetHandle().createDescriptorSetLayout(vk::DescriptorSetLayoutCreateInfo().setBindings(Binding));
			struct FLayoutOwner
			{
				FVulkanDevice& Device;
				vk::DescriptorSetLayout Layout;
				~FLayoutOwner() { Device.GetHandle().destroyDescriptorSetLayout(Layout); }
			} LayoutOwner{Device, Layout};
			const std::array Layouts{Layout};
			FVulkanDescriptorRequirements Requirements;
			Requirements.MaxSets = 1;
			Requirements.DescriptorCounts[vk::DescriptorType::eSampler] = 1;
			TestPools.PrepareForUse();
			require(TestPools.AllocateDescriptorSets(Layouts, Requirements).size() == 1);
			const uint64 GateValue = 1;
			const vk::PipelineStageFlags Stage = vk::PipelineStageFlagBits::eAllCommands;
			vk::TimelineSemaphoreSubmitInfo GateTimeline;
			GateTimeline.setWaitSemaphoreValues(GateValue);
			vk::SubmitInfo GateSubmit;
			GateSubmit.setPNext(&GateTimeline).setWaitSemaphores(Gate).setWaitDstStageMask(Stage);
			Compute.GetHandle().submit(GateSubmit);
			const auto Producer = Submit(Compute, {}, TestPools.GetAllocationOwner(), Timing.GetReference());
			// A graphics frame can pace independently while compute still owns resources.
			Device.GetImmediateContext()->RHIEndFrame();
			const auto& FrameUses = Device.GetCurrentFrame().GetRetirementUsesForTesting();
			require(FrameUses.GetSyncPoints().size() == 1
				&& FRHIGPUSyncPointBackend::GetPoint(FrameUses.GetSyncPoints().front()).Queue == Graphics.GetId());
			Device.GetCurrentFrame().Prepare();
			Result.bFramePacingIndependent = Producer.GetState() == ERHIGPUSubmissionState::Submitted;
			// Graphics completion alone must not reset a pool also used by compute.
			const auto Independent = Submit(Graphics, {}, TestPools.GetAllocationOwner());
			require(Graphics.GetCompletionTracker().WaitForSyncPoint(Independent, 1'000'000'000) == ERHIGPUWaitResult::Complete);
			Device.PollQueues();
			Result.bTimingBlocked = Timing && Timing->GetResult().State == ERHIGPUTimingResultState::Pending;
			Result.bTransferReuseBlocked = !TestTransfers.Acquire(256, 16, Producer).Range;
			UniformOwner.reset();
			const auto ConcurrentUniform = TestUniforms->Reserve(AdmissionPage);
			Result.bUniformReuseBlocked = ConcurrentUniform->Slot.Page != UniformSlot.Page;
			std::weak_ptr<void> PoolOwner = TestPools.GetAllocationOwner();
			TestPools.RetireUsedPools();
			TestPools.PrepareForUse();
			Result.bDescriptorReuseBlocked = TestPools.IsBatchRetainedForTesting(0)
				&& TestPools.GetActiveBatchIndexForTesting() == 1;
			TestPools.RetireUsedPools();
			const auto Consumer = Submit(Graphics, Producer);
			const auto Uses = Device.GetLastReservedUses();
			Result.Producer = FRHIGPUSyncPointBackend::GetPoint(Producer);
			Result.Consumer = FRHIGPUSyncPointBackend::GetPoint(Consumer);
			Result.bConsumerBlocked = Graphics.GetCompletionTracker().WaitForSyncPoint(Consumer, 0) == ERHIGPUWaitResult::Timeout;
			Result.bRetirementBlocked = !Uses.IsRetirementEligible();
			ReleaseGate();
			Device.GetSubmissionCoordinator().WaitForAllocation(PoolOwner);
			Result.bTimingCompleted = Timing && Timing->GetResult().State == ERHIGPUTimingResultState::Ready;
			Result.bTransferReusedAfterCompletion = bool(TestTransfers.Acquire(256, 16, Producer).Range);
			Result.bCompleted = Graphics.GetCompletionTracker().WaitForSyncPoint(Consumer, 1'000'000'000) == ERHIGPUWaitResult::Complete
				&& Compute.GetCompletionTracker().WaitForSyncPoint(Producer, 1'000'000'000) == ERHIGPUWaitResult::Complete
				&& Uses.IsRetirementEligible();
			auto ReusedUniform = TestUniforms->Reserve(AdmissionPage);
			Result.bUniformReusedAfterCompletion = ReusedUniform
				&& ReusedUniform->Slot.Page == UniformSlot.Page
				&& ReusedUniform->Slot.Offset == 0;
			TestPools.PrepareForUse();
			Result.bDescriptorReusedAfterCompletion = TestPools.GetActiveBatchIndexForTesting() == 0;
			require(TestPools.AllocateDescriptorSets(Layouts, Requirements).size() == 1);
			Device.WaitUtilIdle();
			auto MakePayload = [](FVulkanQueue& Queue) {
				return std::make_unique<FVulkanPayload>(Queue, Queue.GetCompletionTracker().ReserveSyncPoint());
			};
			auto ProducerPayload = MakePayload(Compute);
			const auto BatchProducer = ProducerPayload->GetSyncPoint();
			auto ConsumerPayload = MakePayload(Graphics);
			const auto BatchConsumer = ConsumerPayload->GetSyncPoint();
			ConsumerPayload->AddCompletionWait(BatchProducer);
			std::vector<std::unique_ptr<FVulkanPayload>> Batch;
			Batch.push_back(std::move(ConsumerPayload)); // Admission order is deliberately reversed.
			Batch.push_back(std::move(ProducerPayload));
			Device.GetSubmissionCoordinator().SubmitBatch(std::move(Batch));
			Result.bBatchOrdered = Graphics.GetCompletionTracker().WaitForSyncPoint(BatchConsumer,
				1'000'000'000) == ERHIGPUWaitResult::Complete;
			Device.PollQueues();
			Result.bBatchOrdered &= BatchProducer.GetState() == ERHIGPUSubmissionState::Complete;
			Result.bBatchCycleRejected = true;
			for (bool bSameQueue : {false, true})
			{
				auto First = MakePayload(Graphics);
				auto Second = MakePayload(bSameQueue ? Graphics : Compute);
				const auto FirstSyncPoint = First->GetSyncPoint(), SecondSyncPoint = Second->GetSyncPoint();
				First->AddCompletionWait(SecondSyncPoint);
				if (!bSameQueue) Second->AddCompletionWait(FirstSyncPoint);
				const auto Before = Graphics.GetCompletionTracker().GetLastSubmittedToken();
				std::vector<std::unique_ptr<FVulkanPayload>> Cycle;
				Cycle.push_back(std::move(First)); Cycle.push_back(std::move(Second));
				bool bRejected = false;
				try { Device.GetSubmissionCoordinator().SubmitBatch(std::move(Cycle)); }
				catch (const std::runtime_error&) { bRejected = true; }
				Result.bBatchCycleRejected &= bRejected && FirstSyncPoint.GetState() == ERHIGPUSubmissionState::Canceled
					&& SecondSyncPoint.GetState() == ERHIGPUSubmissionState::Canceled
					&& Graphics.GetCompletionTracker().GetLastSubmittedToken() == Before;
			}
			{
				auto First = MakePayload(Graphics);
				auto MissingReservation = MakePayload(Graphics);
				auto Last = MakePayload(Graphics);
				auto Independent = MakePayload(Compute);
				const auto FirstSyncPoint = First->GetSyncPoint(), LastSyncPoint = Last->GetSyncPoint();
				const auto IndependentSyncPoint = Independent->GetSyncPoint();
				const auto BeforeGraphics = Graphics.GetCompletionTracker().GetLastSubmittedToken();
				const auto BeforeCompute = Compute.GetCompletionTracker().GetLastSubmittedToken();
				std::vector<std::unique_ptr<FVulkanPayload>> Incomplete;
				Incomplete.push_back(std::move(Independent));
				Incomplete.push_back(std::move(First));
				Incomplete.push_back(std::move(Last));
				try { Device.GetSubmissionCoordinator().SubmitBatch(std::move(Incomplete)); }
				catch (const std::runtime_error&) { Result.bBatchMissingReservationRejected = true; }
				Result.bBatchMissingReservationRejected &= FirstSyncPoint.GetState() == ERHIGPUSubmissionState::Canceled
					&& LastSyncPoint.GetState() == ERHIGPUSubmissionState::Canceled
					&& IndependentSyncPoint.GetState() == ERHIGPUSubmissionState::Canceled
					&& MissingReservation->GetSyncPoint().GetState() == ERHIGPUSubmissionState::Pending
					&& Graphics.GetCompletionTracker().GetLastSubmittedToken() == BeforeGraphics
					&& Compute.GetCompletionTracker().GetLastSubmittedToken() == BeforeCompute;
			}
			auto Missing = MakePayload(Compute);
			auto Dependent = MakePayload(Graphics);
			const auto DependentSyncPoint = Dependent->GetSyncPoint();
			Dependent->AddCompletionWait(Missing->GetSyncPoint());
			try { Device.GetSubmissionCoordinator().Submit(std::move(Dependent)); }
			catch (const std::runtime_error&) { Result.bBatchMissingProducerRejected = true; }
			Result.bBatchMissingProducerRejected &= DependentSyncPoint.GetState() == ERHIGPUSubmissionState::Canceled
				&& Missing->GetSyncPoint().GetState() == ERHIGPUSubmissionState::Pending;
		}
		catch (...)
		{
			ReleaseGate();
			Device.WaitUtilIdle();
			Device.GetHandle().destroySemaphore(Gate);
			throw;
		}
		Device.GetHandle().destroySemaphore(Gate);
		return Result;
	}

	auto TestVulkanSubmissionBoundary() -> FVulkanSubmissionBoundaryTestResult
	{
		CheckVulkanRHIThread();
		auto& Device = *FVulkanDynamicRHI::Get().GetDeviceForTesting();
		auto& Context = *Device.GetImmediateContext();
		auto& Tracker = Context.GetQueue()->GetCompletionTracker();
		const auto Before = Tracker.GetLastSubmittedToken();
		FVulkanSubmissionBoundaryTestResult Result;
		{
			// Construction order need not match reservation order on the same queue.
			const auto EarlierSyncPoint = Tracker.ReserveSyncPoint();
			const auto LaterSyncPoint = Tracker.ReserveSyncPoint();
			auto Later = std::make_unique<FVulkanPayload>(*Context.GetQueue(), LaterSyncPoint);
			auto Earlier = std::make_unique<FVulkanPayload>(*Context.GetQueue(), EarlierSyncPoint);
			Result.bPayloadUsesExplicitSyncPoint = FRHIGPUSyncPointBackend::GetPoint(Earlier->GetSyncPoint()) == FRHIGPUSyncPointBackend::GetPoint(EarlierSyncPoint)
				&& FRHIGPUSyncPointBackend::GetPoint(Later->GetSyncPoint()) == FRHIGPUSyncPointBackend::GetPoint(LaterSyncPoint)
				&& FRHIGPUSyncPointBackend::GetPoint(Tracker.GetLastReservedSyncPoint()) == FRHIGPUSyncPointBackend::GetPoint(LaterSyncPoint)
				&& Tracker.GetLastReservedToken() == FRHIGPUSyncPointBackend::GetPoint(LaterSyncPoint).Value;
			Earlier.reset();
			Result.bPayloadUsesExplicitSyncPoint &= EarlierSyncPoint.GetState() == ERHIGPUSubmissionState::Canceled
				&& LaterSyncPoint.GetState() == ERHIGPUSubmissionState::Pending;
			Later.reset();
			Result.bPayloadUsesExplicitSyncPoint &= LaterSyncPoint.GetState() == ERHIGPUSubmissionState::Canceled;
		}
		{
			// Independent contexts can interleave reservations on one physical queue.
			// Publishing the earlier recording must not capture the later reservation.
			Context.RHIBeginGPUSubmission({Context.GetQueue()->GetId(), {}});
			FVulkanCommandListContext Other(&FVulkanDynamicRHI::Get(), Device, Context.GetQueue());
			auto Later = Other.Finalize();
			const auto LogicalSignal = FRHIGPUSyncPoint::Create();
			Context.RHIEndGPUSubmission(LogicalSignal);
			auto Earlier = Context.Finalize();
			Result.bLogicalSignalUsesRecordingSyncPoint = FRHIGPUSyncPointBackend::GetPoint(LogicalSignal) == FRHIGPUSyncPointBackend::GetPoint(Earlier->GetSyncPoint())
				&& FRHIGPUSyncPointBackend::GetPoint(LogicalSignal) != FRHIGPUSyncPointBackend::GetPoint(Later->GetSyncPoint());
			Later.reset();
			Earlier.reset();
			Result.bLogicalSignalUsesRecordingSyncPoint &= LogicalSignal.GetState() == ERHIGPUSubmissionState::Canceled;
		}
		auto First = Context.Finalize();
		const auto FirstSyncPoint = First->GetSyncPoint();
		auto Storage = std::make_shared<int>(1);
		FVulkanGlobalDescriptorPool TestPools(Device);
		TestPools.PrepareForUse();
		Context.RetainAllocation(TestPools.GetAllocationOwner());
		std::weak_ptr<int> Weak = Storage;
		Context.RHISetReplayStorageOwner(Storage);
		auto Timing = Device.GetGPUTimingManager().CreateQuery();
		require(Timing && Timing->TryReserveRecording());
		Context.RHIBeginGPUTimingQuery(Timing);
		Context.RHIEndGPUTimingQuery(Timing);
		auto Second = Context.Finalize();
		const auto SecondSyncPoint = Second->GetSyncPoint();
		Context.RHISetReplayStorageOwner({});
		Storage.reset();
		TestPools.RetireUsedPools();
		Result.bAllocationRetained = TestPools.IsBatchRetainedForTesting(0);
		Result.bSealDidNotSubmit = Tracker.GetLastSubmittedToken() == Before
			&& FirstSyncPoint.GetState() == ERHIGPUSubmissionState::Pending
			&& SecondSyncPoint.GetState() == ERHIGPUSubmissionState::Pending;
		Result.bStorageRetained = !Weak.expired();
		Device.GetSubmissionCoordinator().Submit(std::move(First));
		Result.bEarlierSyncPointSubmitted = FirstSyncPoint.GetState() == ERHIGPUSubmissionState::Submitted
			&& SecondSyncPoint.GetState() == ERHIGPUSubmissionState::Pending;
		Second.reset();
		Result.bDiscardCanceled = SecondSyncPoint.GetState() == ERHIGPUSubmissionState::Canceled;
		Result.bTimingDiscarded = Timing->GetResult().State == ERHIGPUTimingResultState::Invalid
			&& Timing->TryReserveRecording();
		Timing->CancelRecording();
		Result.bStorageReleased = Weak.expired();
		Result.bAllocationReturned = !TestPools.IsBatchRetainedForTesting(0);
		TestPools.PrepareForUse();
		Result.bAllocationReturned &= TestPools.GetActiveBatchIndexForTesting() == 0;
		require(Tracker.WaitForSyncPoint(FirstSyncPoint, 1'000'000'000) == ERHIGPUWaitResult::Complete);
		for (bool bDiscard : {false, true})
		{
			auto Owner = std::make_shared<int>(42);
			std::weak_ptr<int> Observer = Owner;
			Context.RHISetReplayStorageOwner(Owner);
			Context.RetainAllocation(Owner);
			const auto RetainedCount = Owner.use_count();
			for (uint32 Repeat = 0; Repeat < 64; ++Repeat) Context.RetainAllocation(Owner);
			const bool bDeduplicated = Owner.use_count() == RetainedCount;
			const auto Queued = Device.GetSubmissionCoordinator().EnqueueContext(Context);
			Context.RHISetReplayStorageOwner({});
			Owner.reset();
			bool bPassed = bDeduplicated && !Observer.expired() && Queued.GetState() == ERHIGPUSubmissionState::Pending;
			if (bDiscard)
			{
				Device.GetSubmissionCoordinator().DiscardPending();
				Result.bQueuedStorageDiscarded = bPassed && Observer.expired()
					&& Queued.GetState() == ERHIGPUSubmissionState::Canceled;
			}
			else
			{
				// Submitting a new context recording drains its sealed predecessor too.
				const auto Last = Device.GetSubmissionCoordinator().SubmitContext(Context);
				bPassed &= FRHIGPUSyncPointBackend::GetPoint(Last).Value > FRHIGPUSyncPointBackend::GetPoint(Queued).Value;
				bPassed &= Tracker.WaitForSyncPoint(Last, 1'000'000'000) == ERHIGPUWaitResult::Complete;
				Result.bQueuedStorageRetired = bPassed && Observer.expired()
					&& Queued.GetState() == ERHIGPUSubmissionState::Complete;
			}
		}
		return Result;
	}

	// Expose recording dependencies only within backend test setup.
	class FVulkanPressureTestContext : public FVulkanCommandListContext
	{
	public:
		using FVulkanCommandListContext::FVulkanCommandListContext;
		using FVulkanCommandListContext::GetPayload;
	};

	auto TestVulkanSealedPressure() -> FVulkanSealedPressureTestResult
	{
		CheckVulkanRHIThread();
		auto& RHI = FVulkanDynamicRHI::Get();
		auto& Device = *RHI.GetDeviceForTesting();
		auto& Coordinator = Device.GetSubmissionCoordinator();
		auto* Queue = Device.GetGraphicsQueue();
		auto& Tracker = Queue->GetCompletionTracker();
		FVulkanPressureTestContext Context(&RHI, Device, Queue);
		FVulkanSealedPressureTestResult Result;
		{
			auto Gap = Context.Finalize();
			const auto Prefix = Coordinator.EnqueueContext(Context);
			FVulkanTransferArena Arena(Device, {
				.AllocationClass = EVulkanAllocationClassCandidate::DynamicUpload,
				.PageSize = 256, .MaxPageCount = 1, .DebugName = "SealedPressurePage",
				.MaxCapacity = 256, .MaxAllocation = 256, .BindingUsage = EBufferUsageFlags::UniformBuffer});
			auto Allocation = Arena.Acquire(256, 256, Context.GetPayload().GetSyncPoint());
			require(Allocation.Range);
			auto Owner = Allocation.Range.GetAllocationOwner();
			auto* Buffer = Allocation.Range.GetBuffer();
			std::weak_ptr<void> Weak = Owner;
			Context.RetainAllocation(Owner);
			Allocation.Range.Retire();
			const auto Target = Coordinator.EnqueueContext(Context);
			const auto Unrelated = Coordinator.EnqueueContext(Context);
			const auto Active = Context.GetPayload().GetSyncPoint();
			Owner.reset();
			const auto Pressure = Arena.Acquire(256, 256, Active);
			require(!Pressure.Range && Pressure.WaitOwner.lock() == Weak.lock()
				&& Pressure.WaitSyncPoint == Target);
			const auto Before = Tracker.GetLastSubmittedToken();
			Result.bMissingReservationPreserved = !Coordinator.TrySubmitPendingAllocation(Weak, Target)
				&& Tracker.GetLastSubmittedToken() == Before && !Weak.expired()
				&& Prefix.GetState() == ERHIGPUSubmissionState::Pending
				&& Target.GetState() == ERHIGPUSubmissionState::Pending;
			Gap.reset();
			Result.bOnlyRequiredPrefixSubmitted = Coordinator.TrySubmitPendingAllocation(Weak, Target)
				&& Prefix.GetState() == ERHIGPUSubmissionState::Submitted
				&& Target.GetState() == ERHIGPUSubmissionState::Submitted
				&& Unrelated.GetState() == ERHIGPUSubmissionState::Pending
				&& Active.GetState() == ERHIGPUSubmissionState::Pending && Context.HasPendingCommands();
			Result.bLeaseReleasedAfterWait = !Weak.expired();
			Coordinator.WaitForAllocation(Weak);
			auto Reused = Arena.Acquire(256, 256, Active);
			Result.bLeaseReleasedAfterWait &= Weak.expired() && Target.GetState() == ERHIGPUSubmissionState::Complete
				&& Reused.Range && Reused.Range.GetBuffer() == Buffer && Arena.GetCapacity() == 256;
			Coordinator.DiscardPending();
			Context.Finalize().reset();
		}
		{
			auto Owner = std::make_shared<int>(42);
			Context.RetainAllocation(Owner);
			const auto Sealed = Coordinator.EnqueueContext(Context);
			Context.RetainAllocation(Owner);
			const auto Active = Context.GetPayload().GetSyncPoint();
			Result.bActiveUsePreserved = !Coordinator.TrySubmitPendingAllocation(Owner, Active)
				&& Sealed.GetState() == ERHIGPUSubmissionState::Pending
				&& Active.GetState() == ERHIGPUSubmissionState::Pending && Context.HasPendingCommands();
			Context.Finalize().reset();
			Coordinator.DiscardPending();
		}
		{
			auto Owner = std::make_shared<int>(42);
			Context.RetainAllocation(Owner);
			FVulkanPressureTestContext Later(&RHI, Device, Queue);
			const auto Future = Coordinator.EnqueueContext(Later);
			Context.GetPayload().AddCompletionWait(Future);
			const auto Target = Coordinator.EnqueueContext(Context);
			const auto Before = Tracker.GetLastSubmittedToken();
			Result.bCyclePreserved = !Coordinator.TrySubmitPendingAllocation(Owner, Target)
				&& Tracker.GetLastSubmittedToken() == Before
				&& Target.GetState() == ERHIGPUSubmissionState::Pending
				&& Future.GetState() == ERHIGPUSubmissionState::Pending;
			Coordinator.DiscardPending();
		}
		return Result;
	}

	auto TestVulkanSealedPressureDependency() -> bool
	{
		CheckVulkanRHIThread();
		auto& RHI = FVulkanDynamicRHI::Get();
		auto& Device = *RHI.GetDeviceForTesting();
		auto& Coordinator = Device.GetSubmissionCoordinator();
		require(Device.GetGraphicsQueue() != Device.GetComputeQueue());
		FVulkanPressureTestContext Producer(&RHI, Device, Device.GetComputeQueue());
		FVulkanPressureTestContext Consumer(&RHI, Device, Device.GetGraphicsQueue());
		const auto Dependency = Producer.GetPayload().GetSyncPoint();
		auto Owner = std::make_shared<int>(42);
		std::weak_ptr<void> Weak = Owner;
		Consumer.RetainAllocation(Owner);
		Consumer.GetPayload().AddCompletionWait(Dependency);
		const auto Target = Coordinator.EnqueueContext(Consumer);
		Owner.reset();
		bool bPassed = !Coordinator.TrySubmitPendingAllocation(Weak, Target)
			&& Dependency.GetState() == ERHIGPUSubmissionState::Pending
			&& Target.GetState() == ERHIGPUSubmissionState::Pending && Producer.HasPendingCommands();
		Coordinator.EnqueueContext(Producer);
		bPassed &= Coordinator.TrySubmitPendingAllocation(Weak, Target)
			&& Dependency.GetState() == ERHIGPUSubmissionState::Submitted
			&& Target.GetState() == ERHIGPUSubmissionState::Submitted;
		Coordinator.WaitForAllocation(Weak);
		// The producer does not own the allocation. Retire its command buffers
		// separately before destroying this helper's local command pool.
		bPassed &= Device.GetComputeQueue()->GetCompletionTracker().WaitForSyncPoint(Dependency,
			1'000'000'000) == ERHIGPUWaitResult::Complete;
		return bPassed && Weak.expired() && Target.GetState() == ERHIGPUSubmissionState::Complete;
	}

	auto GetLastVulkanSyncPointForTesting() -> FRHIGPUSyncPointRef
	{
		CheckVulkanRHIThread();
		auto* Device = FVulkanDynamicRHI::Get().GetDeviceForTesting();
		return Device ? Device->GetCompletionTracker().GetLastReservedSyncPoint()
			: FRHIGPUSyncPointRef{};
	}

	auto TestVulkanSyncPointStorage() -> bool
	{
		CheckVulkanRHIThread();
		auto& Device = *FVulkanDynamicRHI::Get().GetDeviceForTesting();
		auto& Queue = *Device.GetGraphicsQueue();
		auto& Tracker = Queue.GetCompletionTracker();
		auto& Coordinator = Device.GetSubmissionCoordinator();
		Coordinator.SubmitPendingContexts();
		Tracker.WaitForAll();
		std::vector<FRHIGPUSyncPointRef> Observers;
		std::weak_ptr<FRHIGPUReservationState> FirstStorage;
		size_t WarmFenceCount = 0;
		bool bPassed = true;
		for (int Index = 0; Index != 128; ++Index)
		{
			auto Payload = std::make_unique<FVulkanPayload>(Queue, Tracker.ReserveSyncPoint());
			const auto First = FRHIGPUSyncPoint::Create(), Second = FRHIGPUSyncPoint::Create();
			require(Payload->AttachSignal(First) && Payload->AttachSignal(Second));
			Observers.push_back(First);
			Observers.push_back(Second);
			if (Index == 0) FirstStorage = FRHIGPUSyncPointBackend::GetReservation(First);
			if (Index % 2)
			{
				Payload.reset();
				bPassed &= First.GetState() == ERHIGPUSubmissionState::Canceled && !Second->IsComplete();
			}
			else
			{
				Coordinator.Submit(std::move(Payload));
				bPassed &= Tracker.WaitForSyncPoint(First, 1'000'000'000) == ERHIGPUWaitResult::Complete;
				bPassed &= First->IsComplete() && Second->IsComplete();
			}
			const auto FenceCount = Device.GetFenceManager().GetAllocatedFenceCountForTesting();
			if (Index == 0) WarmFenceCount = FenceCount;
			bPassed &= FenceCount == WarmFenceCount && Tracker.GetPendingSubmissionCount() == 0;
		}
		bPassed &= !FirstStorage.expired();
		Observers.clear();
		return bPassed && FirstStorage.expired();
	}

	auto GetVulkanCompletionTestStats() -> FVulkanCompletionTestStats
	{
		CheckVulkanRHIThread();
		FVulkanDevice* Device = FVulkanDynamicRHI::Get().GetDeviceForTesting();
		if (!Device)
		{
			return {};
		}
		FVulkanCompletionTracker& Tracker = Device->GetCompletionTracker();
		return {
			.LastReservedToken = Tracker.GetLastReservedToken(),
			.LastSubmittedToken = Tracker.GetLastSubmittedToken(),
			.CompletedToken = Tracker.GetCompletedToken(),
			.PendingSubmissionCount = Tracker.GetPendingSubmissionCount()};
	}

	auto GetVulkanBackendPoolTestStats() -> FVulkanBackendPoolTestStats
	{
		CheckVulkanRHIThread();
		FVulkanDevice* Device = FVulkanDynamicRHI::Get().GetDeviceForTesting();
		if (!Device)
		{
			return {};
		}
		return {
			.DescriptorPoolTokens = Device->GetGlobalDescriptorPool()
				.GetBatchTokensForTesting()};
	}

	auto SubmitAndRetireDescriptorPoolsForTesting() -> uint64
	{
		CheckVulkanRHIThread();
		FVulkanDevice* Device = FVulkanDynamicRHI::Get().GetDeviceForTesting();
		const FVulkanCompletionToken Token =
			FRHIGPUSyncPointBackend::GetPoint(Device->GetSubmissionCoordinator().SubmitContext(*Device->GetImmediateContext())).Value;
		Device->GetGlobalDescriptorPool().RetireUsedPools();
		return Token;
	}

	auto WaitForAllVulkanSubmissionsForTesting() -> void
	{
		CheckVulkanRHIThread();
		FVulkanDynamicRHI::Get().GetDeviceForTesting()
			->GetCompletionTracker().WaitForAll();
	}

	auto ReleaseCompletedVulkanResourcesForTesting() -> void
	{
		CheckVulkanRHIThread();
		FVulkanDynamicRHI::Get().GetDeviceForTesting()
			->GetDeferredDeletionQueue().ReleaseResources();
	}

	auto TestVulkanCommandBufferStateCache() -> bool
	{
		CheckVulkanRHIThread();
		auto& Device = *FVulkanDynamicRHI::Get().GetDeviceForTesting();
		FVulkanCommandBufferPool Pool(Device);
		Pool.CreatePool(Device.GetGraphicsQueue()->GetFamilyIndex());
		auto* Commands = Pool.Create();
		const auto Native = Device.GetHandle();
		const auto SetLayout = Native.createDescriptorSetLayout(vk::DescriptorSetLayoutCreateInfo{});
		const auto LayoutA = Native.createPipelineLayout(vk::PipelineLayoutCreateInfo{}.setSetLayouts(SetLayout));
		const auto LayoutB = Native.createPipelineLayout(vk::PipelineLayoutCreateInfo{}.setSetLayouts(SetLayout));
		const auto DescriptorPool = Native.createDescriptorPool(vk::DescriptorPoolCreateInfo{}.setMaxSets(2));
		const std::array SetLayouts{SetLayout, SetLayout};
		const auto Sets = Native.allocateDescriptorSets(vk::DescriptorSetAllocateInfo{}
			.setDescriptorPool(DescriptorPool).setSetLayouts(SetLayouts));
		const vk::Viewport Viewport{0, 0, 8, 8, 0, 1};
		const auto BufferDesc = FRHIBufferCreateDesc::Create("BindingCacheTest", 64, 4,
			EBufferUsageFlags::VertexBuffer | EBufferUsageFlags::IndexBuffer | EBufferUsageFlags::Static);
		TRefCountPtr<FVulkanBuffer> BufferA = new FVulkanBuffer(Device, BufferDesc);
		TRefCountPtr<FVulkanBuffer> BufferB = new FVulkanBuffer(Device, BufferDesc);
		vk::Rect2D Scissor{{0, 0}, {8, 8}};
		ResetVulkanHotPathWorkTestStats();
		Commands->Begin();
		Commands->SetGraphicsDynamicState(Viewport, Scissor, 0, 0, 0);
		Commands->SetGraphicsDynamicState(Viewport, Scissor, 0, 0, 0);
		auto Stats = GetVulkanHotPathWorkTestStats();
		bool bPassed = Stats.ViewportWrites == 1 && Stats.ScissorWrites == 1 && Stats.DepthBiasWrites == 1;
		Commands->BindVertexBuffer(0, BufferA->GetHandle(), 0);
		Commands->BindVertexBuffer(0, BufferA->GetHandle(), 0);
		Commands->BindVertexBuffer(1, BufferA->GetHandle(), 0);
		Commands->BindVertexBuffer(0, BufferA->GetHandle(), 4);
		Commands->BindVertexBuffer(0, BufferB->GetHandle(), 4);
		Commands->BindVertexBuffer(0, BufferA->GetHandle(), 0);
		Commands->BindIndexBuffer(BufferA->GetHandle(), 0, vk::IndexType::eUint16);
		Commands->BindIndexBuffer(BufferA->GetHandle(), 0, vk::IndexType::eUint16);
		Commands->BindIndexBuffer(BufferA->GetHandle(), 4, vk::IndexType::eUint16);
		Commands->BindIndexBuffer(BufferA->GetHandle(), 4, vk::IndexType::eUint32);
		Commands->BindIndexBuffer(BufferB->GetHandle(), 4, vk::IndexType::eUint32);
		Stats = GetVulkanHotPathWorkTestStats();
		bPassed &= Stats.VertexBufferBinds == 5 && Stats.IndexBufferBinds == 4;
		Scissor.extent.width = 4;
		Commands->SetGraphicsDynamicState(Viewport, Scissor, 0, 0, 0);
		Commands->SetGraphicsDynamicState(Viewport, Scissor, 0, 0, 2);
		Stats = GetVulkanHotPathWorkTestStats();
		bPassed &= Stats.ViewportWrites == 1 && Stats.ScissorWrites == 2 && Stats.DepthBiasWrites == 2;
		const auto FirstSet = std::span(Sets).first(1);
		const auto SecondSet = std::span(Sets).subspan(1, 1);
		Commands->BindDescriptorSets(vk::PipelineBindPoint::eGraphics, LayoutA, FirstSet, {});
		Commands->BindDescriptorSets(vk::PipelineBindPoint::eGraphics, LayoutA, FirstSet, {});
		// Compute and graphics bindings must not suppress each other.
		Commands->BindDescriptorSets(vk::PipelineBindPoint::eCompute, LayoutA, FirstSet, {});
		Commands->BindDescriptorSets(vk::PipelineBindPoint::eGraphics, LayoutA, SecondSet, {});
		Commands->BindDescriptorSets(vk::PipelineBindPoint::eGraphics, LayoutB, SecondSet, {});
		Commands->BindDescriptorSets(vk::PipelineBindPoint::eGraphics, LayoutA, SecondSet, {});
		bPassed &= GetVulkanHotPathWorkTestStats().DescriptorBinds == 5;
		Commands->End();
		Commands->Reset();
		Commands->Begin();
		// The same values must be emitted again in a new recording.
		Commands->BindVertexBuffer(0, BufferA->GetHandle(), 0);
		Commands->BindIndexBuffer(BufferB->GetHandle(), 4, vk::IndexType::eUint32);
		Stats = GetVulkanHotPathWorkTestStats();
		bPassed &= Stats.VertexBufferBinds == 6 && Stats.IndexBufferBinds == 5;
		Commands->SetGraphicsDynamicState(Viewport, Scissor, 0, 0, 2);
		Stats = GetVulkanHotPathWorkTestStats();
		bPassed &= Stats.ViewportWrites == 2 && Stats.ScissorWrites == 3 && Stats.DepthBiasWrites == 3;
		Commands->BindDescriptorSets(vk::PipelineBindPoint::eGraphics, LayoutA, SecondSet, {});
		bPassed &= GetVulkanHotPathWorkTestStats().DescriptorBinds == 6;
		Commands->End();
		Commands->Reset();
		Native.destroyDescriptorPool(DescriptorPool);
		Native.destroyPipelineLayout(LayoutB);
		Native.destroyPipelineLayout(LayoutA);
		Native.destroyDescriptorSetLayout(SetLayout);
		return bPassed;
	}

	auto ResetVulkanHotPathWorkTestStats() -> void
	{
		GVulkanSync2BufferBarrierCount.store(0, std::memory_order_release);
		GVulkanLegacyBufferBarrierCount.store(0, std::memory_order_release);
		GVulkanSync2ImageBarrierCount.store(0, std::memory_order_release);
		GVulkanLegacyImageBarrierCount.store(0, std::memory_order_release);
		GVulkanBindingValidationVisitCount.store(0, std::memory_order_release);
		GVulkanDescriptorDrawValidationVisitCount.store(0, std::memory_order_release);
		GVulkanDescriptorSortCount.store(0, std::memory_order_release);
		GVulkanDescriptorHashCount.store(0, std::memory_order_release);
		GVulkanViewportWriteCount.store(0, std::memory_order_release);
		GVulkanScissorWriteCount.store(0, std::memory_order_release);
		GVulkanDepthBiasWriteCount.store(0, std::memory_order_release);
		GVulkanPipelineBindCount.store(0, std::memory_order_release);
		GVulkanVertexBufferBindCount.store(0, std::memory_order_release);
		GVulkanIndexBufferBindCount.store(0, std::memory_order_release);
		GVulkanDescriptorBindCount.store(0, std::memory_order_release);
		GVulkanDescriptorOwnerRebuildCount.store(0, std::memory_order_release);
		GVulkanDescriptorOccupancyVerificationVisitCount.store(0, std::memory_order_release);
		GVulkanDescriptorOccupancyMutationCount.store(0, std::memory_order_release);
	}

	auto GetVulkanHotPathWorkTestStats() -> FVulkanHotPathWorkTestStats
	{
		return {
			.Sync2BufferBarriers = GVulkanSync2BufferBarrierCount.load(std::memory_order_acquire),
			.LegacyBufferBarriers = GVulkanLegacyBufferBarrierCount.load(std::memory_order_acquire),
			.Sync2ImageBarriers = GVulkanSync2ImageBarrierCount.load(std::memory_order_acquire),
			.LegacyImageBarriers = GVulkanLegacyImageBarrierCount.load(std::memory_order_acquire),
			.BindingValidationVisits = GVulkanBindingValidationVisitCount.load(std::memory_order_acquire),
			.DescriptorDrawValidationVisits = GVulkanDescriptorDrawValidationVisitCount.load(std::memory_order_acquire),
			.DescriptorSorts = GVulkanDescriptorSortCount.load(std::memory_order_acquire),
			.DescriptorHashes = GVulkanDescriptorHashCount.load(std::memory_order_acquire),
			.ViewportWrites = GVulkanViewportWriteCount.load(std::memory_order_acquire),
			.ScissorWrites = GVulkanScissorWriteCount.load(std::memory_order_acquire),
			.DepthBiasWrites = GVulkanDepthBiasWriteCount.load(std::memory_order_acquire),
			.PipelineBinds = GVulkanPipelineBindCount.load(std::memory_order_acquire),
			.VertexBufferBinds = GVulkanVertexBufferBindCount.load(std::memory_order_acquire),
			.IndexBufferBinds = GVulkanIndexBufferBindCount.load(std::memory_order_acquire),
			.DescriptorBinds = GVulkanDescriptorBindCount.load(std::memory_order_acquire),
			.DescriptorOwnerRebuilds = GVulkanDescriptorOwnerRebuildCount.load(std::memory_order_acquire),
			.DescriptorOccupancyVerificationVisits = GVulkanDescriptorOccupancyVerificationVisitCount.load(std::memory_order_acquire),
			.DescriptorOccupancyMutations = GVulkanDescriptorOccupancyMutationCount.load(std::memory_order_acquire)};
	}

	auto SetVulkanBarrierPathOverrideForTest(
		std::optional<bool> bUseSynchronization2) -> void
	{
		GVulkanBarrierPathOverride.store(
			bUseSynchronization2.has_value() ? (*bUseSynchronization2 ? 1 : 0) : -1,
			std::memory_order_release);
	}

	auto SelectVulkanSynchronization2BarrierPath(
		bool bSupportsSynchronization2) -> bool
	{
		const int32 Override = GVulkanBarrierPathOverride.load(
			std::memory_order_acquire);
		checkf(Override != 1 || bSupportsSynchronization2,
			"Synchronization2 barrier override requires device support.");
		return Override < 0 ? bSupportsSynchronization2 : Override != 0;
	}
} // namespace Durin::VulkanRHI
#endif
