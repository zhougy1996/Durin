#include "Renderers/SceneVisibility.h"

#include "Renderers/ViewPreparationMath.h"

#include "Rendering/PrimitiveSceneProxy.h"
#include "Scene.h"
#include "SceneInfo.h"

namespace Durin
{
	auto PrepareSceneVisibility(
		const FScene& Scene,
		const FSceneView& View,
		FViewRenderTelemetry& Telemetry) -> FSceneVisibilityResult
	{
		Telemetry.Visibility = {};
		FSceneVisibilityResult Result;
		const auto& SceneInfos = Scene.GetPrimitiveSceneInfos();
		Result.PrimitiveRecords.reserve(SceneInfos.size());
		Result.StaticMeshSceneInfos.reserve(SceneInfos.size());
		Result.SkeletalMeshSceneInfos.reserve(SceneInfos.size());
		Result.TerrainSceneInfos.reserve(SceneInfos.size());
		Result.SplineMeshSceneInfos.reserve(SceneInfos.size());

		FViewFrustum Frustum;
		const bool bCullingEnabled =
			View.Settings.Mode.VisibilityMode == EViewVisibilityMode::Normal;
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
			++Telemetry.Visibility.SubmittedPrimitives;

			EPrimitiveVisibilityClassification Classification =
				EPrimitiveVisibilityClassification::Invalid;
			bool bVisible = false;
			if (!SceneInfo->IsVisible())
			{
				Classification = EPrimitiveVisibilityClassification::Hidden;
				++Telemetry.Visibility.HiddenPrimitives;
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
				++Telemetry.Visibility.InvalidViewFallbacks;
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
					++Telemetry.Visibility.FrustumCulledPrimitives;
					break;
				case EViewBoundsClassification::InvalidBounds:
					Classification = EPrimitiveVisibilityClassification::
						VisibleInvalidBoundsFallback;
					bVisible = true;
					++Telemetry.Visibility.InvalidBoundsFallbacks;
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
			++Telemetry.Visibility.VisiblePrimitives;
			switch (SceneInfo->GetKind())
			{
			case EPrimitiveSceneProxyKind::StaticMesh:
				Result.StaticMeshSceneInfos.push_back(SceneInfo);
				break;
			case EPrimitiveSceneProxyKind::SkeletalMesh:
				Result.SkeletalMeshSceneInfos.push_back(SceneInfo);
				++Telemetry.SkeletalMesh.VisibleSkeletalMeshCandidates;
				break;
			case EPrimitiveSceneProxyKind::Terrain:
				Result.TerrainSceneInfos.push_back(SceneInfo);
				++Telemetry.Terrain.VisibleTerrainCandidates;
				break;
			case EPrimitiveSceneProxyKind::SplineMesh:
				Result.SplineMeshSceneInfos.push_back(SceneInfo);
				++Telemetry.SplineMesh.VisibleSplineMeshCandidates;
				break;
			}
		}

		const bool bCountersConserved = Telemetry.Visibility.SubmittedPrimitives
			== Telemetry.Visibility.HiddenPrimitives + Telemetry.Visibility.FrustumCulledPrimitives
				+ Telemetry.Visibility.VisiblePrimitives;
		check(bCountersConserved);
		const bool bRecordCountMatches =
			Result.PrimitiveRecords.size() == Telemetry.Visibility.SubmittedPrimitives;
		check(bRecordCountMatches);
		return Result;
	}
} // namespace Durin
