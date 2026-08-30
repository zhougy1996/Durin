#include "VulkanViewport.h"
#include "VulkanPresentationCandidate.h"

#include "RHICommandList.h"
#include "VulkanCommon.h"
#include "VulkanDynamicRHI.h"
#include "VulkanDevice.h"
#include "VulkanGenericPlatform.h"
#include "VulkanView.h"
#include "VulkanContext.h"
#include "VulkanSwapchain.h"
#include "VulkanQueue.h"
#include "VulkanRHIPrivate.h"
#include "Threading/RunnableThread.h"
#include "RenderingThread.h"
#include "Profiling/Profiling.h"

namespace Durin::VulkanRHI
{
	namespace
	{
		constexpr size_t MaxRetiredSwapchainGenerations = 3;

		auto ToRHISwapchainPixelFormat(vk::Format Format) -> EPixelFormat
		{
			switch (Format)
			{
			case vk::Format::eR8G8B8A8Unorm: return EPixelFormat::RGBA8_UNORM;
			case vk::Format::eB8G8R8A8Unorm: return EPixelFormat::BGRA8_UNORM;
			case vk::Format::eR8G8B8A8Srgb: return EPixelFormat::SRGBA8_UNORM;
			case vk::Format::eB8G8R8A8Srgb: return EPixelFormat::SBGRA8_UNORM;
			default:
				checkf(false, "Vulkan selected an unsupported swapchain format.");
				return EPixelFormat::Unknown;
			}
		}
	}

	FVulkanBackBuffer::FVulkanBackBuffer(FVulkanDevice& InDevice, FVulkanViewport* InViewport)
		: FVulkanTexture(InDevice, nullptr)
		, Viewport(InViewport)
	{
		CheckVulkanRHIThread();
		if (Viewport->GetSwapchain() != nullptr)
		{
			UpdateSwapchain();
		}
	}

	auto FVulkanBackBuffer::UpdateSwapchain() -> void
	{
		CheckVulkanRHIThread();
		AdvanceExternalImageBacking();
		const vk::Extent2D Extent = Viewport->GetSwapchain()->GetExtent();
		SizeX = Extent.width;
		SizeY = Extent.height;
		Format = Viewport->GetSwapchainImageFormat();
		PixelFormat = ToRHISwapchainPixelFormat(Format);
		NumSamples = 1;
		ImageStates.clear();
	}

	auto FVulkanBackBuffer::InvalidateSwapchain() -> void
	{
		CheckVulkanRHIThread();
		AdvanceExternalImageBacking();
		SetExternalImage(VK_NULL_HANDLE);
	}

	auto FVulkanBackBuffer::AcquireBackBufferImage(
		FVulkanCommandListContext& Context) -> bool
	{
		CheckVulkanRHIThread();
		const FVulkanView* View = Viewport->AcquireBackBufferImage();
		if (View == nullptr)
		{
			SetExternalImage(VK_NULL_HANDLE);
			return false;
		}
		SetExternalImage(View->Image);
		const auto Found = std::ranges::find(ImageStates, Image, &std::pair<vk::Image, ERHIAccess>::first);
		const ERHIAccess Access = Found == ImageStates.end() ? ERHIAccess::None : Found->second;
		StateTracker.Apply({ERHITextureAspect::Color, 0, 1, 0, 1}, Access);
		if (Viewport->AcquiredSemaphore != nullptr)
		{
			Context.AddWaitSemaphore(vk::PipelineStageFlagBits::eColorAttachmentOutput, Viewport->AcquiredSemaphore);
		}
		return true;
	}

	auto FVulkanBackBuffer::CommitPresentedImageState() -> void
	{
		check(Image);
		const ERHIAccess Access = StateTracker.Get(ERHITextureAspect::Color, 0, 0);
		checkf(Access == ERHIAccess::Present,
			"Swapchain presentation requires the acquired image to be in Present state.");
		const auto Found = std::ranges::find(ImageStates, Image, &std::pair<vk::Image, ERHIAccess>::first);
		if (Found == ImageStates.end()) ImageStates.emplace_back(Image, Access);
		else Found->second = Access;
	}

