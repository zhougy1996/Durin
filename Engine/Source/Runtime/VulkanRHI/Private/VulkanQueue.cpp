#include "VulkanQueue.h"

#include "VulkanDevice.h"
#include "VulkanCommandBuffer.h"
#include "VulkanMemory.h"

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

	auto FVulkanQueue::Submit(FVulkanCommandBuffer& InCmdBuffer, FVulkanSemaphore* InSignalSemaphores, uint32 NumSignalSemaphores /* = 1*/) -> void
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
		std::vector<FVulkanSemaphore*>& WaitSemaphores = InCmdBuffer.WaitSemaphores;
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

		FVulkanFence* Fence = InCmdBuffer.GetFence();

		// Submit the command buffer
		Queue.submit(submitInfo, Fence->GetHandle());

		InCmdBuffer.MarkSemaphoresAsSubmitted();
		InCmdBuffer.SetSubmitted();
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
}
