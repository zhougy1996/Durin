#include "VulkanRenderPass.h"

#include "VulkanCommandBuffer.h"
#include "VulkanDevice.h"
#include "VulkanFramebuffer.h"
#include "VulkanRHIPrivate.h"
#include "VulkanTexture.h"

namespace Durin::VulkanRHI
{
	namespace
	{
		auto ToVulkanLoadAction(ERHIRenderTargetLoadAction Action) -> vk::AttachmentLoadOp
		{
			switch (Action)
			{
			case ERHIRenderTargetLoadAction::Load: return vk::AttachmentLoadOp::eLoad;
			case ERHIRenderTargetLoadAction::Clear: return vk::AttachmentLoadOp::eClear;
			case ERHIRenderTargetLoadAction::DontCare: return vk::AttachmentLoadOp::eDontCare;
			}
			check(false);
			return vk::AttachmentLoadOp::eDontCare;
		}

		auto ToVulkanStoreAction(ERHIRenderTargetStoreAction Action) -> vk::AttachmentStoreOp
		{
			return Action == ERHIRenderTargetStoreAction::Store ? vk::AttachmentStoreOp::eStore : vk::AttachmentStoreOp::eDontCare;
		}

		auto ToVulkanLayout(ERHITextureLayout Layout) -> vk::ImageLayout
		{
			switch (Layout)
			{
			case ERHITextureLayout::Undefined: return vk::ImageLayout::eUndefined;
			case ERHITextureLayout::ColorAttachment: return vk::ImageLayout::eColorAttachmentOptimal;
			case ERHITextureLayout::DepthStencilAttachment: return vk::ImageLayout::eDepthStencilAttachmentOptimal;
			case ERHITextureLayout::ShaderReadOnly: return vk::ImageLayout::eShaderReadOnlyOptimal;
			case ERHITextureLayout::TransferSource: return vk::ImageLayout::eTransferSrcOptimal;
			case ERHITextureLayout::TransferDestination: return vk::ImageLayout::eTransferDstOptimal;
			case ERHITextureLayout::General: return vk::ImageLayout::eGeneral;
			case ERHITextureLayout::Present: return vk::ImageLayout::ePresentSrcKHR;
			}
			check(false);
			return vk::ImageLayout::eUndefined;
		}

		auto ToVulkanSampleCount(uint8 NumSamples) -> vk::SampleCountFlagBits
		{
			switch (NumSamples)
			{
			case 1: return vk::SampleCountFlagBits::e1;
			case 2: return vk::SampleCountFlagBits::e2;
			case 4: return vk::SampleCountFlagBits::e4;
			case 8: return vk::SampleCountFlagBits::e8;
			case 16: return vk::SampleCountFlagBits::e16;
			default: checkf(false, "Unsupported render target sample count: {}", NumSamples); return vk::SampleCountFlagBits::e1;
			}
		}

		auto AccessStage(ERHIAccess Access) -> vk::PipelineStageFlags
		{
			switch (Access)
			{
			case ERHIAccess::None: return vk::PipelineStageFlagBits::eTopOfPipe;
			case ERHIAccess::ColorAttachmentReadWrite: return vk::PipelineStageFlagBits::eColorAttachmentOutput;
			case ERHIAccess::DepthStencilReadWrite: return vk::PipelineStageFlagBits::eEarlyFragmentTests | vk::PipelineStageFlagBits::eLateFragmentTests;
			case ERHIAccess::GraphicsShaderRead: return vk::PipelineStageFlagBits::eFragmentShader;
			case ERHIAccess::GraphicsShaderReadWrite: return vk::PipelineStageFlagBits::eAllGraphics;
			case ERHIAccess::Present: return vk::PipelineStageFlagBits::eBottomOfPipe;
			}
			check(false);
			return vk::PipelineStageFlagBits::eTopOfPipe;
		}

		auto AccessMask(ERHIAccess Access) -> vk::AccessFlags
		{
			switch (Access)
			{
			case ERHIAccess::None: return {};
			case ERHIAccess::ColorAttachmentReadWrite: return vk::AccessFlagBits::eColorAttachmentRead | vk::AccessFlagBits::eColorAttachmentWrite;
			case ERHIAccess::DepthStencilReadWrite: return vk::AccessFlagBits::eDepthStencilAttachmentRead | vk::AccessFlagBits::eDepthStencilAttachmentWrite;
			case ERHIAccess::GraphicsShaderRead: return vk::AccessFlagBits::eShaderRead;
			case ERHIAccess::GraphicsShaderReadWrite: return vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite;
			case ERHIAccess::Present: return vk::AccessFlagBits::eMemoryRead;
			}
			check(false);
			return {};
		}

