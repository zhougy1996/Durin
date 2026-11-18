#include "Toolkits/AssetEditorToolkit.h"
#include "Threading/RunnableThread.h"
#include "CoreGlobals.h"

#include "Toolkits/AssetEditor.h"
#include "Documents/EditorDocumentManager.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Subsystems/AssetEditorOpenSupport.h"
#include "Toolkits/AssetEditorIdentity.h"
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
	auto FAssetEditorToolkit::TransferToHost(std::shared_ptr<IToolkitHost> NewHost) -> bool
	{
		if (GIsGameThreadIdInitialized) CheckGameThread();
		if (!IsEditorOpen() || !NewHost || !NewHost->IsAvailable()) return false;
		auto* AssetEditorSubsystem = GetAssetEditorSubsystem();
		const auto Lease = shared_from_this();
		const auto Editor = OwningAssetEditor;
		bool Transferred = false;
		AssetEditorSubsystem->DispatchEditorCallback([&] {
			const auto PreviousHost = GetToolkitHost();
			const auto Objects = GetEditingObjects();
			const auto Support = GetAssetSelectionOpenSupport(Objects, OpenMethod,
				Editor ? Editor->GetAssetOpenSupport({OpenMethod}) : GetDefaultAssetOpenSupport({OpenMethod}));
			if (!Support.ResolveToolkitMode(NewHost->GetToolkitMode(), NewHost->GetToolkitMode())
				|| !PrepareForExit()) return;
			if (!IsEditorOpen()) return;
			if (NewHost == PreviousHost) { Transferred = true; return; }
			const FAssetOpenArgs Identity{.OpenMethod = OpenMethod, .ToolkitMode = NewHost->GetToolkitMode(), .InstanceRole = InstanceKey};
			for (const auto& Other : FToolkitManager::Get().GetToolkits(AssetEditorSubsystem))
			{
				if (Other.get() == this || Other->IsClosed()
					|| !MatchesAssetEditorIdentity(*Other, Identity, Editor ? Editor->GetClass() : nullptr,
						Editor ? nullptr : &GetToolkitId())) continue;
				if (HaveSameEditedAssets(Other->GetEditingObjects(), GetEditingObjects())) return;
			}
			if (!NewHost->AttachToolkit(Lease)) return;
			if (!IsEditorOpen() || !NewHost->IsAvailable())
			{ NewHost->DetachToolkit(*this); return; }
			if (PreviousHost) PreviousHost->DetachToolkit(*this);
			if (!IsEditorOpen() || !NewHost->IsAvailable())
			{ NewHost->DetachToolkit(*this); return; }
			Host = NewHost;
			HostIdentity = NewHost.get();
			ToolkitMode = NewHost->GetToolkitMode();
			OnHostChanged();
			Transferred = true;
		});
		return Transferred && IsEditorOpen();
	}
	auto FAssetEditorToolkit::GetInstanceKey() const -> std::string_view { return InstanceKey; }
	auto FAssetEditorToolkit::GetWindowId() const -> std::string_view { return WindowId; }
	auto FAssetEditorToolkit::GetEditorName() const -> FName { return FName(GetToolkitId().GetValue()); }
	auto FAssetEditorToolkit::GetEditingObjects() const -> std::vector<DObject*>
	{
		if (bClosed) return {};
		if (auto Owned = GetOwnedEditingObjects()) return std::move(*Owned);
		return DocumentOwner ? DocumentOwner->GetEditedAssets() : std::vector<DObject*>{};
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
		OnClose();
		if (DocumentOwner) DocumentOwner->RetireToolkit();
		const auto ClosingHost = Host.lock();
		Host.reset();
		HostIdentity = nullptr;
		if (ClosingHost) ClosingHost->DetachToolkit(*this);
	}
	auto FAssetEditorToolkit::FocusWindow(DObject* Object) -> void
	{
		auto* AssetEditorSubsystem = GetAssetEditorSubsystem();
		if (!AssetEditorSubsystem || !IsEditorOpen()) return;
		const auto Lease = shared_from_this();
		const FStrongObjectPtr KeepAlive(OwningAssetEditor.Get());
		AssetEditorSubsystem->DispatchEditorCallback([&] {
			if (!AssetEditorSubsystem->ActivateAssetToolkit(*this)) return;
			const auto Snapshot = DocumentOwner->GetDocuments();
			for (const auto& Document : Snapshot)
				if (!Object || Document.ResourceId == Object->GetObjectPath())
				{
					if (!DocumentOwner->ActivateDocument(Document.Id) || !IsEditorOpen()) return;
					LastActivationTime = FTime::Seconds();
					if (const auto CurrentHost = Host.lock()) CurrentHost->FocusToolkit(*this);
					FAssetOpenTarget Target;
					for (auto* Asset : GetEditingObjects()) if (Asset->GetObjectPath() == Document.ResourceId)
					{ FSoftObjectPath::TryCreate(Asset->GetObjectPath(), Target.AssetPath); Target.AssetClassName = Asset->GetClass()->GetQualifiedName().ToString(); break; }
					AssetEditorSubsystem->PublishEvent({.Type = EAssetEditorEvent::Focused, .Target = std::move(Target), .Editor = FObjectKey(OwningAssetEditor.Get()), .Method = OpenMethod, .InstanceId = std::string(GetInstanceId())});
					return;
				}
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
		const auto Snapshot = DocumentOwner->GetDocuments();
		for (const auto& Document : Snapshot)
			if (DocumentOwner->RequestCloseDocument(Document.Id) != EDocumentCloseResult::Closed) return false;
		return true;
	}

	auto FAssetEditorToolkit::PrepareForExit() -> bool
	{
		return IsClosed() || (DocumentOwner && DocumentOwner->PrepareForExit());
	}

	auto FAssetEditorToolkit::SaveForExit() -> bool
	{
		return IsClosed() || (DocumentOwner && DocumentOwner->SaveDocumentsForExit());
	}

	auto FAssetEditorToolkit::GetUnsavedChanges() const -> std::vector<FAssetEditorUnsavedChange>
	{
		std::vector<FAssetEditorUnsavedChange> Changes;
		if (!IsClosed() && DocumentOwner)
			for (const auto& Document : DocumentOwner->GetDocuments())
				if (IsDocumentDirty(Document)) Changes.push_back({Document.Label, Document.ResourceId});
		return Changes;
	}

	auto FAssetEditorToolkit::InitAssetEditor(const FAssetEditorInitArgs& Args,
		std::shared_ptr<FEditorDocumentManager> Documents, FStrongObjectPtr Provider, DAssetEditor* Editor) -> bool
	{
		if (bInitialized || bClosed || !Documents || !Args.OpenArgs.AssetEditors) return false;
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
		DocumentOwner = std::move(Documents);
		DefinitionProvider = std::move(Provider);
		if (!InitialHost || !InitialHost->IsAvailable() || InitialHost->GetToolkitMode() != Args.OpenArgs.ToolkitMode) return false;
		const auto Weak = weak_from_this();
		if (!DocumentOwner->AttachToolkit(shared_from_this(), [Weak] {
			if (const auto Toolkit = Weak.lock()) if (auto* AssetEditorSubsystem = Toolkit->GetAssetEditorSubsystem()) AssetEditorSubsystem->SynchronizeToolkitAssets(*Toolkit);
		}, AssetEditors.Get())) return false;
		PostInitAssetEditor();
		for (const auto& Request : Args.InitialDocuments)
			if (!DocumentOwner->OpenDocument(Request).IsValid()) return false;
		for (auto* Asset : Args.Assets)
		{
			const auto Path = Asset->GetObjectPath();
			const auto Id = DocumentOwner->OpenDocument({.ToolkitId = GetToolkitId(), .DocumentKey = Path, .ResourceId = Path, .Label = Asset->GetName()});
			if (!Id.IsValid() || DocumentOwner->IsDocumentOpenPending(Id)) return false;
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
	auto FAssetEditorToolkit::GetPresentationDocument(const FEditorDocument& Document) const -> FEditorDocument
	{
		auto Result = Document;
		Result.DocumentKey = GetPresentationRootKey("Document");
		return Result;
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
