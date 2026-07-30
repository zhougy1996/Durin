#include "WorldTestSupport.h"

namespace Durin
{
	enum class ETestLifecycleEvent : uint8
	{
		BeginEnter,
		BeginMutation,
		BeginExit,
		Tick,
		EndEnter,
		EndMutation,
		EndExit,
	};

	class FActorLifecycleMutationTestActor : public AActor
	{
	private:
		static auto GetPrivateStaticClass() -> DClass*;

		DECLARE_CLASS(FActorLifecycleMutationTestActor, AActor, FActorLifecycleMutationTestActor::GetPrivateStaticClass)
		DEFINE_DEFAULT_OBJECT_INITIALIZER_CONSTRUCTOR_CALL(FActorLifecycleMutationTestActor)

	public:
		explicit FActorLifecycleMutationTestActor(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get())
			: Super(ObjectInitializer)
		{
		}

		auto BeginPlay() -> void override
		{
			Publish(ETestLifecycleEvent::BeginEnter);
			Super::BeginPlay();
			Publish(ETestLifecycleEvent::BeginMutation);
			Publish(ETestLifecycleEvent::BeginExit);
		}

		auto Tick(float DeltaSeconds) -> void override
		{
			Super::Tick(DeltaSeconds);
			Publish(ETestLifecycleEvent::Tick);
		}

		auto EndPlay() -> void override
		{
			Publish(ETestLifecycleEvent::EndEnter);
			Publish(ETestLifecycleEvent::EndMutation);
			Super::EndPlay();
			Publish(ETestLifecycleEvent::EndExit);
		}

		inline static std::function<void(FActorLifecycleMutationTestActor&, ETestLifecycleEvent)> Callback;

	private:
		auto Publish(ETestLifecycleEvent Event) -> void
		{
			if (Callback) Callback(*this, Event);
		}
	};

	class FActorLifecycleMutationTestComponent : public DActorComponent
	{
	private:
		static auto GetPrivateStaticClass() -> DClass*;

		DECLARE_CLASS(FActorLifecycleMutationTestComponent, DActorComponent, FActorLifecycleMutationTestComponent::GetPrivateStaticClass)
		DEFINE_DEFAULT_OBJECT_INITIALIZER_CONSTRUCTOR_CALL(FActorLifecycleMutationTestComponent)

	public:
		explicit FActorLifecycleMutationTestComponent(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get())
			: Super(ObjectInitializer)
		{
		}

		auto BeginPlay() -> void override
		{
			Publish(ETestLifecycleEvent::BeginEnter);
			Super::BeginPlay();
			Publish(ETestLifecycleEvent::BeginMutation);
			Publish(ETestLifecycleEvent::BeginExit);
		}

		auto EndPlay() -> void override
		{
			Publish(ETestLifecycleEvent::EndEnter);
			Publish(ETestLifecycleEvent::EndMutation);
			Super::EndPlay();
			Publish(ETestLifecycleEvent::EndExit);
		}

		inline static std::function<void(FActorLifecycleMutationTestComponent&, ETestLifecycleEvent)> Callback;

	private:
		auto Publish(ETestLifecycleEvent Event) -> void
		{
			if (Callback) Callback(*this, Event);
		}
	};

	IMPLEMENT_CLASS_NO_AUTO_REGISTRATION(FActorLifecycleMutationTestActor)
	IMPLEMENT_CLASS_NO_AUTO_REGISTRATION(FActorLifecycleMutationTestComponent)
} // namespace Durin

namespace
{
	using Durin::ETestLifecycleEvent;
	using Durin::FActorLifecycleMutationTestActor;
	using Durin::FActorLifecycleMutationTestComponent;

	struct FMutationCallbackScope
	{
		~FMutationCallbackScope()
		{
			FActorLifecycleMutationTestActor::Callback = {};
			FActorLifecycleMutationTestComponent::Callback = {};
		}
	};