		auto MakeAttachment(const FRHIAttachmentLayout& Layout) -> vk::AttachmentDescription
		{
			return vk::AttachmentDescription()
				.setFormat(ToVulkan_PixelFormat(Layout.Format))
				.setSamples(ToVulkanSampleCount(Layout.NumSamples))
				.setLoadOp(ToVulkanLoadAction(Layout.LoadAction))
				.setStoreOp(ToVulkanStoreAction(Layout.StoreAction))
				.setStencilLoadOp(ToVulkanLoadAction(Layout.StencilLoadAction))
				.setStencilStoreOp(ToVulkanStoreAction(Layout.StencilStoreAction))
				.setInitialLayout(ToVulkanLayout(Layout.InitialLayout))
				.setFinalLayout(ToVulkanLayout(Layout.FinalLayout));
		}

		auto AccumulateAccess(const FRHIAttachmentLayout& Attachment, vk::PipelineStageFlags& InitialStages, vk::AccessFlags& InitialAccess,
			vk::PipelineStageFlags& FinalStages, vk::AccessFlags& FinalAccess) -> void
		{
			InitialStages |= AccessStage(Attachment.InitialAccess);
			InitialAccess |= AccessMask(Attachment.InitialAccess);
			FinalStages |= AccessStage(Attachment.FinalAccess);
			FinalAccess |= AccessMask(Attachment.FinalAccess);
		}
	}

	FVulkanRenderPassKey::FVulkanRenderPassKey(const FRHIRenderTargetLayout& InLayout)
	{
		Layout.NumColorRenderTargets = InLayout.NumColorRenderTargets;
		Layout.bHasDepthStencil = InLayout.bHasDepthStencil;
		for (uint32 Index = 0; Index < Layout.NumColorRenderTargets; ++Index)
		{
			Layout.ColorAttachments[Index] = InLayout.ColorAttachments[Index];
		}
		if (Layout.bHasDepthStencil)
		{
			Layout.DepthStencilAttachment = InLayout.DepthStencilAttachment;
		}
	}

	auto FVulkanRenderPassKeyHasher::operator()(const FVulkanRenderPassKey& Key) const -> size_t
	{
		return FRHIRenderTargetLayoutHasher{}(Key.Layout);
	}

