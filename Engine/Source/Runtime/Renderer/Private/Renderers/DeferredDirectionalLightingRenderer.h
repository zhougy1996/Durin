#pragma once

#include "Math/DurinMath.h"
#include "RendererAPI.h"
#include "RHIResources.h"

#include <array>
#include <limits>
#include <memory>
#include <optional>

namespace Durin
{
	class FFullscreenGeometryResources;
	class FRendererResourceCoordinator;
	class FRHICommandListImmediate;
	struct FSceneView;

	// Records isolated directional lighting into a caller-provided target and
	// owns the typed full-screen payload.
	class RENDERER_API FDeferredDirectionalLightingRenderer final
	{
	public:
		static constexpr uint64 BytesPerPixel = 8;
		static constexpr uint64 MaximumRetainedBytes =
			64ull * 1024ull * 1024ull;
		static constexpr auto CalculateTargetBytes(
			uint32 Width,
			uint32 Height
		) -> uint64
		{
			const uint64 Pixels = static_cast<uint64>(Width) * Height;
			return Pixels > std::numeric_limits<uint64>::max() / BytesPerPixel ? std::numeric_limits<uint64>::max() : Pixels * BytesPerPixel;
		}

		struct alignas(16) FViewUniform
		{
			std::array<FVector4f, 4> ProjectionRows{};
			std::array<float, 16> ViewToWorld{};
			FVector4f ClearColor{0.0f};
			FVector4f Params{0.0f};
			FVector4f ContactParams{0.0f};
		};

		struct FTargets
		{
			FTextureRHIRef Color;
		};

		struct FRenderParameters
		{
			FRHITexture* Material = nullptr;
			FRHITexture* Normals = nullptr;
			FRHITexture* Surface = nullptr;
			FRHITexture* Emissive = nullptr;
			FRHITexture* Depth = nullptr;
			FRHITexture* EnvironmentIrradiance = nullptr;
			FRHITexture* EnvironmentPrefiltered = nullptr;
			FRHITexture* EnvironmentBrdfLut = nullptr;
			FRHISampler* EnvironmentSampler = nullptr;
			FRHITexture* DirectionalShadowTexture = nullptr;
			FRHISampler* DirectionalShadowSampler = nullptr;
			FRHITexture* GroundTruthAmbientOcclusionRaw = nullptr;
			FRHITexture* GroundTruthAmbientOcclusionFiltered = nullptr;
			FRHITexture* GroundTruthAmbientOcclusionResolved = nullptr;
			FRHITexture* GroundTruthAmbientOcclusionSelector = nullptr;
			FRHITexture* ContactVisibility = nullptr;
			FRHITexture* VolumetricCloudVisibility = nullptr;
			FRHIUniformBufferRange Lighting;
			const FSceneView* View = nullptr;
			uint32 DiagnosticMode = 0;
			uint32 GroundTruthAmbientOcclusionDebugMode = 0;
			bool bGroundTruthAmbientOcclusionEnabled = false;
			bool bGroundTruthAmbientOcclusionHalfResolution = false;
			bool bContactVisibilityEnabled = false;
			bool bContactVisibilityDebug = false;
			bool bVolumetricCloudVisibilityEnabled = false;
		};

		FDeferredDirectionalLightingRenderer(
			FRendererResourceCoordinator& InCoordinator,
			FFullscreenGeometryResources& InFullscreenGeometry
		);
		~FDeferredDirectionalLightingRenderer();

		FDeferredDirectionalLightingRenderer(
			const FDeferredDirectionalLightingRenderer&
		) = delete;
		auto operator=(const FDeferredDirectionalLightingRenderer&)
			-> FDeferredDirectionalLightingRenderer& = delete;

		auto EnsureResources_RenderThread(
			FRHICommandListImmediate& CommandList
		) -> bool;
		static auto DescribeTarget(uint32 Width, uint32 Height)
			-> FRHITextureCreateDesc;
		auto Render_RenderThread(
			FRHICommandListImmediate& CommandList,
			const FTargets& Targets,
			const FRenderParameters& Parameters
		) -> bool;
		auto RenderProduction_RenderThread(
			FRHICommandListImmediate& CommandList,
			FRHITexture* SceneColor,
			const FRenderParameters& Parameters
		) -> bool;
		auto ReleaseResources_RenderThread() -> void;

	private:
		auto RenderInternal_RenderThread(
			FRHICommandListImmediate& CommandList,
			FRHITexture* SceneColor,
			const FRenderParameters& Parameters,
			bool bProduction
		) -> bool;
		struct FState;

		FRendererResourceCoordinator& Coordinator;
		FFullscreenGeometryResources& FullscreenGeometry;
		std::unique_ptr<FState> State;
	};

	static_assert(sizeof(FDeferredDirectionalLightingRenderer::FViewUniform) == 176);
	static_assert(alignof(FDeferredDirectionalLightingRenderer::FViewUniform) == 16);
} // namespace Durin
