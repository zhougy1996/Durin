#include "LevelEditorCustomizations.h"
#include "Viewport/EditorPrimitiveDrawing.h"

#include "DObject/Class.h"
#include "DObject/Property.h"
#include "Editor/PropertyView.h"
#include "Math/Operations.h"
#include "Misc/StringHelper.h"
#include "SceneViewProjection.h"

namespace Durin::Editor::Level
{
	namespace
	{
		class FOwnedActorVisualizer final : public IActorEditorVisualizer
		{
		public:
			FOwnedActorVisualizer(std::shared_ptr<IActorEditorVisualizer> InValue,
				FModuleOwnedCallbackGate InGate, FModuleOwnedResourceLease InResource)
				: Resource(std::move(InResource)), Value(std::move(InValue)), Gate(std::move(InGate)) {}
			auto DrawVisualization(AActor* Actor, const FEditorVisualizationContext& Context,
				FEditorVisualizationCollector& Collector) const -> void override
			{ auto Call = Gate.TryEnter(); if (Call) Value->DrawVisualization(Actor, Context, Collector); }
		private:
			FModuleOwnedResourceLease Resource;
			std::shared_ptr<IActorEditorVisualizer> Value;
			FModuleOwnedCallbackGate Gate;
		};

		class FOwnedComponentVisualizer final : public IComponentEditorVisualizer
		{
		public:
			FOwnedComponentVisualizer(std::shared_ptr<IComponentEditorVisualizer> InValue,
				FModuleOwnedCallbackGate InGate, FModuleOwnedResourceLease InResource)
				: Resource(std::move(InResource)), Value(std::move(InValue)), Gate(std::move(InGate)) {}
			auto DrawVisualization(DActorComponent* Component, const FEditorVisualizationContext& Context,
				FEditorVisualizationCollector& Collector) const -> void override
			{ auto Call = Gate.TryEnter(); if (Call) Value->DrawVisualization(Component, Context, Collector); }
		private:
			FModuleOwnedResourceLease Resource;
			std::shared_ptr<IComponentEditorVisualizer> Value;
			FModuleOwnedCallbackGate Gate;
		};

		class FOwnedObjectDetails final : public IObjectDetailsCustomization
		{
		public:
			FOwnedObjectDetails(std::shared_ptr<IObjectDetailsCustomization> InValue,
				FModuleOwnedCallbackGate InGate, FModuleOwnedResourceLease InResource)
				: Resource(std::move(InResource)), Value(std::move(InValue)), Gate(std::move(InGate)) {}
			auto CustomizeDetails(FLevelEditorContext& Context, DObject* Object,
				FObjectPropertyViewBuilder& Builder) -> void override
			{ auto Call = Gate.TryEnter(); if (Call) Value->CustomizeDetails(Context, Object, Builder); }
		private:
			FModuleOwnedResourceLease Resource;
			std::shared_ptr<IObjectDetailsCustomization> Value;
			FModuleOwnedCallbackGate Gate;
		};
	}
	namespace
	{
		auto DistanceToSegment(const FVector2f& Point, const FVector2f& A, const FVector2f& B, float& OutT) -> float
		{
			const FVector2f Segment = B - A;
			const float LengthSquared = Math::LengthSquared(Segment);
			OutT = LengthSquared > 1.e-6f ? std::clamp(Math::Dot(Point - A, Segment) / LengthSquared, 0.0f, 1.0f) : 0.0f;
			return Math::Length(Point - (A + Segment * OutT));
		}

