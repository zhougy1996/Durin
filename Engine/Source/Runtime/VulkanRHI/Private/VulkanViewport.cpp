#include "VulkanViewport.h"

#include "RHICommandList.h"
#include "VulkanDynamicRHI.h"
#include "VulkanDevice.h"
#include "VulkanView.h"
#include "VulkanCommandBuffer.h"
#include "VulkanContext.h"
#include "VulkanSwapChain.h"
#include "VulkanQueue.h"
#include "ThirdParty/Glfw/GlfwCommon.h"

FVulkanBackBuffer::FVulkanBackBuffer(FVulkanDevice& Device, FVulkanViewport* Viewport)
	: FVulkanTexture(Device, nullptr)
	, Viewport_(Viewport)
{
}

auto FVulkanBackBuffer::AcquireBackBufferImage(FVulkanCommandListContext& Context)
{
	const FVulkanTextureView& View = Viewport_->AcquireBackBufferImage();
	Image_ = View.Image;

	FVulkanCommandBufferManager* CmdBufferManager = Context.GetCommandBufferManager();
	FVulkanCommandBuffer* CmdBuffer = CmdBufferManager->GetActiveCommandBuffer();
	CmdBuffer->AddWaitSemaphore(Viewport_->AcquiredSemaphore_);
}

FVulkanViewport::FVulkanViewport(FVulkanDevice& InDevice, void* InGlfwWindowHandle, uint32 InSizeX, uint32 InSizeY, bool InbIsFullScreen, EPixelFormat InPreferredPixelFormat)
	: Device_(InDevice)
	, SizeX_(InSizeX)
	, SizeY_(InSizeY)
	, bIsFullScreen_(InbIsFullScreen)
{
	NativeWindowHandle = GetNativeWindowHandle(static_cast<GLFWwindow*>(InGlfwWindowHandle));
	SwapChain_ = new FVulkanSwapChain(FVulkanDynamicRHI::Get().RHIGetVkInstance(), InDevice, InGlfwWindowHandle, InSizeX, InSizeY, InbIsFullScreen);
	const std::vector<vk::Image>& Images = SwapChain_->GetImages();

	vk::ImageViewCreateInfo ImageViewCreateInfo;
	ImageViewCreateInfo.setViewType(vk::ImageViewType::e2D);
	ImageViewCreateInfo.setFormat(vk::Format::eR8G8B8A8Srgb);
	ImageViewCreateInfo.setComponents({vk::ComponentSwizzle::eR, vk::ComponentSwizzle::eG, vk::ComponentSwizzle::eB, vk::ComponentSwizzle::eA});
	ImageViewCreateInfo.setSubresourceRange({vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1});

	BackBufferImages_.resize(Images.size());
	for (uint32 i = 0; i < Images.size(); ++i)
	{
		BackBufferImages_[i] = Images[i];
		ImageViewCreateInfo.setImage(Images[i]);
		vk::ImageView View = Device_.GetHandle().createImageView(ImageViewCreateInfo);
		TextureViews_.emplace_back(Images[i], View);
	}

	RenderingDoneSemaphores_.resize(Images.size());
	for (uint32 i = 0; i < Images.size(); ++i)
	{
		RenderingDoneSemaphores_[i] = new FVulkanSemaphore(Device_);
	}

	RHIBackBuffer_ = std::make_shared<FVulkanBackBuffer>(Device_, this);

	DOGE_DEBUG("Vulkan image views created. (size: {})", BackBufferImages_.size());
}

FVulkanViewport::~FVulkanViewport()
{
	for (uint32 i = 0; i < TextureViews_.size(); ++i)
	{
		Device_.GetHandle().destroyImageView(TextureViews_[i].ImageView);
	}
	TextureViews_.clear();

	DestroySwapChain();

	for(auto & RenderingDoneSemaphore : RenderingDoneSemaphores_)
	{
		delete RenderingDoneSemaphore;
	}
	RenderingDoneSemaphores_.clear();
}

auto FVulkanViewport::GetWindowHandle() -> void*
{
	return NativeWindowHandle;
}

auto FVulkanViewport::AcquireBackBufferImage() -> FVulkanTextureView&
{
	AcquiredBackBufferIndex_ = static_cast<int32>(SwapChain_->AcquireImageIndex(&AcquiredSemaphore_));
	return TextureViews_[AcquiredBackBufferIndex_];
}

auto FVulkanViewport::DestroySwapChain() -> void
{
	Device_.WaitUtilIdle();

	delete SwapChain_;
	SwapChain_ = nullptr;
}

auto FVulkanViewport::GetBackBuffer(FRHICommandListImmediate& RHICmdList) -> TSharedPtr<FRHITexture>
{
	RHIBackBuffer_->AcquireBackBufferImage(static_cast<FVulkanCommandListContext&>(RHICmdList.GetContext()));
	return RHIBackBuffer_;
}

auto FVulkanViewport::Present(FVulkanCommandListContext& Context, FVulkanCommandBuffer& CmdBuffer, FVulkanQueue& PresentQueue, bool bLockToVsync) -> bool
{
	FVulkanCommandBufferManager* CmdBufferManager = Context.GetCommandBufferManager();
	CmdBufferManager->SubmitActiveCmdBufferFromPresent(RenderingDoneSemaphores_[AcquiredBackBufferIndex_]);
	SwapChain_->Present(&PresentQueue, RenderingDoneSemaphores_[AcquiredBackBufferIndex_]);
	return true;
}

auto FVulkanViewport::WaitForLastFrameCompletion() -> void
{
	FVulkanQueue* Queue = Device_.GetGraphicsQueue();
	LastFrameCommandBuffer_ = Queue->GetLastSubmittedCommandBuffer();
	if (LastFrameCommandBuffer_)
	{
		Device_.GetFenceManager().WaitForFence(LastFrameCommandBuffer_->GetFence(), UINT64_MAX);
		Device_.GetFenceManager().ResetFence(LastFrameCommandBuffer_->GetFence());
	}
}

auto FVulkanDynamicRHI::RHICreateViewport(void* GlfwWindowHandle, uint32 SizeX, uint32 SizeY, bool bIsFullscreen, EPixelFormat PreferredPixelFormat) const -> TSharedPtr<FRHIViewport>
{
	return std::make_shared<FVulkanViewport>(*Device_, GlfwWindowHandle, SizeX, SizeY, bIsFullscreen, PreferredPixelFormat);
}

auto FVulkanDynamicRHI::RHIGetViewportBackBuffer(FRHIViewport* ViewportRHI) -> TSharedPtr<FRHITexture>
{
	return ViewportRHI->GetBackBuffer(FRHICommandListImmediate::Get());
}