#pragma once

#include "CoreMinimal.h"

#include "RHI.h"

#include "VulkanPlatform.h"

#include "Experimental/RHITransition.h"
#include "RHICompletion.h"
#include "VulkanCommon.h"

namespace Durin::VulkanRHI
{
	class FVulkanDevice;

	class FVulkanTransition final : public FRHITransition
	{
	public:
		FVulkanTransition(FVulkanDevice& InDevice, FRHITransitionDesc Desc);
		~FVulkanTransition() override;
		auto GetOrCreateEvent() -> vk::Event;
		auto GetEvent() const -> vk::Event { return Event; }
		auto BeginOn(FRHIQueueId Queue) -> bool;
		auto EndOn(FRHIQueueId Queue) -> bool;
	private:
		FVulkanDevice& Device;
		vk::Event Event;
		FRHIQueueId Queue{};
		bool bBegun = false;
		bool bEnded = false;
	};
}
