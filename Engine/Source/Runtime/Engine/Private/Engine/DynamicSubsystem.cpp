#include "Engine/DynamicSubsystem.h"

#include "CoreGlobals.h"
#include "Threading/RunnableThread.h"
#include "DObject/Class.h"
#include "DObject/ObjectLifecycle.h"
#include "Modules/ModuleManager.h"

namespace Durin
{
	DDynamicSubsystem::DDynamicSubsystem(const FObjectInitializer& Initializer) : Super(Initializer) {}

	FDynamicSubsystemCollection::FDynamicSubsystemCollection(DObject& InHost, DClass* InScope)
		: FSubsystemCollection(InHost, InScope)
	{
		require(InScope && InScope->IsChildOf(DDynamicSubsystem::StaticClass()));
	}
	FDynamicSubsystemCollection::~FDynamicSubsystemCollection() = default;

	auto FDynamicSubsystemCollection::Reconcile(std::vector<FSubsystemDescriptor> Descriptors) -> FSubsystemResult
	{
		require(!GIsGameThreadIdInitialized || IsInGameThread());
		if (State != ESubsystemState::Ready || bPopulating || bClosed)
			return {ESubsystemError::InvalidState, "Subsystem reconciliation requires an idle, ready collection."};
		bPopulating = true;
		struct FReset { bool& Flag; ~FReset() { Flag = false; } } Reset{bPopulating};
		FGarbageCollectionDeferralScope Deferral;
		auto SameRegistration = [](const auto& Descriptors, const FEntry& Entry) {
			return std::ranges::count(Descriptors, Entry.Descriptor.Type, &FSubsystemDescriptor::Type) == 1
				&& std::ranges::any_of(Descriptors, [&](const auto& Descriptor) {
					return Descriptor.Type == Entry.Descriptor.Type && Descriptor.RegistrationIdentity == Entry.Descriptor.RegistrationIdentity;
				});
		};
		// A token change never authorizes replacing a live instance, even without consumers.
		for (const auto& Entry : Entries)
			if (!SameRegistration(Descriptors, Entry)
				&& std::ranges::count(Descriptors, Entry.Descriptor.Type, &FSubsystemDescriptor::Type) == 1)
				return {ESubsystemError::InvalidState, Entry.Descriptor.Type->GetQualifiedName().ToString()
					+ " must be removed in a separate synchronization before registering it again."};
		try
		{
			std::set<DClass*> Types;
			for (const auto& Descriptor : Descriptors)
			{
				if (auto Result = ValidateType(Descriptor); !Result)
					return Result;
				if (!Types.insert(Descriptor.Type).second)
					return {ESubsystemError::DuplicateType, Descriptor.Type->GetQualifiedName().ToString()};
				if (!Descriptor.Provider.IsNone() && !FModuleManager::Get().IsModuleLoaded(Descriptor.Provider))
					return {ESubsystemError::ProviderUnavailable, Descriptor.Provider.ToString()};
			}
			std::vector<FSubsystemDescriptor> Selected;
			for (const auto& Descriptor : Descriptors)
			{
				if (std::ranges::any_of(Entries, [&](const auto& Entry) {
					return Entry.Descriptor.Type == Descriptor.Type && Entry.Descriptor.RegistrationIdentity == Descriptor.RegistrationIdentity;
				})) { Selected.push_back(Descriptor); continue; }
				const auto* Default = static_cast<const DSubsystem*>(Descriptor.Type->GetDefaultObject());
				if (!Default) return {ESubsystemError::InvalidDescriptor, "Dynamic subsystem requires a class default object."};
				const bool bCreate = Default->ShouldCreateSubsystem(&Host);
				if (bClosed) return {ESubsystemError::Aborted, "Host shut down during subsystem eligibility selection."};
				if (bCreate) Selected.push_back(Descriptor);
			}
			Descriptors = std::move(Selected);
		}
		catch (...) { return {ESubsystemError::InitializationFailed, "Subsystem eligibility selection threw."}; }
		if (bClosed) return {ESubsystemError::Aborted, "Host shut down during subsystem eligibility selection."};
		std::vector<FSubsystemDescriptor> Ordered;
		if (auto Result = OrderSelectedDescriptors(std::move(Descriptors), Ordered); !Result)
			return Result;
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
		return {};
	}

	auto FDynamicSubsystemCollection::CanRemoveProvider(FName Provider) const -> bool
	{
		auto Descriptors = FSubsystemRegistration::Snapshot(Scope);
		std::erase_if(Descriptors, [&](const auto& Descriptor) { return Descriptor.Provider == Provider; });
		for (const auto& Descriptor : Descriptors)
		{
			if (!ValidateType(Descriptor)) return false;
			if (!Descriptor.Provider.IsNone() && !FModuleManager::Get().IsModuleLoaded(Descriptor.Provider)) return false;
		}
		std::vector<FSubsystemDescriptor> Ordered;
		return static_cast<bool>(OrderSelectedDescriptors(std::move(Descriptors), Ordered));
	}

}
