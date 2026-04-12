#include "VulkanQueue.h"

#include "VulkanDevice.h"
#include "VulkanCommandBuffer.h"
#include "VulkanMemory.h"
#include "FVulkanSubmission.h"
#include "VulkanRHIPrivate.h"

namespace Doge::VulkanRHI
{
	FVulkanQueue::FVulkanQueue(FVulkanDevice* InDevice, uint32 InFamilyIndex)
		: Device(InDevice)
		, FamilyIndex(InFamilyIndex)
		, QueueIndex(0)
	{
		Queue = Device->GetHandle().getQueue(FamilyIndex, QueueIndex);
	}

	FVulkanQueue::~FVulkanQueue() = default;

	auto FVulkanQueue::Submit(FVulkanCommandBuffer& InCmdBuffer, std::vector<FVulkanSemaphore*>& WaitSemaphores, FVulkanSemaphore* InSignalSemaphores, uint32 NumSignalSemaphores /* = 1*/) -> void
	{
		vk::CommandBuffer Buffer = InCmdBuffer.GetHandle();

		vk::PipelineStageFlags waitStages[] = {vk::PipelineStageFlagBits::eColorAttachmentOutput};

		vk::SubmitInfo submitInfo;
		submitInfo.setCommandBuffers(Buffer);
		submitInfo.setWaitDstStageMask(waitStages);

		// Set signal semaphores
		std::vector<vk::Semaphore> SignalSemaphoresArray{NumSignalSemaphores};
		if (NumSignalSemaphores > 0 && InSignalSemaphores != nullptr)
		{
			for (uint32 i = 0; i < NumSignalSemaphores; ++i)
			{
				SignalSemaphoresArray[i] = InSignalSemaphores[i].GetHandle();
			}
			submitInfo.setSignalSemaphores(SignalSemaphoresArray);
		}

		// Set wait semaphores
		std::vector<vk::Semaphore> WaitSemaphoresArray{WaitSemaphores.size()};
		if (!WaitSemaphores.empty())
		{
			for (uint32 i = 0; i < WaitSemaphores.size(); ++i)
			{
				WaitSemaphoresArray[i] = WaitSemaphores[i]->GetHandle();
			}
			submitInfo.setWaitSemaphores(WaitSemaphoresArray);
		}
		WaitSemaphores.clear();

		// Submit the command buffer
		Queue.submit(submitInfo, nullptr);

		InCmdBuffer.SetSubmitted();
	}

	struct FVulkanSubmitInfoStorage
	{
		std::vector<vk::CommandBuffer> CmdBuffers;
		std::vector<vk::Semaphore> WaitSemaphores;
		std::vector<vk::Semaphore> SignalSemaphores;
	};

	auto FVulkanQueue::SubmitPayloads(std::vector<FVulkanPayload*>& Payloads) -> void
	{
		std::vector<FVulkanSubmitInfoStorage> SubmitInfoStorages;
		SubmitInfoStorages.reserve(Payloads.size());
		vk::Fence Fence = VK_NULL_HANDLE;

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
			std::ranges::for_each(Payload->CommandBuffers, &FVulkanCommandBuffer::SetSubmitted);
			SubmitInfo.setCommandBuffers(CmdBuffers);

			std::ranges::transform(Payload->SignalSemaphores, std::back_inserter(SignalSemaphores), &FVulkanSemaphore::GetHandle);
			SubmitInfo.setSignalSemaphores(SignalSemaphores);

			if (Payload->Fence)
			{
				check(Fence == VK_NULL_HANDLE); // Only one fence per submit.
				Fence = Payload->Fence->GetHandle();
			}
			SubmitInfos.push_back(SubmitInfo);
		}

		Queue.submit(SubmitInfos, Fence);

		auto& FrameContext = Device->GetCurrentFrame();

		FrameContext.TrackInFlightPayload(Payloads);
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
} // namespace Doge::VulkanRHI
