#pragma once

#include "RendererAPI.h"
#include "RHIResources.h"

#include <memory>

namespace Durin
{
	class FRendererResourceCoordinator;
	class FRHICommandListImmediate;
	class FStaticMeshRenderer;
	class FSkeletalMeshRenderer;
	class FTerrainRenderer;
	struct FPreparedDirectionalShadow;
	struct FResolvedDirectionalShadow;
	struct FPreparedSkeletalPaletteTable;
	struct FResolvedSkeletalPaletteTable;
	struct FViewRenderTelemetry;

	using FShadowDepthTimingQuerySink = void (*)(
		const FGPUTimingQueryRHIRef& Query);
	RENDERER_API auto SetShadowDepthTimingQuerySink(
		FShadowDepthTimingQuerySink Sink) -> void;

	// Owns the fixed shadow target, exact views/sampler, failure slot, and pass.
	class FDirectionalShadowRenderer final
	{
	public:
		explicit FDirectionalShadowRenderer(
			FRendererResourceCoordinator& InCoordinator);
		~FDirectionalShadowRenderer();

		auto PrepareResources_RenderThread(
			FRHICommandListImmediate& CommandList,
			FStaticMeshRenderer& StaticMeshes,
			FSkeletalMeshRenderer& SkeletalMeshes,
			FTerrainRenderer& Terrains,
			const FPreparedDirectionalShadow& Shadow,
			FResolvedDirectionalShadow& ResolvedShadow,
			const FPreparedSkeletalPaletteTable& PreparedPalettes,
			FResolvedSkeletalPaletteTable& ResolvedPalettes,
			FViewRenderTelemetry& Telemetry) -> bool;
		auto Render_RenderThread(
			FRHICommandListImmediate& CommandList,
			FStaticMeshRenderer& StaticMeshes,
			FSkeletalMeshRenderer& SkeletalMeshes,
			FTerrainRenderer& Terrains,
			const FPreparedDirectionalShadow& Shadow,
			FResolvedDirectionalShadow& ResolvedShadow,
			FViewRenderTelemetry& Telemetry) -> bool;
		auto GetTexture_RenderThread() const -> FRHITexture*;
		auto GetSampledView_RenderThread() const -> FRHITextureView*;
		auto GetSampler_RenderThread() const -> FRHISampler*;
		auto ReleaseResources_RenderThread() -> void;

	private:
		struct FState;
		FRendererResourceCoordinator& Coordinator;
		std::unique_ptr<FState> State;
	};
}