		auto GetScreenSizedBoxTransform(const FSceneView& View, const FEditorVisualizationBox& Box) -> std::optional<FMatrix>
		{
			const FVector4 Clip = View.ViewProjectionMatrix * FVector4(Box.WorldPosition, 1.0);
			const double ProjectionScale = Math::Length(FVector3(
				View.ProjectionMatrix[0][1], View.ProjectionMatrix[1][1], View.ProjectionMatrix[2][1]));
			if (!std::isfinite(Clip.w) || Clip.w <= 1.e-8 || !std::isfinite(ProjectionScale) || ProjectionScale <= 1.e-8) return std::nullopt;
			const double WorldSize = Box.SizePixels * 2.0 * Clip.w / (ProjectionScale * std::max(1u, View.ViewportHeight));
			if (!std::isfinite(WorldSize) || WorldSize <= 0.0) return std::nullopt;
			return Math::TranslationMatrix(Box.WorldPosition) * Math::ScaleMatrix(FVector3(WorldSize));
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
		const ::Durin::Editor::FPropertyViewOptions& Options, std::string_view SearchKeywords) -> void
	{
		if (!Object || !Property || ArrayIndex >= Property->GetArrayDim()) return;
		const std::string Label = Options.Label.empty() ? ::Durin::Editor::MakePropertyLabel(*Property, ArrayIndex) : Options.Label;
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

	auto FObjectPropertyViewBuilder::DrawRows(::Durin::Editor::FPropertyView& PropertyView,
		const ::Durin::Editor::FPropertyViewContext& ViewContext) const -> FObjectPropertyViewBuilderResult
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
		if (!Line.Actor.IsValid() || !Line.Component.IsValid() || !std::isfinite(Line.WidthPixels) || !std::isfinite(Line.HitTolerancePixels)
			|| !std::isfinite(Line.PatternPeriodPixels)) return;
		if (Math::Length(Line.End - Line.Start) <= kSmallNumber) return;
		Lines.push_back(Line);
	}

	auto FEditorVisualizationCollector::AddIcon(const FEditorVisualizationIcon& Icon) -> void
	{
		if (!Icon.Actor.IsValid() || !Icon.Component.IsValid() || !std::isfinite(Icon.SizePixels) || !std::isfinite(Icon.HitPaddingPixels)
			|| Icon.SizePixels <= 0.0f || Icon.HitPaddingPixels < 0.0f) return;
		Icons.push_back(Icon);
	}

	auto FEditorVisualizationCollector::AddBox(const FEditorVisualizationBox& Box) -> void
	{
		if (!Box.Actor.IsValid() || !Box.Component.IsValid() || !std::isfinite(Box.SizePixels) || !std::isfinite(Box.HitPaddingPixels)
			|| Box.SizePixels <= 0.0f || Box.HitPaddingPixels < 0.0f) return;
		Boxes.push_back(Box);
	}

	auto FEditorVisualizationCollector::AddPrimitive(const FEditorVisualizationPrimitive& Primitive) -> void
	{
		if (!Primitive.Actor.IsValid() || !Primitive.Component.IsValid()
			|| !Math::IsFinite(Primitive.LocalToWorld)) return;
		Primitives.push_back(Primitive);
	}

