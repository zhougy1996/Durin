#pragma once

#include "RHIQueueTransfer.h"

namespace Durin::VulkanRHI
{
	class FVulkanDevice;
	class FVulkanQueue;

	// An owning, single-use pair. The acquire reuses the release's exact native
	// ranges/layouts and consumes its queue ticket; it never reconstructs a pair.
	class FVulkanQueueTransfer final : public FRHIQueueTransfer
	{
	public:
		FVulkanQueueTransfer(FVulkanDevice& Device, FRHIQueueId Source, FRHIQueueId Destination,
			std::span<const FRHIBufferTransition> Buffers, std::span<const FRHITextureTransition> Textures,
			bool bUseSynchronization2 = true);
		~FVulkanQueueTransfer() override;
		auto RecordRelease(FVulkanQueue& Queue, vk::CommandBuffer Commands,
			const FRHIGPUSubmissionTicket& Ticket) -> void;
		auto RecordAcquire(FVulkanQueue& Queue, vk::CommandBuffer Commands) -> void;
		auto GetReleaseTicket() const -> FRHIGPUSubmissionTicket;
	private:
		struct FState;
		std::unique_ptr<FState> State;
	};
}
