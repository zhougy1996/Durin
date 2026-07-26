#include "ViewportTestSupport.h"

TEST(FSplineComponentVisualizerTests, EmitsSelectableCurveAndControlPointLines)
{
	InitializeDObjectSystem();
	auto* Actor = Durin::NewObject<Durin::AActor>(nullptr, "SplineActor");
	auto* Spline = Durin::Cast<Durin::DSplineComponent>(Actor->AddInstanceComponent(Durin::DSplineComponent::StaticClass(), "Spline"));
	ASSERT_NE(Spline, nullptr);
	Durin::FSceneView View;
	View.ViewportWidth = 800;
	View.ViewportHeight = 600;
	Durin::FEditorVisualizationCollector Collector;
	const std::shared_ptr<Durin::IComponentEditorVisualizer> Visualizer = Durin::CreateSplineComponentVisualizer();
	Visualizer->DrawVisualization(Spline, {View, nullptr, true, true}, Collector);

	ASSERT_FALSE(Collector.GetLines().empty());
	EXPECT_TRUE(std::ranges::all_of(Collector.GetLines(), [Actor, Spline](const Durin::FEditorVisualizationLine& Line) {
		return Line.Actor.Get() == Actor && Line.Component.Get() == Spline;
	}));
	EXPECT_GT(Collector.GetLines().size(), static_cast<size_t>(Spline->GetReparamStepsPerSegment()));

	Durin::MarkObjectHierarchyAsGarbage(Actor);
	Durin::CollectGarbage();
}

TEST(FLevelEditorCustomizationRegistryTests, RejectsDuplicatesFindsBaseClassAndUnregisters)
{
	InitializeDObjectSystem();
	auto& Registry = Durin::FLevelEditorCustomizationRegistry::Get();
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

	Durin::FObjectPropertyViewBuilder Builder("rotation");
	Builder.AddProperty(RootComponent, TransformProperty, 0, {.Label = "Transform"}, "Location Rotation Scale");
	Builder.AddCustomRow("Materials Material Slots", [](Durin::FReflectedPropertyView&, const Durin::FReflectedPropertyViewContext&) { return false; });
	Builder.HideProperty(TransformProperty);
	Builder.ReplaceDefaultProperties();

	EXPECT_EQ(Builder.GetVisibleRowCount(), 1u);
	EXPECT_TRUE(Builder.IsPropertyHidden(*TransformProperty));
	EXPECT_TRUE(Builder.IsReplacingDefaultProperties());
	Durin::MarkObjectHierarchyAsGarbage(RootComponent);
	Durin::CollectGarbage();
}

TEST(FObjectPropertyViewCustomizationTests, DeclaresActorTransformRow)
{
	InitializeDObjectSystem();
	Durin::DWorld* World = Durin::NewObject<Durin::DWorld>(nullptr, "PropertyCustomizationWorld");
	Durin::DLevel* Level = Durin::NewObject<Durin::DLevel>(World, "PropertyCustomizationLevel");
	ASSERT_TRUE(World->SetCurrentLevel(Level));
	auto* Actor = Level->SpawnActor<Durin::AStaticMeshActor>("StaticMesh");
	ASSERT_NE(Actor, nullptr);
	Durin::DStaticMeshComponent* Component = Actor->GetStaticMeshComponent();
	Component->SetStaticMesh(Durin::DStaticMesh::CreateDebugTriangle(Level));

	Durin::FLevelEditorContext Context;
	Durin::FObjectPropertyViewBuilder ActorBuilder("scale");
	Durin::CreateActorDetailsCustomization()->CustomizeDetails(Context, Actor, ActorBuilder);
	EXPECT_EQ(ActorBuilder.GetVisibleRowCount(), 1u);

	EXPECT_EQ(Component->GetClass()->FindPropertyByName("Material"), nullptr);
	EXPECT_EQ(Component->GetClass()->FindPropertyByName("Materials"), nullptr);
	EXPECT_EQ(Component->GetClass()->FindPropertyByName("MaterialOverridesVersion"), nullptr);
	Durin::FProperty* OverridesProperty = Component->GetClass()->FindPropertyByName("MaterialOverrides");
	ASSERT_NE(OverridesProperty, nullptr);
	EXPECT_FALSE(OverridesProperty->HasAnyPropertyFlags(Durin::EPropertyFlags::Edit));

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
	Durin::FLevelEditorViewportClient Client;
	Durin::FSceneView View;
	ASSERT_TRUE(Client.CalcSceneView(800, 600, View));
	const Durin::FVector3 Center = Client.GetCameraTransform().GetLocation() + Client.GetCameraTransform().GetForwardVector() * 5.0;
	Durin::FEditorVisualizationCollector Collector;
	Collector.AddLine({Center - Client.GetCameraTransform().GetRightVector(), Center + Client.GetCameraTransform().GetRightVector(), {1.0f, 0.5f, 0.25f, 1.0f}, 3.0f, 7.0f, 4, Actor, Actor->GetCameraComponent()});
	Collector.AppendToView(View);
	ASSERT_EQ(View.OverlayLines.size(), 1u);
	EXPECT_FLOAT_EQ(View.OverlayLines.front().WidthPixels, 3.0f);
	Durin::FVector2f ScreenCenter;
	ASSERT_TRUE(Durin::SceneViewProjection::ProjectWorldToViewport(View, Center, ScreenCenter));
	const Durin::FEditorVisualizationHit Hit = Collector.HitTest(View, ScreenCenter);
	EXPECT_EQ(Hit.Actor, Actor);
	EXPECT_EQ(Hit.Component, Actor->GetCameraComponent());
	EXPECT_EQ(Collector.HitTest(View, ScreenCenter + Durin::FVector2f(0.0f, 50.0f)).Actor, nullptr);
}

