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
#include "Threading/RunnableThread.h"
#include "RenderingThread.h"

namespace Durin::VulkanRHI
{
	FVulkanBackBuffer::FVulkanBackBuffer(FVulkanDevice& InDevice, FVulkanViewport* InViewport)
		: FVulkanTexture(InDevice, nullptr)
		, Viewport(InViewport)
	{
		const vk::Extent2D Extent = Viewport->GetSwapchain()->GetExtent();
		SizeX = Extent.width;
		SizeY = Extent.height;
		Format = Viewport->GetSwapchainImageFormat();
		PixelFormat = Viewport->GetFormat();
		NumSamples = 1;
	}

	auto FVulkanBackBuffer::AcquireBackBufferImage(FVulkanCommandListContext& Context)
	{
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
		Surface = FVulkanGenericPlatform::CreateSurface(InWindowHandle, FVulkanDynamicRHI::Get().RHIGetVkInstance());
		CreateSwapchain();
	}

	FVulkanViewport::~FVulkanViewport()
	{
		Device.WaitUtilIdle();

		for (uint32 i = 0; i < TextureViews.size(); ++i)
		{
			Device.GetDeferredDeletionQueue().EnqueueResource(FDeferredDeletionQueue::EType::ImageView, TextureViews[i].ImageView);
		}
		TextureViews.clear();

		DestroyFrameResources();

		Device.GetDeferredDeletionQueue().ReleaseResources(true);
		DestroySwapchain();
		Device.GetDeferredDeletionQueue().ReleaseResources(true);

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

	auto FVulkanViewport::Resize(FRHICommandListImmediate& RHICmdList, uint32 InSizeX, uint32 InSizeY) -> void
	{
		check(IsInRenderingThread());
		RequestResize(InSizeX, InSizeY);
	}

	auto FVulkanViewport::BeginDrawing(FRHICommandListImmediate& RHICmdList) -> void
	{
		check(IsInRenderingThread());
		PrepareSwapchain(RHICmdList);
	}

	auto FVulkanViewport::RequestResize(uint32 InSizeX, uint32 InSizeY) -> void
	{
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

	auto FVulkanViewport::PrepareSwapchain(FRHICommandListImmediate& RHICmdList) -> void
	{
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

		RecreateSwapchainFromRT(RHICmdList);
		bSwapchainNeedsRecreate = false;
	}

	auto FVulkanViewport::MarkSwapchainNeedsRecreate() -> void
	{
		bSwapchainNeedsRecreate = true;
	}

	auto FVulkanViewport::RecreateSwapchainFromRT(FRHICommandListImmediate& RHICmdList) -> void
	{
		check(IsInRenderingThread());
		AcquiredBackBufferIndex = -1;
		AcquiredSemaphore = nullptr;
		Device.GetGraphicsQueue()->GetHandle().waitIdle();
		if (Device.GetPresentQueue() != Device.GetGraphicsQueue())
		{
			Device.GetPresentQueue()->GetHandle().waitIdle();
		}

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
		AcquiredBackBufferIndex = static_cast<int32>(Swapchain->AcquireImageIndex(&AcquiredSemaphore));
		check(AcquiredBackBufferIndex >= 0 && AcquiredBackBufferIndex < static_cast<int32>(TextureViews.size()));
		return TextureViews[AcquiredBackBufferIndex];
	}

	auto FVulkanViewport::GetBackBuffer(FRHICommandListImmediate& InRHICmdList) -> TRefCountPtr<FRHITexture>
	{
		PrepareSwapchain(InRHICmdList);

		if (AcquiredBackBufferIndex >= 0)
		{
			check(AcquiredBackBufferIndex < static_cast<int32>(TextureViews.size()));
			return RHIBackBuffer;
		}

		if (Swapchain->NeedsRecreate())
		{
			MarkSwapchainNeedsRecreate();
			return nullptr;
		}

		const uint32 AcquiredImageIndex = Swapchain->AcquireImageIndex(&AcquiredSemaphore);
		if (AcquiredImageIndex == INDEX_NONE_U32)
		{
			MarkSwapchainNeedsRecreate();
			AcquiredBackBufferIndex = -1;
			AcquiredSemaphore = nullptr;
			return nullptr;
		}
		else
		{
			AcquiredBackBufferIndex = static_cast<int32>(AcquiredImageIndex);
		}

		RHIBackBuffer->AcquireBackBufferImage(static_cast<FVulkanCommandListContext&>(InRHICmdList.GetContext()));
		return RHIBackBuffer;
	}

	auto FVulkanViewport::Present(FVulkanCommandListContext& InContext, FVulkanCommandBuffer& InCmdBuffer, FVulkanQueue& InPresentQueue, bool bInLockToVsync) -> bool
	{
		if (AcquiredBackBufferIndex < 0 || AcquiredBackBufferIndex >= static_cast<int32>(FrameResources.size()))
		{
			return false;
		}

		FVulkanSemaphore* RenderingDoneSemaphore = FrameResources[AcquiredBackBufferIndex].RenderingDoneSemaphore;
		check(RenderingDoneSemaphore != nullptr);
		InContext.AddSignalSemaphore(RenderingDoneSemaphore);
		InContext.Finalize();
		const bool bPresented = Swapchain->Present(&InPresentQueue, RenderingDoneSemaphore);
		const bool bNeedsRecreate = Swapchain->NeedsRecreate();
		if (!bPresented || bNeedsRecreate)
		{
			MarkSwapchainNeedsRecreate();
		}
		AcquiredBackBufferIndex = -1;
		AcquiredSemaphore = nullptr;
		return bPresented;
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
		BackBufferImages.clear();
		BackBufferImages.resize(InImages.size());

		for (const auto& TextureView : TextureViews)
		{
			Device.GetDeferredDeletionQueue().EnqueueResource(FDeferredDeletionQueue::EType::ImageView, TextureView.ImageView);
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
		// Release old swapchain resources
		RHIBackBuffer = nullptr;

		Swapchain = new FVulkanSwapchain(Device, Surface, SizeX, SizeY, bIsFullScreen, PresentModePolicy, InOldSwapchain);
		const vk::Extent2D SwapchainExtent = Swapchain->GetExtent();
		SizeX = SwapchainExtent.width;
		SizeY = SwapchainExtent.height;

		const std::vector<vk::Image>& SwapchainImages = Swapchain->GetImages();
		InitImages(SwapchainImages);

		RecreateFrameResources(static_cast<uint32>(SwapchainImages.size()));

		RHIBackBuffer = MakeRefCount<FVulkanBackBuffer>(Device, this);
		AcquiredBackBufferIndex = -1;
	}

	auto FVulkanViewport::DestroySwapchain() -> void
	{
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
		if (FrameResources.size() == NumSwapchainImages)
		{
			return;
		}

		DestroyFrameResources();

		FrameResources.resize(NumSwapchainImages);
		for (uint32 i = 0; i < NumSwapchainImages; ++i)
		{
			FrameResources[i].RenderingDoneSemaphore = new FVulkanSemaphore(Device);
		}
	}

	auto FVulkanViewport::DestroyFrameResources() -> void
	{
		for (FVulkanViewportFrameResources& FrameResource : FrameResources)
		{
			delete FrameResource.RenderingDoneSemaphore;
			FrameResource.RenderingDoneSemaphore = nullptr;
		}
		FrameResources.clear();
	}

	auto FVulkanDynamicRHI::RHICreateViewport(void* WindowHandle, uint32 SizeX, uint32 SizeY, bool bIsFullscreen, EPixelFormat PreferredPixelFormat, EViewportPresentModePolicy InPresentModePolicy) const -> TRefCountPtr<FRHIViewport>
	{
		check(IsInGameThread());
		return MakeRefCount<FVulkanViewport>(*Device, WindowHandle, SizeX, SizeY, bIsFullscreen, PreferredPixelFormat, InPresentModePolicy);
	}

	auto FVulkanDynamicRHI::RHIResizeViewport(FRHIViewport* InViewport, uint32 InSizeX, uint32 InSizeY, bool bInIsFullscreen) -> void
	{
		check(IsInGameThread());
		if (InSizeX == 0 || InSizeY == 0)
		{
			return;
		}

		FVulkanViewport* VulkanViewport = static_cast<FVulkanViewport*>(InViewport);
		const FIntPoint OldSize = VulkanViewport->GetSizeXY();
		const FIntPoint NewSize = {InSizeX, InSizeY};
		if (OldSize != NewSize)
		{
			ENQUEUE_RENDER_COMMAND(ResizeViewport)(
				[VulkanViewport, InSizeX, InSizeY, bInIsFullscreen](FRHICommandListImmediate& RHICmdList)
				{
					VulkanViewport->Resize(RHICmdList, InSizeX, InSizeY);
				});
		}
	}

	auto FVulkanDynamicRHI::RHIGetViewportBackBuffer(FRHIViewport* ViewportRHI) -> TRefCountPtr<FRHITexture>
	{
		return ViewportRHI->GetBackBuffer(FRHICommandListImmediate::Get());
	}

}
