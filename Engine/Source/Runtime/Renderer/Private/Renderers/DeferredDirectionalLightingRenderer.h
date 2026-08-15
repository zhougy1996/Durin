#pragma once

#include "Math/DurinMath.h"
#include "RendererAPI.h"
#include "RHIResources.h"

#include <array>
#include <limits>
#include <memory>

namespace Durin
{
	class FFullscreenGeometryResources;
	class FRendererResourceCoordinator;
	class FRHICommandListImmediate;
	struct FSceneView;

	// Owns the isolated M3 qualification target and typed full-screen payload.
	class RENDERER_API FDeferredDirectionalLightingRenderer final
	{
	public:
		static constexpr uint64 BytesPerPixel = 8;
		static constexpr uint64 MaximumRetainedBytes =
			64ull * 1024ull * 1024ull;
		static constexpr auto CalculateTargetBytes(
			uint32 Width,
			uint32 Height) -> uint64
		{
			const uint64 Pixels = static_cast<uint64>(Width) * Height;
			return Pixels > std::numeric_limits<uint64>::max() / BytesPerPixel
				? std::numeric_limits<uint64>::max()
				: Pixels * BytesPerPixel;
		}

		struct alignas(16) FViewUniform
		{
			std::array<FVector4f, 4> ProjectionRows{};
			std::array<float, 16> ViewToWorld{};
			FVector4f ClearColor{0.0f};
			FVector4f Params{0.0f};
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
			FRHIUniformBufferRange Lighting;
			const FSceneView* View = nullptr;
			uint32 DiagnosticMode = 0;
		};

		FDeferredDirectionalLightingRenderer(
			FRendererResourceCoordinator& InCoordinator,
			FFullscreenGeometryResources& InFullscreenGeometry);
		~FDeferredDirectionalLightingRenderer();

		FDeferredDirectionalLightingRenderer(
			const FDeferredDirectionalLightingRenderer&) = delete;
		auto operator=(const FDeferredDirectionalLightingRenderer&)
			-> FDeferredDirectionalLightingRenderer& = delete;

		auto EnsureResources_RenderThread(
			FRHICommandListImmediate& CommandList) -> bool;
		auto EnsureTargets_RenderThread(uint32 Width, uint32 Height) -> FTargets*;
		auto Render_RenderThread(
			FRHICommandListImmediate& CommandList,
			FTargets& Targets,
			const FRenderParameters& Parameters) -> bool;
		auto ReleaseResources_RenderThread() -> void;

	private:
		struct FState;

		FRendererResourceCoordinator& Coordinator;
		FFullscreenGeometryResources& FullscreenGeometry;
		std::unique_ptr<FState> State;
	};

	static_assert(sizeof(FDeferredDirectionalLightingRenderer::FViewUniform)
		== 160);
	static_assert(alignof(FDeferredDirectionalLightingRenderer::FViewUniform)
		== 16);
} // namespace Durin