TEST(FLevelEditorViewportClientTests, PublishesWorldGridStateToTheSceneView)
{
	Durin::FLevelEditorViewportClient Client;
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
	Durin::FLevelEditorViewportClient Client;
	Durin::FSceneView View;
	ASSERT_TRUE(Client.CalcSceneView(800, 600, View));
	const Durin::FVector3 Center = Client.GetCameraTransform().GetLocation() + Client.GetCameraTransform().GetForwardVector() * 5.0;
	Durin::FEditorVisualizationCollector Collector;
	Collector.AddIcon({Durin::EViewOverlayIcon::Camera, Center, Durin::FVector4f(1.0f), 30.0f, 3.0f, 100, Actor, Actor->GetCameraComponent(), true});
	Collector.AppendToView(View);
	ASSERT_EQ(View.OverlayIcons.size(), 1u);
	EXPECT_FLOAT_EQ(View.OverlayIcons.front().SizePixels, 30.0f);
	Durin::FVector2f ScreenCenter;
	ASSERT_TRUE(Durin::SceneViewProjection::ProjectWorldToViewport(View, Center, ScreenCenter));
	const Durin::FEditorVisualizationHit Hit = Collector.HitTest(View, ScreenCenter + Durin::FVector2f(14.0f, 0.0f));
	EXPECT_EQ(Hit.Actor, Actor);
	EXPECT_TRUE(Hit.bDepthIndependent);
	EXPECT_EQ(Collector.HitTest(View, ScreenCenter + Durin::FVector2f(24.0f, 0.0f)).Actor, nullptr);
}

TEST(FEditorVisualizationCollectorTests, AppliesOptionalHoverColorWithoutRegeneratingPrimitives)
{
	InitializeDObjectSystem();
	Durin::DWorld* World = Durin::NewObject<Durin::DWorld>(nullptr, "HoverColorWorld");
	Durin::DLevel* Level = Durin::NewObject<Durin::DLevel>(World, "HoverColorLevel");
	ASSERT_TRUE(World->SetCurrentLevel(Level));
	Durin::ACameraActor* Actor = Level->SpawnActor<Durin::ACameraActor>("Camera");
	ASSERT_NE(Actor, nullptr);
	Durin::FEditorVisualizationCollector Collector;
	const Durin::FVector4f BaseColor{1.0f, 0.0f, 0.0f, 1.0f};
	const Durin::FVector4f HoverColor{0.0f, 1.0f, 0.0f, 1.0f};
	Collector.AddIcon({Durin::EViewOverlayIcon::Camera, {0.0, 0.0, 5.0}, BaseColor, 30.0f, 3.0f, 100,
		Actor, Actor->GetCameraComponent(), true, HoverColor});

	Durin::FSceneView BaseView;
	Collector.AppendToView(BaseView);
	Durin::FSceneView HoveredView;
	Collector.AppendToView(HoveredView, Actor);
	ASSERT_EQ(BaseView.OverlayIcons.size(), 1u);
	ASSERT_EQ(HoveredView.OverlayIcons.size(), 1u);
	EXPECT_EQ(BaseView.OverlayIcons.front().Color, BaseColor);
	EXPECT_EQ(HoveredView.OverlayIcons.front().Color, HoverColor);
	EXPECT_EQ(Collector.GetIcons().front().Color, BaseColor);

	Durin::MarkObjectHierarchyAsGarbage(Actor);
	Durin::CollectGarbage();
	Durin::FSceneView ExpiredView;
	Collector.AppendToView(ExpiredView);
	EXPECT_TRUE(ExpiredView.OverlayIcons.empty());
	EXPECT_EQ(Collector.HitTest(HoveredView, {0.0f, 0.0f}).Actor, nullptr);
}

