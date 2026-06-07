#include "VulkanViewport.h"

#include "RHICommandList.h"
#include "VulkanCommon.h"
#include "VulkanDynamicRHI.h"
#include "VulkanDevice.h"
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
		FIntPoint Extent =  InViewport->GetSizeXY();
		SizeX = Extent.x;
		SizeY = Extent.y;
		Format = Viewport->GetSwapchainImageFormat();
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

	FVulkanViewport::FVulkanViewport(FVulkanDevice& InDevice, void* InWindowHandle, uint32 InSizeX, uint32 InSizeY, bool bInIsFullScreen, EPixelFormat InPreferredPixelFormat)
		: Device(InDevice)
		, SizeX(InSizeX)
		, SizeY(InSizeY)
		, bIsFullScreen(bInIsFullScreen)
		, NativeWindowHandle(InWindowHandle)
	 	, PixelFormat(InPreferredPixelFormat)
	{
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

		for(auto & RenderingDoneSemaphore : RenderingDoneSemaphores)
		{
			delete RenderingDoneSemaphore;
		}
		RenderingDoneSemaphores.clear();

		Device.GetDeferredDeletionQueue().ReleaseResources(true);
		DestroySwapchain();
		Device.GetDeferredDeletionQueue().ReleaseResources(true);
	}

	auto FVulkanViewport::GetWindowHandle() -> void*
	{
		return NativeWindowHandle;
	}

	auto FVulkanViewport::Resize(FRHICommandListImmediate& RHICmdList, uint32 InSizeX, uint32 InSizeY) -> void
	{
		check(IsInRenderingThread());
		SizeX = InSizeX;
		SizeY = InSizeY;

		RecreateSwapchainFromRT(RHICmdList);
	}

	auto FVulkanViewport::RecreateSwapchainFromRT(FRHICommandListImmediate& RHICmdList) -> void
	{
		check(IsInRenderingThread());
		DestroySwapchain();
		CreateSwapchain();
	}

	auto FVulkanViewport::AcquireBackBufferImage() -> FVulkanView&
	{
		AcquiredBackBufferIndex = static_cast<int32>(Swapchain->AcquireImageIndex(&AcquiredSemaphore));
		check(AcquiredBackBufferIndex >= 0 && AcquiredBackBufferIndex < static_cast<int32>(TextureViews.size()));
		return TextureViews[AcquiredBackBufferIndex];
	}

	auto FVulkanViewport::GetBackBuffer(FRHICommandListImmediate& InRHICmdList) -> TRefCountPtr<FRHITexture>
	{
		if (AcquiredBackBufferIndex >= 0)
		{
			check(AcquiredBackBufferIndex < static_cast<int32>(TextureViews.size()));
			return RHIBackBuffer;
		}

		if (Swapchain->NeedsRecreate())
		{
			RecreateSwapchainFromRT(InRHICmdList);
		}

		const uint32 AcquiredImageIndex = Swapchain->AcquireImageIndex(&AcquiredSemaphore);
		if (AcquiredImageIndex == INDEX_NONE_U32)
		{
			RecreateSwapchainFromRT(InRHICmdList);
			const uint32 RetryImageIndex = Swapchain->AcquireImageIndex(&AcquiredSemaphore);
			if (RetryImageIndex == INDEX_NONE_U32)
			{
				AcquiredBackBufferIndex = -1;
				AcquiredSemaphore = nullptr;
				return nullptr;
			}
			AcquiredBackBufferIndex = static_cast<int32>(RetryImageIndex);
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
		if (AcquiredBackBufferIndex < 0 || AcquiredBackBufferIndex >= static_cast<int32>(RenderingDoneSemaphores.size()))
		{
			return false;
		}

		InContext.AddSignalSemaphore(RenderingDoneSemaphores[AcquiredBackBufferIndex]);
		InContext.Finalize();
		const bool bPresented = Swapchain->Present(&InPresentQueue, RenderingDoneSemaphores[AcquiredBackBufferIndex]);
		const bool bNeedsRecreate = Swapchain->NeedsRecreate();
		if (!bPresented || bNeedsRecreate)
		{
			RecreateSwapchainFromRT(FRHICommandListImmediate::Get());
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

	auto FVulkanViewport::CreateSwapchain() -> void
	{
		// Release old swapchain resources
		RHIBackBuffer = nullptr;

		Swapchain = new FVulkanSwapchain(FVulkanDynamicRHI::Get().RHIGetVkInstance(), Device, NativeWindowHandle, SizeX, SizeY, bIsFullScreen);

		const std::vector<vk::Image>& SwapchainImages = Swapchain->GetImages();
		InitImages(SwapchainImages);

		RecreateRenderingDoneSemaphores(static_cast<uint32>(SwapchainImages.size()));

		RHIBackBuffer = MakeRefCount<FVulkanBackBuffer>(Device, this);
		AcquiredBackBufferIndex = -1;
	}

	auto FVulkanViewport::DestroySwapchain() -> void
	{
		GDynamicRHI->RHIBlockUntilGPUIdle();

		const std::vector<vk::Image>& SwapchainImages = Swapchain->GetImages();
		for (uint32 i = 0; i < SwapchainImages.size(); ++i)
		{
			Device.NotifyDeleted_Image(SwapchainImages[i]);
		}

		delete Swapchain;
		Swapchain = nullptr;

		AcquiredBackBufferIndex = -1;
	}

	auto FVulkanViewport::RecreateRenderingDoneSemaphores(uint32 NumSwapchainImages) -> void
	{
		if (RenderingDoneSemaphores.size() == NumSwapchainImages)
		{
			return;
		}

		for (FVulkanSemaphore* RenderingDoneSemaphore : RenderingDoneSemaphores)
		{
			delete RenderingDoneSemaphore;
		}
		RenderingDoneSemaphores.clear();

		RenderingDoneSemaphores.resize(NumSwapchainImages);
		for (uint32 i = 0; i < NumSwapchainImages; ++i)
		{
			RenderingDoneSemaphores[i] = new FVulkanSemaphore(Device);
		}
	}

	auto FVulkanDynamicRHI::RHICreateViewport(void* WindowHandle, uint32 SizeX, uint32 SizeY, bool bIsFullscreen, EPixelFormat PreferredPixelFormat) const -> TRefCountPtr<FRHIViewport>
	{
		check(IsInGameThread());
		return MakeRefCount<FVulkanViewport>(*Device, WindowHandle, SizeX, SizeY, bIsFullscreen, PreferredPixelFormat);
	}

	auto FVulkanDynamicRHI::RHIResizeViewport(FRHIViewport* InViewport, uint32 InSizeX, uint32 InSizeY, bool bInIsFullscreen) -> void
	{
		check(IsInGameThread());
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

			FlushRenderingCommands();
		}
	}

	auto FVulkanDynamicRHI::RHIGetViewportBackBuffer(FRHIViewport* ViewportRHI) -> TRefCountPtr<FRHITexture>
	{
		return ViewportRHI->GetBackBuffer(FRHICommandListImmediate::Get());
	}

}
