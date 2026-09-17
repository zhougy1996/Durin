#include "VulkanSubmission.h"
#include "Backend/RHICompletionBackend.h"

#include "VulkanCommandBuffer.h"
#include "VulkanCompletion.h"
#include "VulkanDevice.h"
#include "VulkanContext.h"
#include "VulkanQueue.h"
#include "VulkanGPUTiming.h"
#include "VulkanRHIPrivate.h"

namespace Durin::VulkanRHI
{
	FVulkanPayload::FVulkanPayload(FVulkanQueue& InQueue, FRHIGPUSyncPointRef InSyncPoint)
		: Queue(InQueue), SyncPoint(std::move(InSyncPoint))
	{
		require(Queue.GetCompletionTracker().Owns(SyncPoint)
			&& SyncPoint.GetState() == ERHIGPUSubmissionState::Pending);
	}

	FVulkanPayload::~FVulkanPayload()
	{
		if (!TimingQueries.empty()) Queue.GetDevice().GetGPUTimingManager().Discard(TimingQueries);
		if (SyncPoint.GetState() == ERHIGPUSubmissionState::Pending)
		{
			Queue.GetCompletionTracker().CancelUnsubmitted(SyncPoint);
			for (auto* CommandBuffer : CommandBuffers) CommandBuffer->Reset();
		}
	}

	auto FVulkanPayload::AttachSignal(const FRHIGPUSyncPointRef& Signal) -> bool
	{
		CheckVulkanRHIThread();
		// Retain the same logical object, allocating storage before publishing its binding.
		Signals.push_back(Signal);
		if (FRHIGPUSyncPointBackend::Attach(Signal, SyncPoint)) return true;
		Signals.pop_back();
		return false;
	}

	auto FVulkanSubmissionCoordinator::Submit(std::unique_ptr<FVulkanPayload> Payload) -> FRHIGPUSyncPointRef
	{
		require(Payload);
		const auto SyncPoint = Payload->GetSyncPoint();
		std::vector<std::unique_ptr<FVulkanPayload>> Batch;
		Batch.push_back(std::move(Payload));
		SubmitBatch(std::move(Batch));
		return SyncPoint;
	}

	auto FVulkanSubmissionCoordinator::SubmitBatch(std::vector<std::unique_ptr<FVulkanPayload>> Payloads) -> void
	{
		CheckVulkanRHIThread();
		Payloads.reserve(Payloads.size() + PendingPayloads.size());
		for (auto& Pending : PendingPayloads) Payloads.push_back(std::move(Pending));
		PendingPayloads.clear();
		// Build the entire dependency order before accepting any native work.
		// Queue-local reservations also impose edges, even without explicit waits.
		std::vector<std::vector<size_t>> Dependencies(Payloads.size());
		for (size_t Index = 0; Index < Payloads.size(); ++Index)
		{
			const auto& Payload = Payloads[Index];
			require(Payload && Device.FindQueue(FRHIGPUSyncPointBackend::GetPoint(Payload->SyncPoint).Queue) == &Payload->Queue
				&& Payload->Queue.GetCompletionTracker().Owns(Payload->SyncPoint)
				&& Payload->SyncPoint.GetState() == ERHIGPUSubmissionState::Pending);
			for (size_t Earlier = 0; Earlier < Payloads.size(); ++Earlier)
				if (Payloads[Earlier] && &Payloads[Earlier]->Queue == &Payload->Queue
					&& FRHIGPUSyncPointBackend::GetPoint(Payloads[Earlier]->SyncPoint).Value < FRHIGPUSyncPointBackend::GetPoint(Payload->SyncPoint).Value)
					Dependencies[Index].push_back(Earlier);
			for (const auto& Wait : Payload->CompletionWaits)
			{
				auto* Producer = Device.FindQueue(FRHIGPUSyncPointBackend::GetPoint(Wait).Queue);
				if (!Producer || !Producer->GetCompletionTracker().Owns(Wait))
					throw std::runtime_error("Vulkan batch dependency has foreign completion authority.");
				const auto State = Wait.GetState();
				if (State == ERHIGPUSubmissionState::Submitted || State == ERHIGPUSubmissionState::Complete) continue;
				if (State != ERHIGPUSubmissionState::Pending)
					throw std::runtime_error("Vulkan batch dependency cannot complete successfully.");
				const auto It = std::ranges::find_if(Payloads, [&](const auto& Candidate) {
					return Candidate && FRHIGPUSyncPointBackend::GetPoint(Candidate->SyncPoint) == FRHIGPUSyncPointBackend::GetPoint(Wait);
				});
				if (It == Payloads.end())
					throw std::runtime_error("Vulkan batch is missing an unsubmitted producer.");
				Dependencies[Index].push_back(static_cast<size_t>(It - Payloads.begin()));
			}
		}
		std::vector<size_t> Order;
		std::vector<bool> Scheduled(Payloads.size(), false);
		Order.reserve(Payloads.size());
		while (Order.size() != Payloads.size())
		{
			const auto Before = Order.size();
			for (size_t Index = 0; Index < Payloads.size(); ++Index)
				if (!Scheduled[Index] && std::ranges::all_of(Dependencies[Index],
					[&](size_t Producer) { return Scheduled[Producer]; }))
				{
					Scheduled[Index] = true;
					Order.push_back(Index);
				}
			if (Order.size() == Before)
				throw std::runtime_error("Vulkan submission batch contains a dependency cycle.");
		}
		// Topological edges alone cannot detect a reservation owned outside this batch.
		// Validate every queue's complete pending prefix before the first native call.
		for (const auto& QueueInfo : Device.GetQueueCapabilities().Queues)
		{
			std::vector<FRHIGPUSyncPointRef> SyncPoints;
			for (size_t Index : Order)
				if (FRHIGPUSyncPointBackend::GetPoint(Payloads[Index]->SyncPoint).Queue == QueueInfo.Id)
					SyncPoints.push_back(Payloads[Index]->SyncPoint);
			if (!SyncPoints.empty() && !Device.FindQueue(QueueInfo.Id)->GetCompletionTracker().CanSubmitBatch(SyncPoints))
				throw std::runtime_error("Vulkan batch is missing an earlier queue reservation.");
		}
		for (size_t Index : Order) SubmitNative(std::move(Payloads[Index]));
	}

