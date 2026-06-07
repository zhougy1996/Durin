#include "VulkanSwapchain.h"

#include "VulkanDevice.h"
#include "VulkanQueue.h"

namespace Durin::VulkanRHI
{
	namespace
	{
		auto GetPreferredPresentModes(EViewportPresentModePolicy Policy) -> std::vector<vk::PresentModeKHR>
		{
			switch (Policy)
			{
			case EViewportPresentModePolicy::ImGuiDetachedViewport:
				return {vk::PresentModeKHR::eMailbox, vk::PresentModeKHR::eImmediate, vk::PresentModeKHR::eFifo};
			case EViewportPresentModePolicy::MainWindow:
			default:
				return {vk::PresentModeKHR::eFifo};
			}
		}

		auto IsRecoverableSwapchainResult(const vk::Result Result) -> bool
		{
			return Result == vk::Result::eErrorOutOfDateKHR || Result == vk::Result::eSuboptimalKHR;
		}

		auto GetSystemErrorResult(const vk::SystemError& Error) -> vk::Result
		{
			return static_cast<vk::Result>(Error.code().value());
		}
	}

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

	auto ChooseSwapPresentMode(const std::vector<vk::PresentModeKHR>& AvailablePresentModes, EViewportPresentModePolicy Policy) -> vk::PresentModeKHR
	{
		for (const vk::PresentModeKHR RequestedMode : GetPreferredPresentModes(Policy))
		{
			if (std::ranges::find(AvailablePresentModes, RequestedMode) != AvailablePresentModes.end())
			{
				return RequestedMode;
			}
		}
		return vk::PresentModeKHR::eFifo;
	}

