#pragma once

#include "Renderers/VolumetricCloudSpatialRenderer.h"
#include "RHIResources.h"

#include <memory>

namespace Durin
{
	class FRendererResourceCoordinator;
	class FFullscreenGeometryResources;
	class FRHICommandListImmediate;
	struct FSceneView;

	// Owns the complete P1 GPU cloud producer and composition resources. Scene
	// authoring remains outside this type: callers publish immutable parameters
	// and generic texture bindings for one view.
	class RENDERER_API FVolumetricCloudRenderer final
	{
	public:
		using FSpatial = FVolumetricCloudSpatialRenderer;
		using ERoute = FSpatial::ERoute;
		using ERouteReason = FSpatial::ERouteReason;
		using FParameters = FSpatial::FParameters;
		using FTextureBindings = FSpatial::FTextureBindings;
		using FExecutionCounters = FSpatial::FExecutionCounters;

		using FTimingQuerySink = void (*)(const FGPUTimingQueryRHIRef&, ERoute);
		using FCaptureSink = void (*)(FRHITexture*, ERoute);
		static auto SetTimingQuerySink(FTimingQuerySink Sink) -> void;
		static auto SetCaptureSink(FCaptureSink Sink) -> void;

		struct FTargets
		{
			FTextureRHIRef Cloud;
		};

		struct FComputeTargets
		{
			FTextureRHIRef Cloud;
			FTextureViewRHIRef SampledView;
			FTextureViewRHIRef StorageView;
		};

		struct FRenderInput
		{
			bool bRequested = false;
			FTextureBindings Textures;
			FParameters Parameters;
			const FSceneView* View = nullptr;
			uint32 Width = 0;
			uint32 Height = 0;
		};

		struct FRenderResult
		{
			FRHITexture* Cloud = nullptr;
			FExecutionCounters Counters;
		};

		FVolumetricCloudRenderer(FRendererResourceCoordinator& InCoordinator,
			FFullscreenGeometryResources& InFullscreenGeometry);
		~FVolumetricCloudRenderer();
		FVolumetricCloudRenderer(const FVolumetricCloudRenderer&) = delete;
		auto operator=(const FVolumetricCloudRenderer&)
			-> FVolumetricCloudRenderer& = delete;

		auto EnsureTargets_RenderThread(uint32 Width, uint32 Height) -> FTargets*;
		auto EnsureComputeTargets_RenderThread(uint32 Width, uint32 Height)
			-> FComputeTargets*;
		auto EnsureDensitySampler_RenderThread() -> FRHISampler*;
		auto Render_RenderThread(FRHICommandListImmediate& CommandList,
			FTargets* FragmentTargets, FComputeTargets* ComputeTargets,
			const FRenderInput& Input) -> FRenderResult;
		auto Composite_RenderThread(FRHICommandListImmediate& CommandList,
			FRHITexture* SceneColor, FRHITexture* Cloud, const FSceneView& View)
			-> FRHITexture*;
		auto GetRetainedTargetBytes_RenderThread() const -> uint64;
		auto ReleaseResources_RenderThread() -> void;

	private:
		struct FState;
		auto EnsureCompositeTargets_RenderThread(uint32 Width, uint32 Height)
			-> FTargets*;
		FRendererResourceCoordinator& Coordinator;
		FFullscreenGeometryResources& FullscreenGeometry;
		std::unique_ptr<FState> State;
	};
} // namespace Durin
