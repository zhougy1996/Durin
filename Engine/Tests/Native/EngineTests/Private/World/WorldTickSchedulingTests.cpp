#include "WorldTestSupport.h"

namespace Durin
{
	class FTickSchedulingTestActor : public AActor
	{
	private:
		static auto GetPrivateStaticClass() -> DClass*;

		DECLARE_CLASS(FTickSchedulingTestActor, AActor, FTickSchedulingTestActor::GetPrivateStaticClass)
		DEFINE_DEFAULT_OBJECT_INITIALIZER_CONSTRUCTOR_CALL(FTickSchedulingTestActor)

	public:
		explicit FTickSchedulingTestActor(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get())
			: Super(ObjectInitializer)
		{
			SetActorTickEnabled(true);
		}

		auto Tick(float DeltaSeconds) -> void override
		{
			Super::Tick(DeltaSeconds);
			if (Callback) Callback(*this);
		}

		inline static std::function<void(FTickSchedulingTestActor&)> Callback;
	};

	class FTickSchedulingTestComponent : public DActorComponent
	{
	private:
		static auto GetPrivateStaticClass() -> DClass*;

		DECLARE_CLASS(FTickSchedulingTestComponent, DActorComponent, FTickSchedulingTestComponent::GetPrivateStaticClass)
		DEFINE_DEFAULT_OBJECT_INITIALIZER_CONSTRUCTOR_CALL(FTickSchedulingTestComponent)

	public:
		explicit FTickSchedulingTestComponent(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get())
			: Super(ObjectInitializer)
		{
			SetComponentTickGroup(ConstructionGroup);
			SetComponentTickEnabled(true);
		}

		auto TickComponent(float DeltaSeconds) -> void override
		{
			Super::TickComponent(DeltaSeconds);
			if (Callback) Callback(*this);
		}

		inline static ETickingGroup ConstructionGroup = ETickingGroup::PrePhysics;
		inline static std::function<void(FTickSchedulingTestComponent&)> Callback;
	};

	IMPLEMENT_CLASS_NO_AUTO_REGISTRATION(FTickSchedulingTestActor)
	IMPLEMENT_CLASS_NO_AUTO_REGISTRATION(FTickSchedulingTestComponent)
} // namespace Durin

namespace
{
	using Durin::ETickingGroup;
	using Durin::FTickSchedulingTestActor;
	using Durin::FTickSchedulingTestComponent;

	struct FTickSchedulingCallbackScope
	{
		~FTickSchedulingCallbackScope()
		{
			FTickSchedulingTestActor::Callback = {};
			FTickSchedulingTestComponent::Callback = {};
			FTickSchedulingTestComponent::ConstructionGroup = ETickingGroup::PrePhysics;
		}
	};

	auto SpawnTickActor(Durin::DWorld* World, std::string_view Name = "Actor") -> FTickSchedulingTestActor*
	{
		return static_cast<FTickSchedulingTestActor*>(
			World->SpawnActor(FTickSchedulingTestActor::StaticClass(), Durin::FName(Name)));
	}

	auto AddTickComponent(
		Durin::AActor* Actor,
		std::string_view Name,
		ETickingGroup Group = ETickingGroup::PrePhysics) -> FTickSchedulingTestComponent*
	{
		FTickSchedulingTestComponent::ConstructionGroup = Group;
		auto* Result = static_cast<FTickSchedulingTestComponent*>(
			Actor->AddInstanceComponent(FTickSchedulingTestComponent::StaticClass(), Durin::FName(Name)));
		FTickSchedulingTestComponent::ConstructionGroup = ETickingGroup::PrePhysics;
		return Result;
	}
}

TEST(FWorldTickSchedulingTests, TicksOwnerBeforeSameGroupComponentAndKeepsComponentIndependent)
{
	FTickSchedulingCallbackScope CallbackScope;
	Durin::DWorld* World = CreateWorld();
	FTickSchedulingTestActor* Actor = SpawnTickActor(World);
	FTickSchedulingTestComponent* Component = AddTickComponent(Actor, "Component");
	ASSERT_NE(Actor, nullptr);
	ASSERT_NE(Component, nullptr);
	std::vector<std::string> Events;
	FTickSchedulingTestActor::Callback = [&](FTickSchedulingTestActor&) { Events.emplace_back("Actor"); };
	FTickSchedulingTestComponent::Callback = [&](FTickSchedulingTestComponent&) { Events.emplace_back("Component"); };
	ASSERT_TRUE(World->BeginPlay({}));

	World->Tick({.DeltaSeconds = 1.0f / 60.0f});
	EXPECT_EQ(Events, (std::vector<std::string>{"Actor", "Component"}));

	Events.clear();
	Actor->SetActorTickEnabled(false);
	World->Tick({.DeltaSeconds = 1.0f / 60.0f});
	EXPECT_EQ(Events, (std::vector<std::string>{"Component"}));

	Durin::MarkObjectHierarchyAsGarbage(World);
	Durin::CollectGarbage();
}