TEST(FLevelEditorViewportClientTests, ReusesOneVisualizationSnapshotAcrossInputAndRendering)
{
	InitializeDObjectSystem();
	int DrawCount = 0;
	auto& Registry = Durin::FLevelEditorCustomizationRegistry::Get();
	FCustomizationGuard Guard{Registry.RegisterComponentVisualizer(
		Durin::DCameraComponent::StaticClass(), std::make_shared<FTestComponentVisualizer>(&DrawCount))};
	ASSERT_TRUE(Guard.Handle);
	Durin::DWorld* World = Durin::NewObject<Durin::DWorld>(nullptr, "ViewportSnapshotWorld");
	Durin::DLevel* Level = Durin::NewObject<Durin::DLevel>(World, "ViewportSnapshotLevel");
	ASSERT_TRUE(World->SetCurrentLevel(Level));
	Durin::ACameraActor* Actor = Level->SpawnActor<Durin::ACameraActor>("Camera");
	ASSERT_NE(Actor, nullptr);
	Durin::FLevelEditorViewportClient Client;
	Client.InitializeForLevel(Level);
	Actor->GetCameraComponent()->SetWorldLocation(
		Client.GetCameraTransform().GetLocation() + Client.GetCameraTransform().GetForwardVector() * 5.0);

	Client.PrepareSceneView(Level, 800, 600);
	ASSERT_EQ(DrawCount, 1);
	Durin::FSceneView InteractionView;
	ASSERT_TRUE(Client.BuildViewMatrices(800, 600, InteractionView));
	Client.UpdateHoveredVisualizationWithView(Level, InteractionView, {400.0f, 300.0f});
	EXPECT_EQ(Client.PickActorWithView(Level, InteractionView, {400.0f, 300.0f}), Actor);
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
	Durin::FLevelEditorViewportClient Client;
	Actor->GetCameraComponent()->SetWorldLocation(Client.GetCameraTransform().GetLocation() + Client.GetCameraTransform().GetForwardVector() * 5.0);
	Durin::FSceneView View;
	ASSERT_TRUE(Client.CalcSceneView(800, 600, View));
	const std::shared_ptr<Durin::IComponentEditorVisualizer> Visualizer = Durin::CreateCameraComponentVisualizer();
	ASSERT_NE(Visualizer, nullptr);

	Durin::FEditorVisualizationCollector Unselected;
	Visualizer->DrawVisualization(Actor->GetCameraComponent(), {View, Level, false, false}, Unselected);
	EXPECT_EQ(Unselected.GetIcons().size(), 1u);
	EXPECT_TRUE(Unselected.GetLines().empty());
	EXPECT_FLOAT_EQ(Unselected.GetIcons().front().SizePixels, Durin::MonaImGui::ScaleUI(36.0f));
	EXPECT_TRUE(Unselected.GetIcons().front().HoverColor.has_value());

	Actor->GetCameraComponent()->SetProjectionParameters(60.0f, 0.25f, 1.0f);
	Durin::FEditorVisualizationCollector Selected;
	Visualizer->DrawVisualization(Actor->GetCameraComponent(), {View, Level, true, true}, Selected);
	EXPECT_EQ(Selected.GetIcons().size(), 1u);
	EXPECT_FLOAT_EQ(Selected.GetIcons().front().SizePixels, Durin::MonaImGui::ScaleUI(40.0f));
	EXPECT_FALSE(Selected.GetIcons().front().HoverColor.has_value());
	ASSERT_EQ(Selected.GetLines().size(), 13u);
	const Durin::FVector3 Forward = Actor->GetCameraComponent()->GetWorldRotation() * Durin::FVectorConstants::Forward;
	const Durin::FVector3 Origin = Actor->GetCameraComponent()->GetWorldLocation();
	EXPECT_NEAR(glm::dot(Selected.GetLines()[0].Start - Origin, Forward), 0.25, 1.e-5);
	EXPECT_NEAR(glm::dot(Selected.GetLines()[0].End - Origin, Forward), 0.25, 1.e-5);
	EXPECT_EQ(Selected.GetLines()[2].Pattern, Durin::EViewOverlayLinePattern::Solid);
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
	Durin::FLevelEditorViewportClient Client;
	Actor->GetCameraComponent()->SetWorldLocation(Client.GetCameraTransform().GetLocation() + Client.GetCameraTransform().GetForwardVector() * 5.0);
	Actor->GetCameraComponent()->SetProjectionParameters(170.0f, 0.1f, 1000.0f);
	Durin::FSceneView View;
	ASSERT_TRUE(Client.CalcSceneView(800, 600, View));
	const std::shared_ptr<Durin::IComponentEditorVisualizer> Visualizer = Durin::CreateCameraComponentVisualizer();
	Durin::FEditorVisualizationCollector Collector;
	Visualizer->DrawVisualization(Actor->GetCameraComponent(), {View, Level, true, true}, Collector);
	ASSERT_EQ(Collector.GetLines().size(), 13u);
	EXPECT_EQ(std::ranges::count_if(Collector.GetLines(), [](const Durin::FEditorVisualizationLine& Line) {
		return Line.Pattern == Durin::EViewOverlayLinePattern::Dashed;
	}), 0);
	const Durin::FVector3 Forward = Actor->GetCameraComponent()->GetWorldRotation() * Durin::FVectorConstants::Forward;
	const Durin::FVector3 Origin = Actor->GetCameraComponent()->GetWorldLocation();
	EXPECT_NEAR(glm::dot(Collector.GetLines()[2].Start - Origin, Forward), 1000.0, 1.e-4);
	EXPECT_NEAR(glm::dot(Collector.GetLines()[2].End - Origin, Forward), 1000.0, 1.e-4);
	EXPECT_TRUE(std::ranges::all_of(Collector.GetLines(), [](const Durin::FEditorVisualizationLine& Line) {
		return std::isfinite(glm::length(Line.End - Line.Start)) && glm::length(Line.End - Line.Start) > Durin::kSmallNumber;
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
	Durin::FLevelEditorViewportClient Client;
	Actor->GetLightComponent()->SetWorldLocation(Client.GetCameraTransform().GetLocation() + Client.GetCameraTransform().GetForwardVector() * 10.0);
	Durin::FSceneView View;
	ASSERT_TRUE(Client.CalcSceneView(800, 600, View));
	const std::shared_ptr<Durin::IComponentEditorVisualizer> Visualizer = Durin::CreateDirectionalLightComponentVisualizer();
	ASSERT_NE(Visualizer, nullptr);

	Durin::FEditorVisualizationCollector Unselected;
	Visualizer->DrawVisualization(Actor->GetLightComponent(), {View, Level, false, false}, Unselected);
	ASSERT_EQ(Unselected.GetIcons().size(), 1u);
	EXPECT_EQ(Unselected.GetIcons().front().Icon, Durin::EViewOverlayIcon::DirectionalLight);
	EXPECT_TRUE(Unselected.GetLines().empty());

	Durin::FEditorVisualizationCollector Selected;
	Visualizer->DrawVisualization(Actor->GetLightComponent(), {View, Level, true, true}, Selected);
	ASSERT_EQ(Selected.GetIcons().size(), 1u);
	ASSERT_EQ(Selected.GetLines().size(), 5u);
	const Durin::FVector3 Origin = Actor->GetLightComponent()->GetWorldLocation();
	const Durin::FVector3 Forward = Actor->GetLightComponent()->GetWorldRotation() * Durin::FVectorConstants::Forward;
	EXPECT_NEAR(glm::length(Selected.GetLines().front().Start - Origin), 0.0, 1.e-6);
	EXPECT_GT(glm::dot(Selected.GetLines().front().End - Origin, Forward), 0.0);
	EXPECT_TRUE(Selected.GetIcons().front().bDepthIndependentHit);
}
