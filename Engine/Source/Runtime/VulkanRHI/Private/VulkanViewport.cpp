#include "VulkanViewport.h"

#include "RHICommandList.h"
#include "VulkanCommon.h"
#include "VulkanDynamicRHI.h"
#include "VulkanDevice.h"
#include "VulkanView.h"
#include "VulkanCommandBuffer.h"
#include "VulkanContext.h"
#include "VulkanSwapChain.h"
#include "VulkanQueue.h"

namespace Doge::VulkanRHI
{
	FVulkanBackBuffer::FVulkanBackBuffer(FVulkanDevice& InDevice, FVulkanViewport* InViewport)
		: FVulkanTexture(InDevice, nullptr)
		, Viewport(InViewport)
	{
		FIntPoint Extent =  InViewport->GetSizeXY();
		SizeX = Extent.x;
		SizeY = Extent.y;
		Format = Viewport->GetVkFormat();
	}

	auto FVulkanBackBuffer::AcquireBackBufferImage(FVulkanCommandListContext& Context)
	{
		const FVulkanTextureView& View = Viewport->AcquireBackBufferImage();
		Image = View.Image;

		FVulkanCommandBufferManager* CmdBufferManager = Context.GetCommandBufferManager();
		FVulkanCommandBuffer* CmdBuffer = CmdBufferManager->GetActiveCommandBuffer();
		CmdBuffer->AddWaitSemaphore(Viewport->AcquiredSemaphore);
	}

	FVulkanViewport::FVulkanViewport(FVulkanDevice& InDevice, void* InWindowHandle, uint32 InSizeX, uint32 InSizeY, bool bInIsFullScreen, EPixelFormat InPreferredPixelFormat)
		: Device(InDevice)
		, SizeX(InSizeX)
		, SizeY(InSizeY)
		, bIsFullScreen(bInIsFullScreen)
		, NativeWindowHandle(InWindowHandle)
	{
		SwapChain = new FVulkanSwapChain(FVulkanDynamicRHI::Get().RHIGetVkInstance(), InDevice, InWindowHandle, InSizeX, InSizeY, bInIsFullScreen);
		vk::Format VkImageFormat = SwapChain->GetFormat();
		ImageFormat = FVulkanPixelFormat::ToPixelFormat(VkImageFormat);

		const std::vector<vk::Image>& Images = SwapChain->GetImages();

		vk::ImageViewCreateInfo ImageViewCreateInfo;
		ImageViewCreateInfo.setViewType(vk::ImageViewType::e2D);
		ImageViewCreateInfo.setFormat(VkImageFormat);
		ImageViewCreateInfo.setComponents({vk::ComponentSwizzle::eR, vk::ComponentSwizzle::eG, vk::ComponentSwizzle::eB, vk::ComponentSwizzle::eA});
		ImageViewCreateInfo.setSubresourceRange({vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1});

		BackBufferImages.resize(Images.size());
		for (uint32 i = 0; i < Images.size(); ++i)
		{
			BackBufferImages[i] = Images[i];
			ImageViewCreateInfo.setImage(Images[i]);
			vk::ImageView View = Device.GetHandle().createImageView(ImageViewCreateInfo);
			TextureViews.emplace_back(Images[i], View);
		}

		RenderingDoneSemaphores.resize(Images.size());
		for (uint32 i = 0; i < Images.size(); ++i)
		{
			RenderingDoneSemaphores[i] = new FVulkanSemaphore(Device);
		}

		RHIBackBuffer = MakeRefCount<FVulkanBackBuffer>(Device, this);

		DOGE_DEBUG("Vulkan image views created. (size: {})", BackBufferImages.size());
	}

	FVulkanViewport::~FVulkanViewport()
	{
		for (uint32 i = 0; i < TextureViews.size(); ++i)
		{
			Device.GetHandle().destroyImageView(TextureViews[i].ImageView);
		}
		TextureViews.clear();

		DestroySwapChain();

		for(auto & RenderingDoneSemaphore : RenderingDoneSemaphores)
		{
			delete RenderingDoneSemaphore;
		}
		RenderingDoneSemaphores.clear();
	}

	auto FVulkanViewport::GetWindowHandle() -> void*
	{
		return NativeWindowHandle;
	}

	auto FVulkanViewport::AcquireBackBufferImage() -> FVulkanTextureView&
	{
		AcquiredBackBufferIndex = static_cast<int32>(SwapChain->AcquireImageIndex(&AcquiredSemaphore));
		return TextureViews[AcquiredBackBufferIndex];
	}

	auto FVulkanViewport::DestroySwapChain() -> void
	{
		Device.WaitUtilIdle();

		delete SwapChain;
		SwapChain = nullptr;
	}

	auto FVulkanViewport::GetBackBuffer(FRHICommandListImmediate& InRHICmdList) -> TRefCountPtr<FRHITexture>
	{
		RHIBackBuffer->AcquireBackBufferImage(static_cast<FVulkanCommandListContext&>(InRHICmdList.GetContext()));
		return RHIBackBuffer;
	}

	auto FVulkanViewport::Present(FVulkanCommandListContext& InContext, FVulkanCommandBuffer& InCmdBuffer, FVulkanQueue& InPresentQueue, bool bInLockToVsync) -> bool
	{
		FVulkanCommandBufferManager* CmdBufferManager = InContext.GetCommandBufferManager();
		CmdBufferManager->SubmitActiveCmdBufferFromPresent(RenderingDoneSemaphores[AcquiredBackBufferIndex]);
		SwapChain->Present(&InPresentQueue, RenderingDoneSemaphores[AcquiredBackBufferIndex]);
		return true;
	}

	auto FVulkanViewport::WaitForLastFrameCompletion() -> void
	{
		FVulkanQueue* Queue = Device.GetGraphicsQueue();
		LastFrameCommandBuffer = Queue->GetLastSubmittedCommandBuffer();
		if (LastFrameCommandBuffer)
		{
			Device.GetFenceManager().WaitForFence(LastFrameCommandBuffer->GetFence(), UINT64_MAX);
			Device.GetFenceManager().ResetFence(LastFrameCommandBuffer->GetFence());
		}
	}
	auto FVulkanViewport::GetFormat() const -> EPixelFormat
	{
		return ImageFormat;
	}

	auto FVulkanViewport::GetVkFormat() const -> vk::Format
	{
		return SwapChain->GetFormat();
	}

	auto FVulkanDynamicRHI::RHICreateViewport(void* WindowHandle, uint32 SizeX, uint32 SizeY, bool bIsFullscreen, EPixelFormat PreferredPixelFormat) const -> TSharedPtr<FRHIViewport>
	{
		return std::make_shared<FVulkanViewport>(*Device, WindowHandle, SizeX, SizeY, bIsFullscreen, PreferredPixelFormat);
	}

	auto FVulkanDynamicRHI::RHIGetViewportBackBuffer(FRHIViewport* ViewportRHI) -> TRefCountPtr<FRHITexture>
	{
		return ViewportRHI->GetBackBuffer(FRHICommandListImmediate::Get());
	}

	auto FVulkanDynamicRHI::RHIBlockUntilGPUIdle() -> void
	{
		Device->WaitUtilIdle();
	}
} // namespace Doge::VulkanRHI