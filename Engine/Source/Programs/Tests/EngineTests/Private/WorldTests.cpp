#include "Actors/CameraActor.h"
#include "Actors/StaticMeshActor.h"
#include "Components/ActorComponent.h"
#include "Components/CameraComponent.h"
#include "Components/StaticMeshComponent.h"
#include "DObject/DObjectGlobals.h"
#include "DObject/ObjectLifecycle.h"
#include "Engine/Engine.h"
#include "Engine/GameEngine.h"
#include "Engine/World.h"

#include <gtest/gtest.h>

namespace
{
	auto InitializeDObjectSystem() -> void
	{
		static const bool bInitialized = []() {
			Durin::DObjectInit();
			return true;
		}();
		(void)bInitialized;
	}

	auto CreateWorld(Durin::DObject* Outer = nullptr) -> Durin::DWorld*
	{
		InitializeDObjectSystem();
		return Durin::NewObject<Durin::DWorld>(Outer, "TestWorld");
	}
} // namespace

TEST(FWorldTests, SpawnsEnumeratesAndFindsActors)
{
	Durin::DWorld* World = CreateWorld();
	Durin::ACameraActor* Camera = World->SpawnActor<Durin::ACameraActor>("Camera");
	Durin::AStaticMeshActor* Mesh = World->SpawnActor<Durin::AStaticMeshActor>("Mesh");

	ASSERT_NE(Camera, nullptr);
	ASSERT_NE(Mesh, nullptr);
	ASSERT_EQ(Camera->GetClass(), Durin::ACameraActor::StaticClass());
	ASSERT_EQ(Mesh->GetClass(), Durin::AStaticMeshActor::StaticClass());
	ASSERT_FALSE(Camera->GetClass()->GetFName().IsNone());
	ASSERT_FALSE(Mesh->GetClass()->GetFName().IsNone());
	EXPECT_EQ(Camera->GetClass()->GetName(), "Durin::ACameraActor");
	EXPECT_EQ(Mesh->GetClass()->GetName(), "Durin::AStaticMeshActor");
	EXPECT_EQ(World->GetActors().size(), 2u);
	EXPECT_TRUE(World->ContainsActor(Camera));
	EXPECT_EQ(World->FindActorByName("Camera"), Camera);
	EXPECT_EQ(Camera->GetOuter(), World);
	EXPECT_EQ(Mesh->GetOuter(), World);
	EXPECT_EQ(Camera->GetCameraComponent()->GetOuter(), Camera);

	Durin::DestroyObject(World);
}

TEST(FWorldTests, MakesDuplicateActorNamesUnique)
{
	Durin::DWorld* World = CreateWorld();
	Durin::ACameraActor* First = World->SpawnActor<Durin::ACameraActor>("Camera");
	Durin::ACameraActor* Second = World->SpawnActor<Durin::ACameraActor>("Camera");
	Durin::ACameraActor* Third = World->SpawnActor<Durin::ACameraActor>("Camera");

	EXPECT_EQ(First->GetName(), "Camera");
	EXPECT_EQ(Second->GetName(), "Camera_2");
	EXPECT_EQ(Third->GetName(), "Camera_3");

	Durin::DestroyObject(World);
}

TEST(FWorldTests, BuiltInActorsOwnTheirDefaultComponents)
{
	Durin::DWorld* World = CreateWorld();
	Durin::ACameraActor* Camera = World->SpawnActor<Durin::ACameraActor>("Camera");
	Durin::AStaticMeshActor* Mesh = World->SpawnActor<Durin::AStaticMeshActor>("Mesh");
	Durin::DCameraComponent* CameraComponent = Camera->GetCameraComponent();
	Durin::DStaticMeshComponent* MeshComponent = Mesh->GetStaticMeshComponent();

	ASSERT_EQ(Camera->GetOwnedComponents().size(), 1u);
	EXPECT_EQ(Camera->GetOwnedComponents().front().Get(), CameraComponent);
	EXPECT_EQ(Camera->FindComponentByClass<Durin::DCameraComponent>(), CameraComponent);
	EXPECT_EQ(Camera->GetRootComponent(), CameraComponent);
	EXPECT_EQ(CameraComponent->GetOwner(), Camera);
	EXPECT_EQ(CameraComponent->GetOuter(), Camera);

	ASSERT_EQ(Mesh->GetOwnedComponents().size(), 1u);
	EXPECT_EQ(Mesh->GetOwnedComponents().front().Get(), MeshComponent);
	EXPECT_EQ(Mesh->FindComponentByClass<Durin::DStaticMeshComponent>(), MeshComponent);
	EXPECT_EQ(Mesh->GetRootComponent(), MeshComponent);
	EXPECT_EQ(MeshComponent->GetOwner(), Mesh);
	EXPECT_EQ(MeshComponent->GetOuter(), Mesh);

	Durin::DestroyObject(World);
}