	FVulkanViewport::FVulkanViewport(FVulkanDevice& InDevice,
		void* InWindowHandle, uint32 InSizeX, uint32 InSizeY,
		bool bInIsFullScreen, EPixelFormat InPreferredPixelFormat,
		EViewportPresentationPolicy InPresentationPolicy,
		vk::SurfaceKHR InPresentationSurface)
		: Device(InDevice)
		, SizeX(InSizeX)
		, SizeY(InSizeY)
		, bIsFullScreen(bInIsFullScreen)
		, NativeWindowHandle(InWindowHandle)
	 	, PixelFormat(InPreferredPixelFormat)
		, PresentationPolicy(InPresentationPolicy)
	{
		CheckVulkanRHIThread();
		Surface = InPresentationSurface
			? InPresentationSurface
			: FVulkanGenericPlatform::CreateSurface(
				InWindowHandle, FVulkanDynamicRHI::Get().RHIGetVkInstance());
		try
		{
			Device.GetRHI().GetDebugUtils().NameObject(Surface,
				Device.GetRHI().GetDebugUtils().MakeInternalName("Surface"));
			bSwapchainNeedsRecreate = true;
			bSwapchainRetryEligible = true;
			PrepareSwapchain();
			if (!RHIBackBuffer)
			{
				RHIBackBuffer = MakeRefCount<FVulkanBackBuffer>(Device, this);
			}
		}
		catch (...)
		{
			if (Surface)
			{
				FVulkanDynamicRHI::Get().RHIGetVkInstance().destroySurfaceKHR(Surface);
				Surface = VK_NULL_HANDLE;
			}
			throw;
		}
	}

	FVulkanViewport::~FVulkanViewport()
	{
		CheckVulkanRHIThread();
		WaitForSwapchainIdle();
		DestroySwapchain();
		CollectRetiredSwapchains(true);

		if (Surface != VK_NULL_HANDLE)
		{
			FVulkanDynamicRHI::Get().RHIGetVkInstance().destroySurfaceKHR(Surface);
			Surface = VK_NULL_HANDLE;
		}
	}

	auto FVulkanViewport::GetWindowHandle() -> void*
	{
		return NativeWindowHandle;
	}

	auto FVulkanViewport::Resize(
		FRHICommandListImmediate& RHICmdList,
		uint32 InSizeX,
		uint32 InSizeY,
		bool bInIsFullScreen) -> void
	{
		check(IsInRenderingThread());
		const bool bFullscreenChanged = bIsFullScreen != bInIsFullScreen;
		GCommandListExecutor.ExecuteSynchronousOperation(true,
			[this, InSizeX, InSizeY, bInIsFullScreen, bFullscreenChanged]() {
				CheckVulkanRHIThread();
				bIsFullScreen = bInIsFullScreen;
				RequestResize(InSizeX, InSizeY);
				bSwapchainNeedsRecreate |= bFullscreenChanged;
				PrepareSwapchain();
			});
	}

	auto FVulkanViewport::BeginDrawing() -> void
	{
		CheckVulkanRHIThread();
		CollectRetiredSwapchains(false);
		PrepareSwapchain();
	}

	auto FVulkanViewport::RequestResize(uint32 InSizeX, uint32 InSizeY) -> void
	{
		CheckVulkanRHIThread();
		if (InSizeX == 0 || InSizeY == 0)
		{
			return;
		}

		if (SizeX == InSizeX && SizeY == InSizeY && !bHasPendingResize)
		{
			return;
		}

		PendingSizeX = InSizeX;
		PendingSizeY = InSizeY;
		bHasPendingResize = true;
		bSwapchainNeedsRecreate = true;
		bSwapchainRetryEligible = true;
	}