	auto FEditorVisualizationCollector::AppendToView(FSceneView& View, const FEditorVisualizationHit* Hovered) const -> void
	{
		for (const FEditorVisualizationLine& Line : Lines)
		{
			const AActor* Actor = Line.Actor.Get();
			if (!Actor || !Line.Component.IsValid()) continue;
			const bool bHovered = Hovered && Actor == Hovered->Actor && Line.Component.Get() == Hovered->Component && Line.Element == Hovered->Element;
			const FVector4f& Color = bHovered && Line.HoverColor ? *Line.HoverColor : Line.Color;
			SubmitXRayVisibleLine(View, Line.Start, Line.End, Color,
				{.WidthPixels = Line.WidthPixels, .Pattern = Line.Pattern,
					.PatternPeriodPixels = Line.PatternPeriodPixels});
		}
		for (const FEditorVisualizationIcon& Icon : Icons)
		{
			const AActor* Actor = Icon.Actor.Get();
			if (!Actor || !Icon.Component.IsValid()) continue;
			const bool bHovered = Hovered && Actor == Hovered->Actor && Icon.Component.Get() == Hovered->Component && Icon.Element == Hovered->Element;
			const FVector4f& Color = bHovered && Icon.HoverColor ? *Icon.HoverColor : Icon.Color;
			SubmitXRayVisibleIcon(View, Icon.Icon, Icon.WorldPosition,
				Color, Icon.SizePixels);
		}
		View.OverlayPrimitives.reserve(
			View.OverlayPrimitives.size() + Primitives.size() + Boxes.size());
		for (const FEditorVisualizationPrimitive& Primitive : Primitives)
		{
			const AActor* Actor = Primitive.Actor.Get();
			if (!Actor || !Primitive.Component.IsValid()) continue;
			const bool bHovered = Hovered && Actor == Hovered->Actor
				&& Primitive.Component.Get() == Hovered->Component
				&& Primitive.Element == Hovered->Element;
			const FVector4f& Color = bHovered && Primitive.HoverColor
				? *Primitive.HoverColor : Primitive.Color;
			View.OverlayPrimitives.push_back({
				Primitive.Shape, Primitive.LocalToWorld, Color});
		}
		for (const FEditorVisualizationBox& Box : Boxes)
		{
			const AActor* Actor = Box.Actor.Get();
			if (!Actor || !Box.Component.IsValid()) continue;
			const std::optional<FMatrix> LocalToWorld = GetScreenSizedBoxTransform(View, Box);
			if (!LocalToWorld) continue;
			const bool bHovered = Hovered && Actor == Hovered->Actor && Box.Component.Get() == Hovered->Component && Box.Element == Hovered->Element;
			const FVector4f& Color = bHovered && Box.HoverColor ? *Box.HoverColor : Box.Color;
			View.OverlayPrimitives.push_back({EViewOverlayShape::Box, *LocalToWorld, Color});
		}
	}

	auto FEditorVisualizationCollector::AppendToView(FSceneView& View, const AActor* HoveredActor) const -> void
	{
		FEditorVisualizationHit Hit;
		Hit.Actor = const_cast<AActor*>(HoveredActor);
		for (const FEditorVisualizationLine& Line : Lines)
		{
			const AActor* Actor = Line.Actor.Get();
			if (!Actor || !Line.Component.IsValid()) continue;
			const FVector4f& Color = Actor == Hit.Actor && Line.HoverColor ? *Line.HoverColor : Line.Color;
			SubmitXRayVisibleLine(View, Line.Start, Line.End, Color,
				{.WidthPixels = Line.WidthPixels, .Pattern = Line.Pattern,
					.PatternPeriodPixels = Line.PatternPeriodPixels});
		}
		for (const FEditorVisualizationIcon& Icon : Icons)
		{
			const AActor* Actor = Icon.Actor.Get();
			if (!Actor || !Icon.Component.IsValid()) continue;
			const FVector4f& Color = Actor == Hit.Actor && Icon.HoverColor ? *Icon.HoverColor : Icon.Color;
			SubmitXRayVisibleIcon(View, Icon.Icon, Icon.WorldPosition,
				Color, Icon.SizePixels);
		}
		View.OverlayPrimitives.reserve(
			View.OverlayPrimitives.size() + Primitives.size() + Boxes.size());
		for (const FEditorVisualizationPrimitive& Primitive : Primitives)
		{
			const AActor* Actor = Primitive.Actor.Get();
			if (!Actor || !Primitive.Component.IsValid()) continue;
			const FVector4f& Color = Actor == Hit.Actor && Primitive.HoverColor
				? *Primitive.HoverColor : Primitive.Color;
			View.OverlayPrimitives.push_back({
				Primitive.Shape, Primitive.LocalToWorld, Color});
		}
		for (const FEditorVisualizationBox& Box : Boxes)
		{
			const AActor* Actor = Box.Actor.Get();
			if (!Actor || !Box.Component.IsValid()) continue;
			const std::optional<FMatrix> LocalToWorld = GetScreenSizedBoxTransform(View, Box);
			if (!LocalToWorld) continue;
			const FVector4f& Color = Actor == Hit.Actor && Box.HoverColor ? *Box.HoverColor : Box.Color;
			View.OverlayPrimitives.push_back({EViewOverlayShape::Box, *LocalToWorld, Color});
		}
	}

