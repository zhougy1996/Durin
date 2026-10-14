#include "VulkanTransition.h"
#include "VulkanBuffer.h"
#include "VulkanDevice.h"

namespace Durin::VulkanRHI
{
	auto FVulkanTransition::CaptureBufferVersions() -> void
	{
		if (std::ranges::none_of(GetBufferTransitions(), [](const auto& Entry) {
			return static_cast<bool>(FVulkanBuffer::Cast(Entry.Buffer)->GetWriteRange());
		})) return;
		BufferVersions.assign(GetBufferTransitions().begin(), GetBufferTransitions().end());
		for (auto& Entry : BufferVersions)
		{
			auto* Buffer = FVulkanBuffer::Cast(Entry.Buffer);
			if (const auto& Range = Buffer->GetWriteRange())
			{
				WriteRanges.push_back(Range);
				Entry.Buffer = Buffer->GetBackingBuffer();
			}
		}
	}

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
