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
		const FRHIBufferViewDesc& InDesc, vk::Result* OutResult)
		: FRHIBufferView(InBuffer, InDesc), Device(InDevice)
	{
		require(InBuffer);
		if (OutResult) *OutResult = vk::Result::eSuccess;
		if (InDesc.Type != ERHIBufferViewType::Formatted) return;
		const auto* Buffer = FVulkanBuffer::Cast(InBuffer);
		vk::BufferViewCreateInfo CreateInfo;
		CreateInfo.setBuffer(Buffer->GetHandle())
			.setFormat(ToVulkan_PixelFormat(InDesc.Format))
			.setOffset(InDesc.Offset)
			.setRange(InDesc.Size);
		const auto Result = Device.GetHandle().createBufferView(&CreateInfo, nullptr, &BufferView);
		if (OutResult) *OutResult = Result;
		if (Result != vk::Result::eSuccess)
		{
			BufferView = nullptr;
			if (!OutResult) ThrowVulkanError(Result, "Vulkan buffer view creation");
			return;
		}
		Device.GetRHI().GetDebugUtils().NameObjectLazy(BufferView,
			[&] { return std::format("{}.BufferView", Buffer->GetDebugName()); });
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
		const FRHITextureViewDesc& InDesc, vk::Result& OutResult)
		: FRHITextureView(InTexture, InDesc), Device(InDevice)
	{
		const auto* Texture = static_cast<const FVulkanTexture*>(InTexture);
		SourceImage = Texture->Image;
		TextureViewBackingGeneration = Texture->GetViewBackingGeneration();
		vk::ImageViewCreateInfo CreateInfo;
		CreateInfo.setImage(SourceImage)
			.setViewType(InDesc.Dimension == ERHITextureViewDimension::Texture3D
				? vk::ImageViewType::e3D
				: (InDesc.Dimension == ERHITextureViewDimension::TextureCubeArray
				? vk::ImageViewType::eCubeArray
				: (InDesc.Dimension == ERHITextureViewDimension::TextureCube
				? vk::ImageViewType::eCube
				: (InDesc.Dimension == ERHITextureViewDimension::Texture2DArray
					? vk::ImageViewType::e2DArray : vk::ImageViewType::e2D))))
			.setFormat(ToVulkan_PixelFormat(InDesc.Format))
			.setSubresourceRange(vk::ImageSubresourceRange(
				ToVulkanAspectFlags(InDesc.Range.Aspects),
				InDesc.Range.FirstMip, InDesc.Range.NumMips,
				InDesc.Range.FirstArrayLayer, InDesc.Range.NumArrayLayers));
		OutResult = Device.GetHandle().createImageView(&CreateInfo, nullptr, &ImageView);
		if (OutResult != vk::Result::eSuccess) { ImageView = nullptr; return; }
		Device.GetRHI().GetDebugUtils().NameObjectLazy(ImageView,
			[&] { return std::format("{}.ImageView", Texture->GetDebugName()); });
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
