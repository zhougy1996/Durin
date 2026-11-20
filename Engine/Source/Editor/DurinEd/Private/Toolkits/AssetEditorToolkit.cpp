#include "Toolkits/AssetEditorToolkit.h"
#include "Threading/RunnableThread.h"
#include "CoreGlobals.h"

#include "Toolkits/AssetEditor.h"
#include "DObject/ObjectGraphReplacement.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Toolkits/ToolkitHost.h"
#include "Toolkits/ToolkitManager.h"
#include "DObject/ObjectLifecycle.h"
#include "Editor/EditorEngine.h"
#include "Misc/Time.h"
#include "Transactions/Transactor.h"

namespace Durin
{
	FAssetEditorToolkit::FAssetEditorToolkit() = default;
	FAssetEditorToolkit::~FAssetEditorToolkit() = default;

	auto FAssetEditorToolkit::GetEditorInstance() const -> IAssetEditorInstance*
	{ if (bClosed) return nullptr; if (auto* Editor = OwningAssetEditor.Get()) return Editor; return bInitialized && OwningAssetEditor.GetKey().IsNull() ? const_cast<FAssetEditorToolkit*>(this) : nullptr; }
	auto FAssetEditorToolkit::GetAssetEditorSubsystem() const -> DAssetEditorSubsystem* { return AssetEditors.Get(); }
	auto FAssetEditorToolkit::GetToolkitHost() const -> std::shared_ptr<IToolkitHost> { return Host.lock(); }
	auto FAssetEditorToolkit::GetToolkitMode() const -> EToolkitMode
	{ return ToolkitMode; }
	auto FAssetEditorToolkit::GetInstanceKey() const -> std::string_view { return InstanceKey; }
	auto FAssetEditorToolkit::GetWindowId() const -> std::string_view { return WindowId; }
	auto FAssetEditorToolkit::GetEditorName() const -> FName { return FName(GetToolkitId().GetValue()); }
	auto FAssetEditorToolkit::GetEditingObjects() const -> std::vector<DObject*>
	{
		if (bClosed) return {};
		if (auto Owned = GetOwnedEditingObjects()) return std::move(*Owned);
		std::vector<DObject*> Result;
		for (const auto& Asset : EditingAssets)
			if (auto* Object = Asset.Get(); IsValid(Object)) Result.push_back(Object);
		return Result;
	}
	auto FAssetEditorToolkit::SupportsRestore() const -> bool
	{ if (bClosed) return false; if (auto* Editor = OwningAssetEditor.Get()) return Editor->SupportsRestore(); return bInitialized && OwningAssetEditor.GetKey().IsNull(); }
	auto FAssetEditorToolkit::IsPrimaryEditor() const -> bool { return bPrimary; }
	auto FAssetEditorToolkit::GetOpenMethod() const -> EAssetOpenMethod { return OpenMethod; }
	auto FAssetEditorToolkit::GetLastActivationTime() -> double { return LastActivationTime; }
	auto FAssetEditorToolkit::NotifyClosed() -> void
	{
		if (bClosed) return;
		bClosed = true;
		for (const auto& Asset : EditingAssets)
			if (auto* Object = Asset.Get(); IsValid(Object)) OnCloseAsset(*Object);
		OnClose();
		EditingAssets.clear();
		ActiveAsset = {};
		const auto ClosingHost = Host.lock();
		Host.reset();
		HostIdentity = nullptr;
		if (ClosingHost) ClosingHost->DetachToolkit(*this);
	}
	auto FAssetEditorToolkit::NotifyEditingObjectsChanged() -> void
	{
		if (auto* Editors = GetAssetEditorSubsystem()) Editors->SynchronizeToolkitAssets(*this);
	}