	auto FEditorVisualizationCollector::HitTest(const FSceneView& View, const FVector2f& ViewportPosition) const -> FEditorVisualizationHit
	{
		FEditorVisualizationHit Best;
		FVector3 RayOrigin, RayDirection;
		if (!SceneViewProjection::BuildViewportRay(View, ViewportPosition, RayOrigin, RayDirection)) return Best;
		(void)RayDirection;
		for (const FEditorVisualizationBox& Box : Boxes)
		{
			AActor* Actor = Box.Actor.Get();
			DActorComponent* Component = Box.Component.Get();
			if (!Actor || !Component) continue;
			FVector2f ScreenPosition;
			if (!SceneViewProjection::ProjectWorldToViewport(View, Box.WorldPosition, ScreenPosition)) continue;
			const float HitHalfExtent = Box.SizePixels * 0.5f + Box.HitPaddingPixels;
			const FVector2f Delta = Math::Abs(ViewportPosition - ScreenPosition);
			if (Delta.x > HitHalfExtent || Delta.y > HitHalfExtent) continue;
			const double Distance = Math::Length(Box.WorldPosition - RayOrigin);
			if (!std::isfinite(Distance)) continue;
			if (Distance < Best.Distance - 1.e-6 || (std::abs(Distance - Best.Distance) <= 1.e-6 && Box.HitPriority > Best.Priority))
				Best = {Actor, Component, Box.Element, Distance, Box.HitPriority, Box.bDepthIndependentHit};
		}
		for (const FEditorVisualizationIcon& Icon : Icons)
		{
			AActor* Actor = Icon.Actor.Get();
			DActorComponent* Component = Icon.Component.Get();
			if (!Actor || !Component) continue;
			FVector2f ScreenPosition;
			if (!SceneViewProjection::ProjectWorldToViewport(View, Icon.WorldPosition, ScreenPosition)) continue;
			const float HitHalfExtent = Icon.SizePixels * 0.5f + Icon.HitPaddingPixels;
			const FVector2f Delta = Math::Abs(ViewportPosition - ScreenPosition);
			if (Delta.x > HitHalfExtent || Delta.y > HitHalfExtent) continue;
			const double Distance = Math::Length(Icon.WorldPosition - RayOrigin);
			if (!std::isfinite(Distance)) continue;
			if (Distance < Best.Distance - 1.e-6 || (std::abs(Distance - Best.Distance) <= 1.e-6 && Icon.HitPriority > Best.Priority))
				Best = {Actor, Component, Icon.Element, Distance, Icon.HitPriority, Icon.bDepthIndependentHit};
		}
		for (const FEditorVisualizationLine& Line : Lines)
		{
			AActor* Actor = Line.Actor.Get();
			DActorComponent* Component = Line.Component.Get();
			if (!Actor || !Component) continue;
			if (Best.bDepthIndependent && Best.Priority >= Line.HitPriority) continue;
			FVector2f StartScreen, EndScreen;
			if (!SceneViewProjection::ProjectWorldToViewport(View, Line.Start, StartScreen) || !SceneViewProjection::ProjectWorldToViewport(View, Line.End, EndScreen)) continue;
			float T = 0.0f;
			if (DistanceToSegment(ViewportPosition, StartScreen, EndScreen, T) > std::max(Line.HitTolerancePixels, Line.WidthPixels * 0.5f)) continue;
			const FVector3 HitLocation = Math::Lerp(Line.Start, Line.End, static_cast<double>(T));
			const double Distance = Math::Length(HitLocation - RayOrigin);
			if (!std::isfinite(Distance)) continue;
			if (Distance < Best.Distance - 1.e-6 || (std::abs(Distance - Best.Distance) <= 1.e-6 && Line.HitPriority > Best.Priority))
				Best = {Actor, Component, Line.Element, Distance, Line.HitPriority, false};
		}
		return Best;
	}

