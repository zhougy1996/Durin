#include "VulkanPipeline.h"

#include "VulkanBuffer.h"
#include "VulkanCommandBuffer.h"
#include "VulkanContext.h"
#include "VulkanDynamicRHI.h"
#include "VulkanDevice.h"
#include "VulkanRHIPrivate.h"
#include "VulkanRenderPass.h"
#include "VulkanShader.h"
#include "VulkanTexture.h"
#include "VulkanDescriptorSets.h"

namespace Durin::VulkanRHI
{
	static auto AppendShaderStageCreateInfo(std::vector<vk::PipelineShaderStageCreateInfo>& ShaderStages, vk::ShaderStageFlagBits ShaderStage, const FRHIShader* ShaderRHI) -> void
	{
		if (ShaderRHI)
		{
			const FVulkanShader* VulkanShader = static_cast<const FVulkanShader*>(ShaderRHI);
			vk::PipelineShaderStageCreateInfo ShaderStageInfo;
			ShaderStageInfo
				.setStage(ShaderStage)
				.setModule(VulkanShader->GetShaderModule())
				.setPName(VulkanShader->GetEntryPoint());

			ShaderStages.push_back(ShaderStageInfo);
		}
	}

	static std::vector<vk::PipelineShaderStageCreateInfo> MakeShaderStageCreateInfos(const FBoundShaders& BoundShaders)
	{
		std::vector<vk::PipelineShaderStageCreateInfo> ShaderStages;

		AppendShaderStageCreateInfo(ShaderStages, vk::ShaderStageFlagBits::eVertex, BoundShaders.VertexShader);
		AppendShaderStageCreateInfo(ShaderStages, vk::ShaderStageFlagBits::eFragment, BoundShaders.FragmentShader);

		return ShaderStages;
	}

	static auto CreateDescriptorSetLayout(FVulkanDevice& Device, const FBindingLayout& InDesc) -> vk::DescriptorSetLayout
	{
		vk::DescriptorSetLayoutCreateInfo LayoutInfo;

		std::vector<vk::DescriptorSetLayoutBinding> Bindings;

		for (const auto& Item : InDesc.BindingLayouts)
		{
			vk::DescriptorSetLayoutBinding Binding;
			Binding
				.setStageFlags(ToVulkan_ShaderStageFlags(Item.StageFlags))
				.setBinding(Item.Slot)
				.setDescriptorCount(Item.ArraySize)
				.setDescriptorType(ToVulkan_RHIBindingType(Item.Type));

			Bindings.push_back(Binding);
		}
		LayoutInfo.setBindings(Bindings);

		return Device.GetHandle().createDescriptorSetLayout(LayoutInfo);
	}

	static auto CreatePushConstantRanges(const FPipelineLayoutDesc Desc) -> std::vector<vk::PushConstantRange>
	{
		std::vector<vk::PushConstantRange> Ranges;
		for (const auto& RangeDesc : Desc.PushConstantRanges)
		{
			vk::PushConstantRange Range;
			Range
				.setStageFlags(ToVulkan_ShaderStageFlags(RangeDesc.StageFlags))
				.setOffset(RangeDesc.Offset)
				.setSize(RangeDesc.Size);

			Ranges.push_back(Range);
		}
		return Ranges;
	}

