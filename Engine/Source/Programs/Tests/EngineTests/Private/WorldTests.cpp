#include "Actors/CameraActor.h"
#include "Actors/StaticMeshActor.h"
#include "DObject/DObjectGlobals.h"
#include "Engine/World.h"

#include <gtest/gtest.h>

namespace
{
	auto CreateWorld() -> std::unique_ptr<Durin::DWorld>
	{
		static const bool bInitialized = []() {
			Durin::DObjectInit();
			return true;
		}();
		(void)bInitialized;
		return std::make_unique<Durin::DWorld>();
	}
} // namespace

TEST(FWorldTests, SpawnsEnumeratesAndFindsActors)
{
	auto World = CreateWorld();
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
}

TEST(FWorldTests, MakesDuplicateActorNamesUnique)
{
	auto World = CreateWorld();
	Durin::ACameraActor* First = World->SpawnActor<Durin::ACameraActor>("Camera");
	Durin::ACameraActor* Second = World->SpawnActor<Durin::ACameraActor>("Camera");
	Durin::ACameraActor* Third = World->SpawnActor<Durin::ACameraActor>("Camera");

	EXPECT_EQ(First->GetName(), "Camera");
	EXPECT_EQ(Second->GetName(), "Camera_2");
	EXPECT_EQ(Third->GetName(), "Camera_3");
}

TEST(FWorldTests, DestroyActorRemovesItFromTheWorld)
{
	auto World = CreateWorld();
	Durin::ACameraActor* Camera = World->SpawnActor<Durin::ACameraActor>("Camera");
	Durin::TObjectPtr<Durin::AActor> Selection = Camera;

	EXPECT_TRUE(World->DestroyActor(Camera));
	EXPECT_TRUE(World->GetActors().empty());
	EXPECT_EQ(World->FindActorByName("Camera"), nullptr);
	EXPECT_FALSE(World->ContainsActor(Selection.Get()));
	Selection = nullptr;
	EXPECT_EQ(Selection.Get(), nullptr);
	EXPECT_FALSE(World->DestroyActor(nullptr));
}