	auto FAssetEditorToolkit::FocusWindow(DObject* Object) -> void
	{
		auto* AssetEditorSubsystem = GetAssetEditorSubsystem();
		if (!AssetEditorSubsystem || !IsEditorOpen()) return;
		const auto Lease = shared_from_this();
		const FStrongObjectPtr KeepAlive(OwningAssetEditor.Get());
		AssetEditorSubsystem->DispatchEditorCallback([&] {
			if (Object && !HasEditingObject(*Object)) return;
			if (!AssetEditorSubsystem->ActivateAssetToolkit(*this)) return;
			auto* Focused = Object ? Object : GetActiveAsset();
			if (Focused && !ActivateAsset(*Focused)) return;
			FocusEditorView();
			if (Focused) Tabs.RequestFocus(*Focused);
			LastActivationTime = FTime::Seconds();
			if (const auto CurrentHost = Host.lock()) CurrentHost->FocusToolkit(*this);
			FAssetOpenTarget Target;
			if (Focused && IsValid(Focused))
			{
				FSoftObjectPath::TryCreate(Focused->GetObjectPath(), Target.AssetPath);
				Target.AssetClassName = Focused->GetClass()->GetQualifiedName().ToString();
			}
			AssetEditorSubsystem->PublishEvent({.Type = EAssetEditorEvent::Focused, .Target = std::move(Target),
				.Editor = FObjectKey(OwningAssetEditor.Get()), .Method = OpenMethod, .InstanceId = std::string(GetInstanceId())});
		});
	}
	auto FAssetEditorToolkit::CloseWindow(EAssetEditorCloseReason Reason) -> bool
	{
		return CloseWindowInternal(Reason, true);
	}

	auto FAssetEditorToolkit::CloseWindowInternal(EAssetEditorCloseReason Reason, bool bPublishRequest) -> bool
	{
		if (GIsGameThreadIdInitialized) CheckGameThread();
		if (bClosed) return true;
		auto* AssetEditorSubsystem = GetAssetEditorSubsystem();
		struct FCloseScope
		{
			std::optional<EAssetEditorCloseReason>& Active;
			FCloseScope(std::optional<EAssetEditorCloseReason>& InActive, EAssetEditorCloseReason Reason) : Active(InActive) { Active = Reason; }
			~FCloseScope() { Active.reset(); }
		};
		if (Reason == EAssetEditorCloseReason::EditorExit)
		{
			if (!AssetEditorSubsystem || !AssetEditorSubsystem->bClosingEditors || ActiveCloseReason) return false;
			const auto Lease = shared_from_this();
			const FCloseScope CloseScope(ActiveCloseReason, Reason);
			const FScopedEditorTransactionContext Context(AssetEditorSubsystem->GetEditor());
			if (!OnRequestClose(Reason)) return false;
			FToolkitManager::Get().CloseToolkit(*this);
			return bClosed;
		}
		if (!AssetEditorSubsystem || !IsEditorOpen() || ActiveCloseReason
			|| AssetEditorSubsystem->FindPendingClose(*this)) return false;
		const auto Lease = shared_from_this();
		const FStrongObjectPtr KeepAlive(OwningAssetEditor.Get());
		bool Closed = false;
		AssetEditorSubsystem->DispatchEditorCallback([&] {
			const FCloseScope CloseScope(ActiveCloseReason, Reason);
			if (bPublishRequest)
				AssetEditorSubsystem->PublishEvent({.Type = EAssetEditorEvent::CloseRequested,
					.Editor = FObjectKey(OwningAssetEditor.Get()), .Method = OpenMethod, .InstanceId = WindowId});
			if (bClosed) { Closed = true; return; }
			if (!OnRequestClose(Reason)) return;
			if (!bClosed) FToolkitManager::Get().CloseToolkit(*this);
			Closed = bClosed;
		});
		return Closed;
	}

	auto FAssetEditorToolkit::OnRequestClose(EAssetEditorCloseReason Reason) -> bool
	{
		if (Reason == EAssetEditorCloseReason::EditorExit) return PrepareForExit();
		const auto Assets = GetEditingObjects();
		for (auto* Asset : Assets)
			if (HasEditingObject(*Asset) && TryCloseAsset(*Asset) != EAssetEditorCloseResult::Closed) return false;
		return true;
	}

