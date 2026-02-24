#include "VulkanFramebuffer.h"

#include "RHIResources.h"
#include "VulkanDevice.h"
#include "VulkanTexture.h"
#include "VulkanView.h"
#include "VulkanRenderPass.h"

FVulkanFramebuffer::FVulkanFramebuffer(FVulkanDevice& Device, const FRHIRenderTargetsInfo& RTInfo, const FVulkanRenderPass& RenderPass)
	: Device_(Device)
{
	NumColorRenderTargets_ = RTInfo.NumColorRenderTargets;

	// TODO: modify this when MSAA implemented.
	NumColorAttachments_ = NumColorRenderTargets_;

	uint32 Width = RTInfo.ColorRenderTargets[0]->GetSizeX();
	uint32 Height = RTInfo.ColorRenderTargets[0]->GetSizeY();
	Extent_ = vk::Extent2D(Width, Height);

	for (uint32 i = 0; i < NumColorRenderTargets_; ++i)
	{
		FVulkanTexture* Texture = static_cast<FVulkanTexture*>(RTInfo.ColorRenderTargets[i]);
		vk::Image Image = Texture->Image_;
		vk::Format Format = Texture->Format_;

		vk::ImageViewCreateInfo ImageViewCreateInfo;
		ImageViewCreateInfo
			.setImage(Image)
			.setViewType(vk::ImageViewType::e2D)
			.setFormat(Format)
			.setComponents(vk::ComponentMapping())
			.setSubresourceRange(vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1));

		vk::ImageView ImageView = Device_.GetHandle().createImageView(ImageViewCreateInfo);

		FVulkanTextureView TextureView{Image, ImageView};

		ColorRenderTargetImages_[i] = Image;
		AttachmentTextureViews_.push_back(TextureView);
	}

	std::vector<vk::ImageView> AttachmentViews;
	for (uint32 i = 0; i < NumColorRenderTargets_; ++i)
	{
		AttachmentViews.push_back(AttachmentTextureViews_[i].ImageView);
	}

	vk::FramebufferCreateInfo FramebufferCreateInfo;

	FramebufferCreateInfo
		.setRenderPass(RenderPass.GetHandle())
		.setAttachments(AttachmentViews)
		.setWidth(Extent_.width)
		.setHeight(Extent_.height)
		.setLayers(1);

	try
	{
		Framebuffer_ = Device_.GetHandle().createFramebuffer(FramebufferCreateInfo);
		DOGE_DEBUG("Vulkan framebuffer created. Render targets count: {}", NumColorRenderTargets_);
	}
	catch (const std::runtime_error& err)
	{
		DOGE_ERROR("Failed to create vulkan framebuffer: {}", err.what());
	}
}
