#include "VulkanPipeline.h"

#include "VulkanCommon.h"
#include "VulkanDynamicRHI.h"
#include "VulkanDevice.h"
#include "VulkanRenderPass.h"
#include "VulkanShader.h"

FVulkanGraphicsPipelineState::FVulkanGraphicsPipelineState(FVulkanDevice& Device)
	: Device_(Device)
{
	// TODO: Correctly set the renderpass
	FVulkanRenderPassManager& RenderPassManager = Device_.GetRenderPassManager();
	// State.SetViewport(0.0f, 0.0f, 0.0f, 800.0f, 600.0f, 1.0f);
	RenderPass_ = RenderPassManager.GetOrCreateRenderPass();

	Shaders_[SHADER_STAGE_VERTEX] = new FVulkanShader(Device_, "../../../../Shaders/spv/test_vert.spv", vk::ShaderStageFlagBits::eVertex);
	Shaders_[SHADER_STAGE_PIXEL] = new FVulkanShader(Device_, "../../../../Shaders/spv/test_frag.spv", vk::ShaderStageFlagBits::eFragment);

	vk::PipelineShaderStageCreateInfo VertShaderInfo;
	VertShaderInfo
		.setStage(vk::ShaderStageFlagBits::eVertex)
		.setModule(Shaders_[SHADER_STAGE_VERTEX]->GetShaderModule())
		.setPName("main");

	vk::PipelineShaderStageCreateInfo FragmentShaderInfo;
	FragmentShaderInfo
		.setStage(vk::ShaderStageFlagBits::eFragment)
		.setModule(Shaders_[SHADER_STAGE_PIXEL]->GetShaderModule())
		.setPName("main");

	vk::PipelineShaderStageCreateInfo ShaderStages[] = {VertShaderInfo, FragmentShaderInfo};

	TArray<vk::DynamicState> DynamicStates = {
		vk::DynamicState::eViewport,
		vk::DynamicState::eScissor};

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

	vk::PipelineLayout PipelineLayout;
	try
	{
		PipelineLayout = Device_.GetHandle().createPipelineLayout(pipelineLayoutInfo);
		DOGE_DEBUG("Vulkan pipeline layout created");
	}
	catch (const std::runtime_error& err)
	{
		DOGE_ERROR("Failed to create vulkan pipeline layout: {}", err.what());
	}

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
		.setRenderPass(RenderPass_->GetHandle())
		.setSubpass(0)
		.setBasePipelineHandle(nullptr)
		.setBasePipelineIndex(-1);

	vk::ResultValue<vk::Pipeline> PipelineCreationResult = Device_.GetHandle().createGraphicsPipeline(nullptr, pipelineInfo);
	if (PipelineCreationResult.result != vk::Result::eSuccess)
	{
		DOGE_ERROR("Failed to create vulkan graphics pipeline: {}", vk::to_string(PipelineCreationResult.result));
	}
	else
	{
		Pipeline_ = PipelineCreationResult.value;
		DOGE_DEBUG("Vulkan graphics pipeline created");
	}
}

auto FVulkanGraphicsPipelineState::SetViewport(float MinX, float MinY, float MinZ, float MaxX, float MaxY, float MaxZ) -> void
{
	float MaxDepth = MinZ == MaxZ ? MinZ + 1.0f : MaxZ;

	Viewport_
		.setX(MinX)
		.setY(MinY)
		.setWidth(MaxX - MinX)
		.setHeight(MaxY - MinY)
		.setMinDepth(MinZ)
		.setMaxDepth(MaxDepth);

	SetScissorRect(static_cast<uint32>(MinX), static_cast<uint32>(MinY), static_cast<uint32>(MaxX - MinX), static_cast<uint32>(MaxY - MinY));
	bScissorEnabled_ = false;
}

auto FVulkanGraphicsPipelineState::SetScissor(float MinX, float MinY, float Width, float Height) -> void
{
	SetScissorRect(static_cast<uint32>(MinX), static_cast<uint32>(MinY), static_cast<uint32>(Width), static_cast<uint32>(Height));
	bScissorEnabled_ = true;
}

auto FVulkanGraphicsPipelineState::Bind(vk::CommandBuffer CmdBuffer) -> void
{
	CmdBuffer.bindPipeline(vk::PipelineBindPoint::eGraphics, Pipeline_);
}

auto FVulkanGraphicsPipelineState::SetScissorRect(uint32 MinX, uint32 MinY, uint32 Width, uint32 Height) -> void
{
	Scissor_
		.setOffset({static_cast<int32>(MinX), static_cast<int32>(MinY)})
		.setExtent({Width, Height});
}


FVulkanPipelineManager::FVulkanPipelineManager(FVulkanDevice& Device)
	: Device_(Device)
{
}

auto FVulkanPipelineManager::CreateGraphicsPipelineState(const FGraphicsPipelineStateInitializer& Initializer) -> TSharedPtr<FVulkanGraphicsPipelineState>
{
	auto State = std::make_shared<FVulkanGraphicsPipelineState>(Device_);

	return State;
}

auto FVulkanDynamicRHI::RHICreateGraphicsPipelineState(const FGraphicsPipelineStateInitializer& Initializer) -> TSharedPtr<FRHIGraphicsPipelineState>
{
	return Device_->GetPipelineManager().CreateGraphicsPipelineState(Initializer);
}