	auto FAssetEditorToolkit::PrepareForExit() -> bool
	{
		if (IsClosed()) return true;
		const auto* Editors = GetAssetEditorSubsystem();
		return (!Editors || !Editors->FindPendingClose(*this)) && RequestDeactivate();
	}

	auto FAssetEditorToolkit::SaveForExit() -> bool
	{
		if (!PrepareForExit()) return false;
		for (auto* Asset : GetEditingObjects())
			if (HasEditingObject(*Asset) && IsAssetDirty(*Asset) && !SaveAsset(*Asset)) return false;
		return PrepareForExit() && GetUnsavedChanges().empty();
	}

	auto FAssetEditorToolkit::GetUnsavedChanges() const -> std::vector<FAssetEditorUnsavedChange>
	{
		std::vector<FAssetEditorUnsavedChange> Changes;
		for (auto* Asset : GetEditingObjects())
			if (IsAssetDirty(*Asset)) Changes.push_back({Asset->GetName(), Asset->GetObjectPath()});
		return Changes;
	}

	auto FAssetEditorToolkit::GetEditorDisplayName() const -> std::string
	{
		const auto* Asset = GetActiveAsset();
		return Asset ? Asset->GetName() : GetEditorName().ToString();
	}

	auto FAssetEditorToolkit::GetActiveAsset() const -> DObject*
	{
		const auto Assets = GetEditingObjects();
		if (Assets.empty()) return nullptr;
		auto* Active = ActiveAsset.ResolveObjectPtr();
		return std::ranges::contains(Assets, Active) ? Active : Assets.front();
	}

	auto FAssetEditorToolkit::HasEditingObject(const DObject& Object) const -> bool
	{
		return std::ranges::contains(GetEditingObjects(), &Object);
	}

	auto FAssetEditorToolkit::ActivateAsset(DObject& Asset) -> bool
	{
		if (!IsEditorOpen() || !HasEditingObject(Asset)) return false;
		const FObjectKey Target(&Asset);
		if (ActiveAsset != Target && !RequestDeactivate()) return false;
		if (!IsEditorOpen() || !HasEditingObject(Asset)) return false;
		ActiveAsset = Target;
		OnActivateAsset(Asset);
		return IsEditorOpen();
	}

	auto FAssetEditorToolkit::IsAssetDirty(const DObject& Asset) const -> bool
	{
		return IsEditorOpen() && !IsReadOnly() && HasEditingObject(Asset) && OnIsAssetDirty(Asset);
	}

	auto FAssetEditorToolkit::SaveAsset(DObject& Asset) -> bool
	{
		return ExecuteEditorCommand([&] { return HasEditingObject(Asset) && OnSaveAsset(Asset); });
	}

	auto FAssetEditorToolkit::DiscardAsset(DObject& Asset) -> bool
	{
		return ExecuteEditorCommand([&] { return HasEditingObject(Asset) && OnDiscardAsset(Asset); });
	}

	auto FAssetEditorToolkit::OnSaveActiveDocument() -> bool
	{
		auto* Asset = GetActiveAsset();
		return Asset && OnSaveAsset(*Asset);
	}

	auto FAssetEditorToolkit::RequestCloseAsset(DObject& Asset) -> EAssetEditorCloseResult
	{
		auto* Editors = GetAssetEditorSubsystem();
		if (!Editors || !IsEditorOpen() || !HasEditingObject(Asset)) return EAssetEditorCloseResult::Rejected;
		if (const auto Pending = Editors->FindPendingClose(*this); Pending && Pending->bResolving)
			return EAssetEditorCloseResult::Rejected;
		const auto Lease = shared_from_this();
		EAssetEditorCloseResult Result = EAssetEditorCloseResult::Rejected;
		Editors->DispatchEditorCallback([&] {
			Result = TryCloseAsset(Asset);
			if (Result == EAssetEditorCloseResult::Closed && GetEditingObjects().empty() && !IsClosed())
				CloseWindow(EAssetEditorCloseReason::AssetEditorHostClosed);
		});
		return Result;
	}

