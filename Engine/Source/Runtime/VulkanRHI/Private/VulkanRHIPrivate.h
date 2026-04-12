#pragma once

#include "RHI.h"
#include "VulkanDynamicRHI.h"
#include "VulkanDevice.h"

namespace Doge::VulkanRHI
{
	class FVulkanDevice;
	class FVulkanFence;
	class FVulkanPayload;

	class FVulkanFrame
	{
	public:
		explicit FVulkanFrame(FVulkanDevice& device);
		~FVulkanFrame();

		auto TrackInFlightPayload(std::vector<FVulkanPayload*>& Payload) -> void;

		auto Prepare() -> void;

		auto GetFrameFence() const -> FVulkanFence* { return FrameFence; }

	private:
		auto Reset() -> void;

		FVulkanDevice& Device;
		FVulkanFence* FrameFence = nullptr;
		std::vector<FVulkanPayload*> InFlightPayloads;
	};

	auto ConvertToVulkanFormat(EPixelFormat InFormat) -> vk::Format;

	auto ConvertToVulkanBufferUsageFlags(EBufferUsageFlags InUsage) -> vk::BufferUsageFlags;

	extern std::atomic<uint64> GVulkanBufferHandleIdCounter;
	extern std::atomic<uint64> GVulkanBufferViewHandleIdCounter;
	extern std::atomic<uint64> GVulkanImageViewHandleIdCounter;
	extern std::atomic<uint64> GVulkanSamplerHandleIdCounter;
	extern std::atomic<uint64> GVulkanDSetLayoutHandleIdCounter;

} // namespace Doge::VulkanRHI