	FVulkanGraphicsPipelineState::FVulkanGraphicsPipelineState(FVulkanDevice& InDevice, const FGraphicsPipelineStateInitializer& Initializer)
		: Device(InDevice)
	{
		// TODO: Correctly set the render pass
		FVulkanRenderPassManager& RenderPassManager = Device.GetRenderPassManager();

		vk::Format VkFormat = ToVulkan_PixelFormat(Initializer.PixelFormat);
		RenderPass = RenderPassManager.GetOrCreateRenderPass(Initializer.RenderPassName, VkFormat);

		std::vector<vk::PipelineShaderStageCreateInfo> ShaderStages = MakeShaderStageCreateInfos(Initializer.BoundShaders);

		std::vector<vk::DynamicState> DynamicStates = {
			vk::DynamicState::eViewport,
			vk::DynamicState::eScissor
		};

		vk::PipelineDynamicStateCreateInfo DynamicStateInfo;
		DynamicStateInfo.setDynamicStates(DynamicStates);

		std::vector<vk::VertexInputBindingDescription> BindingDescriptions;
		std::vector<vk::VertexInputAttributeDescription> AttributeDescriptions;

		const FRHIVertexDeclaration* VertexDeclarationElements = Initializer.VertexDeclaration;
		for (auto& Element : VertexDeclarationElements->GetElements())
		{
			if (Element.Type == EVertexElementType::None)
			{
				break;
			}

			// Check if this stream index already has a binding
			bool bBindingExists = false;
			for (auto& Existing : BindingDescriptions)
			{
				if (Existing.binding == Element.StreamIndex)
				{
					bBindingExists = true;
					break;
				}
			}
			if (!bBindingExists)
			{
				vk::VertexInputBindingDescription BindingDescription;
				BindingDescription
					.setBinding(Element.StreamIndex)
					.setStride(Element.Stride)
					.setInputRate(vk::VertexInputRate::eVertex);
				BindingDescriptions.push_back(BindingDescription);
			}

			vk::VertexInputAttributeDescription AttributeDescription;
			AttributeDescription
				.setLocation(Element.AttributeIndex)
				.setBinding(Element.StreamIndex)
				.setFormat(ToVulkan_VertexElementType(Element.Type))
				.setOffset(Element.Offset);

			AttributeDescriptions.push_back(AttributeDescription);
		}

		vk::PipelineVertexInputStateCreateInfo VertexInputInfo;
		VertexInputInfo
			.setVertexBindingDescriptions(BindingDescriptions)
			.setVertexAttributeDescriptions(AttributeDescriptions);

		vk::PipelineInputAssemblyStateCreateInfo InputAssemblyInfo;
		InputAssemblyInfo
			.setTopology(vk::PrimitiveTopology::eTriangleList)
			.setPrimitiveRestartEnable(vk::False);

		// Viewports and scissors will be set dynamically, so we don't need to specify them here, but we still need to specify the count
		vk::PipelineViewportStateCreateInfo ViewportStateInfo;
		ViewportStateInfo.setViewportCount(1).setScissorCount(1);

		vk::PipelineRasterizationStateCreateInfo RasterizerInfo;
		const vk::CullModeFlags CullMode = Initializer.bEnableBackFaceCulling ? vk::CullModeFlagBits::eBack : vk::CullModeFlagBits::eNone;
		RasterizerInfo
			.setDepthClampEnable(vk::False)
			.setLineWidth(1.0f)
			.setRasterizerDiscardEnable(vk::False)
			.setPolygonMode(vk::PolygonMode::eFill)
			.setCullMode(CullMode)
			.setFrontFace(vk::FrontFace::eClockwise)
			.setDepthBiasEnable(vk::False)
			.setDepthBiasConstantFactor(0.0f)
			.setDepthBiasClamp(0.0f)
			.setDepthBiasSlopeFactor(0.0f);

		vk::PipelineMultisampleStateCreateInfo MultiSamplingInfo;
		MultiSamplingInfo
			.setSampleShadingEnable(vk::False)
			.setRasterizationSamples(vk::SampleCountFlagBits::e1)
			.setMinSampleShading(1.0f)
			.setPSampleMask(nullptr)
			.setAlphaToCoverageEnable(vk::False)
			.setAlphaToOneEnable(vk::False);

		vk::PipelineColorBlendAttachmentState ColorBlendAttachment;
		const bool bEnableAlphaBlend = Initializer.bEnableAlphaBlend;
		ColorBlendAttachment
			.setColorWriteMask(vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG | vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA)
			.setBlendEnable(bEnableAlphaBlend ? vk::True : vk::False)
			.setSrcColorBlendFactor(bEnableAlphaBlend ? vk::BlendFactor::eSrcAlpha : vk::BlendFactor::eOne)
			.setDstColorBlendFactor(vk::BlendFactor::eOneMinusSrcAlpha)
			.setColorBlendOp(vk::BlendOp::eAdd)
			.setSrcAlphaBlendFactor(vk::BlendFactor::eOne)
			.setDstAlphaBlendFactor(bEnableAlphaBlend ? vk::BlendFactor::eOneMinusSrcAlpha : vk::BlendFactor::eZero)
			.setAlphaBlendOp(vk::BlendOp::eAdd);

		vk::PipelineColorBlendStateCreateInfo ColorBlending;
		ColorBlending
			.setLogicOpEnable(vk::False)
			.setLogicOp(vk::LogicOp::eCopy)
			.setAttachments(ColorBlendAttachment)
			.setBlendConstants({0.0f, 0.0f, 0.0f, 0.0f});

		// Create pipeline layout
		FVulkanDescriptorSetsLayoutInfo DescriptorSetsLayoutInfo(Initializer.PipelineLayout.BindingLayouts);
		Layout = Device.GetPipelineManager().FindOrAddLayout(DescriptorSetsLayoutInfo);
		std::vector<vk::PushConstantRange> PushConstantRanges = CreatePushConstantRanges(Initializer.PipelineLayout);

		vk::PipelineLayoutCreateInfo pipelineLayoutInfo;
		pipelineLayoutInfo.setSetLayouts(Layout->GetDescriptorSetsLayout().GetLayoutHandles());
		pipelineLayoutInfo.setPushConstantRanges(PushConstantRanges);

		PipelineLayout = Device.GetHandle().createPipelineLayout(pipelineLayoutInfo);
		DURIN_TRACE("Vulkan pipeline layout created");

		vk::GraphicsPipelineCreateInfo pipelineInfo;
		pipelineInfo
			.setStages(ShaderStages)
			.setPDynamicState(&DynamicStateInfo)
			.setPVertexInputState(&VertexInputInfo)
			.setPInputAssemblyState(&InputAssemblyInfo)
			.setPViewportState(&ViewportStateInfo)
			.setPRasterizationState(&RasterizerInfo)
			.setPMultisampleState(&MultiSamplingInfo)
			.setPColorBlendState(&ColorBlending)
			.setLayout(PipelineLayout)
			.setRenderPass(RenderPass->GetHandle())
			.setSubpass(0)
			.setBasePipelineHandle(nullptr)
			.setBasePipelineIndex(-1);

		vk::ResultValue<vk::Pipeline> PipelineCreationResult = Device.GetHandle().createGraphicsPipeline(nullptr, pipelineInfo);
		if (PipelineCreationResult.result != vk::Result::eSuccess)
		{
			DURIN_ERROR("Failed to create vulkan graphics pipeline: {}", vk::to_string(PipelineCreationResult.result));
			KeepShadersAlive();
		}
		else
		{
			Pipeline = PipelineCreationResult.value;
			DURIN_TRACE("Vulkan graphics pipeline created");
		}
	}

