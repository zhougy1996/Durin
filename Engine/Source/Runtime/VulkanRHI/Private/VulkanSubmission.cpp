#include "VulkanSubmission.h"

#include "VulkanCommandBuffer.h"
#include "VulkanCompletion.h"
#include "VulkanDevice.h"
#include "VulkanRHIPrivate.h"

namespace Durin::VulkanRHI
{

	FVulkanFrame::FVulkanFrame(FVulkanDevice& InDevice)
		: Device(InDevice)
	{
		CheckVulkanRHIThread();
	}

	auto FVulkanFrame::Prepare() -> void
	{
		CheckVulkanRHIThread();
		Device.GetCompletionTracker().WaitForToken(LastSubmittedToken);
	}

	auto FVulkanFrame::SetLastSubmittedToken(uint64 Token) -> void
	{
		CheckVulkanRHIThread();
		LastSubmittedToken = Token;
	}
}
