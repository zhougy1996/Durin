#include "Renderers/MeshPipelineDescription.h"

#include "Renderers/MeshPassProcessor.h"

namespace Durin::RendererPrivate
{
	auto MakeMeshPipelineState(const FMaterialPlanningPassIdentity& Material,
		const FMeshPipelineRasterOptions& Options) -> FMeshPipelineState
	{
		FMeshPipelineState Result;
		Result.Rasterizer.PolygonMode = Options.bWireframe ? ERHIPolygonMode::Line : ERHIPolygonMode::Fill;
		Result.Rasterizer.CullMode = Material.bTwoSided || Options.bOrientationChanging ? ERHICullMode::None : ERHICullMode::Back;
		Result.Rasterizer.FrontFace = Options.bReverseCulling ? ERHIFrontFace::CounterClockwise : ERHIFrontFace::Clockwise;
		Result.Depth.bEnableTest = true;
		Result.Depth.CompareOp = Options.bReversedZ ? ERHIDepthCompareOp::GreaterOrEqual : ERHIDepthCompareOp::Less;
		const bool Translucent = Material.ShaderMap.BlendMode == EMaterialBlendMode::Translucent;
		Result.Depth.bEnableWrite = Material.DepthWritePolicy == EMaterialDepthWritePolicy::Enabled
			|| (Material.DepthWritePolicy == EMaterialDepthWritePolicy::Automatic && !Translucent);
		if (Translucent) Result.ColorBlend = FRHIColorBlendState::StraightAlpha();
		return Result;
	}

	auto MakeShadowMeshPipelineState(const FMeshPipelineState& Source) -> FMeshPipelineState
	{
		auto Result = Source;
		Result.Rasterizer.PolygonMode = ERHIPolygonMode::Fill;
		Result.Rasterizer.bEnableDepthBias = true;
		Result.Rasterizer.DepthBiasConstantFactor = 0.0f;
		Result.Rasterizer.DepthBiasSlopeFactor = 0.0f;
		Result.Rasterizer.DepthBiasClamp = 0.0f;
		Result.Depth.bEnableTest = true;
		Result.Depth.bEnableWrite = true;
		Result.Depth.CompareOp = ERHIDepthCompareOp::Less;
		Result.ColorBlend = {};
		return Result;
	}

	auto DescribeMeshPipeline(EMeshPipelinePass Pass, const FMeshPipelineState& State,
		FGraphicsPipelineStateInitializer::EPrimitiveTopology Topology,
		FMaterialShaderBlendModeKey BlendMode, bool bReversedZ) -> FMeshPipelineDescription
	{
		return FPassProcessorManager::GetMeshPassProcessor(Pass).DescribeMeshDraw(State, Topology, BlendMode, bReversedZ);
	}

	auto FMeshPipelineDescription::MakeInitializer(FRHIShader* Vertex, FRHIShader* Fragment,
		FRHIVertexDeclaration* Declaration, const FPipelineLayoutDesc& Layout) const -> FGraphicsPipelineStateInitializer
	{
		FGraphicsPipelineStateInitializer Result;
		Result.BoundShaders.VertexShader = Vertex;
		Result.BoundShaders.FragmentShader = Fragment;
		Result.VertexDeclaration = Declaration;
		Result.PipelineLayout = Layout;
		Result.RenderTargetLayout = RenderTargets;
		Result.PrimitiveTopology = Topology;
		Result.RasterizerState = Rasterizer;
		Result.DepthStencilState = Depth;
		Result.ColorBlendStates = ColorBlends;
		return Result;
	}
}