	auto SpawnMutationActor(Durin::DWorld* World, std::string_view Name) -> FActorLifecycleMutationTestActor*
	{
		return static_cast<FActorLifecycleMutationTestActor*>(
			World->SpawnActor(FActorLifecycleMutationTestActor::StaticClass(), Durin::FName(Name)));
	}

	auto AddMutationComponent(Durin::AActor* Actor, std::string_view Name) -> FActorLifecycleMutationTestComponent*
	{
		return static_cast<FActorLifecycleMutationTestComponent*>(
			Actor->AddInstanceComponent(FActorLifecycleMutationTestComponent::StaticClass(), Durin::FName(Name)));
	}

	auto EventName(const Durin::DObject& Object, ETestLifecycleEvent Event) -> std::string
	{
		std::string Suffix;
		switch (Event)
		{
		case ETestLifecycleEvent::BeginEnter: Suffix = "BeginEnter"; break;
		case ETestLifecycleEvent::BeginMutation: Suffix = "BeginMutation"; break;
		case ETestLifecycleEvent::BeginExit: Suffix = "BeginExit"; break;
		case ETestLifecycleEvent::Tick: Suffix = "Tick"; break;
		case ETestLifecycleEvent::EndEnter: Suffix = "EndEnter"; break;
		case ETestLifecycleEvent::EndMutation: Suffix = "EndMutation"; break;
		case ETestLifecycleEvent::EndExit: Suffix = "EndExit"; break;
		}
		return std::format("{}.{}", Object.GetFName().ToString(), Suffix);
	}

	auto RetainActorCapacity(Durin::DWorld* World) -> void
	{
		std::vector<Durin::TObjectPtr<Durin::AActor>> Padding;
		for (Durin::uint32 Index = 0; Index < 8; ++Index)
		{
			Padding.emplace_back(SpawnMutationActor(World, std::format("Padding{}", Index)));
		}
		for (const Durin::TObjectPtr<Durin::AActor>& Actor : Padding)
		{
			ASSERT_TRUE(World->DestroyActor(Actor.Get()));
		}
	}

	auto RetainComponentCapacity(Durin::AActor* Actor) -> void
	{
		std::vector<Durin::TObjectPtr<Durin::DActorComponent>> Padding;
		for (Durin::uint32 Index = 0; Index < 8; ++Index)
		{
			Padding.emplace_back(AddMutationComponent(Actor, std::format("Padding{}", Index)));
		}
		for (const Durin::TObjectPtr<Durin::DActorComponent>& Component : Padding)
		{
			ASSERT_TRUE(Actor->DestroyInstanceComponent(Component.Get()));
		}
	}
} // namespace

TEST(FWorldLifecycleMutationTests, BeginPlaySpawnUsesTheSpawnPathExactlyOnce)
{
	FMutationCallbackScope CallbackScope;
	Durin::DWorld* World = CreateWorld();
	FActorLifecycleMutationTestActor* Spawner = SpawnMutationActor(World, "Spawner");
	ASSERT_NE(Spawner, nullptr);
	RetainActorCapacity(World);
	std::vector<std::string> Events;
	FActorLifecycleMutationTestActor* Spawned = nullptr;
	FActorLifecycleMutationTestActor::Callback =
		[&](FActorLifecycleMutationTestActor& Actor, ETestLifecycleEvent Event)
		{
			Events.push_back(EventName(Actor, Event));
			if (&Actor == Spawner && Event == ETestLifecycleEvent::BeginMutation)
			{
				Spawned = SpawnMutationActor(World, "Spawned");
			}
		};

	World->BeginPlay();

	ASSERT_NE(Spawned, nullptr);
	EXPECT_TRUE(Spawned->HasBegunPlay());
	EXPECT_EQ(std::ranges::count(Events, "Spawner.BeginEnter"), 1);
	EXPECT_EQ(std::ranges::count(Events, "Spawned.BeginEnter"), 1);
	Durin::MarkObjectHierarchyAsGarbage(World);
	Durin::CollectGarbage();
}

