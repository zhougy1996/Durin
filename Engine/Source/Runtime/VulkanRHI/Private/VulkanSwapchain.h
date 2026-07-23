#pragma once

namespace Durin::VulkanRHI
{
	class FVulkanDevice;
	class FVulkanFence;
	class FVulkanQueue;
	class FVulkanSemaphore;

	struct FVulkanPresentOutcome
	{
		bool bPresented = false;
		bool bQueueOperationsEnqueued = false;
	};

	class FVulkanSwapchain
	{
	public:
		FVulkanSwapchain(FVulkanDevice& InDevice, vk::SurfaceKHR InSurface, uint32 Width, uint32 Height, bool bIsFullScreen, EViewportPresentModePolicy InPresentModePolicy, vk::SwapchainKHR InOldSwapchain = VK_NULL_HANDLE);

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

		auto DetachSwapchain() -> vk::SwapchainKHR
		{
			vk::SwapchainKHR Handle = Swapchain;
			Swapchain = VK_NULL_HANDLE;
			return Handle;
		}

	private:
		auto MarkNeedsRecreate(std::string_view Operation, vk::Result Result) -> void;

		FVulkanDevice& Device;

		vk::SwapchainKHR Swapchain;

		// Format of the swap chain images, which is determined by the surface format selected during swap chain creation.
		vk::Format ImageFormat;

		vk::Extent2D Extent{};

		std::vector<vk::Image> SwapchainImages;

		vk::SurfaceKHR Surface;

		EViewportPresentModePolicy PresentModePolicy = EViewportPresentModePolicy::MainWindow;

		int32 CurrentImageIndex = -1;

		uint32 NextSemaphoreIndex{};

		bool bNeedsRecreate = false;

		std::vector<FVulkanSemaphore*> ImageAcquiredSemaphores;
	};
}
