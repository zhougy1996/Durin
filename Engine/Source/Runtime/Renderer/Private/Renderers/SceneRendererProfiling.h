#pragma once

#include "RendererAPI.h"
#include "RHIResources.h"
#include "RHI.h"
#include "RHICommandList.h"

namespace Durin
{
	class FRHICommandListImmediate;
	class FRHITexture;
	// Feature-bounded development routes. This type is Renderer-private so
	// production submissions cannot request qualification-only execution.
	struct FRendererQualificationPolicy
	{
		bool bEnableGBuffer = false;
		bool bEnableDeferredDirectional = false;
		bool bEnableGroundTruthAmbientOcclusion = false;
		bool bForceFragmentContactVisibility = false;
		bool bForceFragmentVolumetricCloud = false;
	};

	// Installs one render-thread qualification policy for the lexical duration
	// of a test/tool submission. The fixed executor snapshots the value.
	class RENDERER_API FScopedRendererQualificationPolicy final
	{
	public:
		explicit FScopedRendererQualificationPolicy(
			FRendererQualificationPolicy Policy);
		~FScopedRendererQualificationPolicy();

		FScopedRendererQualificationPolicy(
			const FScopedRendererQualificationPolicy&) = delete;
		auto operator=(const FScopedRendererQualificationPolicy&)
			-> FScopedRendererQualificationPolicy& = delete;

	private:
		FRendererQualificationPolicy Previous;
	};

	template<typename TimingQuerySink>
	class TScopedRendererGPUTimingQuery final
	{
	public:
		TScopedRendererGPUTimingQuery(
			FRHICommandListImmediate& InCommandList, TimingQuerySink InSink)
			: CommandList(InCommandList), Sink(InSink)
		{
			if (Sink == nullptr || GDynamicRHI == nullptr) return;
			Query = GDynamicRHI->RHICreateGPUTimingQuery();
			if (Query) CommandList.BeginGPUTimingQuery(Query);
		}
		~TScopedRendererGPUTimingQuery() { End(); }
		TScopedRendererGPUTimingQuery(
			const TScopedRendererGPUTimingQuery&) = delete;
		auto operator=(const TScopedRendererGPUTimingQuery&)
			-> TScopedRendererGPUTimingQuery& = delete;
		auto End() -> void
		{
			if (Query && !bEnded)
			{
				CommandList.EndGPUTimingQuery(Query);
				bEnded = true;
			}
		}
		auto Commit() -> void
		{
			if (!Query || bCommitted) return;
			End();
			Sink(Query);
			bCommitted = true;
		}

	private:
		FRHICommandListImmediate& CommandList;
		TimingQuerySink Sink;
		FGPUTimingQueryRHIRef Query;
		bool bEnded = false;
		bool bCommitted = false;
	};

	using FSceneColorTimingQuerySink = void (*)(
		const FGPUTimingQueryRHIRef& Query);
	using FPostProcessTimingQuerySink = void (*)(
		const FGPUTimingQueryRHIRef& Query);
	using FGBufferTimingQuerySink = void (*)(
		const FGPUTimingQueryRHIRef& Query);
	using FDeferredDirectionalTimingQuerySink = void (*)(
		const FGPUTimingQueryRHIRef& Query);
	using FRetainedOpaqueTimingQuerySink = void (*)(
		const FGPUTimingQueryRHIRef& Query);
	using FVolumetricCloudTimingQuerySink = void (*)(
		const FGPUTimingQueryRHIRef& Query);
	using FSortedTranslucencyTimingQuerySink = void (*)(
		const FGPUTimingQueryRHIRef& Query);
	using FGroundTruthAmbientOcclusionTimingQuerySink = void (*)(
		const FGPUTimingQueryRHIRef& Query);
	using FGroundTruthAmbientOcclusionFilterTimingQuerySink = void (*)(
		const FGPUTimingQueryRHIRef& Query);
	using FGroundTruthAmbientOcclusionResolveTimingQuerySink = void (*)(
		const FGPUTimingQueryRHIRef& Query);
	using FGroundTruthAmbientOcclusionFeatureTimingQuerySink = void (*)(
		const FGPUTimingQueryRHIRef& Query);
	using FHDRSceneColorCaptureSink = void (*)(
		FRHICommandListImmediate& CommandList,
		FRHITexture* SceneColor,
		FRHITexture* PostProcessInput);
	using FGBufferCaptureSink = void (*)(
		FRHICommandListImmediate& CommandList,
		FRHITexture* Material,
		FRHITexture* Normals,
		FRHITexture* Surface,
		FRHITexture* Emissive,
		FRHITexture* Depth);
	using FDeferredDirectionalCaptureSink = void (*)(
		FRHICommandListImmediate& CommandList,
		FRHITexture* DeferredColor);
	using FGroundTruthAmbientOcclusionCaptureSink = void (*)(
		FRHICommandListImmediate& CommandList,
		FRHITexture* Visibility,
		bool bFiltered);