TEST(FWorldTests, DestroyActorRemovesItFromTheWorld)
{
	Durin::DWorld* World = CreateWorld();
	Durin::ACameraActor* Camera = World->SpawnActor<Durin::ACameraActor>("Camera");
	Durin::TObjectPtr<Durin::AActor> Selection = Camera;

	EXPECT_TRUE(World->DestroyActor(Camera));
	EXPECT_TRUE(World->GetActors().empty());
	EXPECT_EQ(World->FindActorByName("Camera"), nullptr);
	EXPECT_FALSE(World->ContainsActor(Selection.Get()));
	Selection = nullptr;
	EXPECT_EQ(Selection.Get(), nullptr);
	EXPECT_FALSE(World->DestroyActor(nullptr));

	Durin::DestroyObject(World);
}

TEST(FWorldTests, DestroyAllActorsInvalidatesObjectPointers)
{
	Durin::DWorld* World = CreateWorld();
	Durin::TObjectPtr<Durin::AActor> Camera = World->SpawnActor<Durin::ACameraActor>("Camera");
	Durin::TObjectPtr<Durin::AActor> Mesh = World->SpawnActor<Durin::AStaticMeshActor>("Mesh");
	Durin::TObjectPtr<Durin::DActorComponent> CameraComponent = Camera->FindComponentByClass<Durin::DCameraComponent>();
	Durin::TObjectPtr<Durin::DActorComponent> MeshComponent = Mesh->FindComponentByClass<Durin::DStaticMeshComponent>();

	World->DestroyAllActors();

	EXPECT_TRUE(World->GetActors().empty());
	EXPECT_EQ(Camera.Get(), nullptr);
	EXPECT_EQ(Mesh.Get(), nullptr);
	EXPECT_EQ(CameraComponent.Get(), nullptr);
	EXPECT_EQ(MeshComponent.Get(), nullptr);

	Durin::DestroyObject(World);
}

TEST(FWorldTests, DestroyingWorldCascadesToActorsAndComponents)
{
	Durin::DWorld* World = CreateWorld();
	Durin::ACameraActor* Camera = World->SpawnActor<Durin::ACameraActor>("Camera");
	Durin::TObjectPtr<Durin::DWorld> WorldPtr = World;
	Durin::TObjectPtr<Durin::AActor> ActorPtr = Camera;
	Durin::TObjectPtr<Durin::DActorComponent> ComponentPtr = Camera->GetCameraComponent();

	Durin::DestroyObject(World);

	EXPECT_EQ(WorldPtr.Get(), nullptr);
	EXPECT_EQ(ActorPtr.Get(), nullptr);
	EXPECT_EQ(ComponentPtr.Get(), nullptr);
}

TEST(FEngineObjectTests, EngineAndWorldHaveReflectedOwnership)
{
	InitializeDObjectSystem();
	Durin::DEngine* Engine = Durin::NewObject<Durin::DEngine>(nullptr, "TestEngine");
	Durin::DWorld* World = Durin::NewObject<Durin::DWorld>(Engine, "MainWorld");

	EXPECT_EQ(Engine->GetClass(), Durin::DEngine::StaticClass());
	EXPECT_EQ(World->GetClass(), Durin::DWorld::StaticClass());
	EXPECT_EQ(World->GetOuter(), Engine);

	Durin::DestroyObject(Engine);
}

TEST(FEngineObjectTests, RootedEngineSurvivesGarbageCollection)
{
	InitializeDObjectSystem();
	Durin::DEngine* Engine = Durin::NewObject<Durin::DEngine>(nullptr, "RootedEngine");
	Durin::TObjectPtr<Durin::DEngine> EnginePtr = Engine;

	Durin::AddToRoot(Engine);
	Durin::CollectGarbage();
	EXPECT_EQ(EnginePtr.Get(), Engine);

	Durin::RemoveFromRoot(Engine);
	Durin::CollectGarbage();
	EXPECT_EQ(EnginePtr.Get(), nullptr);
}

TEST(FEngineObjectTests, GameEngineHasConcreteRuntimeClass)
{
	InitializeDObjectSystem();
	Durin::DGameEngine* Engine = Durin::NewObject<Durin::DGameEngine>(nullptr, "GameEngine");

	EXPECT_EQ(Engine->GetClass(), Durin::DGameEngine::StaticClass());
	EXPECT_TRUE(Engine->IsA<Durin::DEngine>());

	Durin::DestroyObject(Engine);
}