TEST(FWorldLifecycleMutationTests, BeginPlaySkipsAnActorDestroyedBeforeItsTurn)
{
	FMutationCallbackScope CallbackScope;
	Durin::DWorld* World = CreateWorld();
	FActorLifecycleMutationTestActor* Destroyer = SpawnMutationActor(World, "Destroyer");
	FActorLifecycleMutationTestActor* Target = SpawnMutationActor(World, "Target");
	ASSERT_NE(Destroyer, nullptr);
	ASSERT_NE(Target, nullptr);
	RetainActorCapacity(World);
	std::vector<std::string> Events;
	FActorLifecycleMutationTestActor::Callback =
		[&](FActorLifecycleMutationTestActor& Actor, ETestLifecycleEvent Event)
		{
			Events.push_back(EventName(Actor, Event));
			if (&Actor == Destroyer && Event == ETestLifecycleEvent::BeginMutation)
			{
				EXPECT_TRUE(World->DestroyActor(Target));
			}
		};

	World->BeginPlay();

	EXPECT_EQ(std::ranges::count(Events, "Target.BeginEnter"), 0);
	EXPECT_FALSE(World->ContainsActor(Target));
	Durin::MarkObjectHierarchyAsGarbage(World);
	Durin::CollectGarbage();
}

TEST(FWorldLifecycleMutationTests, BeginPlaySelfDestructionCompletesAfterTheCallbackUnwinds)
{
	FMutationCallbackScope CallbackScope;
	Durin::DWorld* World = CreateWorld();
	FActorLifecycleMutationTestActor* Actor = SpawnMutationActor(World, "Self");
	ASSERT_NE(Actor, nullptr);
	std::vector<std::string> Events;
	FActorLifecycleMutationTestActor::Callback =
		[&](FActorLifecycleMutationTestActor& Candidate, ETestLifecycleEvent Event)
		{
			Events.push_back(EventName(Candidate, Event));
			if (&Candidate == Actor && Event == ETestLifecycleEvent::BeginMutation)
			{
				EXPECT_TRUE(World->DestroyActor(Actor));
			}
		};

	World->BeginPlay();

	EXPECT_EQ(Events, (std::vector<std::string>{
		"Self.BeginEnter",
		"Self.BeginMutation",
		"Self.BeginExit",
		"Self.EndEnter",
		"Self.EndMutation",
		"Self.EndExit",
	}));
	EXPECT_FALSE(World->ContainsActor(Actor));
	Durin::MarkObjectHierarchyAsGarbage(World);
	Durin::CollectGarbage();
}

TEST(FWorldLifecycleMutationTests, EndPlayRejectsSpawnBeforeAllocation)
{
	FMutationCallbackScope CallbackScope;
	Durin::DWorld* World = CreateWorld();
	FActorLifecycleMutationTestActor* Actor = SpawnMutationActor(World, "Actor");
	ASSERT_NE(Actor, nullptr);
	World->BeginPlay();
	const size_t ActorCountBeforeEndPlay = World->GetActors().size();
	FActorLifecycleMutationTestActor* SpawnResult = Actor;
	FActorLifecycleMutationTestActor::Callback =
		[&](FActorLifecycleMutationTestActor& Candidate, ETestLifecycleEvent Event)
		{
			if (&Candidate == Actor && Event == ETestLifecycleEvent::EndMutation)
			{
				SpawnResult = SpawnMutationActor(World, "Rejected");
			}
		};

	World->EndPlay();

	EXPECT_EQ(SpawnResult, nullptr);
	EXPECT_EQ(World->GetActors().size(), ActorCountBeforeEndPlay);
	EXPECT_EQ(World->FindActorByName("Rejected"), nullptr);
	Durin::MarkObjectHierarchyAsGarbage(World);
	Durin::CollectGarbage();
}

