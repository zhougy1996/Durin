#include "VulkanSubmission.h"

#include "VulkanCommandBuffer.h"
#include "VulkanDevice.h"

namespace Durin::VulkanRHI
{

	FVulkanFrame::FVulkanFrame(FVulkanDevice& Device)
	: Device(Device)
	{
		FrameFence = Device.GetFenceManager().AllocateFence(true);
	}

	FVulkanFrame::~FVulkanFrame()
	{
		Device.GetFenceManager().ReleaseFence(FrameFence);
	}

	auto FVulkanFrame::TrackInFlightPayload(std::vector<FVulkanPayload*>& Payload) -> void
	{
		InFlightPayloads.insert(InFlightPayloads.end(), Payload.begin(), Payload.end());
	}

	auto FVulkanFrame::Prepare() -> void
	{
		// Refresh the cached fence state before deciding whether it is safe to reset.
		if (!Device.GetFenceManager().IsFenceSignaled(FrameFence))
		{
			Device.GetFenceManager().WaitForFence(FrameFence, UINT64_MAX);
		}
		Reset();
	}

	auto FVulkanFrame::Reset() -> void
	{
		Device.GetFenceManager().ResetFence(FrameFence);
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
