#pragma once

#include "RHIResources.h"

namespace Durin::VulkanRHI
{
	// Retains the vertex element mapping consumed during Vulkan pipeline creation.
	class FVulkanVertexDeclaration : public FRHIVertexDeclaration
	{
	public:
		FVulkanVertexDeclaration(const FVertexDeclarationElementList& InElements);

		// FVulkanVertexDeclaration(const FVertexDeclarationElementList& InElements, uint32 InHash, uint32 InHashNoStrides);

		auto GetElements() const -> const FVertexDeclarationElementList& override { return Elements; }

	private:
		FVertexDeclarationElementList Elements;
	};

}
