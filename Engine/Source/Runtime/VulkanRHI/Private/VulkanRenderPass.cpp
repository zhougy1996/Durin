#include "VulkanRenderPass.h"

#include "VulkanDevice.h"
#include "VulkanCommandBuffer.h"
#include "VulkanFramebuffer.h"
#include "VulkanTexture.h"

namespace Durin::VulkanRHI
{
	FVulkanRenderPass::FVulkanRenderPass(FVulkanDevice& InDevice, vk::Format InFormat, vk::ImageLayout InFinalLayout)
		: Device(InDevice)
	{
		// Color buffer attachement
		vk::AttachmentDescription ColorAttachment;
		ColorAttachment
			.setSamples(vk::SampleCountFlagBits::e1)
			.setFormat(InFormat)
			.setLoadOp(vk::AttachmentLoadOp::eClear)
			.setStoreOp(vk::AttachmentStoreOp::eStore)
			.setStencilLoadOp(vk::AttachmentLoadOp::eDontCare)
			.setStencilStoreOp(vk::AttachmentStoreOp::eDontCare)
			.setInitialLayout(vk::ImageLayout::eUndefined)
			.setFinalLayout(InFinalLayout);

		vk::AttachmentReference ColorAttachmentRef{};
		ColorAttachmentRef
			.setAttachment(0)
			.setLayout(vk::ImageLayout::eColorAttachmentOptimal);

		vk::SubpassDescription SubPass;
		SubPass
			.setPipelineBindPoint(vk::PipelineBindPoint::eGraphics)
			.setColorAttachments(ColorAttachmentRef)
			.setPDepthStencilAttachment(nullptr);

		vk::RenderPassCreateInfo RenderPassInfo;
		RenderPassInfo
			.setAttachments(ColorAttachment)
			.setSubpasses(SubPass);

		try
		{
			RenderPass = Device.GetHandle().createRenderPass(RenderPassInfo);
			DURIN_TRACE("Vulkan render pass created");
		}
		catch (const std::runtime_error& err)
		{
			DURIN_ERROR("Failed to create vulkan render pass: {}", err.what());
		}
	}

	FVulkanRenderPass::~FVulkanRenderPass()
	{
		Device.GetDeferredDeletionQueue().EnqueueResource(FDeferredDeletionQueue::EType::RenderPass, RenderPass);
	}

	FVulkanRenderPassManager::FVulkanRenderPassManager(FVulkanDevice& InDevice)
		: Device(InDevice)
	{
	}

	FVulkanRenderPassManager::~FVulkanRenderPassManager()
	{
		FrameBuffers.clear();
	}

	auto FVulkanRenderPassManager::GetOrCreateRenderPass(FName InRenderPassName, vk::Format InFormat) -> FVulkanRenderPass*
	{
		auto It = RenderPasses.find(InRenderPassName);
		if (It != RenderPasses.end())
		{
			return It->second.get();
		}

		const bool bIsOffscreenRenderPass = InRenderPassName != "ImGuiRenderPass" && InRenderPassName != "RuntimeSceneViewportClearPass";
		const vk::ImageLayout FinalLayout = bIsOffscreenRenderPass ? vk::ImageLayout::eShaderReadOnlyOptimal : vk::ImageLayout::ePresentSrcKHR;
		RenderPasses.emplace(InRenderPassName, std::make_unique<FVulkanRenderPass>(Device, InFormat, FinalLayout));
		return RenderPasses[InRenderPassName].get();
	}

	auto FVulkanRenderPassManager::GetOrCreateFrameBuffer(const FRHIRenderTargetsInfo& RTInfo) -> FVulkanFramebuffer*
	{
		// TODO: Support multiple color render targets later
		check(RTInfo.NumColorRenderTargets == 1);
		FVulkanTexture* RT = static_cast<FVulkanTexture*>(RTInfo.ColorRenderTargets[0]);

		for (auto& Framebuffer : FrameBuffers)
		{
			if (Framebuffer->ColorRenderTargetImages[0] == RT->Image)
			{
				return Framebuffer.get();
			}
		}
		// TODO: Render pass should be selected based on render target layout, but now we just use the first one for simplicity
		FVulkanRenderPass* TestRenderPass = RenderPasses.begin()->second.get();
		FrameBuffers.push_back(std::make_unique<FVulkanFramebuffer>(Device, RTInfo, *TestRenderPass));
		return FrameBuffers.back().get();
	}

	auto FVulkanRenderPassManager::BeginRenderPass(FVulkanCommandListContext& Context, FVulkanDevice& Device, FVulkanCommandBuffer* CmdBuffer, const FRHIRenderPassInfo& RPInfo, FVulkanRenderPass* RenderPass, FVulkanFramebuffer* Framebuffer) -> void
	{
		const FClearValueBinding& ClearValue = RPInfo.ColorClearValue;
		check(ClearValue.Binding == EClearBinding::Color);
		const vk::ClearValue VulkanClearValue{{ClearValue.ClearValue.Color[0], ClearValue.ClearValue.Color[1], ClearValue.ClearValue.Color[2], ClearValue.ClearValue.Color[3]}};
		CmdBuffer->BeginRenderPass(RenderPass, Framebuffer, VulkanClearValue);
	}

	auto FVulkanRenderPassManager::EndRenderPass(FVulkanCommandBuffer* InCmdBuffer) -> void
	{
		InCmdBuffer->EndRenderPass();
	}

	auto FVulkanRenderPassManager::NotifyDeleted_Image(vk::Image Image) -> void
	{
		auto ToErase = std::ranges::remove_if(
			FrameBuffers,
			[Image](const std::unique_ptr<FVulkanFramebuffer>& Framebuffer) {
				return Framebuffer->ContainsRenderTarget(Image);
			}
		);

		FrameBuffers.erase(ToErase.begin(), ToErase.end());
	}
} // namespace Durin::VulkanRHI