	auto FVulkanViewport::PrepareSwapchain() -> void
	{
		CheckVulkanRHIThread();
		if (Swapchain != nullptr && Swapchain->NeedsRecreate() && !bSwapchainNeedsRecreate)
		{
			bSwapchainNeedsRecreate = true;
			bSwapchainRetryEligible = true;
		}

		if (!bSwapchainNeedsRecreate || !bSwapchainRetryEligible)
		{
			return;
		}

		const uint32 TargetSizeX = bHasPendingResize ? PendingSizeX : SizeX;
		const uint32 TargetSizeY = bHasPendingResize ? PendingSizeY : SizeY;
		if (TargetSizeX == 0 || TargetSizeY == 0)
		{
			return;
		}

		bSwapchainRetryEligible = false;
		if (TryCreateSwapchain(TargetSizeX, TargetSizeY))
		{
			PendingSizeX = 0;
			PendingSizeY = 0;
			bHasPendingResize = false;
			bSwapchainNeedsRecreate = false;
		}
	}

	auto FVulkanViewport::MarkSwapchainNeedsRecreate() -> void
	{
		CheckVulkanRHIThread();
		bSwapchainNeedsRecreate = true;
		bSwapchainRetryEligible = true;
	}

	auto FVulkanViewport::RecreateSwapchain() -> void
	{
		CheckVulkanRHIThread();
		bSwapchainNeedsRecreate = true;
		bSwapchainRetryEligible = true;
		PrepareSwapchain();
	}

	auto FVulkanViewport::AcquireBackBufferImage() -> FVulkanView*
	{
		CheckVulkanRHIThread();
		if (Swapchain == nullptr)
		{
			return nullptr;
		}
		AcquiredBackBufferIndex = static_cast<int32>(
			Swapchain->AcquireImageIndex(&AcquiredSemaphore));
		if (AcquiredBackBufferIndex < 0 && Swapchain->NeedsRecreate())
		{
			MarkSwapchainNeedsRecreate();
			PrepareSwapchain();
			if (Swapchain == nullptr)
			{
				return nullptr;
			}
			AcquiredBackBufferIndex = static_cast<int32>(
				Swapchain->AcquireImageIndex(&AcquiredSemaphore));
		}
		if (AcquiredBackBufferIndex < 0
			|| AcquiredBackBufferIndex >= static_cast<int32>(TextureViews.size()))
		{
			AcquiredBackBufferIndex = -1;
			AcquiredSemaphore = nullptr;
			return nullptr;
		}
		return &TextureViews[AcquiredBackBufferIndex];
	}

	auto FVulkanViewport::GetBackBuffer(FRHICommandListImmediate& InRHICmdList) -> TRefCountPtr<FRHITexture>
	{
		check(IsInRenderingThread());
		if (!RHIBackBuffer)
		{
			return nullptr;
		}
		TRefCountPtr<FVulkanBackBuffer> BackBuffer = RHIBackBuffer;
		InRHICmdList.AcquireBackBufferSynchronously(BackBuffer.GetReference());
		return AcquiredBackBufferIndex >= 0 ? BackBuffer : nullptr;
	}

