#include "ViewportTestSupport.h"
#include "Editor/EditorTransactionTestSupport.h"
#include "DObject/Class.h"
#include "Math/Operations.h"
#include "Actors/SplineMeshActor.h"
#include "Actors/TerrainActor.h"
#include "Actors/VolumetricCloudActor.h"
#include "Components/SplineMeshComponent.h"
#include "Components/TerrainComponent.h"
#include "Components/VolumetricCloudComponent.h"
#include "StaticMesh/StaticMesh.h"
#include "Terrain/TerrainHeightmap.h"
#include "TerrainDetails.h"
#include "VolumetricCloudDetails.h"
#include "TerrainHeightmapThumbnailRenderer.h"

namespace
{
	auto GetWorldSimpleElements(const Durin::FSceneView& View,
		Durin::ESimpleElementType Type)
		-> std::vector<const Durin::FSimpleElement*>
	{
		std::vector<const Durin::FSimpleElement*> Result;
		for (const Durin::FSimpleElement& Element :
			View.SimpleElements.GetElements())
		{
			if (Element.Type == Type
				&& Element.DepthPriorityGroup
					== Durin::ESceneDepthPriorityGroup::World)
				Result.push_back(&Element);
		}
		return Result;
	}
}

TEST(FSplineComponentVisualizerTests, EmitsSelectableCurveLinesAndControlPointBoxes)
{
	InitializeDObjectSystem();
	auto* Actor = Durin::NewObject<Durin::AActor>(nullptr, "SplineActor");
	auto* Spline = Durin::Cast<Durin::DSplineComponent>(Actor->AddInstanceComponent(Durin::DSplineComponent::StaticClass(), "Spline"));
	ASSERT_NE(Spline, nullptr);
	Durin::FSceneView View;
	View.ViewportWidth = 800;
	View.ViewportHeight = 600;
	Durin::Editor::Level::FEditorVisualizationCollector Collector;
	const std::shared_ptr<Durin::Editor::Level::IComponentEditorVisualizer> Visualizer = Durin::Editor::Level::CreateSplineComponentVisualizer();
	Visualizer->DrawVisualization(Spline, {View, nullptr, true, true}, Collector);

	ASSERT_FALSE(Collector.GetLines().empty());
	EXPECT_TRUE(std::ranges::all_of(Collector.GetLines(), [Actor, Spline](const Durin::Editor::Level::FEditorVisualizationLine& Line) {
		return Line.Actor.Get() == Actor && Line.Component.Get() == Spline;
	}));
	EXPECT_GE(Collector.GetLines().size(), 16u);
	ASSERT_EQ(Collector.GetBoxes().size(), Spline->GetNumSplinePoints());
	EXPECT_TRUE(std::ranges::all_of(Collector.GetBoxes(), [Actor, Spline](const Durin::Editor::Level::FEditorVisualizationBox& Box) {
		return Box.Actor.Get() == Actor && Box.Component.Get() == Spline
			&& Box.Element.Kind == Durin::Editor::Level::EEditorSubElementKind::Point;
	}));

	Durin::MarkObjectHierarchyAsGarbage(Actor);
	Durin::CollectGarbage();
}

TEST(FTerrainDetailsCustomizationTests, HidesRawStatusAndAddsComponentFactGroups)
{
	InitializeDObjectSystem();
	auto* Actor = Durin::NewObject<Durin::ATerrainActor>(nullptr, "TerrainDetailsActor");
	auto* Component = Actor->GetTerrainComponent();
	Durin::Editor::Level::FLevelEditorContext Context;
	Context.SelectActor(Actor);
	Context.SelectComponent(Component);
	Durin::Editor::Level::FObjectPropertyViewBuilder Builder;
	Durin::Editor::Level::CreateTerrainDetailsCustomization()->CustomizeDetails(
		Context, Component, Builder);
	for (std::string_view Name : {"RenderStatus", "LastRenderDiagnostic",
		"CollisionStatus", "LastCollisionDiagnostic"})
	{
		Durin::FProperty* Property = Component->GetClass()->FindPropertyByName(Name);
		ASSERT_NE(Property, nullptr);
		EXPECT_TRUE(Builder.IsPropertyHidden(*Property));
	}
	EXPECT_EQ(Builder.GetVisibleRowCount(), 2u);
	Durin::MarkObjectHierarchyAsGarbage(Actor);
	Durin::CollectGarbage();
}

TEST(FVolumetricCloudDetailsCustomizationTests,
	HidesRawEligibilityAndPreservesFrozenPropertyGroups)
{
	InitializeDObjectSystem();
	auto* Actor = Durin::NewObject<Durin::AVolumetricCloudActor>(
		nullptr, "CloudDetailsActor");
	auto* Component = Actor->GetVolumetricCloudComponent();
	ASSERT_NE(Component, nullptr);
	Durin::Editor::Level::FLevelEditorContext Context;
	Durin::Editor::Level::FObjectPropertyViewBuilder Builder;
	Durin::Editor::Level::CreateVolumetricCloudDetailsCustomization()
		->CustomizeDetails(Context, Component, Builder);
	Durin::FProperty* Eligibility = Component->GetClass()->FindPropertyByName(
		"EligibilityStatus");
	ASSERT_NE(Eligibility, nullptr);
	EXPECT_TRUE(Builder.IsPropertyHidden(*Eligibility));
	EXPECT_EQ(Builder.GetVisibleRowCount(), 1u);
	EXPECT_EQ(Component->GetClass()->FindPropertyByName("BaseDensityTexture")
		->GetTypedMetadata().Category, "Density Inputs");
	EXPECT_EQ(Component->GetClass()->FindPropertyByName("MinimumZ")
		->GetTypedMetadata().Category, "Layer");
	EXPECT_EQ(Component->GetClass()->FindPropertyByName("WindOffset")
		->GetTypedMetadata().Category, "Mapping and Motion");
	EXPECT_EQ(Component->GetClass()->FindPropertyByName("Extinction")
		->GetTypedMetadata().Category, "Optical Response");
	EXPECT_EQ(Component->GetClass()->FindPropertyByName("Extinction")
		->GetTypedMetadata().Precision, 6);
	EXPECT_EQ(Component->GetClass()->FindPropertyByName("LightExtinction")
		->GetTypedMetadata().Precision, 6);
	Durin::MarkObjectHierarchyAsGarbage(Actor);
	Durin::CollectGarbage();
}

