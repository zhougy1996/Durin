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
		struct FRegistrationEntry { DClass* Scope; FSubsystemDescriptor Descriptor; };
		auto Registry() -> std::vector<FRegistrationEntry>&
		{
			// Tokens can unregister during static teardown, after ordinary static objects.
			static auto* Entries = new std::vector<FRegistrationEntry>();
			return *Entries;
		}
		uint64 NextRegistration = 0;
		uint64 PublicationRevision = 0;
	}

	FSubsystemRegistration::FSubsystemRegistration(DClass* Scope, FSubsystemDescriptor Descriptor)
	{
		require(!GIsGameThreadIdInitialized || IsInGameThread());
		Identity = ++NextRegistration;
		Descriptor.RegistrationIdentity = Identity;
		Registry().push_back({Scope, std::move(Descriptor)});
		++PublicationRevision;
	}
	FSubsystemRegistration::~FSubsystemRegistration()
	{
		require(!GIsGameThreadIdInitialized || IsInGameThread());
		std::erase_if(Registry(), [this](const auto& Entry) { return Entry.Descriptor.RegistrationIdentity == Identity; });
		++PublicationRevision;
	}
	auto FSubsystemRegistration::GetRevision() -> uint64
	{
		require(!GIsGameThreadIdInitialized || IsInGameThread());
		return PublicationRevision;
	}
	auto FSubsystemRegistration::Snapshot(DClass* Scope) -> std::vector<FSubsystemDescriptor>
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

	auto FSubsystemCollection::ValidateType(const FSubsystemDescriptor& Descriptor) const -> FSubsystemResult
	{
		if (!CanConstructObjectOfClass(Descriptor.Type, Scope))
			return {ESubsystemError::InvalidDescriptor, (Descriptor.Type ? Descriptor.Type->GetQualifiedName().ToString() : "<null>") + " is not a concrete service in scope " + Scope->GetQualifiedName().ToString()};
		return {};
	}

	auto FSubsystemCollection::OrderSelectedDescriptors(std::vector<FSubsystemDescriptor> Descriptors,
		std::vector<FSubsystemDescriptor>& Ordered) const -> FSubsystemResult
	{
		Ordered.clear();
		FSubsystemResult Diagnostic;
		auto Report = [&](ESubsystemError Error, std::string Message) {
			if (Diagnostic) Diagnostic = {Error, std::move(Message)};
		};
		std::map<DClass*, size_t> Counts;
		for (const auto& Descriptor : Descriptors) ++Counts[Descriptor.Type];
		std::erase_if(Descriptors, [&](const auto& Descriptor) {
			if (Counts[Descriptor.Type] != 1) { Report(ESubsystemError::DuplicateType, Descriptor.Type->GetQualifiedName().ToString()); return true; }
			return false;
		});
		bool Changed;
		do
		{
			Changed = false;
			// Decide against an immutable pass; erase_if must not search its moving range.
			std::set<DClass*> Types;
			for (const auto& Descriptor : Descriptors) Types.insert(Descriptor.Type);
			std::erase_if(Descriptors, [&](const auto& Descriptor) {
				for (auto* Dependency : Descriptor.Dependencies)
				{
					if (Dependency && !Dependency->IsChildOf(Scope))
					{ Report(ESubsystemError::InvalidDescriptor, Descriptor.Type->GetQualifiedName().ToString() + " has wrong-scope dependency " + Dependency->GetQualifiedName().ToString()); Changed = true; return true; }
					if (!Types.contains(Dependency))
					{ Report(ESubsystemError::MissingDependency, Descriptor.Type->GetQualifiedName().ToString() + " requires " + (Dependency ? Dependency->GetQualifiedName().ToString() : "<null>")); Changed = true; return true; }
				}
				return false;
			});
		} while (Changed);
		std::ranges::sort(Descriptors, [](const auto& A, const auto& B) {
			return A.Type->GetQualifiedName().ToString() < B.Type->GetQualifiedName().ToString();
		});
		while (!Descriptors.empty())
		{
			auto Next = std::ranges::find_if(Descriptors, [&](const auto& Descriptor) {
				return std::ranges::all_of(Descriptor.Dependencies, [&](auto* Dependency) {
					return std::ranges::any_of(Ordered, [&](const auto& Done) { return Done.Type == Dependency; });
				});
			});
			if (Next == Descriptors.end())
			{
				std::string Message = "Subsystem dependency cycle blocks:";
				for (const auto& Descriptor : Descriptors) Message += " " + Descriptor.Type->GetQualifiedName().ToString();
				Report(ESubsystemError::DependencyCycle, std::move(Message));
				break;
			}
			Ordered.push_back(std::move(*Next)); Descriptors.erase(Next);
		}
		return Diagnostic;
	}

	auto FSubsystemCollection::AddAndInitializeSubsystem(const FSubsystemDescriptor& Descriptor) -> FSubsystemResult
	{
		if (bClosed) return {ESubsystemError::Aborted, "Host shut down before subsystem construction."};
		Entries.push_back({Descriptor});
		auto& Entry = Entries.back();
		// Allocate cleanup state first so a constructed service cannot lose it to allocation failure.
		auto WorkGate = std::make_shared<FSubsystemWorkGate>();
		auto* Object = static_cast<DSubsystem*>(NewObject(Entry.Descriptor.Type, &Host, {}, EObjectFlags::Transient));
		Entry.Object = Object;
		Object->WorkGate = std::move(WorkGate);
		if (bClosed) return {ESubsystemError::Aborted, "Host shut down during subsystem construction."};
		auto Result = Object->Initialize();
		if (!Result) return Result;
		Entry.bInitialized = true;
		if (bClosed) return {ESubsystemError::Aborted, "Host shut down during subsystem initialization."};
		return {};
	}

	auto FSubsystemCollection::CloseEntryWork(FEntry& Entry) -> void
	{
		if (auto* Object = Entry.Object.Get(); Object && Object->WorkGate && Object->WorkGate->IsOpen())
		{
			Object->WorkGate->Cancellation.Cancel();
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
			if (Object->WorkGate) Object->WorkGate->bDeinitialized = true;
		}
		Entries.erase(Entries.begin() + Index);
	}

	auto FSubsystemCollection::Initialize(std::vector<FSubsystemDescriptor> Descriptors) -> FSubsystemResult
	{
		require(!GIsGameThreadIdInitialized || IsInGameThread());
		if (State != ESubsystemState::Uninitialized)
			return {ESubsystemError::InvalidState, "Subsystem initialization is one-shot."};
		State = ESubsystemState::Initializing;
		bPopulating = true;
		FGarbageCollectionDeferralScope Deferral;
		auto Fail = [&](ESubsystemError Error, std::string Message) {
			bPopulating = false;
			Deinitialize();
			State = ESubsystemState::Failed;
			return FSubsystemResult{Error, std::move(Message)};
		};
		try
		{
			std::vector<FSubsystemDescriptor> Selected;
			for (const auto& Descriptor : Descriptors)
			{
				if (auto Result = ValidateType(Descriptor); !Result)
					return Fail(Result.Error, std::move(Result.Message));
				if (!Descriptor.Provider.IsNone() && !FModuleManager::Get().IsModuleLoaded(Descriptor.Provider))
					return Fail(ESubsystemError::ProviderUnavailable, Descriptor.Provider.ToString());
				const auto* Default = static_cast<const DSubsystem*>(Descriptor.Type->GetDefaultObject());
				if (!Default) return Fail(ESubsystemError::InvalidDescriptor, Descriptor.Type->GetQualifiedName().ToString() + " requires a class default object.");
				const bool bCreate = Default->ShouldCreateSubsystem(&Host);
				if (bClosed) return Fail(ESubsystemError::Aborted, "Host shut down during subsystem eligibility selection.");
				if (!bCreate) continue;
				if (auto Result = ValidateSelectedDescriptor(Descriptor); !Result)
					return Fail(Result.Error, std::move(Result.Message));
				Selected.push_back(Descriptor);
			}
			std::vector<FSubsystemDescriptor> Ordered;
			if (auto Result = OrderSelectedDescriptors(std::move(Selected), Ordered); !Result)
				return Fail(Result.Error, std::move(Result.Message));
			for (const auto& Descriptor : Ordered)
				if (auto Result = AddAndInitializeSubsystem(Descriptor); !Result)
					return Fail(Result.Error, std::move(Result.Message));
			bPopulating = false;
			State = ESubsystemState::Ready;
			return {};
		}
		catch (...) { return Fail(ESubsystemError::InitializationFailed, "Subsystem construction or initialization threw an exception."); }
	}

	auto FSubsystemCollection::Reconcile(std::vector<FSubsystemDescriptor> Descriptors) -> FSubsystemResult
	{
		require(!GIsGameThreadIdInitialized || IsInGameThread());
		if (State != ESubsystemState::Ready || bPopulating || bClosed)
			return {ESubsystemError::InvalidState, "Subsystem reconciliation requires an idle, ready collection."};
		bPopulating = true;
		struct FReset { bool& Flag; ~FReset() { Flag = false; } } Reset{bPopulating};
		FGarbageCollectionDeferralScope Deferral;
		FSubsystemResult Diagnostic;
		auto Report = [&](ESubsystemError Error, std::string Message) {
			if (Diagnostic) Diagnostic = {Error, std::move(Message)};
		};
		auto SameRegistration = [](const auto& Descriptors, const FEntry& Entry) {
			return std::ranges::count(Descriptors, Entry.Descriptor.Type, &FSubsystemDescriptor::Type) == 1
				&& std::ranges::any_of(Descriptors, [&](const auto& Descriptor) {
					return Descriptor.Type == Entry.Descriptor.Type && Descriptor.RegistrationIdentity == Entry.Descriptor.RegistrationIdentity;
				});
		};
		std::set<DClass*> Replacing;
		for (const auto& Entry : Entries)
			if (!SameRegistration(Descriptors, Entry)
				&& std::ranges::count(Descriptors, Entry.Descriptor.Type, &FSubsystemDescriptor::Type) == 1)
				Replacing.insert(Entry.Descriptor.Type);
		// Never leave an unchanged consumer bound to a replaced instance.
		for (const auto& Entry : Entries)
			if (SameRegistration(Descriptors, Entry))
				for (auto* Dependency : Entry.Descriptor.Dependencies)
					if (Replacing.contains(Dependency))
						return {ESubsystemError::InvalidState, Entry.Descriptor.Type->GetQualifiedName().ToString()
							+ " must be unregistered or explicitly re-registered before replacing " + Dependency->GetQualifiedName().ToString()};
		try
		{
			std::erase_if(Descriptors, [&](const auto& Descriptor) {
				if (bClosed) return false;
				if (auto Result = ValidateType(Descriptor); !Result)
				{ Report(Result.Error, std::move(Result.Message)); return true; }
				if (std::ranges::any_of(Entries, [&](const auto& Entry) {
					return Entry.Descriptor.Type == Descriptor.Type && Entry.Descriptor.RegistrationIdentity == Descriptor.RegistrationIdentity;
				})) return false;
				const auto* Default = static_cast<const DSubsystem*>(Descriptor.Type->GetDefaultObject());
				if (!Default) { Report(ESubsystemError::InvalidDescriptor, "Dynamic subsystem requires a class default object."); return true; }
				return !Default->ShouldCreateSubsystem(&Host);
			});
		}
		catch (...) { return {ESubsystemError::InitializationFailed, "Subsystem eligibility selection threw."}; }
		if (bClosed) return {ESubsystemError::Aborted, "Host shut down during subsystem eligibility selection."};
		std::vector<FSubsystemDescriptor> Ordered;
		if (auto Result = OrderSelectedDescriptors(std::move(Descriptors), Ordered); !Result)
			Report(Result.Error, std::move(Result.Message));
		std::set<DClass*> Removing;
		for (const auto& Entry : Entries) if (!SameRegistration(Ordered, Entry)) Removing.insert(Entry.Descriptor.Type);
		auto Keep = [&](const FEntry& Entry) { return !Removing.contains(Entry.Descriptor.Type); };
		// Close every removed subsystem gate before dependent cleanup can invoke user callbacks.
		for (auto& Entry : Entries) if (!Keep(Entry)) CloseEntryWork(Entry);
		for (size_t Index = Entries.size(); Index-- > 0;)
			if (!Keep(Entries[Index])) RemoveAndDeinitializeSubsystem(Index);
		const auto Base = Entries.size();
		auto Rollback = [&] {
			for (size_t Index = Base; Index < Entries.size(); ++Index) CloseEntryWork(Entries[Index]);
			for (size_t Index = Entries.size(); Index-- > Base;) RemoveAndDeinitializeSubsystem(Index);
		};
		try
		{
			for (const auto& Descriptor : Ordered)
			{
				if (bClosed) { Rollback(); return {ESubsystemError::Aborted, "Host shut down during dynamic subsystem update."}; }
				if (Find(Descriptor.Type)) continue;
				if (!Descriptor.Provider.IsNone() && !FModuleManager::Get().IsModuleLoaded(Descriptor.Provider))
				{ Rollback(); return {ESubsystemError::ProviderUnavailable, Descriptor.Provider.ToString()}; }
				if (auto Result = AddAndInitializeSubsystem(Descriptor); !Result) { Rollback(); return Result; }
			}
			if (bClosed) { Rollback(); return {ESubsystemError::Aborted, "Host shut down during dynamic subsystem initialization."}; }
		}
		catch (...) { Rollback(); return {ESubsystemError::InitializationFailed, "Dynamic subsystem initialization threw."}; }
		return Diagnostic;
	}

	auto FSubsystemCollection::CloseWork() -> void
	{
		require(!GIsGameThreadIdInitialized || IsInGameThread());
		bClosed = true;
		if (bClosingWork) return;
		FGarbageCollectionDeferralScope Deferral;
		{
			bClosingWork = true;
			struct FReset { bool& Flag; ~FReset() { Flag = false; } } Reset{bClosingWork};
			for (auto& Entry : Entries) CloseEntryWork(Entry);
		}
		// An invalidation observer may request cleanup, but cannot mutate the active traversal.
		if (std::exchange(bDeinitializeRequested, false)) Deinitialize();
	}

	auto FSubsystemCollection::Deinitialize() -> void
	{
		require(!GIsGameThreadIdInitialized || IsInGameThread());
		if (bClosingWork) { bDeinitializeRequested = true; return; }
		if (State == ESubsystemState::Shutdown || State == ESubsystemState::ShuttingDown) return;
		if (bPopulating) { CloseWork(); return; }
		FGarbageCollectionDeferralScope Deferral;
		State = ESubsystemState::ShuttingDown;
		CloseWork();
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