	auto FVulkanViewport::Present(FVulkanCommandListContext& InContext, FVulkanCommandBuffer& InCmdBuffer, FVulkanQueue& InPresentQueue, bool bInLockToVsync) -> bool
	{
		DURIN_PROFILE_CPU_ZONE_NAMED("VulkanViewport.Present");
		CheckVulkanRHIThread();
		if (AcquiredBackBufferIndex < 0 || AcquiredBackBufferIndex >= static_cast<int32>(FrameResources.size()))
		{
			return false;
		}

		FVulkanViewportFrameResources& FrameResource = FrameResources[AcquiredBackBufferIndex];
		check(RHIBackBuffer);
		RHIBackBuffer->CommitPresentedImageState();
		WaitForFrameResource(FrameResource);
		FVulkanSemaphore* RenderingDoneSemaphore = FrameResource.RenderingDoneSemaphore;
		check(RenderingDoneSemaphore != nullptr);
		InContext.AddSignalSemaphore(RenderingDoneSemaphore);
		InContext.Finalize();
		const bool bTrackPresent = Device.SupportsSwapchainMaintenance1();
		const FVulkanPresentOutcome PresentOutcome =
			Swapchain->Present(&InPresentQueue, RenderingDoneSemaphore, bTrackPresent ? FrameResource.PresentFence : VK_NULL_HANDLE);
		if (bTrackPresent)
		{
			FrameResource.State = PresentOutcome.bQueueOperationsEnqueued
				? EVulkanPresentResourceState::PresentPending
				: EVulkanPresentResourceState::Retired;
		}
		const bool bNeedsRecreate = Swapchain->NeedsRecreate();
		if (!PresentOutcome.bPresented || bNeedsRecreate)
		{
			MarkSwapchainNeedsRecreate();
		}
		AcquiredBackBufferIndex = -1;
		AcquiredSemaphore = nullptr;
		if (PresentOutcome.bPresented && Profiling::RecordEditorShellFirstPresent())
			DURIN_PROFILE_STARTUP_FIRST_PRESENT();
		return PresentOutcome.bPresented;
	}

	auto FVulkanViewport::GetFormat() const -> EPixelFormat
	{
		return PixelFormat;
	}

	auto FVulkanViewport::GetSwapchainImageFormat() const -> vk::Format
	{
		return Swapchain != nullptr ? Swapchain->GetFormat() : vk::Format::eUndefined;
	}

