#include "VulkanSwapchain.h"

#include "VulkanDevice.h"
#include "VulkanDynamicRHI.h"
#include "VulkanQueue.h"
#include "VulkanRHIPrivate.h"

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

		auto IsTemporarilyUnavailableAcquireResult(const vk::Result Result) -> bool
		{
			return Result == vk::Result::eNotReady || Result == vk::Result::eTimeout;
		}

		auto ArePresentQueueOperationsEnqueued(const vk::Result Result) -> bool
		{
			// Presentation-engine rejection still leaves the queue operations enqueued.
			const bool bCommonResult = Result == vk::Result::eSuccess
				|| Result == vk::Result::eSuboptimalKHR
				|| Result == vk::Result::eErrorOutOfDateKHR
				|| Result == vk::Result::eErrorSurfaceLostKHR;
#ifdef VK_USE_PLATFORM_WIN32_KHR
			return bCommonResult
				|| Result == vk::Result::eErrorFullScreenExclusiveModeLostEXT;
#else
			return bCommonResult;
#endif
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
		for (const vk::Format PreferredFormat : {
				vk::Format::eR8G8B8A8Srgb,
				vk::Format::eB8G8R8A8Srgb})
		{
			const auto It = std::ranges::find_if(
				AvailableFormats,
				[PreferredFormat](const vk::SurfaceFormatKHR& Format) {
					return Format.format == PreferredFormat
						&& Format.colorSpace
							== vk::ColorSpaceKHR::eSrgbNonlinear;
				});
			if (It != AvailableFormats.end())
			{
				return *It;
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

	auto SelectVulkanSwapchainConfiguration(
		const FVulkanSwapchainSelectionInput& Input,
		FVulkanSwapchainConfiguration& OutConfiguration,
		std::string& OutError) -> bool
	{
		auto Fail = [&OutError](const char* Error) {
			OutError = Error;
			return false;
		};
		if (Input.Formats.empty())
			return Fail("Vulkan swapchain selection failed: the surface reported no formats.");
		if (Input.PresentModes.empty())
			return Fail("Vulkan swapchain selection failed: the surface reported no present modes.");
		if ((Input.Capabilities.supportedUsageFlags & RequiredSwapchainImageUsage)
			!= RequiredSwapchainImageUsage)
			return Fail("Vulkan swapchain selection failed: required backbuffer image usage is unsupported.");
		if (Input.Capabilities.minImageExtent.width > Input.Capabilities.maxImageExtent.width
			|| Input.Capabilities.minImageExtent.height > Input.Capabilities.maxImageExtent.height)
			return Fail("Vulkan swapchain selection failed: the surface extent range is invalid.");
		if (Input.Capabilities.maxImageCount > 0
			&& Input.Capabilities.maxImageCount < Input.Capabilities.minImageCount)
			return Fail("Vulkan swapchain selection failed: the surface image-count range is invalid.");

		FVulkanSwapchainConfiguration Configuration;
		Configuration.SurfaceFormat = ChooseSwapSurfaceFormat(Input.Formats);
		Configuration.PresentMode = ChooseSwapPresentMode(
			Input.PresentModes, Input.PresentModePolicy);
		if (std::ranges::find(Input.PresentModes, Configuration.PresentMode)
			== Input.PresentModes.end())
			return Fail("Vulkan swapchain selection failed: no policy-compatible present mode is supported.");
		Configuration.Extent = ChooseSwapExtent(Input.Capabilities,
			Input.RequestedWidth, Input.RequestedHeight);
		if (Configuration.Extent.width == 0 || Configuration.Extent.height == 0)
			return Fail("Vulkan swapchain selection failed: the selected extent is empty.");
		Configuration.ImageCount = FMath::Max(
			GetMinImageCountForPresentMode(Configuration.PresentMode),
			Input.Capabilities.minImageCount);
		if (Input.Capabilities.maxImageCount > 0)
			Configuration.ImageCount = FMath::Min(
				Configuration.ImageCount, Input.Capabilities.maxImageCount);
		if (Configuration.ImageCount < kFrameInFlight)
			return Fail("Vulkan swapchain selection failed: the supported image count is below the frames-in-flight requirement.");
		Configuration.ImageUsage = RequiredSwapchainImageUsage;
		Configuration.PreTransform = Input.Capabilities.currentTransform;
		for (const vk::CompositeAlphaFlagBitsKHR Candidate : {
			vk::CompositeAlphaFlagBitsKHR::eOpaque,
			vk::CompositeAlphaFlagBitsKHR::ePreMultiplied,
			vk::CompositeAlphaFlagBitsKHR::ePostMultiplied,
			vk::CompositeAlphaFlagBitsKHR::eInherit})
		{
			if (Input.Capabilities.supportedCompositeAlpha & Candidate)
			{
				Configuration.CompositeAlpha = Candidate;
				OutConfiguration = Configuration;
				OutError.clear();
				return true;
			}
		}
		return Fail("Vulkan swapchain selection failed: the surface reported no supported composite-alpha mode.");
	}

	FVulkanSwapchain::FVulkanSwapchain(FVulkanDevice& InDevice, vk::SurfaceKHR InSurface, uint32 Width, uint32 Height, bool bIsFullScreen, EViewportPresentModePolicy InPresentModePolicy, vk::SwapchainKHR InOldSwapchain, bool& bOutNativeSwapchainCreated)
		: Device(InDevice)
		, Surface(InSurface)
		, PresentModePolicy(InPresentModePolicy)
	{
		CheckVulkanRHIThread();
		bOutNativeSwapchainCreated = false;
		if (!Device.SetupPresentQueue(Surface))
		{
			throw std::runtime_error(
				"Vulkan swapchain creation rejected an incompatible surface before native creation.");
		}
		// Get Swap chain support details
		vk::PhysicalDevice Gpu = Device.GetGpu();
		std::vector<vk::SurfaceFormatKHR> Formats = Gpu.getSurfaceFormatsKHR(Surface);
		std::vector<vk::PresentModeKHR> PresentModes = Gpu.getSurfacePresentModesKHR(Surface);
		// Query the size-sensitive capabilities last so the selected extent is as
		// close as possible to native swapchain creation.
		vk::SurfaceCapabilitiesKHR Capabilities = Gpu.getSurfaceCapabilitiesKHR(Surface);

		FVulkanSwapchainConfiguration Configuration;
		std::string SelectionError;
		if (!SelectVulkanSwapchainConfiguration({
				.Capabilities = Capabilities,
				.Formats = std::move(Formats),
				.PresentModes = std::move(PresentModes),
				.RequestedWidth = Width,
				.RequestedHeight = Height,
				.PresentModePolicy = PresentModePolicy}, Configuration, SelectionError))
			throw std::runtime_error(SelectionError);
		Extent = Configuration.Extent;
		ImageFormat = Configuration.SurfaceFormat.format;

		vk::SwapchainCreateInfoKHR SwapchainInfo;
		SwapchainInfo
			.setSurface(Surface)
			.setMinImageCount(Configuration.ImageCount)
			.setImageFormat(Configuration.SurfaceFormat.format)
			.setImageColorSpace(Configuration.SurfaceFormat.colorSpace)
			.setImageExtent(Extent)
			.setImageArrayLayers(1)
			.setImageUsage(Configuration.ImageUsage)
			.setPreTransform(Configuration.PreTransform)
			.setImageSharingMode(vk::SharingMode::eExclusive)
			.setPresentMode(Configuration.PresentMode)
			.setClipped(vk::True)
			.setCompositeAlpha(Configuration.CompositeAlpha)
			.setOldSwapchain(InOldSwapchain);
#if DURIN_VULKAN_TEST_FAILURE_INJECTION
		ThrowIfVulkanNativeCreateFailureIsArmed(EVulkanCreateFailurePoint::Swapchain);
#endif
		Swapchain = Device.GetHandle().createSwapchainKHR(SwapchainInfo);
		Device.GetRHI().GetDebugUtils().NameObject(Swapchain,
			Device.GetRHI().GetDebugUtils().MakeInternalName("Swapchain"));
		bOutNativeSwapchainCreated = true;

		try
		{
			SwapchainImages = Device.GetHandle().getSwapchainImagesKHR(Swapchain);
		}
		catch (...)
		{
			Destroy();
			throw;
		}
		DURIN_DEBUG("Vulkan swapchain {}: extent={}x{}, format={}, colorSpace={}, presentMode={}, images={}, policy={}, windowMode={}.",
			InOldSwapchain ? "recreated" : "created", Extent.width, Extent.height,
			vk::to_string(Configuration.SurfaceFormat.format),
			vk::to_string(Configuration.SurfaceFormat.colorSpace),
			vk::to_string(Configuration.PresentMode), SwapchainImages.size(),
			PresentModePolicyName(PresentModePolicy), bIsFullScreen ? "fullscreen" : "windowed");

	}

	auto FVulkanSwapchain::InitializeSynchronizationResources() -> void
	{
		CheckVulkanRHIThread();
		check(ImageAcquiredSemaphores.empty());
		try
		{
			for (uint32 i = 0; i < SwapchainImages.size(); ++i)
			{
#if DURIN_VULKAN_TEST_FAILURE_INJECTION
				ThrowIfVulkanNativeCreateFailureIsArmed(EVulkanCreateFailurePoint::SwapchainSemaphore);
#endif
				ImageAcquiredSemaphores.push_back(new FVulkanSemaphore(Device));
			}
		}
		catch (...)
		{
			for (FVulkanSemaphore* Semaphore : ImageAcquiredSemaphores)
			{
				Semaphore->DestroyImmediately();
				delete Semaphore;
			}
			ImageAcquiredSemaphores.clear();
			throw;
		}
	}

	FVulkanSwapchain::~FVulkanSwapchain()
	{
		CheckVulkanRHIThread();
		Destroy();
	}

	auto FVulkanSwapchain::GetImages() const -> const std::vector<vk::Image>&
	{
		return SwapchainImages;
	}

	auto FVulkanSwapchain::AcquireImageIndex(FVulkanSemaphore** OutImageAcquiredSemaphore) -> uint32
	{
		CheckVulkanRHIThread();
		FVulkanSemaphore* CurrentSemaphore = ImageAcquiredSemaphores[NextSemaphoreIndex];
		NextSemaphoreIndex = (NextSemaphoreIndex + 1) % ImageAcquiredSemaphores.size();

		const vk::ResultValue<uint32> Result = [&] {
#if DURIN_VULKAN_TEST_FAILURE_INJECTION
			if (ConsumeVulkanSwapchainAcquireTimeoutForTest())
				return vk::ResultValue<uint32>{vk::Result::eTimeout, 0};
#endif
			return Device.GetHandle().acquireNextImageKHR(Swapchain,
				GetSwapchainAcquireTimeout(PresentModePolicy),
				CurrentSemaphore->GetHandle(), nullptr);
		}();

		if (Result.result != vk::Result::eSuccess && Result.result != vk::Result::eSuboptimalKHR)
		{
			CurrentImageIndex = -1;
			if (IsTemporarilyUnavailableAcquireResult(Result.result))
			{
				if (!bAcquireTimeoutReported)
				{
					DURIN_WARN("Vulkan swapchain image acquisition is temporarily unavailable: result={}, extent={}x{}, policy={}; skipping this viewport frame.",
						vk::to_string(Result.result), Extent.width, Extent.height,
						PresentModePolicyName(PresentModePolicy));
					bAcquireTimeoutReported = true;
				}
			}
			else if (IsRecoverableSwapchainResult(Result.result))
			{
				MarkNeedsRecreate("acquire", Result.result);
			}
			else
			{
				DURIN_ERROR("Failed to acquire a Vulkan swapchain image: result={}, extent={}x{}.",
					vk::to_string(Result.result), Extent.width, Extent.height);
				throw vk::SystemError(
					vk::make_error_code(Result.result),
					"Vulkan swapchain image acquisition failed");
			}
			*OutImageAcquiredSemaphore = nullptr;
			return INDEX_NONE_U32;
		}
		if (bAcquireTimeoutReported)
		{
			DURIN_INFO("Vulkan swapchain image acquisition recovered: extent={}x{}, policy={}.",
				Extent.width, Extent.height, PresentModePolicyName(PresentModePolicy));
			bAcquireTimeoutReported = false;
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
		CheckVulkanRHIThread();
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
				throw;
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
				throw vk::SystemError(
					vk::make_error_code(Result),
					"Vulkan swapchain presentation failed");
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
		CheckVulkanRHIThread();
		for (FVulkanSemaphore* Semaphore : ImageAcquiredSemaphores)
		{
			Semaphore->DestroyImmediately();
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
