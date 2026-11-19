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

		auto GetAttachmentHandle(FRHITexture* Target, FRHITextureView* ExplicitView) -> vk::ImageView
		{
			auto* View = static_cast<FVulkanTextureView*>(ExplicitView);
			if (View && View->GetHandle()) return View->GetHandle();
			auto* Texture = static_cast<FVulkanTexture*>(Target);
			return Texture->HasExternalBacking() ? static_cast<FVulkanBackBuffer*>(Texture)->GetCurrentAttachmentView()
				: Texture->GetDefaultAttachmentView();
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

		AttachmentViews.reserve(InRenderPass.GetAttachmentCount());
		auto AddView = [this](FRHITexture* Target, FRHITextureView* ExplicitView) {
			if (AttachmentViews.empty())
			{
				const auto Size = ExplicitView ? ExplicitView->GetExtent() : FIntPoint(Target->GetSizeX(), Target->GetSizeY());
				Extent = vk::Extent2D(Size.x, Size.y);
			}
			if (const auto* Dummy = FVulkanBackBuffer::GetBoundRenderingBackBuffer(*static_cast<FVulkanTexture*>(Target)))
			{
				Extent.width = std::min(Extent.width, Dummy->GetSizeX());
				Extent.height = std::min(Extent.height, Dummy->GetSizeY());
			}
			if (ExplicitView) static_cast<FVulkanTextureView*>(ExplicitView)->MarkUsedByFramebuffer();
			const auto Handle = GetAttachmentHandle(Target, ExplicitView);
			check(Handle);
			AttachmentViews.push_back(Handle);
		};

		for (uint32 Index = 0; Index < NumColorRenderTargets; ++Index)
		{
			ColorRenderTargetImages[Index] = GetImage(RPInfo.ColorRenderTargets[Index]);
			AddView(RPInfo.ColorRenderTargets[Index], RPInfo.ColorRenderTargetViews[Index]);
			if (RPInfo.RenderTargetLayout.ColorAttachments[Index].bHasResolveTarget)
			{
				ColorResolveTargetImages[Index] = GetImage(RPInfo.ColorResolveTargets[Index]);
				AddView(RPInfo.ColorResolveTargets[Index], RPInfo.ColorResolveTargetViews[Index]);
				++NumColorAttachments;
			}
			++NumColorAttachments;
		}

		if (RPInfo.RenderTargetLayout.bHasDepthStencil)
		{
			auto* Texture = static_cast<FVulkanTexture*>(RPInfo.DepthStencilRenderTarget);
			DepthStencilRenderTargetImage = Texture->Image;
			AddView(RPInfo.DepthStencilRenderTarget, RPInfo.DepthStencilRenderTargetView);
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
			if (AttachmentViews[Attachment++] != GetAttachmentHandle(RPInfo.ColorRenderTargets[Index], RPInfo.ColorRenderTargetViews[Index])) return false;
			if (RPInfo.RenderTargetLayout.ColorAttachments[Index].bHasResolveTarget
				&& AttachmentViews[Attachment++] != GetAttachmentHandle(RPInfo.ColorResolveTargets[Index], RPInfo.ColorResolveTargetViews[Index])) return false;
		}
		return !RPInfo.RenderTargetLayout.bHasDepthStencil
			|| AttachmentViews[Attachment] == GetAttachmentHandle(RPInfo.DepthStencilRenderTarget, RPInfo.DepthStencilRenderTargetView);
	}
} // namespace Durin::VulkanRHI
