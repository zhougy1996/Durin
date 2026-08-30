#pragma once

#include "RHIResources.h"
#include "VulkanRHIAPI.h"
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

	// Tracks which presentation phase currently owns a frame's swapchain resources.
	enum class EVulkanPresentResourceState : uint8
	{
		Available,
		PresentPending,
		Retired
	};

	// Bundles the semaphores and image index reused for one frame in flight.
	struct FVulkanViewportFrameResources
	{
		FVulkanSemaphore* RenderingDoneSemaphore = nullptr;
		vk::Fence PresentFence = VK_NULL_HANDLE;
		EVulkanPresentResourceState State = EVulkanPresentResourceState::Available;
	};

	// Retains one replaced presentation generation until its present fences prove
	// that the presentation engine no longer owns any of its resources.
	struct FVulkanRetiredSwapchainGeneration
	{
		FVulkanSwapchain* Swapchain = nullptr;
		std::vector<vk::Image> BackBufferImages;
		std::vector<FVulkanView> TextureViews;
		std::vector<FVulkanViewportFrameResources> FrameResources;
	};

	class FVulkanViewport;

	// Presents the current swapchain image through the stable RHI back-buffer object.
	class FVulkanBackBuffer : public FVulkanTexture
	{
	public:
		FVulkanBackBuffer(FVulkanDevice& InDevice, FVulkanViewport* InViewport);

		auto AcquireBackBufferImage(FVulkanCommandListContext& Context) -> bool;
		auto UpdateSwapchain() -> void;
		auto InvalidateSwapchain() -> void;
		auto CommitPresentedImageState() -> void;

	private:
		FVulkanViewport* Viewport;
		std::vector<std::pair<vk::Image, ERHIAccess>> ImageStates;
	};

	// Coordinates swapchain recreation, image acquisition, and presentation for a window.
	class FVulkanViewport : public FRHIViewport
	{
	public:
		FVulkanViewport(FVulkanDevice& InDevice, void* InWindowHandle,
			uint32 InSizeX, uint32 InSizeY, bool bInIsFullScreen,
			EPixelFormat InPreferredPixelFormat,
			EViewportPresentationPolicy InPresentationPolicy,
			vk::SurfaceKHR InPresentationSurface = VK_NULL_HANDLE);

		~FVulkanViewport() override;

		virtual auto GetWindowHandle() -> void*;

		auto GetSizeXY() const -> FIntPoint { return FIntPoint(SizeX, SizeY); }

		auto Resize(
			FRHICommandListImmediate& RHICmdList,
			uint32 InSizeX,
			uint32 InSizeY,
			bool bInIsFullScreen) -> void;

		VULKANRHI_API auto BeginDrawing() -> void;

		VULKANRHI_API auto RecreateSwapchain() -> void;

		auto GetSwapchain() const -> FVulkanSwapchain* { return Swapchain; }

		auto HasAvailableOutput() const -> bool { return Swapchain != nullptr && RHIBackBuffer; }

		auto GetBackBufferImages() -> const std::vector<vk::Image>& { return BackBufferImages; }

		auto AcquireBackBufferImage() -> FVulkanView*;

		auto GetBackBuffer(FRHICommandListImmediate& InRHICmdList) -> TRefCountPtr<FRHITexture> override;

		auto Present(FVulkanCommandListContext& InContext, FVulkanCommandBuffer& InCmdBuffer, FVulkanQueue& InPresentQueue, bool bInLockToVsync) -> bool;

		// Get the specified preferred pixel format when creating the viewport.
		auto GetFormat() const -> EPixelFormat override;

		// Get the actual swapchain image format, which may be different from the preferred pixel format specified when creating the viewport if the preferred format is not supported.
		auto GetSwapchainImageFormat() const -> vk::Format;

	protected:
		auto RequestResize(uint32 InSizeX, uint32 InSizeY) -> void;

		auto PrepareSwapchain() -> void;

		auto MarkSwapchainNeedsRecreate() -> void;

		auto TryCreateSwapchain(uint32 TargetSizeX, uint32 TargetSizeY) -> bool;

		auto DestroySwapchain() -> void;

		auto SetOutputUnavailable() -> void;

		auto CanDeferCurrentSwapchainDestruction() const -> bool;

		auto RetireCurrentSwapchain() -> void;

		auto CollectRetiredSwapchains(bool bWaitForCompletion) -> void;

		auto IsRetiredSwapchainReady(
			FVulkanRetiredSwapchainGeneration& Generation,
			bool bWaitForCompletion) -> bool;

		auto DestroySwapchainGeneration(
			FVulkanRetiredSwapchainGeneration& Generation) -> void;

		auto DestroyFrameResources(
			std::vector<FVulkanViewportFrameResources>& Resources) -> void;

		auto WaitForFrameResource(FVulkanViewportFrameResources& FrameResource) -> void;

		auto WaitForSwapchainIdle() -> void;

		FVulkanDevice& Device;

		FVulkanSwapchain* Swapchain = nullptr;

		vk::SurfaceKHR Surface = VK_NULL_HANDLE;

		// The preferred pixel format specified when creating the viewport, this may be different from the actual swapchain image format if the preferred format is not supported.
		EPixelFormat PixelFormat = EPixelFormat::Unknown;

		EViewportPresentationPolicy PresentationPolicy = EViewportPresentationPolicy::FramePaced;

		std::vector<vk::Image> BackBufferImages;

		std::vector<FVulkanView> TextureViews;

		std::vector<FVulkanViewportFrameResources> FrameResources;

		std::vector<FVulkanRetiredSwapchainGeneration> RetiredSwapchainGenerations;

		TRefCountPtr<FVulkanBackBuffer> RHIBackBuffer;

		int32 AcquiredBackBufferIndex = -1;

		FVulkanSemaphore* AcquiredSemaphore = nullptr;

		uint32 SizeX;

		uint32 SizeY;

		uint32 PendingSizeX = 0;

		uint32 PendingSizeY = 0;

		bool bHasPendingResize = false;

		bool bSwapchainNeedsRecreate = false;

		bool bSwapchainRetryEligible = false;

		bool bSwapchainFailureReported = false;

		bool bIsFullScreen;

		void* NativeWindowHandle;

		friend class FVulkanBackBuffer;
	};
}
