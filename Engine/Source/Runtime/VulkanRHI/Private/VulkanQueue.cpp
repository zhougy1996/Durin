#include "VulkanQueue.h"

#include "VulkanDevice.h"
#include "VulkanCommandBuffer.h"
#include "VulkanCompletion.h"
#include "VulkanMemory.h"
#include "VulkanSubmission.h"
#include "VulkanRHIPrivate.h"

namespace Durin::VulkanRHI
{
	FVulkanQueue::FVulkanQueue(FVulkanDevice* InDevice, uint32 InFamilyIndex)
		: Device(InDevice)
		, FamilyIndex(InFamilyIndex)
		, QueueIndex(0)
	{
		Queue = Device->GetHandle().getQueue(FamilyIndex, QueueIndex);
	}

	FVulkanQueue::~FVulkanQueue() = default;

	struct FVulkanSubmitInfoStorage
	{
		std::vector<vk::CommandBuffer> CmdBuffers;
		std::vector<vk::Semaphore> WaitSemaphores;
		std::vector<vk::Semaphore> SignalSemaphores;
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
			auto& Storage = SubmitInfoStorages.emplace_back();
			vk::SubmitInfo SubmitInfo;

			auto& WaitSemaphores = Storage.WaitSemaphores;
			auto& CmdBuffers = Storage.CmdBuffers;
			auto& SignalSemaphores = Storage.SignalSemaphores;

			std::ranges::transform(Payload->WaitSemaphores, std::back_inserter(WaitSemaphores), &FVulkanSemaphore::GetHandle);
			SubmitInfo.setWaitDstStageMask(Payload->WaitFlags);
			SubmitInfo.setWaitSemaphores(WaitSemaphores);

			std::ranges::transform(Payload->CommandBuffers, std::back_inserter(CmdBuffers), &FVulkanCommandBuffer::GetHandle);
			SubmitInfo.setCommandBuffers(CmdBuffers);

			std::ranges::transform(Payload->SignalSemaphores, std::back_inserter(SignalSemaphores), &FVulkanSemaphore::GetHandle);
			SubmitInfo.setSignalSemaphores(SignalSemaphores);

			SubmitInfos.push_back(SubmitInfo);
		}

		FVulkanFence* Fence = Device->GetFenceManager().AllocateFence(false);
		auto& Tracker = Device->GetCompletionTracker();
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
