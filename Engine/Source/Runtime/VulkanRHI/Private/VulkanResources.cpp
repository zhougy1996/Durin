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
		return new FVulkanVertexDeclaration(Elements);
	}
} // namespace Doge::VulkanRHI
