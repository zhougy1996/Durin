#include "VulkanSwapChain.h"

#include "VulkanDevice.h"
#include "VulkanGenericPlatform.h"
#include "VulkanQueue.h"

auto ChooseSwapSurfaceFormat(const std::vector<vk::SurfaceFormatKHR>& AvailableFormats) -> vk::SurfaceFormatKHR
{
	if (AvailableFormats.size() == 1 && AvailableFormats[0].format == vk::Format::eUndefined)
	{
		return {vk::Format::eR8G8B8A8Srgb, vk::ColorSpaceKHR::eSrgbNonlinear};
	}
	for (const vk::SurfaceFormatKHR& Format : AvailableFormats)
	{
		if (Format.format == vk::Format::eR8G8B8A8Srgb && Format.colorSpace == vk::ColorSpaceKHR::eSrgbNonlinear)
		{
			return Format;
		}
	}
	return AvailableFormats[0];
}

auto ChooseSwapPresentMode(const std::vector<vk::PresentModeKHR>& AvailablePresentModes) -> vk::PresentModeKHR
{
	return vk::PresentModeKHR::eFifo;
}

auto ChooseSwapExtent(const vk::SurfaceCapabilitiesKHR& Capabilities, uint32 Width, uint32 Height) -> vk::Extent2D
{
	if (Capabilities.currentExtent.width != UINT32_MAX)
	{
		return Capabilities.currentExtent;
	}
	else
	{
		vk::Extent2D ActualExtent = {Width, Height};
		ActualExtent.width = std::max(Capabilities.minImageExtent.width, std::min(Capabilities.maxImageExtent.width, ActualExtent.width));
		ActualExtent.height = std::max(Capabilities.minImageExtent.height, std::min(Capabilities.maxImageExtent.height, ActualExtent.height));
		return ActualExtent;
	}
}

FVulkanSwapChain::FVulkanSwapChain(vk::Instance Instance, FVulkanDevice& Device, void* GlfwWindowHandle, uint32 Width, uint32 Height, bool bIsFullScreen)
	: Device_(Device)
{
	Surface_ = FVulkanGenericPlatform::CreateSurface(GlfwWindowHandle, Instance);

	// Get Swap chain support details
	vk::PhysicalDevice Gpu = Device_.GetGpu();
	vk::SurfaceCapabilitiesKHR Capabilities = Gpu.getSurfaceCapabilitiesKHR(Surface_);
	std::vector<vk::SurfaceFormatKHR> Formats = Gpu.getSurfaceFormatsKHR(Surface_);
	std::vector<vk::PresentModeKHR> PresentModes = Gpu.getSurfacePresentModesKHR(Surface_);

	vk::SurfaceFormatKHR CurrFormat = ChooseSwapSurfaceFormat(Formats);
	vk::PresentModeKHR PresentMode = ChooseSwapPresentMode(PresentModes);
	vk::Extent2D Extent = ChooseSwapExtent(Capabilities, Width, Height);

	uint32 MinImageCount = Capabilities.minImageCount + 1;
	if (Capabilities.maxImageCount > 0 && MinImageCount > Capabilities.maxImageCount)
	{
		MinImageCount = Capabilities.maxImageCount;
	}

	vk::SwapchainCreateInfoKHR SwapChainInfo;
	SwapChainInfo
		.setSurface(Surface_)
		.setMinImageCount(MinImageCount)
		.setImageFormat(CurrFormat.format)
		.setImageColorSpace(CurrFormat.colorSpace)
		.setImageExtent(Extent)
		.setImageArrayLayers(1)
		.setImageUsage(vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst)
		.setPreTransform(Capabilities.currentTransform)
		.setImageSharingMode(vk::SharingMode::eExclusive)
		.setPresentMode(PresentMode)
		.setClipped(vk::True)
		.setCompositeAlpha(vk::CompositeAlphaFlagBitsKHR::eOpaque)
		.setOldSwapchain(VK_NULL_HANDLE);

	try
	{
		SwapChain_ = Device_.GetHandle().createSwapchainKHR(SwapChainInfo);
		DOGE_DEBUG("Vulkan swap chain created");
	}
	catch (const std::runtime_error& err)
	{
		DOGE_ERROR("Failed to create vulkan swap chain: {}", err.what());
	}

	Device_.SetupPresentQueue(Surface_);
	SwapChainImages_ = Device_.GetHandle().getSwapchainImagesKHR(SwapChain_);

	// TODO: use managed semaphore
	ImageAcquiredSemaphore_ = new FVulkanSemaphore(Device_);
}

FVulkanSwapChain::~FVulkanSwapChain()
{
	delete ImageAcquiredSemaphore_;
	Device_.GetHandle().destroySwapchainKHR(SwapChain_);
}

auto FVulkanSwapChain::GetImages() const -> const std::vector<vk::Image>&
{
	return SwapChainImages_;
}

auto FVulkanSwapChain::AcquireImageIndex(FVulkanSemaphore** OutImageAcquiredSemaphore) -> uint32
{
	// TODO: Semaphore
	vk::ResultValue<uint32> Result = Device_.GetHandle().acquireNextImageKHR(SwapChain_, UINT64_MAX, ImageAcquiredSemaphore_->GetHandle());
	if (Result.result != vk::Result::eSuccess)
	{
		CurrentImageIndex_ = -1;
		DOGE_ERROR("Failed to acquire swap chain image: {}", vk::to_string(Result.result));
	}
	CurrentImageIndex_ = Result.value;
	*OutImageAcquiredSemaphore = ImageAcquiredSemaphore_;

	return CurrentImageIndex_;
}

auto FVulkanSwapChain::Present(FVulkanQueue* PresentQueue, FVulkanSemaphore* BackBufferRenderingDoneSemaphore) -> void
{
	vk::PresentInfoKHR PresentInfo;
	auto Semaphore = BackBufferRenderingDoneSemaphore->GetHandle();

	uint32 ImageIndex = static_cast<uint32>(CurrentImageIndex_);
	PresentInfo
		.setWaitSemaphores(Semaphore)
		.setSwapchains(SwapChain_)
		.setImageIndices(ImageIndex);

	vk::Result Result = PresentQueue->GetHandle().presentKHR(PresentInfo);
	check(Result == vk::Result::eSuccess);

	CurrentImageIndex_ = -1;
}
