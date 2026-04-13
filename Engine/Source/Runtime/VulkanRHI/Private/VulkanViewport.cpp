#include "VulkanViewport.h"

#include "RHICommandList.h"
#include "VulkanCommon.h"
#include "VulkanDynamicRHI.h"
#include "VulkanDevice.h"
#include "VulkanView.h"
#include "VulkanCommandBuffer.h"
#include "VulkanContext.h"
#include "VulkanSwapchain.h"
#include "VulkanQueue.h"
#include "Threading/RunnableThread.h"
#include "RenderingThread.h"

namespace Doge::VulkanRHI
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
		const FVulkanTextureView& View = Viewport->AcquireBackBufferImage();
		Image = View.Image;
		Context.AddWaitSemaphore(Viewport->AcquiredSemaphore);
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

	auto FVulkanViewport::AcquireBackBufferImage() -> FVulkanTextureView&
	{
		AcquiredBackBufferIndex = static_cast<int32>(Swapchain->AcquireImageIndex(&AcquiredSemaphore));
		return TextureViews[AcquiredBackBufferIndex];
	}

	auto FVulkanViewport::GetBackBuffer(FRHICommandListImmediate& InRHICmdList) -> TRefCountPtr<FRHITexture>
	{
		RHIBackBuffer->AcquireBackBufferImage(static_cast<FVulkanCommandListContext&>(InRHICmdList.GetContext()));
		return RHIBackBuffer;
	}

	auto FVulkanViewport::Present(FVulkanCommandListContext& InContext, FVulkanCommandBuffer& InCmdBuffer, FVulkanQueue& InPresentQueue, bool bInLockToVsync) -> bool
	{
		InContext.AddSignalSemaphore(RenderingDoneSemaphores[AcquiredBackBufferIndex]);
		InContext.Finalize();
		Swapchain->Present(&InPresentQueue, RenderingDoneSemaphores[AcquiredBackBufferIndex]);
		return true;
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

		// Create semaphores for each swapchain image if they haven't been created yet.
		const bool bCreateSemaphores = RenderingDoneSemaphores.empty();
		check(bCreateSemaphores || RenderingDoneSemaphores.size() == SwapchainImages.size());  // RenderingDoneSemaphores should be either empty or have the same number of semaphores as swapchain images
		if (bCreateSemaphores)
		{
			RenderingDoneSemaphores.resize(SwapchainImages.size());
			for (uint32 i = 0; i < SwapchainImages.size(); ++i)
			{
				RenderingDoneSemaphores[i] = new FVulkanSemaphore(Device);
			}
		}

		RHIBackBuffer = MakeRefCount<FVulkanBackBuffer>(Device, this);
		AcquiredBackBufferIndex = -1;
	}

	auto FVulkanViewport::DestroySwapchain() -> void
	{
		GDynamicRHI->RHIBlockUntilGPUIdle();

		delete Swapchain;
		Swapchain = nullptr;

		AcquiredBackBufferIndex = -1;
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