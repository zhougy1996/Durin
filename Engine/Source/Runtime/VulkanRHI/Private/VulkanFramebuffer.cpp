#include "VulkanFramebuffer.h"

#include "RHIResources.h"
#include "VulkanDevice.h"
#include "VulkanRenderPass.h"
#include "VulkanRHIPrivate.h"
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
		AttachmentTextureViews.reserve(InRenderPass.GetAttachmentCount());
		AttachmentIdentities.reserve(InRenderPass.GetAttachmentCount());
		AttachmentViews.reserve(InRenderPass.GetAttachmentCount());
		auto AddView = [this, &AttachmentViews](FRHITextureView* RHIView) {
			check(RHIView);
			auto* View = static_cast<FVulkanTextureView*>(RHIView);
			AttachmentTextureViews.emplace_back(View);
			AttachmentIdentities.push_back(View);
			AttachmentViews.push_back(View->GetHandle());
		};

		vk::Framebuffer CandidateFramebuffer;
		try
		{
		#if DURIN_VULKAN_TEST_FAILURE_INJECTION
			ThrowIfVulkanNativeCreateFailureIsArmed(
				EVulkanCreateFailurePoint::FramebufferImageView);
		#endif
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
		#if DURIN_VULKAN_TEST_FAILURE_INJECTION
			ThrowIfVulkanNativeCreateFailureIsArmed(EVulkanCreateFailurePoint::Framebuffer);
		#endif
			CandidateFramebuffer = Device.GetHandle().createFramebuffer(CreateInfo);
		#if DURIN_VULKAN_TEST_FAILURE_INJECTION
			GVulkanCreatedFramebufferCount.fetch_add(1, std::memory_order_release);
		#endif
		}
		catch (...)
		{
			if (CandidateFramebuffer)
			{
				Device.GetHandle().destroyFramebuffer(CandidateFramebuffer);
			#if DURIN_VULKAN_TEST_FAILURE_INJECTION
				GVulkanReleasedFramebufferCount.fetch_add(1, std::memory_order_release);
			#endif
			}
			throw;
		}
		Framebuffer = CandidateFramebuffer;
	}

	FVulkanFramebuffer::~FVulkanFramebuffer()
	{
		if (Framebuffer)
		{
			Device.GetDeferredDeletionQueue().EnqueueResource(FDeferredDeletionQueue::EType::Framebuffer, Framebuffer);
		#if DURIN_VULKAN_TEST_FAILURE_INJECTION
			GVulkanReleasedFramebufferCount.fetch_add(1, std::memory_order_release);
		#endif
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
		std::vector<const FRHITextureView*> Current;
		for (uint32 Index = 0; Index < NumColorRenderTargets; ++Index)
		{
			Current.push_back(RPInfo.ColorRenderTargetViews[Index]);
			if (RPInfo.RenderTargetLayout.ColorAttachments[Index].bHasResolveTarget)
				Current.push_back(RPInfo.ColorResolveTargetViews[Index]);
		}
		if (RPInfo.RenderTargetLayout.bHasDepthStencil)
			Current.push_back(RPInfo.DepthStencilRenderTargetView);
		return Current == AttachmentIdentities;
	}
} // namespace Durin::VulkanRHI
