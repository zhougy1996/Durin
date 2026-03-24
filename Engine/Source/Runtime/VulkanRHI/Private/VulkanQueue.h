#pragma once

namespace Doge::VulkanRHI
{
	class FVulkanDevice;
	class FVulkanCommandBuffer;
	class FVulkanSemaphore;

	class FVulkanQueue
	{
	public:
		FVulkanQueue(FVulkanDevice* InDevice, uint32 InFamilyIndex);
		~FVulkanQueue();

		auto Submit(FVulkanCommandBuffer& InCmdBuffer, FVulkanSemaphore* InSignalSemaphores, uint32 NumSignalSemaphores = 1) -> void;

		auto GetHandle() const -> vk::Queue;

		auto GetFamilyIndex() const -> uint32;

		auto GetIndex() const -> uint32;

		auto GetLastSubmittedCommandBuffer() const -> FVulkanCommandBuffer* { return LastSubmittedCommandBuffer; }

	private:
		vk::Queue Queue;

		uint32 FamilyIndex;
		uint32 QueueIndex;

		FVulkanDevice* Device;

		FVulkanCommandBuffer* LastSubmittedCommandBuffer = nullptr;
	};
}