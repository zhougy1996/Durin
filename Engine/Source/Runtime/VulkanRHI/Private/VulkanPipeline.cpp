#include "VulkanPipeline.h"

#include "RHICommandList.h"
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

	static auto GetPipelineInitializerPayloadBytes(
		const FGraphicsPipelineStateInitializer& Initializer) -> size_t
	{
		size_t Bytes = Initializer.PipelineLayout.BindingLayouts.size()
			* sizeof(FBindingLayout);
		Bytes += Initializer.PipelineLayout.PushConstantRanges.size()
			* sizeof(FPushConstantRange);
		for (const FBindingLayout& Layout :
			Initializer.PipelineLayout.BindingLayouts)
		{
			Bytes += Layout.BindingLayouts.size()
				* sizeof(FBindingLayoutItem);
		}
		return Bytes;
	}

	static auto ToVulkan_PrimitiveTopology(FGraphicsPipelineStateInitializer::EPrimitiveTopology Topology) -> vk::PrimitiveTopology
	{
		switch (Topology)
		{
		case FGraphicsPipelineStateInitializer::EPrimitiveTopology::TriangleList:
			return vk::PrimitiveTopology::eTriangleList;
		case FGraphicsPipelineStateInitializer::EPrimitiveTopology::LineList:
			return vk::PrimitiveTopology::eLineList;
		default:
			return vk::PrimitiveTopology::eTriangleList;
		}
	}

	static auto ToVulkan_PolygonMode(FGraphicsPipelineStateInitializer::EPolygonMode Mode) -> vk::PolygonMode
	{
		return Mode == FGraphicsPipelineStateInitializer::EPolygonMode::Line ? vk::PolygonMode::eLine : vk::PolygonMode::eFill;
	}

	static auto ToVulkan_SampleCount(uint8 NumSamples) -> vk::SampleCountFlagBits
	{
		switch (NumSamples)
		{
		case 1: return vk::SampleCountFlagBits::e1;
		case 2: return vk::SampleCountFlagBits::e2;
		case 4: return vk::SampleCountFlagBits::e4;
		case 8: return vk::SampleCountFlagBits::e8;
		case 16: return vk::SampleCountFlagBits::e16;
		default: checkf(false, "Unsupported graphics pipeline sample count: {}", NumSamples); return vk::SampleCountFlagBits::e1;
		}
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
		, RenderTargetLayout(Initializer.RenderTargetLayout)
	{
		CheckVulkanRHIThread();
		checkf(Initializer.RenderTargetLayout.IsValid(), "Graphics pipeline render target layout is invalid.");
		FVulkanRenderPassManager& RenderPassManager = Device.GetRenderPassManager();
		RenderPass = RenderPassManager.GetOrCreateRenderPass(Initializer.RenderTargetLayout);

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
			.setTopology(ToVulkan_PrimitiveTopology(Initializer.PrimitiveTopology))
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
			.setPolygonMode(ToVulkan_PolygonMode(Initializer.PolygonMode))
			.setCullMode(CullMode)
			.setFrontFace(vk::FrontFace::eClockwise)
			.setDepthBiasEnable(vk::False)
			.setDepthBiasConstantFactor(0.0f)
			.setDepthBiasClamp(0.0f)
			.setDepthBiasSlopeFactor(0.0f);

		vk::PipelineMultisampleStateCreateInfo MultiSamplingInfo;
		const uint8 RasterSamples = Initializer.RenderTargetLayout.NumColorRenderTargets > 0
			? Initializer.RenderTargetLayout.ColorAttachments[0].RenderTarget.NumSamples
			: Initializer.RenderTargetLayout.DepthStencilAttachment.NumSamples;
		MultiSamplingInfo
			.setSampleShadingEnable(vk::False)
			.setRasterizationSamples(ToVulkan_SampleCount(RasterSamples))
			.setMinSampleShading(1.0f)
			.setPSampleMask(nullptr)
			.setAlphaToCoverageEnable(vk::False)
			.setAlphaToOneEnable(vk::False);

		vk::PipelineDepthStencilStateCreateInfo DepthStencilInfo;
		DepthStencilInfo.setDepthTestEnable(Initializer.bEnableDepthTest)
			.setDepthWriteEnable(Initializer.bEnableDepthWrite)
			.setDepthCompareOp(vk::CompareOp::eLess)
			.setDepthBoundsTestEnable(false)
			.setStencilTestEnable(false);

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

		std::vector<vk::PipelineColorBlendAttachmentState> ColorBlendAttachments(Initializer.RenderTargetLayout.NumColorRenderTargets, ColorBlendAttachment);
		vk::PipelineColorBlendStateCreateInfo ColorBlending;
		ColorBlending
			.setLogicOpEnable(vk::False)
			.setLogicOp(vk::LogicOp::eCopy)
			.setAttachments(ColorBlendAttachments)
			.setBlendConstants({0.0f, 0.0f, 0.0f, 0.0f});

		// Create pipeline layout
		FVulkanDescriptorSetsLayoutInfo DescriptorSetsLayoutInfo(Initializer.PipelineLayout.BindingLayouts);
		Layout = Device.GetPipelineManager().FindOrAddLayout(DescriptorSetsLayoutInfo);
		std::vector<vk::PushConstantRange> PushConstantRanges = CreatePushConstantRanges(Initializer.PipelineLayout);

		vk::PipelineLayoutCreateInfo pipelineLayoutInfo;
		pipelineLayoutInfo.setSetLayouts(Layout->GetDescriptorSetsLayout().GetLayoutHandles());
		pipelineLayoutInfo.setPushConstantRanges(PushConstantRanges);

		PipelineLayout = Device.GetHandle().createPipelineLayout(pipelineLayoutInfo);

		vk::GraphicsPipelineCreateInfo pipelineInfo;
		pipelineInfo
			.setStages(ShaderStages)
			.setPDynamicState(&DynamicStateInfo)
			.setPVertexInputState(&VertexInputInfo)
			.setPInputAssemblyState(&InputAssemblyInfo)
			.setPViewportState(&ViewportStateInfo)
			.setPRasterizationState(&RasterizerInfo)
			.setPMultisampleState(&MultiSamplingInfo)
			.setPDepthStencilState(&DepthStencilInfo)
			.setPColorBlendState(&ColorBlending)
			.setLayout(PipelineLayout)
			.setRenderPass(RenderPass->GetHandle())
			.setSubpass(0)
			.setBasePipelineHandle(nullptr)
			.setBasePipelineIndex(-1);

		try
		{
#if DURIN_VULKAN_TEST_FAILURE_INJECTION
			ThrowIfVulkanNativeCreateFailureIsArmed(
				EVulkanCreateFailurePoint::GraphicsPipeline);
#endif
			const vk::ResultValue<vk::Pipeline> PipelineCreationResult =
				Device.GetHandle().createGraphicsPipeline(nullptr, pipelineInfo);
			if (PipelineCreationResult.result != vk::Result::eSuccess)
			{
				throw std::runtime_error(std::format(
					"result={}, shaderStages={}, descriptorSetLayouts={}, pushConstantRanges={}",
					vk::to_string(PipelineCreationResult.result), ShaderStages.size(),
					Layout->GetDescriptorSetsLayout().GetLayoutHandles().size(),
					PushConstantRanges.size()));
			}
			Pipeline = PipelineCreationResult.value;
		}
		catch (const std::exception& Exception)
		{
			Device.GetHandle().destroyPipelineLayout(PipelineLayout);
			PipelineLayout = nullptr;
			throw std::runtime_error(std::format(
				"Vulkan graphics-pipeline creation failed: {}", Exception.what()));
		}
		catch (...)
		{
			Device.GetHandle().destroyPipelineLayout(PipelineLayout);
			PipelineLayout = nullptr;
			throw;
		}

		Shaders[EShaderStage::Vertex] = static_cast<FVulkanShader*>(
			Initializer.BoundShaders.VertexShader);
		Shaders[EShaderStage::Fragment] = static_cast<FVulkanShader*>(
			Initializer.BoundShaders.FragmentShader);
		KeepShadersAlive();
	}

	FVulkanGraphicsPipelineState::~FVulkanGraphicsPipelineState()
	{
		CheckVulkanRHIThread();
		ReleaseShaders();
		if (Pipeline)
		{
			Device.GetDeferredDeletionQueue().EnqueueResource(
				FDeferredDeletionQueue::EType::Pipeline, Pipeline);
		}
		if (PipelineLayout)
		{
			Device.GetDeferredDeletionQueue().EnqueueResource(
				FDeferredDeletionQueue::EType::PipelineLayout, PipelineLayout);
		}
	}

	auto FVulkanGraphicsPipelineState::Bind(vk::CommandBuffer CmdBuffer) -> void
	{
		CmdBuffer.bindPipeline(vk::PipelineBindPoint::eGraphics, Pipeline);
	}

	auto FVulkanGraphicsPipelineState::GetDescriptorSetsLayout() const -> const FVulkanDescriptorSetsLayout&
	{
		return Layout->GetDescriptorSetsLayout();
	}

	auto FVulkanGraphicsPipelineState::PushConstants(FVulkanCommandListContext& InContext, EShaderStageFlags StageFlags, uint32 Offset, uint32 Size, const void* pValues) const -> void
	{
		FVulkanCommandBuffer* CmdBuffer = InContext.GetCommandBuffer();
		CmdBuffer->GetHandle().pushConstants(PipelineLayout, ToVulkan_ShaderStageFlags(StageFlags), Offset, Size, pValues);
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
		CheckVulkanRHIThread();
		const auto It = PSOCache.find(Name);
		if (It != PSOCache.end())
		{
			return It->second;
		}
		return nullptr;
	}

	auto FVulkanPipelineStateCacheManager::CreateGraphicsPipelineState(FName Name, const FGraphicsPipelineStateInitializer& Initializer) -> TRefCountPtr<FVulkanGraphicsPipelineState>
	{
		CheckVulkanRHIThread();
		const auto It = PSOCache.find(Name);
		if (It != PSOCache.end())
		{
			checkf(It->second->GetRenderTargetLayout() == Initializer.RenderTargetLayout,
				"Graphics pipeline '{}' was requested with a different render target layout.", Name.ToString());
			return It->second;
		}

		auto NewPSO = MakeRefCount<FVulkanGraphicsPipelineState>(Device, Initializer);
		PSOCache.emplace(Name, NewPSO);
		return NewPSO;
	}

	auto FVulkanPipelineStateCacheManager::FindOrAddLayout(const FVulkanDescriptorSetsLayoutInfo& LayoutInfo) -> FVulkanLayout*
	{
		const auto It = LayoutMap.find(LayoutInfo);
		if (It != LayoutMap.end())
		{
			return It->second;
		}

		auto NewLayout = std::make_unique<FVulkanLayout>(Device);
		NewLayout->DSetsLayout = FVulkanDescriptorSetsLayout(Device, LayoutInfo);
		const auto [InsertedIt, bInserted] =
			LayoutMap.emplace(LayoutInfo, nullptr);
		check(bInserted);
		InsertedIt->second = NewLayout.release();
		return InsertedIt->second;
	}

	auto FVulkanDynamicRHI::RHICreateGraphicsPipelineState(FName Name, const FGraphicsPipelineStateInitializer& Initializer) -> TRefCountPtr<FRHIGraphicsPipelineState>
	{
		TRefCountPtr<FRHIGraphicsPipelineState> Result;
		const FRHIFallibleOperationResult CreationResult =
			ExecuteFallibleVulkanCreationOperation(
				[this, Name, Initializer, &Result]() {
					Result = Device->GetPipelineManager()
						.CreateGraphicsPipelineState(Name, Initializer);
				},
				GetPipelineInitializerPayloadBytes(Initializer));
		if (!CreationResult.IsSuccess())
		{
			DURIN_ERROR("Failed to create Vulkan RHI graphics pipeline '{}': {}",
				Name.ToString(), CreationResult.Diagnostic);
			return nullptr;
		}
		return Result;
	}

	auto FVulkanDynamicRHI::RHIGetGraphicsPipelineState(FName Name) -> TRefCountPtr<FRHIGraphicsPipelineState>
	{
		TRefCountPtr<FRHIGraphicsPipelineState> Result;
		if (GRHIThread && !IsInRHIThread())
		{
			GCommandListExecutor.ExecuteSynchronousOperation(false,
				[this, Name, &Result]() {
					Result = Device->GetPipelineManager()
						.GetGraphicsPipelineState(Name);
				});
			return Result;
		}
		CheckVulkanRHIThread();
		return Device->GetPipelineManager().GetGraphicsPipelineState(Name);
	}
} // namespace Durin::VulkanRHI
