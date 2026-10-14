#pragma once

#include "CoreMinimal.h"
#include "Backend/RHIDynamicBufferPagePool.h"

namespace Durin::VulkanRHI
{
	class FVulkanDevice;
	class FVulkanDynamicBufferPool final
	{
	public:
		explicit FVulkanDynamicBufferPool(FVulkanDevice& InDevice) : Device(InDevice), Pool(4, 4ull * 1024 * 1024) {}
		auto EnsureInitialPage() -> void;
		auto ProcessGrowthRequest() -> void;
		auto TryAcquire(uint32 Size, uint32 Alignment, FRHIDynamicBufferWritePoolSnapshot* Snapshot)
			-> std::shared_ptr<FRHIDynamicBufferWriteRange>;
	private:
		auto EnsurePage(uint32 Size) -> void;
		FVulkanDevice& Device;
		FRHIDynamicBufferPagePool Pool;
		std::mutex GrowthMutex;
		std::atomic<uint32> RequestedGrowth = 0;
		uint32 NextPageSize = 64 * 1024;
	};
}
