#include "VulkanCreationTiming.h"
#include "VulkanResources.h"

#include "VulkanDynamicRHI.h"

namespace Durin::VulkanRHI
{
	FVulkanVertexDeclaration::FVulkanVertexDeclaration(const FVertexDeclarationElementList& InElements)
		: Elements(InElements)
	{
	}

	auto FVulkanDynamicRHI::RHICreateVertexDeclaration(const FVertexDeclarationElementList& Elements) -> TRefCountPtr<FRHIVertexDeclaration>
	{
#if DURIN_VULKAN_TEST_SUPPORT
		FVulkanCreationTimingScope TimingScope(EVulkanCreationKind::VertexDeclaration);
#endif
		TRefCountPtr<FRHIVertexDeclaration> Result = new FVulkanVertexDeclaration(Elements);
#if DURIN_VULKAN_TEST_SUPPORT
		if (auto* Timing = TimingScope.Get()) Timing->bSucceeded = !!Result;
#endif
		return Result;
	}
} // namespace Durin::VulkanRHI
