#include "VulkanResources.h"

#include "RHICommandList.h"
#include "VulkanDynamicRHI.h"
#include "VulkanRHIPrivate.h"

namespace Durin::VulkanRHI
{
	FVulkanVertexDeclaration::FVulkanVertexDeclaration(const FVertexDeclarationElementList& InElements)
		: Elements(InElements)
	{
#if DURIN_VULKAN_TEST_FAILURE_INJECTION
		ThrowIfVulkanNativeCreateFailureIsArmed(
			EVulkanCreateFailurePoint::VertexDeclaration);
#endif
	}

	auto FVulkanDynamicRHI::RHICreateVertexDeclaration(const FVertexDeclarationElementList& Elements) -> TRefCountPtr<FRHIVertexDeclaration>
	{
		TRefCountPtr<FRHIVertexDeclaration> Result;
		const FRHIFallibleOperationResult CreationResult =
			ExecuteFallibleVulkanCreationOperation(
				[Elements, &Result]() {
					Result = new FVulkanVertexDeclaration(Elements);
				}, Elements.size() * sizeof(FVertexElement));
		if (!CreationResult.IsSuccess())
		{
			DURIN_ERROR("Failed to create Vulkan RHI vertex declaration: {}",
				CreationResult.Diagnostic);
			return nullptr;
		}
		return Result;
	}
} // namespace Durin::VulkanRHI
