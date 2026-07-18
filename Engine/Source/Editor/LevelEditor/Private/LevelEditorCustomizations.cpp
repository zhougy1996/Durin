#include "LevelEditorCustomizations.h"

#include "DObject/Class.h"
#include "SceneViewProjection.h"

namespace Durin
{
	namespace
	{
		auto DistanceToSegment(const FVector2f& Point, const FVector2f& A, const FVector2f& B, float& OutT) -> float
		{
			const FVector2f Segment = B - A;
			const float LengthSquared = glm::dot(Segment, Segment);
			OutT = LengthSquared > 1.e-6f ? std::clamp(glm::dot(Point - A, Segment) / LengthSquared, 0.0f, 1.0f) : 0.0f;
			return glm::length(Point - (A + Segment * OutT));
		}

		template<typename T, typename TMap>
		auto FindMostSpecific(const DClass* Class, const TMap& Entries) -> std::shared_ptr<T>
		{
			for (const DClass* Current = Class; Current != nullptr; Current = Current->GetSuperClass())
			{
				if (const auto It = Entries.find(const_cast<DClass*>(Current)); It != Entries.end()) return It->second.Customization;
			}
			return nullptr;
		}
	} // namespace

	auto FEditorVisualizationCollector::AddLine(const FEditorVisualizationLine& Line) -> void
	{
		if (!Line.Actor || !Line.Component || !std::isfinite(Line.WidthPixels) || !std::isfinite(Line.HitTolerancePixels)) return;
		if (glm::length(Line.End - Line.Start) <= kSmallNumber) return;
		Lines.push_back(Line);
	}

	auto FEditorVisualizationCollector::AppendToView(FSceneView& View) const -> void
	{
		View.OverlayLines.reserve(View.OverlayLines.size() + Lines.size());
		for (const FEditorVisualizationLine& Line : Lines)
			View.OverlayLines.push_back({Line.Start, Line.End, Line.Color, Line.WidthPixels});
	}

	auto FEditorVisualizationCollector::HitTest(const FSceneView& View, const FVector2f& ViewportPosition) const -> FEditorVisualizationHit
	{
		FEditorVisualizationHit Best;
		FVector3 RayOrigin, RayDirection;
		if (!SceneViewProjection::BuildViewportRay(View, ViewportPosition, RayOrigin, RayDirection)) return Best;
		(void)RayDirection;
		for (const FEditorVisualizationLine& Line : Lines)
		{
			FVector2f StartScreen, EndScreen;
			if (!SceneViewProjection::ProjectWorldToViewport(View, Line.Start, StartScreen) || !SceneViewProjection::ProjectWorldToViewport(View, Line.End, EndScreen)) continue;
			float T = 0.0f;
			if (DistanceToSegment(ViewportPosition, StartScreen, EndScreen, T) > std::max(Line.HitTolerancePixels, Line.WidthPixels * 0.5f)) continue;
			const FVector3 HitLocation = glm::mix(Line.Start, Line.End, static_cast<double>(T));
			const double Distance = glm::length(HitLocation - RayOrigin);
			if (!std::isfinite(Distance)) continue;
			if (Distance < Best.Distance - 1.e-6 || (std::abs(Distance - Best.Distance) <= 1.e-6 && Line.HitPriority > Best.Priority))
				Best = {Line.Actor, Line.Component, Distance, Line.HitPriority};
		}
		return Best;
	}

	auto FLevelEditorCustomizationRegistry::Get() -> FLevelEditorCustomizationRegistry&
	{
		static FLevelEditorCustomizationRegistry Registry;
		return Registry;
	}

	auto FLevelEditorCustomizationRegistry::RegisterComponentVisualizer(DClass* Class, std::shared_ptr<IComponentEditorVisualizer> Visualizer) -> FLevelEditorCustomizationHandle
	{
		if (!Class || !Visualizer || ComponentVisualizers.contains(Class)) return {};
		const uint64 Id = NextHandleId++;
		ComponentVisualizers.emplace(Class, TEntry<IComponentEditorVisualizer>{Id, std::move(Visualizer)});
		return {Id, ELevelEditorCustomizationKind::ComponentVisualizer};
	}

	auto FLevelEditorCustomizationRegistry::RegisterObjectDetails(DClass* Class, std::shared_ptr<IObjectDetailsCustomization> Customization) -> FLevelEditorCustomizationHandle
	{
		if (!Class || !Customization || ObjectDetails.contains(Class)) return {};
		const uint64 Id = NextHandleId++;
		ObjectDetails.emplace(Class, TEntry<IObjectDetailsCustomization>{Id, std::move(Customization)});
		return {Id, ELevelEditorCustomizationKind::ObjectDetails};
	}

	auto FLevelEditorCustomizationRegistry::Unregister(FLevelEditorCustomizationHandle Handle) -> bool
	{
		if (!Handle) return false;
		if (Handle.Kind == ELevelEditorCustomizationKind::ComponentVisualizer)
		{
			const auto It = std::ranges::find_if(ComponentVisualizers, [Handle](const auto& Pair) { return Pair.second.HandleId == Handle.Id; });
			if (It == ComponentVisualizers.end()) return false;
			ComponentVisualizers.erase(It);
			return true;
		}
		const auto It = std::ranges::find_if(ObjectDetails, [Handle](const auto& Pair) { return Pair.second.HandleId == Handle.Id; });
		if (It == ObjectDetails.end()) return false;
		ObjectDetails.erase(It);
		return true;
	}

	auto FLevelEditorCustomizationRegistry::FindComponentVisualizer(const DClass* Class) const -> std::shared_ptr<IComponentEditorVisualizer>
	{
		return FindMostSpecific<IComponentEditorVisualizer>(Class, ComponentVisualizers);
	}

	auto FLevelEditorCustomizationRegistry::FindObjectDetails(const DClass* Class) const -> std::shared_ptr<IObjectDetailsCustomization>
	{
		return FindMostSpecific<IObjectDetailsCustomization>(Class, ObjectDetails);
	}
} // namespace Durin