	auto FAssetEditorToolkit::OnRequestCloseAsset(DObject& Asset) -> EAssetEditorCloseResult
	{
		if (!RequestDeactivate()) return EAssetEditorCloseResult::Rejected;
		return IsAssetDirty(Asset) ? EAssetEditorCloseResult::PendingConfirmation : EAssetEditorCloseResult::Closed;
	}

	auto FAssetEditorToolkit::TryCloseAsset(DObject& Asset) -> EAssetEditorCloseResult
	{
		const FObjectKey Key(&Asset);
		const auto* Editors = GetAssetEditorSubsystem();
		if (const auto Pending = Editors ? Editors->FindPendingClose(*this) : nullptr; Pending && !Pending->bResolving)
			return !Pending->bResume && Pending->Asset == Key
				? EAssetEditorCloseResult::PendingConfirmation : EAssetEditorCloseResult::Rejected;
		const auto Admission = OnRequestCloseAsset(Asset);
		if (Admission == EAssetEditorCloseResult::Rejected) return Admission;
		if (IsClosed()) return EAssetEditorCloseResult::Closed;
		if (!HasEditingObject(Asset)) return EAssetEditorCloseResult::Rejected;
		if (Admission == EAssetEditorCloseResult::PendingConfirmation)
		{
			auto* Subsystem = GetAssetEditorSubsystem();
			Subsystem->QueueClose(*this, Key, Asset.GetName());
			return EAssetEditorCloseResult::PendingConfirmation;
		}
		OnCloseAsset(Asset);
		std::erase_if(EditingAssets, [&](const auto& Owner) { return Owner.GetKey() == Key; });
		if (ActiveAsset == Key)
		{
			ActiveAsset = {};
			if (auto* Next = GetActiveAsset(); Next && IsEditorOpen()) ActivateAsset(*Next);
		}
		NotifyEditingObjectsChanged();
		return EAssetEditorCloseResult::Closed;
	}

	auto FAssetEditorToolkit::ResolveAssetClose(FObjectKey& Key, EAssetEditorCloseResponse Response) -> EAssetEditorCloseResult
	{
		auto* Existing = Key.ResolveObjectPtr();
		if (!Existing || !HasEditingObject(*Existing)) return EAssetEditorCloseResult::Rejected;
		const std::string Resource = Existing->GetObjectPath();
		const bool bResolved = Response == EAssetEditorCloseResponse::Save
			? SaveAsset(*Existing) : DiscardAsset(*Existing);
		if (IsClosed()) return EAssetEditorCloseResult::Closed;
		if (!IsEditorOpen()) return EAssetEditorCloseResult::Rejected;
		if (!bResolved) return EAssetEditorCloseResult::PendingConfirmation;
		Existing = Key.ResolveObjectPtr();
		if (Response == EAssetEditorCloseResponse::Discard)
			for (auto* Asset : GetEditingObjects())
				if (Asset->GetObjectPath() == Resource) { Existing = Asset; break; }
		if (!IsValid(Existing) || !HasEditingObject(*Existing)) return EAssetEditorCloseResult::Rejected;
		Key = FObjectKey(Existing);
		const auto Result = TryCloseAsset(*Existing);
		return Result == EAssetEditorCloseResult::Rejected ? EAssetEditorCloseResult::PendingConfirmation : Result;
	}

	auto FAssetEditorToolkit::DrawAssetTabs(std::string_view RootKey,
		const std::function<void(DObject&)>& DrawAsset,
		const std::function<void(DObject&, bool)>& UpdateVisibility) -> bool
	{
		return Tabs.DrawAssets(*this, GetPresentationRootKey(RootKey), DrawAsset, UpdateVisibility);
	}

