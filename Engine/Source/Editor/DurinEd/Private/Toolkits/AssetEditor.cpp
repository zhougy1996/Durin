#include "Toolkits/AssetEditor.h"
#include "Threading/RunnableThread.h"
#include "CoreGlobals.h"

#include "Toolkits/AssetEditorToolkit.h"
#include "Documents/EditorDocumentManager.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Toolkits/ToolkitHost.h"
#include "DObject/ObjectLifecycle.h"
#include "Editor/EditorEngine.h"

namespace Durin
{
	FAssetEditorToolkit::~FAssetEditorToolkit() = default;
	auto FAssetEditorToolkit::DrawToolkit(bool bActive) -> bool
	{
		if (GIsGameThreadIdInitialized) CheckGameThread();
		if (!IsSessionActive()) return false;
		if (!bBoundToSession) return OnDrawToolkit(bActive);
		const auto Lease = weak_from_this().lock();
		auto* Session = Owner.Get();
		if (!Lease || !Session) return false;
		const FStrongObjectPtr KeepAlive(Session);
		bool Activated = false;
		Session->GetAssetEditorSubsystem()->DispatchEditorCallback([&] { if (IsSessionActive()) Activated = OnDrawToolkit(bActive); });
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
		const auto* Session = Owner.Get();
		FSoftObjectPath AssetPath;
		if (!Session || !IsSessionActive() || !FSoftObjectPath::TryCreate(Path, AssetPath)) return false;
		auto* Service = Session->GetAssetEditorSubsystem();
		if (!Service) return false;
		const auto Result = Service->OpenAssets({.AssetEditors = Service,
			.Assets = {{AssetPath, std::string(ClassName)}}, .OpenMethod = Session->GetOpenMethod(),
			.ToolkitMode = Session->GetToolkitMode(), .Origin = "RelatedAsset"});
		return std::ranges::any_of(Result.Items, [](const auto& Item) { return Item.Status == EAssetOpenStatus::Completed || Item.Status == EAssetOpenStatus::Pending; });
	}
	auto FAssetEditorToolkit::ExecuteSessionCommand(const std::function<bool()>& Command) -> bool
	{
		if (GIsGameThreadIdInitialized) CheckGameThread();
		if (!IsSessionActive() || IsReadOnly()) return false;
		if (!bBoundToSession) return Command();
		const auto Lease = weak_from_this().lock();
		auto* Session = Owner.Get();
		if (!Lease || !Session) return false;
		const FStrongObjectPtr KeepAlive(Session);
		bool Result = false;
		if (auto* Service = Session->GetAssetEditorSubsystem())
			Service->DispatchEditorCallback([&] { if (IsSessionActive()) Result = Command(); });
		return Result;
	}
	auto FAssetEditorToolkit::IsSessionActive() const -> bool
	{
		const auto* Editor = Owner.Get();
		return !bRetired && (!bBoundToSession || (Editor && Editor->IsInitialized() && !Editor->IsClosed()
			&& Editor->GetAssetEditorSubsystem() && Editor->GetAssetEditorSubsystem()->OwnsAssetEditor(*Editor)));
	}

	namespace
	{
		auto ProfileScope(const DAssetEditor* Editor, EAssetOpenMethod Method) -> std::string
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
	{ return std::string(GetLayoutName()) + ".Layout.v" + std::to_string(GetLayoutVersion()) + ProfileScope(Owner.Get(), OpenMethod); }
	auto FAssetEditorToolkit::GetPresentationRootKey(std::string_view EditorRootKey) const -> std::string
	{
		const auto* Session = Owner.Get();
		return std::string(EditorRootKey) + (Session ? ".Window." + std::string(Session->GetWindowId()) : std::string{});
	}
	auto FAssetEditorToolkit::BindSession(DAssetEditor& Editor,
		std::shared_ptr<FEditorDocumentManager> Documents, EAssetOpenMethod Method,
		FStrongObjectPtr Provider) -> void
	{
		require(!bBoundToSession && !bRetired);
		bBoundToSession = true;
		Owner = &Editor;
		DocumentOwner = std::move(Documents);
		OpenMethod = Method;
		DefinitionProvider = std::move(Provider);
		OnSessionBound();
	}