	// Development seam receiving each explicitly requested Scene Color GPU interval.
	RENDERER_API auto SetSceneColorTimingQuerySink(
		FSceneColorTimingQuerySink Sink) -> void;

	// Development seam receiving the copy or FXAA display-output GPU interval.
	RENDERER_API auto SetPostProcessTimingQuerySink(
		FPostProcessTimingQuerySink Sink) -> void;
	RENDERER_API auto SetGBufferTimingQuerySink(
		FGBufferTimingQuerySink Sink) -> void;
	RENDERER_API auto SetDeferredDirectionalTimingQuerySink(
		FDeferredDirectionalTimingQuerySink Sink) -> void;
	RENDERER_API auto SetRetainedOpaqueTimingQuerySink(
		FRetainedOpaqueTimingQuerySink Sink) -> void;
	RENDERER_API auto SetVolumetricCloudTimingQuerySink(
		FVolumetricCloudTimingQuerySink Sink) -> void;
	RENDERER_API auto SetSortedTranslucencyTimingQuerySink(
		FSortedTranslucencyTimingQuerySink Sink) -> void;
	RENDERER_API auto SetGroundTruthAmbientOcclusionTimingQuerySink(
		FGroundTruthAmbientOcclusionTimingQuerySink Sink) -> void;
	RENDERER_API auto SetGroundTruthAmbientOcclusionFilterTimingQuerySink(
		FGroundTruthAmbientOcclusionFilterTimingQuerySink Sink) -> void;
	RENDERER_API auto SetGroundTruthAmbientOcclusionResolveTimingQuerySink(
		FGroundTruthAmbientOcclusionResolveTimingQuerySink Sink) -> void;
	RENDERER_API auto SetGroundTruthAmbientOcclusionFeatureTimingQuerySink(
		FGroundTruthAmbientOcclusionFeatureTimingQuerySink Sink) -> void;

	// Development seam receiving scene-linear color after optional contact
	// composition and before display mapping.
	RENDERER_API auto SetHDRSceneColorCaptureSink(
		FHDRSceneColorCaptureSink Sink) -> void;

	// Development seam receiving the completed qualification geometry buffers.
	RENDERER_API auto SetGBufferCaptureSink(FGBufferCaptureSink Sink) -> void;
	RENDERER_API auto SetDeferredDirectionalCaptureSink(
		FDeferredDirectionalCaptureSink Sink) -> void;
	RENDERER_API auto SetGroundTruthAmbientOcclusionCaptureSink(
		FGroundTruthAmbientOcclusionCaptureSink Sink) -> void;

	// Renderer-private immutable snapshots of the current observer registrations.
	RENDERER_API auto GetRendererQualificationPolicy()
		-> FRendererQualificationPolicy;
	auto GetSceneColorTimingQuerySink() -> FSceneColorTimingQuerySink;
	auto GetPostProcessTimingQuerySink() -> FPostProcessTimingQuerySink;
	auto GetGBufferTimingQuerySink() -> FGBufferTimingQuerySink;
	auto GetDeferredDirectionalTimingQuerySink()
		-> FDeferredDirectionalTimingQuerySink;
	auto GetRetainedOpaqueTimingQuerySink() -> FRetainedOpaqueTimingQuerySink;
	auto GetVolumetricCloudTimingQuerySink() -> FVolumetricCloudTimingQuerySink;
	auto GetSortedTranslucencyTimingQuerySink()
		-> FSortedTranslucencyTimingQuerySink;
	auto GetGroundTruthAmbientOcclusionTimingQuerySink()
		-> FGroundTruthAmbientOcclusionTimingQuerySink;
	auto GetGroundTruthAmbientOcclusionFilterTimingQuerySink()
		-> FGroundTruthAmbientOcclusionFilterTimingQuerySink;
	auto GetGroundTruthAmbientOcclusionResolveTimingQuerySink()
		-> FGroundTruthAmbientOcclusionResolveTimingQuerySink;
	auto GetGroundTruthAmbientOcclusionFeatureTimingQuerySink()
		-> FGroundTruthAmbientOcclusionFeatureTimingQuerySink;
	auto GetHDRSceneColorCaptureSink() -> FHDRSceneColorCaptureSink;
	auto GetGBufferCaptureSink() -> FGBufferCaptureSink;
	auto GetDeferredDirectionalCaptureSink()
		-> FDeferredDirectionalCaptureSink;
	auto GetGroundTruthAmbientOcclusionCaptureSink()
		-> FGroundTruthAmbientOcclusionCaptureSink;
}
