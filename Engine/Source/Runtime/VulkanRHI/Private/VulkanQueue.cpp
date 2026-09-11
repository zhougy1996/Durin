#include "VulkanQueue.h"

#include "VulkanDevice.h"
#include "VulkanCommandBuffer.h"
#include "VulkanCompletion.h"
#include "VulkanMemory.h"
#include "VulkanSubmission.h"
#include "VulkanRHIPrivate.h"

namespace Durin::VulkanRHI
{
	FVulkanQueue::FVulkanQueue(FVulkanDevice* InDevice, uint32 InFamilyIndex,
		uint32 InQueueIndex, FRHIQueueId InId)
		: Device(InDevice)
		, FamilyIndex(InFamilyIndex)
		, QueueIndex(InQueueIndex)
		, Id(InId)
		, CompletionTracker(std::make_unique<FVulkanCompletionTracker>(
			*InDevice, InDevice->GetDeviceGeneration(), InId))
	{
		Queue = Device->GetHandle().getQueue(FamilyIndex, QueueIndex);
		if (Device->SupportsTimelineSemaphores())
		{
			vk::SemaphoreTypeCreateInfo TypeInfo(vk::SemaphoreType::eTimeline, 0);
			vk::SemaphoreCreateInfo Info;
			Info.setPNext(&TypeInfo);
			TimelineSemaphore = Device->GetHandle().createSemaphore(Info);
		}
	}

	FVulkanQueue::~FVulkanQueue()
	{
		if (TimelineSemaphore) Device->GetHandle().destroySemaphore(TimelineSemaphore);
	}

	struct FVulkanSubmitInfoStorage
	{
		std::vector<vk::CommandBuffer> CmdBuffers;
		std::vector<vk::Semaphore> WaitSemaphores;
		std::vector<vk::Semaphore> SignalSemaphores;
		std::vector<vk::PipelineStageFlags> WaitStages;
		std::vector<uint64> WaitValues;
		std::vector<uint64> SignalValues;
		vk::TimelineSemaphoreSubmitInfo TimelineInfo;
	};

	auto FVulkanQueue::SubmitPayloads(std::vector<FVulkanPayload*>& Payloads)
		-> FVulkanCompletionToken
	{
		CheckVulkanRHIThread();
		check(!Payloads.empty());
		std::vector<FVulkanSubmitInfoStorage> SubmitInfoStorages;
		SubmitInfoStorages.reserve(Payloads.size());

		std::vector<vk::SubmitInfo> SubmitInfos;
		SubmitInfos.reserve(Payloads.size());

		for (FVulkanPayload* Payload : Payloads)
		{
			requiref(&Payload->Queue == this, "A Vulkan payload must submit on its owning physical queue.");
			auto& Storage = SubmitInfoStorages.emplace_back();
			vk::SubmitInfo SubmitInfo;

			auto& WaitSemaphores = Storage.WaitSemaphores;
			auto& CmdBuffers = Storage.CmdBuffers;
			auto& SignalSemaphores = Storage.SignalSemaphores;

			std::ranges::transform(Payload->WaitSemaphores, std::back_inserter(WaitSemaphores), &FVulkanSemaphore::GetHandle);
			Storage.WaitStages = Payload->WaitFlags;
			Storage.WaitValues.resize(WaitSemaphores.size(), 0);
			for (const auto& Ticket : Payload->CompletionWaits)
			{
				auto* Producer = Device->FindQueue(Ticket.GetPoint().Queue);
				const auto Status = Ticket.GetState();
				requiref(Producer && Producer->GetCompletionTracker().Owns(Ticket)
					&& (Status == ERHIGPUSubmissionState::Submitted || Status == ERHIGPUSubmissionState::Complete),
					"Cross-queue waits require an accepted producer on this device.");
				if (Producer == this) continue;
				requiref(TimelineSemaphore && Producer->GetTimelineSemaphore(), "Cross-queue waits require timeline semaphores.");
				const auto Existing = std::ranges::find(WaitSemaphores, Producer->GetTimelineSemaphore());
				if (Existing != WaitSemaphores.end())
				{
					auto& Value = Storage.WaitValues[static_cast<size_t>(Existing - WaitSemaphores.begin())];
					Value = std::max(Value, Ticket.GetPoint().Value);
					continue;
				}
				WaitSemaphores.push_back(Producer->GetTimelineSemaphore());
				Storage.WaitValues.push_back(Ticket.GetPoint().Value);
				Storage.WaitStages.push_back(vk::PipelineStageFlagBits::eAllCommands);
			}
			SubmitInfo.setWaitDstStageMask(Storage.WaitStages);
			SubmitInfo.setWaitSemaphores(WaitSemaphores);

			std::ranges::transform(Payload->CommandBuffers, std::back_inserter(CmdBuffers), &FVulkanCommandBuffer::GetHandle);
			SubmitInfo.setCommandBuffers(CmdBuffers);

			std::ranges::transform(Payload->SignalSemaphores, std::back_inserter(SignalSemaphores), &FVulkanSemaphore::GetHandle);
			Storage.SignalValues.resize(SignalSemaphores.size(), 0);
			if (TimelineSemaphore)
			{
				SignalSemaphores.push_back(TimelineSemaphore);
				Storage.SignalValues.push_back(Payload->Token);
				Storage.TimelineInfo.setWaitSemaphoreValues(Storage.WaitValues);
				Storage.TimelineInfo.setSignalSemaphoreValues(Storage.SignalValues);
				SubmitInfo.setPNext(&Storage.TimelineInfo);
			}
			SubmitInfo.setSignalSemaphores(SignalSemaphores);

			SubmitInfos.push_back(SubmitInfo);
		}

		FVulkanFence* Fence = Device->GetFenceManager().AllocateFence(false);
		auto& Tracker = GetCompletionTracker();
		check(Payloads.size() == 1);
		try
		{
			Tracker.PrepareSubmission(Payloads.front()->Token, Fence, Payloads);
		}
		catch (...)
		{
			Device->GetFenceManager().ReleaseFence(Fence);
			throw;
		}
		try
		{
			Queue.submit(SubmitInfos, Fence->GetHandle());
		}
		catch (const vk::SystemError& Error)
		{
			Tracker.FailSubmission(Error.code().value() == static_cast<int>(vk::Result::eErrorDeviceLost));
			Payloads.clear();
			throw;
		}
		catch (...)
		{
			Tracker.FailSubmission();
			Payloads.clear();
			throw;
		}
		for (auto* Payload : Payloads)
			std::ranges::for_each(Payload->CommandBuffers, &FVulkanCommandBuffer::SetSubmitted);
		return Tracker.CommitSubmission();
	}

	auto FVulkanQueue::GetHandle() const -> vk::Queue
	{
		return Queue;
	}

	auto FVulkanQueue::GetFamilyIndex() const -> uint32
	{
		return FamilyIndex;
	}

	auto FVulkanQueue::GetIndex() const -> uint32
	{
		return QueueIndex;
	}
} // namespace Durin::VulkanRHI
