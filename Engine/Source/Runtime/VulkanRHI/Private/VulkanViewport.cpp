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

namespace Durin::VulkanRHI
{
	FVulkanBackBuffer::FVulkanBackBuffer(FVulkanDevice& InDevice, FVulkanViewport* InViewport)
		: FVulkanTexture(InDevice, nullptr)
		, Viewport(InViewport)
	{
		CheckVulkanRHIThread();
		UpdateSwapchain();
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

	auto FVulkanBackBuffer::AcquireBackBufferImage(
		FVulkanCommandListContext& Context) -> void
	{
		CheckVulkanRHIThread();
		Viewport->AcquireBackBufferImage();
		check(Viewport->AcquiredBackBufferIndex >= 0 && Viewport->AcquiredBackBufferIndex < static_cast<int32>(Viewport->TextureViews.size()));
		const FVulkanView& View = Viewport->TextureViews[Viewport->AcquiredBackBufferIndex];
		Image = View.Image;
		if (Viewport->AcquiredSemaphore != nullptr)
		{
			Context.AddWaitSemaphore(vk::PipelineStageFlagBits::eColorAttachmentOutput, Viewport->AcquiredSemaphore);
		}
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
		CreateSwapchain();
	}

	FVulkanViewport::~FVulkanViewport()
	{
		CheckVulkanRHIThread();
		WaitForSwapchainIdle();

		for (uint32 i = 0; i < TextureViews.size(); ++i)
		{
			Device.GetHandle().destroyImageView(TextureViews[i].ImageView);
		}
		TextureViews.clear();

		DestroyFrameResources();

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
	}

	auto FVulkanViewport::PrepareSwapchain() -> void
	{
		CheckVulkanRHIThread();
		if (Swapchain != nullptr && Swapchain->NeedsRecreate())
		{
			bSwapchainNeedsRecreate = true;
		}

		if (!bSwapchainNeedsRecreate && !bHasPendingResize)
		{
			return;
		}

		if (bHasPendingResize)
		{
			SizeX = PendingSizeX;
			SizeY = PendingSizeY;
			PendingSizeX = 0;
			PendingSizeY = 0;
			bHasPendingResize = false;
		}

		if (SizeX == 0 || SizeY == 0)
		{
			return;
		}

		RecreateSwapchain();
		bSwapchainNeedsRecreate = false;
	}

	auto FVulkanViewport::MarkSwapchainNeedsRecreate() -> void
	{
		CheckVulkanRHIThread();
		bSwapchainNeedsRecreate = true;
	}

	auto FVulkanViewport::RecreateSwapchain() -> void
	{
		CheckVulkanRHIThread();
		AcquiredBackBufferIndex = -1;
		AcquiredSemaphore = nullptr;
		WaitForSwapchainIdle();

		// Detach old swapchain handle — it must stay alive until the new swapchain
		// is created with it as VkSwapchainCreateInfoKHR::oldSwapchain for a smooth transition.
		vk::SwapchainKHR OldSwapchainHandle = VK_NULL_HANDLE;
		if (Swapchain != nullptr)
		{
			OldSwapchainHandle = Swapchain->DetachSwapchain();
		}

		// Destroy old per-frame resources (images, views, etc.) but NOT the VkSwapchainKHR
		DestroySwapchain();

		// Create new swapchain referencing the old one
		CreateSwapchain(OldSwapchainHandle);

		// Now the old swapchain can be safely destroyed — the new one has taken over
		if (OldSwapchainHandle != VK_NULL_HANDLE)
		{
			Device.GetHandle().destroySwapchainKHR(OldSwapchainHandle);
		}
	}

	auto FVulkanViewport::AcquireBackBufferImage() -> FVulkanView&
	{
		CheckVulkanRHIThread();
		AcquiredBackBufferIndex = static_cast<int32>(
			Swapchain->AcquireImageIndex(&AcquiredSemaphore));
		if (AcquiredBackBufferIndex < 0)
		{
			MarkSwapchainNeedsRecreate();
			PrepareSwapchain();
			AcquiredBackBufferIndex = static_cast<int32>(
				Swapchain->AcquireImageIndex(&AcquiredSemaphore));
		}
		check(AcquiredBackBufferIndex >= 0 && AcquiredBackBufferIndex < static_cast<int32>(TextureViews.size()));
		return TextureViews[AcquiredBackBufferIndex];
	}

	auto FVulkanViewport::GetBackBuffer(FRHICommandListImmediate& InRHICmdList) -> TRefCountPtr<FRHITexture>
	{
		check(IsInRenderingThread());
		InRHICmdList.AcquireBackBuffer(RHIBackBuffer.GetReference());
		return RHIBackBuffer;
	}

	auto FVulkanViewport::Present(FVulkanCommandListContext& InContext, FVulkanCommandBuffer& InCmdBuffer, FVulkanQueue& InPresentQueue, bool bInLockToVsync) -> bool
	{
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
		return PresentOutcome.bPresented;
	}

	auto FVulkanViewport::GetFormat() const -> EPixelFormat
	{
		return PixelFormat;
	}

	auto FVulkanViewport::GetSwapchainImageFormat() const -> vk::Format
	{
		return Swapchain->GetFormat();
	}

	auto FVulkanViewport::InitImages(const std::vector<vk::Image>& InImages) -> void
	{
		CheckVulkanRHIThread();
		BackBufferImages.clear();
		BackBufferImages.resize(InImages.size());

		for (const auto& TextureView : TextureViews)
		{
			Device.GetHandle().destroyImageView(TextureView.ImageView);
		}
		TextureViews.clear();

		vk::ImageViewCreateInfo ImageViewCreateInfo;
		ImageViewCreateInfo.setViewType(vk::ImageViewType::e2D);
		ImageViewCreateInfo.setFormat(GetSwapchainImageFormat());
		ImageViewCreateInfo.setComponents({vk::ComponentSwizzle::eR, vk::ComponentSwizzle::eG, vk::ComponentSwizzle::eB, vk::ComponentSwizzle::eA});
		ImageViewCreateInfo.setSubresourceRange({vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1});

		for (uint32 i = 0; i < InImages.size(); ++i)
		{
			BackBufferImages[i] = InImages[i];
			ImageViewCreateInfo.setImage(InImages[i]);
			vk::ImageView View = Device.GetHandle().createImageView(ImageViewCreateInfo);
			TextureViews.emplace_back(InImages[i], View);
		}
	}

	auto FVulkanViewport::CreateSwapchain(vk::SwapchainKHR InOldSwapchain) -> void
	{
		CheckVulkanRHIThread();
		Swapchain = new FVulkanSwapchain(Device, Surface, SizeX, SizeY, bIsFullScreen, PresentModePolicy, InOldSwapchain);
		const vk::Extent2D SwapchainExtent = Swapchain->GetExtent();
		SizeX = SwapchainExtent.width;
		SizeY = SwapchainExtent.height;

		const std::vector<vk::Image>& SwapchainImages = Swapchain->GetImages();
		InitImages(SwapchainImages);

		RecreateFrameResources(static_cast<uint32>(SwapchainImages.size()));

		if (RHIBackBuffer)
		{
			RHIBackBuffer->UpdateSwapchain();
		}
		else
		{
			RHIBackBuffer = MakeRefCount<FVulkanBackBuffer>(Device, this);
		}
		AcquiredBackBufferIndex = -1;
	}

	auto FVulkanViewport::DestroySwapchain() -> void
	{
		CheckVulkanRHIThread();
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
	}

	auto FVulkanViewport::RecreateFrameResources(uint32 NumSwapchainImages) -> void
	{
		CheckVulkanRHIThread();
		DestroyFrameResources();

		FrameResources.resize(NumSwapchainImages);
		for (uint32 i = 0; i < NumSwapchainImages; ++i)
		{
			FrameResources[i].RenderingDoneSemaphore = new FVulkanSemaphore(Device);
			if (Device.SupportsSwapchainMaintenance1())
			{
				FrameResources[i].PresentFence = Device.GetHandle().createFence(vk::FenceCreateInfo());
			}
		}
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
