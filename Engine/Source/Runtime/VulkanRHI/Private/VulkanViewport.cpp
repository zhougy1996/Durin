#include "VulkanViewport.h"

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
		const vk::Extent2D Extent = Viewport->GetSwapchain()->GetExtent();
		SizeX = Extent.width;
		SizeY = Extent.height;
		Format = Viewport->GetSwapchainImageFormat();
		PixelFormat = Viewport->GetFormat();
		NumSamples = 1;
	}

	auto FVulkanBackBuffer::InvalidateSwapchain() -> void
	{
		CheckVulkanRHIThread();
		Image = VK_NULL_HANDLE;
	}

	auto FVulkanBackBuffer::AcquireBackBufferImage(
		FVulkanCommandListContext& Context) -> bool
	{
		CheckVulkanRHIThread();
		const FVulkanView* View = Viewport->AcquireBackBufferImage();
		if (View == nullptr)
		{
			Image = VK_NULL_HANDLE;
			return false;
		}
		Image = View->Image;
		if (Viewport->AcquiredSemaphore != nullptr)
		{
			Context.AddWaitSemaphore(vk::PipelineStageFlagBits::eColorAttachmentOutput, Viewport->AcquiredSemaphore);
		}
		return true;
	}

	FVulkanViewport::FVulkanViewport(FVulkanDevice& InDevice, void* InWindowHandle, uint32 InSizeX, uint32 InSizeY, bool bInIsFullScreen, EPixelFormat InPreferredPixelFormat, EViewportPresentModePolicy InPresentModePolicy)
		: Device(InDevice)
		, SizeX(InSizeX)
		, SizeY(InSizeY)
		, bIsFullScreen(bInIsFullScreen)
		, NativeWindowHandle(InWindowHandle)
	 	, PixelFormat(InPreferredPixelFormat)
		, PresentModePolicy(InPresentModePolicy)
	{
		CheckVulkanRHIThread();
		Surface = FVulkanGenericPlatform::CreateSurface(InWindowHandle, FVulkanDynamicRHI::Get().RHIGetVkInstance());
		bSwapchainNeedsRecreate = true;
		bSwapchainRetryEligible = true;
		PrepareSwapchain();
		if (!RHIBackBuffer)
		{
			RHIBackBuffer = MakeRefCount<FVulkanBackBuffer>(Device, this);
		}
	}

	FVulkanViewport::~FVulkanViewport()
	{
		CheckVulkanRHIThread();
		WaitForSwapchainIdle();
		DestroySwapchain();

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
		if (AcquiredBackBufferIndex < 0)
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
		CheckVulkanRHIThread();
		AcquiredBackBufferIndex = -1;
		AcquiredSemaphore = nullptr;
		WaitForSwapchainIdle();

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
				PresentModePolicy, OldSwapchain, bNativeSwapchainCreated);
			CandidateSwapchain->InitializeSynchronizationResources();
			CandidateImages = CandidateSwapchain->GetImages();

			vk::ImageViewCreateInfo ImageViewCreateInfo;
			ImageViewCreateInfo.setViewType(vk::ImageViewType::e2D);
			ImageViewCreateInfo.setFormat(CandidateSwapchain->GetFormat());
			ImageViewCreateInfo.setComponents({vk::ComponentSwizzle::eR, vk::ComponentSwizzle::eG, vk::ComponentSwizzle::eB, vk::ComponentSwizzle::eA});
			ImageViewCreateInfo.setSubresourceRange({vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1});
			for (const vk::Image Image : CandidateImages)
			{
#if DURIN_VULKAN_TEST_FAILURE_INJECTION
				ThrowIfVulkanNativeCreateFailureIsArmed(EVulkanCreateFailurePoint::SwapchainImageView);
#endif
				ImageViewCreateInfo.setImage(Image);
				CandidateViews.emplace_back(Image, Device.GetHandle().createImageView(ImageViewCreateInfo));
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
					PresentModePolicy == EViewportPresentModePolicy::ImGuiDetachedViewport ? "ImGuiDetachedViewport" : "MainWindow",
					bNativeSwapchainCreated, Error.what());
				bSwapchainFailureReported = true;
			}
			if (bNativeSwapchainCreated)
			{
				SetOutputUnavailable();
			}
			return false;
		}
		catch (...)
		{
			DestroyCandidateResources();
			if (bNativeSwapchainCreated)
			{
				SetOutputUnavailable();
			}
			throw;
		}

		DestroySwapchain();
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
		DestroyFrameResources();
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

	auto FVulkanViewport::SetOutputUnavailable() -> void
	{
		CheckVulkanRHIThread();
		if (RHIBackBuffer)
		{
			RHIBackBuffer->InvalidateSwapchain();
		}
		DestroySwapchain();
	}

	auto FVulkanViewport::DestroyFrameResources() -> void
	{
		CheckVulkanRHIThread();
		for (FVulkanViewportFrameResources& FrameResource : FrameResources)
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
		FrameResources.clear();
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

		// A failed-to-enqueue present did not consume its rendering-done semaphore.
		// Drain the queues before destroying those resources during recreation.
		if (bHasRetiredResources)
		{
			Device.GetGraphicsQueue()->GetHandle().waitIdle();
			if (Device.GetPresentQueue() != Device.GetGraphicsQueue())
			{
				Device.GetPresentQueue()->GetHandle().waitIdle();
			}
		}
	}

	auto FVulkanDynamicRHI::RHICreateViewport(void* WindowHandle, uint32 SizeX, uint32 SizeY, bool bIsFullscreen, EPixelFormat PreferredPixelFormat, EViewportPresentModePolicy InPresentModePolicy) const -> TRefCountPtr<FRHIViewport>
	{
		check(IsInGameThread());
		TRefCountPtr<FRHIViewport> Result;
		if (GRHIThread)
		{
			GCommandListExecutor.ExecuteSynchronousOperation(false,
				[this, WindowHandle, SizeX, SizeY, bIsFullscreen,
				 PreferredPixelFormat, InPresentModePolicy, &Result]() {
					Result = MakeRefCount<FVulkanViewport>(
						*Device, WindowHandle, SizeX, SizeY, bIsFullscreen,
						PreferredPixelFormat, InPresentModePolicy);
				});
			return Result;
		}
		return MakeRefCount<FVulkanViewport>(*Device, WindowHandle, SizeX, SizeY,
			bIsFullscreen, PreferredPixelFormat, InPresentModePolicy);
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