	FVulkanGraphicsPipelineState::~FVulkanGraphicsPipelineState()
	{
		ReleaseShaders();
		Device.GetDeferredDeletionQueue().EnqueueResource(FDeferredDeletionQueue::EType::PipelineLayout, PipelineLayout);
		Device.GetDeferredDeletionQueue().EnqueueResource(FDeferredDeletionQueue::EType::Pipeline, Pipeline);
	}

	auto FVulkanGraphicsPipelineState::SetViewport(float MinX, float MinY, float MinZ, float MaxX, float MaxY, float MaxZ) -> void
	{
		float MaxDepth = MinZ == MaxZ ? MinZ + 1.0f : MaxZ;

		Viewport
			.setX(MinX)
			.setY(MinY)
			.setWidth(MaxX - MinX)
			.setHeight(MaxY - MinY)
			.setMinDepth(MinZ)
			.setMaxDepth(MaxDepth);

		SetScissorRect(static_cast<uint32>(MinX), static_cast<uint32>(MinY), static_cast<uint32>(MaxX - MinX), static_cast<uint32>(MaxY - MinY));
		bScissorEnabled = false;
	}

	auto FVulkanGraphicsPipelineState::SetScissor(float MinX, float MinY, float Width, float Height) -> void
	{
		SetScissorRect(static_cast<uint32>(MinX), static_cast<uint32>(MinY), static_cast<uint32>(Width), static_cast<uint32>(Height));
		bScissorEnabled = true;
	}

