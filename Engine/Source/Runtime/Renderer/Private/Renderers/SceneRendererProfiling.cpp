#include "Renderers/SceneRendererProfiling.h"

namespace Durin
{
	namespace
	{
		std::atomic<FSceneColorTimingQuerySink> GSceneColorTimingQuerySink = nullptr;
		std::atomic<FPostProcessTimingQuerySink> GPostProcessTimingQuerySink = nullptr;
		std::atomic<FGBufferTimingQuerySink> GGBufferTimingQuerySink = nullptr;
		std::atomic<FDeferredDirectionalTimingQuerySink> GDeferredDirectionalTimingQuerySink = nullptr;
		std::atomic<FRetainedOpaqueTimingQuerySink> GRetainedOpaqueTimingQuerySink = nullptr;
		std::atomic<FVolumetricCloudTimingQuerySink> GVolumetricCloudTimingQuerySink = nullptr;
		std::atomic<FSortedTranslucencyTimingQuerySink> GSortedTranslucencyTimingQuerySink = nullptr;
		std::atomic<FGroundTruthAmbientOcclusionTimingQuerySink> GGroundTruthAmbientOcclusionTimingQuerySink = nullptr;
		std::atomic<FGroundTruthAmbientOcclusionFilterTimingQuerySink> GGroundTruthAmbientOcclusionFilterTimingQuerySink = nullptr;
		std::atomic<FGroundTruthAmbientOcclusionResolveTimingQuerySink> GGroundTruthAmbientOcclusionResolveTimingQuerySink = nullptr;
		std::atomic<FGroundTruthAmbientOcclusionFeatureTimingQuerySink> GGroundTruthAmbientOcclusionFeatureTimingQuerySink = nullptr;
		std::atomic<FHDRSceneColorCaptureSink> GHDRSceneColorCaptureSink = nullptr;
		std::atomic<FGBufferCaptureSink> GGBufferCaptureSink = nullptr;
		std::atomic<FDeferredDirectionalCaptureSink> GDeferredDirectionalCaptureSink = nullptr;
		std::atomic<FGroundTruthAmbientOcclusionCaptureSink> GGroundTruthAmbientOcclusionCaptureSink = nullptr;
		std::atomic<FVolumetricCloudPreparationSink> GVolumetricCloudPreparationSink = nullptr;
	}
	auto SetSceneColorTimingQuerySink(FSceneColorTimingQuerySink Sink) -> void
	{
		GSceneColorTimingQuerySink.store(Sink, std::memory_order_release);
	}

	auto SetPostProcessTimingQuerySink(FPostProcessTimingQuerySink Sink) -> void
	{
		GPostProcessTimingQuerySink.store(Sink, std::memory_order_release);
	}

	auto SetGBufferTimingQuerySink(FGBufferTimingQuerySink Sink) -> void
	{
		GGBufferTimingQuerySink.store(Sink, std::memory_order_release);
	}

	auto SetDeferredDirectionalTimingQuerySink(
		FDeferredDirectionalTimingQuerySink Sink
	) -> void
	{
		GDeferredDirectionalTimingQuerySink.store(
			Sink, std::memory_order_release
		);
	}

	auto SetRetainedOpaqueTimingQuerySink(
		FRetainedOpaqueTimingQuerySink Sink
	) -> void
	{
		GRetainedOpaqueTimingQuerySink.store(
			Sink, std::memory_order_release
		);
	}

	auto SetVolumetricCloudTimingQuerySink(
		FVolumetricCloudTimingQuerySink Sink
	) -> void
	{
		GVolumetricCloudTimingQuerySink.store(Sink, std::memory_order_release);
	}

	auto SetSortedTranslucencyTimingQuerySink(
		FSortedTranslucencyTimingQuerySink Sink
	) -> void
	{
		GSortedTranslucencyTimingQuerySink.store(
			Sink, std::memory_order_release
		);
	}

	auto SetGroundTruthAmbientOcclusionTimingQuerySink(
		FGroundTruthAmbientOcclusionTimingQuerySink Sink
	) -> void
	{
		GGroundTruthAmbientOcclusionTimingQuerySink.store(
			Sink, std::memory_order_release
		);
	}

	auto SetGroundTruthAmbientOcclusionFilterTimingQuerySink(
		FGroundTruthAmbientOcclusionFilterTimingQuerySink Sink
	) -> void
	{
		GGroundTruthAmbientOcclusionFilterTimingQuerySink.store(
			Sink, std::memory_order_release
		);
	}

	auto SetGroundTruthAmbientOcclusionResolveTimingQuerySink(
		FGroundTruthAmbientOcclusionResolveTimingQuerySink Sink
	) -> void
	{
		GGroundTruthAmbientOcclusionResolveTimingQuerySink.store(
			Sink, std::memory_order_release
		);
	}

