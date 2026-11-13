#include "Subsystems/AssetEditorSubsystem.h"
#include "Subsystems/AssetEditorOpenSupport.h"
#include "Toolkits/AssetEditorToolkit.h"
#include "Toolkits/AssetEditor.h"
#include "Toolkits/AssetEditorIdentity.h"
#include "Toolkits/ToolkitHost.h"
#include "Documents/EditorDocumentManager.h"
#include "AssetDefinition/AssetDefinition.h"
#include "AssetDefinition/AssetDefinitionRegistry.h"
#include "DObject/Class.h"
#include "DObject/ObjectLifecycle.h"
#include "Misc/Time.h"

namespace Durin
{
	auto DAssetEditorSubsystem::GetAssetToolkits() const -> std::vector<std::shared_ptr<FAssetEditorToolkit>>
	{
		auto Result = DirectToolkits;
		for (const auto& [Key, Session] : Sessions) if (auto Toolkit = Session->GetToolkit()) Result.push_back(std::move(Toolkit));
		std::ranges::sort(Result, [](const auto& A, const auto& B) {
			if (A->GetLastActivationTime() != B->GetLastActivationTime()) return A->GetLastActivationTime() < B->GetLastActivationTime();
			return A->GetWindowId() < B->GetWindowId();
		});
		return Result;
	}
	auto DAssetEditorSubsystem::OwnsToolkit(const FAssetEditorToolkit& Toolkit) const -> bool
	{
		if (!CanCall()) return false;
		if (const auto* Session = Toolkit.GetOwningAssetEditor()) return OwnsAssetEditor(*Session) && Session->GetToolkit().get() == &Toolkit;
		return std::ranges::any_of(DirectToolkits, [&](const auto& Entry) { return Entry.get() == &Toolkit; });
	}
	auto DAssetEditorSubsystem::ActivateAssetToolkit(FAssetEditorToolkit& Toolkit) -> bool
	{
		if (!OwnsToolkit(Toolkit) || Toolkit.IsRetired()) return false;
		if (const auto Previous = GetActiveAssetToolkit(); Previous && Previous.get() != &Toolkit && !Previous->RequestDeactivate()) return false;
		if (!OwnsToolkit(Toolkit) || Toolkit.IsRetired()) return false;
		if (ActiveToolkit.lock().get() != &Toolkit) Toolkit.LastActivationTime = FTime::Seconds();
		ActiveToolkit = Toolkit.shared_from_this();
		return true;
	}
	auto DAssetEditorSubsystem::InitializeDirectToolkit(std::shared_ptr<FAssetEditorToolkit> Toolkit,
		std::shared_ptr<FEditorDocumentManager> Documents, const FAssetOpenArgs& Args,
		std::span<DObject* const> Assets) -> bool
	{
		if (!CanCall() || Args.AssetEditors != this || !Toolkit || Toolkit->GetEditorInstance() || Toolkit->IsRetired() || !Documents || Assets.empty()) return false;
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
		for (const auto& Existing : GetAssetToolkits())
			if (Existing->GetToolkitId() == Toolkit->GetToolkitId() && Existing->GetOpenMethod() == Args.OpenMethod
				&& Existing->GetToolkitMode() == Effective.ToolkitMode && Existing->GetInstanceKey() == Args.GetNormalizedRole()
				&& HaveSameEditedAssets(Existing->GetObjectsToEdit(), Assets)) return false;
		bool Initialized = false;
		DispatchEditorCallback([&] {
			if (const auto Previous = GetActiveAssetToolkit(); Previous && !Previous->RequestDeactivate()) return;
			if (!CanCall()) return;
			if (!Toolkit->InitAssetEditor({.OpenArgs = Effective, .Assets = {Assets.begin(), Assets.end()}}, Documents, DispatchingDefinition) || !CanCall())
			{ Toolkit->Retire(); return; }
			DirectToolkits.push_back(Toolkit);
			SynchronizeToolkitAssets(*Toolkit);
			if (!OwnsToolkit(*Toolkit) || Toolkit->IsRetired()) return;
			Toolkit->FocusWindow();
			Initialized = OwnsToolkit(*Toolkit) && !Toolkit->IsRetired();
		});
		return Initialized;
	}
	auto DAssetEditorSubsystem::SynchronizeToolkitAssets(FAssetEditorToolkit& Toolkit) -> void
	{
		if (!OwnsToolkit(Toolkit) || Toolkit.IsRetired()) return;
		auto* Instance = Toolkit.GetEditorInstance();
		if (!Instance) return;
		const auto Objects = Toolkit.GetObjectsToEdit();
		const auto Found = OpenedEditors.find(Instance);
		const auto Previous = Found == OpenedEditors.end() ? std::vector<FObjectKey>{} : Found->second;
		for (const auto& Key : Previous)
			if (std::ranges::none_of(Objects, [&](auto* Object) { return FObjectKey(Object) == Key; }))
				if (auto* Object = Key.ResolveObjectPtr()) NotifyAssetClosed(Object, Instance);
		for (auto* Object : Objects)
		{
			if (!OwnsToolkit(Toolkit) || Toolkit.IsRetired()) break;
			NotifyAssetOpened(Object, Instance);
		}
	}
	auto DAssetEditorSubsystem::RetireAssetToolkit(FAssetEditorToolkit& Toolkit) -> void
	{
		if (Toolkit.IsRetired()) return;
		auto* Editor = Toolkit.GetOwningAssetEditor();
		if (Editor ? !Sessions.contains(FObjectKey(Editor)) : std::ranges::none_of(DirectToolkits,
			[&](const auto& Entry) { return Entry.get() == &Toolkit; })) return;
		const auto Lease = Toolkit.shared_from_this();
		const FStrongObjectPtr KeepAlive(Editor);
		const FAssetEditorEvent Event{.Type = EAssetEditorEvent::Closed, .Session = FObjectKey(Editor),
			.Method = Toolkit.GetOpenMethod(), .InstanceId = std::string(Toolkit.GetInstanceId())};
		Toolkit.Retire();
		NotifyEditorClosed(Toolkit.GetEditorInstance());
		if (ActiveToolkit.lock().get() == &Toolkit) ActiveToolkit.reset();
		if (Editor) { Sessions.erase(FObjectKey(Editor)); Editor->OnToolkitClosed(); }
		else std::erase(DirectToolkits, Lease);
		PublishEvent(Event);
	}
}