	auto FVulkanGraphicsPipelineState::Bind(vk::CommandBuffer CmdBuffer) -> void
	{
		CmdBuffer.bindPipeline(vk::PipelineBindPoint::eGraphics, Pipeline);
	}

	auto FVulkanGraphicsPipelineState::PrepareForDraw(FVulkanCommandListContext& InContext) -> void
	{
		FVulkanCommandBuffer* CmdBuffer = InContext.GetCommandBuffer();
		CmdBuffer->GetHandle().setViewport(0, Viewport);
		CmdBuffer->GetHandle().setScissor(0, Scissor);

		Device.GetHandle().updateDescriptorSets(DescriptorWrites, {});
		DescriptorWrites.clear();
		ImageInfos.clear();
		CmdBuffer->GetHandle().bindDescriptorSets(vk::PipelineBindPoint::eGraphics, PipelineLayout, 0, DescriptorSets, {});
	}

	auto FVulkanGraphicsPipelineState::PushConstants(FVulkanCommandListContext& InContext, EShaderStageFlags StageFlags, uint32 Offset, uint32 Size, const void* pValues) const -> void
	{
		FVulkanCommandBuffer* CmdBuffer = InContext.GetCommandBuffer();
		CmdBuffer->GetHandle().pushConstants(PipelineLayout, ToVulkan_ShaderStageFlags(StageFlags), Offset, Size, pValues);
	}

	auto FVulkanGraphicsPipelineState::SetUniformBuffer(FVulkanCommandListContext& InContext, FRHIShader* InShader, uint32 SetIndex, uint32 BindIndex, FVulkanBuffer* InUniformBuffer) -> void
	{
		vk::DescriptorBufferInfo BufferInfo{};
		BufferInfo.setBuffer(InUniformBuffer->GetHandle())
			.setOffset(0)
			.setRange(InUniformBuffer->GetSize());

		vk::WriteDescriptorSet DescriptorWrite{};
		DescriptorWrite.setDstSet(DescriptorSets[SetIndex])
			.setDstBinding(BindIndex)
			.setDstArrayElement(0)
			.setDescriptorType(vk::DescriptorType::eUniformBuffer)
			.setDescriptorCount(1)
			.setBufferInfo(BufferInfo);

		DescriptorWrites.push_back(DescriptorWrite);
	}

	auto FVulkanGraphicsPipelineState::SetTexture(FVulkanCommandListContext& InContext, uint32 SetIndex, uint32 BindIndex, FVulkanTexture* InTexture) -> void
	{
		ImageInfos.emplace_back();
		vk::DescriptorImageInfo& ImageInfo = ImageInfos.back();
		ImageInfo.setImageView(InTexture->ImageView)
			.setImageLayout(vk::ImageLayout::eShaderReadOnlyOptimal);

		vk::WriteDescriptorSet DescriptorWrite{};
		DescriptorWrite.setDstSet(DescriptorSets[SetIndex])
			.setDstBinding(BindIndex)
			.setDstArrayElement(0)
			.setDescriptorType(vk::DescriptorType::eSampledImage)
			.setDescriptorCount(1)
			.setImageInfo(ImageInfo);

		DescriptorWrites.push_back(DescriptorWrite);
	}

	auto FVulkanGraphicsPipelineState::SetSampler(FVulkanCommandListContext& InContext, uint32 SetIndex, uint32 BindIndex, FVulkanSampler* InSampler) -> void
	{
		ImageInfos.emplace_back();
		vk::DescriptorImageInfo& ImageInfo = ImageInfos.back();
		ImageInfo.setSampler(InSampler->GetHandle());

		vk::WriteDescriptorSet DescriptorWrite{};
		DescriptorWrite.setDstSet(DescriptorSets[SetIndex])
			.setDstBinding(BindIndex)
			.setDstArrayElement(0)
			.setDescriptorType(vk::DescriptorType::eSampler)
			.setDescriptorCount(1)
			.setImageInfo(ImageInfo);

		DescriptorWrites.push_back(DescriptorWrite);
	}

