#include "VulkanSubmission.h"

#include "VulkanCommandBuffer.h"
#include "VulkanCompletion.h"
#include "VulkanDevice.h"
#include "VulkanContext.h"
#include "VulkanQueue.h"
#include "VulkanGPUTiming.h"
#include "VulkanRHIPrivate.h"

namespace Durin::VulkanRHI
{
	FVulkanPayload::FVulkanPayload(FVulkanQueue& InQueue, uint64 InToken)
		: Queue(InQueue), Token(InToken), Ticket(InQueue.GetCompletionTracker().GetLastReservedTicket())
	{
		require(Ticket.GetPoint().Value == Token);
	}

	FVulkanPayload::~FVulkanPayload()
	{
		if (!TimingQueries.empty()) Queue.GetDevice().GetGPUTimingManager().Discard(TimingQueries);
		if (Ticket.GetState() == ERHIGPUSubmissionState::Pending)
		{
			Queue.GetCompletionTracker().CancelUnsubmitted(Ticket);
			for (auto* CommandBuffer : CommandBuffers) CommandBuffer->Reset();
		}
	}

	auto FVulkanSubmissionCoordinator::Submit(std::unique_ptr<FVulkanPayload> Payload) -> FRHIGPUSubmissionTicket
	{
		require(Payload);
		const auto Ticket = Payload->GetTicket();
		std::vector<std::unique_ptr<FVulkanPayload>> Batch;
		Batch.push_back(std::move(Payload));
		SubmitBatch(std::move(Batch));
		return Ticket;
	}

	auto FVulkanSubmissionCoordinator::SubmitBatch(std::vector<std::unique_ptr<FVulkanPayload>> Payloads) -> void
	{
		CheckVulkanRHIThread();
		// Build the entire dependency order before accepting any native work.
		// Queue-local reservations also impose edges, even without explicit waits.
		std::vector<std::vector<size_t>> Dependencies(Payloads.size());
		for (size_t Index = 0; Index < Payloads.size(); ++Index)
		{
			const auto& Payload = Payloads[Index];
			require(Payload && Device.FindQueue(Payload->Ticket.GetPoint().Queue) == &Payload->Queue
				&& Payload->Queue.GetCompletionTracker().Owns(Payload->Ticket)
				&& Payload->Ticket.GetState() == ERHIGPUSubmissionState::Pending);
			for (size_t Earlier = 0; Earlier < Payloads.size(); ++Earlier)
				if (Payloads[Earlier] && &Payloads[Earlier]->Queue == &Payload->Queue
					&& Payloads[Earlier]->Token < Payload->Token)
					Dependencies[Index].push_back(Earlier);
			for (const auto& Wait : Payload->CompletionWaits)
			{
				auto* Producer = Device.FindQueue(Wait.GetPoint().Queue);
				if (!Producer || !Producer->GetCompletionTracker().Owns(Wait))
					throw std::runtime_error("Vulkan batch dependency has foreign completion authority.");
				const auto State = Wait.GetState();
				if (State == ERHIGPUSubmissionState::Submitted || State == ERHIGPUSubmissionState::Complete) continue;
				if (State != ERHIGPUSubmissionState::Pending)
					throw std::runtime_error("Vulkan batch dependency cannot complete successfully.");
				const auto It = std::ranges::find_if(Payloads, [&](const auto& Candidate) {
					return Candidate && Candidate->Ticket.GetPoint() == Wait.GetPoint();
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
		for (size_t Index : Order) SubmitNative(std::move(Payloads[Index]));
	}

	auto FVulkanSubmissionCoordinator::SubmitNative(std::unique_ptr<FVulkanPayload> Payload) -> void
	{
		CheckVulkanRHIThread();
		require(Payload && Device.FindQueue(Payload->Ticket.GetPoint().Queue) == &Payload->Queue);
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

	auto FVulkanSubmissionCoordinator::SubmitContext(FVulkanCommandListContext& Context) -> FRHIGPUSubmissionTicket
	{
		return Submit(Context.Finalize());
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
			requiref(!Uses.GetTickets().empty(), "Allocation is still owned by an unsubmitted recording.");
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