	auto FVulkanViewport::TryCreateSwapchain(uint32 TargetSizeX, uint32 TargetSizeY) -> bool
	{
		DURIN_PROFILE_CPU_ZONE_NAMED("VulkanViewport.TryCreateSwapchain");
		CheckVulkanRHIThread();
		CollectRetiredSwapchains(false);
		const bool bDeferCurrentSwapchainDestruction =
			CanDeferCurrentSwapchainDestruction();
		if (!bDeferCurrentSwapchainDestruction)
		{
			WaitForSwapchainIdle();
		}
		else if (RetiredSwapchainGenerations.size()
			>= MaxRetiredSwapchainGenerations)
		{
			FVulkanRetiredSwapchainGeneration& OldestGeneration =
				RetiredSwapchainGenerations.front();
			check(IsRetiredSwapchainReady(OldestGeneration, true));
			DestroySwapchainGeneration(OldestGeneration);
			RetiredSwapchainGenerations.erase(
				RetiredSwapchainGenerations.begin());
		}
		AcquiredBackBufferIndex = -1;
		AcquiredSemaphore = nullptr;

		std::unique_ptr<FVulkanSwapchain> CandidateSwapchain;
		std::vector<vk::Image> CandidateImages;
		std::vector<FVulkanView> CandidateViews;
		std::vector<FVulkanViewportFrameResources> CandidateFrameResources;
		bool bNativeSwapchainCreated = false;

		auto DestroyCandidateResources = [&]() {
			for (FVulkanViewportFrameResources& FrameResource : CandidateFrameResources)
			{
				if (FrameResource.RenderingDoneSemaphore != nullptr)
				{
					FrameResource.RenderingDoneSemaphore->DestroyImmediately();
					delete FrameResource.RenderingDoneSemaphore;
					FrameResource.RenderingDoneSemaphore = nullptr;
				}
				if (FrameResource.PresentFence != VK_NULL_HANDLE)
				{
					Device.GetHandle().destroyFence(FrameResource.PresentFence);
					FrameResource.PresentFence = VK_NULL_HANDLE;
				}
			}
			CandidateFrameResources.clear();
			for (const FVulkanView& View : CandidateViews)
			{
				Device.GetHandle().destroyImageView(View.ImageView);
			}
			CandidateViews.clear();
			CandidateSwapchain.reset();
		};

		try
		{
			const vk::SwapchainKHR OldSwapchain = Swapchain != nullptr
				? Swapchain->GetHandle()
				: VK_NULL_HANDLE;
			CandidateSwapchain = std::make_unique<FVulkanSwapchain>(
				Device, Surface, TargetSizeX, TargetSizeY, bIsFullScreen,
				PresentationPolicy, OldSwapchain, bNativeSwapchainCreated);
			CandidateSwapchain->InitializeSynchronizationResources();
			CandidateImages = CandidateSwapchain->GetImages();
			for (uint32 ImageIndex = 0; ImageIndex < CandidateImages.size(); ++ImageIndex)
			{
				Device.GetRHI().GetDebugUtils().NameObject(CandidateImages[ImageIndex],
					std::format("Durin.SwapchainImage.{}", ImageIndex));
			}

			vk::ImageViewCreateInfo ImageViewCreateInfo;
			ImageViewCreateInfo.setViewType(vk::ImageViewType::e2D);
			ImageViewCreateInfo.setFormat(CandidateSwapchain->GetFormat());
			ImageViewCreateInfo.setComponents({vk::ComponentSwizzle::eR, vk::ComponentSwizzle::eG, vk::ComponentSwizzle::eB, vk::ComponentSwizzle::eA});
			ImageViewCreateInfo.setSubresourceRange({vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1});
			for (uint32 ImageIndex = 0; ImageIndex < CandidateImages.size(); ++ImageIndex)
			{
				const vk::Image Image = CandidateImages[ImageIndex];
#if DURIN_VULKAN_TEST_FAILURE_INJECTION
				ThrowIfVulkanNativeCreateFailureIsArmed(EVulkanCreateFailurePoint::SwapchainImageView);
#endif
				ImageViewCreateInfo.setImage(Image);
				CandidateViews.emplace_back(Image, Device.GetHandle().createImageView(ImageViewCreateInfo));
				Device.GetRHI().GetDebugUtils().NameObject(CandidateViews.back().ImageView,
					std::format("Durin.SwapchainImageView.{}", ImageIndex));
			}

			CandidateFrameResources.resize(CandidateImages.size());
			for (FVulkanViewportFrameResources& FrameResource : CandidateFrameResources)
			{
#if DURIN_VULKAN_TEST_FAILURE_INJECTION
				ThrowIfVulkanNativeCreateFailureIsArmed(EVulkanCreateFailurePoint::SwapchainSemaphore);
#endif
				FrameResource.RenderingDoneSemaphore = new FVulkanSemaphore(Device);
				if (Device.SupportsSwapchainMaintenance1())
				{
#if DURIN_VULKAN_TEST_FAILURE_INJECTION
					ThrowIfVulkanNativeCreateFailureIsArmed(EVulkanCreateFailurePoint::SwapchainFence);
#endif
					FrameResource.PresentFence = Device.GetHandle().createFence(vk::FenceCreateInfo());
					Device.GetRHI().GetDebugUtils().NameObject(FrameResource.PresentFence,
						Device.GetRHI().GetDebugUtils().MakeInternalName("PresentFence"));
				}
			}
		}
		catch (const vk::SystemError& Error)
		{
			DestroyCandidateResources();
			const vk::Result Result = static_cast<vk::Result>(Error.code().value());
			if (Result == vk::Result::eErrorDeviceLost
				|| Result == vk::Result::eErrorSurfaceLostKHR
				|| (Result != vk::Result::eErrorOutOfHostMemory
					&& Result != vk::Result::eErrorOutOfDeviceMemory
					&& Result != vk::Result::eErrorOutOfDateKHR))
			{
				throw;
			}
			if (!bSwapchainFailureReported)
			{
				DURIN_ERROR("Failed to build Vulkan viewport output candidate: result={}, extent={}x{}, policy={}, nativeSwapchainCreated={}, error={}",
					vk::to_string(Result), TargetSizeX, TargetSizeY,
					PresentationPolicy == EViewportPresentationPolicy::BestEffort ? "BestEffort" : "FramePaced",
					bNativeSwapchainCreated, Error.what());
				bSwapchainFailureReported = true;
			}
			if (bNativeSwapchainCreated)
			{
				WaitForSwapchainIdle();
				SetOutputUnavailable();
			}
			return false;
		}
		catch (...)
		{
			DestroyCandidateResources();
			if (bNativeSwapchainCreated)
			{
				WaitForSwapchainIdle();
				SetOutputUnavailable();
			}
			throw;
		}

		if (bDeferCurrentSwapchainDestruction)
		{
			RetireCurrentSwapchain();
		}
		else
		{
			DestroySwapchain();
		}
		Swapchain = CandidateSwapchain.release();
		BackBufferImages = std::move(CandidateImages);
		TextureViews = std::move(CandidateViews);
		FrameResources = std::move(CandidateFrameResources);
		const vk::Extent2D SwapchainExtent = Swapchain->GetExtent();
		SizeX = SwapchainExtent.width;
		SizeY = SwapchainExtent.height;
		if (RHIBackBuffer)
		{
			RHIBackBuffer->UpdateSwapchain();
		}
		else
		{
			RHIBackBuffer = MakeRefCount<FVulkanBackBuffer>(Device, this);
		}
		if (bSwapchainFailureReported)
		{
			DURIN_DEBUG("Vulkan viewport output recovered: extent={}x{}.", SizeX, SizeY);
			bSwapchainFailureReported = false;
		}
		AcquiredBackBufferIndex = -1;
		return true;
	}