	auto FAssetEditorToolkit::InitAssetEditor(const FAssetEditorInitArgs& Args,
		FStrongObjectPtr Provider, DAssetEditor* Editor) -> bool
	{
		if (bInitialized || bClosed || !Args.OpenArgs.AssetEditors) return false;
		checkf(Args.OpenArgs.OpenLocation == EAssetEditorOpenLocation::MainWindow
			|| Args.OpenArgs.OpenLocation == EAssetEditorOpenLocation::NewWindow, "Unknown asset editor opening location");
		bInitialized = true;
		OwningAssetEditor = Editor;
		AssetEditors = Args.OpenArgs.AssetEditors;
		const auto InitialHost = AssetEditors.Get()->CreateToolkitHost(Args.OpenArgs);
		Host = InitialHost;
		HostIdentity = InitialHost.get();
		ToolkitMode = Args.OpenArgs.ToolkitMode;
		OpenLocation = Args.OpenArgs.OpenLocation;
		InstanceKey = Args.OpenArgs.GetNormalizedRole();
		bPrimary = Args.OpenArgs.bPrimary;
		static uint64 NextId = 1;
		require(NextId != std::numeric_limits<uint64>::max());
		WindowId = "Toolkit." + std::to_string(NextId++);
		OpenMethod = Args.OpenArgs.OpenMethod;
		DefinitionProvider = std::move(Provider);
		if (!InitialHost || !InitialHost->IsAvailable() || InitialHost->GetToolkitMode() != Args.OpenArgs.ToolkitMode) return false;
		PostInitAssetEditor();
		const bool bOwnsSessionSources = GetOwnedEditingObjects().has_value();
		std::unordered_set<DObject*> Seen;
		for (auto* Asset : Args.Assets)
		{
			if (!Seen.insert(Asset).second) continue;
			if (!IsValid(Asset) || !OnOpenAsset(*Asset) || IsClosed()) return false;
			if (!bOwnsSessionSources) EditingAssets.emplace_back(Asset);
		}
		if (auto* First = GetActiveAsset())
		{
			ActiveAsset = FObjectKey(First);
			OnActivateAsset(*First);
		}
		return InitialHost->AttachToolkit(shared_from_this());
	}

	auto FAssetEditorToolkit::DrawToolkit(bool bActive) -> bool
	{
		if (GIsGameThreadIdInitialized) CheckGameThread();
		if (!IsEditorOpen()) return false;
		const auto Lease = weak_from_this().lock();
		if (!Lease || !GetAssetEditorSubsystem()) return false;
		const auto KeepAlive = OwningAssetEditor;
		bool Activated = false;
		GetAssetEditorSubsystem()->DispatchEditorCallback([&] { if (IsEditorOpen()) Activated = OnDrawToolkit(bActive); });
		return Activated && IsEditorOpen();
	}
	auto FAssetEditorToolkit::ExecuteEditorCommand(const std::function<bool()>& Command) -> bool
	{
		if (GIsGameThreadIdInitialized) CheckGameThread();
		if (!IsEditorOpen() || IsReadOnly()) return false;
		const auto Lease = weak_from_this().lock();
		if (!Lease || !GetAssetEditorSubsystem()) return false;
		const auto KeepAlive = OwningAssetEditor;
		bool Result = false;
		if (auto* AssetEditorSubsystem = GetAssetEditorSubsystem())
			AssetEditorSubsystem->DispatchEditorCallback([&] { if (IsEditorOpen()) Result = Command(); });
		return Result;
	}
	auto FAssetEditorToolkit::IsEditorOpen() const -> bool
	{
		const auto* AssetEditorSubsystem = AssetEditors.Get();
		return bInitialized && !bClosed && AssetEditorSubsystem && AssetEditorSubsystem->OwnsToolkit(*this);
	}

	auto FAssetEditorToolkit::GetLayoutProfileKey() const -> std::string
	{ return std::string(GetLayoutName()) + ".Layout.v" + std::to_string(GetLayoutVersion()) + (OpenMethod == EAssetOpenMethod::View ? ".View" : ""); }
	auto FAssetEditorToolkit::GetPresentationRootKey(std::string_view EditorRootKey) const -> std::string
	{
		return std::string(EditorRootKey) + (GetWindowId().empty() ? std::string{} : ".Window." + std::string(GetWindowId()));
	}
}
