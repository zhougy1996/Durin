#pragma once

#include "RHIResources.h"
#include "VulkanTexture.h"

namespace Doge::VulkanRHI
{
	class FVulkanDevice;
	class FVulkanSwapchain;
	class FVulkanCommandListContext;
	class FVulkanCommandBuffer;
	class FVulkanQueue;
	class FVulkanSemaphore;
	struct FVulkanTextureView;

	class FVulkanViewport;

	class FVulkanBackBuffer : public FVulkanTexture
	{
	public:
		FVulkanBackBuffer(FVulkanDevice& InDevice, FVulkanViewport* InViewport);

		auto AcquireBackBufferImage(FVulkanCommandListContext& Context);

	private:
		FVulkanViewport* Viewport;
	};

	class FVulkanViewport : public FRHIViewport
	{
	public:
		FVulkanViewport(FVulkanDevice& InDevice, void* InWindowHandle, uint32 InSizeX, uint32 InSizeY, bool bInIsFullScreen, EPixelFormat InPreferredPixelFormat);

		~FVulkanViewport() override;

		virtual auto GetWindowHandle() -> void*;

		auto GetSizeXY() const -> FIntPoint { return FIntPoint(SizeX, SizeY); }

		auto Resize(FRHICommandListImmediate& RHICmdList, uint32 InSizeX, uint32 InSizeY) -> void;

		auto RecreateSwapchainFromRT(FRHICommandListImmediate& RHICmdList) -> void;

		auto GetSwapchain() const -> FVulkanSwapchain* { return Swapchain; }

		auto GetBackBufferImages() -> const std::vector<vk::Image>& { return BackBufferImages; }

		auto AcquireBackBufferImage() -> FVulkanTextureView&;

		auto GetBackBuffer(FRHICommandListImmediate& InRHICmdList) -> TRefCountPtr<FRHITexture> override;

		auto Present(FVulkanCommandListContext& InContext, FVulkanCommandBuffer& InCmdBuffer, FVulkanQueue& InPresentQueue, bool bInLockToVsync) -> bool;

		auto WaitForLastFrameCompletion() -> void override;

		auto GetFormat() const -> EPixelFormat override;

		auto GetSwapchainImageFormat() const -> vk::Format;

	protected:
		auto InitImages(const std::vector<vk::Image>& InImages) -> void;

		auto CreateSwapchain() -> void;

		auto DestroySwapchain() -> void;

		FVulkanDevice& Device;

		FVulkanSwapchain* Swapchain;

		// The preferred pixel format specified when creating the viewport, this may be different from the actual swapchain image format if the preferred format is not supported.
		EPixelFormat PixelFormat = EPixelFormat::Unknown;

		std::vector<vk::Image> BackBufferImages;

		std::vector<FVulkanTextureView> TextureViews;

		FVulkanCommandBuffer* LastFrameCommandBuffer = nullptr;

		// These semaphores will be signaled when rendering to the corresponding swapchain image is done, and will be waited on before presenting that image.
		std::vector<FVulkanSemaphore*> RenderingDoneSemaphores;

		TRefCountPtr<FVulkanBackBuffer> RHIBackBuffer;

		int32 AcquiredBackBufferIndex = -1;

		FVulkanSemaphore* AcquiredSemaphore = nullptr;

		uint32 SizeX;

		uint32 SizeY;

		bool bIsFullScreen;

		void* NativeWindowHandle;

		friend class FVulkanBackBuffer;
	};
}
