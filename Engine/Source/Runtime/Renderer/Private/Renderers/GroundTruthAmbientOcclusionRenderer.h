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

	class RENDERER_API FGroundTruthAmbientOcclusionRenderer final
	{
	public:
		static constexpr uint64 BytesPerPixel = 1;
		static constexpr uint64 MaximumRetainedBytes = 32ull * 1024ull * 1024ull;
		static constexpr auto CalculateRawTargetBytes(uint32 Width, uint32 Height)
			-> uint64
		{
			const uint64 Pixels = static_cast<uint64>(Width) * Height;
			return Pixels > std::numeric_limits<uint64>::max() / BytesPerPixel
				? std::numeric_limits<uint64>::max()
				: Pixels * BytesPerPixel;
		}
		static constexpr auto CalculateTargetBytes(uint32 Width, uint32 Height)
			-> uint64
		{
			const uint64 RawBytes = CalculateRawTargetBytes(Width, Height);
			return RawBytes > std::numeric_limits<uint64>::max() / 2
				? std::numeric_limits<uint64>::max()
				: RawBytes * 2;
		}

		struct alignas(16) FViewUniform
		{
			std::array<FVector4f, 4> ProjectionRows{};
			std::array<FVector4f, 4> WorldToViewRows{};
			FVector4f Viewport{};
			FVector4f Controls{0.75f, 0.60f, 96.0f, 0.05f};
		};

		struct FTargets
		{
			FTextureRHIRef Raw;
			FTextureRHIRef Scratch;
		};

		struct alignas(16) FFilterUniform
		{
			std::array<FVector4f, 4> ProjectionRows{};
			FVector4f Viewport{};
			FVector4f DirectionAndThresholds{};
		};

		FGroundTruthAmbientOcclusionRenderer(
			FRendererResourceCoordinator& InCoordinator,
			FFullscreenGeometryResources& InFullscreenGeometry);
		~FGroundTruthAmbientOcclusionRenderer();

		FGroundTruthAmbientOcclusionRenderer(
			const FGroundTruthAmbientOcclusionRenderer&) = delete;
		auto operator=(const FGroundTruthAmbientOcclusionRenderer&)
			-> FGroundTruthAmbientOcclusionRenderer& = delete;

		auto EnsureTargets_RenderThread(uint32 Width, uint32 Height) -> FTargets*;
		auto RenderRaw_RenderThread(
			FRHICommandListImmediate& CommandList,
			FTargets& Targets,
			FRHITexture* Normals,
			FRHITexture* Surface,
			FRHITexture* Depth,
			const FSceneView& View) -> bool;
		auto RenderFilter_RenderThread(
			FRHICommandListImmediate& CommandList,
			FTargets& Targets,
			FRHITexture* Normals,
			FRHITexture* Surface,
			FRHITexture* Depth,
			const FSceneView& View) -> bool;
		auto ReleaseResources_RenderThread() -> void;

	private:
		auto EnsureResources_RenderThread(
			FRHICommandListImmediate& CommandList) -> bool;
		struct FState;

		FRendererResourceCoordinator& Coordinator;
		FFullscreenGeometryResources& FullscreenGeometry;
		std::unique_ptr<FState> State;
	};

	static_assert(sizeof(FGroundTruthAmbientOcclusionRenderer::FViewUniform) == 160);
	static_assert(alignof(FGroundTruthAmbientOcclusionRenderer::FViewUniform) == 16);
	static_assert(sizeof(FGroundTruthAmbientOcclusionRenderer::FFilterUniform) == 96);
	static_assert(alignof(FGroundTruthAmbientOcclusionRenderer::FFilterUniform) == 16);
}