	DAssetEditor::DAssetEditor(const FObjectInitializer& Initializer) : Super(Initializer) {}
	DAssetEditor::~DAssetEditor() = default;
	auto DAssetEditor::DrawToolkitFrame(bool bActive) -> bool
	{
		if (GIsGameThreadIdInitialized) CheckGameThread();
		const FStrongObjectPtr KeepAlive(this);
		const auto FrameToolkit = Toolkit;
		bool Activated = false;
		if (!bClosed && FrameToolkit)
			if (auto* Service = AssetEditors.Get())
				Service->DispatchEditorCallback([&] { Activated = FrameToolkit->DrawToolkit(bActive); });
		return Activated && !bClosed;
	}
	auto DAssetEditor::BeginDestroy() -> void
	{
		if (auto* Service = AssetEditors.Get()) Service->NotifyEditorClosed(this);
		ReleaseToolkit();
		Super::BeginDestroy();
	}
	auto DAssetEditor::ReleaseToolkit() -> void
	{
		if (Toolkit) Toolkit->Retire();
		if (Host && Toolkit) Host->DetachToolkit(*Toolkit);
		Toolkit.reset();
		Documents.reset();
		Host.reset();
	}
	auto DAssetEditor::Initialize(const FAssetEditorInitArgs& Args, FStrongObjectPtr Provider) -> bool
	{
		if (bInitialized || !Args.OpenArgs.AssetEditors || (Args.Assets.empty() && !AllowsEmptyAssets())) return false;
		AssetEditors = Args.OpenArgs.AssetEditors;
		OpenMethod = Args.OpenArgs.OpenMethod;
		Host = Args.OpenArgs.AssetEditors->CreateToolkitHost(Args.OpenArgs);
		if (!Host || !Host->IsAvailable() || Host->GetToolkitMode() != Args.OpenArgs.ToolkitMode) { Host.reset(); return false; }
		bPrimary = Args.bPrimary;
		InstanceKey = Args.InstanceKey.empty() && !bPrimary ? "Secondary" : Args.InstanceKey;
		std::vector<FStrongObjectPtr> InitialRoots;
		for (auto* Asset : Args.Assets)
		{
			FSoftObjectPath Path;
			if (!IsValid(Asset) || Asset->IsTemplateObject() || !FSoftObjectPath::TryCreate(Asset->GetObjectPath(), Path)) return false;
			InitialRoots.emplace_back(Asset);
		}
		static uint64 NextWindowId = 1;
		require(NextWindowId != std::numeric_limits<uint64>::max());
		WindowId = std::to_string(NextWindowId++);
		Documents = std::make_shared<FEditorDocumentManager>();
		Toolkit = CreateToolkit();
		if (!Toolkit || Toolkit->IsRetired()) { ReleaseToolkit(); return false; }
		Toolkit->BindSession(*this, Documents, OpenMethod, std::move(Provider));
		const TWeakObjectPtr<DAssetEditor> WeakThis(this);
		if (!Documents->AttachSessionToolkit(Toolkit, [WeakThis] {
			if (auto* Editor = WeakThis.Get()) Editor->SynchronizeAssets();
		}, Args.OpenArgs.AssetEditors)) { ReleaseToolkit(); return false; }
		for (const auto& Request : Args.InitialDocuments)
			if (!Documents->OpenDocument(Request).IsValid()) { ReleaseToolkit(); return false; }
		for (auto* Asset : Args.Assets)
		{
			const auto ResourceId = Asset->GetObjectPath();
			const auto Id = Documents->OpenDocument({.ToolkitId = Toolkit->GetToolkitId(),
				.DocumentKey = ResourceId, .ResourceId = ResourceId, .Label = Asset->GetName()});
			if (!Id.IsValid() || Documents->IsDocumentOpenPending(Id))
			{
				ReleaseToolkit();
				return false;
			}
		}
		if (!Host->AttachToolkit(Toolkit)) { ReleaseToolkit(); return false; }
		bInitialized = true;
		return true;
	}
	auto DAssetEditor::GetToolkitMode() const -> EToolkitMode
	{ return Host ? Host->GetToolkitMode() : EToolkitMode::Standalone; }
	auto DAssetEditor::GetEditorName() const -> FName
	{
		return Toolkit ? FName(Toolkit->GetToolkitId().GetValue()) : FName{};
	}

