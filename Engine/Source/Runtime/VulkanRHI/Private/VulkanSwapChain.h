#pragma once

class FVulkanDevice;
class FVulkanFence;
class FVulkanQueue;
class FVulkanSemaphore;

class FVulkanSwapChain
{
public:
	FVulkanSwapChain(vk::Instance Instance, FVulkanDevice& Device, void* WindowHandle, uint32 Width, uint32 Height, bool bIsFullScreen);

	~FVulkanSwapChain();

	auto GetImages() const -> const TArray<vk::Image>&;

	// Returns the index of the acquired image
	auto AcquireImageIndex(FVulkanSemaphore** OutImageAcquiredSemaphore) -> uint32;

	auto Present(FVulkanQueue* PresentQueue, FVulkanSemaphore* BackBufferRenderingDoneSemaphore) -> void;

private:
	FVulkanDevice& Device_;

	vk::SwapchainKHR SwapChain_;

	TArray<vk::Image> SwapChainImages_;

	vk::SurfaceKHR Surface_;

	int32 CurrentImageIndex_;

	FVulkanSemaphore* ImageAcquiredSemaphore_;

	FVulkanFence* ImageAcquiredFence_;
};