	auto FVulkanViewport::DestroySwapchain() -> void
	{
		CheckVulkanRHIThread();
		for (const FVulkanView& View : TextureViews)
		{
			Device.GetHandle().destroyImageView(View.ImageView);
		}
		TextureViews.clear();
		DestroyFrameResources(FrameResources);
		if (Swapchain != nullptr)
		{
			const std::vector<vk::Image>& OldImages = Swapchain->GetImages();
			for (uint32 i = 0; i < OldImages.size(); ++i)
			{
				Device.NotifyDeleted_Image(OldImages[i]);
			}

			delete Swapchain;
			Swapchain = nullptr;
		}

		AcquiredBackBufferIndex = -1;
		AcquiredSemaphore = nullptr;
		BackBufferImages.clear();
	}

	auto FVulkanViewport::CanDeferCurrentSwapchainDestruction() const -> bool
	{
		if (Swapchain == nullptr || !Device.SupportsSwapchainMaintenance1()
			|| AcquiredBackBufferIndex >= 0 || AcquiredSemaphore != nullptr)
		{
			return false;
		}
		return std::ranges::none_of(FrameResources, [](const auto& Resource) {
			return Resource.State == EVulkanPresentResourceState::Retired;
		});
	}

	auto FVulkanViewport::RetireCurrentSwapchain() -> void
	{
		check(Swapchain != nullptr);
		FVulkanRetiredSwapchainGeneration& Generation =
			RetiredSwapchainGenerations.emplace_back();
		Generation.Swapchain = std::exchange(Swapchain, nullptr);
		Generation.BackBufferImages = std::move(BackBufferImages);
		Generation.TextureViews = std::move(TextureViews);
		Generation.FrameResources = std::move(FrameResources);
	}

	auto FVulkanViewport::CollectRetiredSwapchains(
		const bool bWaitForCompletion) -> void
	{
		for (auto It = RetiredSwapchainGenerations.begin();
			It != RetiredSwapchainGenerations.end();)
		{
			if (!IsRetiredSwapchainReady(*It, bWaitForCompletion))
			{
				++It;
				continue;
			}
			DestroySwapchainGeneration(*It);
			It = RetiredSwapchainGenerations.erase(It);
		}
	}

