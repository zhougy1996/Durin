#include "LevelEditorModule.h"

#include "Editor/EditorWorkspace.h"
#include "EditorSessionSettings.h"
#include "Engine/Level.h"
#include "LevelEditorWorkspace.h"
#include "Widgets/MLevelEditor.h"

namespace Durin
{
	IMPLEMENT_MODULE(FLevelEditorModule, LevelEditor)

	FLevelEditorModule::~FLevelEditorModule() = default;

	LEVELEDITOR_API auto FLevelEditorModule::StartupModule() -> void
	{
		SessionSettings = std::make_unique<FEditorSessionSettings>();
		SessionSettings->Load();
	}

	LEVELEDITOR_API auto FLevelEditorModule::ShutdownModule() -> void
	{
		SessionSettings.reset();
	}

	LEVELEDITOR_API auto FLevelEditorModule::RegisterLevelEditorWorkspace(FEditorWorkspaceManager& WorkspaceManager) -> bool
	{
		std::shared_ptr<MLevelEditor> Workspace = std::make_shared<MLevelEditor>(*SessionSettings, WorkspaceManager);
		Workspace->Construct();
		if (!WorkspaceManager.RegisterWorkspace(Workspace)) return false;
		if (!WorkspaceManager.RegisterAssetEditor({
			.AssetClassName = DLevel::StaticClass()->GetQualifiedName().ToString(),
			.WorkspaceType = LevelEditorWorkspace::Type,
			.DocumentPolicy = EEditorDocumentPolicy::Singleton,
			.SingletonDocumentKey = "LevelEditor",
			.SingletonLabel = "Level Editor",
			.bClosable = false,
		}))
			return false;
		return WorkspaceManager.OpenDocument({
			.WorkspaceType = LevelEditorWorkspace::Type,
			.DocumentKey = "LevelEditor",
			.Label = "Level Editor",
			.bClosable = false,
		}).IsValid();
	}

	LEVELEDITOR_API auto FLevelEditorModule::GetWindowWidth() const -> int32
	{
		return SessionSettings->GetWindowWidth();
	}

	LEVELEDITOR_API auto FLevelEditorModule::GetWindowHeight() const -> int32
	{
		return SessionSettings->GetWindowHeight();
	}

	LEVELEDITOR_API auto FLevelEditorModule::GetUIScale() const -> float
	{
		return SessionSettings->GetUIScale();
	}

	LEVELEDITOR_API auto FLevelEditorModule::IsWindowMaximized() const -> bool
	{
		return SessionSettings->IsWindowMaximized();
	}
}
