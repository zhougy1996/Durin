#include "VulkanTransition.h"
#include "VulkanDevice.h"

namespace Durin::VulkanRHI
{
	FVulkanTransition::FVulkanTransition(FVulkanDevice& InDevice, FRHITransitionDesc Desc)
		: FRHITransition(std::move(Desc)), Device(InDevice) {}

	FVulkanTransition::~FVulkanTransition()
	{
		if (Event) Device.GetHandle().destroyEvent(Event);
	}

	auto FVulkanTransition::GetOrCreateEvent() -> vk::Event
	{
		if (!Event) Event = Device.GetHandle().createEvent(vk::EventCreateInfo());
		return Event;
	}

	auto FVulkanTransition::BeginOn(FRHIQueueId InQueue) -> bool
	{
		if (bBegun || bEnded) return false;
		Queue = InQueue;
		bBegun = true;
		return true;
	}

	auto FVulkanTransition::EndOn(FRHIQueueId InQueue) -> bool
	{
		if (!bBegun || bEnded || Queue != InQueue) return false;
		bEnded = true;
		return true;
	}
}