TEST(FWorldLifecycleMutationTests, EndPlayNeverBeginsAnActorRequestedByACallback)
{
	FMutationCallbackScope CallbackScope;
	Durin::DWorld* World = CreateWorld();
	FActorLifecycleMutationTestActor* Actor = SpawnMutationActor(World, "Actor");
	ASSERT_NE(Actor, nullptr);
	World->BeginPlay();
	bool bObservedWorldPlaying = true;
	FActorLifecycleMutationTestActor* SpawnResult = nullptr;
	FActorLifecycleMutationTestActor::Callback =
		[&](FActorLifecycleMutationTestActor& Candidate, ETestLifecycleEvent Event)
		{
			if (&Candidate == Actor && Event == ETestLifecycleEvent::EndMutation)
			{
				bObservedWorldPlaying = World->HasBegunPlay();
				SpawnResult = SpawnMutationActor(World, "Requested");
			}
		};

	World->EndPlay();

	EXPECT_FALSE(bObservedWorldPlaying);
	EXPECT_TRUE(!SpawnResult || !SpawnResult->HasBegunPlay());
	Durin::MarkObjectHierarchyAsGarbage(World);
	Durin::CollectGarbage();
}

TEST(FWorldLifecycleMutationTests, EndPlaySkipsASiblingDestroyedBeforeItsTurn)
{
	FMutationCallbackScope CallbackScope;
	Durin::DWorld* World = CreateWorld();
	FActorLifecycleMutationTestActor* Target = SpawnMutationActor(World, "Target");
	FActorLifecycleMutationTestActor* Destroyer = SpawnMutationActor(World, "Destroyer");
	ASSERT_NE(Target, nullptr);
	ASSERT_NE(Destroyer, nullptr);
	RetainActorCapacity(World);
	World->BeginPlay();
	std::vector<std::string> Events;
	Durin::uint32 DestroyRequests = 0;
	bool bDestroyResult = false;
	FActorLifecycleMutationTestActor::Callback =
		[&](FActorLifecycleMutationTestActor& Actor, ETestLifecycleEvent Event)
		{
			Events.push_back(EventName(Actor, Event));
			if (&Actor == Destroyer && Event == ETestLifecycleEvent::EndMutation)
			{
				++DestroyRequests;
				bDestroyResult = World->DestroyActor(Target);
			}
		};

	World->EndPlay();

	EXPECT_EQ(DestroyRequests, 1u);
	EXPECT_TRUE(bDestroyResult);
	EXPECT_EQ(std::ranges::count(Events, "Target.EndEnter"), 1);
	EXPECT_FALSE(World->ContainsActor(Target));
	Durin::MarkObjectHierarchyAsGarbage(World);
	Durin::CollectGarbage();
}

TEST(FWorldLifecycleMutationTests, EndPlaySelfDestructionIsIdempotent)
{
	FMutationCallbackScope CallbackScope;
	Durin::DWorld* World = CreateWorld();
	FActorLifecycleMutationTestActor* Actor = SpawnMutationActor(World, "Self");
	ASSERT_NE(Actor, nullptr);
	World->BeginPlay();
	std::vector<std::string> Events;
	FActorLifecycleMutationTestActor::Callback =
		[&](FActorLifecycleMutationTestActor& Candidate, ETestLifecycleEvent Event)
		{
			Events.push_back(EventName(Candidate, Event));
			if (&Candidate == Actor && Event == ETestLifecycleEvent::EndMutation)
			{
				EXPECT_TRUE(World->DestroyActor(Actor));
			}
		};

	World->EndPlay();

	EXPECT_EQ(std::ranges::count(Events, "Self.EndEnter"), 1);
	EXPECT_EQ(std::ranges::count(Events, "Self.EndExit"), 1);
	EXPECT_FALSE(World->ContainsActor(Actor));
	Durin::MarkObjectHierarchyAsGarbage(World);
	Durin::CollectGarbage();
}