	FVulkanRenderPass::FVulkanRenderPass(FVulkanDevice& InDevice, const FVulkanRenderPassKey& InKey)
		: Device(InDevice)
		, Key(InKey)
	{
		std::vector<vk::AttachmentDescription> Attachments;
		std::vector<vk::AttachmentReference> ColorReferences;
		std::vector<vk::AttachmentReference> ResolveReferences;
		Attachments.reserve(MaxSimultaneousRenderTargets * 2 + 1);
		ColorReferences.reserve(Key.Layout.NumColorRenderTargets);
		ResolveReferences.reserve(Key.Layout.NumColorRenderTargets);

		vk::PipelineStageFlags InitialStages{};
		vk::AccessFlags InitialAccess{};
		vk::PipelineStageFlags FinalStages{};
		vk::AccessFlags FinalAccess{};
		vk::PipelineStageFlags SubpassStages{};
		vk::AccessFlags SubpassAccess{};

		for (uint32 Index = 0; Index < Key.Layout.NumColorRenderTargets; ++Index)
		{
			const auto& Color = Key.Layout.ColorAttachments[Index];
			ColorReferences.emplace_back(static_cast<uint32>(Attachments.size()), vk::ImageLayout::eColorAttachmentOptimal);
			Attachments.push_back(MakeAttachment(Color.RenderTarget));
			AccumulateAccess(Color.RenderTarget, InitialStages, InitialAccess, FinalStages, FinalAccess);
			SubpassStages |= vk::PipelineStageFlagBits::eColorAttachmentOutput;
			SubpassAccess |= vk::AccessFlagBits::eColorAttachmentRead | vk::AccessFlagBits::eColorAttachmentWrite;

			if (Color.bHasResolveTarget)
			{
				ResolveReferences.emplace_back(static_cast<uint32>(Attachments.size()), vk::ImageLayout::eColorAttachmentOptimal);
				Attachments.push_back(MakeAttachment(Color.ResolveTarget));
				AccumulateAccess(Color.ResolveTarget, InitialStages, InitialAccess, FinalStages, FinalAccess);
			}
			else
			{
				ResolveReferences.emplace_back(VK_ATTACHMENT_UNUSED, vk::ImageLayout::eUndefined);
			}
		}

		vk::AttachmentReference DepthReference;
		if (Key.Layout.bHasDepthStencil)
		{
			DepthReference = vk::AttachmentReference(static_cast<uint32>(Attachments.size()), vk::ImageLayout::eDepthStencilAttachmentOptimal);
			Attachments.push_back(MakeAttachment(Key.Layout.DepthStencilAttachment));
			AccumulateAccess(Key.Layout.DepthStencilAttachment, InitialStages, InitialAccess, FinalStages, FinalAccess);
			SubpassStages |= vk::PipelineStageFlagBits::eEarlyFragmentTests | vk::PipelineStageFlagBits::eLateFragmentTests;
			SubpassAccess |= vk::AccessFlagBits::eDepthStencilAttachmentRead | vk::AccessFlagBits::eDepthStencilAttachmentWrite;
		}

		AttachmentCount = static_cast<uint32>(Attachments.size());
		vk::SubpassDescription Subpass;
		Subpass.setPipelineBindPoint(vk::PipelineBindPoint::eGraphics).setColorAttachments(ColorReferences);
		if (std::ranges::any_of(Key.Layout.ColorAttachments | std::views::take(Key.Layout.NumColorRenderTargets), &FRHIColorAttachmentLayout::bHasResolveTarget))
		{
			Subpass.setResolveAttachments(ResolveReferences);
		}
		if (Key.Layout.bHasDepthStencil)
		{
			Subpass.setPDepthStencilAttachment(&DepthReference);
		}

		if (!InitialStages) InitialStages = vk::PipelineStageFlagBits::eTopOfPipe;
		if (!FinalStages) FinalStages = vk::PipelineStageFlagBits::eBottomOfPipe;
		std::array<vk::SubpassDependency, 2> Dependencies;
		Dependencies[0].setSrcSubpass(VK_SUBPASS_EXTERNAL).setDstSubpass(0)
			.setSrcStageMask(InitialStages).setDstStageMask(SubpassStages)
			.setSrcAccessMask(InitialAccess).setDstAccessMask(SubpassAccess)
			.setDependencyFlags(vk::DependencyFlagBits::eByRegion);
		Dependencies[1].setSrcSubpass(0).setDstSubpass(VK_SUBPASS_EXTERNAL)
			.setSrcStageMask(SubpassStages).setDstStageMask(FinalStages)
			.setSrcAccessMask(SubpassAccess).setDstAccessMask(FinalAccess)
			.setDependencyFlags(vk::DependencyFlagBits::eByRegion);

		vk::RenderPassCreateInfo CreateInfo;
		CreateInfo.setAttachments(Attachments).setSubpasses(Subpass).setDependencies(Dependencies);
#if DURIN_VULKAN_TEST_FAILURE_INJECTION
		ThrowIfVulkanNativeCreateFailureIsArmed(EVulkanCreateFailurePoint::RenderPass);
#endif
		try
		{
			RenderPass = Device.GetHandle().createRenderPass(CreateInfo);
		}
		catch (const std::exception& Error)
		{
			throw std::runtime_error(std::format(
				"Vulkan render-pass creation failed: colorAttachments={}, totalAttachments={}, error={}",
				Key.Layout.NumColorRenderTargets, Attachments.size(), Error.what()));
		}
	}

	FVulkanRenderPass::~FVulkanRenderPass()
	{
		if (RenderPass)
		{
			Device.GetDeferredDeletionQueue().EnqueueResource(FDeferredDeletionQueue::EType::RenderPass, RenderPass);
		}
	}

	FVulkanRenderPassManager::FVulkanRenderPassManager(FVulkanDevice& InDevice)
		: Device(InDevice)
	{
	}

	FVulkanRenderPassManager::~FVulkanRenderPassManager()
	{
#if DURIN_VULKAN_TEST_FAILURE_INJECTION
		GVulkanRenderPassEntryCount.fetch_sub(RenderPasses.size(), std::memory_order_release);
		GVulkanFramebufferEntryCount.fetch_sub(FrameBuffers.size(), std::memory_order_release);
#endif
		FrameBuffers.clear();
	}

