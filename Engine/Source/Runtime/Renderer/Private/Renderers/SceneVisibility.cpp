#include "Renderers/SceneVisibility.h"
#include "Renderers/RenderPreparationTasks.h"

#include "Renderers/ViewPreparationMath.h"

#include "Rendering/PrimitiveSceneProxy.h"
#include "Scene.h"
#include "SceneInfo.h"
#include "Profiling/Profiling.h"

namespace Durin
{
	auto ResolveMainViewCullingPolicy(const FSceneView& View) -> FMainViewCullingPolicy
	{
		FMainViewCullingPolicy Policy;
		Policy.bEnabled = View.Settings.Mode.VisibilityMode == EViewVisibilityMode::Normal;
		FViewFrustum Frustum;
		if (Policy.bEnabled && TryBuildViewFrustum(View, Frustum))
			Policy.Frustum = Frustum;
		return Policy;
	}

	static auto ClassifySceneVisibility(std::vector<FPrimitiveVisibilityInput> Inputs, const FMainViewCullingPolicy& Policy,
		bool bAllowTasks) -> FSceneVisibilityClassification
	{
		using EClass = EPrimitiveVisibilityClassification;
		FSceneVisibilityClassification Result;
		Result.Primitives.resize(Inputs.size());
		const auto Classify = [&](size_t Index) {
			const auto& Input = Inputs[Index];
			EClass Classification;
			if (!Input.bVisible) Classification = EClass::Hidden;
			else if (!Policy.bEnabled) Classification = EClass::VisibleCullingDisabled;
			else if (!Policy.Frustum) Classification = EClass::VisibleInvalidViewFallback;
			else
			{
				switch (ClassifyWorldBounds(*Policy.Frustum, Input.WorldBounds))
				{
				case EViewBoundsClassification::Inside: Classification = EClass::VisibleInside; break;
				case EViewBoundsClassification::Intersecting: Classification = EClass::VisibleIntersecting; break;
				case EViewBoundsClassification::Outside: Classification = EClass::FrustumCulled; break;
				case EViewBoundsClassification::InvalidBounds: Classification = EClass::VisibleInvalidBoundsFallback; break;
				default: Classification = EClass::VisibleInvalidBoundsFallback; break;
				}
			}
			Result.Primitives[Index] = Classification;
		};
		Result.TaskCount = RendererPrivate::RunPreparationRanges("Renderer.ClassifyVisibility", Inputs.size(), 512,
			bAllowTasks && Inputs.size() >= 1024, Classify);
		return Result;
	}

	auto ClassifySceneVisibility(std::vector<FPrimitiveVisibilityInput> Inputs,
		const FSceneView& View, bool bAllowTasks) -> FSceneVisibilityClassification
	{
		return ClassifySceneVisibility(std::move(Inputs), ResolveMainViewCullingPolicy(View), bAllowTasks);
	}

	auto PrepareSceneVisibility(
		const FScene& Scene,
		const FMainViewCullingPolicy& Policy,
		FViewRenderTelemetry& Telemetry,
		FSceneVisibilityResult& Result,
		bool bCollectPrimitiveRecords) -> void
	{
		DURIN_PROFILE_CPU_ZONE_NAMED("Renderer.PrepareVisibility");
		Telemetry.Visibility = {};
		Result.PrimitiveRecords.clear();
		Result.SceneInfos.clear();
		const auto& SceneInfos = Scene.GetPrimitiveSceneInfos();
		if (bCollectPrimitiveRecords)
			Result.PrimitiveRecords.reserve(SceneInfos.size());

		std::vector<FPrimitiveVisibilityInput> Inputs;
		Inputs.reserve(SceneInfos.size());
		for (const auto* Info : SceneInfos)
			Inputs.push_back({Info ? Info->GetWorldBounds() : FBox{}, Info && Info->IsVisible()});
		const auto Classified = ClassifySceneVisibility(std::move(Inputs), Policy, true);
		for (size_t Index = 0; Index < SceneInfos.size(); ++Index)
		{
			const auto* SceneInfo = SceneInfos[Index];
			check(SceneInfo != nullptr);
			if (!SceneInfo) continue;
			++Telemetry.Visibility.SubmittedPrimitives;
			const auto Classification = Classified.Primitives[Index];
			const bool bVisible = Classification != EPrimitiveVisibilityClassification::Hidden
				&& Classification != EPrimitiveVisibilityClassification::FrustumCulled;
			Telemetry.Visibility.HiddenPrimitives += Classification == EPrimitiveVisibilityClassification::Hidden;
			Telemetry.Visibility.FrustumCulledPrimitives += Classification == EPrimitiveVisibilityClassification::FrustumCulled;
			Telemetry.Visibility.InvalidViewFallbacks += Classification == EPrimitiveVisibilityClassification::VisibleInvalidViewFallback;
			Telemetry.Visibility.InvalidBoundsFallbacks += Classification == EPrimitiveVisibilityClassification::VisibleInvalidBoundsFallback;
			const bool bClassificationValid = Classification
				!= EPrimitiveVisibilityClassification::Invalid;
			checkf(bClassificationValid,
				"Every submitted primitive requires a known visibility classification.");
			if (bCollectPrimitiveRecords)
				Result.PrimitiveRecords.push_back({SceneInfo, Classification});
			if (!bVisible)
			{
				continue;
			}
			++Telemetry.Visibility.VisiblePrimitives;
			Result.SceneInfos.push_back(SceneInfo);
			if (SceneInfo->GetKind() == EPrimitiveSceneProxyKind::SplineMesh)
				++Telemetry.SplineMesh.VisibleSplineMeshCandidates;
		}

		const bool bCountersConserved = Telemetry.Visibility.SubmittedPrimitives
			== Telemetry.Visibility.HiddenPrimitives + Telemetry.Visibility.FrustumCulledPrimitives
				+ Telemetry.Visibility.VisiblePrimitives;
		check(bCountersConserved);
		const bool bRecordCountMatches =
			!bCollectPrimitiveRecords
			|| Result.PrimitiveRecords.size() == Telemetry.Visibility.SubmittedPrimitives;
		check(bRecordCountMatches);
	}

	auto PrepareSceneVisibility(
		const FScene& Scene,
		const FSceneView& View,
		FViewRenderTelemetry& Telemetry,
		FSceneVisibilityResult& Result,
		bool bCollectPrimitiveRecords) -> void
	{
		PrepareSceneVisibility(Scene, ResolveMainViewCullingPolicy(View), Telemetry, Result, bCollectPrimitiveRecords);
	}

	auto PrepareSceneVisibility(
		const FScene& Scene,
		const FSceneView& View,
		FViewRenderTelemetry& Telemetry) -> FSceneVisibilityResult
	{
		FSceneVisibilityResult Result;
		PrepareSceneVisibility(Scene, View, Telemetry, Result);
		return Result;
	}
} // namespace Durin
