#include "VulkanPipeline.h"

#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

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

	static auto GetPipelineInitializerPayloadBytes(
		const FComputePipelineStateInitializer& Initializer) -> size_t
	{
		size_t Bytes = Initializer.PipelineLayout.BindingLayouts.size()
			* sizeof(FBindingLayout);
		Bytes += Initializer.PipelineLayout.PushConstantRanges.size()
			* sizeof(FPushConstantRange);
		for (const FBindingLayout& Layout : Initializer.PipelineLayout.BindingLayouts)
			Bytes += Layout.BindingLayouts.size() * sizeof(FBindingLayoutItem);
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
		switch (Mode)
		{
		case ERHICullMode::None: return vk::CullModeFlagBits::eNone;
		case ERHICullMode::Front: return vk::CullModeFlagBits::eFront;
		case ERHICullMode::Back: return vk::CullModeFlagBits::eBack;
		default: return vk::CullModeFlagBits::eNone;
		}
	}

	static auto ToVulkan_FrontFace(ERHIFrontFace FrontFace) -> vk::FrontFace
	{
		return FrontFace == ERHIFrontFace::CounterClockwise
			? vk::FrontFace::eCounterClockwise : vk::FrontFace::eClockwise;
	}

	static auto ToVulkan_DepthCompareOp(ERHIDepthCompareOp CompareOp) -> vk::CompareOp
	{
		switch (CompareOp)
		{
		case ERHIDepthCompareOp::Never: return vk::CompareOp::eNever;
		case ERHIDepthCompareOp::Less: return vk::CompareOp::eLess;
		case ERHIDepthCompareOp::Equal: return vk::CompareOp::eEqual;
		case ERHIDepthCompareOp::LessOrEqual: return vk::CompareOp::eLessOrEqual;
		case ERHIDepthCompareOp::Greater: return vk::CompareOp::eGreater;
		case ERHIDepthCompareOp::NotEqual: return vk::CompareOp::eNotEqual;
		case ERHIDepthCompareOp::GreaterOrEqual: return vk::CompareOp::eGreaterOrEqual;
		case ERHIDepthCompareOp::Always: return vk::CompareOp::eAlways;
		default: return vk::CompareOp::eNever;
		}
	}

	static auto ToVulkan_StencilOp(ERHIStencilOp Op) -> vk::StencilOp
	{
		switch (Op)
		{
		case ERHIStencilOp::Keep: return vk::StencilOp::eKeep;
		case ERHIStencilOp::Zero: return vk::StencilOp::eZero;
		case ERHIStencilOp::Replace: return vk::StencilOp::eReplace;
		case ERHIStencilOp::IncrementClamp: return vk::StencilOp::eIncrementAndClamp;
		case ERHIStencilOp::DecrementClamp: return vk::StencilOp::eDecrementAndClamp;
		case ERHIStencilOp::Invert: return vk::StencilOp::eInvert;
		case ERHIStencilOp::IncrementWrap: return vk::StencilOp::eIncrementAndWrap;
		case ERHIStencilOp::DecrementWrap: return vk::StencilOp::eDecrementAndWrap;
		default: return vk::StencilOp::eKeep;
		}
	}

	static auto ToVulkan_StencilFace(const FRHIStencilFaceState& Face,
		const FRHIDepthStencilState& State) -> vk::StencilOpState
	{
		return vk::StencilOpState{}
			.setFailOp(ToVulkan_StencilOp(Face.FailOp))
			.setPassOp(ToVulkan_StencilOp(Face.PassOp))
			.setDepthFailOp(ToVulkan_StencilOp(Face.DepthFailOp))
			.setCompareOp(ToVulkan_DepthCompareOp(Face.CompareOp))
			.setCompareMask(State.StencilCompareMask)
			.setWriteMask(State.StencilWriteMask)
			.setReference(State.StencilReference);
	}

	static auto ToVulkan_BlendFactor(ERHIBlendFactor Factor) -> vk::BlendFactor
	{
		switch (Factor)
		{
		case ERHIBlendFactor::Zero: return vk::BlendFactor::eZero;
		case ERHIBlendFactor::One: return vk::BlendFactor::eOne;
		case ERHIBlendFactor::SrcColor: return vk::BlendFactor::eSrcColor;
		case ERHIBlendFactor::OneMinusSrcColor: return vk::BlendFactor::eOneMinusSrcColor;
		case ERHIBlendFactor::DstColor: return vk::BlendFactor::eDstColor;
		case ERHIBlendFactor::OneMinusDstColor: return vk::BlendFactor::eOneMinusDstColor;
		case ERHIBlendFactor::SrcAlpha: return vk::BlendFactor::eSrcAlpha;
		case ERHIBlendFactor::OneMinusSrcAlpha:
			return vk::BlendFactor::eOneMinusSrcAlpha;
		case ERHIBlendFactor::DstAlpha: return vk::BlendFactor::eDstAlpha;
		case ERHIBlendFactor::OneMinusDstAlpha: return vk::BlendFactor::eOneMinusDstAlpha;
		case ERHIBlendFactor::ConstantColor: return vk::BlendFactor::eConstantColor;
		case ERHIBlendFactor::OneMinusConstantColor: return vk::BlendFactor::eOneMinusConstantColor;
		case ERHIBlendFactor::ConstantAlpha: return vk::BlendFactor::eConstantAlpha;
		case ERHIBlendFactor::OneMinusConstantAlpha: return vk::BlendFactor::eOneMinusConstantAlpha;
		case ERHIBlendFactor::SrcAlphaSaturate: return vk::BlendFactor::eSrcAlphaSaturate;
		default: return vk::BlendFactor::eZero;
		}
	}

	static auto ToVulkan_BlendOp(ERHIBlendOp BlendOp) -> vk::BlendOp
	{
		switch (BlendOp)
		{
		case ERHIBlendOp::Add: return vk::BlendOp::eAdd;
		case ERHIBlendOp::Subtract: return vk::BlendOp::eSubtract;
		case ERHIBlendOp::ReverseSubtract: return vk::BlendOp::eReverseSubtract;
		case ERHIBlendOp::Min: return vk::BlendOp::eMin;
		case ERHIBlendOp::Max: return vk::BlendOp::eMax;
		default: return vk::BlendOp::eAdd;
		}
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

		const vk::DescriptorSetLayout Result =
			Device.GetHandle().createDescriptorSetLayout(LayoutInfo);
		Device.GetRHI().GetDebugUtils().NameObject(Result,
			Device.GetRHI().GetDebugUtils().MakeInternalName("DescriptorSetLayout"));
		return Result;
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

	FVulkanGraphicsPipelineState::FVulkanGraphicsPipelineState(FVulkanDevice& InDevice,
		const FGraphicsPipelineStateInitializer& Initializer,
		FGraphicsPipelineStateKey InKey, std::string_view DebugName)
		: Device(InDevice), Key(std::move(InKey))
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
					.setInputRate(Element.InputRate ==
						FRHIVertexElementIdentity::EInputRate::Instance
							? vk::VertexInputRate::eInstance
							: vk::VertexInputRate::eVertex);
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
			.setDepthClampEnable(Initializer.RasterizerState.bEnableDepthClamp)
			.setLineWidth(Initializer.RasterizerState.LineWidth)
			.setRasterizerDiscardEnable(vk::False)
			.setPolygonMode(ToVulkan_PolygonMode(
				Initializer.RasterizerState.PolygonMode))
			.setCullMode(ToVulkan_CullMode(Initializer.RasterizerState.CullMode))
			.setFrontFace(ToVulkan_FrontFace(
				Initializer.RasterizerState.FrontFace))
			.setDepthBiasEnable(Initializer.RasterizerState.bEnableDepthBias)
			.setDepthBiasConstantFactor(Initializer.RasterizerState.DepthBiasConstantFactor)
			.setDepthBiasClamp(Initializer.RasterizerState.DepthBiasClamp)
			.setDepthBiasSlopeFactor(Initializer.RasterizerState.DepthBiasSlopeFactor);

		vk::PipelineMultisampleStateCreateInfo MultiSamplingInfo;
		MultiSamplingInfo
			.setSampleShadingEnable(vk::False)
			.setRasterizationSamples(ToVulkan_SampleCount(Initializer.MultisampleState.RasterSamples))
			.setMinSampleShading(1.0f)
			.setPSampleMask(nullptr)
			.setAlphaToCoverageEnable(Initializer.MultisampleState.bEnableAlphaToCoverage)
			.setAlphaToOneEnable(vk::False);

		vk::PipelineDepthStencilStateCreateInfo DepthStencilInfo;
		const FRHIDepthStencilState& DepthStencilState = Initializer.DepthStencilState;
		DepthStencilInfo.setDepthTestEnable(DepthStencilState.bEnableTest)
			.setDepthWriteEnable(DepthStencilState.bEnableWrite)
			.setDepthCompareOp(ToVulkan_DepthCompareOp(
				DepthStencilState.CompareOp))
			.setDepthBoundsTestEnable(false)
			.setStencilTestEnable(DepthStencilState.bEnableStencil)
			.setFront(ToVulkan_StencilFace(DepthStencilState.FrontFace, DepthStencilState))
			.setBack(ToVulkan_StencilFace(DepthStencilState.BackFace, DepthStencilState));

		std::vector<vk::PipelineColorBlendAttachmentState> ColorBlendAttachments;
		ColorBlendAttachments.reserve(Initializer.RenderTargetLayout.NumColorRenderTargets);
		for (uint32 Index = 0; Index < Initializer.RenderTargetLayout.NumColorRenderTargets; ++Index)
		{
			const FRHIColorBlendState& BlendState = Initializer.ColorBlendStates[Index];
			ColorBlendAttachments.emplace_back()
				.setColorWriteMask(ToVulkan_ColorWriteMask(BlendState.ColorWriteMask))
				.setBlendEnable(BlendState.bEnable ? vk::True : vk::False)
				.setSrcColorBlendFactor(ToVulkan_BlendFactor(BlendState.SrcColorFactor))
				.setDstColorBlendFactor(ToVulkan_BlendFactor(BlendState.DstColorFactor))
				.setColorBlendOp(ToVulkan_BlendOp(BlendState.ColorOp))
				.setSrcAlphaBlendFactor(ToVulkan_BlendFactor(BlendState.SrcAlphaFactor))
				.setDstAlphaBlendFactor(ToVulkan_BlendFactor(BlendState.DstAlphaFactor))
				.setAlphaBlendOp(ToVulkan_BlendOp(BlendState.AlphaOp));
		}
		vk::PipelineColorBlendStateCreateInfo ColorBlending;
		ColorBlending
			.setLogicOpEnable(vk::False)
			.setLogicOp(vk::LogicOp::eCopy)
			.setAttachments(ColorBlendAttachments)
			.setBlendConstants({0.0f, 0.0f, 0.0f, 0.0f});

		// Create pipeline layout
		FVulkanDescriptorSetsLayoutInfo DescriptorSetsLayoutInfo(Initializer.PipelineLayout.BindingLayouts);
		std::shared_ptr<FVulkanLayout> CandidateLayout =
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
				Device.GetHandle().createGraphicsPipeline(
					Device.GetPipelineManager().GetDriverPipelineCache(), pipelineInfo);
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

		Layout = std::move(CandidateLayout);
		PipelineLayout = CandidatePipelineLayout;
		Pipeline = CandidatePipeline;
		const std::string BaseName = DebugName.empty()
			? Device.GetRHI().GetDebugUtils().MakeInternalName("GraphicsPipeline")
			: std::string(DebugName);
		Device.GetRHI().GetDebugUtils().NameObject(Pipeline, BaseName);
		Device.GetRHI().GetDebugUtils().NameObject(PipelineLayout,
			std::format("{}.PipelineLayout", BaseName));
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

	FVulkanComputePipelineState::FVulkanComputePipelineState(
		FVulkanDevice& InDevice,
		const FComputePipelineStateInitializer& Initializer,
		FComputePipelineStateKey InKey, std::string_view DebugName)
		: Device(InDevice), Key(std::move(InKey))
	{
		CheckVulkanRHIThread();
		const auto* Shader = static_cast<const FVulkanShader*>(Initializer.ComputeShader);
		FVulkanDescriptorSetsLayoutInfo LayoutInfo(
			Initializer.PipelineLayout.BindingLayouts);
		std::shared_ptr<FVulkanLayout> CandidateLayout =
			Device.GetPipelineManager().FindOrAddLayout(LayoutInfo);
		std::vector<vk::PushConstantRange> PushConstantRanges =
			CreatePushConstantRanges(Initializer.PipelineLayout);
		for (const vk::PushConstantRange& Range : PushConstantRanges)
			checkf(static_cast<uint64>(Range.offset) + Range.size
				<= Device.GetGpuProperties().limits.maxPushConstantsSize,
				"Compute push-constant range exceeds the device limit.");

		vk::PipelineLayoutCreateInfo PipelineLayoutInfo;
		PipelineLayoutInfo
			.setSetLayouts(CandidateLayout->GetDescriptorSetsLayout().GetLayoutHandles())
			.setPushConstantRanges(PushConstantRanges);
		vk::PipelineLayout CandidatePipelineLayout{};
		vk::Pipeline CandidatePipeline{};
		try
		{
#if DURIN_VULKAN_TEST_FAILURE_INJECTION
			ThrowIfVulkanNativeCreateFailureIsArmed(
				EVulkanCreateFailurePoint::PipelineLayout);
#endif
			CandidatePipelineLayout =
				Device.GetHandle().createPipelineLayout(PipelineLayoutInfo);
			vk::PipelineShaderStageCreateInfo StageInfo;
			StageInfo.setStage(vk::ShaderStageFlagBits::eCompute)
				.setModule(Shader->GetShaderModule())
				.setPName(Shader->GetEntryPoint());
			vk::ComputePipelineCreateInfo PipelineInfo;
			PipelineInfo.setStage(StageInfo).setLayout(CandidatePipelineLayout);
			const vk::ResultValue<vk::Pipeline> Creation =
				Device.GetHandle().createComputePipeline(
					Device.GetPipelineManager().GetDriverPipelineCache(), PipelineInfo);
			if (Creation.result != vk::Result::eSuccess)
				throw std::runtime_error(std::format("result={}",
					vk::to_string(Creation.result)));
			CandidatePipeline = Creation.value;
		}
		catch (...)
		{
			if (CandidatePipeline) Device.GetHandle().destroyPipeline(CandidatePipeline);
			if (CandidatePipelineLayout)
				Device.GetHandle().destroyPipelineLayout(CandidatePipelineLayout);
			throw;
		}
		Layout = std::move(CandidateLayout);
		PipelineLayout = CandidatePipelineLayout;
		Pipeline = CandidatePipeline;
		const std::string BaseName = DebugName.empty()
			? Device.GetRHI().GetDebugUtils().MakeInternalName("ComputePipeline")
			: std::string(DebugName);
		Device.GetRHI().GetDebugUtils().NameObject(Pipeline, BaseName);
		Device.GetRHI().GetDebugUtils().NameObject(PipelineLayout,
			std::format("{}.PipelineLayout", BaseName));
	}

	FVulkanComputePipelineState::~FVulkanComputePipelineState()
	{
		CheckVulkanRHIThread();
		Device.NotifyDeleted_ComputePipeline(this);
		Device.GetDeferredDeletionQueue().EnqueueResource(
			FDeferredDeletionQueue::EType::Pipeline, Pipeline);
		Device.GetDeferredDeletionQueue().EnqueueResource(
			FDeferredDeletionQueue::EType::PipelineLayout, PipelineLayout);
	}

	auto FVulkanComputePipelineState::Bind(vk::CommandBuffer CmdBuffer) -> void
	{
		CmdBuffer.bindPipeline(vk::PipelineBindPoint::eCompute, Pipeline);
	}

	auto FVulkanComputePipelineState::GetDescriptorSetsLayout() const
		-> const FVulkanDescriptorSetsLayout&
	{
		return Layout->GetDescriptorSetsLayout();
	}

	auto FVulkanComputePipelineState::PushConstants(
		FVulkanCommandListContext& InContext, EShaderStageFlags StageFlags,
		uint32 Offset, uint32 Size, const void* Values) const -> void
	{
		InContext.GetCommandBuffer()->GetHandle().pushConstants(PipelineLayout,
			ToVulkan_ShaderStageFlags(StageFlags), Offset, Size, Values);
	}

	FVulkanPipelineManager::FVulkanPipelineManager(FVulkanDevice& InDevice)
		: Device(InDevice)
	{
		InitializeDriverPipelineCache();
	}

	FVulkanPipelineManager::~FVulkanPipelineManager()
	{
		SaveDriverPipelineCache();
		GraphicsPipelineMap.clear();
		ComputePipelineMap.clear();
#if DURIN_VULKAN_TEST_FAILURE_INJECTION
		GVulkanPipelineLayoutEntryCount.fetch_sub(LayoutMap.size(), std::memory_order_release);
#endif
		LayoutMap.clear();
		if (DriverPipelineCache)
		{
			Device.GetHandle().destroyPipelineCache(DriverPipelineCache);
			DriverPipelineCache = nullptr;
		}
	}

	auto FVulkanPipelineManager::CreateGraphicsPipelineState(
		const FGraphicsPipelineStateInitializer& Initializer,
		FGraphicsPipelineStateKey Key, std::string_view DebugName)
		-> TRefCountPtr<FVulkanGraphicsPipelineState>
	{
		CheckVulkanRHIThread();
		auto& Stats = Device.GetGraphicsCacheStatisticsMutable().GraphicsPipelines;
		if (const auto It = GraphicsPipelineMap.find(Key); It != GraphicsPipelineMap.end())
		{
			++Stats.Hits;
			It->second.LastUsed = ++AccessSerial;
			return It->second.Pipeline;
		}
		++Stats.Misses;
		const bool bNeedsEviction = GraphicsPipelineMap.size() >= Stats.Capacity;
		if (bNeedsEviction && std::ranges::none_of(GraphicsPipelineMap, [](const auto& Entry) {
			return Entry.second.Pipeline->GetRefCount() == 1;
		}))
		{
			++Stats.FailedCandidates;
			throw std::runtime_error("Vulkan graphics pipeline cache is full and has no cache-only entry.");
		}
		TRefCountPtr<FVulkanGraphicsPipelineState> Candidate;
		try
		{
			Candidate = MakeRefCount<FVulkanGraphicsPipelineState>(
				Device, Initializer, Key, DebugName);
		}
		catch (...)
		{
			++Stats.FailedCandidates;
			throw;
		}
		++Stats.NativeCreations;
		if (bNeedsEviction && !EvictPipelineIfNeeded())
			throw std::runtime_error("Vulkan graphics pipeline cache lost its selected eviction candidate.");
		const auto [It, Inserted] = GraphicsPipelineMap.emplace(std::move(Key),
			FPipelineCacheEntry{Candidate, ++AccessSerial});
		check(Inserted);
		Stats.Occupancy = GraphicsPipelineMap.size();
		return It->second.Pipeline;
	}

	auto FVulkanPipelineManager::CreateComputePipelineState(
		const FComputePipelineStateInitializer& Initializer,
		FComputePipelineStateKey Key, std::string_view DebugName)
		-> TRefCountPtr<FVulkanComputePipelineState>
	{
		CheckVulkanRHIThread();
		auto& Stats = Device.GetGraphicsCacheStatisticsMutable().ComputePipelines;
		if (const auto It = ComputePipelineMap.find(Key);
			It != ComputePipelineMap.end())
		{
			++Stats.Hits;
			It->second.LastUsed = ++AccessSerial;
			return It->second.Pipeline;
		}
		++Stats.Misses;
		const bool bNeedsEviction = ComputePipelineMap.size() >= Stats.Capacity;
		if (bNeedsEviction && std::ranges::none_of(ComputePipelineMap,
			[](const auto& Entry) { return Entry.second.Pipeline->GetRefCount() == 1; }))
		{
			++Stats.FailedCandidates;
			throw std::runtime_error(
				"Vulkan compute pipeline cache is full and has no cache-only entry.");
		}
		TRefCountPtr<FVulkanComputePipelineState> Candidate;
		try
		{
			Candidate = MakeRefCount<FVulkanComputePipelineState>(
				Device, Initializer, Key, DebugName);
		}
		catch (...)
		{
			++Stats.FailedCandidates;
			throw;
		}
		++Stats.NativeCreations;
		if (bNeedsEviction && !EvictComputePipelineIfNeeded())
			throw std::runtime_error(
				"Vulkan compute pipeline cache lost its selected eviction candidate.");
		const auto [It, bInserted] = ComputePipelineMap.emplace(std::move(Key),
			FComputePipelineCacheEntry{Candidate, ++AccessSerial});
		check(bInserted);
		Stats.Occupancy = ComputePipelineMap.size();
		return It->second.Pipeline;
	}

	auto FVulkanPipelineManager::FindOrAddLayout(const FVulkanDescriptorSetsLayoutInfo& LayoutInfo) -> std::shared_ptr<FVulkanLayout>
	{
		auto& Stats = Device.GetGraphicsCacheStatisticsMutable().StructuralLayouts;
		const auto It = LayoutMap.find(LayoutInfo);
		if (It != LayoutMap.end())
		{
			++Stats.Hits;
			It->second.LastUsed = ++AccessSerial;
			return It->second.Layout;
		}
		++Stats.Misses;
		const bool bNeedsEviction = LayoutMap.size() >= Stats.Capacity;
		if (bNeedsEviction && std::ranges::none_of(LayoutMap, [](const auto& Entry) {
			return Entry.second.Layout.use_count() == 1;
		}))
		{
			++Stats.FailedCandidates;
			throw std::runtime_error("Vulkan structural layout cache is full and has no cache-only entry.");
		}

		auto NewLayout = std::make_shared<FVulkanLayout>(Device);
		try
		{
			NewLayout->DSetsLayout = FVulkanDescriptorSetsLayout(Device, LayoutInfo);
		}
		catch (...)
		{
			++Stats.FailedCandidates;
			throw;
		}
		if (bNeedsEviction && !EvictLayoutIfNeeded())
			throw std::runtime_error("Vulkan structural layout cache lost its selected eviction candidate.");
		const auto [InsertedIt, bInserted] =
			LayoutMap.emplace(LayoutInfo, FLayoutCacheEntry{NewLayout, ++AccessSerial});
		check(bInserted);
		++Stats.NativeCreations;
		Stats.Occupancy = LayoutMap.size();
#if DURIN_VULKAN_TEST_FAILURE_INJECTION
		GVulkanPipelineLayoutEntryCount.fetch_add(1, std::memory_order_release);
#endif
		return InsertedIt->second.Layout;
	}

	auto FVulkanPipelineManager::EvictLayoutIfNeeded() -> bool
	{
		auto Victim = LayoutMap.end();
		for (auto It = LayoutMap.begin(); It != LayoutMap.end(); ++It)
		{
			if (It->second.Layout.use_count() != 1) continue;
			if (Victim == LayoutMap.end() || It->second.LastUsed < Victim->second.LastUsed)
				Victim = It;
		}
		if (Victim == LayoutMap.end()) return false;
#if DURIN_VULKAN_TEST_FAILURE_INJECTION
		GVulkanPipelineLayoutEntryCount.fetch_sub(1, std::memory_order_release);
#endif
		LayoutMap.erase(Victim);
		auto& Stats = Device.GetGraphicsCacheStatisticsMutable().StructuralLayouts;
		++Stats.Evictions;
		Stats.Occupancy = LayoutMap.size();
		return true;
	}

	auto FVulkanPipelineManager::EvictPipelineIfNeeded() -> bool
	{
		auto Victim = GraphicsPipelineMap.end();
		for (auto It = GraphicsPipelineMap.begin(); It != GraphicsPipelineMap.end(); ++It)
		{
			if (It->second.Pipeline->GetRefCount() != 1) continue;
			if (Victim == GraphicsPipelineMap.end() || It->second.LastUsed < Victim->second.LastUsed)
				Victim = It;
		}
		if (Victim == GraphicsPipelineMap.end()) return false;
		GraphicsPipelineMap.erase(Victim);
		auto& Stats = Device.GetGraphicsCacheStatisticsMutable().GraphicsPipelines;
		++Stats.Evictions;
		Stats.Occupancy = GraphicsPipelineMap.size();
		return true;
	}

	auto FVulkanPipelineManager::EvictComputePipelineIfNeeded() -> bool
	{
		auto Victim = ComputePipelineMap.end();
		for (auto It = ComputePipelineMap.begin(); It != ComputePipelineMap.end(); ++It)
		{
			if (It->second.Pipeline->GetRefCount() != 1) continue;
			if (Victim == ComputePipelineMap.end()
				|| It->second.LastUsed < Victim->second.LastUsed) Victim = It;
		}
		if (Victim == ComputePipelineMap.end()) return false;
		ComputePipelineMap.erase(Victim);
		auto& Stats = Device.GetGraphicsCacheStatisticsMutable().ComputePipelines;
		++Stats.Evictions;
		Stats.Occupancy = ComputePipelineMap.size();
		return true;
	}

	namespace
	{
		constexpr uint32 PipelineCacheMagic = 0x43505544; // DUPC
		constexpr uint32 PipelineCacheSchema = 1;
		constexpr size_t PipelineCachePrefixBytes = 5 * sizeof(uint32) + VK_UUID_SIZE;
		constexpr size_t PipelineCacheMaximumBytes = 16 * 1024 * 1024;

		auto PipelineCachePath() -> std::filesystem::path
		{
			return std::filesystem::path(FPaths::LaunchSavedDir()) / "Vulkan" / "PipelineCache-v1.bin";
		}

		auto ReadU32(const std::span<const uint8> Bytes, const size_t Offset) -> uint32
		{
			uint32 Value = 0;
			if (Offset + sizeof(Value) <= Bytes.size()) std::memcpy(&Value, Bytes.data() + Offset, sizeof(Value));
			return Value;
		}
	}

	auto FVulkanPipelineManager::InitializeDriverPipelineCache() -> void
	{
		std::vector<uint8> FileBytes;
		std::span<const uint8> InitialData;
		auto& Stats = Device.GetGraphicsCacheStatisticsMutable();
		const auto& Properties = Device.GetGpuProperties();
		const std::filesystem::path Path = PipelineCachePath();
		if (FFileHelper::FileExists(Path.generic_string()))
		{
			const bool bLoaded = FFileHelper::LoadFileToArray(FileBytes, Path.generic_string());
			bool bCompatible = bLoaded && FileBytes.size() >= PipelineCachePrefixBytes
				&& FileBytes.size() <= PipelineCacheMaximumBytes
				&& ReadU32(FileBytes, 0) == PipelineCacheMagic
				&& ReadU32(FileBytes, 4) == PipelineCacheSchema
				&& ReadU32(FileBytes, 8) == Properties.vendorID
				&& ReadU32(FileBytes, 12) == Properties.deviceID;
			const uint32 PayloadSize = ReadU32(FileBytes, 16);
			bCompatible = bCompatible && PayloadSize == FileBytes.size() - PipelineCachePrefixBytes
				&& std::memcmp(FileBytes.data() + 20, Properties.pipelineCacheUUID.data(), VK_UUID_SIZE) == 0;
			if (bCompatible)
			{
				InitialData = std::span<const uint8>(FileBytes).subspan(PipelineCachePrefixBytes);
				bCompatible = InitialData.size() >= 32
					&& ReadU32(InitialData, 0) >= 32
					&& ReadU32(InitialData, 4) == VK_PIPELINE_CACHE_HEADER_VERSION_ONE
					&& ReadU32(InitialData, 8) == Properties.vendorID
					&& ReadU32(InitialData, 12) == Properties.deviceID
					&& std::memcmp(InitialData.data() + 16, Properties.pipelineCacheUUID.data(), VK_UUID_SIZE) == 0;
			}
			if (!bCompatible)
			{
				++Stats.PersistentRejects;
				InitialData = {};
				DURIN_WARN("Ignoring incompatible Vulkan pipeline cache '{}'.", Path.generic_string());
			}
		}

		vk::PipelineCacheCreateInfo CreateInfo;
		CreateInfo.setInitialDataSize(InitialData.size()).setPInitialData(InitialData.data());
		try
		{
			DriverPipelineCache = Device.GetHandle().createPipelineCache(CreateInfo);
			Device.GetRHI().GetDebugUtils().NameObject(
				DriverPipelineCache, "Durin.DriverPipelineCache");
			if (!InitialData.empty())
			{
				++Stats.PersistentLoads;
				Stats.PersistentBytes = InitialData.size();
			}
		}
		catch (const std::exception& Error)
		{
			++Stats.PersistentRejects;
			DURIN_WARN("Vulkan rejected persisted pipeline cache '{}': {}. Using an empty cache.", Path.generic_string(), Error.what());
			DriverPipelineCache = Device.GetHandle().createPipelineCache({});
			Device.GetRHI().GetDebugUtils().NameObject(
				DriverPipelineCache, "Durin.DriverPipelineCache");
		}
	}

	auto FVulkanPipelineManager::SaveDriverPipelineCache() -> void
	{
		if (!DriverPipelineCache) return;
		auto& Stats = Device.GetGraphicsCacheStatisticsMutable();
		try
		{
			const std::vector<uint8_t> Payload = Device.GetHandle().getPipelineCacheData(DriverPipelineCache);
			if (Payload.empty() || Payload.size() + PipelineCachePrefixBytes > PipelineCacheMaximumBytes)
			{
				++Stats.PersistentRejects;
				DURIN_WARN("Not saving Vulkan pipeline cache because its payload is empty or exceeds {} bytes.", PipelineCacheMaximumBytes);
				return;
			}
			std::vector<std::byte> Bytes(PipelineCachePrefixBytes + Payload.size());
			const auto WriteU32 = [&Bytes](size_t Offset, uint32 Value) { std::memcpy(Bytes.data() + Offset, &Value, sizeof(Value)); };
			const auto& Properties = Device.GetGpuProperties();
			WriteU32(0, PipelineCacheMagic);
			WriteU32(4, PipelineCacheSchema);
			WriteU32(8, Properties.vendorID);
			WriteU32(12, Properties.deviceID);
			WriteU32(16, static_cast<uint32>(Payload.size()));
			std::memcpy(Bytes.data() + 20, Properties.pipelineCacheUUID.data(), VK_UUID_SIZE);
			std::memcpy(Bytes.data() + PipelineCachePrefixBytes, Payload.data(), Payload.size());
			FFileHelper::FAtomicFileError Error;
			if (!FFileHelper::SaveArrayToFileAtomically(Bytes, PipelineCachePath(), &Error))
			{
				++Stats.PersistentRejects;
				DURIN_WARN("Could not save Vulkan pipeline cache: {}", Error.ToString());
				return;
			}
			++Stats.PersistentSaves;
			Stats.PersistentBytes = Payload.size();
		}
		catch (const std::exception& Error)
		{
			++Stats.PersistentRejects;
			DURIN_WARN("Could not query Vulkan pipeline cache for persistence: {}", Error.what());
		}
	}

	auto FVulkanDynamicRHI::RHICreateGraphicsPipelineState(FName DebugName, const FGraphicsPipelineStateInitializer& Initializer) -> TRefCountPtr<FRHIGraphicsPipelineState>
	{
		FGraphicsPipelineStateKey Key;
		std::string ValidationError;
		if (!BuildGraphicsPipelineStateKey(Initializer, RHIGetCapabilities(), Key,
			ValidationError))
		{
			DURIN_ERROR("Failed to create Vulkan RHI graphics pipeline '{}': {}",
				DebugName.ToString(), ValidationError);
			return nullptr;
		}
		TRefCountPtr<FRHIGraphicsPipelineState> Result;
		const FRHIFallibleOperationResult CreationResult =
			ExecuteFallibleVulkanCreationOperation(
				[this, Initializer, Key = std::move(Key),
				 DebugName = DebugName.ToString(), &Result]() mutable {
					Result = Device->GetPipelineManager()
						.CreateGraphicsPipelineState(
							Initializer, std::move(Key), DebugName);
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

	auto FVulkanDynamicRHI::RHICreateComputePipelineState(FName DebugName,
		const FComputePipelineStateInitializer& Initializer)
		-> TRefCountPtr<FRHIComputePipelineState>
	{
		FComputePipelineStateKey Key;
		std::string ValidationError;
		if (!BuildComputePipelineStateKey(Initializer, RHIGetCapabilities(), Key,
			ValidationError))
		{
			DURIN_ERROR("Failed to create Vulkan RHI compute pipeline '{}': {}",
				DebugName.ToString(), ValidationError);
			return nullptr;
		}
		TRefCountPtr<FRHIComputePipelineState> Result;
		const FRHIFallibleOperationResult CreationResult =
			ExecuteFallibleVulkanCreationOperation(
				[this, Initializer, Key = std::move(Key),
				 DebugName = DebugName.ToString(), &Result]() mutable {
					Result = Device->GetPipelineManager().CreateComputePipelineState(
						Initializer, std::move(Key), DebugName);
				}, GetPipelineInitializerPayloadBytes(Initializer));
		if (!CreationResult.IsSuccess())
		{
			DURIN_ERROR("Failed to create Vulkan RHI compute pipeline '{}': {}",
				DebugName.ToString(), CreationResult.Diagnostic);
			return nullptr;
		}
		return Result;
	}

	auto FVulkanDynamicRHI::RHIGetGraphicsCacheStatistics() const -> FRHIGraphicsCacheStatistics
	{
		return Device ? Device->GetGraphicsCacheStatistics() : FRHIGraphicsCacheStatistics{};
	}

	auto FVulkanDynamicRHI::RHIResetGraphicsCacheStatistics() -> void
	{
		if (Device) Device->ResetGraphicsCacheStatistics();
	}
} // namespace Durin::VulkanRHI