	auto SetGroundTruthAmbientOcclusionFeatureTimingQuerySink(
		FGroundTruthAmbientOcclusionFeatureTimingQuerySink Sink
	) -> void
	{
		GGroundTruthAmbientOcclusionFeatureTimingQuerySink.store(
			Sink, std::memory_order_release
		);
	}

	auto SetHDRSceneColorCaptureSink(FHDRSceneColorCaptureSink Sink) -> void
	{
		GHDRSceneColorCaptureSink.store(Sink, std::memory_order_release);
	}

	auto SetGBufferCaptureSink(FGBufferCaptureSink Sink) -> void
	{
		GGBufferCaptureSink.store(Sink, std::memory_order_release);
	}

	auto SetDeferredDirectionalCaptureSink(
		FDeferredDirectionalCaptureSink Sink
	) -> void
	{
		GDeferredDirectionalCaptureSink.store(
			Sink, std::memory_order_release
		);
	}

	auto SetGroundTruthAmbientOcclusionCaptureSink(
		FGroundTruthAmbientOcclusionCaptureSink Sink
	) -> void
	{
		GGroundTruthAmbientOcclusionCaptureSink.store(
			Sink, std::memory_order_release
		);
	}

	auto SetVolumetricCloudPreparationSink(
		FVolumetricCloudPreparationSink Sink
	) -> void
	{
		GVolumetricCloudPreparationSink.store(Sink, std::memory_order_release);
	}

#define DURIN_DEFINE_SINK_GETTER(Name, Type, Storage) \
	auto Name() -> Type { return Storage.load(std::memory_order_acquire); }
	DURIN_DEFINE_SINK_GETTER(GetSceneColorTimingQuerySink,
		FSceneColorTimingQuerySink, GSceneColorTimingQuerySink)
	DURIN_DEFINE_SINK_GETTER(GetPostProcessTimingQuerySink,
		FPostProcessTimingQuerySink, GPostProcessTimingQuerySink)
	DURIN_DEFINE_SINK_GETTER(GetGBufferTimingQuerySink,
		FGBufferTimingQuerySink, GGBufferTimingQuerySink)
	DURIN_DEFINE_SINK_GETTER(GetDeferredDirectionalTimingQuerySink,
		FDeferredDirectionalTimingQuerySink, GDeferredDirectionalTimingQuerySink)
	DURIN_DEFINE_SINK_GETTER(GetRetainedOpaqueTimingQuerySink,
		FRetainedOpaqueTimingQuerySink, GRetainedOpaqueTimingQuerySink)
	DURIN_DEFINE_SINK_GETTER(GetVolumetricCloudTimingQuerySink,
		FVolumetricCloudTimingQuerySink, GVolumetricCloudTimingQuerySink)
	DURIN_DEFINE_SINK_GETTER(GetSortedTranslucencyTimingQuerySink,
		FSortedTranslucencyTimingQuerySink, GSortedTranslucencyTimingQuerySink)
	DURIN_DEFINE_SINK_GETTER(GetGroundTruthAmbientOcclusionTimingQuerySink,
		FGroundTruthAmbientOcclusionTimingQuerySink,
		GGroundTruthAmbientOcclusionTimingQuerySink)
	DURIN_DEFINE_SINK_GETTER(GetGroundTruthAmbientOcclusionFilterTimingQuerySink,
		FGroundTruthAmbientOcclusionFilterTimingQuerySink,
		GGroundTruthAmbientOcclusionFilterTimingQuerySink)
	DURIN_DEFINE_SINK_GETTER(GetGroundTruthAmbientOcclusionResolveTimingQuerySink,
		FGroundTruthAmbientOcclusionResolveTimingQuerySink,
		GGroundTruthAmbientOcclusionResolveTimingQuerySink)
	DURIN_DEFINE_SINK_GETTER(GetGroundTruthAmbientOcclusionFeatureTimingQuerySink,
		FGroundTruthAmbientOcclusionFeatureTimingQuerySink,
		GGroundTruthAmbientOcclusionFeatureTimingQuerySink)
	DURIN_DEFINE_SINK_GETTER(GetHDRSceneColorCaptureSink,
		FHDRSceneColorCaptureSink, GHDRSceneColorCaptureSink)
	DURIN_DEFINE_SINK_GETTER(GetGBufferCaptureSink,
		FGBufferCaptureSink, GGBufferCaptureSink)
	DURIN_DEFINE_SINK_GETTER(GetDeferredDirectionalCaptureSink,
		FDeferredDirectionalCaptureSink, GDeferredDirectionalCaptureSink)
	DURIN_DEFINE_SINK_GETTER(GetGroundTruthAmbientOcclusionCaptureSink,
		FGroundTruthAmbientOcclusionCaptureSink,
		GGroundTruthAmbientOcclusionCaptureSink)
	DURIN_DEFINE_SINK_GETTER(GetVolumetricCloudPreparationSink,
		FVolumetricCloudPreparationSink, GVolumetricCloudPreparationSink)
#undef DURIN_DEFINE_SINK_GETTER

} // namespace Durin
