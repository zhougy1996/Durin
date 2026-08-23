#pragma once

#include "Math/DurinMath.h"
#include "RendererAPI.h"
#include "RHIResources.h"
#include "SceneView.h"

#include <array>
#include <limits>
#include <memory>
#include <optional>

namespace Durin
{
	class FFullscreenGeometryResources;
	class FRendererResourceCoordinator;
	class FRendererTransientTargetPool;
	class FRHICommandListImmediate;
	struct FSceneView;

	class RENDERER_API FGroundTruthAmbientOcclusionRenderer final
	{
	public:
		static constexpr uint64 BytesPerPixel = 1;
		static constexpr uint8 InvalidSelector = 0;
		static constexpr uint64 MaximumRetainedBytes = 32ull * 1024ull * 1024ull;
		struct FRectangle
		{
			uint32 X = 0;
			uint32 Y = 0;
			uint32 Width = 0;
			uint32 Height = 0;

			auto operator==(const FRectangle&) const -> bool = default;
		};
		struct FRepresentativeCandidate
		{
			bool bValid = false;
			float ViewDepth = 0.0f;
		};

		static constexpr auto CalculateHalfExtent(uint32 Extent) -> uint32
		{
			return Extent / 2 + Extent % 2;
		}
		static constexpr auto MapFullRectangleToHalf(FRectangle Rectangle)
			-> FRectangle
		{
			const uint64 EndX = static_cast<uint64>(Rectangle.X) + Rectangle.Width;
			const uint64 EndY = static_cast<uint64>(Rectangle.Y) + Rectangle.Height;
			const uint64 HalfEndX = EndX / 2 + EndX % 2;
			const uint64 HalfEndY = EndY / 2 + EndY % 2;
			const uint32 HalfX = Rectangle.X / 2;
			const uint32 HalfY = Rectangle.Y / 2;
			return {
				HalfX,
				HalfY,
				static_cast<uint32>(HalfEndX - HalfX),
				static_cast<uint32>(HalfEndY - HalfY)};
		}
		static constexpr auto EncodeSelector(uint32 LocalX, uint32 LocalY)
			-> uint8
		{
			return LocalX < 2 && LocalY < 2
				? static_cast<uint8>(1 + LocalY * 2 + LocalX)
				: InvalidSelector;
		}
		static constexpr auto DecodeSelector(uint8 Selector, uint32& LocalX,
			uint32& LocalY) -> bool
		{
			if (Selector < 1 || Selector > 4) return false;
			const uint32 Index = Selector - 1;
			LocalX = Index % 2;
			LocalY = Index / 2;
			return true;
		}
		static constexpr auto SelectRepresentative(
			const std::array<FRepresentativeCandidate, 4>& Candidates) -> uint8
		{
			uint8 Selected = InvalidSelector;
			float NearestDepth = std::numeric_limits<float>::max();
			for (uint32 Index = 0; Index < Candidates.size(); ++Index)
			{
				const FRepresentativeCandidate& Candidate = Candidates[Index];
				if (!Candidate.bValid || Candidate.ViewDepth < 0.0f
					|| Candidate.ViewDepth >= NearestDepth)
					continue;
				NearestDepth = Candidate.ViewDepth;
				Selected = static_cast<uint8>(Index + 1);
			}
			return Selected;
		}
		static constexpr auto CalculateRawTargetBytes(uint32 Width, uint32 Height)
			-> uint64
		{
			const uint64 Pixels = static_cast<uint64>(Width) * Height;
			return Pixels > std::numeric_limits<uint64>::max() / BytesPerPixel
				? std::numeric_limits<uint64>::max()
				: Pixels * BytesPerPixel;
		}
		static constexpr auto CalculateTargetBytes(uint32 Width, uint32 Height,
			EGroundTruthAmbientOcclusionQuality Quality) -> uint64
		{
			const uint64 RawBytes = CalculateRawTargetBytes(Width, Height);
			if (Quality == EGroundTruthAmbientOcclusionQuality::FullResolution)
			{
				return RawBytes > std::numeric_limits<uint64>::max() / 2
					? std::numeric_limits<uint64>::max()
					: RawBytes * 2;
			}
			const uint64 HalfBytes = CalculateRawTargetBytes(
				CalculateHalfExtent(Width), CalculateHalfExtent(Height));
			return HalfBytes > (std::numeric_limits<uint64>::max() - RawBytes) / 3
				? std::numeric_limits<uint64>::max()
				: RawBytes + HalfBytes * 3;
		}

