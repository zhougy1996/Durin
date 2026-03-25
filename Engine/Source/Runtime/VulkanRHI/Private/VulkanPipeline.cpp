#include "VulkanPipeline.h"

#include "RHIResources.h"
#include "VulkanCommandBuffer.h"
#include "VulkanCommon.h"
#include "VulkanContext.h"
#include "VulkanDynamicRHI.h"
#include "VulkanDevice.h"
#include "VulkanRenderPass.h"
#include "VulkanShader.h"

namespace Doge::VulkanRHI
{
	FVulkanGraphicsPipelineState::FVulkanGraphicsPipelineState(FVulkanDevice& InDevice, const FGraphicsPipelineStateInitializer& Initializer)
		: Device(InDevice)
	{
		// TODO: Correctly set the renderpass
		FVulkanRenderPassManager& RenderPassManager = Device.GetRenderPassManager();
		// State.SetViewport(0.0f, 0.0f, 0.0f, 800.0f, 600.0f, 1.0f);

		vk::Format VkFormat = FVulkanPixelFormat::FromPixelFormat(Initializer.PixelFormat);
		RenderPass = RenderPassManager.GetOrCreateRenderPass(Initializer.RenderPassName, VkFormat);

		Shaders[SHADER_STAGE_VERTEX] = new FVulkanShader(Device, "../../../../Shaders/spv/test_vert.spv", vk::ShaderStageFlagBits::eVertex);
		Shaders[SHADER_STAGE_PIXEL] = new FVulkanShader(Device, "../../../../Shaders/spv/test_frag.spv", vk::ShaderStageFlagBits::eFragment);

		vk::PipelineShaderStageCreateInfo VertShaderInfo;
		VertShaderInfo
			.setStage(vk::ShaderStageFlagBits::eVertex)
			.setModule(Shaders[SHADER_STAGE_VERTEX]->GetShaderModule())
			.setPName("main");

		vk::PipelineShaderStageCreateInfo FragmentShaderInfo;
		FragmentShaderInfo
			.setStage(vk::ShaderStageFlagBits::eFragment)
			.setModule(Shaders[SHADER_STAGE_PIXEL]->GetShaderModule())
			.setPName("main");

		vk::PipelineShaderStageCreateInfo ShaderStages[] = {VertShaderInfo, FragmentShaderInfo};

		std::vector<vk::DynamicState> DynamicStates = {
			vk::DynamicState::eViewport,
			vk::DynamicState::eScissor
		};

		vk::PipelineDynamicStateCreateInfo DynamicStateInfo;
		DynamicStateInfo.setDynamicStates(DynamicStates);

		vk::PipelineVertexInputStateCreateInfo VertexInputInfo;

		vk::PipelineInputAssemblyStateCreateInfo InputAssemblyInfo;
		InputAssemblyInfo
			.setTopology(vk::PrimitiveTopology::eTriangleList)
			.setPrimitiveRestartEnable(vk::False);

		vk::Extent2D SwapChainExtent = {800, 600};

		// Will be set dynamically
		vk::PipelineViewportStateCreateInfo ViewportStateInfo;
		ViewportStateInfo.setViewportCount(1).setScissorCount(1);
		// ViewportStateInfo
		//	.setViewports(Viewport_)
		//	.setScissors(Scissor_);

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

		vk::PipelineLayoutCreateInfo pipelineLayoutInfo;
		pipelineLayoutInfo
			.setSetLayouts(nullptr)
			.setPushConstantRangeCount(0)
			.setPPushConstantRanges(nullptr);

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
		}
		else
		{
			Pipeline = PipelineCreationResult.value;
			DOGE_TRACE("Vulkan graphics pipeline created");
		}
	}

	FVulkanGraphicsPipelineState::~FVulkanGraphicsPipelineState()
	{
		delete Shaders[SHADER_STAGE_VERTEX];
		delete Shaders[SHADER_STAGE_PIXEL];
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

	auto FVulkanGraphicsPipelineState::SetScissorRect(uint32 MinX, uint32 MinY, uint32 Width, uint32 Height) -> void
	{
		Scissor
			.setOffset({static_cast<int32>(MinX), static_cast<int32>(MinY)})
			.setExtent({Width, Height});
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