#include "Panels/DetailsPanelTargeting.h"

#include "Components/ActorComponent.h"
#include "Components/SceneComponent.h"
#include "DObject/ObjectLifecycle.h"
#include "Engine/Actor.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "Workspace/LevelEditorContext.h"
#include "EngineTestSupport.h"

#include <gtest/gtest.h>

TEST(FDetailsPanelTargetingTests, DefaultsToRootComponentAndPreservesRootlessActorState)
{
	InitializeDObjectSystem();
	auto* RootlessActor = Durin::NewObject<Durin::AActor>(nullptr, "RootlessDetailsActor");
	ASSERT_NE(RootlessActor, nullptr);
	auto* LogicComponent = RootlessActor->AddInstanceComponent(Durin::DActorComponent::StaticClass(), "Logic");
	ASSERT_NE(LogicComponent, nullptr);
	ASSERT_EQ(RootlessActor->GetRootComponent(), nullptr);
	const size_t RootlessComponentCount = RootlessActor->GetComponents().size();

	EXPECT_EQ(Durin::Editor::Level::DetailsPanelTargeting::ResolveDefaultComponent(RootlessActor), nullptr);
	EXPECT_EQ(RootlessActor->GetRootComponent(), nullptr);
	EXPECT_EQ(RootlessActor->GetComponents().size(), RootlessComponentCount);

	auto* RootComponent = Durin::Cast<Durin::DSceneComponent>(
		RootlessActor->AddInstanceComponent(Durin::DSceneComponent::StaticClass(), "Root"));
	ASSERT_NE(RootComponent, nullptr);
	EXPECT_EQ(RootlessActor->GetRootComponent(), RootComponent);
	EXPECT_EQ(Durin::Editor::Level::DetailsPanelTargeting::ResolveDefaultComponent(RootlessActor), RootComponent);
	ASSERT_TRUE(RootlessActor->DestroyInstanceComponent(RootComponent));
	EXPECT_EQ(RootlessActor->GetRootComponent(), nullptr);
	EXPECT_FALSE(RootlessActor->OwnsComponent(RootComponent));
	EXPECT_EQ(Durin::Editor::Level::DetailsPanelTargeting::ResolveSelectedComponent(RootlessActor, RootComponent), nullptr);

	Durin::MarkObjectHierarchyAsGarbage(RootlessActor);
	Durin::CollectGarbage();
}

TEST(FDetailsPanelTargetingTests, PreservesExplicitActorSelectionAndRecoversInvalidComponents)
{
	InitializeDObjectSystem();
	auto* RootedActor = Durin::NewObject<Durin::AActor>(nullptr, "RootedDetailsActor");
	auto* RootComponent = Durin::Cast<Durin::DSceneComponent>(
		RootedActor->AddInstanceComponent(Durin::DSceneComponent::StaticClass(), "Root"));
	ASSERT_NE(RootComponent, nullptr);
	auto* OwnedComponent = RootedActor->AddInstanceComponent(Durin::DActorComponent::StaticClass(), "Owned");
	ASSERT_NE(OwnedComponent, nullptr);

	auto* OtherActor = Durin::NewObject<Durin::AActor>(nullptr, "OtherDetailsActor");
	auto* ForeignComponent = OtherActor->AddInstanceComponent(Durin::DActorComponent::StaticClass(), "Foreign");
	ASSERT_NE(ForeignComponent, nullptr);

	EXPECT_EQ(Durin::Editor::Level::DetailsPanelTargeting::ResolveSelectedComponent(RootedActor, nullptr), nullptr);
	EXPECT_EQ(Durin::Editor::Level::DetailsPanelTargeting::ResolveSelectedComponent(RootedActor, OwnedComponent), OwnedComponent);
	EXPECT_EQ(Durin::Editor::Level::DetailsPanelTargeting::ResolveSelectedComponent(RootedActor, ForeignComponent), RootComponent);
	EXPECT_EQ(Durin::Editor::Level::DetailsPanelTargeting::ResolveSelectedComponent(OtherActor, RootComponent), nullptr);

	Durin::MarkObjectHierarchyAsGarbage(RootedActor);
	Durin::MarkObjectHierarchyAsGarbage(OtherActor);
	Durin::CollectGarbage();
}

TEST(FLevelEditorContextSelectionTests, ActorSelectionDefaultsToRootComponentAndPreservesExplicitActorTarget)
{
	InitializeDObjectSystem();
	auto* World = Durin::NewObject<Durin::DWorld>(nullptr, "SharedSelectionWorld");
	auto* Level = Durin::NewObject<Durin::DLevel>(World, "SharedSelectionLevel");
	ASSERT_TRUE(World->SetCurrentLevel(Level));
	auto* Actor = Level->SpawnActor<Durin::AActor>("SelectedActor");
	ASSERT_NE(Actor, nullptr);
	auto* RootComponent = Durin::Cast<Durin::DSceneComponent>(
		Actor->AddInstanceComponent(Durin::DSceneComponent::StaticClass(), "Root"));
	ASSERT_NE(RootComponent, nullptr);
	Durin::Editor::Level::FLevelEditorContext Context;
	Context.Synchronize(World);
	Context.SelectActor(Actor);
	EXPECT_EQ(Context.GetSelectedComponent(), RootComponent);
	Context.SelectComponent(nullptr);
	EXPECT_EQ(Context.GetSelectedComponent(), nullptr);
	Context.SelectActor(Actor);
	EXPECT_EQ(Context.GetSelectedComponent(), RootComponent);

	auto* RootlessActor = Level->SpawnActor<Durin::AActor>("RootlessSelectedActor");
	ASSERT_NE(RootlessActor, nullptr);
	Context.SelectActor(RootlessActor);
	EXPECT_EQ(Context.GetSelectedComponent(), nullptr);
}

TEST(FLevelEditorContextSelectionTests, SharesComponentAndRepairsTypedSubElementSelection)
{
	InitializeDObjectSystem();
	auto* World = Durin::NewObject<Durin::DWorld>(nullptr, "SharedSelectionWorld");
	auto* Level = Durin::NewObject<Durin::DLevel>(World, "SharedSelectionLevel");
	ASSERT_TRUE(World->SetCurrentLevel(Level));
	auto* Actor = Level->SpawnActor<Durin::AActor>("SelectedActor");
	ASSERT_NE(Actor, nullptr);
	auto* Component = Actor->AddInstanceComponent(Durin::DActorComponent::StaticClass(), "SelectedComponent");
	ASSERT_NE(Component, nullptr);
	Durin::Editor::Level::FLevelEditorContext Context;
	Context.Synchronize(World);
	Context.SelectActor(Actor);
	const Durin::Editor::Level::FEditorSubElementSelection Element{Durin::Editor::Level::EEditorSubElementKind::Point, Durin::FGuid::NewGuid()};
	Context.SelectSubElement(Component, Element);
	EXPECT_EQ(Context.GetSelectedComponent(), Component);
	EXPECT_EQ(Context.GetSelectedSubElement(), Element);
	ASSERT_TRUE(Actor->DestroyInstanceComponent(Component));
	Context.Synchronize(World);
	EXPECT_EQ(Context.GetSelectedComponent(), nullptr);
	EXPECT_FALSE(Context.GetSelectedSubElement().IsValid());
}
