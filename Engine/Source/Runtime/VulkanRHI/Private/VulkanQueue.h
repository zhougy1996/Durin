#pragma once
#include "RHICompletion.h"

namespace Durin::VulkanRHI
{
	class FVulkanDevice;
	class FVulkanCommandBuffer;
	class FVulkanSemaphore;
	class FVulkanPayload;
	class FVulkanCompletionTracker;
	using FVulkanCompletionToken = uint64;

	// Serializes submissions to one Vulkan queue and tracks its family capabilities.
	class FVulkanQueue
	{
	public:
		FVulkanQueue(FVulkanDevice* InDevice, uint32 InFamilyIndex, uint32 InQueueIndex, FRHIQueueId InId);
		~FVulkanQueue();

		auto SubmitPayloads(std::vector<FVulkanPayload*>& Payloads)
			-> FVulkanCompletionToken;

		auto GetHandle() const -> vk::Queue;

		auto GetFamilyIndex() const -> uint32;

		auto GetIndex() const -> uint32;
		auto GetId() const -> FRHIQueueId { return Id; }
		auto GetCompletionTracker() const -> FVulkanCompletionTracker& { return *CompletionTracker; }
		auto GetTimelineSemaphore() const -> vk::Semaphore { return TimelineSemaphore; }

	private:
		vk::Queue Queue;

		uint32 FamilyIndex;
		uint32 QueueIndex;

		FVulkanDevice* Device;
		const FRHIQueueId Id;
		std::unique_ptr<FVulkanCompletionTracker> CompletionTracker;
		vk::Semaphore TimelineSemaphore;
	};
}