TEST(FWorldLifecycleMutationTests, RepeatedWorldLifecycleCallsAreIdempotent)
{
	FMutationCallbackScope CallbackScope;
	Durin::DWorld* World = CreateWorld();
	ASSERT_NE(SpawnMutationActor(World, "Actor"), nullptr);
	std::vector<std::string> Events;
	FActorLifecycleMutationTestActor::Callback =
		[&](FActorLifecycleMutationTestActor& Actor, ETestLifecycleEvent Event)
		{
			Events.push_back(EventName(Actor, Event));
			if (Event == ETestLifecycleEvent::BeginMutation) World->BeginPlay();
			if (Event == ETestLifecycleEvent::EndMutation) World->EndPlay();
		};

	World->BeginPlay();
	World->BeginPlay();
	World->EndPlay();
	World->EndPlay();

	EXPECT_EQ(std::ranges::count(Events, "Actor.BeginEnter"), 1);
	EXPECT_EQ(std::ranges::count(Events, "Actor.EndEnter"), 1);
	Durin::MarkObjectHierarchyAsGarbage(World);
	Durin::CollectGarbage();
}

TEST(FWorldLifecycleMutationTests, EndPlayStopsWhenTheCurrentLevelIsReplaced)
{
	FMutationCallbackScope CallbackScope;
	Durin::DWorld* World = CreateWorld();
	FActorLifecycleMutationTestActor* Stale = SpawnMutationActor(World, "Stale");
	FActorLifecycleMutationTestActor* Switcher = SpawnMutationActor(World, "Switcher");
	Durin::DLevel* Replacement = Durin::NewObject<Durin::DLevel>(World, "Replacement");
	ASSERT_NE(Stale, nullptr);
	ASSERT_NE(Switcher, nullptr);
	ASSERT_NE(Replacement, nullptr);
	World->BeginPlay();
	std::vector<std::string> Events;
	FActorLifecycleMutationTestActor::Callback =
		[&](FActorLifecycleMutationTestActor& Actor, ETestLifecycleEvent Event)
		{
			Events.push_back(EventName(Actor, Event));
			if (&Actor == Switcher && Event == ETestLifecycleEvent::EndMutation)
			{
				EXPECT_TRUE(World->SetCurrentLevel(Replacement));
			}
		};

	World->EndPlay();

	EXPECT_EQ(World->GetCurrentLevel(), Replacement);
	EXPECT_EQ(std::ranges::count(Events, "Stale.EndEnter"), 0);
	Durin::MarkObjectHierarchyAsGarbage(World);
	Durin::CollectGarbage();
}

TEST(FWorldLifecycleMutationTests, BeginPlayStopsWhenTheCurrentLevelIsReplaced)
{
	FMutationCallbackScope CallbackScope;
	Durin::DWorld* World = CreateWorld();
	FActorLifecycleMutationTestActor* Switcher = SpawnMutationActor(World, "Switcher");
	FActorLifecycleMutationTestActor* Stale = SpawnMutationActor(World, "Stale");
	Durin::DLevel* Replacement = Durin::NewObject<Durin::DLevel>(World, "Replacement");
	ASSERT_NE(Switcher, nullptr);
	ASSERT_NE(Stale, nullptr);
	ASSERT_NE(Replacement, nullptr);
	std::vector<std::string> Events;
	FActorLifecycleMutationTestActor::Callback =
		[&](FActorLifecycleMutationTestActor& Actor, ETestLifecycleEvent Event)
		{
			Events.push_back(EventName(Actor, Event));
			if (&Actor == Switcher && Event == ETestLifecycleEvent::BeginMutation)
			{
				EXPECT_TRUE(World->SetCurrentLevel(Replacement));
			}
		};

	World->BeginPlay();

	EXPECT_EQ(World->GetCurrentLevel(), Replacement);
	EXPECT_EQ(std::ranges::count(Events, "Stale.BeginEnter"), 0);
	Durin::MarkObjectHierarchyAsGarbage(World);
	Durin::CollectGarbage();
}

