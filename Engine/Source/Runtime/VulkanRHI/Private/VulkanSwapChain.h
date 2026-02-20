#pragma once

class FVulkanDevice;
class FVulkanFence;
class FVulkanQueue;
class FVulkanSemaphore;

class FVulkanSwapChain
{
public:
	FVulkanSwapChain(vk::Instance Instance, FVulkanDevice& Device, void* GlfwWindowHandle, uint32 Width, uint32 Height, bool bIsFullScreen);

	~FVulkanSwapChain();

	auto GetImages() const -> const std::vector<vk::Image>&;

	// Returns the index of the acquired image
	auto AcquireImageIndex(FVulkanSemaphore** OutImageAcquiredSemaphore) -> uint32;

	auto Present(FVulkanQueue* PresentQueue, FVulkanSemaphore* BackBufferRenderingDoneSemaphore) -> void;

	auto GetImageFormat() const -> vk::Format { return ImageFormat; }

private:
	FVulkanDevice& Device_;

	vk::SwapchainKHR SwapChain_;

	// Format of the swap chain images, which is determined by the surface format selected during swap chain creation.
	vk::Format ImageFormat;

	std::vector<vk::Image> SwapChainImages_;

	vk::SurfaceKHR Surface_;

	int32 CurrentImageIndex_{};

	FVulkanSemaphore* ImageAcquiredSemaphore_;

	FVulkanFence* ImageAcquiredFence_{};
};
