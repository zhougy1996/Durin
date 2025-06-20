#pragma once

class FVulkanDevice;
class FVulkanCommandBuffer;
class FVulkanSemaphore;

class FVulkanQueue
{
public:
	FVulkanQueue(FVulkanDevice* Device, uint32 FamilyIndex);

	auto Submit(FVulkanCommandBuffer& CmdBuffer, FVulkanSemaphore* SignalSemaphores, uint32 NumSignalSemaphores = 1) -> void;

	auto GetHandle() const -> vk::Queue;

	auto GetFamilyIndex() const -> uint32;

	auto GetIndex() const -> uint32;

	auto GetLastSubmittedCommandBuffer() -> FVulkanCommandBuffer* { return LastSubmittedCommandBuffer_; }

private:
	vk::Queue Queue_;

	uint32 FamilyIndex_;
	uint32 QueueIndex_;

	FVulkanDevice* Device_;

	FVulkanCommandBuffer* LastSubmittedCommandBuffer_ = nullptr;
};