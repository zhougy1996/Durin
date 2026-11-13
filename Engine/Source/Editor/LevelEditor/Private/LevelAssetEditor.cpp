#include "LevelAssetEditor.h"
#include "LevelEditorModule.h"
#include "Editor/LevelEditorSubsystem.h"
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
	auto DLevelAssetDefinition::OpenAssets(const FAssetOpenArgs& Args) const -> EAssetCommandResult
	{
		if (!Args.AssetEditors || Args.bIsRestore || (Args.bCreateSingleSession && Args.Assets.size() > 1)) return EAssetCommandResult::Unhandled;
		auto* Levels = Args.AssetEditors->GetEditor()->GetEditorSubsystem<DLevelEditorSubsystem>();
		if (!Levels) return EAssetCommandResult::Unhandled;
		if (Args.Host)
		{
			auto* Owner = FModuleManager::LoadModuleChecked<FLevelEditorModule>("LevelEditor").GetOrCreateLevelEditor(*Args.AssetEditors);
			if (!Owner || Owner->GetToolkitHost() != Args.Host) return EAssetCommandResult::Unhandled;
		}
		bool Loaded = true;
		for (const auto& Target : Args.Assets) Loaded = (Args.bPromptToSave ? Levels->OpenLevel(Target.AssetPath.ToString()) : Levels->LoadLevel(Target.AssetPath.ToString())) && Loaded;
		return Loaded ? EAssetCommandResult::Handled : EAssetCommandResult::Unhandled;
	}
}
