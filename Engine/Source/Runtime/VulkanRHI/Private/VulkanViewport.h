#pragma once

#include "RHIResources.h"
#include "VulkanTexture.h"

namespace Durin::VulkanRHI
{
	class FVulkanDevice;
	class FVulkanSwapchain;
	class FVulkanCommandListContext;
	class FVulkanCommandBuffer;
	class FVulkanQueue;
	class FVulkanSemaphore;
	struct FVulkanView;

	struct FVulkanViewportFrameResources
	{
		FVulkanSemaphore* RenderingDoneSemaphore = nullptr;
	};

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

		auto BeginDrawing(FRHICommandListImmediate& RHICmdList) -> void;

		auto RecreateSwapchainFromRT(FRHICommandListImmediate& RHICmdList) -> void;

		auto GetSwapchain() const -> FVulkanSwapchain* { return Swapchain; }

		auto GetBackBufferImages() -> const std::vector<vk::Image>& { return BackBufferImages; }

		auto AcquireBackBufferImage() -> FVulkanView&;

		auto GetBackBuffer(FRHICommandListImmediate& InRHICmdList) -> TRefCountPtr<FRHITexture> override;

		auto Present(FVulkanCommandListContext& InContext, FVulkanCommandBuffer& InCmdBuffer, FVulkanQueue& InPresentQueue, bool bInLockToVsync) -> bool;

		// Get the specified preferred pixel format when creating the viewport.
		auto GetFormat() const -> EPixelFormat override;

		// Get the actual swapchain image format, which may be different from the preferred pixel format specified when creating the viewport if the preferred format is not supported.
		auto GetSwapchainImageFormat() const -> vk::Format;

	protected:
		auto RequestResize(uint32 InSizeX, uint32 InSizeY) -> void;

		auto PrepareSwapchain(FRHICommandListImmediate& RHICmdList) -> void;

		auto MarkSwapchainNeedsRecreate() -> void;

		auto InitImages(const std::vector<vk::Image>& InImages) -> void;

		auto CreateSwapchain(vk::SwapchainKHR InOldSwapchain = VK_NULL_HANDLE) -> void;

		auto DestroySwapchain() -> void;

		auto RecreateFrameResources(uint32 NumSwapchainImages) -> void;

		auto DestroyFrameResources() -> void;

		FVulkanDevice& Device;

		FVulkanSwapchain* Swapchain;

		vk::SurfaceKHR Surface = VK_NULL_HANDLE;

		// The preferred pixel format specified when creating the viewport, this may be different from the actual swapchain image format if the preferred format is not supported.
		EPixelFormat PixelFormat = EPixelFormat::Unknown;

		std::vector<vk::Image> BackBufferImages;

		std::vector<FVulkanView> TextureViews;

		std::vector<FVulkanViewportFrameResources> FrameResources;

		TRefCountPtr<FVulkanBackBuffer> RHIBackBuffer;

		int32 AcquiredBackBufferIndex = -1;

		FVulkanSemaphore* AcquiredSemaphore = nullptr;

		uint32 SizeX;

		uint32 SizeY;

		uint32 PendingSizeX = 0;

		uint32 PendingSizeY = 0;

		bool bHasPendingResize = false;

		bool bSwapchainNeedsRecreate = false;

		bool bIsFullScreen;

		void* NativeWindowHandle;

		friend class FVulkanBackBuffer;
	};
}
