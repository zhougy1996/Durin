#include "Engine/Subsystem.h"
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
		struct FRegistrationEntry { uint64 Identity; DClass* Scope; FSubsystemDescriptor Descriptor; };
		auto Registry() -> std::vector<FRegistrationEntry>&
		{
			// Tokens can unregister during static teardown, after ordinary static objects.
			static auto* Entries = new std::vector<FRegistrationEntry>();
			return *Entries;
		}
		uint64 NextRegistration = 0;
	}

	FSubsystemRegistration::FSubsystemRegistration(DClass* Scope, FSubsystemDescriptor Descriptor)
	{
		require(!GIsGameThreadIdInitialized || IsInGameThread());
		Identity = ++NextRegistration;
		Registry().push_back({Identity, Scope, std::move(Descriptor)});
	}
	FSubsystemRegistration::~FSubsystemRegistration()
	{
		require(!GIsGameThreadIdInitialized || IsInGameThread());
		std::erase_if(Registry(), [this](const auto& Entry) { return Entry.Identity == Identity; });
	}
	auto FSubsystemRegistration::GetRegistrations(DClass* Scope) -> std::vector<FSubsystemDescriptor>
	{
		require(!GIsGameThreadIdInitialized || IsInGameThread());
		std::vector<FSubsystemDescriptor> Result;
		for (const auto& Entry : Registry()) if (Entry.Scope == Scope) Result.push_back(Entry.Descriptor);
		return Result;
	}

	DSubsystem::DSubsystem(const FObjectInitializer& Initializer) : Super(Initializer) {}
	FSubsystemCollection::FSubsystemCollection(DObject& InHost, DClass* InScope) : Host(InHost), Scope(InScope) {}
	FSubsystemCollection::~FSubsystemCollection() = default;
	auto FSubsystemCollection::Find(DClass* Type) const -> DSubsystem*
	{
		require(!GIsGameThreadIdInitialized || IsInGameThread());
		for (const FEntry& Entry : Entries)
			if (Entry.Descriptor.Type == Type && Entry.bInitialized) return Entry.Object.Get();
		return nullptr;
	}

	auto FSubsystemCollection::ValidateType(const FSubsystemDescriptor& Descriptor) const -> void
	{
		requiref(CanConstructObjectOfClass(Descriptor.Type, Scope), "Subsystem must be a concrete service in scope {}.", Scope->GetQualifiedName().ToString());
	}

	auto FSubsystemCollection::InitializeDependency(DClass* Type) -> DSubsystem*
	{
		requiref(Initialization && !Initialization->Stack.empty(), "InitializeDependency is only valid inside subsystem Initialize.");
		ValidateType({Type});
		if (auto* Existing = Find(Type)) return Existing;
		auto Candidates = Initialization->Descriptors;
		auto Registered = FSubsystemRegistration::GetRegistrations(Scope);
		Candidates.insert(Candidates.end(), Registered.begin(), Registered.end());
		const auto Found = std::ranges::find(Candidates, Type, &FSubsystemDescriptor::Type);
		return Found == Candidates.end() ? nullptr : AddAndInitializeSubsystem(*Found);
	}

	auto FSubsystemCollection::AddAndInitializeSubsystem(const FSubsystemDescriptor& Descriptor) -> DSubsystem*
	{
		if (auto* Existing = Find(Descriptor.Type)) return Existing;
		if (!Initialization)
		{
			AddAndInitializeSubsystems({Descriptor});
			return Find(Descriptor.Type);
		}
		if (bClosed) return nullptr;
		requiref(std::ranges::find(Initialization->Stack, Descriptor.Type) == Initialization->Stack.end(), "Recursive subsystem dependency initialization.");
		ValidateType(Descriptor);
		requiref(Descriptor.Provider.IsNone() || FModuleManager::Get().IsModuleLoaded(Descriptor.Provider), "Subsystem provider must be active.");
		const auto* Default = static_cast<const DSubsystem*>(Descriptor.Type->GetDefaultObject());
		requiref(Default, "Subsystem requires a class default object.");
		if (!Default->ShouldCreateSubsystem(&Host) || bClosed) return nullptr;
		ValidateSelectedDescriptor(Descriptor);
		Entries.push_back({Descriptor});
		auto Entry = [&] { return std::ranges::find(Entries, Descriptor.Type, [](const auto& Value) { return Value.Descriptor.Type; }); };
		auto Cleanup = [&] {
			const auto It = Entry();
			InvalidateEntryReferences(*It);
			RemoveAndDeinitializeSubsystem(static_cast<size_t>(It - Entries.begin()));
		};
		DSubsystem* Object = nullptr;
		try
		{
			Object = static_cast<DSubsystem*>(NewObject(Descriptor.Type, &Host, {}, EObjectFlags::Transient));
			// Dependencies can grow Entries; reacquire the entry after user callbacks.
			Entry()->Object = Object;
			if (!bClosed)
			{
				Initialization->Stack.push_back(Descriptor.Type);
				struct FPop { std::vector<DClass*>& Stack; ~FPop() { Stack.pop_back(); } } Pop{Initialization->Stack};
				Object->Initialize(*this);
			}
		}
		catch (...) { Cleanup(); throw; }
		if (bClosed) { Cleanup(); return nullptr; }
		// Completion order places dependencies before consumers for reverse cleanup.
		const auto It = Entry();
		auto Completed = std::move(*It);
		Entries.erase(It); Completed.bInitialized = true; Entries.push_back(std::move(Completed));
		return Object;
	}

	auto FSubsystemCollection::AddAndInitializeSubsystems(std::vector<FSubsystemDescriptor> Descriptors) -> void
	{
		require(!GIsGameThreadIdInitialized || IsInGameThread());
		requiref(!bPopulating && (State == ESubsystemState::Ready || State == ESubsystemState::Initializing), "Subsystem additions require an idle, live collection.");
		if (bClosed) return;
		FGarbageCollectionDeferralScope Deferral;
		FInitialization Context{std::move(Descriptors)};
		Initialization = &Context; bPopulating = true;
		struct FReset { FSubsystemCollection& Self; ~FReset() { Self.Initialization = nullptr; Self.bPopulating = false; } } Reset{*this};
		for (const auto& Descriptor : Context.Descriptors)
		{
			if (bClosed) break;
			AddAndInitializeSubsystem(Descriptor);
		}
	}

	auto FSubsystemCollection::InvalidateEntryReferences(FEntry& Entry) -> void
	{
		if (auto* Object = Entry.Object.Get(); Object && !std::exchange(Entry.bReferencesInvalidated, true))
		{
			NotifyObjectReferencesInvalidated(*Object);
		}
	}

	auto FSubsystemCollection::RemoveAndDeinitializeSubsystem(size_t Index) -> void
	{
		auto& Entry = Entries[Index];
		if (auto* Object = Entry.Object.Get())
		{
			Object->Deinitialize();
			Entry.bInitialized = false;
		}
		Entries.erase(Entries.begin() + Index);
	}

	auto FSubsystemCollection::Initialize(std::vector<FSubsystemDescriptor> Descriptors) -> void
	{
		require(!GIsGameThreadIdInitialized || IsInGameThread());
		requiref(State == ESubsystemState::Uninitialized, "Subsystem initialization is one-shot.");
		State = ESubsystemState::Initializing;
		try { AddAndInitializeSubsystems(std::move(Descriptors)); }
		catch (...) { Deinitialize(); throw; }
		if (bClosed) Deinitialize();
		else State = ESubsystemState::Ready;
	}

	auto FSubsystemCollection::RequestDeinitialize() -> void
	{
		require(!GIsGameThreadIdInitialized || IsInGameThread());
		bClosed = true;
		if (bInvalidatingReferences) return;
		FGarbageCollectionDeferralScope Deferral;
		{
			bInvalidatingReferences = true;
			struct FReset { bool& Flag; ~FReset() { Flag = false; } } Reset{bInvalidatingReferences};
			for (auto& Entry : Entries) InvalidateEntryReferences(Entry);
		}
		// An invalidation observer may request cleanup, but cannot mutate the active traversal.
		if (std::exchange(bDeinitializeRequested, false)) Deinitialize();
	}

	auto FSubsystemCollection::Deinitialize() -> void
	{
		require(!GIsGameThreadIdInitialized || IsInGameThread());
		if (bInvalidatingReferences) { bDeinitializeRequested = true; return; }
		if (State == ESubsystemState::Shutdown || State == ESubsystemState::ShuttingDown) return;
		if (bPopulating) { RequestDeinitialize(); return; }
		FGarbageCollectionDeferralScope Deferral;
		State = ESubsystemState::ShuttingDown;
		RequestDeinitialize();
		for (size_t Index = Entries.size(); Index-- > 0;) RemoveAndDeinitializeSubsystem(Index);
		State = ESubsystemState::Shutdown;
	}

	auto FSubsystemCollection::AddReferencedObjects(FReferenceCollector& Collector) -> void
	{
		for (auto& Entry : Entries)
		{
			DObject* Object = Entry.Object.Get();
			Collector.AddReferencedObject(Object);
		}
	}

}
