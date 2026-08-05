#include "VulkanSubmission.h"

#include "VulkanCommandBuffer.h"
#include "VulkanDevice.h"
#include "VulkanRHIPrivate.h"

namespace Durin::VulkanRHI
{

	FVulkanFrame::FVulkanFrame(FVulkanDevice& Device)
	: Device(Device)
	{
		CheckVulkanRHIThread();
		FrameFence = Device.GetFenceManager().AllocateFence(true);
	}

	FVulkanFrame::~FVulkanFrame()
	{
		CheckVulkanRHIThread();
		Device.GetFenceManager().ReleaseFence(FrameFence);
	}

	auto FVulkanFrame::TrackInFlightPayload(std::vector<FVulkanPayload*>& Payload) -> void
	{
		CheckVulkanRHIThread();
		InFlightPayloads.insert(InFlightPayloads.end(), Payload.begin(), Payload.end());
	}

	auto FVulkanFrame::Prepare() -> void
	{
		CheckVulkanRHIThread();
		// Refresh the cached fence state before deciding whether it is safe to reset.
		if (!Device.GetFenceManager().IsFenceSignaled(FrameFence))
		{
			Device.GetFenceManager().WaitForFence(FrameFence, UINT64_MAX);
		}
		Reset();
	}

	auto FVulkanFrame::Reset() -> void
	{
		CheckVulkanRHIThread();
		Device.GetFenceManager().ResetFence(FrameFence);
		ReleaseInFlightPayloadsAfterDeviceIdle();
	}

	auto FVulkanFrame::ReleaseInFlightPayloadsAfterDeviceIdle() -> void
	{
		CheckVulkanRHIThread();
		for (FVulkanPayload* Payload : InFlightPayloads)
		{
			for (FVulkanCommandBuffer* CommandBuffer : Payload->CommandBuffers)
			{
				CommandBuffer->Reset();
			}

			delete Payload;
		}
		InFlightPayloads.clear();
	}
}
