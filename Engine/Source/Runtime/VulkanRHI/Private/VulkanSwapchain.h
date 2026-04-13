#pragma once

namespace Doge::VulkanRHI
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

		// Returns the index of the acquired image
		auto AcquireImageIndex(FVulkanSemaphore** OutImageAcquiredSemaphore) -> uint32;

		auto Present(FVulkanQueue* PresentQueue, FVulkanSemaphore* BackBufferRenderingDoneSemaphore) -> void;

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

		uint32 CurrentImageIndex{};

		uint32 NextSemaphoreIndex{};

		std::vector<FVulkanSemaphore*> ImageAcquiredSemaphores;
	};
}
