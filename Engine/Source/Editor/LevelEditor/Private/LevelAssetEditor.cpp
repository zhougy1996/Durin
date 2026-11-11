#include "LevelAssetEditor.h"
#include "LevelEditorModule.h"
#include "Engine/Level.h"
#include "Editor/EditorEngine.h"
#include "DObject/Class.h"
#include "Icons/FontAwesomeIcons.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Documents/EditorDocumentManager.h"
#include "Widgets/MLevelEditor.h"

namespace Durin
{
	auto DLevelAssetEditor::CreateToolkit() -> std::shared_ptr<FAssetEditorToolkit>
	{
		if (!HostContext || GetToolkitMode() != EToolkitMode::WorldCentric) return {};
		auto Toolkit = std::make_shared<LevelEditor::MLevelEditor>(HostContext->Settings, *GetDocumentManager(),
			HostContext->Browser, HostContext->OpenProjectBrowser, *GetAssetEditorSubsystem()->GetEditor());
		Toolkit->Construct();
		return Toolkit;
	}
	auto DLevelAssetDefinition::GetAssetClassPath() const -> std::string { return DLevel::StaticClass()->GetQualifiedName().ToString(); }
	auto DLevelAssetDefinition::GetAssetIcon() const -> std::string { return Icons::Home; }
	auto DLevelAssetDefinition::OpenAssets(const FAssetOpenArgs& Args) const -> FAssetOpenResult
	{
		FAssetOpenResult Result;
		for (const auto& Target : Args.Assets)
		{
			FAssetOpenItemResult Item{.Target = Target, .Status = EAssetOpenStatus::Rejected};
			DAssetEditor* Session = nullptr;
			if (Args.bCreateSingleSession && Args.Assets.size() > 1)
			{ Item.Message = "A Level session edits one scene at a time."; Result.Items.push_back(std::move(Item)); continue; }
			if (Args.AssetEditors && GetAssetOpenSupport({Args.OpenMethod}).IsSupported && !Args.bIsRestore)
				Session = FModuleManager::LoadModuleChecked<FLevelEditorModule>("LevelEditor").GetOrCreateLevelEditor(*Args.AssetEditors);
			if (!Session) Item.Message = "The requesting editor has no active Level host.";
			else if (Args.Host && Session->GetToolkitHost() != Args.Host) Item.Message = "Level opening requires its owning scene host.";
			else
			{
				const auto Documents = Session->GetDocumentManager();
				const auto Id = Documents->OpenDocument({.ToolkitId = Session->GetToolkit()->GetToolkitId(),
					.DocumentKey = "LevelEditor", .ResourceId = Target.AssetPath.ToString(), .Label = "Level Editor"});
				if (!Id.IsValid()) { Item.Status = EAssetOpenStatus::Failed; Item.Message = "The authoritative Level operation rejected opening."; }
				else if (Documents->IsDocumentOpenPending(Id))
				{
					const auto Weak = std::weak_ptr(std::dynamic_pointer_cast<LevelEditor::MLevelEditor>(Session->GetToolkit()));
					Item.RequestId = Args.AssetEditors->BeginOpenRequest(Target, [Weak] { const auto Toolkit = Weak.lock(); return !Toolkit || Toolkit->CancelPendingOpen(); });
					Args.AssetEditors->BindOpenRequestToDocument(Item.RequestId, Id.Value);
					Item.Status = EAssetOpenStatus::Pending;
				}
				else { Session->FocusWindow(); Item.Status = EAssetOpenStatus::Completed; }
			}
			Result.Items.push_back(std::move(Item));
		}
		return Result;
	}
}
