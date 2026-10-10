#pragma once

#include "CoreMinimal.h"
#include "RHIShaderParameters.h"
#include "VulkanUniformBuffer.h"

namespace Durin::VulkanRHI
{
	class FVulkanDevice;
	class FVulkanCommandListContext;

	// Captures immutable Uniform allocations at parameter binding; updates require rebinding.
	class FVulkanUniformBufferBindings
	{
	public:
		auto Update(std::span<const FRHIShaderParameterResource> Parameters) -> void;
		auto Resolve(FVulkanDevice& Device)
			-> std::vector<FRHIShaderParameterResource>;
		auto PrepareForUse(FVulkanCommandListContext& Context, ERHIPipeline Pipeline) -> void;
		auto Clear() -> void { Bindings.clear(); }
	private:
		struct FResolved
		{
			TRefCountPtr<FRHIBufferView> View;
			std::shared_ptr<const FVulkanUniformAllocation> Allocation;
			uint64 PhysicalOffset = 0;
			bool bDynamic = false;
		};
		struct FBinding
		{
			FRHIShaderParameterResource Parameter;
			TRefCountPtr<FRHIUniformBuffer> Uniform;
			std::shared_ptr<FResolved> Resolved;
			bool bDirty = true;
		};
		std::vector<FBinding> Bindings;
	};
}
