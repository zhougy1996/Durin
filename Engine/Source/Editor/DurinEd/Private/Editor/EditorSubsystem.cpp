#include "Editor/EditorSubsystem.h"
#include "Editor/EditorEngine.h"
#include "DObject/Class.h"
#include "DObject/ObjectLifecycle.h"
#include "Modules/ModuleManager.h"
#include "Modules/ModularFeature.h"

namespace Durin
{
	DEditorSubsystem::DEditorSubsystem(const FObjectInitializer& Initializer) : Super(Initializer) {}
	auto DEditorSubsystem::GetEditor() const -> DEditorEngine* { return Cast<DEditorEngine>(GetOuter()); }
	FEditorSubsystemCollection::FEditorSubsystemCollection(DEditorEngine& Host) : FDynamicSubsystemCollection(Host, DEditorSubsystem::StaticClass()) {}
	auto FEditorSubsystemCollection::Initialize() -> FSubsystemResult
	{
		auto Selected = FSubsystemRegistration::Snapshot(DEditorSubsystem::StaticClass());
		std::erase_if(Selected, [](const auto& Entry) {
			return !Entry.Provider.IsNone() && !FModuleManager::Get().IsModuleLoaded(Entry.Provider);
		});
		return FSubsystemCollection::Initialize(std::move(Selected));
	}
	FEditorSubsystemCollection::~FEditorSubsystemCollection() = default;
	auto FEditorSubsystemCollection::Synchronize() -> FSubsystemResult
	{
		std::vector<FSubsystemDescriptor> Selected;
		for (const auto& Entry : FSubsystemRegistration::Snapshot(DEditorSubsystem::StaticClass()))
		{
			if (!Entry.Provider.IsNone())
			{
				const auto Module = FModuleManager::Get().FindModule(Entry.Provider);
				if (!Module || Module->State.load() != EModuleState::Active)
				{
					if (!Module || Module->State.load() != EModuleState::Loading)
						return {ESubsystemError::ProviderUnavailable, Entry.Provider.ToString()};
					continue;
				}
			}
			Selected.push_back(Entry);
		}
		return Reconcile(std::move(Selected));
	}
	auto FEditorSubsystemCollection::CanSynchronizeActiveEditor(FName RemovingProvider) -> bool
	{
		if (IsGarbageCollecting() || FModularFeatureRegistry::Get().IsDispatching()) return false;
		if (!GEditor) return true;
		const auto& Collection = GEditor->EditorSubsystems;
		if (Collection.IsPopulating() || Collection.GetState() == ESubsystemState::Initializing
			|| Collection.GetState() == ESubsystemState::ShuttingDown) return false;
		if (Collection.GetState() == ESubsystemState::Ready && !GEditor->CanSynchronizeEditorSubsystems()) return false;
		if (Collection.GetState() == ESubsystemState::Ready && !RemovingProvider.IsNone()
			&& !Collection.CanRemoveProvider(RemovingProvider)) return false;
		return true;
	}
	auto FEditorSubsystemCollection::SynchronizeActiveEditor() -> void
	{
		requiref(CanSynchronizeActiveEditor(), "Editor providers must unregister at an idle host safe point.");
		FGarbageCollectionDeferralScope Deferral;
		if (GEditor && GEditor->GetEditorSubsystemState() == ESubsystemState::Ready)
		{
			const auto Result = GEditor->SynchronizeEditorSubsystems();
			requiref(static_cast<bool>(Result), "Editor provider removal failed validation: {}", Result.Message);
		}
	}

}
