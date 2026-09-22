#pragma once

#include "RendererAPI.h"
#include "RHIResources.h"

#include <memory>
#include <optional>
#include <span>

namespace Durin
{
	class FRendererResourceCoordinator;
	class FRHICommandListImmediate;
	class FRHICommandList;
	struct FPreparedStaticMeshView;
	struct FResolvedStaticMeshView;
	struct FStaticMeshRenderObservations;
	struct FDirectionalShadowCascadeRecording
	{
		FTextureRHIRef Target;
		FTextureViewRHIRef DepthAttachment;
	};
	class FStaticMeshRenderer;
	struct FPreparedDirectionalShadow;
	struct FResolvedDirectionalShadow;
	struct FViewRenderTelemetry;

	using FShadowDepthTimingQuerySink = void (*)(
		const FGPUTimingQueryRHIRef& Query);
	RENDERER_API auto SetShadowDepthTimingQuerySink(
		FShadowDepthTimingQuerySink Sink) -> void;

	using FShadowDepthCaptureSink = void (*)(FRHICommandListImmediate&, FRHITexture*, uint32);
	RENDERER_API auto SetShadowDepthCaptureSink(FShadowDepthCaptureSink Sink) -> void;
	auto HasShadowDepthTimingQuerySink() -> bool;

	// Owns the fixed shadow target, exact views/sampler, failure slot, and pass.
	class FDirectionalShadowRenderer final
	{
	public:
		explicit FDirectionalShadowRenderer(
			FRendererResourceCoordinator& InCoordinator);
		~FDirectionalShadowRenderer();
		RENDERER_API auto CaptureCascade_RenderThread(FRHITexture* Target, uint32 Cascade) const
			-> std::optional<FDirectionalShadowCascadeRecording>;
		RENDERER_API static auto RecordCascade(FRHICommandList& Commands,
			const FDirectionalShadowCascadeRecording& Recording,
			const FPreparedStaticMeshView& Prepared, const FResolvedStaticMeshView& Resolved)
			-> FStaticMeshRenderObservations;
		static auto Complete_RenderThread(FRHICommandListImmediate& Commands,
			FRHITexture* Target, std::span<const FStaticMeshRenderObservations> Counts,
			FResolvedDirectionalShadow& ResolvedShadow, FViewRenderTelemetry& Telemetry) -> void;

		auto PrepareResources_RenderThread(
			FRHICommandListImmediate& CommandList,
			FStaticMeshRenderer& StaticMeshes,
			const FPreparedDirectionalShadow& Shadow,
			FResolvedDirectionalShadow& ResolvedShadow,
			FViewRenderTelemetry& Telemetry) -> bool;
		auto Render_RenderThread(
			FRHICommandListImmediate& CommandList,
			FRHITexture* Target,
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
