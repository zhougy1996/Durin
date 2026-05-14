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

namespace Doge::VulkanRHI
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
		AppendShaderStageCreateInfo(ShaderStages, vk::ShaderStageFlagBits::eFragment, BoundShaders.PixelShader);

		return ShaderStages;
	}

	FVulkanGraphicsPipelineState::FVulkanGraphicsPipelineState(FVulkanDevice& InDevice, const FGraphicsPipelineStateInitializer& Initializer)
		: Device(InDevice)
	{
		// TODO: Correctly set the render pass
		FVulkanRenderPassManager& RenderPassManager = Device.GetRenderPassManager();

		vk::Format VkFormat = ConvertToVulkanFormat(Initializer.PixelFormat);
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
				.setFormat(ConvertToVulkanFormat(Element.Type))
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
		RasterizerInfo
			.setDepthClampEnable(vk::False)
			.setLineWidth(1.0f)
			.setRasterizerDiscardEnable(vk::False)
			.setPolygonMode(vk::PolygonMode::eFill)
			.setCullMode(vk::CullModeFlagBits::eBack)
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
		ColorBlendAttachment
			.setColorWriteMask(vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG | vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA)
			.setBlendEnable(vk::False)
			.setDstColorBlendFactor(vk::BlendFactor::eOneMinusSrcAlpha)
			.setColorBlendOp(vk::BlendOp::eAdd)
			.setSrcAlphaBlendFactor(vk::BlendFactor::eOne)
			.setDstAlphaBlendFactor(vk::BlendFactor::eZero)
			.setAlphaBlendOp(vk::BlendOp::eAdd);

		vk::PipelineColorBlendStateCreateInfo ColorBlending;
		ColorBlending
			.setLogicOpEnable(vk::False)
			.setLogicOp(vk::LogicOp::eCopy)
			.setAttachments(ColorBlendAttachment)
			.setBlendConstants({0.0f, 0.0f, 0.0f, 0.0f});

		std::vector<vk::DescriptorSetLayoutBinding> LayoutBindings;

		vk::DescriptorSetLayoutBinding ImageLayoutBinding;
		ImageLayoutBinding.setBinding(0)
			.setDescriptorType(vk::DescriptorType::eSampledImage)
			.setStageFlags(vk::ShaderStageFlagBits::eFragment)
			.setDescriptorCount(1);
		LayoutBindings.push_back(ImageLayoutBinding);

		vk::DescriptorSetLayoutBinding SamplerLayoutBinding;
		SamplerLayoutBinding.setBinding(1)
			.setDescriptorType(vk::DescriptorType::eSampler)
			.setStageFlags(vk::ShaderStageFlagBits::eFragment)
			.setDescriptorCount(1);
		LayoutBindings.push_back(SamplerLayoutBinding);

		vk::DescriptorSetLayoutCreateInfo LayoutInfo;
		LayoutInfo.setBindings(LayoutBindings);

		DescriptorSetLayout = Device.GetHandle().createDescriptorSetLayout(LayoutInfo);

		std::vector<vk::PushConstantRange> PushConstantRanges;
		for (const auto& PushConstantRange : Initializer.PushConstantRanges)
		{
			vk::PushConstantRange NewPushConstantRange;
			NewPushConstantRange
				.setStageFlags(ConvertToVulkanType(PushConstantRange.StageFlags))
				.setOffset(PushConstantRange.Offset)
				.setSize(PushConstantRange.Size);
			PushConstantRanges.push_back(NewPushConstantRange);
		}

		vk::PipelineLayoutCreateInfo pipelineLayoutInfo;
		pipelineLayoutInfo.setSetLayouts(DescriptorSetLayout);
		pipelineLayoutInfo.setPushConstantRanges(PushConstantRanges);

		PipelineLayout = Device.GetHandle().createPipelineLayout(pipelineLayoutInfo);
		DOGE_TRACE("Vulkan pipeline layout created");

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
			DOGE_ERROR("Failed to create vulkan graphics pipeline: {}", vk::to_string(PipelineCreationResult.result));
			KeepShadersAlive();
		}
		else
		{
			Pipeline = PipelineCreationResult.value;
			DOGE_TRACE("Vulkan graphics pipeline created");
		}

		auto DescriptorPool = Device.GetGlobalDescriptorPool().GetHandle();
		std::vector<vk::DescriptorSetLayout> Layouts(kFrameInFlight, DescriptorSetLayout);
		vk::DescriptorSetAllocateInfo DescriptorSetAllocInfo;
		DescriptorSetAllocInfo
			.setDescriptorPool(DescriptorPool)
			.setSetLayouts(Layouts);

		DescriptorSets = Device.GetHandle().allocateDescriptorSets(DescriptorSetAllocInfo);
	}

	FVulkanGraphicsPipelineState::~FVulkanGraphicsPipelineState()
	{
		ReleaseShaders();
		DescriptorSets.clear();
		Device.GetDeferredDeletionQueue().EnqueueResource(FDeferredDeletionQueue::EType::DescriptorSetLayout, DescriptorSetLayout);
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
	}

	auto FVulkanGraphicsPipelineState::SetUniformBuffer(FVulkanCommandListContext& InContext, FRHIShader* InShader, uint32 SetIndex, uint32 BindIndex, FVulkanBuffer* InUniformBuffer) -> void
	{
		FVulkanCommandBuffer* CmdBuffer = InContext.GetCommandBuffer();
		vk::DescriptorBufferInfo bufferInfo{};
		bufferInfo.setBuffer(InUniformBuffer->GetHandle())
				  .setOffset(0)
				  .setRange(InUniformBuffer->GetSize());

		vk::WriteDescriptorSet descriptorWrite{};
		descriptorWrite.setDstSet(DescriptorSets[GFrameCounterRenderThread % kFrameInFlight])
					   .setDstBinding(BindIndex)
					   .setDstArrayElement(0)
					   .setDescriptorType(vk::DescriptorType::eUniformBuffer)
					   .setDescriptorCount(1)
					   .setBufferInfo(bufferInfo);

		Device.GetHandle().updateDescriptorSets(1, &descriptorWrite, 0, nullptr);

		CmdBuffer->GetHandle().bindDescriptorSets(vk::PipelineBindPoint::eGraphics, PipelineLayout, 0, DescriptorSets[GFrameCounterRenderThread % kFrameInFlight], {});
	}

	auto FVulkanGraphicsPipelineState::SetTexture(FVulkanCommandListContext& InContext, uint32 BindIndex, FVulkanTexture* InTexture) -> void
	{
		vk::DescriptorImageInfo imageInfo{};
		imageInfo.setImageView(InTexture->ImageView)
				 .setImageLayout(vk::ImageLayout::eShaderReadOnlyOptimal);

		vk::WriteDescriptorSet descriptorWrite{};
		descriptorWrite.setDstSet(DescriptorSets[GFrameCounterRenderThread % kFrameInFlight])
					   .setDstBinding(BindIndex)
					   .setDstArrayElement(0)
					   .setDescriptorType(vk::DescriptorType::eSampledImage)
					   .setDescriptorCount(1)
					   .setImageInfo(imageInfo);

		Device.GetHandle().updateDescriptorSets(1, &descriptorWrite, 0, nullptr);
	}

	auto FVulkanGraphicsPipelineState::SetSampler(FVulkanCommandListContext& InContext, uint32 BindIndex, vk::Sampler InSampler) -> void
	{
		vk::DescriptorImageInfo imageInfo{};
		imageInfo.setSampler(InSampler);

		vk::WriteDescriptorSet descriptorWrite{};
		descriptorWrite.setDstSet(DescriptorSets[GFrameCounterRenderThread % kFrameInFlight])
					   .setDstBinding(BindIndex)
					   .setDstArrayElement(0)
					   .setDescriptorType(vk::DescriptorType::eSampler)
					   .setDescriptorCount(1)
					   .setImageInfo(imageInfo);

		Device.GetHandle().updateDescriptorSets(1, &descriptorWrite, 0, nullptr);
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

	FVulkanPipelineManager::FVulkanPipelineManager(FVulkanDevice& InDevice)
		: Device(InDevice)
	{
	}

	FVulkanPipelineManager::~FVulkanPipelineManager()
	{
		PSOCache.clear();
	}

	auto FVulkanPipelineManager::GetGraphicsPipelineState(FName Name) -> TRefCountPtr<FVulkanGraphicsPipelineState>
	{
		const auto It = PSOCache.find(Name);
		if (It != PSOCache.end())
		{
			return It->second;
		}

		return nullptr;
	}

	auto FVulkanPipelineManager::CreateGraphicsPipelineState(FName Name, const FGraphicsPipelineStateInitializer& Initializer) -> TRefCountPtr<FVulkanGraphicsPipelineState>
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

	auto FVulkanDynamicRHI::RHICreateGraphicsPipelineState(FName Name, const FGraphicsPipelineStateInitializer& Initializer) -> TRefCountPtr<FRHIGraphicsPipelineState>
	{
		return Device->GetPipelineManager().CreateGraphicsPipelineState(Name, Initializer);
	}

	auto FVulkanDynamicRHI::RHIGetGraphicsPipelineState(FName Name) -> TRefCountPtr<FRHIGraphicsPipelineState>
	{
		return Device->GetPipelineManager().GetGraphicsPipelineState(Name);
	}
} // namespace Doge::VulkanRHI