TEST(FWorldTickSchedulingTests, SelfDestructionCancelsFutureTicks)
{
	FTickSchedulingCallbackScope CallbackScope;
	Durin::DWorld* World = CreateWorld();
	Durin::AActor* Actor = World->SpawnActor(Durin::AActor::StaticClass(), "Actor");
	FTickSchedulingTestComponent* Component = AddTickComponent(Actor, "Self");
	ASSERT_NE(Component, nullptr);
	uint32 TickCount = 0;
	FTickSchedulingTestComponent::Callback = [&](FTickSchedulingTestComponent& Candidate)
	{
		if (&Candidate != Component) return;
		++TickCount;
		EXPECT_TRUE(Actor->DestroyInstanceComponent(Component));
	};
	ASSERT_TRUE(World->BeginPlay({}));

	World->Tick({});
	World->Tick({});

	EXPECT_EQ(TickCount, 1u);
	EXPECT_FALSE(Actor->IsInstanceComponent(Component));
	Durin::MarkObjectHierarchyAsGarbage(World);
	Durin::CollectGarbage();
}

TEST(FWorldTickSchedulingTests, SiblingDestructionSkipsTheCancelledQueueSlot)
{
	FTickSchedulingCallbackScope CallbackScope;
	Durin::DWorld* World = CreateWorld();
	Durin::AActor* Actor = World->SpawnActor(Durin::AActor::StaticClass(), "Actor");
	FTickSchedulingTestComponent* Destroyer = AddTickComponent(Actor, "Destroyer");
	FTickSchedulingTestComponent* Target = AddTickComponent(Actor, "Target");
	ASSERT_NE(Destroyer, nullptr);
	ASSERT_NE(Target, nullptr);
	uint32 TargetTicks = 0;
	FTickSchedulingTestComponent::Callback = [&](FTickSchedulingTestComponent& Candidate)
	{
		if (&Candidate == Destroyer) EXPECT_TRUE(Actor->DestroyInstanceComponent(Target));
		if (&Candidate == Target) ++TargetTicks;
	};
	ASSERT_TRUE(World->BeginPlay({}));

	World->Tick({});

	EXPECT_EQ(TargetTicks, 0u);
	EXPECT_FALSE(Actor->IsInstanceComponent(Target));
	Durin::MarkObjectHierarchyAsGarbage(World);
	Durin::CollectGarbage();
}

TEST(FWorldTickSchedulingTests, AddedSameGroupComponentStartsOnTheNextFrame)
{
	FTickSchedulingCallbackScope CallbackScope;
	Durin::DWorld* World = CreateWorld();
	Durin::AActor* Actor = World->SpawnActor(Durin::AActor::StaticClass(), "Actor");
	FTickSchedulingTestComponent* Adder = AddTickComponent(Actor, "Adder");
	ASSERT_NE(Adder, nullptr);
	FTickSchedulingTestComponent* Added = nullptr;
	uint32 AddedTicks = 0;
	FTickSchedulingTestComponent::Callback = [&](FTickSchedulingTestComponent& Candidate)
	{
		if (&Candidate == Adder && !Added) Added = AddTickComponent(Actor, "Added");
		else if (&Candidate == Added) ++AddedTicks;
	};
	ASSERT_TRUE(World->BeginPlay({}));

	World->Tick({});
	ASSERT_NE(Added, nullptr);
	EXPECT_EQ(AddedTicks, 0u);
	World->Tick({});
	EXPECT_EQ(AddedTicks, 1u);

	Durin::MarkObjectHierarchyAsGarbage(World);
	Durin::CollectGarbage();
}

