#include "Panels/DetailsPanelTargeting.h"

#include "Components/ActorComponent.h"
#include "Components/SceneComponent.h"
#include "DObject/ObjectLifecycle.h"
#include "Engine/Actor.h"
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
	const size_t RootlessComponentCount = RootlessActor->GetOwnedComponents().size();

	EXPECT_EQ(Durin::DetailsPanelTargeting::ResolveDefaultComponent(RootlessActor), nullptr);
	EXPECT_EQ(RootlessActor->GetRootComponent(), nullptr);
	EXPECT_EQ(RootlessActor->GetOwnedComponents().size(), RootlessComponentCount);

	auto* RootComponent = Durin::Cast<Durin::DSceneComponent>(
		RootlessActor->AddInstanceComponent(Durin::DSceneComponent::StaticClass(), "Root"));
	ASSERT_NE(RootComponent, nullptr);
	EXPECT_EQ(RootlessActor->GetRootComponent(), RootComponent);
	EXPECT_EQ(Durin::DetailsPanelTargeting::ResolveDefaultComponent(RootlessActor), RootComponent);

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

	EXPECT_EQ(Durin::DetailsPanelTargeting::ResolveSelectedComponent(RootedActor, nullptr), nullptr);
	EXPECT_EQ(Durin::DetailsPanelTargeting::ResolveSelectedComponent(RootedActor, OwnedComponent), OwnedComponent);
	EXPECT_EQ(Durin::DetailsPanelTargeting::ResolveSelectedComponent(RootedActor, ForeignComponent), RootComponent);
	EXPECT_EQ(Durin::DetailsPanelTargeting::ResolveSelectedComponent(OtherActor, RootComponent), nullptr);

	Durin::MarkObjectHierarchyAsGarbage(RootedActor);
	Durin::MarkObjectHierarchyAsGarbage(OtherActor);
	Durin::CollectGarbage();
}