TEST(FTerrainHeightmapThumbnailTests, GeneratesFixedCanonicalOrientationAndMarker)
{
	std::shared_ptr<const Durin::FTerrainHeightmapPayload> Payload;
	const std::array<uint16, 6> Samples{0, 10'000, 65'535, 20'000, 30'000, 40'000};
	std::string Error;
	ASSERT_TRUE(Durin::BuildTerrainHeightmapPayload(3, 2, Samples, Payload, Error)) << Error;
	Durin::FByteArray Pixels;
	ASSERT_TRUE(Durin::Editor::Level::GenerateTerrainHeightmapThumbnailPixels(
		*Payload, Pixels, Error)) << Error;
	EXPECT_EQ(Pixels.size(), 256u * 256u * 4u);
	const auto Pixel = [&](uint32 X, uint32 Y, uint32 Channel)
	{
		return Pixels[(static_cast<size_t>(Y) * 256 + X) * 4 + Channel];
	};
	EXPECT_EQ(Pixel(0, 43, 0), std::byte{255});
	EXPECT_EQ(Pixel(0, 43, 3), std::byte{255});
	EXPECT_EQ(Pixel(0, 171, 3), std::byte{255});
	EXPECT_EQ(Pixel(43, 43, 0), std::byte{0});
	EXPECT_GT(Pixel(220, 43, 0), Pixel(43, 43, 0));
	EXPECT_GT(Pixel(255, 212, 0), Pixel(43, 43, 0));
}

TEST(FSplineComponentVisualizerTests, PublishesStableTypedSplineElements)
{
	InitializeDObjectSystem();
	auto* Actor = Durin::NewObject<Durin::AActor>(nullptr, "TypedSplineActor");
	auto* Spline = Durin::Cast<Durin::DSplineComponent>(Actor->AddInstanceComponent(Durin::DSplineComponent::StaticClass(), "Spline"));
	ASSERT_NE(Spline, nullptr);
	Durin::FSplinePoint First = *Spline->GetSplinePoint(0);
	First.TangentMode = Durin::ESplineTangentMode::ManualBroken;
	ASSERT_TRUE(Spline->UpdateSplinePoint(0, First));
	Durin::FSceneView View;
	Durin::Editor::Level::FEditorVisualizationCollector Collector;
	Durin::Editor::Level::CreateSplineComponentVisualizer()->DrawVisualization(Spline, {View, nullptr, true, true, true, {}}, Collector);
	EXPECT_TRUE(std::ranges::any_of(Collector.GetBoxes(), [First](const Durin::Editor::Level::FEditorVisualizationBox& Box) {
		return Box.Element.Kind == Durin::Editor::Level::EEditorSubElementKind::Point && Box.Element.StableId == First.Id;
	}));
	EXPECT_TRUE(std::ranges::any_of(Collector.GetBoxes(), [First](const Durin::Editor::Level::FEditorVisualizationBox& Box) {
		return Box.Element.Kind == Durin::Editor::Level::EEditorSubElementKind::LeaveTangent && Box.Element.StableId == First.Id;
	}));
	EXPECT_TRUE(std::ranges::any_of(Collector.GetLines(), [](const Durin::Editor::Level::FEditorVisualizationLine& Line) {
		return Line.Element.Kind == Durin::Editor::Level::EEditorSubElementKind::Segment && Line.Element.SecondaryIndex == 0;
	}));
	Durin::MarkObjectHierarchyAsGarbage(Actor);
	Durin::CollectGarbage();
}

TEST(FSplineDetailsCustomizationTests, PreservesComponentPropertiesAndHidesRawCurve)
{
	InitializeDObjectSystem();
	auto* Actor = Durin::NewObject<Durin::AActor>(nullptr, "SplineDetailsActor");
	auto* Spline = Durin::Cast<Durin::DSplineComponent>(Actor->AddInstanceComponent(Durin::DSplineComponent::StaticClass(), "Spline"));
	ASSERT_NE(Spline, nullptr);
	Durin::FProperty* SplineCurve = Spline->GetClass()->FindPropertyByName("SplineCurve");
	ASSERT_NE(SplineCurve, nullptr);

	Durin::Editor::Level::FLevelEditorContext Context;
	Context.SelectActor(Actor);
	Context.SelectComponent(Spline);
	Durin::Editor::Level::FObjectPropertyViewBuilder Builder;
	Durin::Editor::Level::CreateSplineDetailsCustomization()->CustomizeDetails(Context, Spline, Builder);

	EXPECT_FALSE(Builder.IsReplacingDefaultProperties());
	EXPECT_TRUE(Builder.IsPropertyHidden(*SplineCurve));
	EXPECT_EQ(Builder.GetVisibleRowCount(), 1u);

	Durin::MarkObjectHierarchyAsGarbage(Actor);
	Durin::CollectGarbage();
}

TEST(FSplineDetailsCustomizationTests, EmitsVisiblePropertyTableRows)
{
	InitializeDObjectSystem();
	auto* Actor = Durin::NewObject<Durin::AActor>(nullptr, "SplineDetailsRowsActor");
	auto* Spline = Durin::Cast<Durin::DSplineComponent>(Actor->AddInstanceComponent(Durin::DSplineComponent::StaticClass(), "Spline"));
	ASSERT_NE(Spline, nullptr);
	Durin::Editor::Level::FLevelEditorContext Context;
	Context.SelectActor(Actor);
	Context.SelectComponent(Spline);
	Durin::Editor::Level::FObjectPropertyViewBuilder Builder;
	Durin::Editor::Level::CreateSplineDetailsCustomization()->CustomizeDetails(Context, Spline, Builder);

	ImGuiContext* ImContext = ImGui::CreateContext();
	ASSERT_NE(ImContext, nullptr);
	ImGuiIO& IO = ImGui::GetIO();
	IO.DisplaySize = {800.0f, 600.0f};
	IO.DeltaTime = 1.0f / 60.0f;
	IO.IniFilename = nullptr;
	IO.Fonts->AddFontDefault();
	IO.Fonts->Build();
	ImGui::NewFrame();
	ImGui::SetNextWindowSize({600.0f, 400.0f}, ImGuiCond_Always);
	ImGui::Begin("Spline Details Test");
	const bool bTableOpen = Durin::MonaImGui::PropertyEdit::BeginTable("SplineDetailsRows");
	if (bTableOpen)
	{
		Durin::Editor::FPropertyView PropertyView;
		const Durin::Editor::Level::FObjectPropertyViewBuilderResult Result = Builder.DrawRows(PropertyView, {});
		EXPECT_EQ(Result.VisibleRowCount, 1u);
		EXPECT_GE(ImGui::TableGetRowIndex(), 2);
		Durin::MonaImGui::PropertyEdit::EndTable();
	}
	ImGui::End();
	ImGui::Render();
	ImGui::DestroyContext(ImContext);
	EXPECT_TRUE(bTableOpen);

	Durin::MarkObjectHierarchyAsGarbage(Actor);
	Durin::CollectGarbage();
}

TEST(FSplineViewportAuthoringTests, CubicSplitPreservesShapeAndCreatesStableId)
{
	InitializeDObjectSystem();
	auto* Actor = Durin::NewObject<Durin::AActor>(nullptr, "SplitSplineActor");
	auto* Spline = Durin::Cast<Durin::DSplineComponent>(Actor->AddInstanceComponent(Durin::DSplineComponent::StaticClass(), "Spline"));
	ASSERT_NE(Spline, nullptr);
	Durin::FSplinePoint Start = *Spline->GetSplinePoint(0);
	Durin::FSplinePoint End = *Spline->GetSplinePoint(1);
	Start.Position = {0.0, 0.0, 0.0}; Start.LeaveTangent = {80.0, 120.0, 0.0}; Start.TangentMode = Durin::ESplineTangentMode::ManualBroken;
	End.Position = {200.0, 40.0, 0.0}; End.ArriveTangent = {120.0, -80.0, 0.0}; End.TangentMode = Durin::ESplineTangentMode::ManualBroken;
	ASSERT_TRUE(Spline->UpdateSplinePoint(0, Start));
	ASSERT_TRUE(Spline->UpdateSplinePoint(1, End));
	std::vector<Durin::FVector3> Before;
	for (uint32 Step = 0; Step <= 100; ++Step) Before.push_back(Spline->GetSampleAtParameter({0, Step / 100.0}).Position);
	Durin::FGuid NewId;
	constexpr double SplitT = 0.37;
	ASSERT_TRUE(Durin::Editor::Level::SplitSplineSegment(*Spline, 0, SplitT, &NewId));
	ASSERT_TRUE(NewId.IsValid());
	ASSERT_EQ(Spline->GetNumSplinePoints(), 3u);
	EXPECT_EQ(Spline->GetSplinePoint(1)->Id, NewId);
	for (uint32 Step = 0; Step <= 100; ++Step)
	{
		const double U = Step / 100.0;
		const Durin::FSplineParameter Parameter = U <= SplitT
			? Durin::FSplineParameter{0, U / SplitT} : Durin::FSplineParameter{1, (U - SplitT) / (1.0 - SplitT)};
		EXPECT_LT(Durin::Math::Length(Spline->GetSampleAtParameter(Parameter).Position - Before[Step]), 1e-4);
	}
	Durin::MarkObjectHierarchyAsGarbage(Actor);
	Durin::CollectGarbage();
}

TEST(FSplineViewportAuthoringTests, AppendContinuesTheTailWithoutAFullLengthJump)
{
	InitializeDObjectSystem();
	auto* Actor = Durin::NewObject<Durin::AActor>(nullptr, "AppendSplineActor");
	auto* Spline = Durin::Cast<Durin::DSplineComponent>(Actor->AddInstanceComponent(Durin::DSplineComponent::StaticClass(), "Spline"));
	ASSERT_NE(Spline, nullptr);
	Durin::FSplinePoint Start = *Spline->GetSplinePoint(0);
	Durin::FSplinePoint End = *Spline->GetSplinePoint(1);
	Start.Position = {10.0, 20.0, 0.0};
	End.Position = {310.0, 420.0, 0.0};
	ASSERT_TRUE(Spline->UpdateSplinePoint(0, Start));
	ASSERT_TRUE(Spline->UpdateSplinePoint(1, End));
	const Durin::FVector3 Position = Durin::Editor::Level::CalculateSplineAppendPosition(*Spline);
	EXPECT_NEAR(Durin::Math::Length(Position - End.Position), 10.0, 1.e-6);
	EXPECT_LT(Durin::Math::Length(Durin::Math::Normalize(Position - End.Position)
		- Durin::Math::Normalize(End.Position - Start.Position)), 1.e-9);

	Durin::MarkObjectHierarchyAsGarbage(Actor);
	Durin::CollectGarbage();
}

TEST(FSplineViewportAuthoringTests, ModeUsesGuidMultiSelectionAndTransactionalDelete)
{
	InitializeDObjectSystem();
	Durin::DWorld* World = Durin::NewObject<Durin::DWorld>(nullptr, "SplineModeWorld");
	Durin::DLevel* Level = Durin::NewObject<Durin::DLevel>(World, "SplineModeLevel");
	ASSERT_TRUE(World->SetCurrentLevel(Level));
	auto* Actor = Level->SpawnActor<Durin::AActor>("SplineActor");
	auto* Spline = Durin::Cast<Durin::DSplineComponent>(Actor->AddInstanceComponent(Durin::DSplineComponent::StaticClass(), "Spline"));
	ASSERT_NE(Spline, nullptr);
	Spline->AddSplinePoint(Durin::FSplinePoint({200.0, 0.0, 0.0}));
	const Durin::FGuid FirstId = Spline->GetSplinePoint(0)->Id;
	const Durin::FGuid SecondId = Spline->GetSplinePoint(1)->Id;
	Durin::Editor::Level::FLevelEditorContext Context;
	Context.Synchronize(World);
	Context.SelectActor(Actor);
	Context.SelectComponent(Spline);
	Context.SelectSubElement(Spline, {Durin::Editor::Level::EEditorSubElementKind::Point, FirstId});
	Context.ToggleSubElement(Spline, {Durin::Editor::Level::EEditorSubElementKind::Point, SecondId});
	ASSERT_EQ(Context.GetSelectedSubElements().size(), 2u);
	ASSERT_TRUE(Spline->MoveSplinePoint(0, 2));
	EXPECT_TRUE(Context.IsSubElementSelected({Durin::Editor::Level::EEditorSubElementKind::Point, FirstId}));

	const Durin::Editor::Level::FLevelViewportEditModeHandle Handle = Durin::Editor::Level::RegisterSplineViewportEditMode();
	ASSERT_TRUE(Handle);
	Durin::Editor::Level::FLevelViewportEditModeManager Manager;
	ASSERT_TRUE(Manager.Activate("Spline", Context));
	const Durin::Editor::Level::FTransformGizmoTargetSet Targets = Manager.GetActiveMode()->GetGizmoTargets(Context);
	ASSERT_EQ(Targets.Targets.size(), 2u);
	EXPECT_EQ(Targets.Targets[0]->GetCapabilities(), Durin::Editor::Level::ETransformGizmoCapability::Translate);

	Durin::Editor::Level::FLevelEditorViewportClient Client;
	Durin::FSceneView View;
	ASSERT_TRUE(Client.CalcSceneView(800, 600, View));
	Durin::Editor::Level::FLevelEditorViewportInput Input;
	Input.bDelete = true;
	Durin::Tests::FTestTransactorOwner Transactions;
	ASSERT_TRUE(Manager.Tick(Context, Client, View, Input, Transactions.Get()));
	EXPECT_EQ(Spline->GetNumSplinePoints(), 1u);
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_EQ(Spline->GetNumSplinePoints(), 3u);
	EXPECT_TRUE(Spline->GetSplineCurve().FindPointIndex(FirstId).has_value());
	EXPECT_TRUE(Spline->GetSplineCurve().FindPointIndex(SecondId).has_value());

	Context.SelectSubElement(Spline, {Durin::Editor::Level::EEditorSubElementKind::Point, FirstId});
	Input = {};
	Input.bCancel = true;
	ASSERT_TRUE(Manager.Tick(Context, Client, View, Input, Transactions.Get()));
	EXPECT_TRUE(Context.GetSelectedSubElements().empty());
	EXPECT_EQ(Manager.GetActiveModeId(), "Spline");
	ASSERT_TRUE(Manager.Tick(Context, Client, View, Input, Transactions.Get()));
	EXPECT_EQ(Manager.GetActiveModeId(), "Select");
	EXPECT_EQ(Context.GetSelectedComponent(), Spline);
	Manager.Shutdown(&Context);
	EXPECT_TRUE(Durin::Editor::Level::FLevelViewportEditModeRegistry::Get().Unregister(Handle));
	Durin::MarkObjectHierarchyAsGarbage(World);
	Durin::CollectGarbage();
}

TEST(FSplineMeshActorViewportTests, PointTargetUpdatesGeneratedIdentityAndCancelRestoresOutput)
{
	InitializeDObjectSystem();
	auto* World = Durin::NewObject<Durin::DWorld>(nullptr, "SplineMeshViewportWorld");
	auto* Level = Durin::NewObject<Durin::DLevel>(World, "SplineMeshViewportLevel");
	ASSERT_TRUE(World->SetCurrentLevel(Level));
	auto* Actor = Level->SpawnActor<Durin::ASplineMeshActor>("SplineMeshActor");
	ASSERT_NE(Actor, nullptr);
	auto* Mesh = Durin::DStaticMesh::CreateDebugTriangle();
	Actor->SetPathMesh(Mesh);
	Actor->GetSplineComponent()->SetSplinePoints({
		Durin::FSplinePoint({0.0, 0.0, 0.0}), Durin::FSplinePoint({100.0, 0.0, 0.0}),
		Durin::FSplinePoint({200.0, 0.0, 0.0})});
	auto Segments = Actor->FindComponentsByClass<Durin::DSplineMeshComponent>();
	ASSERT_EQ(Segments.size(), 2u);
	Durin::DActorComponent* First = Segments[0];
	Durin::DActorComponent* Second = Segments[1];
	Durin::DSplineComponent* Spline = Actor->GetSplineComponent();
	const Durin::FGuid MiddleId = Spline->GetSplinePoint(1)->Id;

	Durin::Editor::Level::FLevelEditorContext Context;
	Context.Synchronize(World);
	Context.SelectActor(Actor);
	Context.SelectComponent(Spline);
	Context.SelectSubElement(Spline,
		{Durin::Editor::Level::EEditorSubElementKind::Point, MiddleId});
	const auto Handle = Durin::Editor::Level::RegisterSplineViewportEditMode();
	ASSERT_TRUE(Handle);
	Durin::Editor::Level::FLevelViewportEditModeManager Manager;
	ASSERT_TRUE(Manager.Activate("Spline", Context));
	auto Targets = Manager.GetActiveMode()->GetGizmoTargets(Context);
	ASSERT_EQ(Targets.Targets.size(), 1u);
	const Durin::FTransform Before = Targets.Targets[0]->GetTransform();
	Durin::FTransform Dragged = Before;
	Dragged.Translation.y += 40.0;
	ASSERT_TRUE(Targets.Targets[0]->SetTransform(Dragged));
	Segments = Actor->FindComponentsByClass<Durin::DSplineMeshComponent>();
	EXPECT_NE(std::ranges::find(Segments, First), Segments.end());
	EXPECT_NE(std::ranges::find(Segments, Second), Segments.end());
	EXPECT_NEAR(Spline->GetSplinePoint(1)->Position.y, 40.0, 1.e-8);
	EXPECT_EQ(Durin::Cast<Durin::DSplineMeshComponent>(First)->GetSplineMeshParams().EndPosition,
		Spline->GetSplinePoint(1)->Position);
	ASSERT_TRUE(Targets.Targets[0]->SetTransform(Before));
	EXPECT_NEAR(Spline->GetSplinePoint(1)->Position.y, 0.0, 1.e-8);
	Segments = Actor->FindComponentsByClass<Durin::DSplineMeshComponent>();
	EXPECT_NE(std::ranges::find(Segments, First), Segments.end());
	EXPECT_NE(std::ranges::find(Segments, Second), Segments.end());

	Manager.Shutdown(&Context);
	EXPECT_TRUE(Durin::Editor::Level::FLevelViewportEditModeRegistry::Get().Unregister(Handle));
	Durin::MarkObjectHierarchyAsGarbage(World);
	Durin::MarkAsGarbage(Mesh);
	Durin::CollectGarbage();
}

TEST(FLevelEditorCustomizationRegistryTests, RejectsDuplicatesFindsBaseClassAndUnregisters)
{
	InitializeDObjectSystem();
	auto& Registry = Durin::Editor::Level::FLevelEditorCustomizationRegistry::Get();
	const auto ActorVisualizer = std::make_shared<FTestActorVisualizer>();
	FCustomizationGuard ActorVisualizerGuard{Registry.RegisterActorVisualizer(Durin::AActor::StaticClass(), ActorVisualizer)};
	ASSERT_TRUE(ActorVisualizerGuard.Handle);
	EXPECT_FALSE(Registry.RegisterActorVisualizer(Durin::AActor::StaticClass(), std::make_shared<FTestActorVisualizer>()));
	EXPECT_EQ(Registry.FindActorVisualizer(Durin::APlayerStart::StaticClass()), ActorVisualizer);

	const auto Visualizer = std::make_shared<FTestComponentVisualizer>();
	FCustomizationGuard VisualizerGuard{Registry.RegisterComponentVisualizer(Durin::DSceneComponent::StaticClass(), Visualizer)};
	ASSERT_TRUE(VisualizerGuard.Handle);
	EXPECT_FALSE(Registry.RegisterComponentVisualizer(Durin::DSceneComponent::StaticClass(), std::make_shared<FTestComponentVisualizer>()));
	EXPECT_EQ(Registry.FindComponentVisualizer(Durin::DCameraComponent::StaticClass()), Visualizer);

	const auto Details = std::make_shared<FTestDetailsCustomization>();
	FCustomizationGuard DetailsGuard{Registry.RegisterObjectDetails(Durin::DSceneComponent::StaticClass(), Details)};
	ASSERT_TRUE(DetailsGuard.Handle);
	EXPECT_EQ(Registry.FindObjectDetails(Durin::DCameraComponent::StaticClass()), Details);
	const auto DerivedDetails = std::make_shared<FTestDetailsCustomization>();
	FCustomizationGuard DerivedDetailsGuard{Registry.RegisterObjectDetails(Durin::DCameraComponent::StaticClass(), DerivedDetails)};
	ASSERT_TRUE(DerivedDetailsGuard.Handle);
	const auto DetailsChain = Registry.FindObjectDetailsCustomizations(Durin::DCameraComponent::StaticClass());
	ASSERT_EQ(DetailsChain.size(), 2u);
	EXPECT_EQ(DetailsChain[0], Details);
	EXPECT_EQ(DetailsChain[1], DerivedDetails);
	ASSERT_TRUE(Registry.Unregister(DetailsGuard.Handle));
	DetailsGuard.Handle = {};
	EXPECT_EQ(Registry.FindObjectDetails(Durin::DCameraComponent::StaticClass()), DerivedDetails);
}

TEST(FObjectPropertyViewBuilderTests, ComposesPropertiesHidingReplacementAndSearch)
{
	InitializeDObjectSystem();
	auto* RootComponent = Durin::NewObject<Durin::DSceneComponent>(nullptr, "BuilderComponent");
	ASSERT_NE(RootComponent, nullptr);
	Durin::FProperty* TransformProperty = RootComponent->GetClass()->FindPropertyByName("RelativeTransform");
	ASSERT_NE(TransformProperty, nullptr);

	Durin::Editor::Level::FObjectPropertyViewBuilder Builder("rotation");
	Builder.AddProperty(RootComponent, TransformProperty, 0, {.Label = "Transform"}, "Location Rotation Scale");
	Builder.AddCustomRow("Materials Material Slots", [](Durin::Editor::FPropertyView&, const Durin::Editor::FPropertyViewContext&) { return false; });
	Builder.HideProperty(TransformProperty);
	Builder.ReplaceDefaultProperties();

	EXPECT_EQ(Builder.GetVisibleRowCount(), 1u);
	EXPECT_TRUE(Builder.IsPropertyHidden(*TransformProperty));
	EXPECT_TRUE(Builder.IsReplacingDefaultProperties());
	Durin::MarkObjectHierarchyAsGarbage(RootComponent);
	Durin::CollectGarbage();
}

TEST(FObjectPropertyViewCustomizationTests, CameraDetailsOnlyCustomizeTheCameraComponent)
{
	InitializeDObjectSystem();
	Durin::DWorld* World = Durin::NewObject<Durin::DWorld>(nullptr, "PropertyCustomizationWorld");
	Durin::DLevel* Level = Durin::NewObject<Durin::DLevel>(World, "PropertyCustomizationLevel");
	ASSERT_TRUE(World->SetCurrentLevel(Level));
	auto* Actor = Level->SpawnActor<Durin::ACameraActor>("Camera");
	ASSERT_NE(Actor, nullptr);
	Durin::DCameraComponent* Component = Actor->GetCameraComponent();
	ASSERT_NE(Component, nullptr);

	Durin::Editor::Level::FLevelEditorContext Context;
	const std::shared_ptr<Durin::Editor::Level::IObjectDetailsCustomization> Customization = Durin::Editor::Level::CreateCameraDetailsCustomization();
	Durin::Editor::Level::FObjectPropertyViewBuilder ActorBuilder("camera");
	Customization->CustomizeDetails(Context, Actor, ActorBuilder);
	EXPECT_EQ(ActorBuilder.GetVisibleRowCount(), 0u);
	EXPECT_FALSE(ActorBuilder.IsReplacingDefaultProperties());

	Durin::Editor::Level::FObjectPropertyViewBuilder ComponentBuilder("camera");
	Customization->CustomizeDetails(Context, Component, ComponentBuilder);
	EXPECT_EQ(ComponentBuilder.GetVisibleRowCount(), 1u);
	EXPECT_FALSE(ComponentBuilder.IsReplacingDefaultProperties());
	Durin::FProperty* TransformProperty = Component->GetClass()->FindPropertyByName("RelativeTransform");
	Durin::FProperty* ProjectionProperty = Component->GetClass()->FindPropertyByName("ProjectionSettings");
	ASSERT_NE(TransformProperty, nullptr);
	ASSERT_NE(ProjectionProperty, nullptr);
	EXPECT_FALSE(ComponentBuilder.IsPropertyHidden(*TransformProperty));
	EXPECT_TRUE(ComponentBuilder.IsPropertyHidden(*ProjectionProperty));

	Durin::MarkObjectHierarchyAsGarbage(World);
	Durin::CollectGarbage();
}

TEST(FEditorVisualizationCollectorTests, UsesTheSameLinesForRenderingAndPicking)
{
	InitializeDObjectSystem();
	Durin::DWorld* World = Durin::NewObject<Durin::DWorld>(nullptr, "VisualizationWorld");
	Durin::DLevel* Level = Durin::NewObject<Durin::DLevel>(World, "VisualizationLevel");
	ASSERT_TRUE(World->SetCurrentLevel(Level));
	Durin::ACameraActor* Actor = Level->SpawnActor<Durin::ACameraActor>("Camera");
	ASSERT_NE(Actor, nullptr);
	Durin::Editor::Level::FLevelEditorViewportClient Client;
	Durin::FSceneView View;
	ASSERT_TRUE(Client.CalcSceneView(800, 600, View));
	const Durin::FVector3 Center = Client.GetCameraTransform().GetLocation() + Client.GetCameraTransform().GetForwardVector() * 5.0;
	Durin::Editor::Level::FEditorVisualizationCollector Collector;
	Collector.AddLine({Center - Client.GetCameraTransform().GetRightVector(), Center + Client.GetCameraTransform().GetRightVector(), {1.0f, 0.5f, 0.25f, 1.0f}, 3.0f, 7.0f, 4, Actor, Actor->GetCameraComponent()});
	View.SimpleElements = {};
	Collector.AppendToView(View);
	const auto WorldLines = GetWorldSimpleElements(
		View, Durin::ESimpleElementType::Line);
	ASSERT_EQ(WorldLines.size(), 1u);
	EXPECT_FLOAT_EQ(std::get<Durin::FSimpleElementLine>(
		WorldLines.front()->Value).Style.WidthPixels, 3.0f);
	Durin::FVector2f ScreenCenter;
	ASSERT_TRUE(Durin::SceneViewProjection::ProjectWorldToViewport(View, Center, ScreenCenter));
	const Durin::Editor::Level::FEditorVisualizationHit Hit = Collector.HitTest(View, ScreenCenter);
	EXPECT_EQ(Hit.Actor, Actor);
	EXPECT_EQ(Hit.Component, Actor->GetCameraComponent());
	EXPECT_EQ(Collector.HitTest(View, ScreenCenter + Durin::FVector2f(0.0f, 50.0f)).Actor, nullptr);
}

TEST(FLevelEditorViewportClientTests, AppendsBoundedHeightFieldCollisionTriangles)
{
	InitializeDObjectSystem();
	Durin::DWorld* World = Durin::NewObject<Durin::DWorld>(nullptr, "HeightFieldOverlayWorld");
	Durin::DLevel* Level = Durin::NewObject<Durin::DLevel>(World, "HeightFieldOverlayLevel");
	ASSERT_TRUE(World->SetCurrentLevel(Level));
	auto* Heightmap = Durin::NewObject<Durin::DTerrainHeightmap>(World, "HeightFieldOverlayAsset");
	const std::array<uint16, 4> Samples{0, 0, 0, 65535};
	std::string Error;
	ASSERT_TRUE(Heightmap->InitializeFromSamples(2, 2, Samples, Error)) << Error;
	auto* Actor = Level->SpawnActor<Durin::ATerrainActor>("HeightFieldOverlayTerrain");
	ASSERT_NE(Actor, nullptr);
	auto* Component = Actor->GetTerrainComponent();
	ASSERT_NE(Component, nullptr);
	ASSERT_TRUE(Component->SetSampleSpacing(1.0, 1.0));
	ASSERT_TRUE(Component->SetHeightRange(1.0, 0.0));
	Component->SetHeightmap(Heightmap);
	ASSERT_TRUE(Component->SetCollisionProfileName(Durin::CollisionProfile::WorldStatic));
	Durin::Editor::Level::FLevelEditorViewportClient Client;
	Client.InitializeForLevel(Level);
	Durin::FSceneView View;
	ASSERT_TRUE(Client.CalcSceneView(800, 600, View));
	EXPECT_TRUE(View.SimpleElements.GetElements().empty());
	World->SetCollisionDebugDrawEnabled(true);
	ASSERT_TRUE(Component->RequestPhysicsStateCreation(true));
	EXPECT_EQ(Component->GetCollisionStatus(),
		Durin::ETerrainCollisionStatus::Ready);
	ASSERT_TRUE(Client.CalcSceneView(800, 600, View));
	const auto CollisionLines = GetWorldSimpleElements(
		View, Durin::ESimpleElementType::Line);
	EXPECT_EQ(CollisionLines.size(), 18u);
	EXPECT_EQ(std::ranges::count_if(CollisionLines, [](const auto* Element) {
		return std::get<Durin::FSimpleElementLine>(Element->Value)
			.Style.WidthPixels == 1.5f;
	}), 6);

	World->SetCollisionDebugDrawEnabled(false);
	ASSERT_TRUE(Client.CalcSceneView(800, 600, View));
	EXPECT_TRUE(View.SimpleElements.GetElements().empty());
	Durin::MarkObjectHierarchyAsGarbage(World);
	Durin::CollectGarbage();
}

TEST(FLevelEditorViewportClientTests, PublishesWorldGridStateToTheSceneView)
{
	Durin::Editor::Level::FLevelEditorViewportClient Client;
	Durin::FSceneView View;
	ASSERT_TRUE(Client.CalcSceneView(800, 600, View));
	EXPECT_TRUE(View.EditorGrid.bVisible);
	EXPECT_DOUBLE_EQ(View.EditorGrid.Height, 0.0);
	EXPECT_GT(View.EditorGrid.FadeDistance, 0.0f);
	EXPECT_GT(View.EditorGrid.AxisXColor.r, View.EditorGrid.AxisXColor.g);
	EXPECT_GT(View.EditorGrid.AxisYColor.g, View.EditorGrid.AxisYColor.r);

	Client.SetGridVisible(false);
	ASSERT_TRUE(Client.CalcSceneView(800, 600, View));
	EXPECT_FALSE(View.EditorGrid.bVisible);
}

TEST(FEditorVisualizationCollectorTests, UsesTheSameIconsForRenderingAndDepthIndependentPicking)
{
	InitializeDObjectSystem();
	Durin::DWorld* World = Durin::NewObject<Durin::DWorld>(nullptr, "IconVisualizationWorld");
	Durin::DLevel* Level = Durin::NewObject<Durin::DLevel>(World, "IconVisualizationLevel");
	ASSERT_TRUE(World->SetCurrentLevel(Level));
	Durin::ACameraActor* Actor = Level->SpawnActor<Durin::ACameraActor>("Camera");
	ASSERT_NE(Actor, nullptr);
	Durin::Editor::Level::FLevelEditorViewportClient Client;
	Durin::FSceneView View;
	ASSERT_TRUE(Client.CalcSceneView(800, 600, View));
	const Durin::FVector3 Center = Client.GetCameraTransform().GetLocation() + Client.GetCameraTransform().GetForwardVector() * 5.0;
	Durin::Editor::Level::FEditorVisualizationCollector Collector;
	Collector.AddIcon({Durin::Editor::Level::EEditorVisualizationIcon::Camera, Center, Durin::FVector4f(1.0f), 30.0f, 3.0f, 100, Actor, Actor->GetCameraComponent(), true});
	View.SimpleElements = {};
	Collector.AppendToView(View);
	const auto WorldSprites = GetWorldSimpleElements(
		View, Durin::ESimpleElementType::Sprite);
	ASSERT_EQ(WorldSprites.size(), 1u);
	EXPECT_FLOAT_EQ(std::get<Durin::FSimpleElementSprite>(
		WorldSprites.front()->Value).SizePixels.x, 30.0f);
	Durin::FVector2f ScreenCenter;
	ASSERT_TRUE(Durin::SceneViewProjection::ProjectWorldToViewport(View, Center, ScreenCenter));
	const Durin::Editor::Level::FEditorVisualizationHit Hit = Collector.HitTest(View, ScreenCenter + Durin::FVector2f(14.0f, 0.0f));
	EXPECT_EQ(Hit.Actor, Actor);
	EXPECT_TRUE(Hit.bDepthIndependent);
	EXPECT_EQ(Collector.HitTest(View, ScreenCenter + Durin::FVector2f(24.0f, 0.0f)).Actor, nullptr);
}

TEST(FEditorVisualizationCollectorTests, UsesScreenSizedBoxesForRenderingAndPicking)
{
	InitializeDObjectSystem();
	Durin::DWorld* World = Durin::NewObject<Durin::DWorld>(nullptr, "BoxVisualizationWorld");
	Durin::DLevel* Level = Durin::NewObject<Durin::DLevel>(World, "BoxVisualizationLevel");
	ASSERT_TRUE(World->SetCurrentLevel(Level));
	Durin::ACameraActor* Actor = Level->SpawnActor<Durin::ACameraActor>("Camera");
	ASSERT_NE(Actor, nullptr);
	Durin::Editor::Level::FLevelEditorViewportClient Client;
	Durin::FSceneView View;
	ASSERT_TRUE(Client.CalcSceneView(800, 600, View));
	const Durin::FVector3 Center = Client.GetCameraTransform().GetLocation() + Client.GetCameraTransform().GetForwardVector() * 5.0;
	const Durin::Editor::Level::FEditorSubElementSelection Element{Durin::Editor::Level::EEditorSubElementKind::Point, Durin::FGuid::NewGuid()};
	Durin::Editor::Level::FEditorVisualizationBox Box{Center, Durin::FVector4f(1.0f), 12.0f, 5.0f, 80,
		Actor, Actor->GetCameraComponent(), true};
	Box.Element = Element;
	Durin::Editor::Level::FEditorVisualizationCollector Collector;
	Collector.AddBox(Box);
	const size_t InitialPrimitiveCount = View.OverlayPrimitives.size();
	Collector.AppendToView(View);
	ASSERT_EQ(View.OverlayPrimitives.size(), InitialPrimitiveCount + 1);
	EXPECT_EQ(View.OverlayPrimitives.back().Shape, Durin::EViewOverlayShape::Box);
	Durin::FVector2f ScreenCenter;
	ASSERT_TRUE(Durin::SceneViewProjection::ProjectWorldToViewport(View, Center, ScreenCenter));
	const Durin::Editor::Level::FEditorVisualizationHit Hit = Collector.HitTest(View, ScreenCenter + Durin::FVector2f(10.0f, 0.0f));
	EXPECT_EQ(Hit.Actor, Actor);
	EXPECT_EQ(Hit.Component, Actor->GetCameraComponent());
	EXPECT_EQ(Hit.Element, Element);
	EXPECT_TRUE(Hit.bDepthIndependent);
	EXPECT_EQ(Collector.HitTest(View, ScreenCenter + Durin::FVector2f(12.0f, 0.0f)).Actor, nullptr);
	Durin::MarkObjectHierarchyAsGarbage(World);
	Durin::CollectGarbage();
}

TEST(FEditorVisualizationCollectorTests, AppliesOptionalHoverColorWithoutRegeneratingPrimitives)
{
	InitializeDObjectSystem();
	Durin::DWorld* World = Durin::NewObject<Durin::DWorld>(nullptr, "HoverColorWorld");
	Durin::DLevel* Level = Durin::NewObject<Durin::DLevel>(World, "HoverColorLevel");
	ASSERT_TRUE(World->SetCurrentLevel(Level));
	Durin::ACameraActor* Actor = Level->SpawnActor<Durin::ACameraActor>("Camera");
	ASSERT_NE(Actor, nullptr);
	Durin::Editor::Level::FEditorVisualizationCollector Collector;
	const Durin::FVector4f BaseColor{1.0f, 0.0f, 0.0f, 1.0f};
	const Durin::FVector4f HoverColor{0.0f, 1.0f, 0.0f, 1.0f};
	Collector.AddIcon({Durin::Editor::Level::EEditorVisualizationIcon::Camera, {0.0, 0.0, 5.0}, BaseColor, 30.0f, 3.0f, 100,
		Actor, Actor->GetCameraComponent(), true, HoverColor});

	Durin::FSceneView BaseView;
	Collector.AppendToView(BaseView);
	Durin::FSceneView HoveredView;
	Collector.AppendToView(HoveredView, Actor);
	const auto BaseSprites = GetWorldSimpleElements(
		BaseView, Durin::ESimpleElementType::Sprite);
	const auto HoveredSprites = GetWorldSimpleElements(
		HoveredView, Durin::ESimpleElementType::Sprite);
	ASSERT_EQ(BaseSprites.size(), 1u);
	ASSERT_EQ(HoveredSprites.size(), 1u);
	EXPECT_EQ(std::get<Durin::FSimpleElementSprite>(
		BaseSprites.front()->Value).Color, BaseColor);
	EXPECT_EQ(std::get<Durin::FSimpleElementSprite>(
		HoveredSprites.front()->Value).Color, HoverColor);
	EXPECT_EQ(Collector.GetIcons().front().Color, BaseColor);

	Durin::MarkObjectHierarchyAsGarbage(Actor);
	Durin::CollectGarbage();
	Durin::FSceneView ExpiredView;
	Collector.AppendToView(ExpiredView);
	EXPECT_TRUE(ExpiredView.SimpleElements.GetElements().empty());
	EXPECT_EQ(Collector.HitTest(HoveredView, {0.0f, 0.0f}).Actor, nullptr);
}

TEST(FEditorVisualizationCollectorTests, PreservesExactComponentAndSubElementHitIdentity)
{
	InitializeDObjectSystem();
	Durin::DWorld* World = Durin::NewObject<Durin::DWorld>(nullptr, "ElementHitWorld");
	Durin::DLevel* Level = Durin::NewObject<Durin::DLevel>(World, "ElementHitLevel");
	ASSERT_TRUE(World->SetCurrentLevel(Level));
	Durin::ACameraActor* Actor = Level->SpawnActor<Durin::ACameraActor>("Camera");
	ASSERT_NE(Actor, nullptr);
	Durin::Editor::Level::FLevelEditorViewportClient Client;
	Durin::FSceneView View;
	ASSERT_TRUE(Client.CalcSceneView(800, 600, View));
	const Durin::FVector3 Center = Client.GetCameraTransform().GetLocation() + Client.GetCameraTransform().GetForwardVector() * 5.0;
	const Durin::Editor::Level::FEditorSubElementSelection Element{Durin::Editor::Level::EEditorSubElementKind::Point, Durin::FGuid::NewGuid()};
	Durin::Editor::Level::FEditorVisualizationIcon Icon{Durin::Editor::Level::EEditorVisualizationIcon::Camera, Center, Durin::FVector4f(1.0f), 30.0f, 3.0f, 100,
		Actor, Actor->GetCameraComponent(), true};
	Icon.Element = Element;
	Durin::Editor::Level::FEditorVisualizationCollector Collector;
	Collector.AddIcon(Icon);
	Durin::FVector2f ScreenCenter;
	ASSERT_TRUE(Durin::SceneViewProjection::ProjectWorldToViewport(View, Center, ScreenCenter));
	const Durin::Editor::Level::FEditorVisualizationHit Hit = Collector.HitTest(View, ScreenCenter);
	EXPECT_EQ(Hit.Actor, Actor);
	EXPECT_EQ(Hit.Component, Actor->GetCameraComponent());
	EXPECT_EQ(Hit.Element, Element);
}

TEST(FLevelEditorViewportClientTests, ReusesOneVisualizationSnapshotAcrossInputAndRendering)
{
	InitializeDObjectSystem();
	int DrawCount = 0;
	auto& Registry = Durin::Editor::Level::FLevelEditorCustomizationRegistry::Get();
	FCustomizationGuard Guard{Registry.RegisterComponentVisualizer(
		Durin::DCameraComponent::StaticClass(), std::make_shared<FTestComponentVisualizer>(&DrawCount))};
	ASSERT_TRUE(Guard.Handle);
	Durin::DWorld* World = Durin::NewObject<Durin::DWorld>(nullptr, "ViewportSnapshotWorld");
	Durin::DLevel* Level = Durin::NewObject<Durin::DLevel>(World, "ViewportSnapshotLevel");
	ASSERT_TRUE(World->SetCurrentLevel(Level));
	Durin::ACameraActor* Actor = Level->SpawnActor<Durin::ACameraActor>("Camera");
	ASSERT_NE(Actor, nullptr);
	Durin::Editor::Level::FLevelEditorViewportClient Client;
	Client.InitializeForLevel(Level);
	Actor->GetCameraComponent()->SetWorldLocation(
		Client.GetCameraTransform().GetLocation() + Client.GetCameraTransform().GetForwardVector() * 5.0);

	Client.PrepareSceneView(Level, 800, 600);
	ASSERT_EQ(DrawCount, 1);
	Durin::FSceneView InteractionView;
	ASSERT_TRUE(Client.BuildViewMatrices(800, 600, InteractionView));
	Client.UpdateHoveredVisualizationWithView(Level, InteractionView, {400.0f, 300.0f});
	const Durin::Editor::Level::FViewportPickSubmission Pick = Client.SubmitViewportPick(Level, InteractionView, {400.0f, 300.0f});
	ASSERT_EQ(Pick.Completion.Status, Durin::Editor::Level::EViewportPickStatus::Completed);
	ASSERT_TRUE(Pick.Completion.Hit);
	EXPECT_EQ(Pick.Completion.Hit->Actor.Get(), Actor);
	EXPECT_EQ(DrawCount, 1);

	Client.PrepareSceneView(Level, 800, 600);
	EXPECT_EQ(DrawCount, 2);
	Durin::FSceneView RenderView;
	ASSERT_TRUE(Client.CalcSceneView(800, 600, RenderView));
	EXPECT_EQ(DrawCount, 2);
	Durin::FVector3 RayOrigin;
	Durin::FVector3 RayDirection;
	EXPECT_TRUE(Client.BuildPickingRay({400.0f, 300.0f}, {800.0f, 600.0f}, RayOrigin, RayDirection));
	Durin::FVector2f Projected;
	EXPECT_TRUE(Client.ProjectWorldToViewport(Actor->GetCameraComponent()->GetWorldLocation(), {800.0f, 600.0f}, Projected));
	EXPECT_EQ(DrawCount, 2);
}

TEST(FCameraComponentVisualizerTests, DrawsOnlyAnIconUntilSelected)
{
	InitializeDObjectSystem();
	Durin::DWorld* World = Durin::NewObject<Durin::DWorld>(nullptr, "CameraVisualizerWorld");
	Durin::DLevel* Level = Durin::NewObject<Durin::DLevel>(World, "CameraVisualizerLevel");
	ASSERT_TRUE(World->SetCurrentLevel(Level));
	Durin::ACameraActor* Actor = Level->SpawnActor<Durin::ACameraActor>("Camera");
	ASSERT_NE(Actor, nullptr);
	Durin::Editor::Level::FLevelEditorViewportClient Client;
	Actor->GetCameraComponent()->SetWorldLocation(Client.GetCameraTransform().GetLocation() + Client.GetCameraTransform().GetForwardVector() * 5.0);
	Durin::FSceneView View;
	ASSERT_TRUE(Client.CalcSceneView(800, 600, View));
	const std::shared_ptr<Durin::Editor::Level::IComponentEditorVisualizer> Visualizer = Durin::Editor::Level::CreateCameraComponentVisualizer();
	ASSERT_NE(Visualizer, nullptr);

	Durin::Editor::Level::FEditorVisualizationCollector Unselected;
	Visualizer->DrawVisualization(Actor->GetCameraComponent(), {View, Level, false, false}, Unselected);
	EXPECT_EQ(Unselected.GetIcons().size(), 1u);
	EXPECT_TRUE(Unselected.GetLines().empty());
	EXPECT_FLOAT_EQ(Unselected.GetIcons().front().SizePixels, Durin::MonaImGui::ScaleUI(36.0f));
	EXPECT_TRUE(Unselected.GetIcons().front().HoverColor.has_value());

	Actor->GetCameraComponent()->SetProjectionParameters(60.0f, 0.25f, 1.0f);
	Durin::Editor::Level::FEditorVisualizationCollector Selected;
	Visualizer->DrawVisualization(Actor->GetCameraComponent(), {View, Level, true, true}, Selected);
	EXPECT_EQ(Selected.GetIcons().size(), 1u);
	EXPECT_FLOAT_EQ(Selected.GetIcons().front().SizePixels, Durin::MonaImGui::ScaleUI(40.0f));
	EXPECT_FALSE(Selected.GetIcons().front().HoverColor.has_value());
	ASSERT_EQ(Selected.GetLines().size(), 13u);
	const Durin::FVector3 Forward = Actor->GetCameraComponent()->GetWorldRotation() * Durin::FVectorConstants::Forward;
	const Durin::FVector3 Origin = Actor->GetCameraComponent()->GetWorldLocation();
	EXPECT_NEAR(Durin::Math::Dot(Selected.GetLines()[0].Start - Origin, Forward), 0.25, 1.e-5);
	EXPECT_NEAR(Durin::Math::Dot(Selected.GetLines()[0].End - Origin, Forward), 0.25, 1.e-5);
	EXPECT_EQ(Selected.GetLines()[2].Pattern, Durin::ESimpleElementLinePattern::Solid);
	const ImVec4& ExpectedForwardColor = Durin::MonaImGui::GetThemeColor(Durin::MonaImGui::EUIThemeColor::AxisX);
	EXPECT_FLOAT_EQ(Selected.GetLines().back().Color.r, ExpectedForwardColor.x);
	EXPECT_FLOAT_EQ(Selected.GetLines().back().Color.g, ExpectedForwardColor.y);
	EXPECT_FLOAT_EQ(Selected.GetLines().back().Color.b, ExpectedForwardColor.z);
	EXPECT_FLOAT_EQ(Selected.GetLines().back().WidthPixels, 3.0f);
}

TEST(FCameraComponentVisualizerTests, UsesTheActualFarPlaneAtExtremeFieldOfView)
{
	InitializeDObjectSystem();
	Durin::DWorld* World = Durin::NewObject<Durin::DWorld>(nullptr, "TruncatedCameraVisualizerWorld");
	Durin::DLevel* Level = Durin::NewObject<Durin::DLevel>(World, "TruncatedCameraVisualizerLevel");
	ASSERT_TRUE(World->SetCurrentLevel(Level));
	Durin::ACameraActor* Actor = Level->SpawnActor<Durin::ACameraActor>("Camera");
	ASSERT_NE(Actor, nullptr);
	Durin::Editor::Level::FLevelEditorViewportClient Client;
	Actor->GetCameraComponent()->SetWorldLocation(Client.GetCameraTransform().GetLocation() + Client.GetCameraTransform().GetForwardVector() * 5.0);
	Actor->GetCameraComponent()->SetProjectionParameters(170.0f, 0.1f, 1000.0f);
	Durin::FSceneView View;
	ASSERT_TRUE(Client.CalcSceneView(800, 600, View));
	const std::shared_ptr<Durin::Editor::Level::IComponentEditorVisualizer> Visualizer = Durin::Editor::Level::CreateCameraComponentVisualizer();
	Durin::Editor::Level::FEditorVisualizationCollector Collector;
	Visualizer->DrawVisualization(Actor->GetCameraComponent(), {View, Level, true, true}, Collector);
	ASSERT_EQ(Collector.GetLines().size(), 13u);
	EXPECT_EQ(std::ranges::count_if(Collector.GetLines(), [](const Durin::Editor::Level::FEditorVisualizationLine& Line) {
		return Line.Pattern == Durin::ESimpleElementLinePattern::Dashed;
	}), 0);
	const Durin::FVector3 Forward = Actor->GetCameraComponent()->GetWorldRotation() * Durin::FVectorConstants::Forward;
	const Durin::FVector3 Origin = Actor->GetCameraComponent()->GetWorldLocation();
	EXPECT_NEAR(Durin::Math::Dot(Collector.GetLines()[2].Start - Origin, Forward), 1000.0, 1.e-4);
	EXPECT_NEAR(Durin::Math::Dot(Collector.GetLines()[2].End - Origin, Forward), 1000.0, 1.e-4);
	EXPECT_TRUE(std::ranges::all_of(Collector.GetLines(), [](const Durin::Editor::Level::FEditorVisualizationLine& Line) {
		return Durin::Math::IsFinite(Line.End - Line.Start)
			&& Durin::Math::Length(Line.End - Line.Start) > Durin::kSmallNumber;
	}));
}

TEST(FDirectionalLightComponentVisualizerTests, DrawsSelectableIconAndSelectedDirectionCue)
{
	InitializeDObjectSystem();
	Durin::DWorld* World = Durin::NewObject<Durin::DWorld>(nullptr, "DirectionalLightVisualizerWorld");
	Durin::DLevel* Level = Durin::NewObject<Durin::DLevel>(World, "DirectionalLightVisualizerLevel");
	ASSERT_TRUE(World->SetCurrentLevel(Level));
	auto* Actor = Level->SpawnActor<Durin::ADirectionalLightActor>("DirectionalLight");
	ASSERT_NE(Actor, nullptr);
	Durin::Editor::Level::FLevelEditorViewportClient Client;
	Actor->GetLightComponent()->SetWorldLocation(Client.GetCameraTransform().GetLocation() + Client.GetCameraTransform().GetForwardVector() * 10.0);
	Durin::FSceneView View;
	ASSERT_TRUE(Client.CalcSceneView(800, 600, View));
	const std::shared_ptr<Durin::Editor::Level::IComponentEditorVisualizer> Visualizer = Durin::Editor::Level::CreateDirectionalLightComponentVisualizer();
	ASSERT_NE(Visualizer, nullptr);

	Durin::Editor::Level::FEditorVisualizationCollector Unselected;
	Visualizer->DrawVisualization(Actor->GetLightComponent(), {View, Level, false, false}, Unselected);
	ASSERT_EQ(Unselected.GetIcons().size(), 1u);
	EXPECT_EQ(Unselected.GetIcons().front().Icon, Durin::Editor::Level::EEditorVisualizationIcon::DirectionalLight);
	EXPECT_TRUE(Unselected.GetLines().empty());

	Durin::Editor::Level::FEditorVisualizationCollector Selected;
	Visualizer->DrawVisualization(Actor->GetLightComponent(), {View, Level, true, true}, Selected);
	ASSERT_EQ(Selected.GetIcons().size(), 1u);
	ASSERT_EQ(Selected.GetLines().size(), 5u);
	EXPECT_TRUE(Selected.GetPrimitives().empty());
	const Durin::FVector3 Origin = Actor->GetLightComponent()->GetWorldLocation();
	const Durin::FVector3 Forward = Actor->GetLightComponent()->GetWorldRotation() * Durin::FVectorConstants::Forward;
	const Durin::Editor::Level::FEditorVisualizationLine& Shaft = Selected.GetLines().front();
	EXPECT_NEAR(Durin::Math::Length(Shaft.Start - Origin), 0.0, 1.e-6);
	EXPECT_GT(Durin::Math::Dot(Shaft.End - Shaft.Start, Forward), 0.0);
	EXPECT_FLOAT_EQ(Shaft.WidthPixels, Durin::MonaImGui::ScaleUI(2.0f));
	EXPECT_TRUE(std::ranges::all_of(Selected.GetLines(), [](const auto& Line) {
		return Durin::Math::IsFinite(Line.End - Line.Start)
			&& Durin::Math::Length(Line.End - Line.Start) > Durin::kSmallNumber;
	}));
	EXPECT_TRUE(Selected.GetIcons().front().bDepthIndependentHit);
}

TEST(FPlayerStartActorVisualizerTests, DrawsSelectableSpawnShapeAndFacingCue)
{
	InitializeDObjectSystem();
	Durin::DWorld* World = Durin::NewObject<Durin::DWorld>(nullptr, "PlayerStartVisualizerWorld");
	Durin::DLevel* Level = Durin::NewObject<Durin::DLevel>(World, "PlayerStartVisualizerLevel");
	ASSERT_TRUE(World->SetCurrentLevel(Level));
	auto* PlayerStart = Level->SpawnActor<Durin::APlayerStart>("PlayerStart");
	ASSERT_NE(PlayerStart, nullptr);
	Durin::Editor::Level::FLevelEditorViewportClient Client;
	const Durin::FVector3 Origin = Client.GetCameraTransform().GetLocation()
		+ Client.GetCameraTransform().GetForwardVector() * 5.0;
	PlayerStart->GetRootComponent()->SetWorldLocation(Origin);
	Durin::FSceneView View;
	ASSERT_TRUE(Client.CalcSceneView(800, 600, View));
	const std::shared_ptr<Durin::Editor::Level::IActorEditorVisualizer> Visualizer =
		Durin::Editor::Level::CreatePlayerStartActorVisualizer();
	ASSERT_NE(Visualizer, nullptr);

	Durin::Editor::Level::FEditorVisualizationCollector Unselected;
	Visualizer->DrawVisualization(PlayerStart, {View, Level, false, false}, Unselected);
	ASSERT_EQ(Unselected.GetIcons().size(), 1u);
	EXPECT_EQ(Unselected.GetIcons().front().Icon, Durin::Editor::Level::EEditorVisualizationIcon::PlayerStart);
	EXPECT_FLOAT_EQ(Unselected.GetIcons().front().SizePixels, Durin::MonaImGui::ScaleUI(36.0f));
	EXPECT_TRUE(Unselected.GetIcons().front().HoverColor.has_value());
	EXPECT_TRUE(Unselected.GetLines().empty());
	EXPECT_TRUE(Unselected.GetBoxes().empty());
	Durin::FVector2f ScreenCenter;
	ASSERT_TRUE(Durin::SceneViewProjection::ProjectWorldToViewport(View, Origin, ScreenCenter));
	const Durin::Editor::Level::FEditorVisualizationHit Hit = Unselected.HitTest(View, ScreenCenter);
	EXPECT_EQ(Hit.Actor, PlayerStart);
	EXPECT_EQ(Hit.Component, PlayerStart->GetRootComponent());
	EXPECT_TRUE(Hit.bDepthIndependent);

	Durin::Editor::Level::FEditorVisualizationCollector Selected;
	Visualizer->DrawVisualization(PlayerStart, {View, Level, true, true}, Selected);
	ASSERT_EQ(Selected.GetIcons().size(), 1u);
	EXPECT_FLOAT_EQ(Selected.GetIcons().front().SizePixels, Durin::MonaImGui::ScaleUI(40.0f));
	EXPECT_FALSE(Selected.GetIcons().front().HoverColor.has_value());
	EXPECT_EQ(Selected.GetLines().size(), 105u);
	EXPECT_FLOAT_EQ(Selected.GetLines().front().WidthPixels, Durin::MonaImGui::ScaleUI(2.0f));
	EXPECT_TRUE(Selected.GetPrimitives().empty());
	const Durin::Editor::Level::FEditorVisualizationLine& ArrowShaft = Selected.GetLines()[100];
	const Durin::FVector3 Forward = PlayerStart->GetRootComponent()->GetWorldRotation()
		* Durin::FVectorConstants::Forward;
	EXPECT_NEAR(Durin::Math::Length(ArrowShaft.Start - (Origin + Forward * 0.4)), 0.0, 1.e-6);
	EXPECT_GT(Durin::Math::Dot(ArrowShaft.End - ArrowShaft.Start, Forward), 0.0);

	Durin::MarkObjectHierarchyAsGarbage(World);
	Durin::CollectGarbage();
}
