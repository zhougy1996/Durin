#pragma once

#include "CoreMinimal.h"

#include "Renderers/ViewPreparationMath.h"

#include "Materials/MaterialRenderProxy.h"
#include "RendererAPI.h"
#include "RHICommandList.h"
#include "RHIResources.h"

namespace Durin
{
	namespace RendererPrivate { struct FMeshPassPipeline; }
	class FRendererResourceCoordinator;
	class FRHICommandListImmediate;
	class FRDGShaderParameterScope;
	struct FSceneView;

	// Records geometry-buffer work into caller-provided attachments without
	// selecting the production opaque rendering path.
	class RENDERER_API FGBufferRenderer final
	{
	public:
		using FPipeline = RendererPrivate::FMeshPassPipeline;

		static constexpr uint64 BytesPerPixel = 12;
		static constexpr uint64 MaximumRetainedBytes =
			128ull * 1024ull * 1024ull;
		static constexpr auto CalculateTargetBytes(
			uint32 Width,
			uint32 Height,
			bool bModelData = false
		) -> uint64
		{
			const uint64 Cost = BytesPerPixel + (bModelData ? 4 : 0);
			const uint64 Pixels = static_cast<uint64>(Width) * Height;
			return Pixels > std::numeric_limits<uint64>::max() / Cost ? std::numeric_limits<uint64>::max() : Pixels * Cost;
		}

		struct FTargets
		{
			FTextureRHIRef Material;
			FTextureRHIRef Normals;
			FTextureRHIRef Surface;
			FTextureRHIRef ModelData;
		};

		struct FPipelineRequest
		{
			FMaterialPlanningPassIdentity Material;
			std::shared_ptr<const FMaterialProgram> CompiledProgram;
			FRHIRasterizerState Rasterizer;
			FRHIDepthStencilState Depth;
			FVertexDeclarationRHIRef VertexDeclaration;
			FXxHash64 FactoryKey;
			FXxHash64 LayoutKey;
				FGraphicsPipelineStateInitializer::EPrimitiveTopology Topology = FGraphicsPipelineStateInitializer::EPrimitiveTopology::TriangleList;
				bool bModelData = false;
		};


		FGBufferRenderer(FRendererResourceCoordinator& InCoordinator);
		~FGBufferRenderer();

		FGBufferRenderer(const FGBufferRenderer&) = delete;
		auto operator=(const FGBufferRenderer&) -> FGBufferRenderer& = delete;

		static auto DescribeTargets(uint32 Width, uint32 Height)
			-> std::array<FRHITextureCreateDesc, 3>;
		auto EnsurePipeline_RenderThread(const FPipelineRequest& Request)
			-> std::shared_ptr<const FPipeline>;

		auto ReleaseResources_RenderThread() -> void;

	private:
		struct FState;

		FRendererResourceCoordinator& Coordinator;
		std::unique_ptr<FState> State;
	};
} // namespace Durin
