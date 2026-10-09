#pragma once

#include "CoreMinimal.h"
#include "RHIResources.h"
#include "VulkanBuffer.h"

namespace Durin::VulkanRHI
{
	// One immutable shader allocation, shared by the resource and recorded submissions.
	struct FVulkanUniformAllocation
	{
		TRefCountPtr<FVulkanBuffer> Buffer;
		std::shared_ptr<void> Lease;
		uint64 Offset = 0;
		uint64 Size = 0;
		std::vector<TRefCountPtr<FRHIResource>> References;
		~FVulkanUniformAllocation();
	};

	// Owns the current physical allocation; updates replace it on ordered RHI replay.
	class FVulkanUniformBuffer final : public FRHIUniformBuffer
	{
	public:
		FVulkanUniformBuffer(FVulkanDevice& InDevice, const FRHIUniformBufferLayout& Layout,
			ERHIBufferLifetimeUsage Usage, FByteView Data, std::span<FRHIResource* const> References)
			: FRHIUniformBuffer(Layout, Usage, Data, References), Device(InDevice) {}
		auto UpdateContents(FByteView Data, std::span<FRHIResource* const> References) -> void override;
		auto GetAllocation() -> const std::shared_ptr<FVulkanUniformAllocation>&;
	private:
		FVulkanDevice& Device;
		std::shared_ptr<FVulkanUniformAllocation> Allocation;
	};

}
