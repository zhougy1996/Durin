#pragma once

#include "RHIResources.h"
#include "VulkanTexture.h"

namespace Doge::VulkanRHI
{
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

		auto GetSwapChain() const -> FVulkanSwapChain* { return SwapChain; }

		auto GetBackBufferImages() -> const std::vector<vk::Image>& { return BackBufferImages; }

		auto AcquireBackBufferImage() -> FVulkanTextureView&;

		auto GetBackBuffer(FRHICommandListImmediate& InRHICmdList) -> TRefCountPtr<FRHITexture> override;

		auto Present(FVulkanCommandListContext& InContext, FVulkanCommandBuffer& InCmdBuffer, FVulkanQueue& InPresentQueue, bool bInLockToVsync) -> bool;

		auto WaitForLastFrameCompletion() -> void override;

		auto GetFormat() const -> EPixelFormat override;

		auto GetVkFormat() const -> vk::Format;

	protected:
		auto DestroySwapChain() -> void;

		FVulkanDevice& Device;

		FVulkanSwapChain* SwapChain;

		EPixelFormat ImageFormat;

		std::vector<vk::Image> BackBufferImages;

		std::vector<FVulkanTextureView> TextureViews;

		FVulkanCommandBuffer* LastFrameCommandBuffer = nullptr;

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