TEST(FWorldTickSchedulingTests, DisableEnableCannotReviveAStaleQueueSlot)
{
	FTickSchedulingCallbackScope CallbackScope;
	Durin::DWorld* World = CreateWorld();
	Durin::AActor* Actor = World->SpawnActor(Durin::AActor::StaticClass(), "Actor");
	FTickSchedulingTestComponent* Mutator = AddTickComponent(Actor, "Mutator");
	FTickSchedulingTestComponent* Target = AddTickComponent(Actor, "Target");
	ASSERT_NE(Mutator, nullptr);
	ASSERT_NE(Target, nullptr);
	bool bMutated = false;
	uint32 TargetTicks = 0;
	FTickSchedulingTestComponent::Callback = [&](FTickSchedulingTestComponent& Candidate)
	{
		if (&Candidate == Mutator && !std::exchange(bMutated, true))
		{
			Target->SetComponentTickEnabled(false);
			Target->SetComponentTickEnabled(true);
		}
		if (&Candidate == Target) ++TargetTicks;
	};
	ASSERT_TRUE(World->BeginPlay({}));

	World->Tick({});
	EXPECT_EQ(TargetTicks, 0u);
	World->Tick({});
	EXPECT_EQ(TargetTicks, 1u);

	Durin::MarkObjectHierarchyAsGarbage(World);
	Durin::CollectGarbage();
}

TEST(FWorldTickSchedulingTests, UnregisterRegisterCannotReviveAStaleQueueSlot)
{
	FTickSchedulingCallbackScope CallbackScope;
	Durin::DWorld* World = CreateWorld();
	Durin::AActor* Actor = World->SpawnActor(Durin::AActor::StaticClass(), "Actor");
	FTickSchedulingTestComponent* Mutator = AddTickComponent(Actor, "Mutator");
	FTickSchedulingTestComponent* Target = AddTickComponent(Actor, "Target");
	ASSERT_NE(Mutator, nullptr);
	ASSERT_NE(Target, nullptr);
	bool bMutated = false;
	uint32 TargetTicks = 0;
	FTickSchedulingTestComponent::Callback = [&](FTickSchedulingTestComponent& Candidate)
	{
		if (&Candidate == Mutator && !std::exchange(bMutated, true))
		{
			Target->UnregisterComponent();
			Target->RegisterComponent();
			Target->DispatchBeginPlay();
		}
		if (&Candidate == Target) ++TargetTicks;
	};
	ASSERT_TRUE(World->BeginPlay({}));

	World->Tick({});
	EXPECT_EQ(TargetTicks, 0u);
	World->Tick({});
	EXPECT_EQ(TargetTicks, 1u);

	Durin::MarkObjectHierarchyAsGarbage(World);
	Durin::CollectGarbage();
}

TEST(FWorldTickSchedulingTests, FutureGroupRegistrationExecutesInTheSameFrame)
{
	FTickSchedulingCallbackScope CallbackScope;
	Durin::DWorld* World = CreateWorld();
	Durin::AActor* Actor = World->SpawnActor(Durin::AActor::StaticClass(), "Actor");
	FTickSchedulingTestComponent* Adder = AddTickComponent(Actor, "Adder", ETickingGroup::PrePhysics);
	ASSERT_NE(Adder, nullptr);
	std::vector<FTickSchedulingTestComponent*> Added;
	uint32 PhysicsTicks = 0;
	FTickSchedulingTestComponent::Callback = [&](FTickSchedulingTestComponent& Candidate)
	{
		if (&Candidate == Adder && Added.empty())
		{
			for (uint32 Index = 0; Index < 32; ++Index)
			{
				Added.push_back(AddTickComponent(Actor, std::format("Physics{}", Index), ETickingGroup::Physics));
			}
		}
		else if (std::ranges::find(Added, &Candidate) != Added.end())
		{
			++PhysicsTicks;
		}
	};
	ASSERT_TRUE(World->BeginPlay({}));

	World->Tick({});

	EXPECT_EQ(Added.size(), 32u);
	EXPECT_EQ(PhysicsTicks, 32u);
	Durin::MarkObjectHierarchyAsGarbage(World);
	Durin::CollectGarbage();
}

TEST(FWorldTickSchedulingTests, RunsGroupsInSerialOrder)
{
	FTickSchedulingCallbackScope CallbackScope;
	Durin::DWorld* World = CreateWorld();
	Durin::AActor* Actor = World->SpawnActor(Durin::AActor::StaticClass(), "Actor");
	FTickSchedulingTestComponent* Pre = AddTickComponent(Actor, "Pre", ETickingGroup::PrePhysics);
	FTickSchedulingTestComponent* Physics = AddTickComponent(Actor, "Physics", ETickingGroup::Physics);
	FTickSchedulingTestComponent* Post = AddTickComponent(Actor, "Post", ETickingGroup::PostPhysics);
	ASSERT_NE(Pre, nullptr);
	ASSERT_NE(Physics, nullptr);
	ASSERT_NE(Post, nullptr);
	std::vector<std::string> Events;
	FTickSchedulingTestComponent::Callback = [&](FTickSchedulingTestComponent& Candidate)
	{
		if (&Candidate == Pre) Events.emplace_back("PrePhysics");
		if (&Candidate == Physics) Events.emplace_back("Physics");
		if (&Candidate == Post) Events.emplace_back("PostPhysics");
	};
	ASSERT_TRUE(World->BeginPlay({}));

	World->Tick({});

	EXPECT_EQ(Events, (std::vector<std::string>{"PrePhysics", "Physics", "PostPhysics"}));
	Durin::MarkObjectHierarchyAsGarbage(World);
	Durin::CollectGarbage();
}

