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
	namespace
	{
		struct FRegistrationEntry { uint64 Identity; FWorldSubsystemDescriptor Descriptor; };
		auto Registry() -> std::vector<FRegistrationEntry>&
		{
			static std::vector<FRegistrationEntry> Entries;
			return Entries;
		}
		uint64 NextRegistration = 0;
	}

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
		: Identity(++NextRegistration)
	{
		require(!GIsGameThreadIdInitialized || IsInGameThread());
		Registry().push_back({Identity, std::move(Descriptor)});
	}
	FWorldSubsystemRegistration::~FWorldSubsystemRegistration()
	{
		require(!GIsGameThreadIdInitialized || IsInGameThread());
		std::erase_if(Registry(), [this](const auto& Entry) { return Entry.Identity == Identity; });
	}

	FWorldSubsystemCollection::FWorldSubsystemCollection(DWorld& InWorld) : World(InWorld) {}
	FWorldSubsystemCollection::~FWorldSubsystemCollection() = default;

	auto FWorldSubsystemCollection::Find(DClass* Type) const -> DWorldSubsystem*
	{
		require(!GIsGameThreadIdInitialized || IsInGameThread());
		for (const FEntry& Entry : Entries)
			if (Entry.Descriptor.Type == Type && Entry.bInitialized) return Entry.Object.Get();
		return nullptr;
	}

	auto FWorldSubsystemCollection::Initialize() -> FWorldSubsystemResult
	{
		require(!GIsGameThreadIdInitialized || IsInGameThread());
		if (State != EWorldSubsystemState::Uninitialized)
			return {EWorldSubsystemError::InvalidState, "World subsystem initialization is one-shot."};
		State = EWorldSubsystemState::Initializing;
		auto Fail = [&](EWorldSubsystemError Error, std::string Message) {
			Shutdown();
			State = EWorldSubsystemState::Failed;
			return FWorldSubsystemResult{Error, std::move(Message)};
		};
		std::vector<FEntry> Selected;
		for (const auto& Registration : Registry())
		{
			const auto& Descriptor = Registration.Descriptor;
			if (!Descriptor.WorldTypes.empty() && std::ranges::find(Descriptor.WorldTypes, World.GetWorldType()) == Descriptor.WorldTypes.end()) continue;
			if (Descriptor.TickGroup >= ETickingGroup::Count || !Descriptor.Type || !CanConstructObjectOfClass(Descriptor.Type, DWorldSubsystem::StaticClass()))
				return Fail(EWorldSubsystemError::InvalidDescriptor, "A subsystem descriptor has no constructible concrete type.");
			if (std::ranges::any_of(Selected, [&](const auto& Entry) { return Entry.Descriptor.Type == Descriptor.Type; }))
				return Fail(EWorldSubsystemError::DuplicateType, Descriptor.Type->GetQualifiedName().ToString());
			std::shared_ptr<void> Lease;
			if (!Descriptor.Provider.IsNone())
			{
				Lease = FModuleManager::Get().AcquireCodeLease(Descriptor.Provider);
				if (!Lease) return Fail(EWorldSubsystemError::ProviderUnavailable, Descriptor.Provider.ToString());
			}
			Selected.push_back({Descriptor, std::move(Lease)});
		}
		std::ranges::sort(Selected, [](const auto& A, const auto& B) {
			return A.Descriptor.Type->GetQualifiedName().ToString() < B.Descriptor.Type->GetQualifiedName().ToString();
		});
		for (const auto& Entry : Selected)
			for (DClass* Dependency : Entry.Descriptor.Dependencies)
				if (!std::ranges::any_of(Selected, [&](const auto& Candidate) { return Candidate.Descriptor.Type == Dependency; }))
					return Fail(EWorldSubsystemError::MissingDependency, Entry.Descriptor.Type->GetQualifiedName().ToString());
		while (!Selected.empty())
		{
			auto Next = std::ranges::find_if(Selected, [&](const auto& Entry) {
				return std::ranges::all_of(Entry.Descriptor.Dependencies, [&](DClass* Dependency) {
					return std::ranges::any_of(Entries, [&](const auto& Done) { return Done.Descriptor.Type == Dependency; });
				});
			});
			if (Next == Selected.end()) return Fail(EWorldSubsystemError::DependencyCycle, "World subsystem dependencies contain a cycle.");
			Entries.push_back(std::move(*Next));
			Selected.erase(Next);
		}
		for (size_t Index = 0; Index < Entries.size(); ++Index)
		{
			auto* Object = Cast<DWorldSubsystem>(NewObject(Entries[Index].Descriptor.Type, &World, {}, EObjectFlags::Transient));
			if (!Object) return Fail(EWorldSubsystemError::InitializationFailed, "Subsystem construction failed.");
			Entries[Index].Object = Object;
			Object->ProviderLease = Entries[Index].Lease;
			Object->WorkGate = std::make_shared<FWorldSubsystemWorkGate>();
			Object->WorkGate->ProviderLease = Entries[Index].Lease;
			Object->WorkGate->RuntimeLease = FModuleManager::Get().AcquireCodeLease("Engine");
			FWorldSubsystemResult Result;
			++World.SubsystemCallbackDepth;
			try { Result = Object->Initialize(); }
			catch (...) { Result = {EWorldSubsystemError::InitializationFailed, "Subsystem Initialize threw an exception."}; }
			--World.SubsystemCallbackDepth;
			if (!Result) return Fail(Result.Error, std::move(Result.Message));
			Entries[Index].bInitialized = true;
			if (World.bShutdownRequested) return Fail(EWorldSubsystemError::Aborted, "World retired during subsystem initialization.");
		}
		State = EWorldSubsystemState::Ready;
		return {};
	}

	auto FWorldSubsystemCollection::CloseWork() -> void
	{
		for (auto& Entry : Entries)
			if (auto* Object = Entry.Object.Get(); Object && Object->WorkGate) Object->WorkGate->Cancellation.RequestCancellation();
	}

	auto FWorldSubsystemCollection::Shutdown() -> void
	{
		require(!GIsGameThreadIdInitialized || IsInGameThread());
		if (State == EWorldSubsystemState::Shutdown || State == EWorldSubsystemState::ShuttingDown) return;
		State = EWorldSubsystemState::ShuttingDown;
		CloseWork();
		for (size_t Index = Entries.size(); Index-- > 0;)
		{
			if (auto* Object = Entries[Index].Object.Get())
			{
				++World.SubsystemCallbackDepth;
				Object->Deinitialize();
				--World.SubsystemCallbackDepth;
				Entries[Index].bInitialized = false;
				MarkObjectHierarchyAsGarbage(Object);
			}
		}
		Entries.clear();
		State = EWorldSubsystemState::Shutdown;
	}

	auto FWorldSubsystemCollection::AddReferencedObjects(FReferenceCollector& Collector) -> void
	{
		for (auto& Entry : Entries)
		{
			DObject* Object = Entry.Object.Get();
			Collector.AddReferencedObject(Object);
		}
	}

	auto FWorldSubsystemCollection::BeginPlay() -> void
	{
		for (size_t Index = 0; Index < Entries.size() && World.CanDispatchSubsystems(); ++Index)
		{
			Entries[Index].bPlaying = true;
			++World.SubsystemCallbackDepth;
			Entries[Index].Object->OnWorldBeginPlay();
			--World.SubsystemCallbackDepth;
		}
	}
	auto FWorldSubsystemCollection::EndPlay() -> void
	{
		for (size_t Index = Entries.size(); Index-- > 0;)
		{
			if (!std::exchange(Entries[Index].bPlaying, false)) continue;
			++World.SubsystemCallbackDepth;
			Entries[Index].Object->OnWorldEndPlay();
			--World.SubsystemCallbackDepth;
		}
	}
	auto FWorldSubsystemCollection::LevelChanged(DLevel& Level, bool bAttached) -> void
	{
		for (size_t Offset = 0; Offset < Entries.size(); ++Offset)
		{
			if (bAttached && !World.CanDispatchSubsystems()) break;
			const size_t Index = bAttached ? Offset : Entries.size() - Offset - 1;
			if (!bAttached && !Entries[Index].bAttached) continue;
			Entries[Index].bAttached = bAttached;
			++World.SubsystemCallbackDepth;
			if (bAttached) Entries[Index].Object->OnLevelAttached(Level);
			else Entries[Index].Object->OnLevelDetached(Level);
			--World.SubsystemCallbackDepth;
		}
	}
	auto FWorldSubsystemCollection::StartTick() -> void
	{
		for (auto& Entry : Entries) Entry.bFrameTickEnabled = Entry.Object->IsTickEnabled();
	}
	auto FWorldSubsystemCollection::Tick(ETickingGroup Group, float DeltaSeconds, bool bGameplay) -> void
	{
		const bool bEditor = World.GetWorldType() == EWorldType::Editor || World.GetWorldType() == EWorldType::Preview;
		for (size_t Index = 0; Index < Entries.size() && World.CanDispatchSubsystems(); ++Index)
		{
			if (bGameplay && !World.HasBegunPlay()) break;
			const auto& Entry = Entries[Index];
			if (!Entry.bFrameTickEnabled || !Entry.Descriptor.bTick || Entry.Descriptor.TickGroup != Group) continue;
			if (!bGameplay && !(bEditor && Entry.Descriptor.bTickInEditorAndPreview)) continue;
			++World.SubsystemCallbackDepth;
			Entry.Object->Tick(DeltaSeconds);
			--World.SubsystemCallbackDepth;
		}
	}
}