	auto FVulkanViewport::IsRetiredSwapchainReady(
		FVulkanRetiredSwapchainGeneration& Generation,
		const bool bWaitForCompletion) -> bool
	{
		for (FVulkanViewportFrameResources& Resource : Generation.FrameResources)
		{
			check(Resource.State != EVulkanPresentResourceState::Retired);
			if (Resource.State != EVulkanPresentResourceState::PresentPending)
			{
				continue;
			}
			if (bWaitForCompletion)
			{
				WaitForFrameResource(Resource);
				continue;
			}
			const vk::Result FenceStatus =
				Device.GetHandle().getFenceStatus(Resource.PresentFence);
			if (FenceStatus == vk::Result::eNotReady)
			{
				return false;
			}
			check(FenceStatus == vk::Result::eSuccess);
			Resource.State = EVulkanPresentResourceState::Available;
		}
		return true;
	}

	auto FVulkanViewport::DestroySwapchainGeneration(
		FVulkanRetiredSwapchainGeneration& Generation) -> void
	{
		for (const FVulkanView& View : Generation.TextureViews)
		{
			Device.GetHandle().destroyImageView(View.ImageView);
		}
		Generation.TextureViews.clear();
		DestroyFrameResources(Generation.FrameResources);
		if (Generation.Swapchain != nullptr)
		{
			for (const vk::Image Image : Generation.BackBufferImages)
			{
				Device.NotifyDeleted_Image(Image);
			}
			delete Generation.Swapchain;
			Generation.Swapchain = nullptr;
		}
		Generation.BackBufferImages.clear();
	}

	auto FVulkanViewport::SetOutputUnavailable() -> void
	{
		CheckVulkanRHIThread();
		if (RHIBackBuffer)
		{
			RHIBackBuffer->InvalidateSwapchain();
		}
		DestroySwapchain();
	}

	auto FVulkanViewport::DestroyFrameResources(
		std::vector<FVulkanViewportFrameResources>& Resources) -> void
	{
		CheckVulkanRHIThread();
		for (FVulkanViewportFrameResources& FrameResource : Resources)
		{
			check(FrameResource.State != EVulkanPresentResourceState::PresentPending);
			FrameResource.RenderingDoneSemaphore->DestroyImmediately();
			delete FrameResource.RenderingDoneSemaphore;
			FrameResource.RenderingDoneSemaphore = nullptr;
			if (FrameResource.PresentFence != VK_NULL_HANDLE)
			{
				Device.GetHandle().destroyFence(FrameResource.PresentFence);
				FrameResource.PresentFence = VK_NULL_HANDLE;
			}
		}
		Resources.clear();
	}

	auto FVulkanViewport::WaitForFrameResource(FVulkanViewportFrameResources& FrameResource) -> void
	{
		CheckVulkanRHIThread();
		if (FrameResource.State == EVulkanPresentResourceState::Available)
		{
			return;
		}
		check(FrameResource.State == EVulkanPresentResourceState::PresentPending);

		const vk::Result Result = Device.GetHandle().waitForFences(FrameResource.PresentFence, vk::True, UINT64_MAX);
		check(Result == vk::Result::eSuccess);
		Device.GetHandle().resetFences(FrameResource.PresentFence);
		FrameResource.State = EVulkanPresentResourceState::Available;
	}

	auto FVulkanViewport::WaitForSwapchainIdle() -> void
	{
		CheckVulkanRHIThread();
		if (!Device.SupportsSwapchainMaintenance1())
		{
			Device.GetGraphicsQueue()->GetHandle().waitIdle();
			if (Device.GetPresentQueue() != Device.GetGraphicsQueue())
			{
				Device.GetPresentQueue()->GetHandle().waitIdle();
			}
			return;
		}

		// Present fences cover both rendering-done semaphore consumption and the
		// presentation engine's access, so unrelated viewport work may continue.
		bool bHasRetiredResources = false;
		for (FVulkanViewportFrameResources& FrameResource : FrameResources)
		{
			if (FrameResource.State == EVulkanPresentResourceState::PresentPending)
			{
				WaitForFrameResource(FrameResource);
			}
			else if (FrameResource.State == EVulkanPresentResourceState::Retired)
			{
				bHasRetiredResources = true;
			}
		}

		// Present fences do not cover an acquire semaphore whose graphics wait was
		// submitted before the frame reached PresentPending. Resize can land in that
		// interval, so drain graphics work before destroying swapchain-owned acquire
		// semaphores. Swapchain recreation is rare and correctness owns this stall.
		Device.GetGraphicsQueue()->GetHandle().waitIdle();

		// A failed-to-enqueue present did not consume its rendering-done semaphore.
		// Drain the queues before destroying those resources during recreation.
		if (bHasRetiredResources)
		{
			if (Device.GetPresentQueue() != Device.GetGraphicsQueue())
			{
				Device.GetPresentQueue()->GetHandle().waitIdle();
			}
		}
	}

