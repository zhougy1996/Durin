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

		auto ArePresentQueueOperationsEnqueued(const vk::Result Result) -> bool
		{
			// Presentation-engine rejection still leaves the queue operations enqueued.
			return Result == vk::Result::eSuccess
				|| Result == vk::Result::eSuboptimalKHR
				|| Result == vk::Result::eErrorOutOfDateKHR
				|| Result == vk::Result::eErrorSurfaceLostKHR
				|| Result == vk::Result::eErrorFullScreenExclusiveModeLostEXT;
		}

		auto GetSystemErrorResult(const vk::SystemError& Error) -> vk::Result
		{
			return static_cast<vk::Result>(Error.code().value());
		}

		auto PresentModePolicyName(const EViewportPresentModePolicy Policy) -> const char*
		{
			return Policy == EViewportPresentModePolicy::ImGuiDetachedViewport ? "ImGuiDetachedViewport" : "MainWindow";
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
		}
		catch (const vk::SystemError& err)
		{
			DURIN_ERROR("Failed to create Vulkan swapchain: result={}, extent={}x{}, format={}, colorSpace={}, presentMode={}, policy={}, error={}",
				vk::to_string(GetSystemErrorResult(err)), Extent.width, Extent.height, vk::to_string(CurrFormat.format), vk::to_string(CurrFormat.colorSpace),
				vk::to_string(PresentMode), PresentModePolicyName(PresentModePolicy), err.what());
		}
		catch (const std::runtime_error& err)
		{
			DURIN_ERROR("Failed to create Vulkan swapchain: result=unavailable, extent={}x{}, format={}, colorSpace={}, presentMode={}, policy={}, error={}",
				Extent.width, Extent.height, vk::to_string(CurrFormat.format), vk::to_string(CurrFormat.colorSpace), vk::to_string(PresentMode),
				PresentModePolicyName(PresentModePolicy), err.what());
		}

		Device.SetupPresentQueue(Surface);
		SwapchainImages = Device.GetHandle().getSwapchainImagesKHR(Swapchain);
		DURIN_DEBUG("Vulkan swapchain {}: extent={}x{}, format={}, colorSpace={}, presentMode={}, images={}, policy={}, windowMode={}.",
			InOldSwapchain ? "recreated" : "created", Extent.width, Extent.height, vk::to_string(CurrFormat.format), vk::to_string(CurrFormat.colorSpace),
			vk::to_string(PresentMode), SwapchainImages.size(), PresentModePolicyName(PresentModePolicy), bIsFullScreen ? "fullscreen" : "windowed");

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
			if (IsRecoverableSwapchainResult(Result.result))
			{
				MarkNeedsRecreate("acquire", Result.result);
			}
			else
			{
				DURIN_ERROR("Failed to acquire a Vulkan swapchain image: result={}, extent={}x{}.",
					vk::to_string(Result.result), Extent.width, Extent.height);
			}
			*OutImageAcquiredSemaphore = nullptr;
			return INDEX_NONE_U32;
		}
		if (Result.result == vk::Result::eSuboptimalKHR)
		{
			MarkNeedsRecreate("acquire", Result.result);
		}
		CurrentImageIndex = static_cast<int32>(Result.value);
		*OutImageAcquiredSemaphore = CurrentSemaphore;

		return static_cast<uint32>(CurrentImageIndex);
	}

	auto FVulkanSwapchain::Present(FVulkanQueue* PresentQueue, FVulkanSemaphore* BackBufferRenderingDoneSemaphore, vk::Fence PresentFence) -> FVulkanPresentOutcome
	{
		if (CurrentImageIndex < 0)
		{
			return {};
		}

		vk::PresentInfoKHR PresentInfo;
		auto Semaphore = BackBufferRenderingDoneSemaphore->GetHandle();

		uint32 ImageIndex = static_cast<uint32>(CurrentImageIndex);
		PresentInfo
			.setWaitSemaphores(Semaphore)
			.setSwapchains(Swapchain)
			.setImageIndices(ImageIndex);
		vk::SwapchainPresentFenceInfoEXT PresentFenceInfo;
		if (PresentFence != VK_NULL_HANDLE)
		{
			PresentFenceInfo.setFences(PresentFence);
			PresentInfo.setPNext(&PresentFenceInfo);
		}

		vk::Result Result = vk::Result::eSuccess;
		try
		{
			Result = PresentQueue->GetHandle().presentKHR(PresentInfo);
		}
		catch (const vk::SystemError& Error)
		{
			const vk::Result ErrorResult = GetSystemErrorResult(Error);
			CurrentImageIndex = -1;
			if (IsRecoverableSwapchainResult(ErrorResult))
			{
				MarkNeedsRecreate("present", ErrorResult);
			}
			else
			{
				DURIN_ERROR("Failed to present a Vulkan swapchain image: result={}, image={}, extent={}x{}, error={}",
					vk::to_string(ErrorResult), ImageIndex, Extent.width, Extent.height, Error.what());
			}
			return {
				.bPresented = false,
				.bQueueOperationsEnqueued = ArePresentQueueOperationsEnqueued(ErrorResult)
			};
		}

		if (Result != vk::Result::eSuccess && Result != vk::Result::eSuboptimalKHR)
		{
			if (IsRecoverableSwapchainResult(Result))
			{
				MarkNeedsRecreate("present", Result);
			}
			else
			{
				DURIN_ERROR("Failed to present a Vulkan swapchain image: result={}, image={}, extent={}x{}.",
					vk::to_string(Result), ImageIndex, Extent.width, Extent.height);
			}
			CurrentImageIndex = -1;
			return {
				.bPresented = false,
				.bQueueOperationsEnqueued = ArePresentQueueOperationsEnqueued(Result)
			};
		}
		if (Result == vk::Result::eSuboptimalKHR)
		{
			MarkNeedsRecreate("present", Result);
		}

		CurrentImageIndex = -1;
		return {
			.bPresented = true,
			.bQueueOperationsEnqueued = true
		};
	}

	auto FVulkanSwapchain::MarkNeedsRecreate(const std::string_view Operation, const vk::Result Result) -> void
	{
		if (!bNeedsRecreate)
		{
			DURIN_DEBUG("Vulkan swapchain requires recreation: operation={}, result={}, extent={}x{}.",
				Operation, vk::to_string(Result), Extent.width, Extent.height);
		}
		bNeedsRecreate = true;
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
