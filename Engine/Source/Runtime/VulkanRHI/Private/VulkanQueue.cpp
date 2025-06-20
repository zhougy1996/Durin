#include "VulkanQueue.h"

#include "VulkanDevice.h"
#include "VulkanCommandBuffer.h"
#include "VulkanMemory.h"

FVulkanQueue::FVulkanQueue(FVulkanDevice* Device, uint32 FamilyIndex)
	: Device_(Device)
	, FamilyIndex_(FamilyIndex)
	, QueueIndex_(0)
{
	Queue_ = Device_->GetHandle().getQueue(FamilyIndex_, QueueIndex_);
}

auto FVulkanQueue::Submit(FVulkanCommandBuffer& CmdBuffer, FVulkanSemaphore* SignalSemaphores, uint32 NumSignalSemaphores /* = 1*/) -> void
{
	vk::CommandBuffer Buffer = CmdBuffer.GetHandle();

	vk::PipelineStageFlags waitStages[] = {vk::PipelineStageFlagBits::eColorAttachmentOutput};

	vk::SubmitInfo submitInfo;
	submitInfo.setCommandBuffers(Buffer);
	submitInfo.setWaitDstStageMask(waitStages);

	// Set signal semaphores
	TArray<vk::Semaphore> SignalSemaphoresArray{NumSignalSemaphores};
	if (NumSignalSemaphores > 0 && SignalSemaphores != nullptr)
	{
		for (uint32 i = 0; i < NumSignalSemaphores; ++i)
		{
			SignalSemaphoresArray[i] = SignalSemaphores[i].GetHandle();
		}
		submitInfo.setSignalSemaphores(SignalSemaphoresArray);
	}

	// Set wait semaphores
	TArray<FVulkanSemaphore*>& WaitSemaphores = CmdBuffer.WaitSemaphores_;
	TArray<vk::Semaphore> WaitSemaphoresArray{WaitSemaphores.size()};
	if (!WaitSemaphores.empty())
	{
		for (uint32 i = 0; i < WaitSemaphores.size(); ++i)
		{
			WaitSemaphoresArray[i] = WaitSemaphores[i]->GetHandle();
		}
		submitInfo.setWaitSemaphores(WaitSemaphoresArray);
	}
	WaitSemaphores.clear();

	FVulkanFence* Fence = CmdBuffer.GetFence();

	// Submit the command buffer
	Queue_.submit(submitInfo, Fence->GetHandle());

	CmdBuffer.MarkSemaphoresAsSubmitted();
	LastSubmittedCommandBuffer_ = &CmdBuffer;
}

auto FVulkanQueue::GetHandle() const -> vk::Queue
{
	return Queue_;
}

auto FVulkanQueue::GetFamilyIndex() const -> uint32
{
	return FamilyIndex_;
}

auto FVulkanQueue::GetIndex() const -> uint32
{
	return QueueIndex_;
}
