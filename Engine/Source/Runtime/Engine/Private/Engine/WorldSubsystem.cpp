#include "Engine/WorldSubsystem.h"

#include "Engine/World.h"
#include "CoreGlobals.h"
#include "Threading/RunnableThread.h"
#include "DObject/Class.h"
#include "DObject/DObjectGlobals.h"
#include "DObject/ObjectLifecycle.h"
#include "Modules/ModuleManager.h"

namespace Durin
{
	DWorldSubsystem::DWorldSubsystem(const FObjectInitializer& Initializer) : Super(Initializer) {}
	auto DWorldSubsystem::BeginDestroy() -> void
	{
		if (DWorld* World = GetWorld()) World->Shutdown();
		Super::BeginDestroy();
	}

	auto DWorldSubsystem::SetTickEnabled(bool bEnabled) -> void
	{
		require(!GIsGameThreadIdInitialized || IsInGameThread());
		bTickEnabled = bEnabled;
	}

	auto DWorldSubsystem::IsReadyForFinishDestroy() -> bool
	{
		const auto* World = GetWorld();
		return !World || const_cast<DWorld*>(World)->IsReadyForFinishDestroy();
	}

	auto DWorldSubsystem::GetWorld() const -> DWorld* { return Cast<DWorld>(GetOuter()); }

	FWorldSubsystemRegistration::FWorldSubsystemRegistration(FWorldSubsystemDescriptor Descriptor)
		: FSubsystemRegistration(DWorldSubsystem::StaticClass(),
			{Descriptor.Type, Descriptor.Provider, Descriptor.Dependencies}, Descriptor) {}
	FWorldSubsystemRegistration::~FWorldSubsystemRegistration() = default;

	FWorldSubsystemCollection::FWorldSubsystemCollection(DWorld& InWorld) : FSubsystemCollection(InWorld, DWorldSubsystem::StaticClass()), World(InWorld) {}
	FWorldSubsystemCollection::~FWorldSubsystemCollection() = default;

	auto FWorldSubsystemCollection::Find(DClass* Type) const -> DWorldSubsystem*
	{
		return static_cast<DWorldSubsystem*>(FSubsystemCollection::Find(Type));
	}

	auto FWorldSubsystemCollection::Initialize() -> FWorldSubsystemResult
	{
		if (State != EWorldSubsystemState::Uninitialized)
			return {EWorldSubsystemError::InvalidState, "World subsystem initialization is one-shot."};
		std::vector<FSubsystemDescriptor> Selected;
		for (const auto& Registration : FSubsystemRegistration::Snapshot(DWorldSubsystem::StaticClass()))
		{
			const auto* Policy = std::any_cast<FWorldSubsystemDescriptor>(&Registration.Policy);
			const FWorldSubsystemDescriptor DefaultPolicy{.Type = Registration.Descriptor.Type,
				.Provider = Registration.Descriptor.Provider, .Dependencies = Registration.Descriptor.Dependencies};
			const auto& Descriptor = Policy ? *Policy : DefaultPolicy;
			if (!Descriptor.WorldTypes.empty() && std::ranges::find(Descriptor.WorldTypes, World.GetWorldType()) == Descriptor.WorldTypes.end()) continue;
			if (Descriptor.TickGroup >= ETickingGroup::Count)
			{
				State = EWorldSubsystemState::Failed;
				return {EWorldSubsystemError::InvalidDescriptor, "Invalid World Tick group."};
			}
			Selected.push_back({Descriptor.Type, Descriptor.Provider, Descriptor.Dependencies});
			WorldEntries[Descriptor.Type] = {Descriptor};
		}
		return FSubsystemCollection::Initialize(std::move(Selected));
	}

	auto FWorldSubsystemCollection::BeginPlay() -> void
	{
		for (size_t Index = 0; Index < Entries.size() && World.CanDispatchSubsystems(); ++Index)
		{
			WorldEntries[Entries[Index].Descriptor.Type].bPlaying = true;
			static_cast<DWorldSubsystem*>(Entries[Index].Object.Get())->OnWorldBeginPlay();
		}
	}
	auto FWorldSubsystemCollection::EndPlay() -> void
	{
		for (size_t Index = Entries.size(); Index-- > 0;)
		{
			if (!std::exchange(WorldEntries[Entries[Index].Descriptor.Type].bPlaying, false)) continue;
			static_cast<DWorldSubsystem*>(Entries[Index].Object.Get())->OnWorldEndPlay();
		}
	}
	auto FWorldSubsystemCollection::LevelChanged(DLevel& Level, bool bAttached) -> void
	{
		for (size_t Offset = 0; Offset < Entries.size(); ++Offset)
		{
			if (bAttached && !World.CanDispatchSubsystems()) break;
			const size_t Index = bAttached ? Offset : Entries.size() - Offset - 1;
			if (!bAttached && !WorldEntries[Entries[Index].Descriptor.Type].bAttached) continue;
			WorldEntries[Entries[Index].Descriptor.Type].bAttached = bAttached;
			if (bAttached) static_cast<DWorldSubsystem*>(Entries[Index].Object.Get())->OnLevelAttached(Level);
			else static_cast<DWorldSubsystem*>(Entries[Index].Object.Get())->OnLevelDetached(Level);
		}
	}
	auto FWorldSubsystemCollection::StartTick() -> void
	{
		for (auto& Entry : Entries) WorldEntries[Entry.Descriptor.Type].bFrameTickEnabled = static_cast<DWorldSubsystem*>(Entry.Object.Get())->IsTickEnabled();
	}
	auto FWorldSubsystemCollection::Tick(ETickingGroup Group, float DeltaSeconds, bool bGameplay) -> void
	{
		const bool bEditor = World.GetWorldType() == EWorldType::Editor || World.GetWorldType() == EWorldType::Preview;
		for (size_t Index = 0; Index < Entries.size() && World.CanDispatchSubsystems(); ++Index)
		{
			if (bGameplay && !World.HasBegunPlay()) break;
			const auto& Entry = WorldEntries[Entries[Index].Descriptor.Type];
			if (!Entry.bFrameTickEnabled || !Entry.Descriptor.bTick || Entry.Descriptor.TickGroup != Group) continue;
			if (!bGameplay && !(bEditor && Entry.Descriptor.bTickInEditorAndPreview)) continue;
			static_cast<DWorldSubsystem*>(Entries[Index].Object.Get())->Tick(DeltaSeconds);
		}
	}
}