		struct alignas(16) FViewUniform
		{
			std::array<FVector4f, 4> ProjectionRows{};
			std::array<FVector4f, 4> WorldToViewRows{};
			FVector4f Viewport{};
			FVector4f HalfViewport{};
			FVector4f Controls{0.75f, 0.60f, 96.0f, 0.05f};
		};

		struct FTargets
		{
			FTextureRHIRef Raw;
			FTextureRHIRef Scratch;
			FTextureRHIRef Selector;
			FTextureRHIRef Resolved;
			EGroundTruthAmbientOcclusionQuality Quality =
				EGroundTruthAmbientOcclusionQuality::HalfResolution;
		};

		struct alignas(16) FFilterUniform
		{
			std::array<FVector4f, 4> ProjectionRows{};
			FVector4f Viewport{};
			FVector4f HalfViewport{};
			FVector4f DirectionAndThresholds{};
		};

		FGroundTruthAmbientOcclusionRenderer(
			FRendererResourceCoordinator& InCoordinator,
			FFullscreenGeometryResources& InFullscreenGeometry,
			FRendererTransientTargetPool& InTransientTargets);
		~FGroundTruthAmbientOcclusionRenderer();

		FGroundTruthAmbientOcclusionRenderer(
			const FGroundTruthAmbientOcclusionRenderer&) = delete;
		auto operator=(const FGroundTruthAmbientOcclusionRenderer&)
			-> FGroundTruthAmbientOcclusionRenderer& = delete;

		auto EnsureTargets_RenderThread(uint32 Width, uint32 Height,
			EGroundTruthAmbientOcclusionQuality Quality)
			-> std::optional<FTargets>;
		auto RenderRaw_RenderThread(
			FRHICommandListImmediate& CommandList,
			const FTargets& Targets,
			FRHITexture* Normals,
			FRHITexture* Surface,
			FRHITexture* Depth,
			const FSceneView& View) -> bool;
		auto RenderFilter_RenderThread(
			FRHICommandListImmediate& CommandList,
			const FTargets& Targets,
			FRHITexture* Normals,
			FRHITexture* Surface,
			FRHITexture* Depth,
			const FSceneView& View) -> bool;
		auto RenderResolve_RenderThread(
			FRHICommandListImmediate& CommandList,
			const FTargets& Targets,
			FRHITexture* Normals,
			FRHITexture* Surface,
			FRHITexture* Depth,
			const FSceneView& View) -> bool;
		auto GetRetainedTargetBytes_RenderThread() const -> uint64;
		auto ReleaseResources_RenderThread() -> void;

	private:
		auto EnsureResources_RenderThread(
			FRHICommandListImmediate& CommandList) -> bool;
		struct FState;

		FRendererResourceCoordinator& Coordinator;
		FFullscreenGeometryResources& FullscreenGeometry;
		FRendererTransientTargetPool& TransientTargets;
		std::unique_ptr<FState> State;
	};

	static_assert(sizeof(FGroundTruthAmbientOcclusionRenderer::FViewUniform) == 176);
	static_assert(alignof(FGroundTruthAmbientOcclusionRenderer::FViewUniform) == 16);
	static_assert(sizeof(FGroundTruthAmbientOcclusionRenderer::FFilterUniform) == 112);
	static_assert(alignof(FGroundTruthAmbientOcclusionRenderer::FFilterUniform) == 16);
}
