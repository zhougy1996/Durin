#include "Renderers/SceneVisibility.h"

#include "Renderers/ViewPreparationMath.h"

#include "Rendering/PrimitiveSceneProxy.h"
#include "Scene.h"
#include "SceneInfo.h"
#include "Profiling/Profiling.h"
#include "Threading/TaskComposition.h"
#include <deque>

namespace Durin
{
	auto ClassifySceneVisibility(std::vector<FPrimitiveVisibilityInput> Inputs,
		const FSceneView& View, bool bAllowTasks) -> FSceneVisibilityClassification
	{
		using EClass = EPrimitiveVisibilityClassification;
		FViewFrustum Frustum{};
		const bool bCulling = View.Settings.Mode.VisibilityMode == EViewVisibilityMode::Normal;
		const bool bValid = !bCulling || TryBuildViewFrustum(View, Frustum);
		auto Owner = std::make_shared<const std::vector<FPrimitiveVisibilityInput>>(std::move(Inputs));
		auto ClassifyRange = [Owner, Frustum, bCulling, bValid](size_t First, size_t Count) {
			std::vector<EClass> Classes;
			Classes.reserve(Count);
			for (size_t Index = First; Index < First + Count; ++Index)
			{
				const auto& Input = (*Owner)[Index];
				EClass Classification;
				if (!Input.bVisible) Classification = EClass::Hidden;
				else if (!bCulling) Classification = EClass::VisibleCullingDisabled;
				else if (!bValid) Classification = EClass::VisibleInvalidViewFallback;
				else
				{
					switch (ClassifyWorldBounds(Frustum, Input.WorldBounds))
					{
					case EViewBoundsClassification::Inside: Classification = EClass::VisibleInside; break;
					case EViewBoundsClassification::Intersecting: Classification = EClass::VisibleIntersecting; break;
					case EViewBoundsClassification::Outside: Classification = EClass::FrustumCulled; break;
					case EViewBoundsClassification::InvalidBounds: Classification = EClass::VisibleInvalidBoundsFallback; break;
					default: Classification = EClass::VisibleInvalidBoundsFallback; break;
					}
				}
				Classes.push_back(Classification);
			}
			return Classes;
		};
		if (!bAllowTasks || Owner->size() < 1024 || !IsTaskSchedulerRunning())
			return {ClassifyRange(0, Owner->size()), 0};
		struct FPending
		{
			std::deque<Tasks::TTask<std::vector<EClass>>> Tasks;
			~FPending()
			{
				for (auto& Task : Tasks) Durin::Tasks::Cancel(Task.GetCompletion());
				for (auto& Task : Tasks) require(Task.Wait().WaitStatus == ETaskWaitStatus::Completed);
			}
		} Pending;
		FSceneVisibilityClassification Result;
		Result.Primitives.reserve(Owner->size());
		size_t Next = 0;
		auto Launch = [&] {
			const size_t First = Next, Count = std::min(size_t(512), Owner->size() - Next);
			Pending.Tasks.push_back(Tasks::LaunchIndependentTask("Renderer.ClassifyVisibility",
				[ClassifyRange, First, Count] { return ClassifyRange(First, Count); }));
			Next += Count;
			++Result.TaskCount;
		};
		while (Next < Owner->size() && Pending.Tasks.size() < 8) Launch();
		while (!Pending.Tasks.empty())
		{
			auto Task = std::move(Pending.Tasks.front());
			Pending.Tasks.pop_front();
			const auto Wait = Task.Wait();
			require(Wait.WaitStatus == ETaskWaitStatus::Completed);
			// No externally visible effect has occurred: retry the pure classification
			// inline. Scope exit drains outstanding work before publishing the result.
			if (Wait.TaskState != ETaskState::Succeeded)
				return {ClassifyRange(0, Owner->size()), Result.TaskCount};
			auto Classes = std::move(Task).TakeResult();
			Result.Primitives.insert(Result.Primitives.end(), Classes.begin(), Classes.end());
			if (Next < Owner->size()) Launch();
		}
		return Result;
	}

	auto PrepareSceneVisibility(
		const FScene& Scene,
		const FSceneView& View,
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
		const auto Classified = ClassifySceneVisibility(std::move(Inputs), View);
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
		FViewRenderTelemetry& Telemetry) -> FSceneVisibilityResult
	{
		FSceneVisibilityResult Result;
		PrepareSceneVisibility(Scene, View, Telemetry, Result);
		return Result;
	}
} // namespace Durin