TEST(FWorldTickSchedulingTests, OwnerDestructionStopsRemainingComponentTicks)
{
	FTickSchedulingCallbackScope CallbackScope;
	Durin::DWorld* World = CreateWorld();
	Durin::AActor* Actor = World->SpawnActor(Durin::AActor::StaticClass(), "Actor");
	FTickSchedulingTestComponent* Destroyer = AddTickComponent(Actor, "Destroyer");
	FTickSchedulingTestComponent* Target = AddTickComponent(Actor, "Target");
	ASSERT_NE(Destroyer, nullptr);
	ASSERT_NE(Target, nullptr);
	uint32 TargetTicks = 0;
	FTickSchedulingTestComponent::Callback = [&](FTickSchedulingTestComponent& Candidate)
	{
		if (&Candidate == Destroyer) EXPECT_TRUE(World->DestroyActor(Actor));
		if (&Candidate == Target) ++TargetTicks;
	};
	ASSERT_TRUE(World->BeginPlay({}));

	World->Tick({});

	EXPECT_EQ(TargetTicks, 0u);
	EXPECT_FALSE(World->ContainsActor(Actor));
	Durin::MarkObjectHierarchyAsGarbage(World);
	Durin::CollectGarbage();
}

TEST(FWorldTickSchedulingTests, PendingLevelTransitionStopsTheActiveGroup)
{
	FTickSchedulingCallbackScope CallbackScope;
	Durin::DWorld* World = CreateWorld();
	Durin::AActor* Actor = World->SpawnActor(Durin::AActor::StaticClass(), "Actor");
	FTickSchedulingTestComponent* Requester = AddTickComponent(Actor, "Requester");
	FTickSchedulingTestComponent* Target = AddTickComponent(Actor, "Target");
	Durin::DLevel* Replacement = Durin::NewObject<Durin::DLevel>(World, "Replacement");
	ASSERT_NE(Requester, nullptr);
	ASSERT_NE(Target, nullptr);
	ASSERT_NE(Replacement, nullptr);
	uint32 TargetTicks = 0;
	FTickSchedulingTestComponent::Callback = [&](FTickSchedulingTestComponent& Candidate)
	{
		if (&Candidate == Requester) EXPECT_TRUE(World->RequestLevelTransition(Replacement));
		if (&Candidate == Target) ++TargetTicks;
	};
	ASSERT_TRUE(World->BeginPlay({}));

	World->Tick({});

	EXPECT_EQ(TargetTicks, 0u);
	EXPECT_NE(World->GetCurrentLevel(), Replacement);
	World->Tick({});
	EXPECT_EQ(World->GetCurrentLevel(), Replacement);
	Durin::MarkObjectHierarchyAsGarbage(World);
	Durin::CollectGarbage();
}

TEST(FWorldTickSchedulingTests, LargeRegisteredSetExecutesExactlyOncePerFrame)
{
	FTickSchedulingCallbackScope CallbackScope;
	Durin::DWorld* World = CreateWorld();
	Durin::AActor* Actor = World->SpawnActor(Durin::AActor::StaticClass(), "Actor");
	constexpr uint32 ComponentCount = 256;
	for (uint32 Index = 0; Index < ComponentCount; ++Index)
	{
		ASSERT_NE(AddTickComponent(Actor, std::format("Component{}", Index)), nullptr);
	}
	uint32 TickCount = 0;
	FTickSchedulingTestComponent::Callback = [&](FTickSchedulingTestComponent&) { ++TickCount; };
	ASSERT_TRUE(World->BeginPlay({}));

	World->Tick({});
	EXPECT_EQ(TickCount, ComponentCount);
	World->Tick({});
	EXPECT_EQ(TickCount, ComponentCount * 2);

	Durin::MarkObjectHierarchyAsGarbage(World);
	Durin::CollectGarbage();
}