	auto FLevelEditorCustomizationRegistry::Get() -> FLevelEditorCustomizationRegistry&
	{
		static FLevelEditorCustomizationRegistry Registry;
		return Registry;
	}

	auto FLevelEditorCustomizationRegistry::RegisterComponentVisualizer(
		DClass* Class, std::shared_ptr<IComponentEditorVisualizer> Visualizer,
		FModuleOwnedCallbackGate OwnerGate) -> FLevelEditorCustomizationHandle
	{
		auto Call = OwnerGate.TryEnter();
		if (OwnerGate.IsValid() && !Call) return {};
		if (!Class || !Visualizer || ComponentVisualizers.contains(Class)) return {};
		if (OwnerGate.IsValid())
		{
			auto Resource = OwnerGate.RetainResource();
			if (!Resource) return {};
			Visualizer = std::make_shared<FOwnedComponentVisualizer>(
				std::move(Visualizer), OwnerGate, std::move(Resource));
		}
		const uint64 Id = NextHandleId++;
		ComponentVisualizers.emplace(Class, TEntry<IComponentEditorVisualizer>{Id, std::move(Visualizer)});
		return {Id, ELevelEditorCustomizationKind::ComponentVisualizer};
	}

	auto FLevelEditorCustomizationRegistry::RegisterActorVisualizer(
		DClass* Class, std::shared_ptr<IActorEditorVisualizer> Visualizer,
		FModuleOwnedCallbackGate OwnerGate) -> FLevelEditorCustomizationHandle
	{
		auto Call = OwnerGate.TryEnter();
		if (OwnerGate.IsValid() && !Call) return {};
		if (!Class || !Visualizer || ActorVisualizers.contains(Class)) return {};
		if (OwnerGate.IsValid())
		{
			auto Resource = OwnerGate.RetainResource();
			if (!Resource) return {};
			Visualizer = std::make_shared<FOwnedActorVisualizer>(
				std::move(Visualizer), OwnerGate, std::move(Resource));
		}
		const uint64 Id = NextHandleId++;
		ActorVisualizers.emplace(Class, TEntry<IActorEditorVisualizer>{Id, std::move(Visualizer)});
		return {Id, ELevelEditorCustomizationKind::ActorVisualizer};
	}

	auto FLevelEditorCustomizationRegistry::RegisterObjectDetails(
		DClass* Class, std::shared_ptr<IObjectDetailsCustomization> Customization,
		FModuleOwnedCallbackGate OwnerGate) -> FLevelEditorCustomizationHandle
	{
		auto Call = OwnerGate.TryEnter();
		if (OwnerGate.IsValid() && !Call) return {};
		if (!Class || !Customization || ObjectDetails.contains(Class)) return {};
		if (OwnerGate.IsValid())
		{
			auto Resource = OwnerGate.RetainResource();
			if (!Resource) return {};
			Customization = std::make_shared<FOwnedObjectDetails>(
				std::move(Customization), OwnerGate, std::move(Resource));
		}
		const uint64 Id = NextHandleId++;
		ObjectDetails.emplace(Class, TEntry<IObjectDetailsCustomization>{Id, std::move(Customization)});
		return {Id, ELevelEditorCustomizationKind::ObjectDetails};
	}

	auto FLevelEditorCustomizationRegistry::Unregister(FLevelEditorCustomizationHandle Handle) -> bool
	{
		if (!Handle) return false;
		if (Handle.Kind == ELevelEditorCustomizationKind::ActorVisualizer)
		{
			const auto It = std::ranges::find_if(ActorVisualizers, [Handle](const auto& Pair) { return Pair.second.HandleId == Handle.Id; });
			if (It == ActorVisualizers.end()) return false;
			ActorVisualizers.erase(It);
			return true;
		}
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

	auto FLevelEditorCustomizationRegistry::FindActorVisualizer(const DClass* Class) const -> std::shared_ptr<IActorEditorVisualizer>
	{
		return FindMostSpecific<IActorEditorVisualizer>(Class, ActorVisualizers);
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
} // namespace Durin::Editor::Level
