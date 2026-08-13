#include "VulkanView.h"

#include "VulkanBuffer.h"
#include "VulkanDevice.h"
#include "VulkanDynamicRHI.h"
#include "VulkanResourceState.h"
#include "VulkanRHIPrivate.h"
#include "VulkanTexture.h"

namespace Durin::VulkanRHI
{
	FVulkanBufferView::FVulkanBufferView(
		FVulkanDevice& InDevice,
		FRHIBuffer* InBuffer,
		const FRHIBufferViewDesc& InDesc)
		: FRHIBufferView(InBuffer, InDesc), Device(InDevice)
	{
		CheckVulkanRHIThread();
		if (InDesc.Type != ERHIBufferViewType::Formatted) return;
		const auto* Buffer = static_cast<const FVulkanBuffer*>(InBuffer);
		vk::BufferViewCreateInfo CreateInfo;
		CreateInfo.setBuffer(Buffer->GetHandle())
			.setFormat(ToVulkan_PixelFormat(InDesc.Format))
			.setOffset(InDesc.Offset)
			.setRange(InDesc.Size);
#if DURIN_VULKAN_TEST_FAILURE_INJECTION
		ThrowIfVulkanNativeCreateFailureIsArmed(EVulkanCreateFailurePoint::BufferView);
#endif
		BufferView = Device.GetHandle().createBufferView(CreateInfo);
		Device.GetRHI().GetDebugUtils().NameObject(BufferView,
			std::format("{}.BufferView", Buffer->GetDebugName()));
	}

	FVulkanBufferView::~FVulkanBufferView()
	{
		CheckVulkanRHIThread();
		if (BufferView)
		{
			Device.GetDeferredDeletionQueue().EnqueueResource(
				FDeferredDeletionQueue::EType::BufferView, BufferView);
		}
	}

	FVulkanTextureView::FVulkanTextureView(
		FVulkanDevice& InDevice,
		FRHITexture* InTexture,
		const FRHITextureViewDesc& InDesc)
		: FRHITextureView(InTexture, InDesc), Device(InDevice)
	{
		CheckVulkanRHIThread();
		const auto* Texture = static_cast<const FVulkanTexture*>(InTexture);
		SourceImage = Texture->Image;
		TextureViewBackingGeneration = Texture->GetViewBackingGeneration();
		vk::ImageViewCreateInfo CreateInfo;
		CreateInfo.setImage(SourceImage)
			.setViewType(InDesc.Dimension == ERHITextureViewDimension::TextureCube
				? vk::ImageViewType::eCube
				: (InDesc.Dimension == ERHITextureViewDimension::Texture2DArray
					? vk::ImageViewType::e2DArray : vk::ImageViewType::e2D))
			.setFormat(ToVulkan_PixelFormat(InDesc.Format))
			.setSubresourceRange(vk::ImageSubresourceRange(
				ToVulkanAspectFlags(InDesc.Range.Aspects),
				InDesc.Range.FirstMip, InDesc.Range.NumMips,
				InDesc.Range.FirstArrayLayer, InDesc.Range.NumArrayLayers));
#if DURIN_VULKAN_TEST_FAILURE_INJECTION
		ThrowIfVulkanNativeCreateFailureIsArmed(EVulkanCreateFailurePoint::ImageView);
#endif
		ImageView = Device.GetHandle().createImageView(CreateInfo);
		Device.GetRHI().GetDebugUtils().NameObject(ImageView,
			std::format("{}.ImageView", Texture->GetDebugName()));
		DebugIdentity = GVulkanImageViewHandleIdCounter.fetch_add(
			1, std::memory_order_relaxed) + 1;
	}

	FVulkanTextureView::~FVulkanTextureView()
	{
		CheckVulkanRHIThread();
		if (ImageView)
		{
			Device.GetDeferredDeletionQueue().EnqueueResource(
				FDeferredDeletionQueue::EType::ImageView, ImageView);
		}
	}

}
