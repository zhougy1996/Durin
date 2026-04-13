#include "VulkanResources.h"

#include "VulkanDynamicRHI.h"

namespace Doge::VulkanRHI
{
	FVulkanVertexDeclaration::FVulkanVertexDeclaration(const FVertexDeclarationElementList& InElements)
		: Elements(InElements)
	{
	}

	auto FVulkanDynamicRHI::RHICreateVertexDeclaration(const FVertexDeclarationElementList& Elements) -> TRefCountPtr<FRHIVertexDeclaration>
	{
		return new FVulkanVertexDeclaration(Elements);
	}
} // namespace Doge::VulkanRHI
