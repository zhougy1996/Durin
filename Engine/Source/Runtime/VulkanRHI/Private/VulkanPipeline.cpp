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
#if DURIN_VULKAN_TEST_FAILURE_INJECTION
	namespace
	{
		std::atomic<uint64> GCommittedGraphicsPipelineCount = 0;
		std::atomic<uint64> GDestroyedGraphicsPipelineCount = 0;
		std::atomic<uint64> GCreatedGraphicsPipelineLayoutCount = 0;
		std::atomic<uint64> GRolledBackGraphicsPipelineLayoutCount = 0;
	}

	auto GetVulkanGraphicsPipelineTestStats()
		-> FVulkanGraphicsPipelineTestStats
	{
		return {
			.CommittedPipelineCount =
				GCommittedGraphicsPipelineCount.load(std::memory_order_acquire),
			.DestroyedPipelineCount =
				GDestroyedGraphicsPipelineCount.load(std::memory_order_acquire),
			.CreatedPipelineLayoutCount =
				GCreatedGraphicsPipelineLayoutCount.load(std::memory_order_acquire),
			.RolledBackPipelineLayoutCount =
				GRolledBackGraphicsPipelineLayoutCount.load(std::memory_order_acquire),
		};
	}
#endif

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

	static auto ToVulkan_PolygonMode(ERHIPolygonMode Mode) -> vk::PolygonMode
	{
		return Mode == ERHIPolygonMode::Line ? vk::PolygonMode::eLine : vk::PolygonMode::eFill;
	}

	static auto ToVulkan_CullMode(ERHICullMode Mode) -> vk::CullModeFlags
	{
		return Mode == ERHICullMode::Back
			? vk::CullModeFlagBits::eBack : vk::CullModeFlagBits::eNone;
	}

	static auto ToVulkan_FrontFace(ERHIFrontFace FrontFace) -> vk::FrontFace
	{
		return FrontFace == ERHIFrontFace::CounterClockwise
			? vk::FrontFace::eCounterClockwise : vk::FrontFace::eClockwise;
	}

	static auto ToVulkan_DepthCompareOp(ERHIDepthCompareOp CompareOp) -> vk::CompareOp
	{
		return CompareOp == ERHIDepthCompareOp::Less
			? vk::CompareOp::eLess : vk::CompareOp::eNever;
	}

	static auto ToVulkan_BlendFactor(ERHIBlendFactor Factor) -> vk::BlendFactor
	{
		switch (Factor)
		{
		case ERHIBlendFactor::Zero: return vk::BlendFactor::eZero;
		case ERHIBlendFactor::One: return vk::BlendFactor::eOne;
		case ERHIBlendFactor::SrcAlpha: return vk::BlendFactor::eSrcAlpha;
		case ERHIBlendFactor::OneMinusSrcAlpha:
			return vk::BlendFactor::eOneMinusSrcAlpha;
		default: return vk::BlendFactor::eZero;
		}
	}

	static auto ToVulkan_BlendOp(ERHIBlendOp BlendOp) -> vk::BlendOp
	{
		return BlendOp == ERHIBlendOp::Add ? vk::BlendOp::eAdd : vk::BlendOp::eAdd;
	}

	static auto ToVulkan_ColorWriteMask(ERHIColorWriteMask Mask)
		-> vk::ColorComponentFlags
	{
		vk::ColorComponentFlags Result;
		if (EnumHasAnyFlags(Mask, ERHIColorWriteMask::Red))
			Result |= vk::ColorComponentFlagBits::eR;
		if (EnumHasAnyFlags(Mask, ERHIColorWriteMask::Green))
			Result |= vk::ColorComponentFlagBits::eG;
		if (EnumHasAnyFlags(Mask, ERHIColorWriteMask::Blue))
			Result |= vk::ColorComponentFlagBits::eB;
		if (EnumHasAnyFlags(Mask, ERHIColorWriteMask::Alpha))
			Result |= vk::ColorComponentFlagBits::eA;
		return Result;
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
	{
		CheckVulkanRHIThread();
		checkf(Initializer.RenderTargetLayout.IsValid(), "Graphics pipeline render target layout is invalid.");
		checkf(Initializer.BoundShaders.VertexShader != nullptr,
			"Graphics pipeline requires a vertex shader.");
		checkf(Initializer.BoundShaders.FragmentShader != nullptr,
			"Graphics pipeline requires a fragment shader.");
		checkf(Initializer.VertexDeclaration != nullptr,
			"Graphics pipeline requires a vertex declaration.");
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
		RasterizerInfo
			.setDepthClampEnable(vk::False)
			.setLineWidth(1.0f)
			.setRasterizerDiscardEnable(vk::False)
			.setPolygonMode(ToVulkan_PolygonMode(
				Initializer.RasterizerState.PolygonMode))
			.setCullMode(ToVulkan_CullMode(Initializer.RasterizerState.CullMode))
			.setFrontFace(ToVulkan_FrontFace(
				Initializer.RasterizerState.FrontFace))
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
		DepthStencilInfo.setDepthTestEnable(Initializer.DepthState.bEnableTest)
			.setDepthWriteEnable(Initializer.DepthState.bEnableWrite)
			.setDepthCompareOp(ToVulkan_DepthCompareOp(
				Initializer.DepthState.CompareOp))
			.setDepthBoundsTestEnable(false)
			.setStencilTestEnable(false);

		vk::PipelineColorBlendAttachmentState ColorBlendAttachment;
		const FRHIColorBlendState& BlendState = Initializer.ColorBlendState;
		ColorBlendAttachment
			.setColorWriteMask(ToVulkan_ColorWriteMask(BlendState.ColorWriteMask))
			.setBlendEnable(BlendState.bEnable ? vk::True : vk::False)
			.setSrcColorBlendFactor(ToVulkan_BlendFactor(BlendState.SrcColorFactor))
			.setDstColorBlendFactor(ToVulkan_BlendFactor(BlendState.DstColorFactor))
			.setColorBlendOp(ToVulkan_BlendOp(BlendState.ColorOp))
			.setSrcAlphaBlendFactor(ToVulkan_BlendFactor(BlendState.SrcAlphaFactor))
			.setDstAlphaBlendFactor(ToVulkan_BlendFactor(BlendState.DstAlphaFactor))
			.setAlphaBlendOp(ToVulkan_BlendOp(BlendState.AlphaOp));

		std::vector<vk::PipelineColorBlendAttachmentState> ColorBlendAttachments(Initializer.RenderTargetLayout.NumColorRenderTargets, ColorBlendAttachment);
		vk::PipelineColorBlendStateCreateInfo ColorBlending;
		ColorBlending
			.setLogicOpEnable(vk::False)
			.setLogicOp(vk::LogicOp::eCopy)
			.setAttachments(ColorBlendAttachments)
			.setBlendConstants({0.0f, 0.0f, 0.0f, 0.0f});

		// Create pipeline layout
		FVulkanDescriptorSetsLayoutInfo DescriptorSetsLayoutInfo(Initializer.PipelineLayout.BindingLayouts);
		FVulkanLayout* CandidateLayout =
			Device.GetPipelineManager().FindOrAddLayout(DescriptorSetsLayoutInfo);
		std::vector<vk::PushConstantRange> PushConstantRanges = CreatePushConstantRanges(Initializer.PipelineLayout);

		vk::PipelineLayoutCreateInfo pipelineLayoutInfo;
		pipelineLayoutInfo.setSetLayouts(CandidateLayout->GetDescriptorSetsLayout().GetLayoutHandles());
		pipelineLayoutInfo.setPushConstantRanges(PushConstantRanges);

		vk::PipelineLayout CandidatePipelineLayout{};
		vk::Pipeline CandidatePipeline{};
		try
		{
#if DURIN_VULKAN_TEST_FAILURE_INJECTION
			ThrowIfVulkanNativeCreateFailureIsArmed(
				EVulkanCreateFailurePoint::PipelineLayout);
#endif
			CandidatePipelineLayout =
				Device.GetHandle().createPipelineLayout(pipelineLayoutInfo);
#if DURIN_VULKAN_TEST_FAILURE_INJECTION
			GCreatedGraphicsPipelineLayoutCount.fetch_add(
				1, std::memory_order_release);
#endif

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
				.setLayout(CandidatePipelineLayout)
				.setRenderPass(RenderPass->GetHandle())
				.setSubpass(0)
				.setBasePipelineHandle(nullptr)
				.setBasePipelineIndex(-1);

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
					CandidateLayout->GetDescriptorSetsLayout().GetLayoutHandles().size(),
					PushConstantRanges.size()));
			}
			CandidatePipeline = PipelineCreationResult.value;
		}
		catch (const std::exception& Exception)
		{
			if (CandidatePipeline)
			{
				Device.GetHandle().destroyPipeline(CandidatePipeline);
			}
			if (CandidatePipelineLayout)
			{
				Device.GetHandle().destroyPipelineLayout(CandidatePipelineLayout);
#if DURIN_VULKAN_TEST_FAILURE_INJECTION
				GRolledBackGraphicsPipelineLayoutCount.fetch_add(
					1, std::memory_order_release);
#endif
			}
			throw std::runtime_error(std::format(
				"Vulkan graphics-pipeline creation failed: {}", Exception.what()));
		}
		catch (...)
		{
			if (CandidatePipeline)
			{
				Device.GetHandle().destroyPipeline(CandidatePipeline);
			}
			if (CandidatePipelineLayout)
			{
				Device.GetHandle().destroyPipelineLayout(CandidatePipelineLayout);
#if DURIN_VULKAN_TEST_FAILURE_INJECTION
				GRolledBackGraphicsPipelineLayoutCount.fetch_add(
					1, std::memory_order_release);
#endif
			}
			throw;
		}

		Layout = CandidateLayout;
		PipelineLayout = CandidatePipelineLayout;
		Pipeline = CandidatePipeline;
