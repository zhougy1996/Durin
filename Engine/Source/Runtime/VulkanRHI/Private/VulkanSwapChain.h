#pragma once

namespace Doge::VulkanRHI
{
	class FVulkanDevice;
	class FVulkanFence;
	class FVulkanQueue;
	class FVulkanSemaphore;

	class FVulkanSwapChain
	{
	public:
		FVulkanSwapChain(vk::Instance InInstance, FVulkanDevice& InDevice, void* InWindowHandle, uint32 Width, uint32 Height, bool bIsFullScreen);

		~FVulkanSwapChain();

		auto GetImages() const -> const std::vector<vk::Image>&;

		// Returns the index of the acquired image
		auto AcquireImageIndex(FVulkanSemaphore** OutImageAcquiredSemaphore) -> uint32;

		auto Present(FVulkanQueue* PresentQueue, FVulkanSemaphore* BackBufferRenderingDoneSemaphore) -> void;

		auto GetFormat() const -> vk::Format { return ImageFormat; }

	private:
		FVulkanDevice& Device;

		vk::SwapchainKHR SwapChain;

		// Format of the swap chain images, which is determined by the surface format selected during swap chain creation.
		vk::Format ImageFormat;

		std::vector<vk::Image> SwapChainImages;

		vk::SurfaceKHR Surface;

		int32 CurrentImageIndex{};

		FVulkanSemaphore* ImageAcquiredSemaphore;

		FVulkanFence* ImageAcquiredFence{};
	};
}
