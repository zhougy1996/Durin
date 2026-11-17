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
		// The sole creation boundary: invalid queue/range descriptions return null.
		VULKANRHI_API static auto Create(FVulkanDevice& Device, const FRHIQueueTransferDesc& Desc,
			bool bUseSynchronization2 = true) -> std::shared_ptr<FVulkanQueueTransfer>;
		VULKANRHI_API ~FVulkanQueueTransfer() override;

		auto RecordRelease(FVulkanQueue& Queue, vk::CommandBuffer Commands,
			const FRHIGPUSyncPointRef& SyncPoint) -> void;
		auto RecordAcquire(FVulkanQueue& Queue, vk::CommandBuffer Commands,
			const FRHIGPUSyncPointRef& SyncPoint) -> void;
		auto GetReleaseSyncPoint() const -> FRHIGPUSyncPointRef;
	private:
		FVulkanQueueTransfer(FVulkanDevice& Device, const FRHIQueueTransferDesc& Desc,
			bool bUseSynchronization2);
		struct FState;
		std::unique_ptr<FState> State;
	};
}