#if DURIN_VULKAN_TEST_FAILURE_INJECTION
		GCommittedGraphicsPipelineCount.fetch_add(1, std::memory_order_release);
#endif
	}

	FVulkanGraphicsPipelineState::~FVulkanGraphicsPipelineState()
	{
		CheckVulkanRHIThread();
		check(Pipeline);
		check(PipelineLayout);
#if DURIN_VULKAN_TEST_FAILURE_INJECTION
		GDestroyedGraphicsPipelineCount.fetch_add(1, std::memory_order_release);
#endif
		Device.NotifyDeleted_GraphicsPipeline(this);
		Device.GetDeferredDeletionQueue().EnqueueResource(
			FDeferredDeletionQueue::EType::Pipeline, Pipeline);
		Device.GetDeferredDeletionQueue().EnqueueResource(
			FDeferredDeletionQueue::EType::PipelineLayout, PipelineLayout);
	}

	auto FVulkanGraphicsPipelineState::Bind(vk::CommandBuffer CmdBuffer) -> void
	{
		check(Pipeline);
		CmdBuffer.bindPipeline(vk::PipelineBindPoint::eGraphics, Pipeline);
	}

	auto FVulkanGraphicsPipelineState::GetDescriptorSetsLayout() const -> const FVulkanDescriptorSetsLayout&
	{
		check(Layout);
		return Layout->GetDescriptorSetsLayout();
	}

	auto FVulkanGraphicsPipelineState::PushConstants(FVulkanCommandListContext& InContext, EShaderStageFlags StageFlags, uint32 Offset, uint32 Size, const void* pValues) const -> void
	{
		check(PipelineLayout);
		FVulkanCommandBuffer* CmdBuffer = InContext.GetCommandBuffer();
		CmdBuffer->GetHandle().pushConstants(PipelineLayout, ToVulkan_ShaderStageFlags(StageFlags), Offset, Size, pValues);
	}

	FVulkanPipelineManager::FVulkanPipelineManager(FVulkanDevice& InDevice)
		: Device(InDevice)
	{
	}

	FVulkanPipelineManager::~FVulkanPipelineManager()
	{
		for (const auto& Layout : LayoutMap | std::views::values)
		{
			// LayoutMap owns the FVulkanLayout objects; clean them up.
			// The layouts' descriptor set handles will be released by the
			// DescriptorSetLayoutCache (owned by FVulkanDevice) which outlives us.
			delete Layout;
		}
		LayoutMap.clear();
	}

	auto FVulkanPipelineManager::CreateGraphicsPipelineState(const FGraphicsPipelineStateInitializer& Initializer) -> TRefCountPtr<FVulkanGraphicsPipelineState>
	{
		CheckVulkanRHIThread();
		return MakeRefCount<FVulkanGraphicsPipelineState>(Device, Initializer);
	}

	auto FVulkanPipelineManager::FindOrAddLayout(const FVulkanDescriptorSetsLayoutInfo& LayoutInfo) -> FVulkanLayout*
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

	auto FVulkanDynamicRHI::RHICreateGraphicsPipelineState(FName DebugName, const FGraphicsPipelineStateInitializer& Initializer) -> TRefCountPtr<FRHIGraphicsPipelineState>
	{
		if (!Initializer.IsValid())
		{
			DURIN_ERROR("Failed to create Vulkan RHI graphics pipeline '{}': initializer is invalid or unsupported.",
				DebugName.ToString());
			return nullptr;
		}
		TRefCountPtr<FRHIGraphicsPipelineState> Result;
		const FRHIFallibleOperationResult CreationResult =
			ExecuteFallibleVulkanCreationOperation(
				[this, Initializer, &Result]() {
					Result = Device->GetPipelineManager()
						.CreateGraphicsPipelineState(Initializer);
				},
				GetPipelineInitializerPayloadBytes(Initializer));
		if (!CreationResult.IsSuccess())
		{
			DURIN_ERROR("Failed to create Vulkan RHI graphics pipeline '{}': {}",
				DebugName.ToString(), CreationResult.Diagnostic);
			return nullptr;
		}
		return Result;
	}
} // namespace Durin::VulkanRHI
