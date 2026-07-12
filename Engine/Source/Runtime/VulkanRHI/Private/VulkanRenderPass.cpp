#include "VulkanRenderPass.h"

#include "VulkanDevice.h"
#include "VulkanCommandBuffer.h"
#include "VulkanFramebuffer.h"
#include "VulkanTexture.h"

namespace Durin::VulkanRHI
{
	FVulkanRenderPass::FVulkanRenderPass(FVulkanDevice& InDevice, vk::Format InFormat, vk::ImageLayout InFinalLayout, vk::Format InDepthFormat)
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

		vk::AttachmentDescription DepthAttachment;
		DepthAttachment.setSamples(vk::SampleCountFlagBits::e1)
			.setFormat(InDepthFormat)
			.setLoadOp(vk::AttachmentLoadOp::eClear)
			.setStoreOp(vk::AttachmentStoreOp::eDontCare)
			.setStencilLoadOp(vk::AttachmentLoadOp::eDontCare)
			.setStencilStoreOp(vk::AttachmentStoreOp::eDontCare)
			.setInitialLayout(vk::ImageLayout::eUndefined)
			.setFinalLayout(vk::ImageLayout::eDepthStencilAttachmentOptimal);
		vk::AttachmentReference DepthAttachmentRef(1, vk::ImageLayout::eDepthStencilAttachmentOptimal);

		vk::SubpassDescription SubPass;
		SubPass
			.setPipelineBindPoint(vk::PipelineBindPoint::eGraphics)
			.setColorAttachments(ColorAttachmentRef);
		if (InDepthFormat != vk::Format::eUndefined)
		{
			SubPass.setPDepthStencilAttachment(&DepthAttachmentRef);
		}

		std::array<vk::SubpassDependency, 2> OffscreenDependencies;
		OffscreenDependencies[0]
			.setSrcSubpass(VK_SUBPASS_EXTERNAL)
			.setDstSubpass(0)
			.setSrcStageMask(vk::PipelineStageFlagBits::eFragmentShader)
			.setDstStageMask(vk::PipelineStageFlagBits::eColorAttachmentOutput)
			.setSrcAccessMask(vk::AccessFlagBits::eShaderRead)
			.setDstAccessMask(vk::AccessFlagBits::eColorAttachmentWrite)
			.setDependencyFlags(vk::DependencyFlagBits::eByRegion);
		OffscreenDependencies[1]
			.setSrcSubpass(0)
			.setDstSubpass(VK_SUBPASS_EXTERNAL)
			.setSrcStageMask(vk::PipelineStageFlagBits::eColorAttachmentOutput)
			.setDstStageMask(vk::PipelineStageFlagBits::eFragmentShader)
			.setSrcAccessMask(vk::AccessFlagBits::eColorAttachmentWrite)
			.setDstAccessMask(vk::AccessFlagBits::eShaderRead)
			.setDependencyFlags(vk::DependencyFlagBits::eByRegion);

		vk::RenderPassCreateInfo RenderPassInfo;
		std::array<vk::AttachmentDescription, 2> Attachments{ColorAttachment, DepthAttachment};
		RenderPassInfo.setAttachmentCount(InDepthFormat == vk::Format::eUndefined ? 1u : 2u)
			.setPAttachments(Attachments.data())
			.setSubpasses(SubPass);
		if (InFinalLayout == vk::ImageLayout::eShaderReadOnlyOptimal)
		{
			RenderPassInfo.setDependencies(OffscreenDependencies);
		}

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

	auto FVulkanRenderPassManager::GetOrCreateRenderPass(FName InRenderPassName, vk::Format InFormat, vk::Format InDepthFormat) -> FVulkanRenderPass*
	{
		auto It = RenderPasses.find(InRenderPassName);
		if (It != RenderPasses.end())
		{
			return It->second.get();
		}

		const bool bIsPresentRenderPass = InRenderPassName == "ImGuiRenderPass"
			|| InRenderPassName == "RuntimeSceneViewportClearPass"
			|| InRenderPassName == "PostProcessPresentRenderPass";
		const bool bIsOffscreenRenderPass = !bIsPresentRenderPass;
		const vk::ImageLayout FinalLayout = bIsOffscreenRenderPass ? vk::ImageLayout::eShaderReadOnlyOptimal : vk::ImageLayout::ePresentSrcKHR;
		RenderPasses.emplace(InRenderPassName, std::make_unique<FVulkanRenderPass>(Device, InFormat, FinalLayout, InDepthFormat));
		return RenderPasses[InRenderPassName].get();
	}

	auto FVulkanRenderPassManager::GetOrCreateFrameBuffer(const FRHIRenderTargetsInfo& RTInfo, const FVulkanRenderPass& RenderPass) -> FVulkanFramebuffer*
	{
		// TODO: Support multiple color render targets later
		check(RTInfo.NumColorRenderTargets == 1);
		FVulkanTexture* RT = static_cast<FVulkanTexture*>(RTInfo.ColorRenderTargets[0]);
		const vk::Image DepthImage = RTInfo.DepthStencilRenderTarget != nullptr
			? static_cast<FVulkanTexture*>(RTInfo.DepthStencilRenderTarget)->Image : vk::Image{};

		for (auto& Framebuffer : FrameBuffers)
		{
			if (Framebuffer->IsCompatibleWith(RenderPass, RT->Image, DepthImage))
			{
				return Framebuffer.get();
			}
		}
		FrameBuffers.push_back(std::make_unique<FVulkanFramebuffer>(Device, RTInfo, RenderPass));
		return FrameBuffers.back().get();
	}

	auto FVulkanRenderPassManager::BeginRenderPass(FVulkanCommandListContext& Context, FVulkanDevice& Device, FVulkanCommandBuffer* CmdBuffer, const FRHIRenderPassInfo& RPInfo, FVulkanRenderPass* RenderPass, FVulkanFramebuffer* Framebuffer) -> void
	{
		const FClearValueBinding& ClearValue = RPInfo.ColorClearValue;
		check(ClearValue.Binding == EClearBinding::Color);
		std::array<vk::ClearValue, 2> VulkanClearValues;
		VulkanClearValues[0].setColor({ClearValue.ClearValue.Color[0], ClearValue.ClearValue.Color[1], ClearValue.ClearValue.Color[2], ClearValue.ClearValue.Color[3]});
		uint32 ClearValueCount = 1;
		if (RPInfo.DepthStencilRenderTarget != nullptr)
		{
			const auto& DepthClear = RPInfo.DepthStencilClearValue;
			check(DepthClear.Binding == EClearBinding::DepthStencil);
			VulkanClearValues[1].setDepthStencil({DepthClear.ClearValue.DSValue.Depth, DepthClear.ClearValue.DSValue.Stencil});
			ClearValueCount = 2;
		}
		CmdBuffer->BeginRenderPass(RenderPass, Framebuffer, std::span(VulkanClearValues).first(ClearValueCount));
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
