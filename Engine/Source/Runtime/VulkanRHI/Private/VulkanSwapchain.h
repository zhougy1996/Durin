#pragma once

#include "VulkanRHIAPI.h"

namespace Durin::VulkanRHI
{
	class FVulkanDevice;
	class FVulkanFence;
	class FVulkanQueue;
	class FVulkanSemaphore;

	inline constexpr vk::ImageUsageFlags RequiredSwapchainImageUsage =
		vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eSampled
		| vk::ImageUsageFlagBits::eTransferDst;
	inline constexpr uint64 BestEffortAcquireTimeoutNanoseconds = 1'000'000;

	constexpr auto GetSwapchainAcquireTimeout(
		EViewportPresentationPolicy Policy) -> uint64
	{
		return Policy == EViewportPresentationPolicy::BestEffort
			? BestEffortAcquireTimeoutNanoseconds : UINT64_MAX;
	}

	struct FVulkanSwapchainSelectionInput
	{
		vk::SurfaceCapabilitiesKHR Capabilities{};
		std::vector<vk::SurfaceFormatKHR> Formats;
		std::vector<vk::PresentModeKHR> PresentModes;
		uint32 RequestedWidth = 0;
		uint32 RequestedHeight = 0;
		EViewportPresentationPolicy PresentationPolicy = EViewportPresentationPolicy::FramePaced;
	};

	struct FVulkanSwapchainConfiguration
	{
		vk::SurfaceFormatKHR SurfaceFormat{};
		vk::PresentModeKHR PresentMode = vk::PresentModeKHR::eFifo;
		vk::Extent2D Extent{};
		uint32 ImageCount = 0;
		vk::ImageUsageFlags ImageUsage{};
		vk::SurfaceTransformFlagBitsKHR PreTransform = vk::SurfaceTransformFlagBitsKHR::eIdentity;
		vk::CompositeAlphaFlagBitsKHR CompositeAlpha = vk::CompositeAlphaFlagBitsKHR::eOpaque;
	};

	VULKANRHI_API auto SelectVulkanSwapchainConfiguration(
		const FVulkanSwapchainSelectionInput& Input,
		FVulkanSwapchainConfiguration& OutConfiguration,
		std::string& OutError) -> bool;

	// Reports whether presentation succeeded and whether the swapchain must be recreated.
	struct FVulkanPresentOutcome
	{
		bool bPresented = false;
		bool bQueueOperationsEnqueued = false;
	};

	// Owns the window swapchain, its images, and acquire/present synchronization.
	class FVulkanSwapchain
	{
	public:
		FVulkanSwapchain(FVulkanDevice& InDevice, vk::SurfaceKHR InSurface, uint32 Width, uint32 Height, bool bIsFullScreen, EViewportPresentationPolicy InPresentationPolicy, vk::SwapchainKHR InOldSwapchain, bool& bOutNativeSwapchainCreated);

		~FVulkanSwapchain();

		auto GetImages() const -> const std::vector<vk::Image>&;

		// Returns the index of the acquired image, or INDEX_NONE_U32 if the swapchain must be recreated.
		auto AcquireImageIndex(FVulkanSemaphore** OutImageAcquiredSemaphore) -> uint32;

		auto Present(FVulkanQueue* PresentQueue, FVulkanSemaphore* BackBufferRenderingDoneSemaphore, vk::Fence PresentFence = VK_NULL_HANDLE) -> FVulkanPresentOutcome;

		auto NeedsRecreate() const -> bool { return bNeedsRecreate; }

		// Return the actual format of the swap chain images, which is determined by the surface format selected during swap chain creation.
		// This may be different from the preferred pixel format specified when creating the viewport.
		auto GetFormat() const -> vk::Format { return ImageFormat; }

		auto GetExtent() const -> vk::Extent2D { return Extent; }

		auto Destroy() -> void;

		auto GetHandle() const -> vk::SwapchainKHR { return Swapchain; }

		// Completes the acquire-side synchronization owned by a local viewport
		// candidate. The swapchain must not be published before this succeeds.
		auto InitializeSynchronizationResources() -> void;

	private:
		auto MarkNeedsRecreate(std::string_view Operation, vk::Result Result) -> void;

		FVulkanDevice& Device;

		vk::SwapchainKHR Swapchain;

		// Format of the swap chain images, which is determined by the surface format selected during swap chain creation.
		vk::Format ImageFormat;

		vk::Extent2D Extent{};

		std::vector<vk::Image> SwapchainImages;

		vk::SurfaceKHR Surface;

		EViewportPresentationPolicy PresentationPolicy = EViewportPresentationPolicy::FramePaced;

		int32 CurrentImageIndex = -1;

		uint32 NextSemaphoreIndex{};

		bool bNeedsRecreate = false;

		bool bAcquireTimeoutReported = false;

		std::vector<FVulkanSemaphore*> ImageAcquiredSemaphores;
	};
}
