#pragma once

#include "CoreMinimal.h"
#include "Backend/RHIDynamicBufferPagePool.h"

namespace Durin::VulkanRHI
{
	class FVulkanDevice;
	class FVulkanDynamicBufferPool final
	{
	public:
		explicit FVulkanDynamicBufferPool(FVulkanDevice& InDevice);
		auto EnsureInitialPage() -> void;
		auto ProcessGrowthRequest() -> void;
		auto TryAcquire(uint32 Size, FRHIDynamicBufferWritePoolSnapshot* Snapshot)
			-> std::shared_ptr<FRHIDynamicBufferWriteRange>;
	private:
		static constexpr uint32 PageGranularity = 64 * 1024;
		static constexpr uint32 MaxPages = 4;
		static constexpr uint32 MaxBytes = 4 * 1024 * 1024;
		auto EnsurePage(uint32 Size) -> void;
		FVulkanDevice& Device;
		const uint32 WriteAlignment;
		FRHIDynamicBufferPagePool Pool;
		std::mutex GrowthMutex;
		std::atomic<uint32> RequestedGrowth = 0;
		uint32 NextPageSize = PageGranularity;
	};
}
