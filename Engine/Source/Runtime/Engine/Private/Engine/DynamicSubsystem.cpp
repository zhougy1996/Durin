#include "Engine/DynamicSubsystem.h"
#include "CoreGlobals.h"
#include "Threading/RunnableThread.h"
#include "DObject/Class.h"
#include "DObject/ObjectLifecycle.h"

namespace Durin
{
	DDynamicSubsystem::DDynamicSubsystem(const FObjectInitializer& Initializer) : Super(Initializer) {}
	FDynamicSubsystemCollection::FDynamicSubsystemCollection(DObject& InHost, DClass* InScope)
		: FSubsystemCollection(InHost, InScope) { require(InScope && InScope->IsChildOf(DDynamicSubsystem::StaticClass())); }
	FDynamicSubsystemCollection::~FDynamicSubsystemCollection() = default;
	auto FDynamicSubsystemCollection::RemoveAndDeinitializeSubsystems(std::span<DClass* const> Types) -> void
	{
		require(!GIsGameThreadIdInitialized || IsInGameThread());
		requiref(State == ESubsystemState::Ready && !bPopulating && !bClosed, "Subsystem removal requires an idle, ready collection.");
		FGarbageCollectionDeferralScope Deferral;
		bPopulating = true;
		struct FReset { bool& Flag; ~FReset() { Flag = false; } } Reset{bPopulating};
		auto Removing = [&](const FEntry& Entry) { return std::ranges::find(Types, Entry.Descriptor.Type) != Types.end(); };
		for (auto& Entry : Entries) if (Removing(Entry)) InvalidateEntryReferences(Entry);
		for (size_t Index = Entries.size(); Index-- > 0;)
			if (Removing(Entries[Index])) RemoveAndDeinitializeSubsystem(Index);
	}
}
