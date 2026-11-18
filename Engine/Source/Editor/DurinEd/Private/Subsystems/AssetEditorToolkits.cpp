#include "Subsystems/AssetEditorSubsystem.h"
#include "Subsystems/AssetEditorOpenSupport.h"
#include "Toolkits/AssetEditorToolkit.h"
#include "Toolkits/AssetEditor.h"
#include "Toolkits/AssetEditorIdentity.h"
#include "Toolkits/ToolkitHost.h"
#include "Toolkits/ToolkitManager.h"
#include "Documents/EditorDocumentManager.h"
#include "AssetDefinition/AssetDefinition.h"
#include "AssetDefinition/AssetDefinitionRegistry.h"
#include "DObject/Class.h"
#include "DObject/ObjectLifecycle.h"
#include "Misc/Time.h"

namespace Durin
{
	auto DAssetEditorSubsystem::OwnsToolkit(const FAssetEditorToolkit& Toolkit) const -> bool
	{
		if (!CanCall()) return false;
		return Toolkit.GetAssetEditorSubsystem() == this && FToolkitManager::Get().Contains(Toolkit);
	}
	auto DAssetEditorSubsystem::ActivateAssetToolkit(FAssetEditorToolkit& Toolkit) -> bool
	{
		if (!OwnsToolkit(Toolkit) || Toolkit.IsClosed()) return false;
		if (const auto Previous = GetActiveAssetToolkit(); Previous && Previous.get() != &Toolkit && !Previous->RequestDeactivate()) return false;
		if (!OwnsToolkit(Toolkit) || Toolkit.IsClosed()) return false;
		if (ActiveToolkit.lock().get() != &Toolkit) Toolkit.LastActivationTime = FTime::Seconds();
		ActiveToolkit = Toolkit.shared_from_this();
		return true;
	}
	auto DAssetEditorSubsystem::InitializeDirectToolkit(std::shared_ptr<FAssetEditorToolkit> Toolkit,
		std::shared_ptr<FEditorDocumentManager> Documents, const FAssetOpenArgs& Args,
		std::span<DObject* const> Assets) -> bool
	{
		if (!CanCall() || Args.AssetEditors != this || !Toolkit || Toolkit->GetEditorInstance() || Toolkit->IsClosed() || !Documents || Assets.empty()) return false;
		// Direct admission remains restricted to inspection.
		// Immutable inspectors may use Edit; editable features must be fixed to View.
		if (Args.OpenMethod == EAssetOpenMethod::Edit && Toolkit->CanEditAssets()) return false;
		std::vector<FStrongObjectPtr> Roots;
		for (auto* Asset : Assets)
		{
			if (!IsValid(Asset) || Asset->IsTemplateObject()) return false;
			Roots.emplace_back(Asset);
		}
		const auto Support = GetAssetSelectionOpenSupport(Assets, Args.OpenMethod, GetDefaultAssetOpenSupport({Args.OpenMethod}));
		const auto Mode = Support.ResolveToolkitMode(Args.ToolkitMode,
			Args.Host ? std::optional(Args.Host->GetToolkitMode()) : std::nullopt);
		if (!Mode || (Args.Host && !Args.Host->IsAvailable())) return false;
		auto Effective = Args; Effective.ToolkitMode = *Mode;
		for (const auto& Existing : FToolkitManager::Get().GetToolkits(this))
			if (MatchesAssetEditorIdentity(*Existing, Effective, nullptr, &Toolkit->GetToolkitId())
				&& HaveSameEditedAssets(Existing->GetEditingObjects(), Assets)) return false;
		bool Initialized = false;
		DispatchEditorCallback([&] {
			if (const auto Previous = GetActiveAssetToolkit(); Previous && !Previous->RequestDeactivate()) return;
			if (!CanCall()) return;
			if (!Toolkit->InitAssetEditor({.OpenArgs = Effective, .Assets = {Assets.begin(), Assets.end()}}, Documents, DispatchingDefinition) || !CanCall())
			{ Toolkit->NotifyClosed(); return; }
			if (!FToolkitManager::Get().RegisterNewToolkit(Toolkit))
			{ Toolkit->NotifyClosed(); return; }
			SynchronizeToolkitAssets(*Toolkit);
			if (!OwnsToolkit(*Toolkit) || Toolkit->IsClosed()) return;
			Toolkit->FocusWindow();
			Initialized = OwnsToolkit(*Toolkit) && !Toolkit->IsClosed();
		});
		return Initialized;
	}
	auto DAssetEditorSubsystem::SynchronizeToolkitAssets(FAssetEditorToolkit& Toolkit, std::vector<FAssetEditorEvent>* Events) -> void
	{
		if (!OwnsToolkit(Toolkit) || Toolkit.IsClosed()) return;
		auto* Instance = Toolkit.GetEditorInstance();
		if (!Instance) return;
		const auto Objects = Toolkit.GetEditingObjects();
		const auto Found = OpenedEditors.find(Instance);
		const auto Previous = Found == OpenedEditors.end() ? std::vector<FObjectKey>{} : Found->second;
		for (const auto& Key : Previous)
			if (std::ranges::none_of(Objects, [&](auto* Object) { return FObjectKey(Object) == Key; }))
				RemoveAssetAssociation(Key, Instance, Events);
		for (auto* Object : Objects)
		{
			if (!OwnsToolkit(Toolkit) || Toolkit.IsClosed()) break;
			AddAssetAssociation(Object, Instance, Events);
		}
	}
	auto DAssetEditorSubsystem::FinishToolkitClose(FAssetEditorToolkit& Toolkit,
		DAssetEditor* Editor, IAssetEditorInstance* Instance) -> void
	{
		if (const auto Request = FindPendingClose(Toolkit)) std::erase(PendingCloses, Request);
		const FAssetEditorEvent Event{.Type = EAssetEditorEvent::Closed, .Editor = FObjectKey(Editor),
			.Method = Toolkit.GetOpenMethod(), .InstanceId = std::string(Toolkit.GetInstanceId())};
		NotifyEditorClosed(Instance);
		if (ActiveToolkit.lock().get() == &Toolkit) ActiveToolkit.reset();
		if (Editor) Editor->OnToolkitClosed();
		PublishEvent(Event);
	}
}
