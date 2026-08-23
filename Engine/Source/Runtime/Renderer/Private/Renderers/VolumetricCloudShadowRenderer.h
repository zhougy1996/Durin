#pragma once

#include "Renderers/VolumetricCloudSpatialRenderer.h"
#include "RHIResources.h"

#include <memory>

namespace Durin
{
	class FRendererResourceCoordinator;
	class FRendererTransientTargetPool;
	class FFullscreenGeometryResources;
	class FRHICommandListImmediate;
	struct FSceneView;

	class RENDERER_API FVolumetricCloudShadowRenderer final
	{
	public:
		enum class ERoute : uint8 { FactorOne, Compute, Fragment };
		enum class ERouteReason : uint8
		{
			DisabledOrUnneeded,
			InvalidInputs,
			InvalidExtent,
			Compute,
			ComputePayloadUnavailable,
			ComputeTargetUnavailable,
			ComputeExtentUnsupported,
			FragmentUnavailable,
			Count
		};
		struct FTargets { FTextureRHIRef Visibility; };
		struct FComputeTargets
		{
			FTextureRHIRef Visibility;
			FTextureViewRHIRef SampledView;
			FTextureViewRHIRef StorageView;
		};
		struct FRenderInput
		{
			bool bRequested = false;
			FRHITexture* BaseDensity = nullptr;
			FRHITexture* DetailDensity = nullptr;
			FRHITexture* Weather = nullptr;
			FRHITexture* SceneDepth = nullptr;
			FRHISampler* DensitySampler = nullptr;
			FVolumetricCloudSpatialRenderer::FParameters Parameters;
			const FSceneView* View = nullptr;
			FVolumetricCloudSpatialRenderer::EQualityTier QualityTier =
				FVolumetricCloudSpatialRenderer::DefaultQualityTier;
			uint32 Width = 0;
			uint32 Height = 0;
		};
		struct FRenderResult
		{
			FRHITexture* Visibility = nullptr;
			ERoute Route = ERoute::FactorOne;
			ERouteReason Reason = ERouteReason::DisabledOrUnneeded;
			uint32 SampleCount = 0;
			uint64 TargetBytes = 0;
		};
		using FTimingQuerySink = void (*)(const FGPUTimingQueryRHIRef&, ERoute);
		using FCaptureSink = void (*)(FRHITexture*, ERoute);
		static auto SetTimingQuerySink(FTimingQuerySink Sink) -> void;
		static auto SetCaptureSink(FCaptureSink Sink) -> void;

		static constexpr uint64 BytesPerPixel = 1;
		static constexpr uint64 MaximumRetainedBytesPerRoute = 16ull * 1024ull * 1024ull;
		static constexpr uint32 ThreadGroupSize = 8;
		static constexpr auto CalculateTargetBytes(uint32 Width, uint32 Height) -> uint64
		{
			return static_cast<uint64>(Width) * Height;
		}
		static constexpr auto CalculateGroupCount(uint32 Extent) -> uint32
		{
			return Extent == 0 ? 0 : (Extent + ThreadGroupSize - 1) / ThreadGroupSize;
		}
		static constexpr auto ResolveSampleCount(
			FVolumetricCloudSpatialRenderer::EQualityTier Tier) -> uint32
		{
			switch (Tier)
			{
			case FVolumetricCloudSpatialRenderer::EQualityTier::Performance: return 4;
			case FVolumetricCloudSpatialRenderer::EQualityTier::High: return 6;
			case FVolumetricCloudSpatialRenderer::EQualityTier::Epic:
			case FVolumetricCloudSpatialRenderer::EQualityTier::Reference: return 8;
			}
			return 6;
		}

		FVolumetricCloudShadowRenderer(FRendererResourceCoordinator& InCoordinator,
			FFullscreenGeometryResources& InFullscreenGeometry,
			FRendererTransientTargetPool& InTransientTargets);
		~FVolumetricCloudShadowRenderer();
		FVolumetricCloudShadowRenderer(const FVolumetricCloudShadowRenderer&) = delete;
		auto operator=(const FVolumetricCloudShadowRenderer&)
			-> FVolumetricCloudShadowRenderer& = delete;

		auto EnsureTargets_RenderThread(uint32 Width, uint32 Height) -> FTargets*;
		auto EnsureComputeTargets_RenderThread(uint32 Width, uint32 Height) -> FComputeTargets*;
		auto Render_RenderThread(FRHICommandListImmediate& CommandList,
			FTargets* FragmentTargets, FComputeTargets* ComputeTargets,
			const FRenderInput& Input) -> FRenderResult;
		auto GetRetainedTargetBytes_RenderThread() const -> uint64;
		auto ReleaseResources_RenderThread() -> void;

	private:
		struct FState;
		FRendererResourceCoordinator& Coordinator;
		FFullscreenGeometryResources& FullscreenGeometry;
		FRendererTransientTargetPool& TransientTargets;
		std::unique_ptr<FState> State;
	};
}
