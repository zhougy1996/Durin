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
		const auto& Candidates = Initialization->Descriptors;
		if (const auto Found = std::ranges::find(Candidates, Type, &FSubsystemDescriptor::Type); Found != Candidates.end())
			return AddAndInitializeSubsystem(*Found);
		auto Registered = FSubsystemRegistration::GetRegistrations(Scope);
		const auto Found = std::ranges::find(Registered, Type, &FSubsystemDescriptor::Type);
		return Found == Registered.end() ? nullptr : AddAndInitializeSubsystem(*Found);
	}

	auto FSubsystemCollection::AddAndInitializeSubsystem(const FSubsystemDescriptor& Descriptor) -> DSubsystem*
	{
		requiref(Initialization, "Subsystem creation requires an initialization context.");
		if (auto* Existing = Find(Descriptor.Type)) return Existing;
		requiref(std::ranges::find(Initialization->Stack, Descriptor.Type) == Initialization->Stack.end(), "Recursive subsystem dependency initialization.");
		ValidateType(Descriptor);
		requiref(Descriptor.Provider.IsNone() || FModuleManager::Get().IsModuleLoaded(Descriptor.Provider), "Subsystem provider must be active.");
		const auto* Default = static_cast<const DSubsystem*>(Descriptor.Type->GetDefaultObject());
		requiref(Default, "Subsystem requires a class default object.");
		if (!Default->ShouldCreateSubsystem(&Host)) return nullptr;
		Entries.push_back({Descriptor});
		auto Entry = [&] { return std::ranges::find(Entries, Descriptor.Type, [](const auto& Value) { return Value.Descriptor.Type; }); };
		auto Cleanup = [&] {
			const auto It = Entry();
			RemoveAndDeinitializeSubsystem(static_cast<size_t>(It - Entries.begin()));
		};
		DSubsystem* Object = nullptr;
		try
		{
			Object = static_cast<DSubsystem*>(NewObject(Descriptor.Type, &Host, {}, EObjectFlags::Transient));
			// Dependencies can grow Entries; reacquire the entry after user callbacks.
			Entry()->Object = Object;
			Initialization->Stack.push_back(Descriptor.Type);
			struct FPop { std::vector<DClass*>& Stack; ~FPop() { Stack.pop_back(); } } Pop{Initialization->Stack};
			Object->Initialize(*this);
		}
		catch (...) { Cleanup(); throw; }
		// Completion order places dependencies before consumers for reverse cleanup.
		const auto It = Entry();
		auto Completed = std::move(*It);
		Entries.erase(It); Completed.bInitialized = true; Entries.push_back(std::move(Completed));
		return Object;
	}

	auto FSubsystemCollection::AddAndInitializeSubsystems(std::vector<FSubsystemDescriptor> Descriptors) -> void
	{
		require(!GIsGameThreadIdInitialized || IsInGameThread());
		requiref(IsInitialized() && !bPopulating, "Subsystem additions require an idle, initialized collection.");
		FGarbageCollectionDeferralScope Deferral;
		FInitialization Context{std::move(Descriptors)};
		Initialization = &Context; bPopulating = true;
		struct FReset { FSubsystemCollection& Self; ~FReset() { Self.Initialization = nullptr; Self.bPopulating = false; } } Reset{*this};
		for (const auto& Descriptor : Context.Descriptors)
		{
			AddAndInitializeSubsystem(Descriptor);
		}
	}

	auto FSubsystemCollection::RemoveAndDeinitializeSubsystem(size_t Index) -> void
	{
		auto& Entry = Entries[Index];
		if (auto* Object = Entry.Object.Get())
		{
			Object->Deinitialize();
		}
		Entries.erase(Entries.begin() + Index);
	}

	auto FSubsystemCollection::Initialize(std::vector<FSubsystemDescriptor> Descriptors) -> void
	{
		require(!GIsGameThreadIdInitialized || IsInGameThread());
		requiref(!bInitialized && !bDeinitComplete && !bPopulating, "Subsystem initialization is one-shot.");
		bInitialized = true;
		try { AddAndInitializeSubsystems(std::move(Descriptors)); }
		catch (...) { Deinitialize(); throw; }
	}

	auto FSubsystemCollection::Deinitialize() -> void
	{
		require(!GIsGameThreadIdInitialized || IsInGameThread());
		if (bDeinitComplete) return;
		requiref(!bPopulating, "Subsystem cleanup requires an idle collection; request host shutdown from callbacks.");
		FGarbageCollectionDeferralScope Deferral;
		bDeinitComplete = true;
		bInitialized = false;
		for (size_t Index = Entries.size(); Index-- > 0;) RemoveAndDeinitializeSubsystem(Index);
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