	auto FVulkanRenderPassManager::GetOrCreateRenderPass(const FRHIRenderTargetLayout& InLayout) -> FVulkanRenderPass*
	{
		checkf(InLayout.IsValid(), "Cannot create a Vulkan render pass from an invalid render target layout.");
		const FVulkanRenderPassKey Key(InLayout);
		if (const auto It = RenderPasses.find(Key); It != RenderPasses.end())
		{
			return It->second.get();
		}

		auto Candidate = std::make_unique<FVulkanRenderPass>(Device, Key);
		auto [It, bInserted] = RenderPasses.emplace(Key, std::move(Candidate));
		check(bInserted);
#if DURIN_VULKAN_TEST_FAILURE_INJECTION
		GVulkanRenderPassEntryCount.fetch_add(1, std::memory_order_release);
#endif
		return It->second.get();
	}

	auto FVulkanRenderPassManager::GetOrCreateFrameBuffer(const FRHIRenderPassInfo& RPInfo, const FVulkanRenderPass& RenderPass) -> FVulkanFramebuffer*
	{
		for (auto& Framebuffer : FrameBuffers)
		{
			if (Framebuffer->IsCompatibleWith(RenderPass, RPInfo))
			{
				return Framebuffer.get();
			}
		}
		auto Candidate = std::make_unique<FVulkanFramebuffer>(Device, RPInfo, RenderPass);
		FrameBuffers.push_back(std::move(Candidate));
#if DURIN_VULKAN_TEST_FAILURE_INJECTION
		GVulkanFramebufferEntryCount.fetch_add(1, std::memory_order_release);
#endif
		return FrameBuffers.back().get();
	}

	auto FVulkanRenderPassManager::BeginRenderPass(FVulkanCommandListContext&, FVulkanDevice&, FVulkanCommandBuffer* CmdBuffer,
		const FRHIRenderPassInfo& RPInfo, FVulkanRenderPass* RenderPass, FVulkanFramebuffer* Framebuffer, FName DebugName) -> void
	{
		std::vector<vk::ClearValue> ClearValues(RenderPass->GetAttachmentCount());
		uint32 AttachmentIndex = 0;
		for (uint32 Index = 0; Index < RPInfo.RenderTargetLayout.NumColorRenderTargets; ++Index)
		{
			const auto& Clear = RPInfo.ColorClearValues[Index];
			check(Clear.Binding == EClearBinding::Color);
			ClearValues[AttachmentIndex++].setColor({Clear.ClearValue.Color[0], Clear.ClearValue.Color[1], Clear.ClearValue.Color[2], Clear.ClearValue.Color[3]});
			if (RPInfo.RenderTargetLayout.ColorAttachments[Index].bHasResolveTarget)
			{
				++AttachmentIndex;
			}
		}
		if (RPInfo.RenderTargetLayout.bHasDepthStencil)
		{
			const auto& Clear = RPInfo.DepthStencilClearValue;
			check(Clear.Binding == EClearBinding::DepthStencil);
			ClearValues[AttachmentIndex].setDepthStencil({Clear.ClearValue.DSValue.Depth, Clear.ClearValue.DSValue.Stencil});
		}
		CmdBuffer->BeginRenderPass(RenderPass, Framebuffer, ClearValues, DebugName);
	}

	auto FVulkanRenderPassManager::EndRenderPass(FVulkanCommandBuffer* InCmdBuffer) -> void
	{
		InCmdBuffer->EndRenderPass();
	}

	auto FVulkanRenderPassManager::NotifyDeleted_Image(vk::Image Image) -> void
	{
		const size_t PreviousSize = FrameBuffers.size();
		auto ToErase = std::ranges::remove_if(FrameBuffers, [Image](const std::unique_ptr<FVulkanFramebuffer>& Framebuffer) {
			return Framebuffer->ContainsRenderTarget(Image);
		});
		FrameBuffers.erase(ToErase.begin(), ToErase.end());
#if DURIN_VULKAN_TEST_FAILURE_INJECTION
		GVulkanFramebufferEntryCount.fetch_sub(
			PreviousSize - FrameBuffers.size(), std::memory_order_release);
#endif
	}
} // namespace Durin::VulkanRHI
