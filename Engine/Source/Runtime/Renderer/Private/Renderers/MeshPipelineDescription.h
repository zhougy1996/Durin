#pragma once

#include "CoreMinimal.h"
#include "Materials/MaterialRenderTypes.h"
#include "Materials/MaterialTypes.h"
#include "RendererAPI.h"
#include "RHIResources.h"
#include "Shader/MaterialShaderIdentity.h"

namespace Durin::RendererPrivate
{
	struct FMeshPipelineState
	{
		FRHIRasterizerState Rasterizer;
		FRHIDepthStencilState Depth;
		FRHIColorBlendState ColorBlend;
	};

	struct FMeshPipelineRasterOptions
	{
		bool bWireframe = false;
		bool bReverseCulling = false;
		bool bOrientationChanging = false;
		bool bReversedZ = false;
	};

	enum class EMeshPipelinePass : uint8
	{
		Forward,
		HybridRetainedForward,
		SortedTranslucency,
		ShadowDepth,
		GBuffer,
		HitProxy,
	};

	// Value-only pass description: no scene, visibility, shader map, native shader,
	// vertex buffers or command list is needed to collect it. Shader selection and
	// vertex declaration preparation remain with their respective owners.
	struct FMeshPipelineDescription
	{
		FRHIRenderTargetLayout RenderTargets;
		FGraphicsPipelineStateInitializer::EPrimitiveTopology Topology = FGraphicsPipelineStateInitializer::EPrimitiveTopology::TriangleList;
		FRHIRasterizerState Rasterizer;
		FRHIDepthStencilState Depth;
		std::array<FRHIColorBlendState, MaxSimultaneousRenderTargets> ColorBlends{};

		// The returned initializer borrows shaders/declaration. The caller retains
		// those resources until the pipeline service has captured the request.
		RENDERER_API auto MakeInitializer(FRHIShader* Vertex, FRHIShader* Fragment,
			FRHIVertexDeclaration* Declaration, const FPipelineLayoutDesc& Layout) const
			-> FGraphicsPipelineStateInitializer;
	};

	RENDERER_API auto MakeMeshPipelineState(const FMaterialPlanningPassIdentity& Material,
		const FMeshPipelineRasterOptions& Options) -> FMeshPipelineState;
	// Normalizes dynamic shadow-bias magnitudes out of pipeline identity.
	RENDERER_API auto MakeShadowMeshPipelineState(const FMeshPipelineState& Source) -> FMeshPipelineState;
	RENDERER_API auto DescribeMeshPipeline(EMeshPipelinePass Pass, const FMeshPipelineState& State,
		FGraphicsPipelineStateInitializer::EPrimitiveTopology Topology,
		FMaterialShaderBlendModeKey BlendMode = EMaterialBlendMode::Opaque, bool bReversedZ = false)
		-> FMeshPipelineDescription;
}
