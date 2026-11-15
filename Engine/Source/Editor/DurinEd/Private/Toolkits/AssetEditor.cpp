#include "Toolkits/AssetEditor.h"

#include "Toolkits/AssetEditorToolkit.h"
#include "Toolkits/ToolkitManager.h"
#include "Documents/EditorDocumentManager.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "DObject/ObjectLifecycle.h"

namespace Durin
{
	DAssetEditor::DAssetEditor(const FObjectInitializer& Initializer) : Super(Initializer) {}
	DAssetEditor::~DAssetEditor() = default;
	auto DAssetEditor::BeginDestroy() -> void
	{
		if (auto* AssetEditorSubsystem = GetAssetEditorSubsystem()) AssetEditorSubsystem->NotifyEditorClosed(this);
		ReleaseToolkit();
		Super::BeginDestroy();
	}
	auto DAssetEditor::OnToolkitClosed() -> void
	{
		bClosed = true;
		Toolkit.reset();
	}
	auto DAssetEditor::ReleaseToolkit() -> void
	{
		if (const auto Lease = Toolkit.lock())
		{
			if (FToolkitManager::Get().Contains(*Lease)) FToolkitManager::Get().CloseToolkit(*Lease);
			else { Lease->NotifyClosed(); Lease->OwningAssetEditor.Reset(); }
		}
		Toolkit.reset();
	}
	auto DAssetEditor::Initialize(const FAssetEditorInitArgs& Args, FStrongObjectPtr Provider) -> std::shared_ptr<FAssetEditorToolkit>
	{
		if (bInitialized || bClosed || !Args.OpenArgs.AssetEditors || (Args.Assets.empty() && !AllowsEmptyAssets())) return {};
		std::vector<FStrongObjectPtr> InitialRoots;
		for (auto* Asset : Args.Assets)
		{
			FSoftObjectPath Path;
			if (!IsValid(Asset) || Asset->IsTemplateObject() || !FSoftObjectPath::TryCreate(Asset->GetObjectPath(), Path)) return {};
			InitialRoots.emplace_back(Asset);
		}
		InitializingArgs = &Args;
		InitializingDocuments = std::make_shared<FEditorDocumentManager>();
		const auto Candidate = CreateToolkit();
		Toolkit = Candidate;
		auto Documents = std::move(InitializingDocuments);
		InitializingArgs = nullptr;
		if (!Candidate || !Candidate->InitAssetEditor(Args, std::move(Documents), std::move(Provider), this))
		{ ReleaseToolkit(); return {}; }
		bInitialized = true;
		return Candidate;
	}
	auto DAssetEditor::IsClosed() const -> bool { const auto Lease = Toolkit.lock(); return bClosed || (Lease && Lease->IsClosed()); }
	auto DAssetEditor::GetToolkitMode() const -> EToolkitMode
	{ const auto Lease = Toolkit.lock(); return Lease ? Lease->GetToolkitMode() : InitializingArgs ? InitializingArgs->OpenArgs.ToolkitMode : EToolkitMode::Standalone; }
	auto DAssetEditor::GetAssetEditorSubsystem() const -> DAssetEditorSubsystem*
	{ const auto Lease = Toolkit.lock(); return Lease ? Lease->GetAssetEditorSubsystem() : InitializingArgs ? InitializingArgs->OpenArgs.AssetEditors : nullptr; }
	auto DAssetEditor::GetDocumentManager() const -> std::shared_ptr<FEditorDocumentManager>
	{ const auto Lease = Toolkit.lock(); return Lease ? Lease->GetDocumentManager() : InitializingDocuments; }
	auto DAssetEditor::GetToolkitHost() const -> std::shared_ptr<IToolkitHost>
	{ const auto Lease = Toolkit.lock(); return Lease ? Lease->GetToolkitHost() : nullptr; }
	auto DAssetEditor::GetInstanceKey() const -> std::string_view
	{ const auto Lease = Toolkit.lock(); return Lease ? Lease->GetInstanceKey() : std::string_view{}; }
	auto DAssetEditor::GetWindowId() const -> std::string_view
	{ const auto Lease = Toolkit.lock(); return Lease ? Lease->GetWindowId() : std::string_view{}; }
	auto DAssetEditor::GetEditorName() const -> FName { const auto Lease = Toolkit.lock(); return Lease ? Lease->GetEditorName() : FName{}; }
	auto DAssetEditor::IsPrimaryEditor() const -> bool { const auto Lease = Toolkit.lock(); return Lease && Lease->IsPrimaryEditor(); }
	auto DAssetEditor::GetLastActivationTime() -> double { const auto Lease = Toolkit.lock(); return Lease ? Lease->GetLastActivationTime() : 0; }
	auto DAssetEditor::GetOpenMethod() const -> EAssetOpenMethod
	{ const auto Lease = Toolkit.lock(); return Lease ? Lease->GetOpenMethod() : EAssetOpenMethod::Edit; }
	auto DAssetEditor::GetObjectsToEdit() const -> std::vector<DObject*>
	{ const auto Lease = Toolkit.lock(); return Lease ? Lease->GetObjectsToEdit() : std::vector<DObject*>{}; }
	auto DAssetEditor::FocusWindow(DObject* Object) -> void
	{ if (const auto Lease = Toolkit.lock()) Lease->FocusWindow(Object); }
	auto DAssetEditor::CloseWindow(EAssetEditorCloseReason Reason) -> bool
	{ if (bClosed) return true; const auto Lease = Toolkit.lock(); return Lease && Lease->CloseWindow(Reason); }
}
