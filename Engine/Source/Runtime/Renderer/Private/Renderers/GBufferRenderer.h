#pragma once

#include "Materials/MaterialRenderProxy.h"
#include "RendererAPI.h"
#include "RHICommandList.h"
#include "RHIResources.h"

#include <array>
#include <limits>
#include <memory>

namespace Durin
{
	class FRendererResourceCoordinator;
	class FRendererTransientTargetPool;
	class FRHICommandListImmediate;

	enum class EGBufferVertexDomain : uint8
	{
		Local,
		Spline,
		Skeletal,
		Terrain,
	};

	// Owns byte-bounded geometry-buffer attachments without selecting the
	// production opaque rendering path.
	class RENDERER_API FGBufferRenderer final
	{
	public:
		struct FPipeline;

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
			FMaterialPipelineIdentity Material;
			FRHIRasterizerState Rasterizer;
			FRHIDepthStencilState Depth;
			FVertexDeclarationRHIRef VertexDeclaration;
			EGBufferVertexDomain VertexDomain = EGBufferVertexDomain::Local;
		};

		struct FVertexParameters
		{
			FRHIUniformBufferRange Transform;
			FRHIUniformBufferRange SplineMesh;
			FRHIStorageBufferRange SkinPalette;
			FRHITexture* HeightTexture = nullptr;
			FRHIUniformBufferRange Terrain;
			FRHIStorageBufferRange TerrainPatchOrigins;
		};

		struct FFragmentParameters
		{
			FRHIUniformBufferRange Material;
			std::array<FRHITexture*, 8> Textures{};
			std::array<FRHISampler*, 8> Samplers{};
		};

		FGBufferRenderer(FRendererResourceCoordinator& InCoordinator,
			FRendererTransientTargetPool& InTransientTargets);
		~FGBufferRenderer();

		FGBufferRenderer(const FGBufferRenderer&) = delete;
		auto operator=(const FGBufferRenderer&) -> FGBufferRenderer& = delete;

		auto EnsureTargets_RenderThread(uint32 Width, uint32 Height) -> FTargets*;
		auto EnsurePipeline_RenderThread(const FPipelineRequest& Request)
			-> FPipeline*;
		auto BindPipeline_RenderThread(
			FRHICommandListImmediate& CommandList,
			FPipeline& Pipeline,
			const FVertexParameters& VertexParameters,
			const FFragmentParameters& FragmentParameters) -> bool;
		auto ReleaseResources_RenderThread() -> void;

	private:
		struct FState;

		FRendererResourceCoordinator& Coordinator;
		FRendererTransientTargetPool& TransientTargets;
		std::unique_ptr<FState> State;
	};
} // namespace Durin