TEST(FComponentLifecycleMutationTests, BeginPlayAdditionUsesTheAddPathExactlyOnce)
{
	FMutationCallbackScope CallbackScope;
	Durin::DWorld* World = CreateWorld();
	Durin::AActor* Actor = World->SpawnActor(Durin::AActor::StaticClass(), "Actor");
	FActorLifecycleMutationTestComponent* Adder = AddMutationComponent(Actor, "Adder");
	ASSERT_NE(Actor, nullptr);
	ASSERT_NE(Adder, nullptr);
	RetainComponentCapacity(Actor);
	std::vector<std::string> Events;
	FActorLifecycleMutationTestComponent* Added = nullptr;
	FActorLifecycleMutationTestComponent::Callback =
		[&](FActorLifecycleMutationTestComponent& Component, ETestLifecycleEvent Event)
		{
			Events.push_back(EventName(Component, Event));
			if (&Component == Adder && Event == ETestLifecycleEvent::BeginMutation)
			{
				Added = AddMutationComponent(Actor, "Added");
			}
		};

	World->BeginPlay();

	ASSERT_NE(Added, nullptr);
	EXPECT_TRUE(Added->HasBegunPlay());
	EXPECT_EQ(std::ranges::count(Events, "Added.BeginEnter"), 1);
	Durin::MarkObjectHierarchyAsGarbage(World);
	Durin::CollectGarbage();
}

TEST(FComponentLifecycleMutationTests, BeginPlaySkipsAComponentDestroyedBeforeItsTurn)
{
	FMutationCallbackScope CallbackScope;
	Durin::DWorld* World = CreateWorld();
	Durin::AActor* Actor = World->SpawnActor(Durin::AActor::StaticClass(), "Actor");
	FActorLifecycleMutationTestComponent* Destroyer = AddMutationComponent(Actor, "Destroyer");
	FActorLifecycleMutationTestComponent* Target = AddMutationComponent(Actor, "Target");
	ASSERT_NE(Actor, nullptr);
	ASSERT_NE(Destroyer, nullptr);
	ASSERT_NE(Target, nullptr);
	RetainComponentCapacity(Actor);
	std::vector<std::string> Events;
	FActorLifecycleMutationTestComponent::Callback =
		[&](FActorLifecycleMutationTestComponent& Component, ETestLifecycleEvent Event)
		{
			Events.push_back(EventName(Component, Event));
			if (&Component == Destroyer && Event == ETestLifecycleEvent::BeginMutation)
			{
				EXPECT_TRUE(Actor->DestroyInstanceComponent(Target));
			}
		};

	World->BeginPlay();

	EXPECT_EQ(std::ranges::count(Events, "Target.BeginEnter"), 0);
	EXPECT_FALSE(Actor->IsInstanceComponent(Target));
	Durin::MarkObjectHierarchyAsGarbage(World);
	Durin::CollectGarbage();
}

TEST(FComponentLifecycleMutationTests, BeginPlaySelfDestructionCompletesAfterTheCallbackUnwinds)
{
	FMutationCallbackScope CallbackScope;
	Durin::DWorld* World = CreateWorld();
	Durin::AActor* Actor = World->SpawnActor(Durin::AActor::StaticClass(), "Actor");
	FActorLifecycleMutationTestComponent* Component = AddMutationComponent(Actor, "Self");
	ASSERT_NE(Actor, nullptr);
	ASSERT_NE(Component, nullptr);
	std::vector<std::string> Events;
	FActorLifecycleMutationTestComponent::Callback =
		[&](FActorLifecycleMutationTestComponent& Candidate, ETestLifecycleEvent Event)
		{
			Events.push_back(EventName(Candidate, Event));
			if (&Candidate == Component && Event == ETestLifecycleEvent::BeginMutation)
			{
				EXPECT_TRUE(Actor->DestroyInstanceComponent(Component));
			}
		};

	World->BeginPlay();

	EXPECT_EQ(Events, (std::vector<std::string>{
		"Self.BeginEnter",
		"Self.BeginMutation",
		"Self.BeginExit",
		"Self.EndEnter",
		"Self.EndMutation",
		"Self.EndExit",
	}));
	EXPECT_FALSE(Actor->IsInstanceComponent(Component));
	Durin::MarkObjectHierarchyAsGarbage(World);
	Durin::CollectGarbage();
}

