#pragma once

#include "Materials/MaterialRenderProxy.h"
#include "RendererAPI.h"
#include "RHICommandList.h"
#include "RHIResources.h"
#include "VertexFactory.h"

#include <array>
#include <limits>
#include <memory>
#include <optional>

namespace Durin
{
	namespace RendererPrivate { class FMeshVertexShaderBinding; class FCompiledSurfaceBindingLayout; class FPreparedSurfaceMaterialBindings; }
	struct FGBufferPipeline;
	class FRendererResourceCoordinator;
	class FRHICommandListImmediate;


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
		auto GetVertexBinding(const FPipeline& Pipeline, const FVertexFactoryBinding& Binding) const
			-> std::shared_ptr<const RendererPrivate::FMeshVertexShaderBinding>;
		auto GetFragmentShader(const FPipeline& Pipeline) const -> FRHIShader*;
		auto GetSurfaceLayout(const FPipeline& Pipeline) const -> const RendererPrivate::FCompiledSurfaceBindingLayout&;
		auto BindPipeline_RenderThread(
			FRHICommandListImmediate& CommandList,
			const FPipeline& Pipeline,
			const std::shared_ptr<const FRHIShaderParameterBatch>& VertexBindings,
			const RendererPrivate::FPreparedSurfaceMaterialBindings& FragmentBindings) -> bool;
		auto ReleaseResources_RenderThread() -> void;

	private:
		struct FState;

		FRendererResourceCoordinator& Coordinator;
		std::unique_ptr<FState> State;
	};
} // namespace Durin