	auto DAssetEditor::TransferToHost(std::shared_ptr<IToolkitHost> NewHost) -> bool
	{
		if (auto* Service = AssetEditors.Get()) return Service->TransferAssetEditor(*this, std::move(NewHost));
		return false;
	}
	auto DAssetEditor::GetObjectsToEdit() const -> std::vector<DObject*>
	{
		return Documents ? Documents->GetEditedAssets() : std::vector<DObject*>{};
	}
	auto DAssetEditor::SynchronizeAssets() -> void
	{
		if (!bInitialized || bClosed) return;
		if (auto* Service = AssetEditors.Get())
		{
			Service->SynchronizeSessionAssets(*this);
		}
	}
	auto DAssetEditor::FocusWindow(DObject* Object) -> void
	{
		if (auto* Service = AssetEditors.Get())
		{
			Service->CancelRestoreFocus();
			Service->DispatchEditorCallback([&] { FocusWindowImpl(Object); });
		}
	}

	auto DAssetEditor::FocusWindowImpl(DObject* Object) -> void
	{
		if (!bInitialized || bClosed || !Documents) return;
		const FStrongObjectPtr KeepAlive(this);
		const auto Snapshot = Documents->GetDocuments();
		for (const auto& Document : Snapshot)
			if (!Object || Document.ResourceId == Object->GetObjectPath())
			{
				if (auto* Service = AssetEditors.Get(); !Service || !Service->ActivateAssetEditor(*this)) return;
				if (!Documents->ActivateDocument(Document.Id)) return;
				LastActivationTime = FTime::Seconds();
				if (Host && Toolkit) Host->FocusToolkit(*Toolkit);
				FAssetOpenTarget Target;
				for (auto* Asset : GetObjectsToEdit())
					if (Asset->GetObjectPath() == Document.ResourceId)
					{ FSoftObjectPath::TryCreate(Asset->GetObjectPath(), Target.AssetPath); Target.AssetClassName = Asset->GetClass()->GetQualifiedName().ToString(); break; }
				if (auto* Service = AssetEditors.Get()) Service->PublishEvent({.Type = EAssetEditorEvent::Focused,
					.Target = std::move(Target), .Session = FObjectKey(this), .Method = OpenMethod});
				return;
			}
	}
	auto DAssetEditor::CloseWindow(EAssetEditorCloseReason Reason) -> bool
	{
		if (GIsGameThreadIdInitialized) CheckGameThread();
		if (bClosed) return true;
		bool Closed = false;
		if (auto* Service = AssetEditors.Get())
			Service->DispatchEditorCallback([&] { Closed = CloseWindowImpl(Reason); });
		return Closed;
	}

	auto DAssetEditor::CloseWindowImpl(EAssetEditorCloseReason Reason) -> bool
	{
		if (bClosed) return true;
		if (!bInitialized || !Documents) return false;
		const bool bNotify = !bCloseRequested;
		bCloseRequested = true;
		CloseReason = Reason;
		CloseCancellationRevision = Documents->GetCloseCancellationRevision();
		const FStrongObjectPtr KeepAlive(this);
		const auto KeepToolkit = Toolkit;
		if (bNotify)
			if (auto* Service = AssetEditors.Get()) Service->PublishEvent({.Type = EAssetEditorEvent::CloseRequested,
				.Session = FObjectKey(this), .Method = OpenMethod});
		if (bClosed || !Documents) return bClosed;
		const auto Snapshot = Documents->GetDocuments();
		for (const auto& Document : Snapshot)
		{
			LastCloseResult = Documents->RequestCloseDocument(Document.Id);
			if (LastCloseResult != EDocumentCloseResult::Closed)
			{ bCloseRequested = LastCloseResult == EDocumentCloseResult::PendingConfirmation; return false; }
		}
		LastCloseResult = EDocumentCloseResult::Closed;
		bClosed = true;
		bCloseRequested = false;
		if (auto* Service = AssetEditors.Get()) Service->RetireAssetEditor(*this);
		ReleaseToolkit();
		return true;
	}
}