	auto GetMinImageCountForPresentMode(vk::PresentModeKHR PresentMode) -> uint32
	{
		switch (PresentMode)
		{
		case vk::PresentModeKHR::eMailbox:
			return 3;
		case vk::PresentModeKHR::eImmediate:
		case vk::PresentModeKHR::eFifo:
		default:
			return 2;
		}
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

	FVulkanSwapchain::FVulkanSwapchain(FVulkanDevice& InDevice, vk::SurfaceKHR InSurface, uint32 Width, uint32 Height, bool bIsFullScreen, EViewportPresentModePolicy InPresentModePolicy, vk::SwapchainKHR InOldSwapchain)
		: Device(InDevice)
		, Surface(InSurface)
		, PresentModePolicy(InPresentModePolicy)
	{
		// Get Swap chain support details
		vk::PhysicalDevice Gpu = Device.GetGpu();
		vk::SurfaceCapabilitiesKHR Capabilities = Gpu.getSurfaceCapabilitiesKHR(Surface);
		std::vector<vk::SurfaceFormatKHR> Formats = Gpu.getSurfaceFormatsKHR(Surface);
		std::vector<vk::PresentModeKHR> PresentModes = Gpu.getSurfacePresentModesKHR(Surface);

		vk::SurfaceFormatKHR CurrFormat = ChooseSwapSurfaceFormat(Formats);
		vk::PresentModeKHR PresentMode = ChooseSwapPresentMode(PresentModes, PresentModePolicy);
		Extent = ChooseSwapExtent(Capabilities, Width, Height);

		ImageFormat = CurrFormat.format;

		uint32 MinImageCount = GetMinImageCountForPresentMode(PresentMode);
		MinImageCount = FMath::Max(MinImageCount, Capabilities.minImageCount);
		if (Capabilities.maxImageCount > 0 && MinImageCount > Capabilities.maxImageCount)
		{
			MinImageCount = Capabilities.maxImageCount;
		}

		check(MinImageCount >= kFrameInFlight);

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
			.setOldSwapchain(InOldSwapchain);
		try
		{
			Swapchain = Device.GetHandle().createSwapchainKHR(SwapchainInfo);
			DURIN_TRACE("Vulkan swap chain created");
		}
		catch (const std::runtime_error& err)
		{
			DURIN_ERROR("Failed to create vulkan swap chain: {}", err.what());
		}

		Device.SetupPresentQueue(Surface);
		SwapchainImages = Device.GetHandle().getSwapchainImagesKHR(Swapchain);

		// Each acquire semaphore must not be reused while a previous acquire/submit using it is still pending.
		ImageAcquiredSemaphores.resize(SwapchainImages.size());
		for (uint32 i = 0; i < ImageAcquiredSemaphores.size(); i++)
		{
			ImageAcquiredSemaphores[i] = new FVulkanSemaphore(Device);
		}

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
		FVulkanSemaphore* CurrentSemaphore = ImageAcquiredSemaphores[NextSemaphoreIndex];
		NextSemaphoreIndex = (NextSemaphoreIndex + 1) % ImageAcquiredSemaphores.size();

		const vk::ResultValue<uint32> Result = Device.GetHandle().acquireNextImageKHR(Swapchain, UINT64_MAX, CurrentSemaphore->GetHandle(), nullptr);

		if (Result.result != vk::Result::eSuccess && Result.result != vk::Result::eSuboptimalKHR)
		{
			CurrentImageIndex = -1;
			bNeedsRecreate = IsRecoverableSwapchainResult(Result.result);
			if (!IsRecoverableSwapchainResult(Result.result))
			{
				DURIN_ERROR("Failed to acquire swap chain image: {}", vk::to_string(Result.result));
			}
			*OutImageAcquiredSemaphore = nullptr;
			return INDEX_NONE_U32;
		}
		if (Result.result == vk::Result::eSuboptimalKHR)
		{
			bNeedsRecreate = true;
		}
		CurrentImageIndex = static_cast<int32>(Result.value);
		*OutImageAcquiredSemaphore = CurrentSemaphore;

		return static_cast<uint32>(CurrentImageIndex);
	}

	auto FVulkanSwapchain::Present(FVulkanQueue* PresentQueue, FVulkanSemaphore* BackBufferRenderingDoneSemaphore) -> bool
	{
		if (CurrentImageIndex < 0)
		{
			return false;
		}

		vk::PresentInfoKHR PresentInfo;
		auto Semaphore = BackBufferRenderingDoneSemaphore->GetHandle();

		uint32 ImageIndex = static_cast<uint32>(CurrentImageIndex);
		PresentInfo
			.setWaitSemaphores(Semaphore)
			.setSwapchains(Swapchain)
			.setImageIndices(ImageIndex);

		vk::Result Result = vk::Result::eSuccess;
		try
		{
			Result = PresentQueue->GetHandle().presentKHR(PresentInfo);
		}
		catch (const vk::SystemError& Error)
		{
			const vk::Result ErrorResult = GetSystemErrorResult(Error);
			CurrentImageIndex = -1;
			bNeedsRecreate = IsRecoverableSwapchainResult(ErrorResult);
			if (!IsRecoverableSwapchainResult(ErrorResult))
			{
				DURIN_ERROR("Failed to present swap chain image: {}", Error.what());
			}
			return false;
		}

		if (Result != vk::Result::eSuccess && Result != vk::Result::eSuboptimalKHR)
		{
			bNeedsRecreate = IsRecoverableSwapchainResult(Result);
			if (!IsRecoverableSwapchainResult(Result))
			{
				DURIN_ERROR("Failed to present swap chain image: {}", vk::to_string(Result));
			}
			CurrentImageIndex = -1;
			return false;
		}
		if (Result == vk::Result::eSuboptimalKHR)
		{
			bNeedsRecreate = true;
		}

		CurrentImageIndex = -1;
		return true;
	}

	auto FVulkanSwapchain::Destroy() -> void
	{
		for (FVulkanSemaphore* Semaphore : ImageAcquiredSemaphores)
		{
			delete Semaphore;
		}
		ImageAcquiredSemaphores.clear();

		if (Swapchain != VK_NULL_HANDLE)
		{
			Device.GetHandle().destroySwapchainKHR(Swapchain);
			Swapchain = VK_NULL_HANDLE;
		}
	}
} // namespace Durin::VulkanRHI
