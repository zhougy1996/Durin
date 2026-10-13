#include "VulkanTestSupport.h"

#include "VulkanRHIPrivate.h"
#include "Threading/RunnableThread.h"
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
	namespace
	{
		auto GetTestDevice(FVulkanDynamicRHI& RHI = FVulkanDynamicRHI::Get()) -> FVulkanDevice*
		{
			return GetVulkanDeviceForTest(RHI);
		}

		auto GetTestDispatcher() -> vk::DispatchLoaderDynamic
		{
			auto& RHI = *static_cast<IVulkanDynamicRHI*>(GDynamicRHI);
			vk::DispatchLoaderDynamic Dispatcher;
			Dispatcher.init(::vkGetInstanceProcAddr);
			Dispatcher.init(RHI.RHIGetVkInstance());
			Dispatcher.init(RHI.RHIGetVkDevice());
			return Dispatcher;
		}

	}
	auto GetVulkanDeviceForTest(FVulkanDynamicRHI& RHI) -> FVulkanDevice*
	{
		FVulkanDevice* Device = nullptr;
		auto ReadDevice = [&] {
			auto* Context = static_cast<FVulkanCommandListContext*>(
				static_cast<FDynamicRHI&>(RHI).RHIGetDefaultContext());
			Device = Context ? &Context->GetQueue()->GetDevice() : nullptr;
		};
		if (GRHIThread && !IsInRHIThread())
			GCommandListExecutor.ExecuteSynchronousOperation(false, ReadDevice);
		else ReadDevice();
		return Device;
	}

	auto TestVulkanUniformBufferAdmission() -> bool
	{
		constexpr uint64 PageSize = 4ull * 1024 * 1024;
		auto Admission = std::make_shared<FVulkanUniformBufferAdmission>(256);
		auto A = Admission->Reserve(1), B = Admission->Reserve(1), Tail = Admission->Reserve(PageSize - 512);
		if (!A || !B || !Tail) return false;
		if (A->Slot.Offset != 0 || B->Slot.Offset != 256
			|| Tail->Slot.Offset != 512) return false;
		// Fragmented pages grow without waiting; released adjacent intervals are reused.
		B.reset();
		auto Grown = Admission->Reserve(512);
		if (!Grown || Grown->Slot.Page == A->Slot.Page) return false;
		A.reset();
		auto Reused = Admission->Reserve(512);
		if (!Reused || Reused->Slot.Offset != 0) return false;
		Reused.reset(); Tail.reset();
		std::vector<std::shared_ptr<FVulkanUniformBufferAdmission::FReservation>> Concurrent(16);
		std::vector<std::thread> Threads;
		for (size_t Index = 0; Index < Concurrent.size(); ++Index)
			Threads.emplace_back([&, Index] { Concurrent[Index] = Admission->Reserve(PageSize / 8); });
		for (auto& Thread : Threads) Thread.join();
		return std::ranges::count_if(Concurrent, [](const auto& Item) { return bool(Item); }) == 16;
	}

	auto PollVulkanGPUTimingForTest(FVulkanDynamicRHI& RHI) -> void
	{
		auto& Device = *GetTestDevice(RHI);
		Device.PollQueues();
	}
	auto GetVulkanGPUTimingStatisticsForTest(FVulkanDynamicRHI& RHI)
		-> FVulkanGPUTimingStatistics
	{
		if (IsInRHIThread())
			return GetTestDevice(RHI)->GetGPUTimingManager().Snapshot();
		FVulkanGPUTimingStatistics Result;
		GCommandListExecutor.ExecuteSynchronousOperation(false, [&]() {
			Result = GetTestDevice(RHI)->GetGPUTimingManager().Snapshot();
		});
		return Result;
	}
	auto ReserveVulkanUniformBufferBackingForTest(uint32 Size) -> std::shared_ptr<void>
	{
		auto* Device = GetTestDevice();
		require(Device);
		return Device->GetUniformBufferPool().Reserve(Size);
	}
	auto GetVulkanDeviceDescriptionForTiming() -> std::string
	{
		const auto& Props = GetTestDevice()->GetGpuProperties();
		FRHIDiagnosticAvailability Availability;
		auto CaptureAvailability = [&] { Availability = GDynamicRHI->RHIGetDiagnosticSnapshot().Availability; };
		if (GRHIThread && !IsInRHIThread())
			GCommandListExecutor.ExecuteSynchronousOperation(false, CaptureAvailability);
		else CaptureAvailability();
		return std::format("gpu={}\nvendor={}\ndevice={}\ndriver={}\napi={}\nvalidation_active={}\ndebug_utils_active={}\n",
			Props.deviceName.data(), Props.vendorID, Props.deviceID, Props.driverVersion, Props.apiVersion,
			Availability.bValidationLayerActive, Availability.bDebugUtilsActive);
	}
	auto GetVulkanDeviceLuidForTiming() -> std::optional<std::array<std::byte, 8>>
	{
		const auto* Device = GetTestDevice();
		if (!Device) return std::nullopt;
		const auto Properties = Device->GetGpu().getProperties2<vk::PhysicalDeviceProperties2,
			vk::PhysicalDeviceIDProperties>(GetTestDispatcher());
		const auto& Id = Properties.get<vk::PhysicalDeviceIDProperties>();
		if (!Id.deviceLUIDValid) return std::nullopt;
		std::array<std::byte, 8> Result;
		static_assert(Result.size() == VK_LUID_SIZE);
		std::memcpy(Result.data(), Id.deviceLUID.data(), Result.size());
		return Result;
	}

	auto RunVulkanQueueTransferForTesting(bool bSynchronization2) -> FVulkanQueueTransferTestResult
	{
		CheckVulkanRHIThread();
		auto& RHI = FVulkanDynamicRHI::Get();
		auto& Device = *GetTestDevice(RHI);
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
		auto& Compute = *Device.GetQueueContext(DestinationQueue.GetId());
		struct FDrainBeforePoolDestruction
		{
			FVulkanDevice& Device;
			~FDrainBeforePoolDestruction() { try { Device.WaitUtilIdle(); } catch (const vk::SystemError&) {} }
		} Drain{Device};
		const FRHITextureSubresourceRange Range{ERHITextureAspect::Color, 1, 1, 0, 1};
		const std::array InitializeBuffer{FRHIBufferTransition{Shared.GetReference(), 0, 64, ERHIAccess::None, ERHIAccess::TransferWrite}};
		const std::array InitializeTexture{FRHITextureTransition{Texture.GetReference(), Range, ERHIAccess::None, ERHIAccess::TransferWrite}};
		static_cast<IRHICommandContext&>(Graphics).RHITransitionBuffers(InitializeBuffer);
		static_cast<IRHICommandContext&>(Graphics).RHITransitionTextures(InitializeTexture);
		Graphics.GetCommandBuffer()->GetHandle().fillBuffer(Shared->GetHandle(), 0, 64, 0x12345678, GetTestDispatcher());
		Graphics.GetCommandBuffer()->GetHandle().clearColorImage(Texture->Image, vk::ImageLayout::eTransferDstOptimal,
			vk::ClearColorValue(std::array<float, 4>{1, 0, 0, 1}), vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 1, 1, 0, 1), GetTestDispatcher());
		const std::array BufferTransitions{FRHIBufferTransition{Shared.GetReference(), 16, 16, ERHIAccess::TransferWrite, ERHIAccess::TransferRead}};
		const std::array TextureTransitions{FRHITextureTransition{Texture.GetReference(), Range, ERHIAccess::TransferWrite, ERHIAccess::TransferRead}};
		auto Transfer = std::make_shared<FVulkanQueueTransfer>(Device, SourceQueue.GetId(), DestinationQueue.GetId(),
			BufferTransitions, TextureTransitions, bSynchronization2);
		// A canceled later recording must not replace the release's producer sync point.
		auto LaterGraphics = std::make_unique<FVulkanPayload>(SourceQueue,
			SourceQueue.GetCompletionTracker().ReserveSyncPoint());
		Graphics.ReleaseQueueOwnership(Transfer);
		LaterGraphics.reset();
		Device.GetSubmissionCoordinator().EnqueueContext(Graphics);
		Compute.AcquireQueueOwnership(Transfer);
		const std::array InitializeReadback{FRHIBufferTransition{Readback.GetReference(), 0, 64, ERHIAccess::None, ERHIAccess::TransferWrite}};
		static_cast<IRHICommandContext&>(Compute).RHITransitionBuffers(InitializeReadback);
		Compute.GetCommandBuffer()->GetHandle().copyBuffer(Shared->GetHandle(), Readback->GetHandle(), vk::BufferCopy(16, 0, 16), GetTestDispatcher());
		const vk::BufferImageCopy ImageCopy(16, 0, 0, {vk::ImageAspectFlagBits::eColor, 1, 0, 1}, {0, 0, 0}, {2, 2, 1});
		Compute.GetCommandBuffer()->GetHandle().copyImageToBuffer(Texture->Image, vk::ImageLayout::eTransferSrcOptimal, Readback->GetHandle(), ImageCopy, GetTestDispatcher());
		const std::array HostRead{FRHIBufferTransition{Readback.GetReference(), 0, 64, ERHIAccess::TransferWrite, ERHIAccess::HostRead}};
		static_cast<IRHICommandContext&>(Compute).RHITransitionBuffers(HostRead);
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
		auto& Device = *GetTestDevice();
		auto& Compute = *Device.GetComputeQueue();
		require(&Compute != Device.GetGraphicsQueue() && Device.SupportsTimelineSemaphores());
		vk::Semaphore Gate;
		GCommandListExecutor.ExecuteSynchronousOperation(false, [&] {
			CheckVulkanRHIThread();
			vk::SemaphoreTypeCreateInfo Type(vk::SemaphoreType::eTimeline, 0);
			vk::SemaphoreCreateInfo Info;
			Info.setPNext(&Type);
			Gate = Device.GetHandle().createSemaphore(Info, nullptr, GetTestDispatcher());
		});
		bool bReleased = false;
		const std::function<void()> Release = [&] {
			if (bReleased) return;
			GCommandListExecutor.ExecuteSynchronousOperation(false, [&] {
				const vk::SemaphoreSignalInfo Signal(Gate, 1);
				if (Device.GetGpuProperties().apiVersion >= VK_API_VERSION_1_2)
					Device.GetHandle().signalSemaphore(Signal, GetTestDispatcher());
				else Device.GetHandle().signalSemaphoreKHR(Signal, GetTestDispatcher());
			});
			bReleased = true;
		};
		auto Cleanup = [&] {
			Release();
			GCommandListExecutor.ExecuteSynchronousOperation(false, [&] {
				Compute.GetHandle().waitIdle(GetTestDispatcher());
				Device.GetHandle().destroySemaphore(Gate, nullptr, GetTestDispatcher());
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
				Compute.GetHandle().submit(Submit, {}, GetTestDispatcher());
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
		auto& Device = *GetTestDevice();
		auto& Compute = *Device.GetComputeQueue();
		auto& Graphics = *Device.GetGraphicsQueue();
		require(&Compute != &Graphics && Device.SupportsTimelineSemaphores());
		vk::SemaphoreTypeCreateInfo GateType(vk::SemaphoreType::eTimeline, 0);
		vk::SemaphoreCreateInfo GateInfo;
		GateInfo.setPNext(&GateType);
		const auto Gate = Device.GetHandle().createSemaphore(GateInfo, nullptr, GetTestDispatcher());
		bool bGateReleased = false;
		auto ReleaseGate = [&] {
			if (bGateReleased) return;
			const vk::SemaphoreSignalInfo Signal(Gate, 1);
			if (Device.GetGpuProperties().apiVersion >= VK_API_VERSION_1_2)
				Device.GetHandle().signalSemaphore(Signal, GetTestDispatcher());
			else Device.GetHandle().signalSemaphoreKHR(Signal, GetTestDispatcher());
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
				static_cast<IRHICommandContext&>(Context).RHIBeginGPUTimingQuery(Timing);
				static_cast<IRHICommandContext&>(Context).RHIEndGPUTimingQuery(Timing);
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
			const auto Layout = Device.GetHandle().createDescriptorSetLayout(vk::DescriptorSetLayoutCreateInfo().setBindings(Binding), nullptr, GetTestDispatcher());
			struct FLayoutOwner
			{
				FVulkanDevice& Device;
				vk::DescriptorSetLayout Layout;
				~FLayoutOwner() { Device.GetHandle().destroyDescriptorSetLayout(Layout, nullptr, GetTestDispatcher()); }
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
			Compute.GetHandle().submit(GateSubmit, {}, GetTestDispatcher());
			const auto Producer = Submit(Compute, {}, TestPools.GetAllocationOwner(), Timing.GetReference());
			// A graphics frame can pace independently while compute still owns resources.
			static_cast<IRHICommandContext*>(Device.GetImmediateContext())->RHIEndFrame();
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
			Device.GetHandle().destroySemaphore(Gate, nullptr, GetTestDispatcher());
			throw;
		}
		Device.GetHandle().destroySemaphore(Gate, nullptr, GetTestDispatcher());
		return Result;
	}

	auto TestVulkanSubmissionBoundary() -> FVulkanSubmissionBoundaryTestResult
	{
		CheckVulkanRHIThread();
		auto& Device = *GetTestDevice();
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
			// Later reservations must not replace the current recording sync point.
			// Publishing the earlier recording must not capture the later reservation.
			static_cast<IRHICommandContext&>(Context).RHIBeginGPUSubmission({Context.GetQueue()->GetId(), {}});
			auto Later = std::make_unique<FVulkanPayload>(*Context.GetQueue(), Tracker.ReserveSyncPoint());
			const auto LogicalSignal = FRHIGPUSyncPoint::Create();
			static_cast<IRHICommandContext&>(Context).RHIEndGPUSubmission(LogicalSignal);
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
		std::weak_ptr<void> Allocation = TestPools.GetAllocationOwner();
		Context.RetainAllocation(TestPools.GetAllocationOwner());
		std::weak_ptr<int> Weak = Storage;
		static_cast<IRHICommandContext&>(Context).RHISetReplayStorageOwner(Storage);
		auto Timing = Device.GetGPUTimingManager().CreateQuery();
		require(Timing && Timing->TryReserveRecording());
		static_cast<IRHICommandContext&>(Context).RHIBeginGPUTimingQuery(Timing);
		static_cast<IRHICommandContext&>(Context).RHIEndGPUTimingQuery(Timing);
		auto Second = Context.Finalize();
		const auto SecondSyncPoint = Second->GetSyncPoint();
		static_cast<IRHICommandContext&>(Context).RHISetReplayStorageOwner({});
		Storage.reset();
		TestPools.RetireUsedPools();
		Result.bAllocationRetained = !Allocation.expired();
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
		Result.bAllocationReturned = Allocation.expired();
		TestPools.PrepareForUse();
		require(Tracker.WaitForSyncPoint(FirstSyncPoint, 1'000'000'000) == ERHIGPUWaitResult::Complete);
		for (bool bDiscard : {false, true})
		{
			auto Owner = std::make_shared<int>(42);
			std::weak_ptr<int> Observer = Owner;
			static_cast<IRHICommandContext&>(Context).RHISetReplayStorageOwner(Owner);
			Context.RetainAllocation(Owner);
			const auto RetainedCount = Owner.use_count();
			for (uint32 Repeat = 0; Repeat < 64; ++Repeat) Context.RetainAllocation(Owner);
			const bool bDeduplicated = Owner.use_count() == RetainedCount;
			const auto Queued = Device.GetSubmissionCoordinator().EnqueueContext(Context);
			static_cast<IRHICommandContext&>(Context).RHISetReplayStorageOwner({});
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

	// Force a recording through the ordinary command-buffer entry point.
	static auto GetRecordingSyncPoint(FVulkanCommandListContext& Context) -> FRHIGPUSyncPointRef
	{
		Context.GetCommandBuffer();
		return Context.GetQueue()->GetCompletionTracker().GetLastReservedSyncPoint();
	}

	auto TestVulkanSealedPressure() -> FVulkanSealedPressureTestResult
	{
		CheckVulkanRHIThread();
		auto& RHI = FVulkanDynamicRHI::Get();
		auto& Device = *GetTestDevice(RHI);
		auto& Coordinator = Device.GetSubmissionCoordinator();
		auto* Queue = Device.GetGraphicsQueue();
		auto& Tracker = Queue->GetCompletionTracker();
		auto& Context = *Device.GetImmediateContext();
		FVulkanSealedPressureTestResult Result;
		{
			auto Gap = Context.Finalize();
			const auto Prefix = Coordinator.EnqueueContext(Context);
			FVulkanTransferArena Arena(Device, {
				.AllocationClass = EVulkanAllocationClassCandidate::DynamicUpload,
				.PageSize = 256, .MaxPageCount = 1, .DebugName = "SealedPressurePage",
				.MaxCapacity = 256, .MaxAllocation = 256, .BindingUsage = EBufferUsageFlags::UniformBuffer});
			auto Allocation = Arena.Acquire(256, 256, GetRecordingSyncPoint(Context));
			require(Allocation.Range);
			auto Owner = Allocation.Range.GetAllocationOwner();
			auto* Buffer = Allocation.Range.GetBuffer();
			std::weak_ptr<void> Weak = Owner;
			Context.RetainAllocation(Owner);
			Allocation.Range.Retire();
			const auto Target = Coordinator.EnqueueContext(Context);
			const auto Unrelated = Coordinator.EnqueueContext(Context);
			const auto Active = GetRecordingSyncPoint(Context);
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
			const auto Active = GetRecordingSyncPoint(Context);
			Result.bActiveUsePreserved = !Coordinator.TrySubmitPendingAllocation(Owner, Active)
				&& Sealed.GetState() == ERHIGPUSubmissionState::Pending
				&& Active.GetState() == ERHIGPUSubmissionState::Pending && Context.HasPendingCommands();
			Context.Finalize().reset();
			Coordinator.DiscardPending();
		}

		return Result;
	}

	auto TestVulkanSealedPressureDependency() -> bool
	{
		CheckVulkanRHIThread();
		auto& RHI = FVulkanDynamicRHI::Get();
		auto& Device = *GetTestDevice(RHI);
		auto& Coordinator = Device.GetSubmissionCoordinator();
		require(Device.GetGraphicsQueue() != Device.GetComputeQueue());
		auto& Producer = *Device.GetQueueContext(Device.GetComputeQueue()->GetId());
		auto& Consumer = *Device.GetImmediateContext();
		const auto Dependency = GetRecordingSyncPoint(Producer);
		auto Owner = std::make_shared<int>(42);
		std::weak_ptr<void> Weak = Owner;
		Consumer.RetainAllocation(Owner);
		auto& Commands = static_cast<IRHICommandContext&>(Consumer);
		Commands.RHIBeginGPUSubmission({Consumer.GetQueue()->GetId(), {Dependency}});
		const auto Target = FRHIGPUSyncPoint::Create();
		Commands.RHIEndGPUSubmission(Target);
		Owner.reset();
		bool bPassed = !Coordinator.TrySubmitPendingAllocation(Weak, Target)
			&& Dependency.GetState() == ERHIGPUSubmissionState::Pending
			&& Target.GetState() == ERHIGPUSubmissionState::Pending && Producer.HasPendingCommands();
		Coordinator.EnqueueContext(Producer);
		bPassed &= Coordinator.TrySubmitPendingAllocation(Weak, Target)
			&& Dependency.GetState() == ERHIGPUSubmissionState::Submitted
			&& Target.GetState() == ERHIGPUSubmissionState::Submitted;
		Coordinator.WaitForAllocation(Weak);
		// The producer does not own the allocation; wait for its completion separately.
		bPassed &= Device.GetComputeQueue()->GetCompletionTracker().WaitForSyncPoint(Dependency,
			1'000'000'000) == ERHIGPUWaitResult::Complete;
		return bPassed && Weak.expired() && Target.GetState() == ERHIGPUSubmissionState::Complete;
	}

	auto GetLastVulkanSyncPointForTesting() -> FRHIGPUSyncPointRef
	{
		CheckVulkanRHIThread();
		auto* Device = GetTestDevice();
		return Device ? Device->GetCompletionTracker().GetLastReservedSyncPoint()
			: FRHIGPUSyncPointRef{};
	}

	auto TestVulkanSyncPointStorage() -> bool
	{
		CheckVulkanRHIThread();
		auto& Device = *GetTestDevice();
		auto& Queue = *Device.GetGraphicsQueue();
		auto& Tracker = Queue.GetCompletionTracker();
		auto& Coordinator = Device.GetSubmissionCoordinator();
		Coordinator.SubmitPendingContexts();
		Tracker.WaitForAll();
		std::vector<FRHIGPUSyncPointRef> Observers;
		std::weak_ptr<FRHIGPUReservationState> FirstStorage;
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
			bPassed &= Tracker.GetPendingSubmissionCount() == 0;
		}
		bPassed &= !FirstStorage.expired();
		Observers.clear();
		return bPassed && FirstStorage.expired();
	}

	auto GetVulkanCompletionTestStats() -> FVulkanCompletionTestStats
	{
		CheckVulkanRHIThread();
		FVulkanDevice* Device = GetTestDevice();
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

	auto SubmitAndRetireDescriptorPoolsForTesting() -> uint64
	{
		CheckVulkanRHIThread();
		FVulkanDevice* Device = GetTestDevice();
		const FVulkanCompletionToken Token =
			FRHIGPUSyncPointBackend::GetPoint(Device->GetSubmissionCoordinator().SubmitContext(*Device->GetImmediateContext())).Value;
		Device->GetGlobalDescriptorPool().RetireUsedPools();
		return Token;
	}

	auto WaitForAllVulkanSubmissionsForTesting() -> void
	{
		CheckVulkanRHIThread();
		GetTestDevice()
			->GetCompletionTracker().WaitForAll();
	}

	auto ReleaseCompletedVulkanResourcesForTesting() -> void
	{
		CheckVulkanRHIThread();
		GetTestDevice()
			->GetDeferredDeletionQueue().ReleaseResources();
	}

} // namespace Durin::VulkanRHI