	auto FVulkanGraphicsPipelineState::SetScissorRect(uint32 MinX, uint32 MinY, uint32 Width, uint32 Height) -> void
	{
		Scissor
			.setOffset({static_cast<int32>(MinX), static_cast<int32>(MinY)})
			.setExtent({Width, Height});
	}

	auto FVulkanGraphicsPipelineState::KeepShadersAlive() -> void
	{
		for (FVulkanShader* Shader : Shaders)
		{
			if (Shader)
			{
				(void)Shader->AddRef();
			}
		}
	}

	auto FVulkanGraphicsPipelineState::ReleaseShaders() -> void
	{
		for (FVulkanShader* Shader : Shaders)
		{
			if (Shader)
			{
				(void)Shader->Release();
			}
		}
	}

	auto FVulkanGraphicsPipelineState::PrepareDescriptorSets() -> void
	{
		auto DescriptorPool = Device.GetGlobalDescriptorPool().GetPool();
		vk::DescriptorSetAllocateInfo DescriptorSetAllocInfo;
		DescriptorSetAllocInfo
			.setDescriptorPool(DescriptorPool)
			.setSetLayouts(Layout->GetDescriptorSetsLayout().GetLayoutHandles());

		DescriptorSets = Device.GetHandle().allocateDescriptorSets(DescriptorSetAllocInfo);
	}

	FVulkanPipelineStateCacheManager::FVulkanPipelineStateCacheManager(FVulkanDevice& InDevice)
		: Device(InDevice)
	{
	}

	FVulkanPipelineStateCacheManager::~FVulkanPipelineStateCacheManager()
	{
		PSOCache.clear();
		for (const auto& Layout : LayoutMap | std::views::values)
		{
			// LayoutMap owns the FVulkanLayout objects; clean them up.
			// The layouts' descriptor set handles will be released by the
			// DescriptorSetLayoutCache (owned by FVulkanDevice) which outlives us.
			delete Layout;
		}
		LayoutMap.clear();
	}

	auto FVulkanPipelineStateCacheManager::GetGraphicsPipelineState(FName Name) -> TRefCountPtr<FVulkanGraphicsPipelineState>
	{
		const auto It = PSOCache.find(Name);
		if (It != PSOCache.end())
		{
			return It->second;
		}
		return nullptr;
	}

	auto FVulkanPipelineStateCacheManager::CreateGraphicsPipelineState(FName Name, const FGraphicsPipelineStateInitializer& Initializer) -> TRefCountPtr<FVulkanGraphicsPipelineState>
	{
		const auto It = PSOCache.find(Name);
		if (It != PSOCache.end())
		{
			return It->second;
		}

		auto NewPSO = MakeRefCount<FVulkanGraphicsPipelineState>(Device, Initializer);
		PSOCache[Name] = NewPSO;
		return NewPSO;
	}

	auto FVulkanPipelineStateCacheManager::FindOrAddLayout(const FVulkanDescriptorSetsLayoutInfo& LayoutInfo) -> FVulkanLayout*
	{
		const auto It = LayoutMap.find(LayoutInfo);
		if (It != LayoutMap.end())
		{
			return It->second;
		}

		FVulkanLayout* NewLayout = new FVulkanLayout(Device);
		NewLayout->DSetsLayout = FVulkanDescriptorSetsLayout(Device, LayoutInfo);
		LayoutMap[LayoutInfo] = NewLayout;
		return NewLayout;
	}

	auto FVulkanDynamicRHI::RHICreateGraphicsPipelineState(FName Name, const FGraphicsPipelineStateInitializer& Initializer) -> TRefCountPtr<FRHIGraphicsPipelineState>
	{
		return Device->GetPipelineManager().CreateGraphicsPipelineState(Name, Initializer);
	}

	auto FVulkanDynamicRHI::RHIGetGraphicsPipelineState(FName Name) -> TRefCountPtr<FRHIGraphicsPipelineState>
	{
		return Device->GetPipelineManager().GetGraphicsPipelineState(Name);
	}
} // namespace Durin::VulkanRHI