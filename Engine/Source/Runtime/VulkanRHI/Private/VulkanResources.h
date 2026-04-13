#pragma once

#include "RHIResources.h"

namespace Doge::VulkanRHI
{
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