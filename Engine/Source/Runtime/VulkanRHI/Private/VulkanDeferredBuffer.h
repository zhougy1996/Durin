#pragma once

#include "RHIShaderParameters.h"
#include "Backend/RHIDeferredBufferBackend.h"

namespace Durin::VulkanRHI
{
	class FVulkanDevice;
	class FVulkanCommandListContext;

	// Keeps logical bindings separate from the physical descriptor snapshot.
	class FVulkanDeferredBufferBindings
	{
	public:
		auto Update(std::span<const FRHIShaderParameterResource> Parameters) -> void;
		auto Resolve(FVulkanDevice& Device, FVulkanCommandListContext& Context, ERHIPipeline Pipeline)
			-> std::vector<FRHIShaderParameterResource>;
		auto Clear() -> void { Bindings.clear(); }
	private:
		struct FResolved
		{
			std::shared_ptr<const FRHIDeferredBufferSnapshot> Snapshot;
			TRefCountPtr<FRHIBufferView> View;
		};
		struct FBinding
		{
			FRHIShaderParameterResource Parameter;
			TRefCountPtr<FRHIBufferView> Logical;
			std::shared_ptr<FResolved> Resolved;
		};
		std::vector<FBinding> Bindings;
	};
}
