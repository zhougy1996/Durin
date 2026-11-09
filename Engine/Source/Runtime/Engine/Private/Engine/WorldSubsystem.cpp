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
		auto* World = GetWorld();
		return !World || World->IsReadyForFinishDestroy();
	}

	auto DWorldSubsystem::GetWorld() const -> DWorld* { return Cast<DWorld>(GetOuter()); }

	namespace
	{
		struct FWorldRegistrationEntry { uint64 Identity; FWorldSubsystemDescriptor Descriptor; };
		auto WorldRegistry() -> std::vector<FWorldRegistrationEntry>&
		{
			static auto* Entries = new std::vector<FWorldRegistrationEntry>();
			return *Entries;
		}
		uint64 NextWorldRegistration = 0;
	}

	auto DWorldSubsystem::ShouldCreateSubsystem(DObject* Outer) const -> bool
	{
		const auto* World = Cast<DWorld>(Outer);
		return Super::ShouldCreateSubsystem(Outer) && World && DoesSupportWorldType(World->GetWorldType());
	}

	auto DWorldSubsystem::DoesSupportWorldType(EWorldType WorldType) const -> bool
	{
		return WorldType == EWorldType::Game || WorldType == EWorldType::Editor || WorldType == EWorldType::PIE;
	}

	FWorldSubsystemRegistration::FWorldSubsystemRegistration(FWorldSubsystemDescriptor Descriptor)
	{
		require(!GIsGameThreadIdInitialized || IsInGameThread());
		Identity = ++NextWorldRegistration;
		WorldRegistry().push_back({Identity, std::move(Descriptor)});
	}
	FWorldSubsystemRegistration::~FWorldSubsystemRegistration()
	{
		require(!GIsGameThreadIdInitialized || IsInGameThread());
		std::erase_if(WorldRegistry(), [this](const auto& Entry) { return Entry.Identity == Identity; });
	}
	auto FWorldSubsystemRegistration::Snapshot() -> std::vector<FWorldSubsystemDescriptor>
	{
		require(!GIsGameThreadIdInitialized || IsInGameThread());
		std::vector<FWorldSubsystemDescriptor> Result;
		for (const auto& Entry : WorldRegistry()) Result.push_back(Entry.Descriptor);
		return Result;
	}

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
		for (const auto& Descriptor : FWorldSubsystemRegistration::Snapshot())
		{
			Selected.push_back({Descriptor.Type, Descriptor.Provider, Descriptor.Dependencies});
			WorldEntries[Descriptor.Type] = {Descriptor.bTick, Descriptor.bTickInEditorAndPreview, Descriptor.TickGroup};
		}
		return FSubsystemCollection::Initialize(std::move(Selected));
	}

	auto FWorldSubsystemCollection::ValidateSelectedDescriptor(const FSubsystemDescriptor& Descriptor) const -> FSubsystemResult
	{
		if (WorldEntries.at(Descriptor.Type).TickGroup >= ETickingGroup::Count)
			return {EWorldSubsystemError::InvalidDescriptor, "Invalid World Tick group."};
		return {};
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
	auto FWorldSubsystemCollection::RenderSceneChanged(
		FSceneInterface* Previous, FSceneInterface* Current) -> void
	{
		for (auto& Entry : Entries)
			static_cast<DWorldSubsystem*>(Entry.Object.Get())->OnRenderSceneChanged(Previous, Current);
	}
	auto FWorldSubsystemCollection::StartTick() -> void
	{
		for (auto& Entry : Entries) WorldEntries[Entry.Descriptor.Type].bFrameTickEnabled = static_cast<DWorldSubsystem*>(Entry.Object.Get())->IsTickEnabled();
	}
	auto FWorldSubsystemCollection::Tick(ETickingGroup Group, float DeltaSeconds, bool bGameplay) -> void
	{
		const bool bEditor = World.GetWorldType() == EWorldType::Editor || World.GetWorldType() == EWorldType::EditorPreview;
		for (size_t Index = 0; Index < Entries.size() && World.CanDispatchSubsystems(); ++Index)
		{
			const auto& Entry = WorldEntries[Entries[Index].Descriptor.Type];
			if (!Entry.bFrameTickEnabled || !Entry.bTick || Entry.TickGroup != Group) continue;
			if (!bGameplay && !(bEditor && Entry.bTickInEditorAndPreview)) continue;
			static_cast<DWorldSubsystem*>(Entries[Index].Object.Get())->Tick(DeltaSeconds);
		}
	}
}
