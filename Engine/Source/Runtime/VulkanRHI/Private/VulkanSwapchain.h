#pragma once

namespace Durin::VulkanRHI
{
	class FVulkanDevice;
	class FVulkanFence;
	class FVulkanQueue;
	class FVulkanSemaphore;

	class FVulkanSwapchain
	{
	public:
		FVulkanSwapchain(vk::Instance InInstance, FVulkanDevice& InDevice, void* InWindowHandle, uint32 Width, uint32 Height, bool bIsFullScreen);

		~FVulkanSwapchain();

		auto GetImages() const -> const std::vector<vk::Image>&;

		// Returns the index of the acquired image, or INDEX_NONE_U32 if the swapchain must be recreated.
		auto AcquireImageIndex(FVulkanSemaphore** OutImageAcquiredSemaphore) -> uint32;

		auto Present(FVulkanQueue* PresentQueue, FVulkanSemaphore* BackBufferRenderingDoneSemaphore) -> bool;

		// Return the actual format of the swap chain images, which is determined by the surface format selected during swap chain creation.
		// This may be different from the preferred pixel format specified when creating the viewport.
		auto GetFormat() const -> vk::Format { return ImageFormat; }

		// TODO: recreate
		auto Destroy() -> void;

	private:
		vk::Instance Instance;

		FVulkanDevice& Device;

		vk::SwapchainKHR Swapchain;

		// Format of the swap chain images, which is determined by the surface format selected during swap chain creation.
		vk::Format ImageFormat;

		std::vector<vk::Image> SwapchainImages;

		vk::SurfaceKHR Surface;

		int32 CurrentImageIndex = -1;

		uint32 NextSemaphoreIndex{};

		std::vector<FVulkanSemaphore*> ImageAcquiredSemaphores;
	};
}
