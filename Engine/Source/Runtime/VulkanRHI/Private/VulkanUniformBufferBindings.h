#pragma once

#include "CoreMinimal.h"
#include "RHIShaderParameters.h"
#include "VulkanUniformBuffer.h"

namespace Durin::VulkanRHI
{
	class FVulkanCommandListContext;

	// Captures immutable Uniform allocations at parameter binding; updates require rebinding.
	class FVulkanUniformBufferBindings
	{
	public:
		auto Update(std::span<const FRHIShaderParameterResource> Parameters) -> void;
		auto Resolve()
			-> std::vector<FRHIShaderParameterResource>;
		auto PrepareForUse(FVulkanCommandListContext& Context, ERHIPipeline Pipeline) -> void;
		auto Clear() -> void { Bindings.clear(); }
	private:
		struct FBinding
		{
			FRHIShaderParameterResource Parameter;
			TRefCountPtr<FRHIUniformBuffer> Uniform;
			std::shared_ptr<FVulkanUniformAllocation> Allocation;
			bool bDirty = true;
		};
		std::vector<FBinding> Bindings;
	};
}