TEST(FComponentLifecycleMutationTests, EndPlayAdditionDoesNotBegin)
{
	FMutationCallbackScope CallbackScope;
	Durin::DWorld* World = CreateWorld();
	Durin::AActor* Actor = World->SpawnActor(Durin::AActor::StaticClass(), "Actor");
	FActorLifecycleMutationTestComponent* Adder = AddMutationComponent(Actor, "Adder");
	ASSERT_NE(Actor, nullptr);
	ASSERT_NE(Adder, nullptr);
	World->BeginPlay();
	FActorLifecycleMutationTestComponent* Added = nullptr;
	FActorLifecycleMutationTestComponent::Callback =
		[&](FActorLifecycleMutationTestComponent& Component, ETestLifecycleEvent Event)
		{
			if (&Component == Adder && Event == ETestLifecycleEvent::EndMutation)
			{
				Added = AddMutationComponent(Actor, "Added");
			}
		};

	World->EndPlay();

	ASSERT_NE(Added, nullptr);
	EXPECT_FALSE(Added->HasBegunPlay());
	Durin::MarkObjectHierarchyAsGarbage(World);
	Durin::CollectGarbage();
}

TEST(FComponentLifecycleMutationTests, EndPlaySiblingDestructionIsExactlyOnce)
{
	FMutationCallbackScope CallbackScope;
	Durin::DWorld* World = CreateWorld();
	Durin::AActor* Actor = World->SpawnActor(Durin::AActor::StaticClass(), "Actor");
	FActorLifecycleMutationTestComponent* Target = AddMutationComponent(Actor, "Target");
	FActorLifecycleMutationTestComponent* Destroyer = AddMutationComponent(Actor, "Destroyer");
	ASSERT_NE(Actor, nullptr);
	ASSERT_NE(Target, nullptr);
	ASSERT_NE(Destroyer, nullptr);
	RetainComponentCapacity(Actor);
	World->BeginPlay();
	std::vector<std::string> Events;
	FActorLifecycleMutationTestComponent::Callback =
		[&](FActorLifecycleMutationTestComponent& Component, ETestLifecycleEvent Event)
		{
			Events.push_back(EventName(Component, Event));
			if (&Component == Destroyer && Event == ETestLifecycleEvent::EndMutation)
			{
				EXPECT_TRUE(Actor->DestroyInstanceComponent(Target));
			}
		};

	World->EndPlay();

	EXPECT_EQ(std::ranges::count(Events, "Target.EndEnter"), 1);
	EXPECT_FALSE(Actor->IsInstanceComponent(Target));
	Durin::MarkObjectHierarchyAsGarbage(World);
	Durin::CollectGarbage();
}

TEST(FComponentLifecycleMutationTests, EndPlaySelfDestructionIsIdempotent)
{
	FMutationCallbackScope CallbackScope;
	Durin::DWorld* World = CreateWorld();
	Durin::AActor* Actor = World->SpawnActor(Durin::AActor::StaticClass(), "Actor");
	FActorLifecycleMutationTestComponent* Component = AddMutationComponent(Actor, "Self");
	ASSERT_NE(Actor, nullptr);
	ASSERT_NE(Component, nullptr);
	World->BeginPlay();
	std::vector<std::string> Events;
	FActorLifecycleMutationTestComponent::Callback =
		[&](FActorLifecycleMutationTestComponent& Candidate, ETestLifecycleEvent Event)
		{
			Events.push_back(EventName(Candidate, Event));
			if (&Candidate == Component && Event == ETestLifecycleEvent::EndMutation)
			{
				EXPECT_TRUE(Actor->DestroyInstanceComponent(Component));
			}
		};

	World->EndPlay();

	EXPECT_EQ(std::ranges::count(Events, "Self.EndEnter"), 1);
	EXPECT_EQ(std::ranges::count(Events, "Self.EndExit"), 1);
	EXPECT_FALSE(Actor->IsInstanceComponent(Component));
	Durin::MarkObjectHierarchyAsGarbage(World);
	Durin::CollectGarbage();
}
