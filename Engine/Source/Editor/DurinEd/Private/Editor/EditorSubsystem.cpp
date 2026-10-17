#include "Editor/EditorSubsystem.h"
#include "Editor/EditorEngine.h"
#include "DObject/Class.h"
#include "DObject/ObjectLifecycle.h"
#include "Modules/ModuleManager.h"
#include "Modules/ModularFeature.h"

namespace Durin
{
	namespace
	{
		auto Collections() -> std::vector<FEditorSubsystemCollection*>&
		{ static auto* Value = new std::vector<FEditorSubsystemCollection*>(); return *Value; }
	}

	DEditorSubsystem::DEditorSubsystem(const FObjectInitializer& Initializer) : Super(Initializer) {}
	auto DEditorSubsystem::BeginDestroy() -> void
	{
		if (!HasCompletedDeinitialization())
			if (auto* Host = GetEditor()) Host->PrepareForShutdown();
		Super::BeginDestroy();
	}
	auto DEditorSubsystem::IsReadyForFinishDestroy() -> bool
	{
		if (HasCompletedDeinitialization()) return true;
		auto* Host = GetEditor();
		return !Host || Host->IsReadyForFinishDestroy();
	}
	auto DEditorSubsystem::GetEditor() const -> DEditorEngine* { return Cast<DEditorEngine>(GetOuter()); }
	FEditorSubsystemCollection::FEditorSubsystemCollection(DEditorEngine& Host) : FSubsystemCollection(Host, DEditorSubsystem::StaticClass()) { Collections().push_back(this); }
	auto FEditorSubsystemCollection::Initialize() -> FSubsystemResult
	{
		std::vector<FSubsystemDescriptor> Selected;
		for (const auto& Entry : FSubsystemRegistration::Snapshot(DEditorSubsystem::StaticClass()))
		{
			if (!Entry.Descriptor.Provider.IsNone() && !FModuleManager::Get().IsModuleLoaded(Entry.Descriptor.Provider)) continue;
			Selected.push_back(Entry.Descriptor);
		}
		return FSubsystemCollection::Initialize(std::move(Selected));
	}
	FEditorSubsystemCollection::~FEditorSubsystemCollection() { std::erase(Collections(), this); }
	auto FEditorSubsystemCollection::Synchronize(bool bForce) -> FSubsystemResult
	{
		const auto Revision = FSubsystemRegistration::GetRevision();
		if (!bForce && LastRevision == Revision) return LastResult;
		std::vector<FSubsystemDescriptor> Selected;
		bool PendingStartup = false;
		FSubsystemResult ProviderError;
		for (const auto& Entry : FSubsystemRegistration::Snapshot(DEditorSubsystem::StaticClass()))
		{
			if (!Entry.Descriptor.Provider.IsNone())
			{
				const auto Module = FModuleManager::Get().FindModule(Entry.Descriptor.Provider);
				if (!Module || Module->State.load() != EModuleState::Active)
				{
					const bool Loading = Module && Module->State.load() == EModuleState::Loading;
					PendingStartup |= Loading;
					if (!Loading) ProviderError = {ESubsystemError::ProviderUnavailable, Entry.Descriptor.Provider.ToString()};
					continue;
				}
			}
			Selected.push_back(Entry.Descriptor);
		}
		LastRevision = PendingStartup ? UINT64_MAX : Revision;
		LastResult = Reconcile(std::move(Selected));
		if (LastResult && !ProviderError) LastResult = std::move(ProviderError);
		return LastResult;
	}
	auto FEditorSubsystemCollection::CanSynchronizeAll() -> bool
	{
		if (IsGarbageCollecting() || FModularFeatureRegistry::Get().IsDispatching()) return false;
		for (auto* Collection : Collections())
			if (Collection->IsDispatching() || Collection->GetState() == ESubsystemState::Initializing
				|| Collection->GetState() == ESubsystemState::ShuttingDown) return false;
		for (auto* Collection : Collections())
			if (Collection->GetState() == ESubsystemState::Ready && !static_cast<DEditorEngine&>(Collection->Host).CanRefreshEditorSubsystems()) return false;
		return true;
	}
	auto FEditorSubsystemCollection::SynchronizeAll() -> void
	{
		requiref(CanSynchronizeAll(), "Editor providers must unregister at an idle host safe point.");
		// No host can be physically destroyed by another host's cleanup callbacks.
		FGarbageCollectionDeferralScope Deferral;
		const auto Hosts = Collections();
		for (auto* Collection : Hosts)
			if (Collection->GetState() == ESubsystemState::Ready)
				(void)static_cast<DEditorEngine&>(Collection->Host).RefreshEditorSubsystems(true);
	}

}
