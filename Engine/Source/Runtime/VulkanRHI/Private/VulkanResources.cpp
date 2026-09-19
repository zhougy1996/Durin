#include "VulkanCreation.h"
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
		auto Result = CreateVulkanResource([&]() -> TRefCountPtr<FRHIVertexDeclaration> {
			return new FVulkanVertexDeclaration(Elements);
		}, "vertex declaration");
#if DURIN_VULKAN_TEST_FAILURE_INJECTION
		if (auto* Timing = TimingScope.Get()) Timing->bSucceeded = !!Result;
#endif
		return Result;
	}
} // namespace Durin::VulkanRHI
