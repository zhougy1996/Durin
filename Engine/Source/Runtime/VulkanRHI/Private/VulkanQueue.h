#pragma once

namespace Durin::VulkanRHI
{
	class FVulkanDevice;
	class FVulkanCommandBuffer;
	class FVulkanSemaphore;
	class FVulkanPayload;

	class FVulkanQueue
	{
	public:
		FVulkanQueue(FVulkanDevice* InDevice, uint32 InFamilyIndex);
		~FVulkanQueue();

		auto SubmitPayloads(std::vector<FVulkanPayload*>& Payloads) -> void;

		auto GetHandle() const -> vk::Queue;

		auto GetFamilyIndex() const -> uint32;

		auto GetIndex() const -> uint32;

	private:
		vk::Queue Queue;

		uint32 FamilyIndex;
		uint32 QueueIndex;

		FVulkanDevice* Device;
	};
}