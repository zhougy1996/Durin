#include "VulkanFramebuffer.h"

#include "RHIResources.h"
#include "VulkanDevice.h"
#include "VulkanRenderPass.h"
#include "VulkanTexture.h"
#include "VulkanView.h"

namespace Durin::VulkanRHI
{
	namespace
	{
		auto GetImage(const FRHITexture* Texture) -> vk::Image
		{
			return Texture != nullptr ? static_cast<const FVulkanTexture*>(Texture)->Image : vk::Image{};
		}

		auto DepthAspect(vk::Format Format) -> vk::ImageAspectFlags
		{
			vk::ImageAspectFlags Flags = vk::ImageAspectFlagBits::eDepth;
			if (Format == vk::Format::eD24UnormS8Uint || Format == vk::Format::eD32SfloatS8Uint)
			{
				Flags |= vk::ImageAspectFlagBits::eStencil;
			}
			return Flags;
		}
	}

	FVulkanFramebuffer::FVulkanFramebuffer(FVulkanDevice& InDevice, const FRHIRenderPassInfo& RPInfo, const FVulkanRenderPass& InRenderPass)
		: Device(InDevice)
		, RenderPass(&InRenderPass)
	{
		std::ranges::fill(ColorRenderTargetImages, vk::Image{});
		std::ranges::fill(ColorResolveTargetImages, vk::Image{});
		DepthStencilRenderTargetImage = vk::Image{};
		NumColorRenderTargets = RPInfo.RenderTargetLayout.NumColorRenderTargets;

		FRHITexture* ExtentSource = NumColorRenderTargets > 0 ? RPInfo.ColorRenderTargets[0] : RPInfo.DepthStencilRenderTarget;
		check(ExtentSource != nullptr);
		Extent = vk::Extent2D(ExtentSource->GetSizeX(), ExtentSource->GetSizeY());

		std::vector<vk::ImageView> AttachmentViews;
		AttachmentViews.reserve(InRenderPass.GetAttachmentCount());
		auto AddView = [this, &AttachmentViews](FRHITexture* RHITexture, vk::ImageAspectFlags Aspect) {
			auto* Texture = static_cast<FVulkanTexture*>(RHITexture);
			vk::ImageViewCreateInfo CreateInfo;
			CreateInfo.setImage(Texture->Image)
				.setViewType(vk::ImageViewType::e2D)
				.setFormat(Texture->Format)
				.setComponents(vk::ComponentMapping())
				.setSubresourceRange(vk::ImageSubresourceRange(Aspect, 0, 1, 0, 1));
			vk::ImageView ImageView = Device.GetHandle().createImageView(CreateInfo);
			AttachmentTextureViews.emplace_back(Texture->Image, ImageView);
			AttachmentViews.push_back(ImageView);
		};

		for (uint32 Index = 0; Index < NumColorRenderTargets; ++Index)
		{
			ColorRenderTargetImages[Index] = GetImage(RPInfo.ColorRenderTargets[Index]);
			AddView(RPInfo.ColorRenderTargets[Index], vk::ImageAspectFlagBits::eColor);
			if (RPInfo.RenderTargetLayout.ColorAttachments[Index].bHasResolveTarget)
			{
				ColorResolveTargetImages[Index] = GetImage(RPInfo.ColorResolveTargets[Index]);
				AddView(RPInfo.ColorResolveTargets[Index], vk::ImageAspectFlagBits::eColor);
				++NumColorAttachments;
			}
			++NumColorAttachments;
		}

		if (RPInfo.RenderTargetLayout.bHasDepthStencil)
		{
			auto* Texture = static_cast<FVulkanTexture*>(RPInfo.DepthStencilRenderTarget);
			DepthStencilRenderTargetImage = Texture->Image;
			AddView(Texture, DepthAspect(Texture->Format));
		}

		vk::FramebufferCreateInfo CreateInfo;
		CreateInfo.setRenderPass(InRenderPass.GetHandle())
			.setAttachments(AttachmentViews)
			.setWidth(Extent.width)
			.setHeight(Extent.height)
			.setLayers(1);
		Framebuffer = Device.GetHandle().createFramebuffer(CreateInfo);
	}

	FVulkanFramebuffer::~FVulkanFramebuffer()
	{
		for (FVulkanView& View : AttachmentTextureViews)
		{
			Device.GetDeferredDeletionQueue().EnqueueResource(FDeferredDeletionQueue::EType::ImageView, View.ImageView);
		}
		Device.GetDeferredDeletionQueue().EnqueueResource(FDeferredDeletionQueue::EType::Framebuffer, Framebuffer);
	}

	auto FVulkanFramebuffer::ContainsRenderTarget(vk::Image Image) const -> bool
	{
		for (uint32 Index = 0; Index < NumColorRenderTargets; ++Index)
		{
			if (ColorRenderTargetImages[Index] == Image || ColorResolveTargetImages[Index] == Image)
			{
				return true;
			}
		}
		return DepthStencilRenderTargetImage == Image;
	}

	auto FVulkanFramebuffer::IsCompatibleWith(const FVulkanRenderPass& InRenderPass, const FRHIRenderPassInfo& RPInfo) const -> bool
	{
		if (RenderPass != &InRenderPass || NumColorRenderTargets != RPInfo.RenderTargetLayout.NumColorRenderTargets)
		{
			return false;
		}
		for (uint32 Index = 0; Index < NumColorRenderTargets; ++Index)
		{
			if (ColorRenderTargetImages[Index] != GetImage(RPInfo.ColorRenderTargets[Index])
				|| ColorResolveTargetImages[Index] != GetImage(RPInfo.ColorResolveTargets[Index]))
			{
				return false;
			}
		}
		return DepthStencilRenderTargetImage == GetImage(RPInfo.DepthStencilRenderTarget);
	}
} // namespace Durin::VulkanRHI
