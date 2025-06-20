#pragma once

#include "RHIResources.h"
#include "VulkanTexture.h"

class FVulkanDevice;
class FVulkanSwapChain;
class FVulkanCommandListContext;
class FVulkanCommandBuffer;
class FVulkanQueue;
class FVulkanSemaphore;
struct FVulkanTextureView;

class FVulkanViewport;

class FVulkanBackBuffer : public FVulkanTexture
{
public:
	FVulkanBackBuffer(FVulkanDevice& Device, FVulkanViewport* Viewport);

	auto AcquireBackBufferImage(FVulkanCommandListContext& Context);

private:
	FVulkanViewport* Viewport_;
};

class FVulkanViewport : public FRHIViewport
{
public:
	FVulkanViewport(FVulkanDevice& Device, void* WindowHandle, uint32 SizeX, uint32 SizeY, bool bIsFullScreen, EPixelFormat PreferredPixelFormat);

	virtual ~FVulkanViewport();

	virtual auto GetWindowHandle() -> void*;

	auto GetSizeXY() -> FIntPoint { return FIntPoint(SizeX_, SizeY_); }

	auto GetSwapChain() -> FVulkanSwapChain* { return SwapChain_; }

	auto GetBackBufferImages() -> const TArray<vk::Image>& { return BackBufferImages_; }

	auto AcquireBackBufferImage() -> FVulkanTextureView&;

	auto GetBackBuffer(FRHICommandListImmediate& RHICmdList) -> TSharedPtr<FRHITexture> override;

	auto Present(FVulkanCommandListContext& Context, FVulkanCommandBuffer& CmdBuffer, FVulkanQueue& PresentQueue, bool bLockToVsync) -> bool;

	auto WaitForLastFrameCompletion() -> void override;

protected:
	auto DestroySwapChain() -> void;

	FVulkanDevice& Device_;

	FVulkanSwapChain* SwapChain_;

	TArray<vk::Image> BackBufferImages_;

	TArray<FVulkanTextureView> TextureViews_;

	FVulkanCommandBuffer* LastFrameCommandBuffer_ = nullptr;

	TArray<FVulkanSemaphore*> RenderingDoneSemaphores_;

	TSharedPtr<FVulkanBackBuffer> RHIBackBuffer_;

	int32 AcquiredBackBufferIndex_ = -1;

	FVulkanSemaphore* AcquiredSemaphore_ = nullptr;

	uint32 SizeX_;

	uint32 SizeY_;

	bool bIsFullScreen_;

	void* WindowHandle_;

	friend class FVulkanBackBuffer;
};
