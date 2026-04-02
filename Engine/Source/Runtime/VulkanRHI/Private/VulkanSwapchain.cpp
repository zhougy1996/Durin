#include "VulkanSwapchain.h"

#include "VulkanDevice.h"
#include "VulkanGenericPlatform.h"
#include "VulkanQueue.h"

namespace Doge::VulkanRHI
{
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

	FVulkanSwapchain::FVulkanSwapchain(vk::Instance InInstance, FVulkanDevice& Device, void* InWindowHandle, uint32 Width, uint32 Height, bool bIsFullScreen)
		: Instance(InInstance)
		, Device(Device)
	{
		Surface = FVulkanGenericPlatform::CreateSurface(InWindowHandle, InInstance);

		// Get Swap chain support details
		vk::PhysicalDevice Gpu = Device.GetGpu();
		vk::SurfaceCapabilitiesKHR Capabilities = Gpu.getSurfaceCapabilitiesKHR(Surface);
		std::vector<vk::SurfaceFormatKHR> Formats = Gpu.getSurfaceFormatsKHR(Surface);
		std::vector<vk::PresentModeKHR> PresentModes = Gpu.getSurfacePresentModesKHR(Surface);

		vk::SurfaceFormatKHR CurrFormat = ChooseSwapSurfaceFormat(Formats);
		vk::PresentModeKHR PresentMode = ChooseSwapPresentMode(PresentModes);
		vk::Extent2D Extent = ChooseSwapExtent(Capabilities, Width, Height);

		ImageFormat = CurrFormat.format;

		uint32 MinImageCount = Capabilities.minImageCount + 1;
		if (Capabilities.maxImageCount > 0 && MinImageCount > Capabilities.maxImageCount)
		{
			MinImageCount = Capabilities.maxImageCount;
		}

		vk::SwapchainCreateInfoKHR SwapchainInfo;
		SwapchainInfo
			.setSurface(Surface)
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
			Swapchain = Device.GetHandle().createSwapchainKHR(SwapchainInfo);
			DOGE_TRACE("Vulkan swap chain created");
		}
		catch (const std::runtime_error& err)
		{
			DOGE_ERROR("Failed to create vulkan swap chain: {}", err.what());
		}

		Device.SetupPresentQueue(Surface);
		SwapchainImages = Device.GetHandle().getSwapchainImagesKHR(Swapchain);

		// TODO: use managed semaphore
		ImageAcquiredSemaphore = new FVulkanSemaphore(Device);
	}

	FVulkanSwapchain::~FVulkanSwapchain()
	{
		Destroy();
	}

	auto FVulkanSwapchain::GetImages() const -> const std::vector<vk::Image>&
	{
		return SwapchainImages;
	}

	auto FVulkanSwapchain::AcquireImageIndex(FVulkanSemaphore** OutImageAcquiredSemaphore) -> uint32
	{
		// TODO: Semaphore
		vk::ResultValue<uint32> Result = Device.GetHandle().acquireNextImageKHR(Swapchain, UINT64_MAX, ImageAcquiredSemaphore->GetHandle());
		if (Result.result != vk::Result::eSuccess)
		{
			CurrentImageIndex = -1;
			DOGE_ERROR("Failed to acquire swap chain image: {}", vk::to_string(Result.result));
		}
		CurrentImageIndex = Result.value;
		*OutImageAcquiredSemaphore = ImageAcquiredSemaphore;

		return CurrentImageIndex;
	}

	auto FVulkanSwapchain::Present(FVulkanQueue* PresentQueue, FVulkanSemaphore* BackBufferRenderingDoneSemaphore) -> void
	{
		vk::PresentInfoKHR PresentInfo;
		auto Semaphore = BackBufferRenderingDoneSemaphore->GetHandle();

		uint32 ImageIndex = static_cast<uint32>(CurrentImageIndex);
		PresentInfo
			.setWaitSemaphores(Semaphore)
			.setSwapchains(Swapchain)
			.setImageIndices(ImageIndex);

		vk::Result Result = PresentQueue->GetHandle().presentKHR(PresentInfo);
		check(Result == vk::Result::eSuccess);

		CurrentImageIndex = -1;
	}

	auto FVulkanSwapchain::Destroy() -> void
	{
		delete ImageAcquiredSemaphore;
		Device.GetHandle().destroySwapchainKHR(Swapchain);
		Instance.destroySurfaceKHR(Surface);
		Surface = nullptr;
	}
} // namespace Doge::VulkanRHI