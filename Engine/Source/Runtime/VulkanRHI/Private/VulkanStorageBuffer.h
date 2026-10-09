#pragma once

#include "CoreMinimal.h"
#include "Backend/RHIStorageBuffer.h"
#include "VulkanBuffer.h"

namespace Durin::VulkanRHI
{
	struct FVulkanStorageAllocation
	{
		TRefCountPtr<FVulkanBuffer> Buffer;
		std::shared_ptr<void> Lease;
		uint64 Offset = 0;
		uint64 Size = 0;
		~FVulkanStorageAllocation();
	};

	class FVulkanStorageBuffer final : public FRHIStorageBuffer
	{
	public:
		FVulkanStorageBuffer(FVulkanDevice& InDevice, const FRHIBufferDesc& Desc,
			ERHIBufferLifetimeUsage Usage, FByteView Data)
			: FRHIStorageBuffer(Desc, Usage, Data), Device(InDevice) {}
		auto UpdateContents(uint32 Offset, FByteView Data) -> void override;
		auto GetAllocation() -> const std::shared_ptr<FVulkanStorageAllocation>&;
	private:
		auto AllocateContents() -> void;
		FVulkanDevice& Device;
		std::shared_ptr<FVulkanStorageAllocation> Allocation;
	};
}
