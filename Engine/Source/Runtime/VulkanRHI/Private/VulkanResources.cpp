#include "VulkanCreationTiming.h"
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
#if DURIN_VULKAN_TEST_FAILURE_INJECTION
		FVulkanCreationTimingScope TimingScope(EVulkanCreationKind::VertexDeclaration);
#endif
		TRefCountPtr<FRHIVertexDeclaration> Result;
		const auto CreationResult = ExecuteFallibleRHICreationOperation(
			MakeVulkanCreationOperation([Elements, &Result]() {
				Result = new FVulkanVertexDeclaration(Elements);
			}));
		if (!CreationResult.IsSuccess())
		{
			DURIN_ERROR("Failed to create Vulkan RHI vertex declaration: {}",
				CreationResult.Diagnostic);
			return nullptr;
		}
#if DURIN_VULKAN_TEST_FAILURE_INJECTION
		if (auto* Timing = TimingScope.Get()) Timing->bSucceeded = !!Result;
#endif
		return Result;
	}
} // namespace Durin::VulkanRHI
