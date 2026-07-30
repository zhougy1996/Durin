#include "WorldTestSupport.h"

#include "Engine/ActorIterator.h"

static_assert(std::forward_iterator<Durin::FActorIterator>);
static_assert(std::ranges::forward_range<Durin::FActorRange>);

TEST(FActorIteratorTests, EmptyWorldAndLevelProduceEmptyRanges)
{
	Durin::DWorld* World = CreateEmptyWorld();
	Durin::FActorRange WithoutLevel(World);
	EXPECT_EQ(WithoutLevel.begin(), WithoutLevel.end());
	EXPECT_EQ(WithoutLevel.GetInitialCandidateCount(), 0u);

	ASSERT_TRUE(World->SetCurrentLevel(Durin::NewObject<Durin::DLevel>(World, "Level")));
	Durin::FActorRange EmptyLevel(World);
	EXPECT_EQ(EmptyLevel.begin(), EmptyLevel.end());
	EXPECT_EQ(EmptyLevel.GetInitialCandidateCount(), 0u);
	Durin::MarkObjectHierarchyAsGarbage(World);
	Durin::CollectGarbage();
}

TEST(FActorIteratorTests, PreservesSnapshotOrderAndFiltersByReflectedClass)
{
	Durin::DWorld* World = CreateWorld();
	Durin::ACameraActor* FirstCamera = World->SpawnActor<Durin::ACameraActor>("FirstCamera");
	Durin::AStaticMeshActor* Mesh = World->SpawnActor<Durin::AStaticMeshActor>("Mesh");
	Durin::ACameraActor* SecondCamera = World->SpawnActor<Durin::ACameraActor>("SecondCamera");
	ASSERT_NE(FirstCamera, nullptr);
	ASSERT_NE(Mesh, nullptr);
	ASSERT_NE(SecondCamera, nullptr);

	std::vector<Durin::AActor*> AllActors;
	for (Durin::AActor* Actor : Durin::FActorRange(World)) AllActors.push_back(Actor);
	EXPECT_EQ(AllActors, (std::vector<Durin::AActor*>{FirstCamera, Mesh, SecondCamera}));

	std::vector<Durin::AActor*> Cameras;
	for (Durin::AActor* Actor : Durin::FActorRange(World, {.ActorClass = Durin::ACameraActor::StaticClass()}))
	{
		Cameras.push_back(Actor);
	}
	EXPECT_EQ(Cameras, (std::vector<Durin::AActor*>{FirstCamera, SecondCamera}));
	Durin::MarkObjectHierarchyAsGarbage(World);
	Durin::CollectGarbage();
}

TEST(FActorIteratorTests, SpawnDuringIterationIsNotObserved)
{
	Durin::DWorld* World = CreateWorld();
	Durin::AActor* First = World->SpawnActor(Durin::AActor::StaticClass(), "First");
	Durin::AActor* Second = World->SpawnActor(Durin::AActor::StaticClass(), "Second");
	ASSERT_NE(First, nullptr);
	ASSERT_NE(Second, nullptr);
	Durin::FActorRange Actors(World);
	std::vector<Durin::AActor*> Visited;
	Durin::AActor* Spawned = nullptr;

	for (Durin::AActor* Actor : Actors)
	{
		Visited.push_back(Actor);
		if (!Spawned) Spawned = World->SpawnActor(Durin::AActor::StaticClass(), "Spawned");
	}

	ASSERT_NE(Spawned, nullptr);
	EXPECT_EQ(Actors.GetInitialCandidateCount(), 2u);
	EXPECT_EQ(Visited, (std::vector<Durin::AActor*>{First, Second}));
	Durin::MarkObjectHierarchyAsGarbage(World);
	Durin::CollectGarbage();
}

TEST(FActorIteratorTests, DestroyedCurrentAndNextCandidatesAreNotRepublished)
{
	Durin::DWorld* World = CreateWorld();
	Durin::AActor* First = World->SpawnActor(Durin::AActor::StaticClass(), "First");
	Durin::AActor* Second = World->SpawnActor(Durin::AActor::StaticClass(), "Second");
	Durin::AActor* Third = World->SpawnActor(Durin::AActor::StaticClass(), "Third");
	ASSERT_NE(First, nullptr);
	ASSERT_NE(Second, nullptr);
	ASSERT_NE(Third, nullptr);
	Durin::FActorRange Actors(World);
	auto It = Actors.begin();
	ASSERT_NE(It, Actors.end());
	EXPECT_EQ(*It, First);

	EXPECT_TRUE(World->DestroyActor(First));
	EXPECT_TRUE(World->DestroyActor(Second));
	++It;

	ASSERT_NE(It, Actors.end());
	EXPECT_EQ(*It, Third);
	++It;
	EXPECT_EQ(It, Actors.end());
	Durin::MarkObjectHierarchyAsGarbage(World);
	Durin::CollectGarbage();
}

TEST(FActorIteratorTests, LevelReplacementInvalidatesCapturedCandidates)
{
	Durin::DWorld* World = CreateWorld();
	ASSERT_NE(World->SpawnActor(Durin::AActor::StaticClass(), "Actor"), nullptr);
	Durin::FActorRange Actors(World);
	auto It = Actors.begin();
	ASSERT_NE(It, Actors.end());
	Durin::DLevel* Replacement = Durin::NewObject<Durin::DLevel>(World, "Replacement");
	ASSERT_TRUE(World->SetCurrentLevel(Replacement));

	EXPECT_EQ(It, Actors.end());
	Durin::MarkObjectHierarchyAsGarbage(World);
	Durin::CollectGarbage();
}

TEST(FActorIteratorTests, CopiedIteratorsOwnStableSharedCandidateState)
{
	Durin::DWorld* World = CreateWorld();
	Durin::AActor* First = World->SpawnActor(Durin::AActor::StaticClass(), "First");
	Durin::AActor* Second = World->SpawnActor(Durin::AActor::StaticClass(), "Second");
	ASSERT_NE(First, nullptr);
	ASSERT_NE(Second, nullptr);
	Durin::FActorRange Actors(World);
	auto FirstIterator = Actors.begin();
	auto Copy = FirstIterator;

	EXPECT_EQ(*FirstIterator, First);
	EXPECT_EQ(*Copy, First);
	++Copy;
	EXPECT_EQ(*FirstIterator, First);
	EXPECT_EQ(*Copy, Second);
	Durin::MarkObjectHierarchyAsGarbage(World);
	Durin::CollectGarbage();
}

TEST(FActorIteratorTests, GarbageCollectionInvalidatesHandlesWithoutRetainingObjects)
{
	Durin::DWorld* World = CreateWorld();
	ASSERT_NE(World->SpawnActor(Durin::AActor::StaticClass(), "Actor"), nullptr);
	Durin::FActorRange Actors(World);
	auto It = Actors.begin();
	ASSERT_NE(It, Actors.end());

	Durin::MarkObjectHierarchyAsGarbage(World);
	Durin::CollectGarbage();

	EXPECT_EQ(It, Actors.end());
}
