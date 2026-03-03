#include "VulkanFramebuffer.h"

#include "RHIResources.h"
#include "VulkanDevice.h"
#include "VulkanTexture.h"
#include "VulkanView.h"
#include "VulkanRenderPass.h"

namespace Doge::VulkanRHI
{
	FVulkanFramebuffer::FVulkanFramebuffer(FVulkanDevice& InDevice, const FRHIRenderTargetsInfo& RTInfo, const FVulkanRenderPass& InRenderPass)
		: Device(InDevice)
	{
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

			FVulkanTextureView TextureView{Image, ImageView};

			ColorRenderTargetImages[i] = Image;
			AttachmentTextureViews.push_back(TextureView);
		}

		std::vector<vk::ImageView> AttachmentViews;
		for (uint32 i = 0; i < NumColorRenderTargets; ++i)
		{
			AttachmentViews.push_back(AttachmentTextureViews[i].ImageView);
		}

		vk::FramebufferCreateInfo FramebufferCreateInfo;

		FramebufferCreateInfo
			.setRenderPass(InRenderPass.GetHandle())
			.setAttachments(AttachmentViews)
			.setWidth(Extent.width)
			.setHeight(Extent.height)
			.setLayers(1);

		try
		{
			Framebuffer = Device.GetHandle().createFramebuffer(FramebufferCreateInfo);
			DOGE_DEBUG("Vulkan framebuffer created. Render targets count: {}", NumColorRenderTargets);
		}
		catch (const std::runtime_error& err)
		{
			DOGE_ERROR("Failed to create vulkan framebuffer: {}", err.what());
		}
	}
}