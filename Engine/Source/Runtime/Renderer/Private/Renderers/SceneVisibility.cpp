#include "Renderers/SceneVisibility.h"

#include "Renderers/ViewPreparationMath.h"

#include "Engine/FPrimitiveSceneProxy.h"
#include "Scene.h"

#include <atomic>
#include <unordered_set>

namespace Durin
{
	namespace
	{
		std::atomic<FViewRenderCounterSink> GViewRenderCounterSink = nullptr;
	}

	auto SetViewRenderCounterSink(FViewRenderCounterSink Sink) -> void
	{
		GViewRenderCounterSink.store(Sink, std::memory_order_release);
	}

	auto EmitViewRenderCounterSnapshot(
		const FViewRenderCounters& Counters) -> void
	{
		if (const FViewRenderCounterSink Sink =
			GViewRenderCounterSink.load(std::memory_order_acquire))
		{
			Sink(Counters);
		}
	}

	auto PrepareSceneVisibility(
		const FScene& Scene,
		const FSceneView& View,
		FViewRenderCounters& Counters) -> FSceneVisibilityResult
	{
		Counters = {};
		FSceneVisibilityResult Result;
		const auto& SceneInfos = Scene.GetPrimitiveSceneInfos();
		Result.PrimitiveRecords.reserve(SceneInfos.size());
		Result.StaticMeshSceneInfos.reserve(SceneInfos.size());
		Result.SkeletalMeshSceneInfos.reserve(SceneInfos.size());
		Result.TerrainSceneInfos.reserve(SceneInfos.size());
		Result.SplineMeshSceneInfos.reserve(SceneInfos.size());

		FViewFrustum Frustum;
		const bool bCullingEnabled =
			View.Settings.VisibilityMode == EViewVisibilityMode::Normal;
		const bool bValidView =
			!bCullingEnabled || TryBuildViewFrustum(View, Frustum);
		std::unordered_set<FPrimitiveSceneId, FSceneIdHash> ClassifiedIds;

		for (const FPrimitiveSceneInfo* SceneInfo : SceneInfos)
		{
			check(SceneInfo != nullptr);
			if (SceneInfo == nullptr)
			{
				continue;
			}
			const bool bFirstClassification =
				ClassifiedIds.emplace(SceneInfo->GetId()).second;
			check(bFirstClassification);
			++Counters.SubmittedPrimitives;

			EPrimitiveVisibilityClassification Classification =
				EPrimitiveVisibilityClassification::Invalid;
			bool bVisible = false;
			if (!SceneInfo->IsVisible())
			{
				Classification = EPrimitiveVisibilityClassification::Hidden;
				++Counters.HiddenPrimitives;
			}
			else if (!bCullingEnabled)
			{
				Classification =
					EPrimitiveVisibilityClassification::VisibleCullingDisabled;
				bVisible = true;
			}
			else if (!bValidView)
			{
				Classification =
					EPrimitiveVisibilityClassification::VisibleInvalidViewFallback;
				bVisible = true;
				++Counters.InvalidViewFallbacks;
			}
			else
			{
				switch (ClassifyWorldBounds(Frustum, SceneInfo->GetWorldBounds()))
				{
				case EViewBoundsClassification::Inside:
					Classification =
						EPrimitiveVisibilityClassification::VisibleInside;
					bVisible = true;
					break;
				case EViewBoundsClassification::Intersecting:
					Classification =
						EPrimitiveVisibilityClassification::VisibleIntersecting;
					bVisible = true;
					break;
				case EViewBoundsClassification::Outside:
					Classification =
						EPrimitiveVisibilityClassification::FrustumCulled;
					++Counters.FrustumCulledPrimitives;
					break;
				case EViewBoundsClassification::InvalidBounds:
					Classification = EPrimitiveVisibilityClassification::
						VisibleInvalidBoundsFallback;
					bVisible = true;
					++Counters.InvalidBoundsFallbacks;
					break;
				}
			}

			const bool bClassificationValid = Classification
				!= EPrimitiveVisibilityClassification::Invalid;
			checkf(bClassificationValid,
				"Every submitted primitive requires a known visibility classification.");
			Result.PrimitiveRecords.push_back({SceneInfo, Classification});
			if (!bVisible)
			{
				continue;
			}
			++Counters.VisiblePrimitives;
			switch (SceneInfo->GetKind())
			{
			case EPrimitiveSceneProxyKind::StaticMesh:
				Result.StaticMeshSceneInfos.push_back(SceneInfo);
				break;
			case EPrimitiveSceneProxyKind::SkeletalMesh:
				Result.SkeletalMeshSceneInfos.push_back(SceneInfo);
				++Counters.VisibleSkeletalMeshCandidates;
				break;
			case EPrimitiveSceneProxyKind::Terrain:
				Result.TerrainSceneInfos.push_back(SceneInfo);
				++Counters.VisibleTerrainCandidates;
				break;
			case EPrimitiveSceneProxyKind::SplineMesh:
				Result.SplineMeshSceneInfos.push_back(SceneInfo);
				++Counters.VisibleSplineMeshCandidates;
				break;
			}
		}

		const bool bCountersConserved = Counters.SubmittedPrimitives
			== Counters.HiddenPrimitives + Counters.FrustumCulledPrimitives
				+ Counters.VisiblePrimitives;
		check(bCountersConserved);
		const bool bRecordCountMatches =
			Result.PrimitiveRecords.size() == Counters.SubmittedPrimitives;
		check(bRecordCountMatches);
		return Result;
	}
} // namespace Durin
