#include "VulkanFramebuffer.h"

#include "RHIResources.h"
#include "VulkanDevice.h"
#include "VulkanTexture.h"
#include "VulkanView.h"
#include "VulkanRenderPass.h"

namespace Durin::VulkanRHI
{
	FVulkanFramebuffer::FVulkanFramebuffer(FVulkanDevice& InDevice, const FRHIRenderTargetsInfo& RTInfo, const FVulkanRenderPass& InRenderPass)
		: Device(InDevice)
		, RenderPass(&InRenderPass)
	{
		std::ranges::fill(ColorRenderTargetImages, vk::Image{});
		std::ranges::fill(ColorResolveTargetImages, vk::Image{});
		DepthStencilRenderTargetImage = vk::Image{};

		NumColorRenderTargets = RTInfo.NumColorRenderTargets;

		// TODO: modify this when MSAA implemented.
		NumColorAttachments = NumColorRenderTargets;

		uint32 Width = RTInfo.ColorRenderTargets[0]->GetSizeX();
		uint32 Height = RTInfo.ColorRenderTargets[0]->GetSizeY();
		Extent = vk::Extent2D(Width, Height);

		for (uint32 i = 0; i < NumColorRenderTargets; ++i)
		{
			FVulkanTexture* Texture = static_cast<FVulkanTexture*>(RTInfo.ColorRenderTargets[i]);
			vk::Image Image = Texture->Image;
			vk::Format Format = Texture->Format;

			vk::ImageViewCreateInfo ImageViewCreateInfo;
			ImageViewCreateInfo
				.setImage(Image)
				.setViewType(vk::ImageViewType::e2D)
				.setFormat(Format)
				.setComponents(vk::ComponentMapping())
				.setSubresourceRange(vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1));

			vk::ImageView ImageView = Device.GetHandle().createImageView(ImageViewCreateInfo);

			FVulkanView TextureView{Image, ImageView};

			ColorRenderTargetImages[i] = Image;
			AttachmentTextureViews.push_back(TextureView);
		}
		if (RTInfo.DepthStencilRenderTarget != nullptr)
		{
			auto* Texture = static_cast<FVulkanTexture*>(RTInfo.DepthStencilRenderTarget);
			DepthStencilRenderTargetImage = Texture->Image;
			vk::ImageViewCreateInfo ImageViewCreateInfo;
			ImageViewCreateInfo.setImage(Texture->Image)
				.setViewType(vk::ImageViewType::e2D)
				.setFormat(Texture->Format)
				.setComponents(vk::ComponentMapping())
				.setSubresourceRange(vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eDepth, 0, 1, 0, 1));
			AttachmentTextureViews.emplace_back(Texture->Image, Device.GetHandle().createImageView(ImageViewCreateInfo));
		}

		std::vector<vk::ImageView> AttachmentViews;
		for (uint32 i = 0; i < NumColorRenderTargets; ++i)
		{
			AttachmentViews.push_back(AttachmentTextureViews[i].ImageView);
		}
		if (RTInfo.DepthStencilRenderTarget != nullptr)
		{
			AttachmentViews.push_back(AttachmentTextureViews.back().ImageView);
		}

		vk::FramebufferCreateInfo FramebufferCreateInfo;

		FramebufferCreateInfo
			.setRenderPass(InRenderPass.GetHandle())
			.setAttachments(AttachmentViews)
			.setWidth(Extent.width)
			.setHeight(Extent.height)
			.setLayers(1);

		Framebuffer = Device.GetHandle().createFramebuffer(FramebufferCreateInfo);
	}

	FVulkanFramebuffer::~FVulkanFramebuffer()
	{
		for (FVulkanView& View : AttachmentTextureViews)
		{
			Device.GetDeferredDeletionQueue().EnqueueResource(FDeferredDeletionQueue::EType::ImageView, View.ImageView);
		}
		Device.GetDeferredDeletionQueue(). EnqueueResource(FDeferredDeletionQueue::EType::Framebuffer, Framebuffer);
	}

	auto FVulkanFramebuffer::ContainsRenderTarget(vk::Image Image) const -> bool
	{
		for (uint32 i = 0; i < NumColorAttachments; ++i)
		{
			if (ColorRenderTargetImages[i] == Image)
			{
				return true;
			}
		}

		return DepthStencilRenderTargetImage == Image;
	}

	auto FVulkanFramebuffer::IsCompatibleWith(const FVulkanRenderPass& InRenderPass, vk::Image ColorImage, vk::Image DepthImage) const -> bool
	{
		return RenderPass == &InRenderPass && ColorRenderTargetImages[0] == ColorImage && DepthStencilRenderTargetImage == DepthImage;
	}
} // namespace Durin::VulkanRHI
