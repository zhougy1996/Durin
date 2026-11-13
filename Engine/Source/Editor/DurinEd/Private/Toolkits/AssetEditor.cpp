#include "Toolkits/AssetEditor.h"
#include "Threading/RunnableThread.h"
#include "CoreGlobals.h"

#include "Toolkits/AssetEditorToolkit.h"
#include "Documents/EditorDocumentManager.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Toolkits/ToolkitHost.h"
#include "DObject/ObjectLifecycle.h"
#include "Editor/EditorEngine.h"
#include "Misc/Time.h"

namespace Durin
{
	FAssetEditorToolkit::FAssetEditorToolkit() = default;
	FAssetEditorToolkit::~FAssetEditorToolkit() = default;

	auto FAssetEditorToolkit::GetEditorInstance() const -> IAssetEditorInstance*
	{ if (auto* Editor = Owner.Get()) return Editor; return bInitialized && Owner.GetKey().IsNull() ? const_cast<FAssetEditorToolkit*>(this) : nullptr; }
	auto FAssetEditorToolkit::GetAssetEditorSubsystem() const -> DAssetEditorSubsystem* { return AssetEditors.Get(); }
	auto FAssetEditorToolkit::GetToolkitHost() const -> std::shared_ptr<IToolkitHost> { return Host; }
	auto FAssetEditorToolkit::GetToolkitMode() const -> EToolkitMode
	{ return Host ? Host->GetToolkitMode() : EToolkitMode::Standalone; }
	auto FAssetEditorToolkit::GetInstanceKey() const -> std::string_view { return InstanceKey; }
	auto FAssetEditorToolkit::GetWindowId() const -> std::string_view { return WindowId; }
	auto FAssetEditorToolkit::GetEditorName() const -> FName { return FName(GetToolkitId().GetValue()); }
	auto FAssetEditorToolkit::GetObjectsToEdit() const -> std::vector<DObject*>
	{ return !bRetired && DocumentOwner ? DocumentOwner->GetEditedAssets() : std::vector<DObject*>{}; }
	auto FAssetEditorToolkit::SupportsRestore() const -> bool
	{ if (auto* Editor = Owner.Get()) return Editor->SupportsRestore(); return bInitialized && Owner.GetKey().IsNull(); }
	auto FAssetEditorToolkit::IsPrimaryEditor() const -> bool { return bPrimary; }
	auto FAssetEditorToolkit::GetOpenMethod() const -> EAssetOpenMethod { return OpenMethod; }
	auto FAssetEditorToolkit::GetLastActivationTime() -> double { return LastActivationTime; }
	auto FAssetEditorToolkit::Retire() -> void
	{
		if (bRetired) return;
		bRetired = true;
		bCloseRequested = false;
		ReleaseSessionUI();
		if (Host) Host->DetachToolkit(*this);
	}
	auto FAssetEditorToolkit::FocusWindow(DObject* Object) -> void
	{
		auto* Service = GetAssetEditorSubsystem();
		if (!Service || !IsSessionActive()) return;
		const auto Lease = shared_from_this();
		const FStrongObjectPtr KeepAlive(Owner.Get());
		Service->CancelRestoreFocus();
		Service->DispatchEditorCallback([&] {
			if (!Service->ActivateAssetToolkit(*this)) return;
			const auto Snapshot = DocumentOwner->GetDocuments();
			for (const auto& Document : Snapshot)
				if (!Object || Document.ResourceId == Object->GetObjectPath())
				{
					if (!DocumentOwner->ActivateDocument(Document.Id) || !IsSessionActive()) return;
					LastActivationTime = FTime::Seconds();
					Host->FocusToolkit(*this);
					FAssetOpenTarget Target;
					for (auto* Asset : GetObjectsToEdit()) if (Asset->GetObjectPath() == Document.ResourceId)
					{ FSoftObjectPath::TryCreate(Asset->GetObjectPath(), Target.AssetPath); Target.AssetClassName = Asset->GetClass()->GetQualifiedName().ToString(); break; }
					Service->PublishEvent({.Type = EAssetEditorEvent::Focused, .Target = std::move(Target), .Session = FObjectKey(Owner.Get()), .Method = OpenMethod, .InstanceId = std::string(GetInstanceId())});
					return;
				}
		});
	}
	auto FAssetEditorToolkit::CloseWindow(EAssetEditorCloseReason Reason) -> bool
	{
		if (GIsGameThreadIdInitialized) CheckGameThread();
		if (bRetired) return true;
		auto* Service = GetAssetEditorSubsystem();
		if (!Service || !IsSessionActive()) return false;
		const auto Lease = shared_from_this();
		const FStrongObjectPtr KeepAlive(Owner.Get());
		bool Closed = false;
		Service->DispatchEditorCallback([&] {
			const bool bNotify = !bCloseRequested;
			bCloseRequested = true;
			CloseReason = Reason;
			CloseCancellationRevision = DocumentOwner->GetCloseCancellationRevision();
			if (bNotify) Service->PublishEvent({.Type = EAssetEditorEvent::CloseRequested,
				.Session = FObjectKey(Owner.Get()), .Method = OpenMethod, .InstanceId = WindowId});
			if (bRetired) { Closed = true; return; }
			const auto Snapshot = DocumentOwner->GetDocuments();
			for (const auto& Document : Snapshot)
			{
				const auto Result = DocumentOwner->RequestCloseDocument(Document.Id);
				if (Result != EDocumentCloseResult::Closed)
				{ bCloseRequested = Result == EDocumentCloseResult::PendingConfirmation; return; }
			}
			Service->RetireAssetToolkit(*this);
			Closed = bRetired;
		});
		return Closed;
	}
	auto FAssetEditorToolkit::InitAssetEditor(const FAssetEditorInitArgs& Args,
		std::shared_ptr<FEditorDocumentManager> Documents, FStrongObjectPtr Provider, DAssetEditor* Editor) -> bool
	{
		if (bInitialized || bRetired || !Documents || !Args.OpenArgs.AssetEditors) return false;
		bInitialized = true;
		Owner = Editor;
		AssetEditors = Args.OpenArgs.AssetEditors;
		Host = AssetEditors.Get()->CreateToolkitHost(Args.OpenArgs);
		InstanceKey = Args.OpenArgs.GetNormalizedRole();
		bPrimary = Args.OpenArgs.bPrimary;
		static uint64 NextId = 1;
		require(NextId != std::numeric_limits<uint64>::max());
		WindowId = "Toolkit." + std::to_string(NextId++);
		OpenMethod = Args.OpenArgs.OpenMethod;
		DocumentOwner = std::move(Documents);
		DefinitionProvider = std::move(Provider);
		if (!Host || !Host->IsAvailable() || Host->GetToolkitMode() != Args.OpenArgs.ToolkitMode) return false;
		const auto Weak = weak_from_this();
		if (!DocumentOwner->AttachSessionToolkit(shared_from_this(), [Weak] {
			if (const auto Toolkit = Weak.lock()) if (auto* Service = Toolkit->GetAssetEditorSubsystem()) Service->SynchronizeToolkitAssets(*Toolkit);
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
		return Host->AttachToolkit(shared_from_this());
	}

	auto FAssetEditorToolkit::DrawToolkit(bool bActive) -> bool
	{
		if (GIsGameThreadIdInitialized) CheckGameThread();
		if (!IsSessionActive()) return false;
		const auto Lease = weak_from_this().lock();
		auto* Session = Owner.Get();
		if (!Lease || !GetAssetEditorSubsystem()) return false;
		const FStrongObjectPtr KeepAlive(Session);
		bool Activated = false;
		GetAssetEditorSubsystem()->DispatchEditorCallback([&] { if (IsSessionActive()) Activated = OnDrawToolkit(bActive); });
		return Activated && IsSessionActive();
	}
	auto FAssetEditorToolkit::GetPresentationDocument(const FEditorDocument& Document) const -> FEditorDocument
	{
		auto Result = Document;
		Result.DocumentKey = GetPresentationRootKey("Document");
		return Result;
	}
	auto FAssetEditorToolkit::OpenRelatedAsset(std::string_view Path, std::string_view ClassName) -> bool
	{
		FSoftObjectPath AssetPath;
		auto* Service = GetAssetEditorSubsystem();
		if (!Service || !IsSessionActive() || !FSoftObjectPath::TryCreate(Path, AssetPath)) return false;
		return Service->OpenAssets({.AssetEditors = Service,
			.Assets = {{AssetPath, std::string(ClassName)}}, .OpenMethod = GetOpenMethod(),
			.ToolkitMode = GetToolkitMode()});
	}
	auto FAssetEditorToolkit::ExecuteSessionCommand(const std::function<bool()>& Command) -> bool
	{
		if (GIsGameThreadIdInitialized) CheckGameThread();
		if (!IsSessionActive() || IsReadOnly()) return false;
		const auto Lease = weak_from_this().lock();
		auto* Session = Owner.Get();
		if (!Lease || !GetAssetEditorSubsystem()) return false;
		const FStrongObjectPtr KeepAlive(Session);
		bool Result = false;
		if (auto* Service = GetAssetEditorSubsystem())
			Service->DispatchEditorCallback([&] { if (IsSessionActive()) Result = Command(); });
		return Result;
	}
	auto FAssetEditorToolkit::IsSessionActive() const -> bool
	{
		const auto* Service = AssetEditors.Get();
		return bInitialized && !bRetired && Service && Service->OwnsToolkit(*this);
	}

	namespace
	{
		auto ProfileScope(const FAssetEditorToolkit* Editor, EAssetOpenMethod Method) -> std::string
		{
			std::string Key;
			if (Method == EAssetOpenMethod::View) Key += ".View";
			if (Editor)
			{
				if (Editor->GetToolkitMode() == EToolkitMode::WorldCentric) Key += ".WorldCentric";
				if (!Editor->GetInstanceKey().empty()) Key += ".Instance." + FXxHash128::HashBuffer(Editor->GetInstanceKey()).ToString();
			}
			return Key;
		}
	}
	auto FAssetEditorToolkit::GetLayoutProfileKey() const -> std::string
	{ return std::string(GetLayoutName()) + ".Layout.v" + std::to_string(GetLayoutVersion()) + ProfileScope(this, OpenMethod); }
	auto FAssetEditorToolkit::GetPresentationRootKey(std::string_view EditorRootKey) const -> std::string
	{
		return std::string(EditorRootKey) + (GetWindowId().empty() ? std::string{} : ".Window." + std::string(GetWindowId()));
	}
	DAssetEditor::DAssetEditor(const FObjectInitializer& Initializer) : Super(Initializer) {}
	DAssetEditor::~DAssetEditor() = default;
	auto DAssetEditor::BeginDestroy() -> void
	{
		if (auto* Service = GetAssetEditorSubsystem()) Service->NotifyEditorClosed(this);
		ReleaseToolkit();
		Super::BeginDestroy();
	}
	auto DAssetEditor::OnToolkitClosed() -> void
	{
		bClosed = true;
		ReleaseToolkit();
	}
	auto DAssetEditor::ReleaseToolkit() -> void
	{
		if (Toolkit) Toolkit->Retire();
		Toolkit.reset();
	}
	auto DAssetEditor::Initialize(const FAssetEditorInitArgs& Args, FStrongObjectPtr Provider) -> bool
	{
		if (bInitialized || bClosed || !Args.OpenArgs.AssetEditors || (Args.Assets.empty() && !AllowsEmptyAssets())) return false;
		std::vector<FStrongObjectPtr> InitialRoots;
		for (auto* Asset : Args.Assets)
		{
			FSoftObjectPath Path;
			if (!IsValid(Asset) || Asset->IsTemplateObject() || !FSoftObjectPath::TryCreate(Asset->GetObjectPath(), Path)) return false;
			InitialRoots.emplace_back(Asset);
		}
		InitializingArgs = &Args;
		InitializingDocuments = std::make_shared<FEditorDocumentManager>();
		Toolkit = CreateToolkit();
		auto Documents = std::move(InitializingDocuments);
		InitializingArgs = nullptr;
		if (!Toolkit || !Toolkit->InitAssetEditor(Args, std::move(Documents), std::move(Provider), this))
		{ ReleaseToolkit(); return false; }
		bInitialized = true;
		return true;
	}
	auto DAssetEditor::IsClosed() const -> bool { return bClosed || (Toolkit && Toolkit->IsRetired()); }
	auto DAssetEditor::GetToolkitMode() const -> EToolkitMode
	{ return Toolkit ? Toolkit->GetToolkitMode() : InitializingArgs ? InitializingArgs->OpenArgs.ToolkitMode : EToolkitMode::Standalone; }
	auto DAssetEditor::GetAssetEditorSubsystem() const -> DAssetEditorSubsystem*
	{ return Toolkit ? Toolkit->GetAssetEditorSubsystem() : InitializingArgs ? InitializingArgs->OpenArgs.AssetEditors : nullptr; }
	auto DAssetEditor::GetDocumentManager() const -> std::shared_ptr<FEditorDocumentManager>
	{ return Toolkit ? Toolkit->GetDocumentManager() : InitializingDocuments; }
	auto DAssetEditor::GetToolkitHost() const -> std::shared_ptr<IToolkitHost>
	{ return Toolkit ? Toolkit->GetToolkitHost() : nullptr; }
	auto DAssetEditor::GetInstanceKey() const -> std::string_view
	{ return Toolkit ? Toolkit->GetInstanceKey() : std::string_view{}; }
	auto DAssetEditor::GetWindowId() const -> std::string_view
	{ return Toolkit ? Toolkit->GetWindowId() : std::string_view{}; }
	auto DAssetEditor::GetEditorName() const -> FName { return Toolkit ? Toolkit->GetEditorName() : FName{}; }
	auto DAssetEditor::IsPrimaryEditor() const -> bool { return Toolkit && Toolkit->IsPrimaryEditor(); }
	auto DAssetEditor::GetLastActivationTime() -> double { return Toolkit ? Toolkit->GetLastActivationTime() : 0; }
	auto DAssetEditor::GetOpenMethod() const -> EAssetOpenMethod
	{ return Toolkit ? Toolkit->GetOpenMethod() : EAssetOpenMethod::Edit; }
	auto DAssetEditor::TransferToHost(std::shared_ptr<IToolkitHost> NewHost) -> bool
	{
		if (auto* Service = GetAssetEditorSubsystem()) return Service->TransferAssetEditor(*this, std::move(NewHost));
		return false;
	}
	auto DAssetEditor::GetObjectsToEdit() const -> std::vector<DObject*>
	{ return Toolkit ? Toolkit->GetObjectsToEdit() : std::vector<DObject*>{}; }
	auto DAssetEditor::FocusWindow(DObject* Object) -> void
	{ if (const auto Lease = Toolkit) Lease->FocusWindow(Object); }
	auto DAssetEditor::CloseWindow(EAssetEditorCloseReason Reason) -> bool
	{ if (bClosed) return true; const auto Lease = Toolkit; return Lease && Lease->CloseWindow(Reason); }
}
