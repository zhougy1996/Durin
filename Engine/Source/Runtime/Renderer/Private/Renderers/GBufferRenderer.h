#pragma once

#include "CoreMinimal.h"

#include "Renderers/SceneRendererProfiling.h"
#include "Renderers/ViewPreparationMath.h"

#include "Materials/MaterialRenderProxy.h"
#include "RendererAPI.h"
#include "RHICommandList.h"
#include "RHIResources.h"

namespace Durin
{
	namespace RendererPrivate { struct FMeshPassPipeline; }
	struct FGBufferPipeline;
	class FRendererResourceCoordinator;
	class FRHICommandListImmediate;
	class FRDGShaderParameterScope;
	struct FSceneView;

	struct alignas(16) FGBufferGPUCullingCandidate final
	{
		FVector3f BoundsMin{};
		uint32 ArgumentIndex = 0;
		FVector3f BoundsMax{};
		uint32 TransformIndex = 0;
	};
	static_assert(sizeof(FGBufferGPUCullingCandidate) == 32);

	struct alignas(16) FGBufferGPUCullingUniform final
	{
		std::array<FVector4f, 6> Planes{};
		FVector4f WorldOriginAndRelativeTolerance{};
		uint32 CandidateCount = 0;
		uint32 PlaneCount = 0;
		uint32 Padding[2]{};
	};
	static_assert(sizeof(FGBufferGPUCullingUniform) == 128);


	// Records geometry-buffer work into caller-provided attachments without
	// selecting the production opaque rendering path.
	class RENDERER_API FGBufferRenderer final
	{
	public:
		using FPipeline = FGBufferPipeline;

		static constexpr uint64 BytesPerPixel = 16;
		static constexpr uint64 MaximumRetainedBytes =
			128ull * 1024ull * 1024ull;
		static constexpr auto CalculateTargetBytes(
			uint32 Width,
			uint32 Height) -> uint64
		{
			const uint64 Pixels = static_cast<uint64>(Width) * Height;
			return Pixels > std::numeric_limits<uint64>::max() / BytesPerPixel
				? std::numeric_limits<uint64>::max()
				: Pixels * BytesPerPixel;
		}

		struct FTargets
		{
			FTextureRHIRef Material;
			FTextureRHIRef Normals;
			FTextureRHIRef Surface;
			FTextureRHIRef Emissive;
		};

		struct FPipelineRequest
		{
			FMaterialPlanningPassIdentity Material;
			std::shared_ptr<const FMaterialCompilerResult> CompiledProgram;
			FRHIRasterizerState Rasterizer;
			FRHIDepthStencilState Depth;
			FVertexDeclarationRHIRef VertexDeclaration;
			FXxHash64 FactoryKey;
			FXxHash64 LayoutKey;
			bool bGPUCulling = false;
			EGPUCullingFailure Failure = EGPUCullingFailure::None;
			FGraphicsPipelineStateInitializer::EPrimitiveTopology Topology = FGraphicsPipelineStateInitializer::EPrimitiveTopology::TriangleList;
		};


		FGBufferRenderer(FRendererResourceCoordinator& InCoordinator);
		~FGBufferRenderer();

		FGBufferRenderer(const FGBufferRenderer&) = delete;
		auto operator=(const FGBufferRenderer&) -> FGBufferRenderer& = delete;

		static auto DescribeTargets(uint32 Width, uint32 Height)
			-> std::array<FRHITextureCreateDesc, 4>;
		auto EnsurePipeline_RenderThread(const FPipelineRequest& Request)
			-> std::shared_ptr<const FPipeline>;
		static auto GetPipelineResources(const std::shared_ptr<const FPipeline>& Pipeline)
			-> std::shared_ptr<const RendererPrivate::FMeshPassPipeline>;
		auto ReleaseResources_RenderThread() -> void;
		auto EnsureGPUCullingResources_RenderThread(EGPUCullingFailure Failure = EGPUCullingFailure::None) -> bool;
		auto DispatchGPUCulling_RenderThread(
			FRHICommandListImmediate& CommandList,
			const FSceneView& View, const std::optional<FViewFrustum>& Frustum,
			uint32 CandidateCount,
			const FRDGShaderParameterScope& GraphShaderParameters,
			EGPUCullingFailure Failure = EGPUCullingFailure::None) -> bool;

	private:
		struct FState;

		FRendererResourceCoordinator& Coordinator;
		std::unique_ptr<FState> State;
	};
} // namespace Durin
