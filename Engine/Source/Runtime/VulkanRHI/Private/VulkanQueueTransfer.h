#pragma once

#include "CoreMinimal.h"
#include "VulkanRHIAPI.h"

#include "RHI.h"

#include "VulkanPlatform.h"

#include "RHIQueueTransfer.h"

namespace Durin::VulkanRHI
{
	class FVulkanDevice;
	class FVulkanQueue;

	// An owning, single-use pair. The acquire reuses the release's exact native
	// ranges/layouts and consumes its queue sync point; it never reconstructs a pair.
	class FVulkanQueueTransfer final : public FRHIQueueTransfer
	{
	public:
		VULKANRHI_API FVulkanQueueTransfer(FVulkanDevice& Device, FRHIQueueId Source, FRHIQueueId Destination,
			std::span<const FRHIBufferTransition> Buffers, std::span<const FRHITextureTransition> Textures,
			bool bUseSynchronization2 = true);
		VULKANRHI_API ~FVulkanQueueTransfer() override;
		auto RecordRelease(FVulkanQueue& Queue, vk::CommandBuffer Commands,
			const FRHIGPUSyncPointRef& SyncPoint) -> void;
		auto RecordAcquire(FVulkanQueue& Queue, vk::CommandBuffer Commands,
			const FRHIGPUSyncPointRef& SyncPoint) -> void;
		auto GetReleaseSyncPoint() const -> FRHIGPUSyncPointRef;
	private:
		struct FState;
		std::unique_ptr<FState> State;
	};
}
