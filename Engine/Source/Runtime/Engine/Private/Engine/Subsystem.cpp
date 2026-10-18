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
		struct FRegistrationEntry { uint64 Identity; DClass* Scope; FSubsystemRegistrationSnapshot Snapshot; };
		auto Registry() -> std::vector<FRegistrationEntry>&
		{
			// Tokens can retire during static teardown, after ordinary static objects.
			static auto* Entries = new std::vector<FRegistrationEntry>();
			return *Entries;
		}
		uint64 NextRegistration = 0;
		uint64 PublicationRevision = 0;
	}

	FSubsystemRegistration::FSubsystemRegistration(DClass* Scope, FSubsystemDescriptor Descriptor, std::any Policy)
	{
		require(!GIsGameThreadIdInitialized || IsInGameThread());
		Identity = ++NextRegistration;
		Descriptor.RegistrationIdentity = Identity;
		Registry().push_back({Identity, Scope, {std::move(Descriptor), std::move(Policy)}});
		++PublicationRevision;
	}
	FSubsystemRegistration::~FSubsystemRegistration()
	{
		require(!GIsGameThreadIdInitialized || IsInGameThread());
		std::erase_if(Registry(), [this](const auto& Entry) { return Entry.Identity == Identity; });
		++PublicationRevision;
	}
	auto FSubsystemRegistration::GetRevision() -> uint64
	{
		require(!GIsGameThreadIdInitialized || IsInGameThread());
		return PublicationRevision;
	}
	auto FSubsystemRegistration::Snapshot(DClass* Scope) -> std::vector<FSubsystemRegistrationSnapshot>
	{
		require(!GIsGameThreadIdInitialized || IsInGameThread());
		std::vector<FSubsystemRegistrationSnapshot> Result;
		for (const auto& Entry : Registry()) if (Entry.Scope == Scope) Result.push_back(Entry.Snapshot);
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

	auto FSubsystemCollection::Initialize(std::vector<FSubsystemDescriptor> Descriptors) -> FSubsystemResult
	{
		require(!GIsGameThreadIdInitialized || IsInGameThread());
		if (State != ESubsystemState::Uninitialized)
			return {ESubsystemError::InvalidState, "Subsystem initialization is one-shot."};
		State = ESubsystemState::Initializing;
		bInitializing = true;
		FGarbageCollectionDeferralScope Deferral;
		auto Fail = [&](ESubsystemError Error, std::string Message) {
			bInitializing = false;
			Shutdown();
			State = ESubsystemState::Failed;
			return FSubsystemResult{Error, std::move(Message)};
		};
		try
		{
			std::vector<FEntry> Selected;
			for (const auto& Descriptor : Descriptors)
			{
				if (!Descriptor.Type || !CanConstructObjectOfClass(Descriptor.Type, Scope))
					return Fail(ESubsystemError::InvalidDescriptor, (Descriptor.Type ? Descriptor.Type->GetQualifiedName().ToString() : "<null>") + " is not a concrete service in scope " + Scope->GetQualifiedName().ToString());
				if (std::ranges::any_of(Selected, [&](const auto& Entry) { return Entry.Descriptor.Type == Descriptor.Type; }))
					return Fail(ESubsystemError::DuplicateType, Descriptor.Type->GetQualifiedName().ToString());
				if (!Descriptor.Provider.IsNone() && !FModuleManager::Get().IsModuleLoaded(Descriptor.Provider))
					return Fail(ESubsystemError::ProviderUnavailable, Descriptor.Provider.ToString());
				Selected.push_back({Descriptor});
			}
			std::ranges::sort(Selected, [](const auto& A, const auto& B) {
				return A.Descriptor.Type->GetQualifiedName().ToString() < B.Descriptor.Type->GetQualifiedName().ToString();
			});
			for (const auto& Entry : Selected)
				for (DClass* Dependency : Entry.Descriptor.Dependencies)
				{
					if (Dependency && !Dependency->IsChildOf(Scope))
						return Fail(ESubsystemError::InvalidDescriptor, Entry.Descriptor.Type->GetQualifiedName().ToString() + " has wrong-scope dependency " + Dependency->GetQualifiedName().ToString());
					if (!std::ranges::any_of(Selected, [&](const auto& Candidate) { return Candidate.Descriptor.Type == Dependency; }))
						return Fail(ESubsystemError::MissingDependency, Entry.Descriptor.Type->GetQualifiedName().ToString() + " requires " + (Dependency ? Dependency->GetQualifiedName().ToString() : "<null>"));
				}
			while (!Selected.empty())
			{
				auto Next = std::ranges::find_if(Selected, [&](const auto& Entry) {
					return std::ranges::all_of(Entry.Descriptor.Dependencies, [&](DClass* Dependency) {
						return std::ranges::any_of(Entries, [&](const auto& Done) { return Done.Descriptor.Type == Dependency; });
					});
				});
				if (Next == Selected.end())
				{
					std::string Message = "Subsystem dependency cycle blocks:";
					for (const auto& Entry : Selected) Message += " " + Entry.Descriptor.Type->GetQualifiedName().ToString();
					return Fail(ESubsystemError::DependencyCycle, std::move(Message));
				}
				Entries.push_back(std::move(*Next));
				Selected.erase(Next);
			}
			for (size_t Index = 0; Index < Entries.size(); ++Index)
			{
				if (bClosed) return Fail(ESubsystemError::Aborted, "Host retired before subsystem construction.");
				auto* Object = Cast<DSubsystem>(NewObject(Entries[Index].Descriptor.Type, &Host, {}, EObjectFlags::Transient));
				if (!Object) return Fail(ESubsystemError::InitializationFailed, "Subsystem construction failed.");
				Entries[Index].Object = Object;
				Object->WorkGate = std::make_shared<FSubsystemWorkGate>();
				if (bClosed) return Fail(ESubsystemError::Aborted, "Host retired during subsystem construction.");
				FSubsystemResult Result;
				try { Result = Object->Initialize(); }
				catch (...) { Result = {ESubsystemError::InitializationFailed, "Subsystem Initialize threw an exception."}; }
				if (!Result) return Fail(Result.Error, std::move(Result.Message));
				Entries[Index].bInitialized = true;
				if (bClosed) return Fail(ESubsystemError::Aborted, "Host retired during subsystem initialization.");
			}
			bInitializing = false;
			if (bClosed) return Fail(ESubsystemError::Aborted, "Host retired during subsystem initialization.");
			State = ESubsystemState::Ready;
			return {};
		}
		catch (...) { return Fail(ESubsystemError::InitializationFailed, "Subsystem construction or initialization threw an exception."); }
	}

	auto FSubsystemCollection::Reconcile(std::vector<FSubsystemDescriptor> Descriptors) -> FSubsystemResult
	{
		require(!GIsGameThreadIdInitialized || IsInGameThread());
		if (State != ESubsystemState::Ready || bInitializing || bClosed)
			return {ESubsystemError::InvalidState, "Subsystem reconciliation requires an idle, ready collection."};
		bInitializing = true;
		struct FReset { bool& Flag; ~FReset() { Flag = false; } } Reset{bInitializing};
		FGarbageCollectionDeferralScope Deferral;
		FSubsystemResult Diagnostic;
		auto Report = [&](ESubsystemError Error, std::string Message) {
			if (Diagnostic) Diagnostic = {Error, std::move(Message)};
		};
		std::map<DClass*, size_t> Counts;
		for (const auto& Descriptor : Descriptors) ++Counts[Descriptor.Type];
		std::erase_if(Descriptors, [&](const auto& Descriptor) {
			if (Counts[Descriptor.Type] != 1) { Report(ESubsystemError::DuplicateType, "Duplicate dynamic subsystem type."); return true; }
			if (!Descriptor.Type || !CanConstructObjectOfClass(Descriptor.Type, Scope))
			{ Report(ESubsystemError::InvalidDescriptor, "Invalid dynamic subsystem type or scope."); return true; }
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
					if (!Types.contains(Dependency))
					{ Report(ESubsystemError::MissingDependency, "Dynamic subsystem dependency is unavailable."); Changed = true; return true; }
				return false;
			});
		} while (Changed);
		std::ranges::sort(Descriptors, [](const auto& A, const auto& B) {
			return A.Type->GetQualifiedName().ToString() < B.Type->GetQualifiedName().ToString();
		});
		std::vector<FSubsystemDescriptor> Ordered;
		while (!Descriptors.empty())
		{
			auto Next = std::ranges::find_if(Descriptors, [&](const auto& Descriptor) {
				return std::ranges::all_of(Descriptor.Dependencies, [&](auto* Dependency) {
					return std::ranges::any_of(Ordered, [&](const auto& Done) { return Done.Type == Dependency; });
				});
			});
			if (Next == Descriptors.end()) { Report(ESubsystemError::DependencyCycle, "Dynamic subsystem dependency cycle."); break; }
			Ordered.push_back(std::move(*Next)); Descriptors.erase(Next);
		}
		auto SameRegistration = [&](const FEntry& Entry) {
			return std::ranges::any_of(Ordered, [&](const auto& Descriptor) {
				return Descriptor.Type == Entry.Descriptor.Type && Descriptor.RegistrationIdentity == Entry.Descriptor.RegistrationIdentity;
			});
		};
		std::set<DClass*> Retiring;
		for (const auto& Entry : Entries) if (!SameRegistration(Entry)) Retiring.insert(Entry.Descriptor.Type);
		do
		{
			Changed = false;
			for (const auto& Entry : Entries)
				if (std::ranges::any_of(Entry.Descriptor.Dependencies, [&](auto* Dependency) { return Retiring.contains(Dependency); }))
					Changed |= Retiring.insert(Entry.Descriptor.Type).second;
		} while (Changed);
		auto Keep = [&](const FEntry& Entry) { return SameRegistration(Entry) && !Retiring.contains(Entry.Descriptor.Type); };
		for (auto& Entry : Entries)
			if (!Keep(Entry) && Entry.Object && Entry.Object->WorkGate) { Entry.Object->WorkGate->Cancellation.RequestCancellation(); NotifyObjectReferencesInvalidated(*Entry.Object); }
		for (size_t Index = Entries.size(); Index-- > 0;)
			if (auto& Entry = Entries[Index]; !Keep(Entry) && Entry.Object)
			{
				Entry.Object->Deinitialize(); Entry.bInitialized = false;
				if (Entry.Object->WorkGate) Entry.Object->WorkGate->bDeinitialized = true;
				MarkObjectHierarchyAsGarbage(Entry.Object.Get());
			}
		std::erase_if(Entries, [&](const auto& Entry) { return !Keep(Entry); });
		const auto Base = Entries.size();
		auto Rollback = [&] {
			for (size_t Index = Entries.size(); Index-- > Base;)
				if (auto* Object = Entries[Index].Object.Get())
				{
					if (Object->WorkGate) { Object->WorkGate->Cancellation.RequestCancellation(); NotifyObjectReferencesInvalidated(*Object); }
					Object->Deinitialize(); MarkObjectHierarchyAsGarbage(Object);
					if (Object->WorkGate) Object->WorkGate->bDeinitialized = true;
				}
			Entries.resize(Base);
		};
		try
		{
			for (const auto& Descriptor : Ordered)
			{
				if (bClosed) { Rollback(); return {ESubsystemError::Aborted, "Host retired during dynamic subsystem update."}; }
				if (Find(Descriptor.Type)) continue;
				if (!Descriptor.Provider.IsNone() && !FModuleManager::Get().IsModuleLoaded(Descriptor.Provider))
				{ Rollback(); return {ESubsystemError::ProviderUnavailable, Descriptor.Provider.ToString()}; }
				Entries.push_back({Descriptor});
				auto* Object = Cast<DSubsystem>(NewObject(Descriptor.Type, &Host, {}, EObjectFlags::Transient));
				if (!Object) { Rollback(); return {ESubsystemError::InitializationFailed, "Dynamic subsystem construction failed."}; }
				auto& Entry = Entries.back(); Entry.Object = Object;
				Object->WorkGate = std::make_shared<FSubsystemWorkGate>();
				if (bClosed) { Rollback(); return {ESubsystemError::Aborted, "Host retired during dynamic subsystem construction."}; }
				auto Result = Object->Initialize();
				if (!Result) { Rollback(); return Result; }
				Entry.bInitialized = true;
			}
			if (bClosed) { Rollback(); return {ESubsystemError::Aborted, "Host retired during dynamic subsystem initialization."}; }
		}
		catch (...) { Rollback(); return {ESubsystemError::InitializationFailed, "Dynamic subsystem initialization threw."}; }
		return Diagnostic;
	}

	auto FSubsystemCollection::CloseWork() -> void
	{
		require(!GIsGameThreadIdInitialized || IsInGameThread());
		bClosed = true;
		for (auto& Entry : Entries)
			if (auto* Object = Entry.Object.Get(); Object && Object->WorkGate) { Object->WorkGate->Cancellation.RequestCancellation(); NotifyObjectReferencesInvalidated(*Object); }
	}

	auto FSubsystemCollection::Shutdown() -> void
	{
		require(!GIsGameThreadIdInitialized || IsInGameThread());
		CloseWork();
		if (bInitializing || State == ESubsystemState::Shutdown || State == ESubsystemState::ShuttingDown) return;
		FGarbageCollectionDeferralScope Deferral;
		State = ESubsystemState::ShuttingDown;
		CloseWork();
		for (size_t Index = Entries.size(); Index-- > 0;)
		{
			if (auto* Object = Entries[Index].Object.Get())
			{
				Object->Deinitialize();
				if (Object->WorkGate) Object->WorkGate->bDeinitialized = true;
				Entries[Index].bInitialized = false;
				MarkObjectHierarchyAsGarbage(Object);
			}
		}
		Entries.clear();
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
