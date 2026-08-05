#include "VulkanResources.h"

#include "RHICommandList.h"
#include "VulkanDynamicRHI.h"
#include "VulkanRHIPrivate.h"

namespace Durin::VulkanRHI
{
	FVulkanVertexDeclaration::FVulkanVertexDeclaration(const FVertexDeclarationElementList& InElements)
		: Elements(InElements)
	{
	}

	auto FVulkanDynamicRHI::RHICreateVertexDeclaration(const FVertexDeclarationElementList& Elements) -> TRefCountPtr<FRHIVertexDeclaration>
	{
		TRefCountPtr<FRHIVertexDeclaration> Result;
		if (GRHIThread && !IsInRHIThread())
		{
			GCommandListExecutor.ExecuteSynchronousOperation(false,
				[Elements, &Result]() {
					Result = new FVulkanVertexDeclaration(Elements);
				});
			return Result;
		}
		CheckVulkanRHIThread();
		return new FVulkanVertexDeclaration(Elements);
	}
} // namespace Durin::VulkanRHI