	auto FVulkanSubmissionCoordinator::SubmitNative(std::unique_ptr<FVulkanPayload> Payload) -> void
	{
		CheckVulkanRHIThread();
		require(Payload && Device.FindQueue(FRHIGPUSyncPointBackend::GetPoint(Payload->SyncPoint).Queue) == &Payload->Queue);
		std::vector<FVulkanPayload*> NativePayloads{Payload.get()};
		try { Payload->Queue.SubmitPayloads(NativePayloads); }
		catch (...)
		{
			// An ambiguous native failure transfers ownership into quarantine.
			if (NativePayloads.empty()) Payload.release();
			throw;
		}
		auto* Submitted = Payload.release(); // The queue completion tracker now owns it.
		Device.GetGPUTimingManager().MarkSubmitted(Submitted->TimingQueries);
	}

	auto FVulkanSubmissionCoordinator::SubmitContext(FVulkanCommandListContext& Context) -> FRHIGPUSyncPointRef
	{
		const auto SyncPoint = EnqueueContext(Context);
		SubmitPendingContexts();
		return SyncPoint;
	}

	auto FVulkanSubmissionCoordinator::EnqueueContext(FVulkanCommandListContext& Context) -> FRHIGPUSyncPointRef
	{
		CheckVulkanRHIThread();
		auto Payload = Context.Finalize();
		const auto SyncPoint = Payload->GetSyncPoint();
		PendingPayloads.push_back(std::move(Payload));
		return SyncPoint;
	}

	auto FVulkanSubmissionCoordinator::DiscardPending() -> void
	{
		CheckVulkanRHIThread();
		PendingPayloads.clear();
	}

	auto FVulkanSubmissionCoordinator::SubmitPendingContexts(FVulkanCommandListContext* CallingContext) -> void
	{
		CheckVulkanRHIThread();
		std::vector<std::unique_ptr<FVulkanPayload>> Batch;
		if (CallingContext && CallingContext->HasPendingCommands()) Batch.push_back(CallingContext->Finalize());
		for (const auto& Queue : Device.GetQueueCapabilities().Queues)
			if (auto* Context = Device.GetQueueContext(Queue.Id); Context && Context->HasPendingCommands())
				Batch.push_back(Context->Finalize());
		SubmitBatch(std::move(Batch));
	}

	auto FVulkanSubmissionCoordinator::GetAllocationUses(const std::shared_ptr<void>& Owner) const -> FRHIRetirementPrerequisites
	{
		CheckVulkanRHIThread();
		FRHIRetirementPrerequisites Uses;
		for (const auto& Queue : Device.GetQueueCapabilities().Queues)
			Device.FindQueue(Queue.Id)->GetCompletionTracker().AppendAllocationUses(Owner, Uses);
		return Uses;
	}

	auto FVulkanSubmissionCoordinator::WaitForAllocation(const std::weak_ptr<void>& Owner) -> void
	{
		CheckVulkanRHIThread();
		if (auto Retained = Owner.lock())
		{
			const auto Uses = GetAllocationUses(Retained);
			requiref(!Uses.GetSyncPoints().empty(), "Allocation is still owned by an unsubmitted recording.");
			Device.WaitForUses(Uses);
		}
		requiref(Owner.expired(), "Allocation is still owned outside completed submissions.");
	}

	FVulkanFrame::FVulkanFrame(FVulkanDevice& InDevice)
		: Device(InDevice)
	{
		CheckVulkanRHIThread();
	}

	auto FVulkanFrame::Prepare() -> void
	{
		CheckVulkanRHIThread();
		Device.WaitForUses(RetirementUses);
	}

	auto FVulkanFrame::SetRetirementUses(FRHIRetirementPrerequisites Uses) -> void
	{
		CheckVulkanRHIThread();
		RetirementUses = std::move(Uses);
	}
}