	auto FVulkanDynamicRHI::RHICreateViewport(
		const FRHIViewportCreateInfo& CreateInfo) -> TRefCountPtr<FRHIViewport>
	{
		check(IsInGameThread());
		if (!CreateInfo.NativeWindowHandle
			|| CreateInfo.SizeX == 0 || CreateInfo.SizeY == 0)
		{
			DURIN_ERROR("Cannot create a Vulkan viewport from an invalid presentation target or zero extent.");
			return {};
		}
		vk::SurfaceKHR PresentationSurface = VK_NULL_HANDLE;
		if (CreateInfo.bAdoptInitializationPresentationCandidate)
		{
			if (!InitializationPresentationCandidate)
			{
				DURIN_ERROR("Vulkan startup presentation candidate adoption was requested after no candidate was available.");
				return {};
			}
			PresentationSurface =
				InitializationPresentationCandidate->TakeForNativeWindow(
					CreateInfo.NativeWindowHandle);
			if (!PresentationSurface)
			{
				DURIN_ERROR("Vulkan startup presentation candidate is mismatched or already consumed.");
				return {};
			}
		}
		TRefCountPtr<FRHIViewport> Result;
		if (GRHIThread)
		{
			GCommandListExecutor.ExecuteSynchronousOperation(false,
				[this, CreateInfo,
					 PresentationSurface, &Result]() {
					Result = MakeRefCount<FVulkanViewport>(
						*Device, CreateInfo.NativeWindowHandle,
						CreateInfo.SizeX, CreateInfo.SizeY,
						CreateInfo.bIsFullscreen,
						CreateInfo.PreferredPixelFormat,
						CreateInfo.PresentationPolicy,
						PresentationSurface);
				});
			return Result;
		}
		return MakeRefCount<FVulkanViewport>(*Device,
			CreateInfo.NativeWindowHandle,
			CreateInfo.SizeX, CreateInfo.SizeY, CreateInfo.bIsFullscreen,
			CreateInfo.PreferredPixelFormat, CreateInfo.PresentationPolicy,
			PresentationSurface);
	}

	auto FVulkanDynamicRHI::RHIResizeViewport(FRHIViewport* InViewport, uint32 InSizeX, uint32 InSizeY, bool bInIsFullscreen) -> void
	{
		check(IsInGameThread());
		if (InSizeX == 0 || InSizeY == 0)
		{
			return;
		}

		TRefCountPtr<FRHIViewport> Viewport = InViewport;
		ENQUEUE_RENDER_COMMAND(ResizeViewport)(
			[Viewport = std::move(Viewport), InSizeX, InSizeY,
			 bInIsFullscreen](FRHICommandListImmediate& RHICmdList)
			{
				auto* VulkanViewport = static_cast<FVulkanViewport*>(
					Viewport.GetReference());
				VulkanViewport->Resize(
					RHICmdList, InSizeX, InSizeY, bInIsFullscreen);
			});
	}

	auto FVulkanDynamicRHI::RHIGetViewportBackBuffer(FRHIViewport* ViewportRHI) -> TRefCountPtr<FRHITexture>
	{
		check(IsInRenderingThread());
		return ViewportRHI->GetBackBuffer(FRHICommandListImmediate::Get());
	}

}
