#pragma once

#include "Renderers/VolumetricCloudSpatialRenderer.h"
#include "RHIResources.h"

#include <memory>
#include <optional>

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
		struct FRenderPolicy
		{
			bool bPreparationOnly = false;
			bool bInputsExpected = false;
			bool bFragmentTargetExpected = false;
			bool bComputeTargetExpected = false;
			bool bGraphManagedTextureAccess = false;
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
			case FVolumetricCloudSpatialRenderer::EQualityTier::Count: return 6;
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

		auto EnsureTargets_RenderThread(uint32 Width, uint32 Height)
			-> std::optional<FTargets>;
		auto EnsureComputeTargets_RenderThread(uint32 Width, uint32 Height)
			-> std::optional<FComputeTargets>;
		auto Render_RenderThread(FRHICommandListImmediate& CommandList,
			const FTargets* FragmentTargets,
			const FComputeTargets* ComputeTargets,
			const FRenderInput& Input,
			const FRenderPolicy& Policy) -> FRenderResult;
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
