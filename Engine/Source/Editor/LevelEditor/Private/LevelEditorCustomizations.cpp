#include "LevelEditorCustomizations.h"

#include "DObject/Class.h"
#include "DObject/Property.h"
#include "Editor/ReflectedPropertyView.h"
#include "Misc/StringHelper.h"
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

	FObjectPropertyViewBuilder::FObjectPropertyViewBuilder(std::string_view InSearchText)
		: SearchText(InSearchText)
	{
	}

	auto FObjectPropertyViewBuilder::AddProperty(DObject* Object, FProperty* Property, uint32 ArrayIndex,
		const FPropertyViewOptions& Options, std::string_view SearchKeywords) -> void
	{
		if (!Object || !Property || ArrayIndex >= Property->GetArrayDim()) return;
		const std::string Label = Options.Label.empty() ? MakeReflectedPropertyLabel(*Property, ArrayIndex) : Options.Label;
		Rows.push_back({Object, Property, ArrayIndex, Label,
			std::format("{} {} {}", Property->NamePrivate.ToString(), Label, SearchKeywords), {}});
	}

	auto FObjectPropertyViewBuilder::AddCustomRow(std::string_view SearchKeywords, FCustomRowDrawer Drawer) -> void
	{
		if (!Drawer) return;
		Rows.push_back({nullptr, nullptr, 0, {}, std::string(SearchKeywords), std::move(Drawer)});
	}

	auto FObjectPropertyViewBuilder::HideProperty(FProperty* Property) -> void
	{
		if (Property) HiddenProperties.insert(Property);
	}

	auto FObjectPropertyViewBuilder::ReplaceDefaultProperties() -> void
	{
		bReplaceDefaultProperties = true;
	}

	auto FObjectPropertyViewBuilder::IsPropertyHidden(const FProperty& Property) const -> bool
	{
		return HiddenProperties.contains(const_cast<FProperty*>(&Property));
	}

	auto FObjectPropertyViewBuilder::IsReplacingDefaultProperties() const -> bool
	{
		return bReplaceDefaultProperties;
	}

	auto FObjectPropertyViewBuilder::MatchesSearch(std::string_view Candidate) const -> bool
	{
		return SearchText.empty() || StringUtils::ContainsInsensitive(Candidate, SearchText);
	}

	auto FObjectPropertyViewBuilder::GetVisibleRowCount() const -> uint32
	{
		return static_cast<uint32>(std::ranges::count_if(Rows, [this](const FRow& Row) { return MatchesSearch(Row.SearchText); }));
	}

	auto FObjectPropertyViewBuilder::DrawRows(FReflectedPropertyView& PropertyView,
		const FReflectedPropertyViewContext& ViewContext) const -> FObjectPropertyViewBuilderResult
	{
		FObjectPropertyViewBuilderResult Result;
		for (const FRow& Row : Rows)
		{
			if (!MatchesSearch(Row.SearchText)) continue;
			++Result.VisibleRowCount;
			if (Row.Property)
				Result.bChanged |= PropertyView.EditProperty(ViewContext, Row.Object, Row.Property, Row.ArrayIndex, {.Label = Row.Label});
			else
				Result.bChanged |= Row.Drawer(PropertyView, ViewContext);
		}
		return Result;
	}

	auto FEditorVisualizationCollector::AddLine(const FEditorVisualizationLine& Line) -> void
	{
		if (!Line.Actor || !Line.Component || !std::isfinite(Line.WidthPixels) || !std::isfinite(Line.HitTolerancePixels)
			|| !std::isfinite(Line.PatternPeriodPixels)) return;
		if (glm::length(Line.End - Line.Start) <= kSmallNumber) return;
		Lines.push_back(Line);
	}

	auto FEditorVisualizationCollector::AddIcon(const FEditorVisualizationIcon& Icon) -> void
	{
		if (!Icon.Actor || !Icon.Component || !std::isfinite(Icon.SizePixels) || !std::isfinite(Icon.HitPaddingPixels)
			|| Icon.SizePixels <= 0.0f || Icon.HitPaddingPixels < 0.0f) return;
		Icons.push_back(Icon);
	}

	auto FEditorVisualizationCollector::AppendToView(FSceneView& View) const -> void
	{
		View.OverlayLines.reserve(View.OverlayLines.size() + Lines.size());
		for (const FEditorVisualizationLine& Line : Lines)
			View.OverlayLines.push_back({Line.Start, Line.End, Line.Color, Line.WidthPixels, Line.Pattern, Line.PatternPeriodPixels});
		View.OverlayIcons.reserve(View.OverlayIcons.size() + Icons.size());
		for (const FEditorVisualizationIcon& Icon : Icons)
			View.OverlayIcons.push_back({Icon.Icon, Icon.WorldPosition, Icon.Color, Icon.SizePixels});
	}

	auto FEditorVisualizationCollector::HitTest(const FSceneView& View, const FVector2f& ViewportPosition) const -> FEditorVisualizationHit
	{
		FEditorVisualizationHit Best;
		FVector3 RayOrigin, RayDirection;
		if (!SceneViewProjection::BuildViewportRay(View, ViewportPosition, RayOrigin, RayDirection)) return Best;
		(void)RayDirection;
		for (const FEditorVisualizationIcon& Icon : Icons)
		{
			FVector2f ScreenPosition;
			if (!SceneViewProjection::ProjectWorldToViewport(View, Icon.WorldPosition, ScreenPosition)) continue;
			const float HitHalfExtent = Icon.SizePixels * 0.5f + Icon.HitPaddingPixels;
			const FVector2f Delta = glm::abs(ViewportPosition - ScreenPosition);
			if (Delta.x > HitHalfExtent || Delta.y > HitHalfExtent) continue;
			const double Distance = glm::length(Icon.WorldPosition - RayOrigin);
			if (!std::isfinite(Distance)) continue;
			if (Distance < Best.Distance - 1.e-6 || (std::abs(Distance - Best.Distance) <= 1.e-6 && Icon.HitPriority > Best.Priority))
				Best = {Icon.Actor, Icon.Component, Distance, Icon.HitPriority, Icon.bDepthIndependentHit};
		}
		for (const FEditorVisualizationLine& Line : Lines)
		{
			if (Best.bDepthIndependent && Best.Priority >= Line.HitPriority) continue;
			FVector2f StartScreen, EndScreen;
			if (!SceneViewProjection::ProjectWorldToViewport(View, Line.Start, StartScreen) || !SceneViewProjection::ProjectWorldToViewport(View, Line.End, EndScreen)) continue;
			float T = 0.0f;
			if (DistanceToSegment(ViewportPosition, StartScreen, EndScreen, T) > std::max(Line.HitTolerancePixels, Line.WidthPixels * 0.5f)) continue;
			const FVector3 HitLocation = glm::mix(Line.Start, Line.End, static_cast<double>(T));
			const double Distance = glm::length(HitLocation - RayOrigin);
			if (!std::isfinite(Distance)) continue;
			if (Distance < Best.Distance - 1.e-6 || (std::abs(Distance - Best.Distance) <= 1.e-6 && Line.HitPriority > Best.Priority))
				Best = {Line.Actor, Line.Component, Distance, Line.HitPriority, false};
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

	auto FLevelEditorCustomizationRegistry::FindObjectDetailsCustomizations(const DClass* Class) const -> std::vector<std::shared_ptr<IObjectDetailsCustomization>>
	{
		std::vector<std::shared_ptr<IObjectDetailsCustomization>> Result;
		std::vector<const DClass*> Hierarchy;
		for (const DClass* Current = Class; Current != nullptr; Current = Current->GetSuperClass()) Hierarchy.push_back(Current);
		for (auto It = Hierarchy.rbegin(); It != Hierarchy.rend(); ++It)
		{
			if (const auto Entry = ObjectDetails.find(const_cast<DClass*>(*It)); Entry != ObjectDetails.end())
				Result.push_back(Entry->second.Customization);
		}
		return Result;
	}
} // namespace Durin
