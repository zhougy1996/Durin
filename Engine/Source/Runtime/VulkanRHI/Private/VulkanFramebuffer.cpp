#include "VulkanFramebuffer.h"


#include "RHIResources.h"
#include "VulkanDevice.h"
#include "VulkanDynamicRHI.h"
#include "VulkanRenderPass.h"
#include "VulkanRHIPrivate.h"
#include "VulkanTexture.h"
#include "VulkanView.h"
#include "VulkanViewport.h"

namespace Durin::VulkanRHI
{
	namespace
	{
		auto GetImage(const FRHITexture* Texture) -> vk::Image
		{
			return Texture != nullptr ? static_cast<const FVulkanTexture*>(Texture)->Image : vk::Image{};
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

		FRHITextureView* ExtentSource = NumColorRenderTargets > 0 ? RPInfo.ColorRenderTargetViews[0] : RPInfo.DepthStencilRenderTargetView;
		check(ExtentSource != nullptr);
		const FIntPoint ViewExtent = ExtentSource->GetExtent();
		Extent = vk::Extent2D(ViewExtent.x, ViewExtent.y);

		AttachmentDescs.reserve(InRenderPass.GetAttachmentCount());
		AttachmentViews.reserve(InRenderPass.GetAttachmentCount());
		auto AddView = [this](FRHITextureView* RHIView) {
			check(RHIView);
			auto* View = static_cast<FVulkanTextureView*>(RHIView);
			// A logical swapchain attachment can resolve to the 1x1 surface-loss dummy.
			if (const auto* Dummy = FVulkanBackBuffer::GetBoundRenderingBackBuffer(
				*static_cast<FVulkanTexture*>(View->GetTexture())))
			{
				Extent.width = std::min(Extent.width, Dummy->GetSizeX());
				Extent.height = std::min(Extent.height, Dummy->GetSizeY());
			}
			AttachmentDescs.push_back(View->GetDesc());
			View->MarkUsedByFramebuffer();
			AttachmentViews.push_back(View->GetHandle());
		};

		for (uint32 Index = 0; Index < NumColorRenderTargets; ++Index)
		{
			ColorRenderTargetImages[Index] = GetImage(RPInfo.ColorRenderTargets[Index]);
			AddView(RPInfo.ColorRenderTargetViews[Index]);
			if (RPInfo.RenderTargetLayout.ColorAttachments[Index].bHasResolveTarget)
			{
				ColorResolveTargetImages[Index] = GetImage(RPInfo.ColorResolveTargets[Index]);
				AddView(RPInfo.ColorResolveTargetViews[Index]);
				++NumColorAttachments;
			}
			++NumColorAttachments;
		}

		if (RPInfo.RenderTargetLayout.bHasDepthStencil)
		{
			auto* Texture = static_cast<FVulkanTexture*>(RPInfo.DepthStencilRenderTarget);
			DepthStencilRenderTargetImage = Texture->Image;
			AddView(RPInfo.DepthStencilRenderTargetView);
		}

		vk::FramebufferCreateInfo CreateInfo;
		CreateInfo.setRenderPass(InRenderPass.GetHandle())
			.setAttachments(AttachmentViews)
			.setWidth(Extent.width)
			.setHeight(Extent.height)
			.setLayers(1);
		Framebuffer = Device.GetHandle().createFramebuffer(CreateInfo);
		Device.GetRHI().GetDebugUtils().NameObjectLazy(Framebuffer,
			[&] { return Device.GetRHI().GetDebugUtils().MakeInternalName("Framebuffer"); });
	}

	FVulkanFramebuffer::~FVulkanFramebuffer()
	{
		if (Framebuffer)
		{
			Device.GetDeferredDeletionQueue().EnqueueResource(FDeferredDeletionQueue::EType::Framebuffer, Framebuffer);
		}
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
		if (DepthStencilRenderTargetImage != GetImage(RPInfo.DepthStencilRenderTarget)) return false;
		size_t Attachment = 0;
		for (uint32 Index = 0; Index < NumColorRenderTargets; ++Index)
		{
			if (AttachmentDescs[Attachment++] != RPInfo.ColorRenderTargetViews[Index]->GetDesc()) return false;
			if (RPInfo.RenderTargetLayout.ColorAttachments[Index].bHasResolveTarget
				&& AttachmentDescs[Attachment++] != RPInfo.ColorResolveTargetViews[Index]->GetDesc()) return false;
		}
		return !RPInfo.RenderTargetLayout.bHasDepthStencil
			|| AttachmentDescs[Attachment] == RPInfo.DepthStencilRenderTargetView->GetDesc();
	}
} // namespace Durin::VulkanRHI
